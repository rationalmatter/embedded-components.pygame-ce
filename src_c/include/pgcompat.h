#if !defined(PGCOMPAT_H)
#define PGCOMPAT_H

#include <Python.h>

/* provides PyThreadState_GetUnchecked() on versions older than 3.13 */
#include "pythoncapi_compat.h"

/* In CPython, Py_Exit finalises the python interpreter before calling C exit()
 * This does not exist on PyPy, so use exit() directly here */
#ifdef PYPY_VERSION
#define PG_EXIT(n) exit(n)
#else
#define PG_EXIT(n) Py_Exit(n)
#endif

/* Helper for the SDL callbacks that need the GIL. Such a callback can be
 * reached two ways: from an SDL-owned thread, where no thread state is current
 * and the GIL genuinely has to be acquired, or from a python thread that
 * already holds it. A thread state being current implies the GIL is held;
 * PyGILState_Ensure on such a thread can mint a second thread state against
 * the wrong interpreter in embedded runtimes, so take the gilstate path only
 * when there is no current thread state. Behaviour on a thread without one --
 * the SDL threads these callbacks were written for -- is unchanged. */
typedef struct {
    PyGILState_STATE gstate;
    int ensured;
} pgGILState;

static inline pgGILState
pg_gil_ensure(void)
{
    pgGILState state = {PyGILState_UNLOCKED, 0};

    if (PyThreadState_GetUnchecked() == NULL) {
        state.gstate = PyGILState_Ensure();
        state.ensured = 1;
    }
    return state;
}

static inline void
pg_gil_release(pgGILState state)
{
    if (state.ensured) {
        PyGILState_Release(state.gstate);
    }
}

/* define common types where SDL is not included */
#ifndef SDL_VERSION_ATLEAST
#ifdef _MSC_VER
typedef unsigned __int8 uint8_t;
typedef unsigned __int32 uint32_t;
#else
#include <stdint.h>
#endif
typedef uint32_t Uint32;
typedef uint8_t Uint8;
#endif /* no SDL */

/* SDL_VERSION_ATLEAST is in every supported SDL version, but the code gets a
 * warning without this check here, which is very weird. */
#ifdef SDL_VERSION_ATLEAST

// SDL does not provide endian independent names for 32 bit formats without
// alpha channels the way they do for ones with alpha channels.
// E.g. SDL_PIXELFORMAT_RGBA32. This macro allows us the convenience of the
// endian independent name.

#if SDL_BYTEORDER == SDL_LIL_ENDIAN
#define PG_PIXELFORMAT_RGBX32 SDL_PIXELFORMAT_XBGR8888
#else
#define PG_PIXELFORMAT_RGBX32 SDL_PIXELFORMAT_RGBX8888
#endif

#if SDL_VERSION_ATLEAST(2, 0, 18)
#define PG_GetTicks SDL_GetTicks64
#else
#define PG_GetTicks SDL_GetTicks
#endif /* SDL_VERSION_ATLEAST(2, 0, 18) */

#endif /* defined(SDL_VERSION_ATLEAST) */

#ifndef SDL_MOUSEWHEEL_FLIPPED
#define NO_SDL_MOUSEWHEEL_FLIPPED
#endif

#endif /* ~defined(PGCOMPAT_H) */
