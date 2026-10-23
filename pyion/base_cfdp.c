#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>

#include <cfdp.h>
#include <bputa.h>

#include "base_cfdp.h"
#include "return_codes.h"
/**
 * Worker file to handle pure C function
 * interaction between PyION and ION.
 *
 */

/* ============================================================================
 * === CFDP params registry and lifecycle
 * ============================================================================
 * Mirrors the BP/LTP layers: the Python side holds an opaque, never-reused
 * integer handle, not a raw pointer. C entry points resolve the handle through
 * a registry of live params blocks, so a stale handle (double close, use after
 * close) is rejected with PYION_INVALID_HANDLE_ERR instead of dereferencing
 * freed memory. The refcount keeps a params block alive while any thread is
 * inside a C call on it, and it is freed only when the last reference drops
 * after a close was requested (deferred free).
 *
 * Lock order: registry_lock before state_lock. op_lock is a leaf lock taken by
 * an operation after it has acquired (pinned) its handle.
 */

static CfdpReqParms  *registry_head = NULL;
static unsigned long  registry_next_handle = 1; // 0 is reserved as "invalid"
static pthread_mutex_t registry_lock = PTHREAD_MUTEX_INITIALIZER;

// Serializes attach/detach, which mutate process-global ION CFDP state.
static pthread_mutex_t ion_cfdp_lock = PTHREAD_MUTEX_INITIALIZER;

static void registry_register(CfdpReqParms *params) {
    pthread_mutex_lock(&registry_lock);
    params->handle = registry_next_handle++;
    params->registry_next = registry_head;
    registry_head = params;
    pthread_mutex_unlock(&registry_lock);
}

static void registry_unregister(CfdpReqParms *params) {
    CfdpReqParms **pp;
    pthread_mutex_lock(&registry_lock);
    for (pp = &registry_head; *pp != NULL; pp = &((*pp)->registry_next)) {
        if (*pp == params) {
            *pp = params->registry_next;
            break;
        }
    }
    pthread_mutex_unlock(&registry_lock);
}

// Find a live params block by handle. Caller must hold registry_lock.
static CfdpReqParms *registry_find_locked(unsigned long handle) {
    CfdpReqParms *p;
    for (p = registry_head; p != NULL; p = p->registry_next)
        if (p->handle == handle)
            return p;
    return NULL;
}

// Tear down the params block. Caller must guarantee no thread is inside a C
// call on it.
static void params_destroy(CfdpReqParms *params) {
    registry_unregister(params);
    pthread_mutex_destroy(&(params->state_lock));
    pthread_mutex_destroy(&(params->op_lock));
    free(params);
}

// Decrement the in-use refcount; free once the last reference drops after a
// close has been requested.
static void params_release(CfdpReqParms *params) {
    int do_free;
    pthread_mutex_lock(&(params->state_lock));
    params->refcount--;
    do_free = (params->refcount == 0 && params->close_requested);
    pthread_mutex_unlock(&(params->state_lock));
    if (do_free)
        params_destroy(params);
}

// Resolve a handle and pin the params block (refcount++). On success returns
// the block; on failure returns NULL and sets *err to PYION_INVALID_HANDLE_ERR
// (unknown handle, or one already marked for close).
static CfdpReqParms *params_acquire(unsigned long handle, int *err) {
    CfdpReqParms *params;

    pthread_mutex_lock(&registry_lock);
    params = registry_find_locked(handle);
    if (params == NULL) {
        pthread_mutex_unlock(&registry_lock);
        *err = PYION_INVALID_HANDLE_ERR;
        return NULL;
    }
    pthread_mutex_lock(&(params->state_lock));
    if (params->close_requested) {
        pthread_mutex_unlock(&(params->state_lock));
        pthread_mutex_unlock(&registry_lock);
        *err = PYION_INVALID_HANDLE_ERR;
        return NULL;
    }
    params->refcount++;
    pthread_mutex_unlock(&(params->state_lock));
    pthread_mutex_unlock(&registry_lock);
    *err = 0;
    return params;
}

/* ============================================================================
 * === Attach / Detach
 * ============================================================================ */

