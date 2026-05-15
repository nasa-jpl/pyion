/**
 * Worker file to handle pure C function
 * interaction between PyION and ION.
 * 
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <bp.h>
#include "return_codes.h"
#include "base_bp.h"
#include "_utils.c"
#include "macros.h"

// Serializes process-global ION attach/detach. These mutate state shared by
// every endpoint, so they must not run concurrently with one another.
static pthread_mutex_t ion_global_lock = PTHREAD_MUTEX_INITIALIZER;

int base_bp_attach()
{
    int result;
    pthread_mutex_lock(&ion_global_lock);
    result = bp_attach();
    pthread_mutex_unlock(&ion_global_lock);
    return result;
}

void base_bp_detach()
{
    pthread_mutex_lock(&ion_global_lock);
    bp_detach();
    pthread_mutex_unlock(&ion_global_lock);
}

/* ============================================================================
 * === Endpoint registry and lifecycle
 * ============================================================================
 * The Python layer holds an opaque integer ``handle`` for each endpoint, not
 * a raw pointer. C entry points resolve the handle through a registry of live
 * endpoints, so a stale handle -- a double close, or any use after close --
 * is rejected with PYION_INVALID_HANDLE_ERR instead of dereferencing freed
 * memory. Handles come from a monotonic counter and are never reused, so a
 * freed endpoint's handle can never alias a newly opened one.
 *
 * No lock is ever held across a blocking ION call. A blocked bp_receive is
 * woken via ION's own bp_interrupt; the refcount (not a lock) keeps the state
 * struct alive while any thread is inside a C call on it.
 *
 * Lock order: registry_lock is acquired before state_lock; no path holds
 * state_lock while acquiring registry_lock.
 */

static BpSapState     *registry_head = NULL;
static unsigned long   registry_next_handle = 1; // 0 is reserved as "invalid"
static pthread_mutex_t registry_lock = PTHREAD_MUTEX_INITIALIZER;

// Assign a unique handle to state and add it to the registry of live endpoints.
static void registry_register(BpSapState *state)
{
    pthread_mutex_lock(&registry_lock);
    state->handle = registry_next_handle++;
    state->registry_next = registry_head;
    registry_head = state;
    pthread_mutex_unlock(&registry_lock);
}

// Remove state from the registry. Caller must not hold registry_lock.
static void registry_unregister(BpSapState *state)
{
    BpSapState **pp;
    pthread_mutex_lock(&registry_lock);
    for (pp = &registry_head; *pp != NULL; pp = &((*pp)->registry_next))
    {
        if (*pp == state)
        {
            *pp = state->registry_next;
            break;
        }
    }
    pthread_mutex_unlock(&registry_lock);
}

// Find a live endpoint by handle. Caller must hold registry_lock.
static BpSapState *registry_find_locked(unsigned long handle)
{
    BpSapState *s;
    for (s = registry_head; s != NULL; s = s->registry_next)
        if (s->handle == handle)
            return s;
    return NULL;
}

// Tear down the SAP and free the state. The caller must guarantee that no
// other thread is inside a C call on this state.
static void endpoint_destroy(BpSapState *state)
{
    registry_unregister(state);
    if (state->attendant)
    {
        ionStopAttendant(state->attendant);
        free(state->attendant);
    }
    bp_close(state->sap);
    pthread_mutex_destroy(&(state->state_lock));
    pthread_mutex_destroy(&(state->send_lock));
    free(state);
}

// Decrement the in-use refcount. If this was the last reference and a close
// has been requested, the state is destroyed and freed here.
static void endpoint_release(BpSapState *state)
{
    int do_free;
    pthread_mutex_lock(&(state->state_lock));
    state->refcount--;
    do_free = (state->refcount == 0 && state->close_requested);
    pthread_mutex_unlock(&(state->state_lock));
    if (do_free)
        endpoint_destroy(state);
}

