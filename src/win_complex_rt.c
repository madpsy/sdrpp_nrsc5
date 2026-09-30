// Complex float multiply and divide helpers, for Windows.
//
// clang compiles C99 `float complex` a * b and a / b to calls to __mulsc3 and
// __divsc3 (for a * b only when the inline result is NaN + NaN i), which on
// other platforms come from libgcc or compiler-rt. The MSVC runtime has
// neither, so they are provided here, following C11 Annex G (G.5.1): the
// straightforward formula, with the special cases for infinite and NaN
// operands that make, for instance, inf * finite come out infinite rather
// than NaN.
//
// As in libgcc, the arithmetic is done in double: the product of two floats
// is exact there, and no quotient of floats can overflow or underflow it, so
// the result is rounded once, to float, and needs no rescaling.
#include <math.h>

static double copysign1(double x, double s) { return copysign(isinf(x) ? 1.0 : 0.0, s); }

float _Complex __mulsc3(float fa, float fb, float fc, float fd) {
    double a = fa, b = fb, c = fc, d = fd;
    double ac = a * c, bd = b * d, ad = a * d, bc = b * c;
    double re = ac - bd, im = ad + bc;
    if (isnan(re) && isnan(im)) {
        int recalc = 0;
        if (isinf(a) || isinf(b)) {
            a = copysign1(a, a);
            b = copysign1(b, b);
            if (isnan(c)) { c = copysign(0.0, c); }
            if (isnan(d)) { d = copysign(0.0, d); }
            recalc = 1;
        }
        if (isinf(c) || isinf(d)) {
            c = copysign1(c, c);
            d = copysign1(d, d);
            if (isnan(a)) { a = copysign(0.0, a); }
            if (isnan(b)) { b = copysign(0.0, b); }
            recalc = 1;
        }
        if (!recalc && (isinf(ac) || isinf(bd) || isinf(ad) || isinf(bc))) {
            if (isnan(a)) { a = copysign(0.0, a); }
            if (isnan(b)) { b = copysign(0.0, b); }
            if (isnan(c)) { c = copysign(0.0, c); }
            if (isnan(d)) { d = copysign(0.0, d); }
            recalc = 1;
        }
        if (recalc) {
            re = INFINITY * (a * c - b * d);
            im = INFINITY * (a * d + b * c);
        }
    }
    return __builtin_complex((float)re, (float)im);
}

float _Complex __divsc3(float fa, float fb, float fc, float fd) {
    double a = fa, b = fb, c = fc, d = fd;
    double denom = c * c + d * d;
    double re = (a * c + b * d) / denom;
    double im = (b * c - a * d) / denom;
    if (isnan(re) && isnan(im)) {
        if (denom == 0.0 && (!isnan(a) || !isnan(b))) {
            re = copysign(INFINITY, c) * a;
            im = copysign(INFINITY, c) * b;
        }
        else if ((isinf(a) || isinf(b)) && isfinite(c) && isfinite(d)) {
            a = copysign1(a, a);
            b = copysign1(b, b);
            re = INFINITY * (a * c + b * d);
            im = INFINITY * (b * c - a * d);
        }
        else if ((isinf(c) || isinf(d)) && isfinite(a) && isfinite(b)) {
            c = copysign1(c, c);
            d = copysign1(d, d);
            re = 0.0 * (a * c + b * d);
            im = 0.0 * (b * c - a * d);
        }
    }
    return __builtin_complex((float)re, (float)im);
}
