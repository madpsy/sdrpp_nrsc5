// C99 <complex.h> for nrsc5 under clang-cl.
//
// Microsoft's <complex.h> has no `float complex`: it declares struct types
// (_Fcomplex) and functions taking them. clang supports C99 _Complex in any
// mode, so nrsc5 is compiled against this instead, which only needs the
// builtins and <math.h>. Complex multiply and divide compile to calls to
// __mulsc3/__divsc3, which the MSVC runtime does not have; src/win_complex_rt.c
// provides them.
#pragma once
#include <math.h>

#define complex _Complex
#define _Complex_I (__extension__ 1.0iF)
#undef I
#define I _Complex_I
#define CMPLXF(x, y) __builtin_complex((float)(x), (float)(y))

static __inline float crealf(float _Complex z) { return __real__ z; }
static __inline float cimagf(float _Complex z) { return __imag__ z; }
static __inline float _Complex conjf(float _Complex z) { return __builtin_complex(__real__ z, -__imag__ z); }
static __inline float cabsf(float _Complex z) { return hypotf(__real__ z, __imag__ z); }
static __inline float cargf(float _Complex z) { return atan2f(__imag__ z, __real__ z); }
static __inline float _Complex cexpf(float _Complex z) {
    float m = expf(__real__ z);
    return __builtin_complex(m * cosf(__imag__ z), m * sinf(__imag__ z));
}