// Resolve a handle and pin the endpoint (refcount++). On success returns the
// state; on failure returns NULL and sets *err: PYION_INVALID_HANDLE_ERR if
// the handle is not a live endpoint, PYION_CONN_ABORTED_ERR if it is closing.
static BpSapState *endpoint_acquire(unsigned long handle, int *err)
{
    BpSapState *state;

    pthread_mutex_lock(&registry_lock);
    state = registry_find_locked(handle);
    if (state == NULL)
    {
        pthread_mutex_unlock(&registry_lock);
        *err = PYION_INVALID_HANDLE_ERR;
        return NULL;
    }
    // Pin under registry_lock so the state cannot be freed between the lookup
    // and the refcount increment.
    pthread_mutex_lock(&(state->state_lock));
    if (state->close_requested)
    {
        pthread_mutex_unlock(&(state->state_lock));
        pthread_mutex_unlock(&registry_lock);
        *err = PYION_CONN_ABORTED_ERR;
        return NULL;
    }
    state->refcount++;
    pthread_mutex_unlock(&(state->state_lock));
    pthread_mutex_unlock(&registry_lock);
    *err = 0;
    return state;
}

int base_bp_close(unsigned long handle)
{
    BpSapState *state;

    pthread_mutex_lock(&registry_lock);
    state = registry_find_locked(handle);
    if (state == NULL)
    {
        // Unknown handle: never opened, or already closed and freed.
        pthread_mutex_unlock(&registry_lock);
        return PYION_INVALID_HANDLE_ERR;
    }
    pthread_mutex_lock(&(state->state_lock));
    if (state->close_requested)
    {
        // A close is already in progress; nothing else to do.
        pthread_mutex_unlock(&(state->state_lock));
        pthread_mutex_unlock(&registry_lock);
        return 0;
    }
    state->close_requested = 1;
    atomic_store(&(state->status), EID_CLOSING);
    state->refcount++; // close holds its own reference
    pthread_mutex_unlock(&(state->state_lock));
    pthread_mutex_unlock(&registry_lock);

    // Wake any blocked receiver. Touching state->sap is safe: we hold a
    // reference, so the state cannot be freed underneath us.
    bp_interrupt(state->sap);
    if (state->attendant)
        ionPauseAttendant(state->attendant);

    // Drop close's reference; frees the state if it was the last one.
    endpoint_release(state);
    return 0;
}

int base_bp_interrupt(unsigned long handle)
{
    int err;
    int expected = EID_RUNNING;
    BpSapState *state = endpoint_acquire(handle, &err);

    if (state == NULL)
    {
        // A closing endpoint needs no interrupt (the close path issues its
        // own); only a genuinely unknown handle is an error.
        return (err == PYION_INVALID_HANDLE_ERR) ? PYION_INVALID_HANDLE_ERR : 0;
    }

    // Transition into INTERRUPTING only from RUNNING, so an IDLE or CLOSING
    // state is never clobbered. Interrupt ION only when we actually woke a
    // running receiver, to avoid arming a spurious interrupt on an idle SAP.
    if (atomic_compare_exchange_strong(&(state->status), &expected, EID_INTERRUPTING))
    {
        bp_interrupt(state->sap);
        if (state->attendant)
            ionPauseAttendant(state->attendant);
    }

    endpoint_release(state);
    return 0;
}

int base_init_bp_tx_payload(BpTx *txInfo)
{
    txInfo->destEid = NULL;
    txInfo->reportEid = NULL;
    txInfo->ttl = 0;
    txInfo->classOfService = 0;
    txInfo->custodySwitch = 0;
    txInfo->rrFlags = 0;
    txInfo->ackReq = 0;
    txInfo->retxTimer = 0;
    txInfo->data = NULL;
    txInfo->data_size = 0;
    txInfo->ancillaryData = NULL;
    return 0;
}

int base_init_bp_rx_payload(BpRx *obj)
{
    obj->len = 0;
    obj->do_malloc = 0;
    obj->payload = NULL;
    return 0;
}

