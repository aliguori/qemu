/*
===============================================================================

This C header file is part of the SoftFloat IEC/IEEE Floating-point
Arithmetic Package, Release 2a.

Written by John R. Hauser.  This work was made possible in part by the
International Computer Science Institute, located at Suite 600, 1947 Center
Street, Berkeley, California 94704.  Funding was partially provided by the
National Science Foundation under grant MIP-9311980.  The original version
of this code was written as part of a project to build a fixed-point vector
processor in collaboration with the University of California at Berkeley,
overseen by Profs. Nelson Morgan and John Wawrzynek.  More information
is available through the Web page `http://HTTP.CS.Berkeley.EDU/~jhauser/
arithmetic/SoftFloat.html'.

THIS SOFTWARE IS DISTRIBUTED AS IS, FOR FREE.  Although reasonable effort
has been made to avoid it, THIS SOFTWARE MAY CONTAIN FAULTS THAT WILL AT
TIMES RESULT IN INCORRECT BEHAVIOR.  USE OF THIS SOFTWARE IS RESTRICTED TO
PERSONS AND ORGANIZATIONS WHO CAN AND WILL TAKE FULL RESPONSIBILITY FOR ANY
AND ALL LOSSES, COSTS, OR OTHER PROBLEMS ARISING FROM ITS USE.

Derivative works are acceptable, even for commercial purposes, so long as
(1) they include prominent notice that the work is derivative, and (2) they
include prominent notice akin to these four paragraphs for those parts of
this code that are retained.

===============================================================================
*/

#ifndef SOFTFLOAT_H
#define SOFTFLOAT_H

#include <inttypes.h>
#include "config.h"

/*
----------------------------------------------------------------------------
Each of the following `typedef's defines the most convenient type that holds
integers of at least as many bits as specified.  For example, `uint8' should
be the most convenient type that can hold unsigned integers of as many as
8 bits.  The `flag' type must be able to hold either a 0 or 1.  For most
implementations of C, `flag', `uint8', and `int8' should all be `typedef'ed
to the same as `int'.
----------------------------------------------------------------------------
*/
typedef char flag;
typedef uint8_t uint8;
typedef int8_t int8;
typedef int uint16;
typedef int int16;
typedef unsigned int uint32;
typedef signed int int32;
typedef uint64_t uint64;
typedef int64_t int64;

/*
----------------------------------------------------------------------------
Each of the following `typedef's defines a type that holds integers
of _exactly_ the number of bits specified.  For instance, for most
implementation of C, `bits16' and `sbits16' should be `typedef'ed to
`unsigned short int' and `signed short int' (or `short int'), respectively.
----------------------------------------------------------------------------
*/
typedef uint8_t bits8;
typedef int8_t sbits8;
typedef uint16_t bits16;
typedef int16_t sbits16;
typedef uint32_t bits32;
typedef int32_t sbits32;
typedef uint64_t bits64;
typedef int64_t sbits64;

#define LIT64( a ) a##LL
#define INLINE static inline

/*
-------------------------------------------------------------------------------
The macro `FLOATX80' must be defined to enable the extended double-precision
floating-point format `floatx80'.  If this macro is not defined, the
`floatx80' type will not be defined, and none of the functions that either
input or output the `floatx80' type will be defined.  The same applies to
the `FLOAT128' macro and the quadruple-precision format `float128'.
-------------------------------------------------------------------------------
*/
#ifdef CONFIG_SOFTFLOAT
/* bit exact soft float support */
#define FLOATX80
#define FLOAT128
#else
/* native float support */
#if (defined(__i386__) || defined(__x86_64__)) && !defined(_BSD)
#define FLOATX80
#endif
#endif /* !CONFIG_SOFTFLOAT */

#define STATUS_PARAM , float_status *status
#define STATUS(field) status->field
#define STATUS_VAR , status

#ifdef CONFIG_SOFTFLOAT
/*
-------------------------------------------------------------------------------
Software IEC/IEEE floating-point types.
-------------------------------------------------------------------------------
*/
typedef uint32_t float32;
typedef uint64_t float64;
#ifdef FLOATX80
typedef struct {
    uint64_t low;
    uint16_t high;
} floatx80;
#endif
#ifdef FLOAT128
typedef struct {
#ifdef WORDS_BIGENDIAN
    uint64_t high, low;
#else
    uint64_t low, high;
#endif
} float128;
#endif

/*
-------------------------------------------------------------------------------
Software IEC/IEEE floating-point underflow tininess-detection mode.
-------------------------------------------------------------------------------
*/
enum {
    float_tininess_after_rounding  = 0,
    float_tininess_before_rounding = 1
};

