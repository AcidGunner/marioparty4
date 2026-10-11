/* Host replacement for include/dolphin/os/OSFastCast.h.
 * The original uses Gekko paired-single quantized load/store (psq_l/psq_st).
 * Gekko quantization converts float -> integer by truncating toward zero and
 * saturating to the destination range; int -> float is exact (scale 0). */
#ifndef _DOLPHIN_OSFASTCAST
#define _DOLPHIN_OSFASTCAST

#include <dolphin/types.h>

#ifdef __cplusplus
extern "C" {
#endif

#define OS_GQR_F32 0x0000
#define OS_GQR_U8 0x0004
#define OS_GQR_U16 0x0005
#define OS_GQR_S8 0x0006
#define OS_GQR_S16 0x0007

#define OS_FASTCAST_U8 2
#define OS_FASTCAST_U16 3
#define OS_FASTCAST_S8 4
#define OS_FASTCAST_S16 5

static inline void OSInitFastCast(void) {}

#define OS_FASTCAST_DEFINE(NAME, T, LO, HI)                       \
    static inline T __OSf32to##NAME(f32 v)                        \
    {                                                             \
        if (!(v == v)) return 0; /* NaN */                        \
        if (v <= (f32)(LO)) return (T)(LO);                       \
        if (v >= (f32)(HI)) return (T)(HI);                       \
        return (T)v; /* C cast truncates toward zero */           \
    }                                                             \
    static inline void OSf32to##NAME(f32 *f, T *out)              \
    {                                                             \
        *out = __OSf32to##NAME(*f);                               \
    }                                                             \
    static inline float __OS##NAME##tof32(const T *arg)           \
    {                                                             \
        return (float)*arg;                                       \
    }                                                             \
    static inline void OS##NAME##tof32(const T *in, float *out)   \
    {                                                             \
        *out = __OS##NAME##tof32(in);                             \
    }

OS_FASTCAST_DEFINE(s16, s16, -32768, 32767)
OS_FASTCAST_DEFINE(u8, u8, 0, 255)
OS_FASTCAST_DEFINE(s8, s8, -128, 127)
OS_FASTCAST_DEFINE(u16, u16, 0, 65535)

#undef OS_FASTCAST_DEFINE

#ifdef __cplusplus
}
#endif

#endif // _DOLPHIN_OSFASTCAST