int help_receive_data(BpSapState *state, BpDelivery *dlv, BpRx *msg)
{
    // Define variables
    int data_size, len, rx_ret, do_malloc;
    Sdr sdr;
    ZcoReader reader;
    char err_msg[150];

    // Define variables to store the bundle payload. If payload size is less than
    // MAX_PREALLOC_BUFFER, then use preallocated buffer to save time. Otherwise,
    // call malloc to allocate as much memory as you need.
    char *payload;

    // Get ION's SDR
    sdr = bp_get_sdr();
    CHKZERO(sdr); //asert statement, checking SDR
    CHKZERO(dlv);
    CHKZERO(msg);

    while (atomic_load(&(state->status)) == EID_RUNNING)
    {
        // Blocking receive. No lock is held here: a concurrent interrupt or
        // close wakes this call via bp_interrupt.
        rx_ret = bp_receive(state->sap, dlv, BP_BLOCKING);

        // Check if error while receiving a bundle
        if ((rx_ret < 0) && (atomic_load(&(state->status)) == EID_RUNNING))
        {
            return PYION_IO_ERR;
        }

        // If dlv is not interrupted (e.g., it was successful), get out of loop.
        // From Scott Burleigh: BpReceptionInterrupted can happen because SO triggers an
        // interruption without the user doing anything. Therefore, bp_receive always
        // needs to be enclosed in this type of while loops.
        if (dlv->result != BpReceptionInterrupted)
            break;
    }

    // If you exited because of interruption
    if (atomic_load(&(state->status)) == EID_INTERRUPTING)
    {
        return PYION_INTERRUPTED_ERR;
    }

    // If you exited because of closing
    if (atomic_load(&(state->status)) == EID_CLOSING)
    {
        return PYION_CONN_ABORTED_ERR;
    }

    // If endpoint was stopped, finish
    if (dlv->result == BpEndpointStopped)
    {
        return PYION_CONN_ABORTED_ERR;
    }

    // If bundle does not have the payload, raise IOError
    if (dlv->result != BpPayloadPresent)
    {
        return PYION_IO_ERR;
    }

    // Get content data size
    SDR_BEGIN_XN
    data_size = zco_source_data_length(sdr, dlv->adu);
    SDR_END_XN

    // Check if we need to allocate memory dynamically
    do_malloc = (data_size > MAX_PREALLOC_BUFFER);

    // Allocate memory if necessary
    payload = do_malloc ? (char *)malloc(data_size) : msg->payload_prealloc;

    // Initialize reader
    zco_start_receiving(dlv->adu, &reader);

    // Get bundle data
    SDR_BEGIN_XN
    len = zco_receive_source(sdr, &reader, data_size, payload);

    // Store the data in the output structure
    msg->payload = payload;
    msg->len = len;
    msg->do_malloc = do_malloc;

    
    // Add headers
    msg->bundleSourceEid = malloc(strlen(dlv->bundleSourceEid) + 1);
    strcpy(msg->bundleSourceEid, dlv->bundleSourceEid);
    msg->bundleCreationTime.msec = dlv->bundleCreationTime.msec;
    msg->bundleCreationTime.count = dlv->bundleCreationTime.count;
	msg->timeToLive = dlv->timeToLive;
	msg->metadataType = dlv->metadataType;
	msg->metadataLen = dlv->metadataLen;
	memcpy(msg->metadata, dlv->metadata,
			BP_MAX_METADATA_LEN);

    // Close transaction and/or handle error while getting the payload
    if (sdr_end_xn(sdr) < 0 || len < 0)
    {
        // Clean up tasks
        if (do_malloc) free(payload);

        return PYION_IO_ERR;
    }
    return 0;
}

int base_bp_receive_data(unsigned long handle, BpRx *msg)
{
    BpDelivery dlv;
    int status, err;
    BpSapState *state;

    // Resolve the handle and pin the state for the duration of this call.
    state = endpoint_acquire(handle, &err);
    if (state == NULL)
        return err;

    // Enforce at most one concurrent receiver per endpoint.
    pthread_mutex_lock(&(state->state_lock));
    if (state->receivers > 0)
    {
        pthread_mutex_unlock(&(state->state_lock));
        endpoint_release(state);
        return PYION_BUSY_ERR;
    }
    state->receivers = 1;
    atomic_store(&(state->status), EID_RUNNING);
    pthread_mutex_unlock(&(state->state_lock));

    // Blocking receive runs with no lock held.
    status = help_receive_data(state, &dlv, msg);
    bp_release_delivery(&dlv, 1);

    // Clear the receiver slot. Leave an EID_CLOSING status intact so a
    // concurrent close is not masked; the deferred free handles teardown.
    pthread_mutex_lock(&(state->state_lock));
    state->receivers = 0;
    if (atomic_load(&(state->status)) != EID_CLOSING)
        atomic_store(&(state->status), EID_IDLE);
    pthread_mutex_unlock(&(state->state_lock));

    endpoint_release(state);
    return status;
}