/*
-------------------------------------------------------------------------------
Software IEC/IEEE floating-point rounding mode.
-------------------------------------------------------------------------------
*/
enum {
    float_round_nearest_even = 0,
    float_round_down         = 1,
    float_round_up           = 2,
    float_round_to_zero      = 3
};

/*
-------------------------------------------------------------------------------
Software IEC/IEEE floating-point exception flags.
-------------------------------------------------------------------------------
*/
enum {
    float_flag_invalid   =  1,
    float_flag_divbyzero =  4,
    float_flag_overflow  =  8,
    float_flag_underflow = 16,
    float_flag_inexact   = 32
};
typedef struct float_status {
    signed char float_detect_tininess;
    signed char float_rounding_mode;
    signed char float_exception_flags;
#ifdef FLOATX80
    signed char floatx80_rounding_precision;
#endif
} float_status;

void set_float_rounding_mode(int val STATUS_PARAM);
#ifdef FLOATX80
void set_floatx80_rounding_precision(int val STATUS_PARAM);
#endif

exception flags.
-------------------------------------------------------------------------------
*/
void float_raise( signed char STATUS_PARAM);

/*
-------------------------------------------------------------------------------
Software IEC/IEEE integer-to-floating-point conversion routines.
-------------------------------------------------------------------------------
*/
float32 int32_to_float32( int STATUS_PARAM );
float64 int32_to_float64( int STATUS_PARAM );
#ifdef FLOATX80
floatx80 int32_to_floatx80( int STATUS_PARAM );
#endif
#ifdef FLOAT128
float128 int32_to_float128( int STATUS_PARAM );
#endif
float32 int64_to_float32( int64_t STATUS_PARAM );
float64 int64_to_float64( int64_t STATUS_PARAM );
#ifdef FLOATX80
floatx80 int64_to_floatx80( int64_t STATUS_PARAM );
#endif
#ifdef FLOAT128
float128 int64_to_float128( int64_t STATUS_PARAM );
#endif

/*
-------------------------------------------------------------------------------
Software IEC/IEEE single-precision conversion routines.
-------------------------------------------------------------------------------
*/
int float32_to_int32( float32 STATUS_PARAM );
int float32_to_int32_round_to_zero( float32 STATUS_PARAM );
long long float32_to_int64( float32 STATUS_PARAM );
long long float32_to_int64_round_to_zero( float32 STATUS_PARAM );
float64 float32_to_float64( float32 STATUS_PARAM );
#ifdef FLOATX80
floatx80 float32_to_floatx80( float32 STATUS_PARAM );
#endif
#ifdef FLOAT128
float128 float32_to_float128( float32 STATUS_PARAM );
#endif

/*
-------------------------------------------------------------------------------
Software IEC/IEEE single-precision operations.
-------------------------------------------------------------------------------
*/
float32 float32_round_to_int( float32 STATUS_PARAM );
float32 float32_add( float32, float32 STATUS_PARAM );
float32 float32_sub( float32, float32 STATUS_PARAM );
float32 float32_mul( float32, float32 STATUS_PARAM );
float32 float32_div( float32, float32 STATUS_PARAM );
float32 float32_rem( float32, float32 STATUS_PARAM );
float32 float32_sqrt( float32 STATUS_PARAM );
char float32_eq( float32, float32 STATUS_PARAM );
char float32_le( float32, float32 STATUS_PARAM );
char float32_lt( float32, float32 STATUS_PARAM );
char float32_eq_signaling( float32, float32 STATUS_PARAM );
char float32_le_quiet( float32, float32 STATUS_PARAM );
char float32_lt_quiet( float32, float32 STATUS_PARAM );
char float32_is_signaling_nan( float32 );

/*
-------------------------------------------------------------------------------
Software IEC/IEEE double-precision conversion routines.
-------------------------------------------------------------------------------
*/
int float64_to_int32( float64 STATUS_PARAM );
int float64_to_int32_round_to_zero( float64 STATUS_PARAM );
int64_t float64_to_int64( float64 STATUS_PARAM );
int64_t float64_to_int64_round_to_zero( float64 STATUS_PARAM );
float32 float64_to_float32( float64 STATUS_PARAM );
#ifdef FLOATX80
floatx80 float64_to_floatx80( float64 STATUS_PARAM );
#endif
#ifdef FLOAT128
float128 float64_to_float128( float64 STATUS_PARAM );
#endif

