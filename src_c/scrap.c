/*
    pygame-ce - Python Game Library
    Copyright (C) 2006, 2007 Rene Dudfield, Marcus von Appen

    Originally written and put in the public domain by Sam Lantinga.

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
*/

/* Handle clipboard text and data in arbitrary formats */
#include <limits.h>
#include <stdio.h>

#ifdef PG_SDL3
#include <SDL3/SDL.h>
#else
#include <SDL.h>
#include "SDL_syswm.h"
#endif

#include "pygame.h"

#include "pgcompat.h"

#include "doc/scrap_doc.h"

#include "scrap.h"

/**
 * Indicates, whether pygame.scrap was initialized or not.
 */
#ifdef PG_PER_INTERPRETER_STATE
/*
 * This module's own state, which is per-interpreter by construction: the
 * module def below declares m_size, so PyModule_Create allocates and zeroes
 * one copy of this struct for every module object it creates, and the
 * interpreter that created the module is the one that destroys it.
 *
 * The three names this replaces are all things an interpreter must own alone
 * -- the two dicts hold whatever put() was handed, so their contents are
 * objects of the interpreter that called it, and the flag says whether that
 * interpreter has created them. Neither the upstream file-scope statics nor
 * per-interpreter storage for them can be right: a static is shared, and a
 * slot is not an owner, because the release hook runs after the interpreter is
 * finalized, when a decref would walk freed memory.
 *
 * Nor can the module's own *attributes* be the owner, which is the trap this
 * replaces. An attribute is writable from Python: `del pygame.scrap._clipdata`
 * or a rebinding drops the dict while the init flag still reports the module
 * ready, and the next get()/put() reads freed memory -- a C-level
 * use-after-free reachable from ordinary user code. Module state carries no
 * name in the module dict, so nothing in Python can reach it, and m_clear /
 * m_free release it while the interpreter is still alive.
 *
 * The flag lives in the same struct for the reason it had to stop being
 * process-wide: it is what asserts "this interpreter created its dicts", and
 * one allocation makes that structural instead of an agreement between two
 * separately-scoped lifetimes.
 */
typedef struct {
    PyObject *clipdata;
    PyObject *selectiondata;
    int scrapinitialized;
} _ScrapState;

/* Completed at the bottom of the file, where scrap_builtins is in scope. */
static struct PyModuleDef _module;

/*
 * The state of a module that is not there: uninitialized, no dicts. The module
 * index is emptied part-way through interpreter finalization while destructors
 * can still run, so SCRAP_MODULE can come back null in that window, and
 * PyModule_GetState would dereference it. Answering with an empty state makes
 * every reader refuse, which is the truth at that point. Nothing writes it --
 * every write below goes through a module this file was handed -- so it stays
 * what it reads as.
 */
static _ScrapState _scrap_no_module_state;

#define SCRAP_MOD_STATE(mod) \
    ((mod) ? (_ScrapState *)PyModule_GetState(mod) : &_scrap_no_module_state)

/* Every entry point below is a module-level PyCFunction, so `self` is this
 * module and the whole state -- both dicts and the flag that gates them --
 * comes from one struct. SCRAP_MODULE is for the backends, which take no
 * module argument and only ever read; it is why MODINIT registers the module
 * in the per-interpreter module index. */
#define SCRAP_MODULE PyState_FindModule(&_module)
#define SCRAP_INITIALIZED(mod) (SCRAP_MOD_STATE(mod)->scrapinitialized)
#else
static int _scrapinitialized = 0;
#define SCRAP_MODULE NULL
#define SCRAP_INITIALIZED(mod) _scrapinitialized
#endif

/**
 * Currently active Clipboard object.
 */
static ScrapClipType _currentmode;
#ifdef PG_PER_INTERPRETER_STATE
#define SCRAP_SELECTIONDATA(mod) (SCRAP_MOD_STATE(mod)->selectiondata)
#define SCRAP_CLIPDATA(mod) (SCRAP_MOD_STATE(mod)->clipdata)
#else
static PyObject *_selectiondata = NULL;
static PyObject *_clipdata = NULL;
#define SCRAP_SELECTIONDATA(mod) _selectiondata
#define SCRAP_CLIPDATA(mod) _clipdata
#endif