int base_cfdp_attach()
{
    int result;
    pthread_mutex_lock(&ion_cfdp_lock);
    result = cfdp_attach();
    pthread_mutex_unlock(&ion_cfdp_lock);
    return result;
}

void base_cfdp_detach()
{
    pthread_mutex_lock(&ion_cfdp_lock);
    cfdp_detach();
    pthread_mutex_unlock(&ion_cfdp_lock);
}

/* ============================================================================
 * === Open / Close entity
 * ============================================================================ */

int base_cfdp_open(uvast entityId, int lifespan, int classOfService,
                   int ordinal, int srrFlags, int criticality,
                   unsigned long *handle_out)
{
    CfdpReqParms *params;

    *handle_out = 0;

    params = (CfdpReqParms *)calloc(1, sizeof(CfdpReqParms));
    if (params == NULL)
        return PYION_MALLOC_ERR;

    if (pthread_mutex_init(&(params->state_lock), NULL) != 0) {
        free(params);
        return PYION_MALLOC_ERR;
    }
    if (pthread_mutex_init(&(params->op_lock), NULL) != 0) {
        pthread_mutex_destroy(&(params->state_lock));
        free(params);
        return PYION_MALLOC_ERR;
    }

    // Fill in the UT parameters parsed from Python.
    params->utParms.lifespan = lifespan;
    params->utParms.classOfService = classOfService;
    params->utParms.ancillaryData.ordinal = ordinal;
    params->utParms.srrFlags = srrFlags;

    // Compress the destination EID and apply the criticality flag.
    cfdp_compress_number(&(params->destinationEntityNbr), entityId);
    if (criticality == 1)
        params->utParms.ancillaryData.flags |= BP_MINIMUM_LATENCY;
    else
        params->utParms.ancillaryData.flags &= (~BP_MINIMUM_LATENCY);

    // Assign a handle and publish the params block in the registry.
    registry_register(params);
    *handle_out = params->handle;
    return 0;
}

int base_cfdp_close(unsigned long handle)
{
    CfdpReqParms *params;

    pthread_mutex_lock(&registry_lock);
    params = registry_find_locked(handle);
    if (params == NULL) {
        // Unknown handle: never opened, or already closed and freed.
        pthread_mutex_unlock(&registry_lock);
        return PYION_INVALID_HANDLE_ERR;
    }
    pthread_mutex_lock(&(params->state_lock));
    if (params->close_requested) {
        pthread_mutex_unlock(&(params->state_lock));
        pthread_mutex_unlock(&registry_lock);
        return 0;
    }
    params->close_requested = 1;
    params->refcount++; // close holds its own reference
    pthread_mutex_unlock(&(params->state_lock));
    pthread_mutex_unlock(&registry_lock);

    // No per-handle blocking receive to wake: CFDP events are global. The
    // deferred free runs once all in-flight operations on this handle return.
    params_release(params);
    return 0;
}

/* ============================================================================
 * === Internal helpers (no handle; callers already hold a pinned params)
 * ============================================================================ */

static MetadataList base_cfdp_create_usrmsg_list(void)
{
    return cfdp_create_usrmsg_list();
}

static MetadataList base_cfdp_create_fsreq_list(void)
{
    return cfdp_create_fsreq_list();
}

int noteSegmentTime(uvast fileOffset, unsigned int recordOffset,
                    unsigned int length, int sourceFileFd, char *buffer)
{
    writeTimestampLocal(getCtime(), buffer);
    return strlen(buffer) + 1;
}

static void base_cfdp_set_params(CfdpReqParms *params, char *sourceFile, char *destFile,
                                 int segMetadata, int closureLat, long int mode)
{
    // Fill in basic parameters
    params->sourceFileName = sourceFile;
    params->destFileName = destFile;
    params->segMetadataFn = (segMetadata == 0) ? NULL : noteSegmentTime;
    params->closureLatency = closureLat;

    // mode = 1: Select unreliable CFDP mode
    if (mode & 0x01) {
        params->utParms.ancillaryData.flags |= BP_BEST_EFFORT;
        return;
    }

    // mode = 2: Select native BP reliability
    if (mode & 0x02) {
        params->utParms.custodySwitch = SourceCustodyRequired;
        return;
    }

    // Mode = 3: Select CL reliability
    params->utParms.ancillaryData.flags &= (~BP_BEST_EFFORT);
    params->utParms.custodySwitch = NoCustodyRequested;
}