int base_bp_open(BpSapState **state_ref, char *ownEid, int detained, int mem_ctrl)
{
    // Define variables
    int ok;
    BpSapState *state;

    // malloc space for bp sap state
    state = (BpSapState *)malloc(sizeof(BpSapState));
    *state_ref = state;

    if (state == NULL)
    {
        return -1;
    }
    memset((char *)state, 0, sizeof(BpSapState));

    // Initialize the per-endpoint locks.
    if (pthread_mutex_init(&(state->state_lock), NULL) != 0)
    {
        free(state);
        *state_ref = NULL;
        return -2;
    }
    if (pthread_mutex_init(&(state->send_lock), NULL) != 0)
    {
        pthread_mutex_destroy(&(state->state_lock));
        free(state);
        *state_ref = NULL;
        return -2;
    }
    atomic_init(&(state->status), EID_IDLE);

    // Open the endpoint. This call fills out the SAP information
    // NOTE: An endpoint must be opened in detained mode if you want
    //       to have custody-based re-tx.
    if (detained == 0)
    {
        ok = bp_open(ownEid, &(state->sap));
    }
    else
    {
        ok = bp_open_source(ownEid, &(state->sap), 1);
    }
    if (ok < 0)
    {
        pthread_mutex_destroy(&(state->state_lock));
        pthread_mutex_destroy(&(state->send_lock));
        free(state);
        *state_ref = NULL;
        return PYION_IO_ERR;
    }

    state->detained = (detained > 0);

    if (mem_ctrl)
    {
        // Allocate memory for new attendant
        state->attendant = (ReqAttendant *)malloc(sizeof(ReqAttendant));

        // Initialize the attendant
        if (state->attendant == NULL || ionStartAttendant(state->attendant))
        {
            free(state->attendant);
            bp_close(state->sap);
            pthread_mutex_destroy(&(state->state_lock));
            pthread_mutex_destroy(&(state->send_lock));
            free(state);
            *state_ref = NULL;
            return -3;
        }
    }
    else
    {
        state->attendant = NULL;
    }

    // Assign a handle and publish the endpoint in the registry. After this
    // the endpoint is reachable by handle from other threads.
    registry_register(state);

    return ok;
}

/**
 * 
 * Base-level function to send a bundle through
 * ION's bp.h library.
 * 
 */
/*destEid, reportEid, ttl, classOfService, 
           custodySwitch, rrFlags, ackReq, ancillaryData, 
           bundleZco, &newBundle*/
int base_bp_send(unsigned long handle, BpTx *txInfo)
{
    char err_msg[150];
    Object newBundle;
    Sdr sdr = NULL;
    Object bundleZco;
    Object bundleSdr;
    int ok;
    int result = 0;
    int err;
    BpSapState *state;

    // Resolve the handle and pin the state for the duration of this call.
    state = endpoint_acquire(handle, &err);
    if (state == NULL)
        return err;

    // Serialize sends on this endpoint. send_lock may be held across the
    // (possibly blocking) send: it only stalls other senders on the same
    // endpoint, never receive/interrupt/close, which use other locks.
    pthread_mutex_lock(&(state->send_lock));

    // Initialize variables
    sdr = bp_get_sdr();

    // Insert data to SDR
    SDR_BEGIN_XN
    bundleSdr = sdr_insert(sdr, txInfo->data, (size_t)txInfo->data_size);
    SDR_END_XN

    // Create ZCO and send
    bundleZco = ionCreateZco(ZcoSdrSource, bundleSdr, 0, txInfo->data_size,
                             txInfo->classOfService, 0, ZcoOutbound, state->attendant);
    if (bundleZco == 0)
    {
        result = PYION_IO_ERR;
        goto done;
    }
    ok = bp_send(state->sap, txInfo->destEid, txInfo->reportEid, txInfo->ttl,
                 txInfo->classOfService, txInfo->custodySwitch, txInfo->rrFlags,
                 txInfo->ackReq, txInfo->ancillaryData, bundleZco, &newBundle);

    // If you want custody transfer and have specified a re-transmission timer,
    // then activate it
    if (txInfo->custodySwitch == SourceCustodyRequired && txInfo->retxTimer > 0)
    {
        // Note: The timer starts as soon as bp_memo is called.
        ok = bp_memo(newBundle, txInfo->retxTimer);

        // Handle error in bp_memo
        if (ok < 0)
        {
            result = ok;
            goto done;
        }
    }

    // If you have opened this endpoint in detained mode, you need to release the bundle
    if (state->detained)
        bp_release(newBundle);

done:
    pthread_mutex_unlock(&(state->send_lock));
    endpoint_release(state);
    return result;
}