/* Forward declarations. */
static PyObject *
_scrap_get_types(PyObject *self, PyObject *args);
static PyObject *
_scrap_contains(PyObject *self, PyObject *args);
static PyObject *
_scrap_get_scrap(PyObject *self, PyObject *args);
static PyObject *
_scrap_put_scrap(PyObject *self, PyObject *args);
static PyObject *
_scrap_lost_scrap(PyObject *self, PyObject *args);
static PyObject *
_scrap_set_mode(PyObject *self, PyObject *args);

static PyObject *
_scrap_get_text(PyObject *self, PyObject *args);
static PyObject *
_scrap_put_text(PyObject *self, PyObject *args);
static PyObject *
_scrap_has_text(PyObject *self, PyObject *args);

/* Determine what type of clipboard we are using */
#if !defined(__WIN32__)
#define SDL2_SCRAP
#include "scrap_sdl2.c"

#elif defined(__WIN32__)
#define WIN_SCRAP
#include "scrap_win.c"

#else
#error Unknown window manager for clipboard handling
#endif /* scrap type */

/**
 * \brief Indicates whether the scrap module is already initialized.
 *
 * \return 0 if the module is not initialized, 1, if it is.
 */
int
pygame_scrap_initialized(void)
{
    return SCRAP_INITIALIZED(SCRAP_MODULE);
}

/*
 * Initializes the pygame scrap module.
 */
static PyObject *
_scrap_init(PyObject *self, PyObject *args)
{
    VIDEO_INIT_CHECK();

    if (PyErr_WarnEx(PyExc_DeprecationWarning,
                     "pygame.scrap.init deprecated since 2.2.0", 1) == -1) {
        return NULL;
    }

    if (!PYGAME_SCRAP_INITIALIZED(self)) {
#ifdef PG_PER_INTERPRETER_STATE
        /* Same replace-the-outgoing-pair semantics as the branch below, with
         * the failures upstream ignores taken seriously: the flag is raised
         * once the backend is up, so leaving either field null would arm every
         * entry point against a null dict. Take the pair all or nothing and
         * return before the backend runs, which leaves the flag clear -- the
         * module stays uninitialized and the next call starts over. */
        _ScrapState *state = SCRAP_MOD_STATE(self);
        PyObject *clipdata = PyDict_New();
        PyObject *selectiondata;

        if (!clipdata) {
            return NULL;
        }
        selectiondata = PyDict_New();
        if (!selectiondata) {
            Py_DECREF(clipdata);
            return NULL;
        }
        Py_XSETREF(state->clipdata, clipdata);
        Py_XSETREF(state->selectiondata, selectiondata);
#else
        Py_XDECREF(_clipdata);
        Py_XDECREF(_selectiondata);
        _clipdata = PyDict_New();
        _selectiondata = PyDict_New();
#endif
    }

    /* In case we've got not video surface, we won't initialize
     * anything.
     * Here is old SDL1 code for future reference
     * if (!SDL_GetVideoSurface())
     *     return RAISE(pgExc_SDLError, "No display mode is set");
     */
    if (!pygame_scrap_init()) {
        return RAISE(pgExc_SDLError, SDL_GetError());
    }

#ifdef PG_PER_INTERPRETER_STATE
    /* The backends raise this flag themselves where it is process-wide, and
     * return success exactly when they do. Here it has to land on the same
     * module as the dicts it gates, and they have no handle on one. */
    SCRAP_INITIALIZED(self) = 1;
#endif

    Py_RETURN_NONE;
}

/*
 * Indicates whether the scrap module is currently initialized.
 *
 * Note: All platforms supported here.
 */
static PyObject *
_scrap_get_init(PyObject *self, PyObject *_null)
{
    if (PyErr_WarnEx(PyExc_DeprecationWarning,
                     "pygame.scrap.get_init deprecated since 2.2.0",
                     1) == -1) {
        return NULL;
    }

    return PyBool_FromLong(PYGAME_SCRAP_INITIALIZED(self));
}

/*
 * Gets the currently available types from the active clipboard.
 */
