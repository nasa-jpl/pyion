#include <stdio.h>
#include <stdlib.h>
#include <ltp.h>
#include <ion.h>
#include "base_ltp.h"
#include "return_codes.h"
#include "_utils.c"

#include "macros.h"


int base_ltp_attach(void) {
    return ltp_attach();
}

void base_ltp_detach(void) {
    return ltp_detach();
}

/* ============================================================================
 * === LTP SAP registry and lifecycle
 * ============================================================================
 * Mirrors the BP layer: the Python side holds an opaque, never-reused integer
 * handle, not a raw pointer. C entry points resolve the handle through a
 * registry of live SAPs, so a stale handle (double close, use after close) is
 * rejected with PYION_INVALID_HANDLE_ERR instead of dereferencing freed
 * memory. No lock is held across a blocking ION call; a blocked receiver is
 * woken via ltp_interrupt, and the refcount keeps the SAP alive while any
 * thread is inside a C call on it.
 *
 * Lock order: registry_lock before state_lock.
 */

static LtpSAP        *registry_head = NULL;
static unsigned long  registry_next_handle = 1; // 0 is reserved as "invalid"
static pthread_mutex_t registry_lock = PTHREAD_MUTEX_INITIALIZER;

static void registry_register(LtpSAP *state) {
    pthread_mutex_lock(&registry_lock);
    state->handle = registry_next_handle++;
    state->registry_next = registry_head;
    registry_head = state;
    pthread_mutex_unlock(&registry_lock);
}

static void registry_unregister(LtpSAP *state) {
    LtpSAP **pp;
    pthread_mutex_lock(&registry_lock);
    for (pp = &registry_head; *pp != NULL; pp = &((*pp)->registry_next)) {
        if (*pp == state) {
            *pp = state->registry_next;
            break;
        }
    }
    pthread_mutex_unlock(&registry_lock);
}

// Find a live SAP by handle. Caller must hold registry_lock.
static LtpSAP *registry_find_locked(unsigned long handle) {
    LtpSAP *s;
    for (s = registry_head; s != NULL; s = s->registry_next)
        if (s->handle == handle)
            return s;
    return NULL;
}

// Tear down the SAP. Caller must guarantee no thread is inside a C call on it.
static void endpoint_destroy(LtpSAP *state) {
    registry_unregister(state);
    ltp_close(state->clientId);
    pthread_mutex_destroy(&(state->state_lock));
    pthread_mutex_destroy(&(state->send_lock));
    free(state);
}

// Decrement the in-use refcount; free once the last reference drops after a
// close has been requested.
static void endpoint_release(LtpSAP *state) {
    int do_free;
    pthread_mutex_lock(&(state->state_lock));
    state->refcount--;
    do_free = (state->refcount == 0 && state->close_requested);
    pthread_mutex_unlock(&(state->state_lock));
    if (do_free)
        endpoint_destroy(state);
}

// Resolve a handle and pin the SAP (refcount++). On success returns the SAP;
// on failure returns NULL and sets *err: PYION_INVALID_HANDLE_ERR for an
// unknown handle, PYION_ERR_LTP_RECEPTION_CLOSED for a closing SAP.
static LtpSAP *endpoint_acquire(unsigned long handle, int *err) {
    LtpSAP *state;

    pthread_mutex_lock(&registry_lock);
    state = registry_find_locked(handle);
    if (state == NULL) {
        pthread_mutex_unlock(&registry_lock);
        *err = PYION_INVALID_HANDLE_ERR;
        return NULL;
    }
    pthread_mutex_lock(&(state->state_lock));
    if (state->close_requested) {
        pthread_mutex_unlock(&(state->state_lock));
        pthread_mutex_unlock(&registry_lock);
        *err = PYION_ERR_LTP_RECEPTION_CLOSED;
        return NULL;
    }
    state->refcount++;
    pthread_mutex_unlock(&(state->state_lock));
    pthread_mutex_unlock(&registry_lock);
    *err = 0;
    return state;
}

int base_ltp_open(unsigned int clientId, LtpSAP **state_ref) {
    LtpSAP *state;

    state = (LtpSAP *)calloc(1, sizeof(LtpSAP));
    *state_ref = state;
    if (state == NULL)
        return PYION_MALLOC_ERR;

    if (pthread_mutex_init(&(state->state_lock), NULL) != 0) {
        free(state);
        *state_ref = NULL;
        return PYION_MALLOC_ERR;
    }
    if (pthread_mutex_init(&(state->send_lock), NULL) != 0) {
        pthread_mutex_destroy(&(state->state_lock));
        free(state);
        *state_ref = NULL;
        return PYION_MALLOC_ERR;
    }
    atomic_init(&(state->status), SAP_IDLE);

    int retStatus = ltp_open(clientId);
    if (retStatus < 0) {
        pthread_mutex_destroy(&(state->state_lock));
        pthread_mutex_destroy(&(state->send_lock));
        free(state);
        *state_ref = NULL;
        return retStatus;
    }

    state->clientId = clientId;

    // Assign a handle and publish the SAP in the registry.
    registry_register(state);
    return retStatus;
}

