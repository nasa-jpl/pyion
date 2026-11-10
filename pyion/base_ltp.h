#ifndef BASE_LTP_H
#define BASE_LTP_H

#define PYION_ERR_LTP_NOTICE -128001
#define PYION_ERR_LTP_IMPORT -128002
#define PYION_ERR_LTP_EXPORT -128003
#define PYION_ERR_LTP_GREEN -128004
#define PYION_ERR_LTP_RED -128005
#define PYION_ERR_LTP_RECEPTION_CLOSED -128006
#define PYION_ERR_LTP_BLOCK_NOT_DELIVERED -128007
#define PYION_ERR_LTP_EXTRACT -128008

#include <ltp.h>
#include <pthread.h>
#include <stdatomic.h>

// Possible states of an LTP service access point
typedef enum {
    SAP_IDLE = 0,
    SAP_RUNNING,
    SAP_CLOSING
} LtpStateEnum;

// State of the LTP service access point.
//
// Thread-safety mirrors the BP layer: the Python side holds an opaque,
// never-reused ``handle`` resolved through a registry, so a stale handle
// (double close, use after close) is rejected instead of dereferencing freed
// memory. ``status`` is atomic; ``state_lock`` is a short-held mutex never
// held across a blocking ION call; the SAP is freed only when the last
// in-flight call returns (refcount-based deferred free).
typedef struct LtpSAP {
    unsigned int clientId;      // 1=BP, 2=SDA, 3=CFDP, other numbers available
    atomic_int status;

    pthread_mutex_t state_lock;
    int refcount;
    int receivers;
    int close_requested;

    pthread_mutex_t send_lock;

    unsigned long handle;
    struct LtpSAP *registry_next;
} LtpSAP;


typedef struct
{
    char *payload;
    int len;
    int do_malloc;
    unsigned char reasonCode;
} LtpRxPayload;




typedef struct
{
    unsigned long long destEngineId;
    char *data;
    int data_size;
    LtpSessionId   sessionId;

} LtpTxPayload;



/*
Attaches to ltp endpoint.
*/
int base_ltp_attach(void);

void base_ltp_detach(void);

/* base_ltp_open registers the new SAP and sets its ``handle`` field, the
 * opaque handle to hand back to the caller. The functions below take that
 * handle and resolve it through the registry; an unknown handle yields
 * PYION_INVALID_HANDLE_ERR rather than a use-after-free.
 */
int base_ltp_open(unsigned int clientId, LtpSAP **state);

int base_ltp_close(unsigned long handle);

int base_ltp_interrupt(unsigned long handle);

int base_ltp_send(unsigned long handle, LtpTxPayload *txInfo);

int base_ltp_receive_data(unsigned long handle, LtpRxPayload *payloadObj);



#endif