/*
-------------------------------------------------------------------------------
Software IEC/IEEE double-precision operations.
-------------------------------------------------------------------------------
*/
float64 float64_round_to_int( float64 STATUS_PARAM );
float64 float64_add( float64, float64 STATUS_PARAM );
float64 float64_sub( float64, float64 STATUS_PARAM );
float64 float64_mul( float64, float64 STATUS_PARAM );
float64 float64_div( float64, float64 STATUS_PARAM );
float64 float64_rem( float64, float64 STATUS_PARAM );
float64 float64_sqrt( float64 STATUS_PARAM );
char float64_eq( float64, float64 STATUS_PARAM );
char float64_le( float64, float64 STATUS_PARAM );
char float64_lt( float64, float64 STATUS_PARAM );
char float64_eq_signaling( float64, float64 STATUS_PARAM );
char float64_le_quiet( float64, float64 STATUS_PARAM );
char float64_lt_quiet( float64, float64 STATUS_PARAM );
char float64_is_signaling_nan( float64 );

#ifdef FLOATX80

/*
-------------------------------------------------------------------------------
Software IEC/IEEE extended double-precision conversion routines.
-------------------------------------------------------------------------------
*/
int floatx80_to_int32( floatx80 STATUS_PARAM );
int floatx80_to_int32_round_to_zero( floatx80 STATUS_PARAM );
int64_t floatx80_to_int64( floatx80 STATUS_PARAM );
int64_t floatx80_to_int64_round_to_zero( floatx80 STATUS_PARAM );
float32 floatx80_to_float32( floatx80 STATUS_PARAM );
float64 floatx80_to_float64( floatx80 STATUS_PARAM );
#ifdef FLOAT128
float128 floatx80_to_float128( floatx80 STATUS_PARAM );
#endif

/*
-------------------------------------------------------------------------------
Software IEC/IEEE extended double-precision operations.
-------------------------------------------------------------------------------
*/
floatx80 floatx80_round_to_int( floatx80 STATUS_PARAM );
floatx80 floatx80_add( floatx80, floatx80 STATUS_PARAM );
floatx80 floatx80_sub( floatx80, floatx80 STATUS_PARAM );
floatx80 floatx80_mul( floatx80, floatx80 STATUS_PARAM );
floatx80 floatx80_div( floatx80, floatx80 STATUS_PARAM );
floatx80 floatx80_rem( floatx80, floatx80 STATUS_PARAM );
floatx80 floatx80_sqrt( floatx80 STATUS_PARAM );
char floatx80_eq( floatx80, floatx80 STATUS_PARAM );
char floatx80_le( floatx80, floatx80 STATUS_PARAM );
char floatx80_lt( floatx80, floatx80 STATUS_PARAM );
char floatx80_eq_signaling( floatx80, floatx80 STATUS_PARAM );
char floatx80_le_quiet( floatx80, floatx80 STATUS_PARAM );
char floatx80_lt_quiet( floatx80, floatx80 STATUS_PARAM );
char floatx80_is_signaling_nan( floatx80 );

#endif

#ifdef FLOAT128

/*
-------------------------------------------------------------------------------
Software IEC/IEEE quadruple-precision conversion routines.
-------------------------------------------------------------------------------
*/
int float128_to_int32( float128 STATUS_PARAM );
int float128_to_int32_round_to_zero( float128 STATUS_PARAM );
int64_t float128_to_int64( float128 STATUS_PARAM );
int64_t float128_to_int64_round_to_zero( float128 STATUS_PARAM );
float32 float128_to_float32( float128 STATUS_PARAM );
float64 float128_to_float64( float128 STATUS_PARAM );
#ifdef FLOATX80
floatx80 float128_to_floatx80( float128 STATUS_PARAM );
#endif

/*
-------------------------------------------------------------------------------
Software IEC/IEEE quadruple-precision operations.
-------------------------------------------------------------------------------
*/
float128 float128_round_to_int( float128 STATUS_PARAM );
float128 float128_add( float128, float128 STATUS_PARAM );
float128 float128_sub( float128, float128 STATUS_PARAM );
float128 float128_mul( float128, float128 STATUS_PARAM );
float128 float128_div( float128, float128 STATUS_PARAM );
float128 float128_rem( float128, float128 STATUS_PARAM );
float128 float128_sqrt( float128 STATUS_PARAM );
char float128_eq( float128, float128 STATUS_PARAM );
char float128_le( float128, float128 STATUS_PARAM );
char float128_lt( float128, float128 STATUS_PARAM );
char float128_eq_signaling( float128, float128 STATUS_PARAM );
char float128_le_quiet( float128, float128 STATUS_PARAM );
char float128_lt_quiet( float128, float128 STATUS_PARAM );
char float128_is_signaling_nan( float128 );

#endif

#else /* CONFIG_SOFTFLOAT */

#include "softfloat-native.h"

#endif /* !CONFIG_SOFTFLOAT */

#endif /* !SOFTFLOAT_H */
