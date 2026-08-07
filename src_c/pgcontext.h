/*
  pygame-ce - Python Game Library
  Copyright (C) 2000-2001  Pete Shinners

  This library is free software; you can redistribute it and/or
  modify it under the terms of the GNU Library General Public
  License as published by the Free Software Foundation; either
  version 2 of the License, or (at your option) any later version.

  This library is distributed in the hope that it will be useful,
  but WITHOUT ANY WARRANTY; without even the implied warranty of
  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
  Library General Public License for more details.

  You should have received a copy of the GNU Library General Public
  License along with this library; if not, write to the Free
  Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA

  Pete Shinners
  pete@shinners.org
*/

#ifndef PGCONTEXT_H
#define PGCONTEXT_H

/*
 * Per-interpreter storage for module-level state.
 *
 * Compiled only when PG_PER_INTERPRETER_STATE is defined, which no ordinary
 * build defines (`meson setup -Dper_interpreter_state=true` is the switch).
 * It exists for embedding runtimes that create several interpreters in one
 * process and run this extension's init cycle once per interpreter. A
 * file-scope static is process-wide, so in that configuration the second init
 * cycle overwrites the pointers the first one stored, and once the first
 * interpreter is finalized every later read of the shared static dereferences
 * an object that no longer exists.
 *
 * PG_CONTEXT_PTR(type, name) declares the storage for one such pointer and
 * PG_CONTEXT_VAR(name) reads and writes it, so call sites keep spelling the
 * name exactly as upstream does. Each declaration is paired with an #else
 * branch that keeps the upstream static verbatim, so a build without the
 * define is byte-for-byte the upstream one.
 *
 * This is the right tool only for a pointer some *other* owner keeps alive --
 * a module attribute, sys.modules, an exception the module dict holds. When
 * the module itself is the owner, real module state (a PyModuleDef with
 * m_size, plus m_traverse/m_clear/m_free) is strictly better and should be
 * preferred: it is per-interpreter for the same reason, it releases what it
 * holds while the interpreter is still alive, it carries flags and scalars
 * alongside the pointers they gate, and nothing in Python can reach it.
 * scrap.c is the worked example.
 *
 * Ownership is NOT moved into the slot. The slot stores the pointer; whoever
 * owned the reference upstream still owns it and still drops it in the same
 * place (the note above each declaration records where). The release hook
 * below therefore frees the C wrapper and nothing else: it runs after the
 * owning interpreter has been finalized, when every object that interpreter
 * created is already gone, so a Py_XDECREF from here would walk freed memory.
 *
 * Allocation failure in the create hook surfaces as a null slot pointer and
 * hence a null dereference at the call site. That is deliberate: the
 * alternative is a failure check at every one of the ~30 call sites for a
 * condition (out of memory while allocating one pointer, during module
 * init) that has no recovery path.
 */

#ifdef PG_PER_INTERPRETER_STATE

#if defined(BUILD_STATIC)
#error \
    "PG_PER_INTERPRETER_STATE cannot be combined with BUILD_STATIC: the \
static build gives these names external linkage and resolves them from other \
translation units, which an accessor macro cannot provide."
#endif

#include <stdlib.h>

/*
 * Supplied by the embedding runtime on its include path, never vendored
 * here: this is the contract the embedder implements, typically as a thin
 * alias onto its own per-interpreter storage API. The header must provide,
 * as a function or function-like macro:
 *
 *     void *pg_host_get_context_slot(
 *         const char *key,             -- unique name for the stored pointer
 *         const char *file, int line,  -- call-site provenance, diagnostics
 *         void *(*create)(void),       -- runs on the key's first use in an
 *                                         interpreter; result becomes the
 *                                         interpreter's slot
 *         void (*destroy)(void *),     -- runs on the slot after its owning
 *                                         interpreter is finalized
 *         void (*report)(void *));     -- optional inspection hook; this
 *                                         extension always passes NULL
 *
 * It returns the calling interpreter's slot for `key`, stable for that
 * interpreter's lifetime. A slot is identified by the TRIPLE (key, file,
 * line), not by the key alone, so each key must be reached from exactly
 * one source location -- which is what the wrapper generated below does.
 * Expanding this call at each use site instead would create one slot per
 * use site, and reads would not find what the writer stored.
 */
#include "pgcontext_host.h"

