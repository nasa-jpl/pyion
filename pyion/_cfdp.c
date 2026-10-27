/* ============================================================================
 * Experimental Python C Extension for CFDP
 *
 * Author: Marc Sanchez Net
 * Date:   04/26/2019
 * Copyright (c) 2019, California Institute of Technology ("Caltech").
 * U.S. Government sponsorship acknowledged.
 * =========================================================================== */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <Python.h>

#include <cfdp.h>
#include <bputa.h>
#include "base_cfdp.h"
#include "return_codes.h"

/* ============================================================================
 * === _cfdp module definitions
 * ============================================================================ */

// Docstring for this module
static char module_docstring[] =
    "Extension module to interface Python and CFDP in ION.";

// Docstring for the functions being wrapped
static char cfdp_attach_docstring[] =
    "Attach to CFDP agent.";
static char cfdp_detach_docstring[] =
    "Detach from CFDP agent.";
static char cfdp_open_docstring[] =
    "Open and CFDP Entity object.";
static char cfdp_close_docstring[] =
    "Close a CFDP Entity object.";
static char cfdp_send_docstring[] =
    "Send a file to another host using CFDP.";
static char cfdp_request_docstring[] =
    "Request a file from another host using CFDP.";
static char cfdp_cancel_docstring[] =
    "Cancel a CFDP transaction.";
static char cfdp_suspend_docstring[] =
    "Suspend a CFDP transaction.";
static char cfdp_resume_docstring[] =
    "Resume a CFDP transaction.";
static char cfdp_report_docstring[] =
    "Report a CFDP transaction.";
static char cfdp_add_usr_msg_docstring[] =
    "Add a user message to the next CFDP transaction.";
static char cfdp_add_fs_req_docstring[] =
    "Add a user message to the next CFDP transaction.";
static char cfdp_next_evs_docstring[] =
    "Get next CFDP event.";
static char cfdp_interrupt_evs_docstring[] =
    "Handle CFDP events.";

// Declare the functions to wrap
static PyObject *pyion_cfdp_attach(PyObject *self, PyObject *args);
static PyObject *pyion_cfdp_detach(PyObject *self, PyObject *args);
static PyObject *pyion_cfdp_open(PyObject *self, PyObject *args);
static PyObject *pyion_cfdp_close(PyObject *self, PyObject *args);
static PyObject *pyion_cfdp_send(PyObject *self, PyObject *args);
static PyObject *pyion_cfdp_request(PyObject *self, PyObject *args);
static PyObject *pyion_cfdp_cancel(PyObject *self, PyObject *args);
static PyObject *pyion_cfdp_suspend(PyObject *self, PyObject *args);
static PyObject *pyion_cfdp_resume(PyObject *self, PyObject *args);
static PyObject *pyion_cfdp_report(PyObject *self, PyObject *args);
static PyObject *pyion_cfdp_add_usr_msg(PyObject *self, PyObject *args);
static PyObject *pyion_cfdp_add_fs_req(PyObject *self, PyObject *args);
static PyObject *pyion_cfdp_next_event(PyObject *self, PyObject *args);
static PyObject *pyion_cfdp_interrupt_events(PyObject *self, PyObject *args);

// Define member functions of this module
static PyMethodDef module_methods[] = {
    {"cfdp_attach", pyion_cfdp_attach, METH_VARARGS, cfdp_attach_docstring},
    {"cfdp_detach", pyion_cfdp_detach, METH_VARARGS, cfdp_detach_docstring},
    {"cfdp_open", pyion_cfdp_open, METH_VARARGS, cfdp_open_docstring},
    {"cfdp_close", pyion_cfdp_close, METH_VARARGS, cfdp_close_docstring},
    {"cfdp_send", pyion_cfdp_send, METH_VARARGS, cfdp_send_docstring},
    {"cfdp_request", pyion_cfdp_request, METH_VARARGS, cfdp_request_docstring},
    {"cfdp_cancel", pyion_cfdp_cancel, METH_VARARGS, cfdp_cancel_docstring},
    {"cfdp_suspend", pyion_cfdp_suspend, METH_VARARGS, cfdp_suspend_docstring},
    {"cfdp_resume", pyion_cfdp_resume, METH_VARARGS, cfdp_resume_docstring},
    {"cfdp_report", pyion_cfdp_report, METH_VARARGS, cfdp_report_docstring},
    {"cfdp_add_usr_msg", pyion_cfdp_add_usr_msg, METH_VARARGS, cfdp_add_usr_msg_docstring},
    {"cfdp_add_filestore_request", pyion_cfdp_add_fs_req, METH_VARARGS, cfdp_add_fs_req_docstring},
    {"cfdp_next_event", pyion_cfdp_next_event, METH_VARARGS, cfdp_next_evs_docstring},
    {"cfdp_interrupt_events", pyion_cfdp_interrupt_events, METH_VARARGS, cfdp_interrupt_evs_docstring},
    {NULL, NULL, 0, NULL}
};

