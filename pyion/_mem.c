/* ============================================================================
 * Extension module to interface ION's memory management functions and Python.
 * It is based on ION's ``sdrwatch`` and ``psmwatch`` utilities.
 *
 * Author: Marc Sanchez Net
 * Date:   8/12/2019
 * Copyright (c) 2019, California Institute of Technology ("Caltech").
 * U.S. Government sponsorship acknowledged.
 * =========================================================================== */

#include <Python.h>
#include <pthread.h>

#include "_utils.c"
#include "base_mem.h"

/* ============================================================================
 * === _mem module definitions
 * ============================================================================ */

// Docstring for this module
static char module_docstring[] =
    "Extension module to interface ION's memory management functions and Python.";

// Docstring for the functions being wrapped
static char sdr_dump_docstring[] =
    "Dump the state of the SDR.";
static char psm_dump_docstring[] =
    "Dump the state of the PSM.";

// Declare the functions to wrap
static PyObject *pyion_sdr_dump(PyObject *self, PyObject *args);
static PyObject *pyion_psm_dump(PyObject *self, PyObject *args);

// Define member functions of this module
static PyMethodDef module_methods[] = {
    {"sdr_dump", pyion_sdr_dump, METH_VARARGS, sdr_dump_docstring},
    {"psm_dump", pyion_psm_dump, METH_VARARGS, psm_dump_docstring},
    {NULL, NULL, 0, NULL}
};

/* ============================================================================
 * === Thread-safety
 * ============================================================================
 * _mem only performs read-only usage queries. sdr_dump reads the SDR usage
 * summary inside an SDR transaction (ION-serialized), but psm_usage has no
 * transaction, and both touch process-global ION state. A single module lock
 * serializes the two entry points against each other so the module is safe to
 * run without the GIL. The lock is short-held -- only around the ION query,
 * not around the Python object building that follows it.
 */
static pthread_mutex_t mem_lock = PTHREAD_MUTEX_INITIALIZER;

/* ============================================================================
 * === Define _mem as a Python module (PEP 489 multi-phase init)
 * ============================================================================ */

static int _mem_exec(PyObject *module) {
    (void)module;
    return 0;
}

static PyModuleDef_Slot _mem_slots[] = {
    {Py_mod_exec, _mem_exec},
#ifdef Py_mod_gil
    // _mem's queries are serialized by mem_lock and (for the SDR) by an SDR
    // transaction; declare the module safe without the GIL. On pre-3.13 Python
    // Py_mod_gil is undefined and the module stays GIL-required.
    {Py_mod_gil, Py_MOD_GIL_NOT_USED},
#endif
    {0, NULL}};

static struct PyModuleDef moduledef = {
    PyModuleDef_HEAD_INIT,
    "_mem",
    module_docstring,
    0, // m_size: multi-phase init requires >= 0
    module_methods,
    _mem_slots,
    NULL,
    NULL,
    NULL};

PyMODINIT_FUNC PyInit__mem(void) {
    return PyModuleDef_Init(&moduledef);
}

/* ============================================================================
 * === SDR-related functions
 * ============================================================================ */

static PyObject *pyion_sdr_dump(PyObject *self, PyObject *args) {
    // Define variables
    int             i;
    Sdr		        sdr;
	SdrUsageSummary	sdrUsage;
    PyObject        *sp_blocks = PyDict_New();
    PyObject        *lp_blocks = PyDict_New();
    size_t          sp_blk_size = 0;
    size_t          lp_blk_size = WORD_SIZE;

    // Attach to SDR
    sdr = getIonsdr();
    if (sdr == NULL) {
        pyion_SetExc(PyExc_MemoryError, "Cannot attach to ION's SDR.");
        return NULL;
    }

    // Get the state of the SDR, serialized against other _mem calls. The
    // usage summary is copied into a local, so the lock is released before the
    // Python object building below.
    // NOTE: sdr_usage needs to be in a transaction even though it is a read.
    pthread_mutex_lock(&mem_lock);
    if (!sdr_pybegin_xn(sdr)) {
        pthread_mutex_unlock(&mem_lock);
        return NULL;
    }
    base_sdr_usage(sdr, &sdrUsage);
    sdr_pyexit_xn(sdr);
    pthread_mutex_unlock(&mem_lock);

    // Get amount of data available in small pool [bytes]
    size_t sp_avail = sdrUsage.smallPoolFree;
    size_t sp_used  = sdrUsage.smallPoolAllocated;
    size_t sp_total = sdrUsage.smallPoolSize;

    // Get amount of data available in large pool [bytes]
    size_t lp_avail = sdrUsage.largePoolFree;
    size_t lp_used  = sdrUsage.largePoolAllocated;
    size_t lp_total = sdrUsage.largePoolSize;

    // Head data, all in [bytes]
    size_t hp_size  = sdrUsage.heapSize;
    size_t hp_avail = sdrUsage.unusedSize;

    // Get the amount of blocks available in the small pool
    for (i = 0; i < SMALL_SIZES; i++) {
        sp_blk_size  += WORD_SIZE;
        PyObject *key = Py_BuildValue("k", (unsigned long)sp_blk_size);
        PyObject *val = Py_BuildValue("k", (unsigned long)sdrUsage.smallPoolFreeBlockCount[i]);
        PyDict_SetItem(sp_blocks, key, val);
    }

    // Get the amount of blocks available in the large pool
    for (i = 0; i < LARGE_ORDERS; i++) {
        lp_blk_size  *= 2;
        PyObject *key = Py_BuildValue("k", (unsigned long)lp_blk_size);
        PyObject *val = Py_BuildValue("k", (unsigned long)sdrUsage.largePoolFreeBlockCount[i]);
        PyDict_SetItem(lp_blocks, key, val);
    }

    // Return summary statistics in a dictionary
    PyObject *ret = Py_BuildValue("({s:k, s:k, s:k, s:k, s:k, s:k, s:k, s:k}, O, O)",
                         "small_pool_avail",  (unsigned long)sp_avail,
                         "small_pool_used",   (unsigned long)sp_used,
                         "small_pool_total",  (unsigned long)sp_total,
                         "large_pool_avail",  (unsigned long)lp_avail,
                         "large_pool_used",   (unsigned long)lp_used,
                         "large_pool_total",  (unsigned long)lp_total,
                         "heap_size",         (unsigned long)hp_size,
                         "heap_avail",        (unsigned long)hp_avail,
                         sp_blocks, lp_blocks );

    return ret;
}

