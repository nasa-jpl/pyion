#ifndef BASEBP_H
#define BASEBP_H

#include <bp.h>
#include <pthread.h>
#include <stdatomic.h>

#define MAX_PREALLOC_BUFFER 1024

/**
 * Shim-layer 
 */

// Possible states of an enpoint. This is used to avoid race conditions
// when closing receiving threads.
typedef enum
{
    EID_IDLE = 0,
    EID_RUNNING,
    EID_CLOSING,
    EID_INTERRUPTING
} SapStateEnum;


// A combination of a BpSAP object and a representation of its status.
//
// Thread-safety:
//  - ``status`` holds SapStateEnum values but is declared atomic because it is
//    read by a receiver thread (in its blocking loop) while being written by
//    interrupt/close on other threads.
//  - ``state_lock`` is a short-held mutex protecting the bookkeeping fields
//    below. It is NEVER held across a blocking ION call.
//  - ``refcount`` tracks how many threads are currently inside a C call on
//    this state. The state is freed only when refcount drops to 0 after a
//    close has been requested (deferred free), preventing use-after-free.
//  - ``receivers`` enforces at most one concurrent bp_receive (0 or 1).
//  - ``close_requested`` marks that a close is pending; once set, no new
//    operation may acquire the state.
typedef struct
{
    BpSAP sap;
    atomic_int status;
    int detained;
    ReqAttendant *attendant;

    pthread_mutex_t state_lock;
    int refcount;
    int receivers;
    int close_requested;
} BpSapState;


/**
 * Struct designed to contain or reference received message information.
 * To avoid calls to malloc, we can preallocate a buffer on the stack to
 * contain the received message. If this buffer is of insufficient size,
 * Then it is up to us to dynamically allocate *payload and free it when
 * necessary. 
 */
typedef struct
{
    int len;
    int do_malloc;
    char payload_prealloc[MAX_PREALLOC_BUFFER];
    char *payload; // If we must malloc, remember to set do_malloc to 1         
    char *bundleSourceEid;
	BpTimestamp	bundleCreationTime;
	unsigned int timeToLive;
    unsigned char	metadataType;	/*	See RFC 6258.		*/
	unsigned char	metadataLen;
	unsigned char	metadata[BP_MAX_METADATA_LEN];
} BpRx;

/**
 * Struct designed to reference data to pass to functions involved
 * in sending messages. The user must provide memory management for
 * the strings.*/
typedef struct
{
    char *data;
    char *destEid;
    char *reportEid;
    int ttl;
    int classOfService;
    int custodySwitch;
    int rrFlags;
    int ackReq;
    unsigned int retxTimer;
    BpAncillaryData *ancillaryData;
    int data_size;

} BpTx;

/** The following "shim-layer" functions call their respective
 * counterparts in ION while handling state information, so that
 * the user of this API does not have to themself.
 *
 * In addition, the various paramaters needed for message reception,
 * transmission, and memory management are encapulated into BpRx and 
 * BpTx.
 */

/**
 * Calls bp_attach, returns result
 */
int base_bp_attach();

/**
 * Calls bp_detach, which is a void function
 */
void base_bp_detach();

/**
 * Request that an endpoint be closed. Marks the state as closing, wakes any
 * blocked receiver, and frees the state once no thread is using it. After
 * this call returns, ``state`` must not be used by the caller again.
 */
int base_bp_close(BpSapState *state);

int base_bp_interrupt(BpSapState *state);

int base_bp_receive_data(BpSapState *state, BpRx *msg);

int base_bp_send(BpSapState *state, BpTx *txInfo);

int base_bp_open(BpSapState **state, char *ownEid, int detained, int mem_ctrl);



/**
 * Helper "constructor" functions for BpTx and BpRx objects
 */

// Initialize BpTx structure
int base_init_bp_tx_payload(BpTx *obj);

// Initialize BpRx structure
int base_init_bp_rx_payload(BpRx *obj);

#endif