/* ============================================================================
 * === Define _cfdp as a Python module (PEP 489 multi-phase init)
 * ============================================================================ */

static int _cfdp_exec(PyObject *module) {
    // Add CFDP Event Types
    PyModule_AddIntConstant(module, "CfdpNoEvent", CfdpNoEvent);
    PyModule_AddIntConstant(module, "CfdpTransactionInd", CfdpTransactionInd);
    PyModule_AddIntConstant(module, "CfdpEofSentInd", CfdpEofSentInd);
    PyModule_AddIntConstant(module, "CfdpTransactionFinishedInd", CfdpTransactionFinishedInd);
    PyModule_AddIntConstant(module, "CfdpMetadataRecvInd", CfdpMetadataRecvInd);
    PyModule_AddIntConstant(module, "CfdpFileSegmentRecvInd", CfdpFileSegmentRecvInd);
    PyModule_AddIntConstant(module, "CfdpEofRecvInd", CfdpEofRecvInd);
    PyModule_AddIntConstant(module, "CfdpSuspendedInd", CfdpSuspendedInd);
    PyModule_AddIntConstant(module, "CfdpResumedInd", CfdpResumedInd);
    PyModule_AddIntConstant(module, "CfdpReportInd", CfdpReportInd);
    PyModule_AddIntConstant(module, "CfdpFaultInd", CfdpFaultInd);
    PyModule_AddIntConstant(module, "CfdpAbandonedInd", CfdpAbandonedInd);

    // Add CFDP Condition
    PyModule_AddIntConstant(module, "CfdpNoError", CfdpNoError);
    PyModule_AddIntConstant(module, "CfdpAckLimitReached", CfdpAckLimitReached);
    PyModule_AddIntConstant(module, "CfdpKeepaliveLimitReached", CfdpKeepaliveLimitReached);
    PyModule_AddIntConstant(module, "CfdpInvalidTransmissionMode", CfdpInvalidTransmissionMode);
    PyModule_AddIntConstant(module, "CfdpFilestoreRejection", CfdpFilestoreRejection);
    PyModule_AddIntConstant(module, "CfdpChecksumFailure", CfdpChecksumFailure);
    PyModule_AddIntConstant(module, "CfdpFileSizeError", CfdpFileSizeError);
    PyModule_AddIntConstant(module, "CfdpNakLimitReached", CfdpNakLimitReached);
    PyModule_AddIntConstant(module, "CfdpInactivityDetected", CfdpInactivityDetected);
    PyModule_AddIntConstant(module, "CfdpInvalidFileStructure", CfdpInvalidFileStructure);
    PyModule_AddIntConstant(module, "CfdpCheckLimitReached", CfdpCheckLimitReached);
    PyModule_AddIntConstant(module, "CfdpSuspendRequested", CfdpSuspendRequested);
    PyModule_AddIntConstant(module, "CfdpCancelRequested", CfdpCancelRequested);

    // Add CFDP File Status
    PyModule_AddIntConstant(module, "CfdpFileDiscarded", CfdpFileDiscarded);
    PyModule_AddIntConstant(module, "CfdpFileRejected", CfdpFileRejected);
    PyModule_AddIntConstant(module, "CfdpFileRetained", CfdpFileRetained);
    PyModule_AddIntConstant(module, "CfdpFileStatusUnreported", CfdpFileStatusUnreported);

    // Add CFDP file delivery code
    PyModule_AddIntConstant(module, "CfdpDataComplete", CfdpDataComplete);
    PyModule_AddIntConstant(module, "CfdpDataIncomplete", CfdpDataIncomplete);

    // Add CFDP Actions
    PyModule_AddIntConstant(module, "CfdpCreateFile", CfdpCreateFile);
    PyModule_AddIntConstant(module, "CfdpDeleteFile", CfdpDeleteFile);
    PyModule_AddIntConstant(module, "CfdpRenameFile", CfdpRenameFile);
    PyModule_AddIntConstant(module, "CfdpAppendFile", CfdpAppendFile);
    PyModule_AddIntConstant(module, "CfdpReplaceFile", CfdpReplaceFile);
    PyModule_AddIntConstant(module, "CfdpCreateDirectory", CfdpCreateDirectory);
    PyModule_AddIntConstant(module, "CfdpRemoveDirectory", CfdpRemoveDirectory);
    PyModule_AddIntConstant(module, "CfdpDenyFile", CfdpDenyFile);
    PyModule_AddIntConstant(module, "CfdpDenyDirectory", CfdpDenyDirectory);

    return 0;
}

