/* ============================================================================
 * Experimental Python C Extension to interface with ION's LTP implementation
 * 
 * Author: Marc Sanchez Net
 * Date:   05/30/2019
 * Copyright (c) 2019, California Institute of Technology ("Caltech").  
 * U.S. Government sponsorship acknowledged.
 * =========================================================================== */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>
#include <ion.h>
#include <zco.h>
#include <ltp.h>
#include <ltpP.h>
#include <Python.h>

#include "_utils.c"
#include "base_ltp.h"

/* ============================================================================
 * === _ltp module definitions
 * ============================================================================ */

// Docstring for this module
static char module_docstring[] =
    "Extension module to interface Python and Licklider Transmission Protocol in ION.";

// Docstring for the functions being wrapped
static char ltp_attach_docstring[] =
    "Attach to the local LTP engine.";
static char ltp_detach_docstring[] =
    "Detach from the local LTP engine.";
static char ltp_open_docstring[] =
    "Open a connection to the local LTP engine.";
static char ltp_close_docstring[] =
    "Close a connection to the local LTP engine.\n";
static char ltp_send_docstring[] =
    "Send a blob of bytes using LTP.";
static char ltp_receive_docstring[] =
    "Receive a blob of bytes using LTP.";
static char ltp_interrupt_docstring[] =
    "Interrupt the reception of LTP data.";
static char ltp_init_docstring[] =
    "Initialize LTP database.";
static char ltp_dequeue_outbound_segment_docstring[] =
    "Dequeue an outbound LTP segment.";
static char ltp_handle_inbound_segment_docstring[] =
    "Enqueue an inbound LTP segment.";

// Declare the functions to wrap
static PyObject *pyion_ltp_attach(PyObject *self, PyObject *args);
static PyObject *pyion_ltp_detach(PyObject *self, PyObject *args);
static PyObject *pyion_ltp_open(PyObject *self, PyObject *args);
static PyObject *pyion_ltp_close(PyObject *self, PyObject *args);
static PyObject *pyion_ltp_send(PyObject *self, PyObject *args);
static PyObject *pyion_ltp_receive(PyObject *self, PyObject *args);
static PyObject *pyion_ltp_interrupt(PyObject *self, PyObject *args);
static PyObject *pyion_ltp_init(PyObject *self, PyObject *args);
static PyObject *pyion_ltp_dequeue_outbound_segment(PyObject *self, PyObject *args);
static PyObject *pyion_ltp_handle_inbound_segment(PyObject *self, PyObject *args);

// Define member functions of this module
static PyMethodDef module_methods[] = {
    {"ltp_attach", pyion_ltp_attach, METH_VARARGS, ltp_attach_docstring},
    {"ltp_detach", pyion_ltp_detach, METH_VARARGS, ltp_detach_docstring},
    {"ltp_open", pyion_ltp_open, METH_VARARGS, ltp_open_docstring},
    {"ltp_close", pyion_ltp_close, METH_VARARGS, ltp_close_docstring},
    {"ltp_send", pyion_ltp_send, METH_VARARGS, ltp_send_docstring},
    {"ltp_receive", pyion_ltp_receive, METH_VARARGS, ltp_receive_docstring},
    {"ltp_interrupt", pyion_ltp_interrupt, METH_VARARGS, ltp_interrupt_docstring},
    {"ltp_init", pyion_ltp_init, METH_VARARGS, ltp_init_docstring},
    {"ltp_dequeue_outbound_segment", pyion_ltp_dequeue_outbound_segment, METH_VARARGS, ltp_dequeue_outbound_segment_docstring},
    {"ltp_handle_inbound_segment", pyion_ltp_handle_inbound_segment, METH_VARARGS, ltp_handle_inbound_segment_docstring},
    {NULL, NULL, 0, NULL}
};

/* ============================================================================
 * === Define _ltp as a Python module (multi-phase initialization, PEP 489)
 * ============================================================================ */

// Module execution slot: _ltp has no constants to register.
static int _ltp_exec(PyObject *module) {
    (void)module;
    return 0;
}

static PyModuleDef_Slot _ltp_slots[] = {
    {Py_mod_exec, _ltp_exec},
#ifdef Py_mod_gil
    // The LTP C layer is hardened for thread-safety (handle registry, refcount
    // lifecycle, per-SAP and global locks); declare it safe without the GIL.
    // On pre-3.13 Python Py_mod_gil is undefined and the module stays
    // GIL-required.
    {Py_mod_gil, Py_MOD_GIL_NOT_USED},
#endif
    {0, NULL}};

static struct PyModuleDef moduledef = {
    PyModuleDef_HEAD_INIT,
    "_ltp",
    module_docstring,
    0, // m_size: multi-phase init requires >= 0
    module_methods,
    _ltp_slots,
    NULL,
    NULL,
    NULL};

PyMODINIT_FUNC PyInit__ltp(void) {
    return PyModuleDef_Init(&moduledef);
}

/* ============================================================================
 * === Define global variables
 * ============================================================================ */