static PyObject *
_scrap_get_types(PyObject *self, PyObject *_null)
{
    int i = 0;
    char **types;
    char *type;
    PyObject *list;
    PyObject *tmp;

    if (PyErr_WarnEx(PyExc_DeprecationWarning,
                     "pygame.scrap.get_types deprecated since 2.2.0",
                     1) == -1) {
        return NULL;
    }

    PYGAME_SCRAP_INIT_CHECK(self);
    if (!pygame_scrap_lost()) {
        switch (_currentmode) {
            case SCRAP_SELECTION:
                return PyDict_Keys(SCRAP_SELECTIONDATA(self));
            case SCRAP_CLIPBOARD:
            default:
                return PyDict_Keys(SCRAP_CLIPDATA(self));
        }
    }

    list = PyList_New(0);
    types = pygame_scrap_get_types();
    if (!types) {
        return list;
    }
    while (types[i] != NULL) {
        type = types[i];
        tmp = PyUnicode_DecodeASCII(type, strlen(type), 0);
        if (!tmp) {
            Py_DECREF(list);
            return 0;
        }
        if (PyList_Append(list, tmp)) {
            Py_DECREF(list);
            Py_DECREF(tmp);
            return 0;
        }
        Py_DECREF(tmp);
        i++;
    }
    return list;
}

/*
 * Checks whether the active clipboard contains a certain type.
 */
static PyObject *
_scrap_contains(PyObject *self, PyObject *args)
{
    char *type = NULL;

    if (PyErr_WarnEx(PyExc_DeprecationWarning,
                     "pygame.scrap.contains deprecated since 2.2.0",
                     1) == -1) {
        return NULL;
    }

    if (!PyArg_ParseTuple(args, "s", &type)) {
        return NULL;
    }
    if (pygame_scrap_contains(type)) {
        Py_RETURN_TRUE;
    }
    Py_RETURN_FALSE;
}

/*
 * Gets the content for a certain type from the active clipboard.
 */
static PyObject *
_scrap_get_scrap(PyObject *self, PyObject *args)
{
    char *scrap = NULL;
    PyObject *retval;
    char *scrap_type;
    size_t count;

    if (PyErr_WarnEx(PyExc_DeprecationWarning,
                     "pygame.scrap.get deprecated since 2.2.0. Consider using"
                     " pygame.scrap.get_text instead.",
                     1) == -1) {
        return NULL;
    }

    PYGAME_SCRAP_INIT_CHECK(self);

    if (!PyArg_ParseTuple(args, "s", &scrap_type)) {
        return NULL;
    }

    if (!pygame_scrap_lost()) {
        /* Still own the clipboard. */
        PyObject *scrap_dict = NULL;
        PyObject *key = NULL;
        PyObject *val = NULL;

        switch (_currentmode) {
            case SCRAP_SELECTION:
                scrap_dict = SCRAP_SELECTIONDATA(self);
                break;

            case SCRAP_CLIPBOARD:
            default:
                scrap_dict = SCRAP_CLIPDATA(self);
                break;
        }

        key = PyUnicode_FromString(scrap_type);
        if (NULL == key) {
            return PyErr_Format(PyExc_ValueError,
                                "invalid scrap data type identifier (%s)",
                                scrap_type);
        }

        val = PyDict_GetItemWithError(scrap_dict, key);
        Py_DECREF(key);

        if (NULL == val) {
            if (PyErr_Occurred()) {
                return PyErr_Format(PyExc_SystemError,
                                    "pygame.scrap internal error (key=%s)",
                                    scrap_type);
            }

            Py_RETURN_NONE;
        }

        Py_INCREF(val);
        return val;
    }

    /* pygame_get_scrap() only returns NULL or !NULL, but won't set any
     * errors. */
    scrap = pygame_scrap_get(scrap_type, &count);
    if (!scrap) {
        Py_RETURN_NONE;
    }

    retval = PyBytes_FromStringAndSize(scrap, count);
#if defined(PYGAME_SCRAP_FREE_STRING)
    free(scrap);
#endif

    return retval;
}

/*
 * This will put a python string into the clipboard.
 */