static PyModuleDef_Slot _cfdp_slots[] = {
    {Py_mod_exec, _cfdp_exec},
#ifdef Py_mod_gil
    // The CFDP C layer is hardened for thread-safety (handle registry, refcount
    // lifecycle, per-handle and global locks); declare it safe without the GIL.
    // On pre-3.13 Python Py_mod_gil is undefined and the module stays
    // GIL-required.
    {Py_mod_gil, Py_MOD_GIL_NOT_USED},
#endif
    {0, NULL}};

static struct PyModuleDef moduledef = {
    PyModuleDef_HEAD_INIT,
    "_cfdp",
    module_docstring,
    0, // m_size: multi-phase init requires >= 0
    module_methods,
    _cfdp_slots,
    NULL,
    NULL,
    NULL};

PyMODINIT_FUNC PyInit__cfdp(void) {
    return PyModuleDef_Init(&moduledef);
}

/* ============================================================================
 * === Attach/Detach Functions
 * ============================================================================ */

static PyObject *pyion_cfdp_attach(PyObject *self, PyObject *args) {
    // Define variables
    char err_msg[150];

    // Try to attach to CFDP agent
    int result;

    Py_BEGIN_ALLOW_THREADS
    result = base_cfdp_attach();
    Py_END_ALLOW_THREADS

    if (result < 0) {
        sprintf(err_msg, "Cannot attach to CFDP engine. Is ION running on this host? If so, is CFDP being used?");
        PyErr_SetString(PyExc_SystemError, err_msg);
        return NULL;
    }

    // Return True to indicate success
    Py_RETURN_NONE;
}

static PyObject *pyion_cfdp_detach(PyObject *self, PyObject *args) {
    // Detach from CFDP agent
    Py_BEGIN_ALLOW_THREADS
    base_cfdp_detach();
    Py_END_ALLOW_THREADS

    // Return True to indicate success
    Py_RETURN_NONE;
}

/* ============================================================================
 * === Open/Close Entity Functions
 * ============================================================================ */

static PyObject *pyion_cfdp_open(PyObject *self, PyObject *args) {
    // Define variables
    char err_msg[150];
    uvast entityId;
    int lifespan, classOfService, ordinal, srrFlags, criticality;
    unsigned long handle = 0;
    int status;

    // Parse the input tuple FIRST. Raises error automatically if not possible.
    // Parsing before any allocation means a bad call leaks nothing.
    if (!PyArg_ParseTuple(args, "Kiiiii",
                          (unsigned long long *)&entityId,
                          &lifespan, &classOfService, &ordinal,
                          &srrFlags, &criticality))
        return NULL;

    // Allocate, initialize and register the params block; returns its handle.
    status = base_cfdp_open(entityId, lifespan, classOfService, ordinal,
                            srrFlags, criticality, &handle);
    if (status < 0) {
        if (status == PYION_MALLOC_ERR)
            sprintf(err_msg, "Cannot malloc for CFDP parameters");
        else
            sprintf(err_msg, "Cannot open CFDP entity.");
        PyErr_SetString(PyExc_RuntimeError, err_msg);
        return NULL;
    }

    // Return the opaque handle as an unsigned long.
    return Py_BuildValue("k", handle);
}