int base_ltp_close(unsigned long handle) {
    LtpSAP *state;

    pthread_mutex_lock(&registry_lock);
    state = registry_find_locked(handle);
    if (state == NULL) {
        // Unknown handle: never opened, or already closed and freed.
        pthread_mutex_unlock(&registry_lock);
        return PYION_INVALID_HANDLE_ERR;
    }
    pthread_mutex_lock(&(state->state_lock));
    if (state->close_requested) {
        pthread_mutex_unlock(&(state->state_lock));
        pthread_mutex_unlock(&registry_lock);
        return 0;
    }
    state->close_requested = 1;
    atomic_store(&(state->status), SAP_CLOSING);
    state->refcount++; // close holds its own reference
    pthread_mutex_unlock(&(state->state_lock));
    pthread_mutex_unlock(&registry_lock);

    // Wake any blocked receiver.
    ltp_interrupt(state->clientId);

    endpoint_release(state);
    return 0;
}

int base_ltp_interrupt(unsigned long handle) {
    int err;
    int expected = SAP_RUNNING;
    LtpSAP *state = endpoint_acquire(handle, &err);

    if (state == NULL) {
        // A closing SAP needs no interrupt; only an unknown handle is an error.
        return (err == PYION_INVALID_HANDLE_ERR) ? PYION_INVALID_HANDLE_ERR : 0;
    }

    // Move a running receiver into CLOSING and wake it. LTP has no separate
    // INTERRUPTING state, so interrupt and close converge on CLOSING.
    if (atomic_compare_exchange_strong(&(state->status), &expected, SAP_CLOSING))
        ltp_interrupt(state->clientId);

    endpoint_release(state);
    return 0;
}

int base_ltp_send(unsigned long handle, LtpTxPayload *msg) {
    char       err_msg[150];
    Sdr        sdr;
    int        ok;
    int        err;
    Object     extent;
    Object     item = 0;
    LtpSAP    *state;

    state = endpoint_acquire(handle, &err);
    if (state == NULL)
        return err;

    // Serialize sends on this SAP.
    pthread_mutex_lock(&(state->send_lock));

    sdr = getIonsdr();

    // Start SDR transaction. On SDR failure go through the single cleanup
    // path (done:) rather than returning here -- an early return would leak
    // the held send_lock (wedging all future sends on this SAP) and the SAP
    // refcount (the state would never be freed).
    if (!sdr_begin_xn(sdr)) {
        ok = PYION_SDR_ERR;
        goto done;
    }

    // Allocate SDR memory
    extent = sdr_insert(sdr, msg->data, (size_t)msg->data_size);
    if (!extent) {
        sdr_cancel_xn(sdr);
        sprintf(err_msg, "SDR memory could not be allocated");
        ok = PYION_SDR_ERR;
        goto done;
    }

    // End SDR transaction
    if (sdr_end_xn(sdr) < 0) {
        ok = PYION_SDR_ERR;
        goto done;
    }

    // Create ZCO object (not blocking because there is no attendant)
    item = ionCreateZco(ZcoSdrSource, extent, 0, msg->data_size,
                        0, 0, ZcoOutbound, NULL);

    // Handle error while creating ZCO object
    if (!item || item == (Object)ERROR) {
        sprintf(err_msg, "ZCO object creation failed");
        ok = PYION_SDR_ERR;
        goto done;
    }

    // Send using LTP protocol. All data is sent as RED LTP by definition.
    // NOTE 1: SessionId is filled by ``ltp_send``, but you do not care about it
    // NOTE 2: In general, ltp_send does not block. However, if you exceed the
    //         max number of export sessions defined in ltprc, then it will.
    ok = ltp_send((uvast)msg->destEngineId, state->clientId, item, LTP_ALL_RED,
                  &(msg->sessionId));

done:
    pthread_mutex_unlock(&(state->send_lock));
    endpoint_release(state);
    return ok;
}