#define MAX_LTP_SESSIONS 1024

/* ============================================================================
 * === Attach/Detach Functions
 * ============================================================================ */

static PyObject *pyion_ltp_attach(PyObject *self, PyObject *args) {
    // Define variables
    char err_msg[150];
    // Try to attach to BP agent
    if (base_ltp_attach() < 0) {
        sprintf(err_msg, "Cannot attach to LTP engine. Is ION running on this host?");
        PyErr_SetString(PyExc_SystemError, err_msg);
        return NULL;
    }
    Py_RETURN_NONE;
}

static PyObject *pyion_ltp_detach(PyObject *self, PyObject *args) {
    // Detach from BP agent
    base_ltp_detach();
    Py_RETURN_NONE;
}

/* ============================================================================
 * === Open/Close Endpoint Functions
 * ============================================================================ */

static PyObject *pyion_ltp_open(PyObject *self, PyObject *args) {
    // Define variables
    unsigned int clientId;
    char err_msg[150];
    LtpSAP *state = NULL;    
    
    // Parse the input tuple. Raises error automatically if not possible
    if (!PyArg_ParseTuple(args, "I", &clientId))
        return NULL;

    // Open connection to LTP client

    int open_status = base_ltp_open(clientId, &state);

    if (open_status < 0) {
        switch (open_status) {
            case PYION_MALLOC_ERR:
                sprintf(err_msg, "Failed to malloc for LtpSapState");
                PyErr_SetString(PyExc_MemoryError, err_msg);
                return NULL;
            default:
                sprintf(err_msg, "Cannot open LTP client access point.");
                PyErr_SetString(PyExc_RuntimeError, err_msg);
                 return NULL;
        }
    }

    // Return the SAP's opaque handle as an unsigned long.
    return Py_BuildValue("k", state->handle);
}

static PyObject *pyion_ltp_close(PyObject *self, PyObject *args) {
    // Define variables
    unsigned long handle;
    int status;

    // Parse the input tuple. Raises error automatically if not possible
    if (!PyArg_ParseTuple(args, "k", &handle))
        return NULL;

    // Request the close. base_ltp_close wakes any blocked receiver and defers
    // the actual free until no thread is using the SAP.
    Py_BEGIN_ALLOW_THREADS
    status = base_ltp_close(handle);
    Py_END_ALLOW_THREADS

    if (status == PYION_INVALID_HANDLE_ERR) {
        PyErr_SetString(PyExc_ValueError, "Invalid or already-closed LTP handle.");
        return NULL;
    }

    Py_RETURN_NONE;
}

/* ============================================================================
 * === Interrupt Endpoint Functions
 * ============================================================================ */

static PyObject *pyion_ltp_interrupt(PyObject *self, PyObject *args) {
    // Define variables
    unsigned long handle;

    // Parse the input tuple. Raises error automatically if not possible
    if (!PyArg_ParseTuple(args, "k", &handle))
        return NULL;

    // base_ltp_interrupt is a no-op unless the SAP is currently receiving.
    if (base_ltp_interrupt(handle) == PYION_INVALID_HANDLE_ERR) {
        PyErr_SetString(PyExc_ValueError, "Invalid or already-closed LTP handle.");
        return NULL;
    }

    Py_RETURN_NONE;
}

/* ============================================================================
 * === Send Functionality
 * ============================================================================ */

static PyObject *pyion_ltp_send(PyObject *self, PyObject *args) {
    // Define variables
    char           err_msg[150];
    unsigned long  handle;
    int            ok;
    LtpTxPayload   txInfo;
    Py_ssize_t     data_size;

    // Parse input arguments. First one is the SAP handle.
    if (!PyArg_ParseTuple(args, "kKs#", &handle, &(txInfo.destEngineId),
     &(txInfo.data), &data_size))
        return NULL;

    // Unsafe cast from Py_ssize_t to int. Mandatory starting with python 3.10
    txInfo.data_size = (int)data_size;

    // Send using LTP protocol. All data is sent as RED LTP by definition.
    // NOTE 1: SessionId is filled by ``ltp_send``, but you do not care about it
    // NOTE 2: In general, ltp_send does not block. However, if you exceed the max
    //         number of export sessions defined in ltprc, then it will.
    Py_BEGIN_ALLOW_THREADS
    ok = base_ltp_send(handle, &txInfo);
    Py_END_ALLOW_THREADS

    // Handle error in ltp_send
    if (ok == PYION_INVALID_HANDLE_ERR) {
        PyErr_SetString(PyExc_ValueError, "Invalid or already-closed LTP handle.");
        return NULL;
    }
    if (ok <= 0) {
        sprintf(err_msg, "Error while sending the data through LTP (err code=%i)", ok);
        PyErr_SetString(PyExc_RuntimeError, err_msg);
        return NULL;
    }

    Py_RETURN_NONE;
}

/* ============================================================================
 * === Receive Functionality
 * ============================================================================ */