static PyObject *pyion_cfdp_close(PyObject *self, PyObject *args) {
    // Define variables
    unsigned long handle;
    int status;

    // Parse the input tuple. Raises error automatically if not possible
    if (!PyArg_ParseTuple(args, "k", &handle))
        return NULL;

    // Request the close. The params block is freed once no in-flight call is
    // using the handle (deferred free).
    Py_BEGIN_ALLOW_THREADS
    status = base_cfdp_close(handle);
    Py_END_ALLOW_THREADS

    if (status == PYION_INVALID_HANDLE_ERR) {
        PyErr_SetString(PyExc_ValueError, "Invalid or already-closed CFDP handle.");
        return NULL;
    }

    // Return True to indicate success
    Py_RETURN_NONE;
}

/* ============================================================================
 * === Small helper to raise from a base-layer return code
 * ============================================================================ */

// Returns 1 and sets a Python exception if status indicates failure; else 0.
static int cfdp_raise_if_error(int status, const char *op_msg) {
    if (status == PYION_INVALID_HANDLE_ERR) {
        PyErr_SetString(PyExc_ValueError, "Invalid or already-closed CFDP handle.");
        return 1;
    }
    if (status < 0) {
        PyErr_SetString(PyExc_RuntimeError, op_msg);
        return 1;
    }
    return 0;
}

/* ============================================================================
 * === Add user messages and filestore requests
 * ============================================================================ */

static PyObject *pyion_cfdp_add_usr_msg(PyObject *self, PyObject *args) {
    // Define variables
    unsigned long handle;
    char *usrMsg;
    int status;

    // Parse the input tuple. Raises error automatically if not possible
    if (!PyArg_ParseTuple(args, "kz", &handle, &usrMsg))
        return NULL;

    // If no user message, return (no-op, but still validate the handle below
    // only if a message is present to mirror the original behavior).
    if (usrMsg == NULL)
        Py_RETURN_NONE;

    Py_BEGIN_ALLOW_THREADS
    status = base_cfdp_add_usr_msg(handle, usrMsg);
    Py_END_ALLOW_THREADS

    if (cfdp_raise_if_error(status, "Cannot add CFDP user message."))
        return NULL;

    Py_RETURN_NONE;
}

static PyObject *pyion_cfdp_add_fs_req(PyObject *self, PyObject *args) {
    // Define variables
    unsigned long handle;
    int action;
    char *firstPathName;
    char *secondPathName;
    int status;

    // Parse the input tuple. Raises error automatically if not possible
    if (!PyArg_ParseTuple(args, "kisz", &handle, &action,
                          &firstPathName, &secondPathName))
        return NULL;

    Py_BEGIN_ALLOW_THREADS
    status = base_cfdp_add_fs_req(handle, action, firstPathName, secondPathName);
    Py_END_ALLOW_THREADS

    if (cfdp_raise_if_error(status, "Cannot add CFDP filestore request."))
        return NULL;

    Py_RETURN_NONE;
}

static PyObject *pyion_cfdp_send(PyObject *self, PyObject *args) {
    // Define variables
    unsigned long handle;
    char *sourceFile;
    char *destFile;
    int closureLat, segMetadata;
    long int mode;
    int status;

    // Parse the input tuple. Raises error automatically if not possible
    if (!PyArg_ParseTuple(args, "ksziil", &handle, &sourceFile,
                          &destFile, &closureLat, &segMetadata, &mode))
        return NULL;

    uvast txNbr = 0;

    Py_BEGIN_ALLOW_THREADS
    status = base_cfdp_send(handle, sourceFile, destFile, closureLat, segMetadata, mode, &txNbr);
    Py_END_ALLOW_THREADS

    if (cfdp_raise_if_error(status, "Cannot do cfdp_put operation, check ion.log."))
        return NULL;

    // Return the transaction number ION assigned so callers can correlate this
    // send with its later CFDP events (e.g. the transaction-finished indication).
    return Py_BuildValue("K", (unsigned long long)txNbr);
}

static PyObject *pyion_cfdp_request(PyObject *self, PyObject *args) {
    // Define variables
    unsigned long handle;
    char *sourceFile;
    char *destFile;
    int closureLat, segMetadata;
    long int mode;
    int status;

    // Parse the input tuple. Raises error automatically if not possible
    if (!PyArg_ParseTuple(args, "ksziil", &handle, &sourceFile,
                          &destFile, &closureLat, &segMetadata, &mode))
        return NULL;

    Py_BEGIN_ALLOW_THREADS
    status = base_cfdp_request(handle, sourceFile, destFile, closureLat, segMetadata, mode);
    Py_END_ALLOW_THREADS

    if (cfdp_raise_if_error(status, "Cannot do cfdp_get operation, check ion.log."))
        return NULL;

    Py_RETURN_NONE;
}