#define PG_CONTEXT_PTR(type, name)                                   \
    static void *pg_context_create_##name(void)                      \
    {                                                                \
        type *slot = (type *)malloc(sizeof(type));                   \
        if (slot) {                                                  \
            *slot = NULL;                                            \
        }                                                            \
        return slot;                                                 \
    }                                                                \
                                                                     \
    static void pg_context_free_##name(void *ptr)                    \
    {                                                                \
        /* The C wrapper only. Never release the object the slot     \
         * pointed at from here -- see the ownership note above. */  \
        free(ptr);                                                   \
    }                                                                \
                                                                     \
    static type *pg_context_slot_##name(void)                        \
    {                                                                \
        return (type *)pg_host_get_context_slot(                     \
            "pygame_" #name, __FILE_NAME__, __LINE__,                \
            pg_context_create_##name, pg_context_free_##name, NULL); \
    }

#define PG_CONTEXT_VAR(name) (*pg_context_slot_##name())

/*
 * The cross-module C API needs two more shapes, and they are two halves of one
 * thing, so they are declared together.
 *
 * The exporting side is PG_CONTEXT_API_TABLE. Upstream keeps a module's slot
 * table in one file-scope array and hands its address to a capsule that the
 * module publishes as an attribute. With several interpreters every init cycle
 * writes that same array, so the capsule each interpreter holds addresses the
 * same memory and the last cycle to run decides what all of them read. Per
 * interpreter, the table an init cycle fills is the table the capsule it
 * publishes points at, and an interpreter that goes away takes only its own.
 *
 * The importing side is pg_context_slots(). A consumer reads the capsule once
 * and caches the table pointer; upstream caches it in a file-scope variable,
 * which puts the shared table back one indirection further out. Converting one
 * half without the other buys nothing -- it only moves which pointer goes
 * stale -- so both are gated on the same define and land together.
 *
 * Neither slot owns anything Python. A table holds function pointers plus a
 * few object pointers whose owners are named where the exporting module fills
 * them in, and a cached table pointer is a plain address. So both release
 * hooks free their own allocation and nothing else, for the reason given
 * above: they run once the owning interpreter is already finalized.
 *
 * The key for a table is the exporting module's name, and so is the key for
 * the cached pointer. That makes one cached pointer per exporting module per
 * interpreter rather than one per consumer, which is what the value already
 * is: in a given interpreter there is exactly one export table per exporting
 * module, so every consumer that resolves it arrives at the same address.
 *
 * The importing helpers are static inline because they sit in a header that
 * most translation units include without naming every module; a plain static
 * would be an unused-function warning wherever a name goes unused.
 *
 * Allocation failure behaves as it does above: a null slot and a null
 * dereference at the call site.
 */

#define PG_CONTEXT_API_TABLE(module, nslots)                                \
    static void *pg_context_create_capi_##module(void)                      \
    {                                                                       \
        return calloc((size_t)(nslots), sizeof(void *));                    \
    }                                                                       \
                                                                            \
    static void pg_context_free_capi_##module(void *ptr)                    \
    {                                                                       \
        /* The table only. Never release anything it pointed at from        \
         * here -- see the ownership note above. */                         \
        free(ptr);                                                          \
    }                                                                       \
                                                                            \
    static void **pg_context_api_table_##module(void)                       \
    {                                                                       \
        return (void **)pg_host_get_context_slot(                           \
            "pygame_capi_" #module, __FILE_NAME__, __LINE__,                \
            pg_context_create_capi_##module, pg_context_free_capi_##module, \
            NULL);                                                          \
    }

#define PG_CONTEXT_API_TABLE_VAR(module) pg_context_api_table_##module()

static inline void *
pg_context_create_slots(void)
{
    void ***slot = (void ***)malloc(sizeof(void **));
    if (slot) {
        *slot = NULL;
    }
    return slot;
}

static inline void
pg_context_free_slots(void *ptr)
{
    /* The one-pointer wrapper only. The table it addressed belongs to the
     * exporting module of the same interpreter and is released by that
     * module's own hook -- see the ownership note above. */
    free(ptr);
}

static inline void ***
pg_context_slots(const char *key, const char *file, int line)
{
    return (void ***)pg_host_get_context_slot(
        key, file, line, pg_context_create_slots, pg_context_free_slots, NULL);
}

#endif /* PG_PER_INTERPRETER_STATE */

#endif /* PGCONTEXT_H */
