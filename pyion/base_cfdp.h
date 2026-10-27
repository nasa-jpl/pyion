/**
 * Shim-layer
 */
#ifndef BASE_CFDP_H
#define BASE_CFDP_H

#include <cfdp.h>
#include <bputa.h>
#include <pthread.h>

// CFDP request parameters plus thread-safety bookkeeping.
//
// Thread-safety mirrors the BP/LTP layers: the Python side holds an opaque,
// never-reused ``handle`` resolved through a registry, so a stale handle
// (double close, use after close) is rejected instead of dereferencing freed
// memory. ``state_lock`` is a short-held mutex guarding the refcount and the
// close flag; the params block is freed only when the last in-flight call
// returns (refcount-based deferred free). ``op_lock`` serializes the
// request-mutating operations (send/request/cancel/.../add_usr_msg) on one
// handle, since they read and write the shared params fields and the ION CFDP
// transaction they drive.
//
// Unlike BP/LTP there is no per-handle blocking receive: CFDP events are read
// through the process-global cfdp_get_event / cfdp_interrupt, which take no
// handle and are never lock-wrapped (the "no lock across a blocking call"
// rule). So this struct needs no atomic status or receiver bookkeeping.
typedef struct CfdpReqParms
{
    CfdpHandler faultHandlers[16];
    CfdpNumber destinationEntityNbr;
    char *sourceFileName;
    char destFileNameBuf[256];
    char *destFileName;
    BpUtParms utParms;
    unsigned int closureLatency;
    CfdpMetadataFn segMetadataFn;
    MetadataList msgsToUser;
    MetadataList fsRequests;
    CfdpTransactionId transactionId;

    // Thread-safety bookkeeping (see comment above).
    pthread_mutex_t state_lock;
    int refcount;
    int close_requested;
    pthread_mutex_t op_lock;
    unsigned long handle;
    struct CfdpReqParms *registry_next;
} CfdpReqParms;

int base_cfdp_attach();

void base_cfdp_detach();

/* base_cfdp_open allocates a params block, registers it, and returns its
 * opaque ``handle`` through *handle_out. The functions below take that handle
 * and resolve it through the registry; an unknown handle yields
 * PYION_INVALID_HANDLE_ERR rather than a use-after-free. */
int base_cfdp_open(uvast entityId, int lifespan, int classOfService,
                   int ordinal, int srrFlags, int criticality,
                   unsigned long *handle_out);

int base_cfdp_close(unsigned long handle);

int base_cfdp_add_usr_msg(unsigned long handle, char *usrMsg);

int base_cfdp_add_fs_req(unsigned long handle, int action,
                         char *firstPathName, char *secondPathName);

int base_cfdp_send(unsigned long handle, char *sourceFile, char *destFile,
                   int closureLat, int segMetadata, long int mode,
                   uvast *transactionNbr);

int base_cfdp_request(unsigned long handle, char *sourceFile, char *destFile,
                      int closureLat, int segMetadata, long int mode);

int base_cfdp_cancel(unsigned long handle);

int base_cfdp_suspend(unsigned long handle);

int base_cfdp_resume(unsigned long handle);

int base_cfdp_report(unsigned long handle);

/* Process-global event handling: no handle, never lock-wrapped. */
void base_cfdp_interrupt(void);

void base_cfdp_decompress_number(uvast *toNbr, CfdpNumber *from);

int base_cfdp_get_usrmsg(MetadataList *list, unsigned char *textBuf, int *length);

int base_cfdp_get_fsresp(MetadataList *list, CfdpAction *action,
                         int *status, char *firstFileNameBuf, char *secondFileNameBuf, char *messageBuf);

int base_cfdp_get_event(CfdpEventType *type, time_t *time,
int *reqNbr, CfdpTransactionId *transactionId, char *sourceFileNameBuf, char *destFileNameBuf,
uvast *fileSize, MetadataList *messagesToUser, uvast *offset, unsigned int *length, unsigned int
*recordBoundsRespected, CfdpContinuationState *continuationState, unsigned int *segMetadataLength,
 char *segMetadataBuffer, CfdpCondition *condition, uvast *progress, CfdpFileStatus *fileStatus,
 CfdpDeliveryCode *deliveryCode, CfdpTransactionId *originatingTransactionId, char *statusReportBuf,
 MetadataList *filestoreResponses, unsigned int *closureRequested);

#endif