static PyObject *
_scrap_put_scrap(PyObject *self, PyObject *args)
{
    Py_ssize_t scraplen;
    char *scrap = NULL;
    char *scrap_type;
    PyObject *tmp;
    static const char argfmt[] = "sy#";

    if (PyErr_WarnEx(PyExc_DeprecationWarning,
                     "pygame.scrap.put deprecated since 2.2.0. Consider using"
                     " pygame.scrap.put_text instead.",
                     1) == -1) {
        return NULL;
    }

    PYGAME_SCRAP_INIT_CHECK(self);

    if (!PyArg_ParseTuple(args, argfmt, &scrap_type, &scrap, &scraplen)) {
        return NULL;
    }

    /* Set it in the clipboard. */
    if (!pygame_scrap_put(scrap_type, scraplen, scrap)) {
        return RAISE(pgExc_SDLError,
                     "content could not be placed in clipboard.");
    }

    /* Add or replace the set value. */
    switch (_currentmode) {
        case SCRAP_SELECTION: {
            tmp = PyBytes_FromStringAndSize(scrap, scraplen);
            PyDict_SetItemString(SCRAP_SELECTIONDATA(self), scrap_type, tmp);
            Py_DECREF(tmp);
            break;
        }
        case SCRAP_CLIPBOARD:
        default: {
            tmp = PyBytes_FromStringAndSize(scrap, scraplen);
            PyDict_SetItemString(SCRAP_CLIPDATA(self), scrap_type, tmp);
            Py_DECREF(tmp);
            break;
        }
    }

    Py_RETURN_NONE;
}

/*
 * Checks whether the pygame window has lost the clipboard.
 */
static PyObject *
_scrap_lost_scrap(PyObject *self, PyObject *_null)
{
    PYGAME_SCRAP_INIT_CHECK(self);

    if (PyErr_WarnEx(PyExc_DeprecationWarning,
                     "pygame.scrap.lost deprecated since 2.2.0", 1) == -1) {
        return NULL;
    }

    if (pygame_scrap_lost()) {
        Py_RETURN_TRUE;
    }
    Py_RETURN_FALSE;
}

/*
 * Sets the clipboard mode. This only works for the X11 environment, which
 * diverses between mouse selections and the clipboard.
 */
static PyObject *
_scrap_set_mode(PyObject *self, PyObject *args)
{
    PYGAME_SCRAP_INIT_CHECK(self);

    if (PyErr_WarnEx(PyExc_DeprecationWarning,
                     "pygame.scrap.set_mode deprecated since 2.2.0",
                     1) == -1) {
        return NULL;
    }

    if (!PyArg_ParseTuple(args, "i", &_currentmode)) {
        return NULL;
    }

    if (_currentmode != SCRAP_CLIPBOARD && _currentmode != SCRAP_SELECTION) {
        return RAISE(PyExc_ValueError, "invalid clipboard mode");
    }

    /* Force the clipboard, if not in a X11 environment. */
    _currentmode = SCRAP_CLIPBOARD;
    Py_RETURN_NONE;
}

/**
 * @brief Fetches a python string from the SDL clipboard. If
 *        there is nothing in the clipboard, it will return empty
 *
 * @return PyObject*
 */
static PyObject *
_scrap_get_text(PyObject *self, PyObject *args)
{
    const SDL_bool hasText = SDL_HasClipboardText();

    char *text = SDL_GetClipboardText();

    // if SDL_GetClipboardText fails, it returns an empty string
    // hasText helps determine if an actual error occurred
    // vs just an empty string in the clipboard
    if (*text == '\0' && hasText == SDL_TRUE) {
        SDL_free(text);
        return RAISE(pgExc_SDLError, SDL_GetError());
    }

    PyObject *returnValue = PyUnicode_FromString(text);
    SDL_free(text);

    return returnValue;
}

/**
 * @brief Puts a python string into the SDL clipboard
 *
 * @param args A python string to be put into the clipboard
 *
 * @return PyObject*
 */
