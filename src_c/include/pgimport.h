#ifndef PGIMPORT_H
#define PGIMPORT_H

/* Prefix when importing module */
#define IMPPREFIX "pygame."

#include "pgcompat.h"

#define PYGAMEAPI_LOCAL_ENTRY "_PYGAME_C_API"
#define PG_CAPSULE_NAME(m) (IMPPREFIX m "." PYGAMEAPI_LOCAL_ENTRY)

/*
 * fill API slots defined by PYGAMEAPI_DEFINE_SLOTS/PYGAMEAPI_EXTERN_SLOTS
 */
#define _IMPORT_PYGAME_MODULE(module)                                         \
    {                                                                         \
        PyObject *_mod_##module = PyImport_ImportModule(IMPPREFIX #module);   \
                                                                              \
        if (_mod_##module != NULL) {                                          \
            PyObject *_c_api =                                                \
                PyObject_GetAttrString(_mod_##module, PYGAMEAPI_LOCAL_ENTRY); \
                                                                              \
            Py_DECREF(_mod_##module);                                         \
            if (_c_api != NULL && PyCapsule_CheckExact(_c_api)) {             \
                void **localptr = (void **)PyCapsule_GetPointer(              \
                    _c_api, PG_CAPSULE_NAME(#module));                        \
                PYGAMEAPI_SET_SLOTS(module, localptr);                        \
            }                                                                 \
            Py_XDECREF(_c_api);                                               \
        }                                                                     \
    }

#define PYGAMEAPI_IS_IMPORTED(module) (PYGAMEAPI_SLOTS(module) != NULL)

/*
 * source file must include one of these in order to use _IMPORT_PYGAME_MODULE.
 * this is set by import_pygame_*() functions.
 * disable with NO_PYGAME_C_API
 */
#ifdef PG_PER_INTERPRETER_STATE
/*
 * An embedding runtime that runs this extension's init cycle once per
 * interpreter needs the cached table pointer to be per interpreter as well:
 * the file-scope variable below is written by whichever interpreter imported
 * last, and it keeps addressing that interpreter's export table after the
 * interpreter is gone. Only the storage moves -- every use is still spelled
 * PYGAMEAPI_GET_SLOT / PYGAMEAPI_IS_IMPORTED, and with the define off the
 * declarations below are the upstream ones unchanged.
 *
 * The write is guarded so a null can never replace a good table. One slot
 * serves every consumer in an interpreter (see pgcontext.h), and the value it
 * holds only ever comes from that interpreter's own exporting module, so
 * caching a null over it would take a working table away from consumers that
 * had already resolved it. Callers detect the failure from the exception the
 * import path sets, which is how they detect it upstream too.
 */
#include "../pgcontext.h"

#define PYGAMEAPI_DEFINE_SLOTS(module)
#define PYGAMEAPI_EXTERN_SLOTS(module)
#define PYGAMEAPI_SLOTS(module) \
    (*pg_context_slots("pygame_slots_" #module, __FILE_NAME__, __LINE__))
#define PYGAMEAPI_SET_SLOTS(module, ptr) \
    if (ptr != NULL)                     \
    PYGAMEAPI_SLOTS(module) = ptr
#else /* ~PG_PER_INTERPRETER_STATE */
#define PYGAMEAPI_DEFINE_SLOTS(module) void **_PGSLOTS_##module = NULL
#define PYGAMEAPI_EXTERN_SLOTS(module) extern void **_PGSLOTS_##module
#define PYGAMEAPI_SLOTS(module) _PGSLOTS_##module
#define PYGAMEAPI_SET_SLOTS(module, ptr) PYGAMEAPI_SLOTS(module) = ptr
#endif /* ~PG_PER_INTERPRETER_STATE */

#define PYGAMEAPI_GET_SLOT(module, index) PYGAMEAPI_SLOTS(module)[(index)]

/*
 * disabled API with NO_PYGAME_C_API; do nothing instead
 */
#ifdef NO_PYGAME_C_API

#undef PYGAMEAPI_DEFINE_SLOTS
#undef PYGAMEAPI_EXTERN_SLOTS

#define PYGAMEAPI_DEFINE_SLOTS(module)
#define PYGAMEAPI_EXTERN_SLOTS(module)

/* PYGAMEAPI_GET_SLOT and PYGAMEAPI_IS_IMPORTED reach the cached table pointer
 * through this, so dropping it is what keeps the deliberate error below an
 * error however that pointer is stored. */
#undef PYGAMEAPI_SLOTS

/* intentionally leave this defined to cause a compiler error *
#define PYGAMEAPI_GET_SLOT(api_root, index)
#undef PYGAMEAPI_GET_SLOT*/

#undef _IMPORT_PYGAME_MODULE
#define _IMPORT_PYGAME_MODULE(module)

#endif /* NO_PYGAME_C_API */

#define encapsulate_api(ptr, module) \
    PyCapsule_New(ptr, PG_CAPSULE_NAME(module), NULL)

#endif /* ~PGIMPORT_H */
