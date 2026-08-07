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
 * PG_CONTEXT_PTR(type, name) declares the storage for one such pointer,
 * PG_CONTEXT_INT(name) for a flag that gates per-interpreter state, and
 * PG_CONTEXT_VAR(name) reads and writes either, so call sites keep spelling
 * the name exactly as upstream does. Each declaration is paired with an #else
 * branch that keeps the upstream static verbatim, so a build without the
 * define is byte-for-byte the upstream one.
 *
 * A flag is worth converting only when what it gates is per-interpreter. One
 * that guards process-wide setup must stay process-wide, or that setup is
 * repeated; one that guards per-interpreter state must not, or every
 * interpreter after the first skips its own setup and is left with none.
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

#define PG_CONTEXT_STORAGE(type, name, empty)                        \
    static void *pg_context_create_##name(void)                      \
    {                                                                \
        type *slot = (type *)malloc(sizeof(type));                   \
        if (slot) {                                                  \
            *slot = empty;                                           \
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

#define PG_CONTEXT_PTR(type, name) PG_CONTEXT_STORAGE(type, name, NULL)
#define PG_CONTEXT_INT(name) PG_CONTEXT_STORAGE(int, name, 0)

#define PG_CONTEXT_VAR(name) (*pg_context_slot_##name())

#endif /* PG_PER_INTERPRETER_STATE */

#endif /* PGCONTEXT_H */