static PyObject *pyion_ltp_receive(PyObject *self, PyObject *args) {
    char err_msg[150];

    // Define variables
    unsigned long handle;
    LtpRxPayload payloadObj;
    int ok;

    // Parse the input tuple. Raises error automatically if not possible
    if (!PyArg_ParseTuple(args, "k", &handle))
        return NULL;

    // Trigger reception of data
    Py_BEGIN_ALLOW_THREADS
    ok = base_ltp_receive_data(handle, &payloadObj);
    Py_END_ALLOW_THREADS


    switch(ok) {

        case (PYION_MALLOC_ERR):
            sprintf(err_msg, "Cannot malloc for LTP state.");
            PyErr_SetString(PyExc_MemoryError, err_msg); 
            return NULL;

        case (PYION_ERR_LTP_IMPORT):
            sprintf(err_msg, "LTP import session cancelled (reason code=%d)", (unsigned int)payloadObj.reasonCode);
            PyErr_SetString(PyExc_RuntimeError, err_msg); 
            return NULL;

        case (PYION_ERR_LTP_EXPORT):
            sprintf(err_msg, "LTP export session cancelled (reason code=%d)", (unsigned int)payloadObj.reasonCode);
            PyErr_SetString(PyExc_RuntimeError, err_msg);
            return NULL;

        case (PYION_ERR_LTP_GREEN):
            PyErr_SetString(PyExc_NotImplementedError, "An LTP block cannot have green parts");
            return NULL;
        
        case (PYION_ERR_LTP_RED):
            PyErr_SetString(PyExc_NotImplementedError, "Only red part of LTP is supported");
            return NULL;
        
        case (PYION_ERR_LTP_RECEPTION_CLOSED):
            PyErr_SetString(PyExc_ConnectionAbortedError, "LTP reception closed.");
            return NULL;
        
        case (PYION_ERR_LTP_BLOCK_NOT_DELIVERED):
            PyErr_SetString(PyExc_RuntimeError, "LTP block was not delivered as expected.");
            return NULL;

        case (PYION_ERR_LTP_EXTRACT):
            sprintf(err_msg, "Error extracting data from block");
            //PyErr_SetString(PyExc_IOError, err_msg);
            return NULL;

        case (PYION_INVALID_HANDLE_ERR):
            PyErr_SetString(PyExc_ValueError, "Invalid or already-closed LTP handle.");
            return NULL;

        case (PYION_BUSY_ERR):
            PyErr_SetString(PyExc_RuntimeError, "Another receive is already in progress on this LTP client.");
            return NULL;

        default:
        ;
    }

    // base_ltp_receive_data resets the SAP status and the deferred free
    // handles teardown; nothing to do here on the success path.
    PyObject *ret = Py_BuildValue("y#", payloadObj.payload, (Py_ssize_t)payloadObj.len);

    free(payloadObj.payload);

    // Return value
    return ret;
}

/* ============================================================================
 * === Segment Queueing Functionality
 * ============================================================================ */

static PyObject *pyion_ltp_init(PyObject *self, PyObject *args) {
    int estMaxExportSessions;

    if (!PyArg_ParseTuple(args, "i", &estMaxExportSessions)) {
        return NULL;
    }

    if (ltpInit(estMaxExportSessions) < 0) {
        PyErr_SetString(PyExc_RuntimeError, "Error initializing LTP.");
        return NULL;
    }

    Py_RETURN_NONE;
}

static PyObject *pyion_ltp_dequeue_outbound_segment(PyObject *self, PyObject *args) {
    unsigned long long vspan_addr;

    if (!PyArg_ParseTuple(args, "K", &vspan_addr)) {
        return NULL;
    }

    LtpVspan *vspan = (LtpVspan *)vspan_addr;
    char *segment;
    int segmentLen;

    // Release GIL
    PyGILState_STATE gstate = PyGILState_Ensure();
    Py_BEGIN_ALLOW_THREADS

    segmentLen = ltpDequeueOutboundSegment(vspan, &segment);
    if (segmentLen < 0)
    {
        PyErr_SetString(PyExc_RuntimeError, "Nonpositive LTP segment length.");
        return NULL;
    }

    // Reacquire GIL
    Py_END_ALLOW_THREADS
    PyGILState_Release(gstate);

    return PyBytes_FromStringAndSize(segment, segmentLen);
}

static PyObject *pyion_ltp_handle_inbound_segment(PyObject *self, PyObject *args) {
    char *buffer;
    Py_ssize_t segment_size;

    if (!PyArg_ParseTuple(args, "y#", &buffer, &segment_size)) {
        return NULL;
    }

    // Release GIL
    PyGILState_STATE gstate = PyGILState_Ensure();
    Py_BEGIN_ALLOW_THREADS

    if (ltpHandleInboundSegment(buffer, segment_size) < 0)
    {
        PyErr_SetString(PyExc_RuntimeError, "Unable to ingest inbound LTP segment.");
        return NULL;
    }

    // Reacquire GIL
    Py_END_ALLOW_THREADS
    PyGILState_Release(gstate);

    Py_RETURN_NONE;
}