/* ============================================================================
 * === Request-mutating operations (handle-resolved, serialized by op_lock)
 * ============================================================================ */

int base_cfdp_add_usr_msg(unsigned long handle, char *usrMsg)
{
    CfdpReqParms *params;
    int err;

    params = params_acquire(handle, &err);
    if (params == NULL)
        return err;

    pthread_mutex_lock(&(params->op_lock));
    if (params->msgsToUser == 0)
        params->msgsToUser = base_cfdp_create_usrmsg_list();
    oK(cfdp_add_usrmsg(params->msgsToUser, (unsigned char *)usrMsg,
                       strlen((const char *)usrMsg) + 1));
    pthread_mutex_unlock(&(params->op_lock));

    params_release(params);
    return 0;
}

int base_cfdp_add_fs_req(unsigned long handle, int action,
                         char *firstPathName, char *secondPathName)
{
    CfdpReqParms *params;
    int err;

    params = params_acquire(handle, &err);
    if (params == NULL)
        return err;

    pthread_mutex_lock(&(params->op_lock));
    if (params->fsRequests == 0)
        params->fsRequests = base_cfdp_create_fsreq_list();
    oK(cfdp_add_fsreq(params->fsRequests, (CfdpAction)action,
                      firstPathName, secondPathName));
    pthread_mutex_unlock(&(params->op_lock));

    params_release(params);
    return 0;
}

int base_cfdp_send(unsigned long handle, char *sourceFile, char *destFile,
                   int closureLat, int segMetadata, long int mode)
{
    CfdpReqParms *params;
    int err;
    int result = 0;

    params = params_acquire(handle, &err);
    if (params == NULL)
        return err;

    pthread_mutex_lock(&(params->op_lock));

    base_cfdp_set_params(params, sourceFile, destFile, segMetadata, closureLat, mode);

    if (cfdp_put(&(params->destinationEntityNbr), sizeof(BpUtParms),
                 (unsigned char *)&(params->utParms), params->sourceFileName,
                 params->destFileName, NULL, params->segMetadataFn,
                 NULL, 0, NULL, params->closureLatency, params->msgsToUser,
                 params->fsRequests, &(params->transactionId)) < 0) {
        result = PYION_IO_ERR;
    }

    // Reset per-transaction lists whether or not the put succeeded.
    params->msgsToUser = 0;
    params->fsRequests = 0;

    pthread_mutex_unlock(&(params->op_lock));

    params_release(params);
    return result;
}

int base_cfdp_request(unsigned long handle, char *sourceFile, char *destFile,
                      int closureLat, int segMetadata, long int mode)
{
    CfdpReqParms *params;
    CfdpProxyTask task;
    int err;
    int result = 0;

    params = params_acquire(handle, &err);
    if (params == NULL)
        return err;

    pthread_mutex_lock(&(params->op_lock));

    base_cfdp_set_params(params, sourceFile, destFile, segMetadata, closureLat, mode);

    // Create CFDP Task structure
    task.sourceFileName = params->sourceFileName;
    task.destFileName = params->destFileName;
    task.messagesToUser = params->msgsToUser;
    task.filestoreRequests = params->fsRequests;
    task.faultHandlers = NULL;
    task.unacknowledged = 1;
    task.flowLabelLength = 0;
    task.flowLabel = NULL;
    task.recordBoundsRespected = 0;
    task.closureRequested = !(params->closureLatency == 0);

    if (cfdp_get(&(params->destinationEntityNbr), sizeof(BpUtParms),
                 (unsigned char *)&(params->utParms), NULL, NULL, NULL,
                 NULL, 0, NULL, 0, 0, 0, &task, &(params->transactionId)) < 0) {
        result = PYION_IO_ERR;
    }

    // Reset per-transaction lists.
    params->msgsToUser = 0;
    params->fsRequests = 0;

    pthread_mutex_unlock(&(params->op_lock));

    params_release(params);
    return result;
}