int base_ltp_receive_data(unsigned long handle, LtpRxPayload *payloadObj) {
    // Define variables
    ZcoReader      reader;
    Sdr            sdr;
    LtpNoticeType  type;
    LtpSessionId   sessionId;
    unsigned char  endOfBlock;
    unsigned char  reasonCode;
    unsigned int   dataOffset;
    unsigned int   dataLength;
    Object         data;
    int            receiving_block, notice, do_malloc;
    vast           data_size;
    int            err;
    int            result = 0;
    LtpSAP        *state;

    state = endpoint_acquire(handle, &err);
    if (state == NULL)
        return err;

    // Enforce at most one concurrent receiver per SAP.
    pthread_mutex_lock(&(state->state_lock));
    if (state->receivers > 0) {
        pthread_mutex_unlock(&(state->state_lock));
        endpoint_release(state);
        return PYION_BUSY_ERR;
    }
    state->receivers = 1;
    atomic_store(&(state->status), SAP_RUNNING);
    pthread_mutex_unlock(&(state->state_lock));

    receiving_block = 1;

    // Process incoming indications.
    while ((atomic_load(&(state->status)) == SAP_RUNNING) && (receiving_block == 1)) {
        // Get the next LTP notice
        notice = ltp_get_notice(state->clientId, &type, &sessionId, &reasonCode,
                                &endOfBlock, &dataOffset, &dataLength, &data);

        // Handle error while receiving notices
        if (notice < 0) {
            result = PYION_ERR_LTP_NOTICE;
            goto done;
        }
        payloadObj->reasonCode = reasonCode;

        // Handle different notice types
        switch (type) {
            case LtpExportSessionComplete:      // Transmit success
                break;
            case LtpImportSessionCanceled:      // Cancelled; no data received yet.
                ltp_release_data(data);
                result = PYION_ERR_LTP_IMPORT;
                goto done;
            case LtpExportSessionCanceled:      // Transmit failure.
                ltp_release_data(data);
                result = PYION_ERR_LTP_EXPORT;
                goto done;
            case LtpRecvRedPart:
                // A partially-green block is not allowed.
                if (!endOfBlock) {
                    ltp_release_data(data);
                    result = PYION_ERR_LTP_GREEN;
                    goto done;
                }
                receiving_block = 0;
                break;
            case LtpRecvGreenSegment:
                ltp_release_data(data);
                result = PYION_ERR_LTP_RED;
                goto done;
            default:
                break;
        }

        // Make sure other tasks have a chance to run
        sm_TaskYield();
    }

    // If you exited because of closing/interrupt, report it.
    if (atomic_load(&(state->status)) == SAP_CLOSING) {
        result = PYION_ERR_LTP_RECEPTION_CLOSED;
        goto done;
    }

    // If no block received by now and you are not closing, error.
    if (receiving_block == -1) {
        result = PYION_ERR_LTP_BLOCK_NOT_DELIVERED;
        goto done;
    }

    // Get ION SDR
    sdr = getIonsdr();

    // Get content data size. On SDR failure route through done: rather than
    // returning from the macro -- an early return would bypass the receiver-
    // slot reset and refcount drop below, leaving the SAP permanently BUSY
    // and never freed.
    if (!sdr_begin_xn(sdr)) {
        result = PYION_SDR_ERR;
        goto done;
    }
    data_size = zco_source_data_length(sdr, data);
    if (sdr_end_xn(sdr) < 0) {
        result = PYION_SDR_ERR;
        goto done;
    }

    do_malloc = 1;
    payloadObj->payload = (char *)malloc(data_size);

    // Prepare to receive the block
    zco_start_receiving(data, &reader);

    // Get block data
    if (!sdr_begin_xn(sdr)) {
        free(payloadObj->payload);
        result = PYION_SDR_ERR;
        goto done;
    }
    payloadObj->len = zco_receive_source(sdr, &reader, data_size, payloadObj->payload);
    if (sdr_end_xn(sdr) < 0) {
        free(payloadObj->payload);
        result = PYION_SDR_ERR;
        goto done;
    }

    // Handle error while getting the payload
    if (payloadObj->len < 0) {
        if (do_malloc) free(payloadObj->payload);
        result = PYION_ERR_LTP_EXTRACT;
        goto done;
    }

    // Release LTP object now that you are done with it.
    ltp_release_data(data);
    result = 0;

done:
    // Clear the receiver slot. Leave a SAP_CLOSING status intact so a
    // concurrent close is not masked; the deferred free handles teardown.
    pthread_mutex_lock(&(state->state_lock));
    state->receivers = 0;
    if (atomic_load(&(state->status)) != SAP_CLOSING)
        atomic_store(&(state->status), SAP_IDLE);
    pthread_mutex_unlock(&(state->state_lock));

    endpoint_release(state);
    return result;
}