/* ============================================================================
 * === Cancel/Suspend/Resume/Report Functions
 * ============================================================================ */

static PyObject *pyion_cfdp_cancel(PyObject *self, PyObject *args) {
    unsigned long handle;
    int status;

    if (!PyArg_ParseTuple(args, "k", &handle))
        return NULL;

    Py_BEGIN_ALLOW_THREADS
    status = base_cfdp_cancel(handle);
    Py_END_ALLOW_THREADS

    if (cfdp_raise_if_error(status, "Cannot do cfdp_cancel operation, check ion.log."))
        return NULL;

    Py_RETURN_NONE;
}

static PyObject *pyion_cfdp_suspend(PyObject *self, PyObject *args) {
    unsigned long handle;
    int status;

    if (!PyArg_ParseTuple(args, "k", &handle))
        return NULL;

    Py_BEGIN_ALLOW_THREADS
    status = base_cfdp_suspend(handle);
    Py_END_ALLOW_THREADS

    if (cfdp_raise_if_error(status, "Cannot do cfdp_suspend operation, check ion.log."))
        return NULL;

    Py_RETURN_NONE;
}

static PyObject *pyion_cfdp_resume(PyObject *self, PyObject *args) {
    unsigned long handle;
    int status;

    if (!PyArg_ParseTuple(args, "k", &handle))
        return NULL;

    Py_BEGIN_ALLOW_THREADS
    status = base_cfdp_resume(handle);
    Py_END_ALLOW_THREADS

    if (cfdp_raise_if_error(status, "Cannot do cfdp_resume operation, check ion.log."))
        return NULL;

    Py_RETURN_NONE;
}

static PyObject *pyion_cfdp_report(PyObject *self, PyObject *args) {
    unsigned long handle;
    int status;

    if (!PyArg_ParseTuple(args, "k", &handle))
        return NULL;

    Py_BEGIN_ALLOW_THREADS
    status = base_cfdp_report(handle);
    Py_END_ALLOW_THREADS

    if (cfdp_raise_if_error(status, "Cannot do cfdp_report operation, check ion.log."))
        return NULL;

    Py_RETURN_NONE;
}

/* ============================================================================
 * === Handling of CFDP Events (see CCSDS CDFP, section 3.5.6 onwards)
 * ============================================================================ */

