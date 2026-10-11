/* Force-included (-include) when building for host / PS3 with gcc.
 * Papers over Metrowerks-isms without touching upstream sources. */
#ifndef PORT_COMPAT_H
#define PORT_COMPAT_H

/* The decomp ships its own include/math.h (static inline sinf/cosf/... wrappers). It is reachable through
 * quoted includes from files inside include/ (ext_math.h does `#include "math.h"`, which resolves next to
 * itself). On glibc its guard (_MATH_H) happens to equal glibc's, so it silently vanishes; newlib (PS3) uses
 * _MATH_H_, so both get included and clash. Load the real libm header first and claim the decomp's guard. */
#include <math.h>
#ifndef _MATH_H
#define _MATH_H
#endif

/* __declspec(weak) -> attribute */
#ifndef __MWERKS__
#define __declspec(x) __attribute__((x))
#endif

/* The PC GX API takes the array size: GXSetArray(attr, data, size, stride).
 * Game code uses the GameCube 3-arg form. Accept both; size is unknown (0) for
 * 3-arg callers, so a real backend must derive it (e.g. from max index drawn).
 * TODO(port): audit the ~160 call sites and pass real sizes where known. */
#define GX_ARRAY_SIZE_UNKNOWN 0
#define GX_SETARRAY_SEL_(a, b, c, d, NAME, ...) NAME
#define GXSetArray3_(a, d, s) GXSetArray(a, d, GX_ARRAY_SIZE_UNKNOWN, s)
#define GXSetArray(...) GX_SETARRAY_SEL_(__VA_ARGS__, GXSetArray, GXSetArray3_)(__VA_ARGS__)

#endif
