/* Force-included into every musl file by recomp/build.py (third_party/musl/README.md).
 *
 * musl's math compiles against the host's own headers. Every function gets a musl_ prefix,
 * so it can't mix with the host's libm and the compiler doesn't treat it as a builtin
 * (which could constant-fold it with the host's results). sqrt, sqrtf and floor, which musl
 * calls internally, are provided by runtime/src/hle_math.c as the exact AArch64 instructions
 * (FSQRT, FRINTM), as in musl's aarch64 build.
 */
#define _USE_MATH_DEFINES  /* M_PI_2 etc. from the host's math.h */

#define __cos musl___cos
#define __cosdf musl___cosdf
#define __expo2 musl___expo2
#define __rem_pio2 musl___rem_pio2
#define __rem_pio2_large musl___rem_pio2_large
#define __rem_pio2f musl___rem_pio2f
#define __sin musl___sin
#define __sindf musl___sindf
#define __tan musl___tan
#define __tandf musl___tandf
#define acos musl_acos
#define acosf musl_acosf
#define asin musl_asin
#define asinf musl_asinf
#define atan musl_atan
#define atan2 musl_atan2
#define atan2f musl_atan2f
#define atanf musl_atanf
#define cos musl_cos
#define cosf musl_cosf
#define cosh musl_cosh
#define exp musl_exp
#define exp2f musl_exp2f
#define expf musl_expf
#define expm1 musl_expm1
#define fmod musl_fmod
#define fmodf musl_fmodf
#define frexp musl_frexp
#define ldexp musl_ldexp
#define log musl_log
#define log10 musl_log10
#define log10f musl_log10f
#define logf musl_logf
#define modf musl_modf
#define modff musl_modff
#define pow musl_pow
#define powf musl_powf
#define scalbn musl_scalbn
#define scalbnf musl_scalbnf
#define sin musl_sin
#define sinf musl_sinf
#define sinh musl_sinh
#define tan musl_tan
#define tanf musl_tanf
#define tanh musl_tanh
#define sqrt musl_sqrt
#define sqrtf musl_sqrtf
#define floor musl_floor