int base_cfdp_cancel(unsigned long handle)
{
    CfdpReqParms *params;
    int err;
    int result = 0;

    params = params_acquire(handle, &err);
    if (params == NULL)
        return err;

    pthread_mutex_lock(&(params->op_lock));
    if (cfdp_cancel(&(params->transactionId)) < 0)
        result = PYION_IO_ERR;
    pthread_mutex_unlock(&(params->op_lock));

    params_release(params);
    return result;
}

int base_cfdp_suspend(unsigned long handle)
{
    CfdpReqParms *params;
    int err;
    int result = 0;

    params = params_acquire(handle, &err);
    if (params == NULL)
        return err;

    pthread_mutex_lock(&(params->op_lock));
    if (cfdp_suspend(&(params->transactionId)) < 0)
        result = PYION_IO_ERR;
    pthread_mutex_unlock(&(params->op_lock));

    params_release(params);
    return result;
}

int base_cfdp_resume(unsigned long handle)
{
    CfdpReqParms *params;
    int err;
    int result = 0;

    params = params_acquire(handle, &err);
    if (params == NULL)
        return err;

    pthread_mutex_lock(&(params->op_lock));
    if (cfdp_resume(&(params->transactionId)) < 0)
        result = PYION_IO_ERR;
    pthread_mutex_unlock(&(params->op_lock));

    params_release(params);
    return result;
}

int base_cfdp_report(unsigned long handle)
{
    CfdpReqParms *params;
    int err;
    int result = 0;

    params = params_acquire(handle, &err);
    if (params == NULL)
        return err;

    pthread_mutex_lock(&(params->op_lock));
    if (cfdp_report(&(params->transactionId)) < 0)
        result = PYION_IO_ERR;
    pthread_mutex_unlock(&(params->op_lock));

    params_release(params);
    return result;
}

/* ============================================================================
 * === Process-global event handling (no handle, never lock-wrapped)
 * ============================================================================ */

void base_cfdp_interrupt(void)
{
    cfdp_interrupt();
}

void base_cfdp_decompress_number(uvast *toNbr, CfdpNumber *from)
{
    cfdp_decompress_number(toNbr, from);
}

int base_cfdp_get_usrmsg(MetadataList *list, unsigned char *textBuf, int *length)
{
    return cfdp_get_usrmsg(list, textBuf, length);
}

int base_cfdp_get_fsresp(MetadataList *list, CfdpAction *action,
                         int *status, char *firstFileNameBuf, char *secondFileNameBuf, char *messageBuf)
{
    return cfdp_get_fsresp(list, action, status, firstFileNameBuf,
                           secondFileNameBuf, messageBuf);
}

int base_cfdp_get_event(CfdpEventType *type, time_t *time,
int *reqNbr, CfdpTransactionId *transactionId, char *sourceFileNameBuf, char *destFileNameBuf,
uvast *fileSize, MetadataList *messagesToUser, uvast *offset, unsigned int *length, unsigned int
*recordBoundsRespected, CfdpContinuationState *continuationState, unsigned int *segMetadataLength,
 char *segMetadataBuffer, CfdpCondition *condition, uvast *progress, CfdpFileStatus *fileStatus,
 CfdpDeliveryCode *deliveryCode, CfdpTransactionId *originatingTransactionId, char *statusReportBuf,
 MetadataList *filestoreResponses, unsigned int *closureRequested) {
    return cfdp_get_event(
        type,
        time,
        reqNbr,
        transactionId,
        sourceFileNameBuf,
        destFileNameBuf,
        fileSize,
        messagesToUser,
        offset,
        length,
        recordBoundsRespected,
        continuationState,
        segMetadataLength,
        segMetadataBuffer,
        condition,
        progress,
        fileStatus,
        deliveryCode,
        originatingTransactionId,
        statusReportBuf,
        filestoreResponses,
        closureRequested
    );
}