/* ============================================================================
 * === PSM-related functions
 * ============================================================================ */

static PyObject *pyion_psm_dump(PyObject *self, PyObject *args) {
    // Define variables
    int             i;
    PsmPartition    psm;
    PsmUsageSummary	psmUsage;
    PyObject        *sp_blocks = PyDict_New();
    PyObject        *lp_blocks = PyDict_New();
    size_t          sp_blk_size = 0;
    size_t          lp_blk_size = WORD_SIZE;

    // Attach to PSM
    psm = getIonwm();
    if (psm == NULL) {
        pyion_SetExc(PyExc_MemoryError, "Cannot attach to ION's PSM.");
        return NULL;
    }

    // Get the state of the PSM, serialized against other _mem calls. psm_usage
    // has no transaction of its own; the summary is copied into a local so the
    // lock is released before the Python object building below.
    pthread_mutex_lock(&mem_lock);
    base_psm_usage(psm, &psmUsage);
    pthread_mutex_unlock(&mem_lock);

    // Get amount of data available in small pool [bytes]
    size_t sp_avail = psmUsage.smallPoolFree;
    size_t sp_used  = psmUsage.smallPoolAllocated;
    size_t sp_total = psmUsage.smallPoolSize;

    // Get amount of data available in large pool [bytes]
    size_t lp_avail = psmUsage.largePoolFree;
    size_t lp_used  = psmUsage.largePoolAllocated;
    size_t lp_total = psmUsage.largePoolSize;

    // Head data, all in [bytes]
    size_t wm_size  = psmUsage.partitionSize;
    size_t wm_avail = psmUsage.unusedSize;

    // Get the amount of blocks available in the small pool
    for (i = 0; i < SMALL_SIZES; i++) {
        sp_blk_size  += WORD_SIZE;
        PyObject *key = Py_BuildValue("k", (unsigned long)sp_blk_size);
        PyObject *val = Py_BuildValue("k", (unsigned long)psmUsage.smallPoolFreeBlockCount[i]);
        PyDict_SetItem(sp_blocks, key, val);
    }

    // Get the amount of blocks available in the large pool
    for (i = 0; i < LARGE_ORDERS; i++) {
        lp_blk_size  *= 2;
        PyObject *key = Py_BuildValue("k", (unsigned long)lp_blk_size);
        PyObject *val = Py_BuildValue("k", (unsigned long)psmUsage.largePoolFreeBlockCount[i]);
        PyDict_SetItem(lp_blocks, key, val);
    }

    // Return summary statistics in a dictionary
    return Py_BuildValue("({s:k, s:k, s:k, s:k, s:k, s:k, s:k, s:k}, O, O)",
                         "small_pool_avail",  (unsigned long)sp_avail,
                         "small_pool_used",   (unsigned long)sp_used,
                         "small_pool_total",  (unsigned long)sp_total,
                         "large_pool_avail",  (unsigned long)lp_avail,
                         "large_pool_used",   (unsigned long)lp_used,
                         "large_pool_total",  (unsigned long)lp_total,
                         "wm_size",           (unsigned long)wm_size,
                         "wm_avail",          (unsigned long)wm_avail,
                         sp_blocks, lp_blocks);
}