static PyObject *
_scrap_put_text(PyObject *self, PyObject *args)
{
    char *text;

    if (!PyArg_ParseTuple(args, "s", &text)) {
        return NULL;
    }

#if SDL_VERSION_ATLEAST(3, 0, 0)
    if (!SDL_SetClipboardText(text)) {
#else
    if (SDL_SetClipboardText(text)) {
#endif
        return RAISE(pgExc_SDLError, SDL_GetError());
    }

    Py_RETURN_NONE;
}

/**
 * @brief If the SDL clipboard has something in it, will return True.
 *          Else it returns False.
 *
 * @return PyObject*
 */
static PyObject *
_scrap_has_text(PyObject *self, PyObject *args)
{
    const SDL_bool hasText = SDL_HasClipboardText();

    if (hasText) {
        Py_RETURN_TRUE;
    }

    Py_RETURN_FALSE;
}

static PyMethodDef scrap_builtins[] = {
/*
 * Only initialise these functions for ones we know about.
 *
 * Note, the macosx stuff is done in sdlosx_main.m
 */
#if (defined(WIN_SCRAP) || defined(SDL2_SCRAP))

    {"init", _scrap_init, 1, DOC_SCRAP_INIT},
    {"get_init", _scrap_get_init, METH_NOARGS, DOC_SCRAP_GETINIT},
    {"contains", _scrap_contains, METH_VARARGS, DOC_SCRAP_CONTAINS},
    {"get", _scrap_get_scrap, METH_VARARGS, DOC_SCRAP_GET},
    {"get_types", _scrap_get_types, METH_NOARGS, DOC_SCRAP_GETTYPES},
    {"put", _scrap_put_scrap, METH_VARARGS, DOC_SCRAP_PUT},
    {"lost", _scrap_lost_scrap, METH_NOARGS, DOC_SCRAP_LOST},
    {"set_mode", _scrap_set_mode, METH_VARARGS, DOC_SCRAP_SETMODE},

#endif
    {"get_text", _scrap_get_text, METH_NOARGS, DOC_SCRAP_GETTEXT},
    {"has_text", _scrap_has_text, METH_NOARGS, DOC_SCRAP_HASTEXT},
    {"put_text", _scrap_put_text, METH_VARARGS, DOC_SCRAP_PUTTEXT},
    {NULL, NULL, 0, NULL}};

#ifdef PG_PER_INTERPRETER_STATE
static int
_scrap_traverse(PyObject *mod, visitproc visit, void *arg)
{
    _ScrapState *state = SCRAP_MOD_STATE(mod);

    Py_VISIT(state->clipdata);
    Py_VISIT(state->selectiondata);
    return 0;
}

/*
 * Releases what this interpreter's init cycle created. Unlike a
 * per-interpreter storage release hook, this runs while the interpreter is
 * still alive -- module clearing happens during its finalization, not after it
 * -- so Py_CLEAR is correct here, and this is the only place the two dicts are
 * released. Clearing the flag with them keeps "initialized" meaning "the dicts
 * exist", which is what every PYGAME_SCRAP_INIT_CHECK(self) is asserting.
 */
static int
_scrap_clear(PyObject *mod)
{
    _ScrapState *state = SCRAP_MOD_STATE(mod);

    Py_CLEAR(state->clipdata);
    Py_CLEAR(state->selectiondata);
    state->scrapinitialized = 0;
    return 0;
}

/* Module deallocation does not run m_clear, only m_free. */
static void
_scrap_free(void *mod)
{
    _scrap_clear((PyObject *)mod);
}

static struct PyModuleDef _module = {
    PyModuleDef_HEAD_INIT, "scrap",        DOC_SCRAP,
    sizeof(_ScrapState),   scrap_builtins, NULL,
    _scrap_traverse,       _scrap_clear,   _scrap_free};
#endif /* PG_PER_INTERPRETER_STATE */

MODINIT_DEFINE(scrap)
{
#ifdef PG_PER_INTERPRETER_STATE
    PyObject *module;
#else
    static struct PyModuleDef _module = {PyModuleDef_HEAD_INIT,
                                         "scrap",
                                         DOC_SCRAP,
                                         -1,
                                         scrap_builtins,
                                         NULL,
                                         NULL,
                                         NULL,
                                         NULL};
#endif

    /* imported needed apis; Do this first so if there is an error
       the module is not loaded.
    */
    import_pygame_base();
    if (PyErr_Occurred()) {
        return NULL;
    }

#ifdef PG_PER_INTERPRETER_STATE
    /* create the module */
    module = PyModule_Create(&_module);
    if (!module) {
        return NULL;
    }

#ifndef PYPY_VERSION
    /* Register in the per-interpreter module index so PyState_FindModule-based
       state lookup works in embedding runtimes that run module init per
       interpreter. The module object is new here, so this can never be the
       repeat add that PyState_AddModule treats as fatal. */
    if (PyState_AddModule(module, &_module) < 0) {
        Py_DECREF(module);
        return NULL;
    }
#endif /* PYPY_VERSION */

    return module;
#else
    /* create the module */
    return PyModule_Create(&_module);
#endif
}