static PyObject *pyion_cfdp_next_event(PyObject *self, PyObject *args) {
    // Define variables for cfdp_get_event
    CfdpEventType type;
    time_t time;
    int reqNbr;
    CfdpTransactionId transactionId;
	char sourceFileNameBuf[256];
	char destFileNameBuf[256];
    uvast			fileSize;
	MetadataList		messagesToUser;
	uvast			offset;
	unsigned int		length;
	unsigned int		recordBoundsRespected;
	CfdpContinuationState	continuationState;
	unsigned int		segMetadataLength;
	char			segMetadata[63];
	CfdpCondition		condition;
	uvast			progress;
	CfdpFileStatus		fileStatus;
	CfdpDeliveryCode	deliveryCode;
	CfdpTransactionId	originatingTransactionId;
	char			statusReportBuf[256];
	MetadataList		filestoreResponses;
    unsigned int closureRequested;

    // Define other variables
    char err_msg[150];
    int rx_ret;
    uvast transaction_id, source_entity_nbr;

    // Receive the next CFDP event. This is a blocking call. It is process
    // global and takes no handle, so it is never lock-wrapped.
    Py_BEGIN_ALLOW_THREADS                                // Release the GIL
    rx_ret = base_cfdp_get_event(&type, &time, &reqNbr,
        &transactionId, sourceFileNameBuf, destFileNameBuf,
        &fileSize, &messagesToUser, &offset, &length,
        &recordBoundsRespected, &continuationState,
        &segMetadataLength, segMetadata,
        &condition, &progress, &fileStatus,
        &deliveryCode, &originatingTransactionId,
        statusReportBuf, &filestoreResponses,
        &closureRequested
    );
    Py_END_ALLOW_THREADS                                  // Acquire the GIL

    // If reception of event failed, return
    if (rx_ret < 0) {
        sprintf(err_msg, "Failed while getting CFDP event, check ion.log.");
        PyErr_SetString(PyExc_RuntimeError, err_msg);
        return NULL;
    }

    // If no event, just return
    if (type == CfdpNoEvent) {
        return Py_BuildValue("(i, z)", (int)CfdpNoEvent, NULL);
    }

    // Every event carries a transaction identity. Expose the source entity and
    // transaction number consistently across all event dictionaries so a caller
    // consuming the node-global CFDP queue can correlate events by
    // (source_entity_id, transaction_id). The source entity is taken from the
    // event's own transactionId, not the proxy-only originatingTransactionId,
    // which is zero for a direct (non-proxy) transfer.
    base_cfdp_decompress_number(&transaction_id, &(transactionId.transactionNbr));
    base_cfdp_decompress_number(&source_entity_nbr, &(transactionId.sourceEntityNbr));

    // Handle CfdpTransactionInd
    if (type == CfdpTransactionInd) {
        return Py_BuildValue("(i, {s:K, s:K})", (int)CfdpTransactionInd,
                                           "transaction_id", (unsigned long long)transaction_id,
                                           "source_entity_id", (unsigned long long)source_entity_nbr);
    }

    // Handle CfdpEofSentInd
    if (type == CfdpEofSentInd) {
        return Py_BuildValue("(i, {s:K, s:K})", (int)CfdpEofSentInd,
                                           "transaction_id", (unsigned long long)transaction_id,
                                           "source_entity_id", (unsigned long long)source_entity_nbr);
    }

    // Handle CfdpEofRecvInd
    if (type == CfdpEofRecvInd) {
        return Py_BuildValue("(i, {s:K, s:K})", (int)CfdpEofRecvInd,
                                           "transaction_id", (unsigned long long)transaction_id,
                                           "source_entity_id", (unsigned long long)source_entity_nbr);
    }

    // Handle CfdpSuspendedInd
    if (type == CfdpSuspendedInd) {
        return Py_BuildValue("(i, {s:K, s:K, s:i})", (int)CfdpSuspendedInd,
                                           "transaction_id", (unsigned long long)transaction_id,
                                           "source_entity_id", (unsigned long long)source_entity_nbr,
                                           "condition", (int)condition);
    }

    // Handle CfdpResumedInd
    if (type == CfdpResumedInd) {
        return Py_BuildValue("(i, {s:K, s:K, s:K})", (int)CfdpResumedInd,
                                           "transaction_id", (unsigned long long)transaction_id,
                                           "source_entity_id", (unsigned long long)source_entity_nbr,
                                           "progress", (unsigned long long)progress);
    }

    // Handle CfdpReportInd
    if (type == CfdpReportInd) {
        return Py_BuildValue("(i, {s:K, s:K, s:i})", (int)CfdpReportInd,
                                                "transaction_id", (unsigned long long)transaction_id,
                                                "source_entity_id", (unsigned long long)source_entity_nbr,
                                                "status", (int)fileStatus);
    }

    // Handle CfdpFaultInd
    if (type == CfdpFaultInd) {
        return Py_BuildValue("(i, {s:K, s:K, s:i, s:K})", (int)CfdpFaultInd,
                                                     "transaction_id", (unsigned long long)transaction_id,
                                                     "source_entity_id", (unsigned long long)source_entity_nbr,
                                                     "code", (int)deliveryCode,
                                                     "progress", (unsigned long long)progress);
    }

    // Handle CfdpAbandonedInd
    if (type == CfdpAbandonedInd) {
        return Py_BuildValue("(i, {s:K, s:K, s:i, s:K})", (int)CfdpAbandonedInd,
                                                     "transaction_id", (unsigned long long)transaction_id,
                                                     "source_entity_id", (unsigned long long)source_entity_nbr,
                                                     "code", (int)deliveryCode,
                                                     "progress", (unsigned long long)progress);
    }

    // Handle CfdpFileSegmentRecvInd
    if (type == CfdpFileSegmentRecvInd) {
        return Py_BuildValue("(i, {s:K, s:K, s:K, s:I})", (int)CfdpFileSegmentRecvInd,
                                               "transaction_id", (unsigned long long)transaction_id,
                                               "source_entity_id", (unsigned long long)source_entity_nbr,
                                               "offset", (unsigned long long)offset,
                                               "length", (unsigned int)length);
    }

    // Define variables
    unsigned char usrmsgBuf[256];
    PyObject*     py_usrmsgs = PyList_New(0);

    // Handle CfdpMetadataRecvInd
    if (type == CfdpMetadataRecvInd) {
        // transaction_id and source_entity_nbr were decompressed above from the
        // event's own transactionId; for a received file that source entity is
        // the sender, which is what the receiver needs to report.

        // Get all messages
        while (messagesToUser) {
            // Get next message
            if (base_cfdp_get_usrmsg(&messagesToUser, usrmsgBuf, (int *)&length) < 0) {
                sprintf(err_msg, "Failed getting user messages, check ion.log.");
                PyErr_SetString(PyExc_RuntimeError, err_msg);
                return NULL;
            }

            // If empty message, continue
            if (length == 0)
                continue;

            // Indicate end of user message
            usrmsgBuf[length] = '\0';

            // Add the user message to the list
            PyList_Append(py_usrmsgs, Py_BuildValue("s", (char *)usrmsgBuf));
        }

        // Build the return value
        return Py_BuildValue("(i, {s:K, s:K, s:K, s:s, s:s, s:O})",
                            (int)CfdpMetadataRecvInd,
                            "transaction_id", (unsigned long long)transaction_id,
                            "source_entity_id", (unsigned long long)source_entity_nbr,
                            "file_size", (unsigned long long)fileSize,
                            "source_file_name", sourceFileNameBuf,
                            "dest_file_name", destFileNameBuf,
                            "user_messages", py_usrmsgs);
    }

    // Define variables
    CfdpAction	action;
	int			status;
	char		firstPathName[256];
	char		secondPathName[256];
	char		msgBuf[256];
    PyObject*   py_fs = PyDict_New();

    // Handle CfdpTransactionFinishedInd
    if (type == CfdpTransactionFinishedInd) {
        // Expose the transaction-level results
        PyDict_SetItemString(py_fs, "transaction_id", Py_BuildValue("K", (unsigned long long)transaction_id));
        PyDict_SetItemString(py_fs, "source_entity_id", Py_BuildValue("K", (unsigned long long)source_entity_nbr));
        PyDict_SetItemString(py_fs, "status_report", Py_BuildValue("s", statusReportBuf));
        PyDict_SetItemString(py_fs, "condition_code", Py_BuildValue("i", (int)condition));
        PyDict_SetItemString(py_fs, "file_status", Py_BuildValue("i", (int)fileStatus));
        PyDict_SetItemString(py_fs, "delivery_code", Py_BuildValue("i", (int)deliveryCode));

        // Get all filestore responses
        PyObject* py_filestore_response_list = PyList_New(0);
        while (filestoreResponses) {
            // Get next response
            if (base_cfdp_get_fsresp(&filestoreResponses, &action, &status, firstPathName,
                                secondPathName, msgBuf) < 0) {
                sprintf(err_msg, "Failed getting FS response, check ion.log.");
                PyErr_SetString(PyExc_RuntimeError, err_msg);
                return NULL;
            }

            // If no action
            if (action == ((CfdpAction) -1))
                continue;

            // Build a value for this filestore response
            PyObject* py_filestore_response = PyDict_New();
            PyDict_SetItemString(py_filestore_response, "action_code", Py_BuildValue("i", (int)action));
            PyDict_SetItemString(py_filestore_response, "status_code", Py_BuildValue("i", (int)status));
            PyDict_SetItemString(py_filestore_response, "first_file_name", Py_BuildValue("s", firstPathName));
            PyDict_SetItemString(py_filestore_response, "second_file_name", Py_BuildValue("s", secondPathName));
            PyDict_SetItemString(py_filestore_response, "filestore_message", Py_BuildValue("s", msgBuf));
            PyList_Append(py_filestore_response_list, py_filestore_response);
        }

        // Store the filestore responses
        PyDict_SetItemString(py_fs, "filestore_responses", py_filestore_response_list);

        // Build return value
        return Py_BuildValue("(i, O)", (int)CfdpTransactionFinishedInd, py_fs);
    }

    // If you reach this point, you cannot process this event type
    sprintf(err_msg, "Unknown CFDP type.");
    PyErr_SetString(PyExc_RuntimeError, err_msg);
    return NULL;
}

static PyObject *pyion_cfdp_interrupt_events(PyObject *self, PyObject *args) {
    // Interrupt CFDP event handler (process-global; no handle, no lock)
    base_cfdp_interrupt();

    // Return None to indicate success
    Py_RETURN_NONE;
}
