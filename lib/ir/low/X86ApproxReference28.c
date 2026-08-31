/* Copyright (c) 2015, Intel Corporation Redistribution and use in source and
   binary forms, with or without modification, are permitted provided that the
   following conditions are met:

  * Redistributions of source code must retain the above copyright notice, this
    list of conditions and the following disclaimer.
  * Redistributions in binary form must reproduce the above copyright notice,
    this list of conditions and the following disclaimer in the documentation
    and/or other materials provided with the distribution.
  * Neither the name of Intel Corporation nor the names of its contributors may
    be used to endorse or promote products derived from this software without
    specific prior written permission.

  THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
  AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
  IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE
  DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT OWNER OR CONTRIBUTORS BE LIABLE
  FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
  DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR
  SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER
  CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY,
  OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
  OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE. */

/*
    ****************************************************************************
    ****************************************************************************
    ** THIS FILE CONTAINS EMULATION ROUTINES FOR THE UNDERLYING ALGORITHMS OF:**
    **   VRCP28PD Approximation to the Reciprocal of Packed Double Precision  **
    **       Floating-Point Values with Less Than 2^-28 Relative Error        **
    **   VRCP28SD Approximation to the Reciprocal of Scalar Double Precision  **
    **       Floating-Point Value with Less Than 2^-28 Relative Error         **
    **   VRCP28PS Approximation to the Reciprocal of Packed Single Precision  **
    **       Floating-Point Values with Less Than 2^-28 Relative Error        **
    **   VRCP28SS Approximation to the Reciprocal of Scalar Single Precision  **
    **       Floating-Point Value with Less Than 2^-28 Relative Error         **
    **   VRSQRT28PD Approximation to the Reciprocal Square Root of Packed     **
    **       Double Precision Floating-Point Values with Less Than 2^-28      **
    **       Relative Error                                                   **
    **   VRSQRT28SD Approximation to the Reciprocal Square Root of Scalar     **
    **       Double Precision Floating-Point Value with Less Than 2^-28       **
    **       Relative Error                                                   **
    **   VRSQRT28PS Approximation to the Reciprocal Square Root of Packed     **
    **       Single Precision Floating-Point Values with Less Than 2^-28      **
    **       Relative Error                                                   **
    **   VRSQRT28SS Approximation to the Reciprocal Square Root of Scalar     **
    **       Single Precision Floating-Point Value with Less Than 2^-28       **
    **       Relative Error                                                   **
    **   VEXP2PD Approximation to the Exponential 2^x of Packed               **
    **       Double Precision Floating-Point Values with Less Than 2^-23      **
    **       Relative Error                                                   **
    **   VEXP2PS Approximation to the Exponential 2^x of Packed               **
    **       Single Precision Floating-Point Values with Less Than 2^-23      **
    **       Relative Error                                                   **
    **                                                                        **
    ** THE CORRESPONDING EMULATION ROUTINES (ONLY SCALAR VERSIONS) ARE:       **
    **   RCP28S - RECIPROCAL APPROXIMATION FOR Float32                        **
    **   RCP28D - RECIPROCAL APPROXIMATION FOR Float64                        **
    **   RSQRT28S - RECIPROCAL SQUARE ROOT APPROXIMATION FOR Float32          **
    **   RSQRT28D - RECIPROCAL SQUARE ROOT APPROXIMATION FOR Float64          **
    **   EXP2S - BASE-2 EXPONENTIAL APPROXIMATION FOR Float32                 **
    **   EXP2D - BASE-2 EXPONENTIAL APPROXIMATION FOR Float64                 **
    ****************************************************************************
    ****************************************************************************
*/

#include <math.h>
#include <stdint.h>

typedef union {
    unsigned int u;
    float f;
} type32;

typedef union {
    unsigned long long u;
    double f;
} type64;

#define FP32_EXP_POS                      23
#define FP32_SIGN_POS                     31
#define FP32_QNAN_BIT_POS                 22
#define FP32_EXP_BIAS                     127
#define FP32_NON_NORMAL_MASK              0xfe
#define FP32_SIGN_BIT                     0x80000000
#define FP32_EXP_MASK                     0x7f800000
#define FP32_MANT_MASK                    0x007fffff
#define FP32_HIDDEN_BIT                   0x00800000
#define FP32_QNAN_BIT                     0x00400000

#define FP32_MINUS_ZERO_AS_UINT32         0x80000000
#define FP32_PLUS_ZERO_AS_UINT32          0x00000000
#define FP32_PLUS_ONE_AS_UINT32           0x3f800000
#define FP32_MINUS_INF_AS_UINT32          0xff800000
#define FP32_PLUS_INF_AS_UINT32           0x7f800000
#define FP32_DEFAULT_NAN_AS_UINT32        0xffc00000

#define FP32_MAX_EXP                      0xff
#define FP32_UNBIASED_EXP(u)              ((int)(((u) & FP32_EXP_MASK) >> FP32_EXP_POS) - FP32_EXP_BIAS)
#define FP32_QUIETIZE_NAN(u)              ((u) |= FP32_QNAN_BIT)

#define FP64_EXP_POS                      52
#define FP64_SIGN_POS                     63
#define FP64_QNAN_BIT_POS                 51
#define FP64_EXP_BIAS                     1023
#define FP64_NON_NORMAL_MASK              0x7fe
#define FP64_SIGN_BIT                     0x8000000000000000ull
#define FP64_EXP_MASK                     0x7ff0000000000000ull
#define FP64_MANT_MASK                    0x000fffffffffffffull
#define FP64_HIDDEN_BIT                   0x0010000000000000ull
#define FP64_QNAN_BIT                     0x0008000000000000ull

#define FP64_MINUS_ZERO_AS_UINT64         0x8000000000000000ull
#define FP64_PLUS_ZERO_AS_UINT64          0x0000000000000000ull
#define FP64_PLUS_ONE_AS_UINT64           0x3ff0000000000000ull
#define FP64_PLUS_INF_AS_UINT64           0x7ff0000000000000ull
#define FP64_MINUS_INF_AS_UINT64          0xfff0000000000000ull
#define FP64_DEFAULT_NAN_AS_UINT64        0xfff8000000000000ull

#define FP64_MAX_EXP                      0x7ff
#define FP64_UNBIASED_EXP(u)              ((int)(((u) & FP64_EXP_MASK) >> FP64_EXP_POS) - FP64_EXP_BIAS)
#define FP64_QUIETIZE_NAN(u)              ((u) |= FP64_QNAN_BIT)

#define POS_CLASS(__c)                (0 + 2*(__c))
#define NEG_CLASS(__c)                (1 + 2*(__c))

typedef enum {
    inexactFlag   = 0x20,
    underflowFlag = 0x10,
    overflowFlag  =  0x8,
    divByZeroFlag =  0x4,
    denormalFlag  =  0x2,
    invalidFlag   =  0x1,
} statusFlags_t;

typedef enum {
        // base classes
        avx3Zero        = 0,
        avx3Denorm      = 1,
        avx3Normal      = 2,
        avx3Infinity    = 3,
        avx3Nan         = 4,
        // "signed" classes
        avx3PosZero     = POS_CLASS(avx3Zero),
        avx3NegZero     = NEG_CLASS(avx3Zero),
        avx3PosDenorm   = POS_CLASS(avx3Denorm),
        avx3NegDenorm   = NEG_CLASS(avx3Denorm),
        avx3PosNormal   = POS_CLASS(avx3Normal),
        avx3NegNormal   = NEG_CLASS(avx3Normal),
        avx3PosInfinity = POS_CLASS(avx3Infinity),
        avx3NegInfinity = NEG_CLASS(avx3Infinity),
        avx3SNan        = POS_CLASS(avx3Nan),
        avx3QNan        = NEG_CLASS(avx3Nan),
} classType_e;

#define RET_DST(_val_)       dst->u = (_val_); return flags
#define SET_Z_RET_DST(_val_) dst->u = (_val_); return (flags |= divByZeroFlag)
#define SET_V_RET_DST(_val_) dst->u = (_val_); return (flags |= invalidFlag)

// ============================================================================
// The next three tables are used by the Emulation() routine. A detailed description
// of the relationship between the coefficients and Emulation() can be found in the
// code preceding the routine.
//
// The following two macros are used to initialize the EMU constant tables.
// UINIT assumes that all of the entries are positive and SINIT assumes that
// the second coefficient is negative. The choices of S and K in the macros are
// descibed in the comments preceding the Emulation() routine
// ============================================================================

typedef const struct { long long int  a; int  b; int  c; } EmulationCoefficients_t;

#define UINIT(a,b,c) {  (0x ## a ## LL)       << (_N - _Na + _S),   \
                        (0x ## b)             << (_N - _Nb + _S),   \
                        (0x ## c)             << (_N - _Nc + _S) },

#define SINIT(a,b,c) {  (0x ## a ## LL)       << (_N - _Na + _S),   \
                      -((0x400000 - (0x ## b)) << (_N - _Nb + _S)), \
                        (0x ## c)             << (_N - _Nc + _S) },
#define _SA 33
#define _SB 20
#define _SC 14

#define _Na (_SA)
#define _Nb (_SB + _K + _M)
#define _Nc (_SC + 2*_K + _T)


// ============================================================================

#define _S	 3
#define _M	22
#define _K	 8
#define _T	20

#define _N _Na
#if (_Nb > _Na)
#   undef  _N
#   define _N _Nb
#endif
#if (_Nc > _N)
#   undef  _N
#   define _N _Nb
#endif

EmulationCoefficients_t _rcp28Table[] = {
    SINIT(1fffffff0, 300009, 3fa0) // i = 0
    SINIT(1fe01fdf1, 301fd9, 3ee4) // i = 1
    SINIT(1fc07f00f, 303f4b, 3e28) // i = 2
    SINIT(1fa11ca90, 305e5f, 3d73) // i = 3
    SINIT(1f81f81e9, 307d18, 3cbe) // i = 4
    SINIT(1f6310abb, 309b77, 3c0b) // i = 5
    SINIT(1f44659d5, 30b97d, 3b5b) // i = 6
    SINIT(1f25f6434, 30d72b, 3ab0) // i = 7
    SINIT(1f07c1efa, 30f483, 3a07) // i = 8
    SINIT(1ee9c7f77, 311186, 3961) // i = 9
    SINIT(1ecc07b22, 312e36, 38bb) // i = 10
    SINIT(1eae8079e, 314a93, 381a) // i = 11
    SINIT(1e9131ab1, 31669f, 377b) // i = 12
    SINIT(1e741aa4c, 31825c, 36da) // i = 13
    SINIT(1e573ac83, 319dc9, 3641) // i = 14
    SINIT(1e3a91790, 31b8e9, 35a8) // i = 15
    SINIT(1e1e1e1d4, 31d3bd, 350f) // i = 16
    SINIT(1e01e01d3, 31ee45, 347b) // i = 17
    SINIT(1de5d6e32, 320883, 33e8) // i = 18
    SINIT(1dca01dc0, 322277, 335b) // i = 19
    SINIT(1dae6075f, 323c24, 32cc) // i = 20
    SINIT(1d92f2225, 32558a, 323e) // i = 21
    SINIT(1d77b6543, 326ea9, 31b6) // i = 22
    SINIT(1d5cac7fc, 328784, 312c) // i = 23
    SINIT(1d41d41cb, 32a01a, 30a8) // i = 24
    SINIT(1d272ca33, 32b86e, 3022) // i = 25
    SINIT(1d0cb58ed, 32d07f, 2fa0) // i = 26
    SINIT(1cf26e5bb, 32e84f, 2f20) // i = 27
    SINIT(1cd856884, 32ffdf, 2ea1) // i = 28
    SINIT(1cbe6d959, 33172f, 2e25) // i = 29
    SINIT(1ca4b304b, 332e41, 2daa) // i = 30
    SINIT(1c8b265a4, 334516, 2d2d) // i = 31
    SINIT(1c71c71bc, 335bad, 2cb8) // i = 32
    SINIT(1c5894d06, 337209, 2c40) // i = 33
    SINIT(1c3f8f014, 338829, 2bcd) // i = 34
    SINIT(1c26b538b, 339e0f, 2b5b) // i = 35
    SINIT(1c0e07030, 33b3bc, 2ae8) // i = 36
    SINIT(1bf583ee1, 33c930, 2a78) // i = 37
    SINIT(1bdd2b891, 33de6c, 2a0a) // i = 38
    SINIT(1bc4fd64e, 33f371, 299d) // i = 39
    SINIT(1bacf9144, 34083f, 2933) // i = 40
    SINIT(1b951e2a8, 341cd8, 28c8) // i = 41
    SINIT(1b7d6c3d4, 34313c, 285e) // i = 42
    SINIT(1b65e2e36, 34456b, 27f8) // i = 43
    SINIT(1b4e81b46, 345967, 2792) // i = 44
    SINIT(1b37484a5, 346d30, 272d) // i = 45
    SINIT(1b20363fd, 3480c7, 26c9) // i = 46
    SINIT(1b094b314, 34942c, 2668) // i = 47
    SINIT(1af286bc2, 34a760, 2608) // i = 48
    SINIT(1adbe87f2, 34ba64, 25a8) // i = 49
    SINIT(1ac5701a7, 34cd38, 254b) // i = 50
    SINIT(1aaf1d2f0, 34dfdd, 24f0) // i = 51
    SINIT(1a98ef602, 34f254, 2493) // i = 52
    SINIT(1a82e650d, 35049d, 2439) // i = 53
    SINIT(1a6d01a66, 3516b9, 23df) // i = 54
    SINIT(1a574106d, 3528a8, 2388) // i = 55
    SINIT(1a41a419d, 353a6b, 2331) // i = 56
    SINIT(1a2c2a876, 354c03, 22d9) // i = 57
    SINIT(1a16d3f8f, 355d70, 2283) // i = 58
    SINIT(1a01a0198, 356eb2, 2230) // i = 59
    SINIT(19ec8e94c, 357fca, 21de) // i = 60
    SINIT(19d79f16f, 3590b9, 218d) // i = 61
    SINIT(19c2d14e6, 35a17f, 213d) // i = 62
    SINIT(19ae24e9f, 35b21d, 20ec) // i = 63
    SINIT(199999991, 35c293, 209e) // i = 64
    SINIT(19852f0d1, 35d2e2, 204f) // i = 65
    SINIT(1970e4f79, 35e30a, 2001) // i = 66
    SINIT(195cbb0b9, 35f30b, 1fb7) // i = 67
    SINIT(1948b0fc6, 3602e7, 1f6b) // i = 68
    SINIT(1934c67f3, 36129d, 1f22) // i = 69
    SINIT(1920fb497, 36222e, 1eda) // i = 70
    SINIT(190d4f119, 36319b, 1e91) // i = 71
    SINIT(18f9c18f6, 3640e3, 1e4c) // i = 72
    SINIT(18e6527a8, 365009, 1e02) // i = 73
    SINIT(18d3018cd, 365f0a, 1dc0) // i = 74
    SINIT(18bfce805, 366de9, 1d7b) // i = 75
    SINIT(18acb90ef, 367ca6, 1d37) // i = 76
    SINIT(1899c0f59, 368b41, 1cf3) // i = 77
    SINIT(1886e5f03, 3699ba, 1cb2) // i = 78
    SINIT(187427bc6, 36a812, 1c71) // i = 79
    SINIT(18618617f, 36b64a, 1c2e) // i = 80
    SINIT(184f00c21, 36c461, 1bee) // i = 81
    SINIT(183c977a6, 36d258, 1baf) // i = 82
    SINIT(182a4a017, 36e02f, 1b72) // i = 83
    SINIT(18181817c, 36ede8, 1b32) // i = 84
    SINIT(1806017ff, 36fb81, 1af7) // i = 85
    SINIT(17f405fcb, 3708fc, 1aba) // i = 86
    SINIT(17e22550f, 371659, 1a7e) // i = 87
    SINIT(17d05f411, 372398, 1a43) // i = 88
    SINIT(17beb391d, 3730b9, 1a0b) // i = 89
    SINIT(17ad22089, 373dbe, 19cf) // i = 90
    SINIT(179baa6b5, 374aa6, 1995) // i = 91
    SINIT(178a4c813, 375771, 195d) // i = 92
    SINIT(177908115, 376420, 1926) // i = 93
    SINIT(1767dce3e, 3770b3, 18f1) // i = 94
    SINIT(1756cac1b, 377d2b, 18ba) // i = 95
    SINIT(1745d1741, 378988, 1883) // i = 96
    SINIT(1734f0c4f, 3795ca, 184d) // i = 97
    SINIT(1724287ef, 37a1f1, 181a) // i = 98
    SINIT(1713786d5, 37adfe, 17e6) // i = 99
    SINIT(1702e05bc, 37b9f1, 17b3) // i = 100
    SINIT(16f26016a, 37c5ca, 1782) // i = 101
    SINIT(16e1f76af, 37d18a, 1750) // i = 102
    SINIT(16d1a6262, 37dd31, 171e) // i = 103
    SINIT(16c16c168, 37e8bf, 16ed) // i = 104
    SINIT(16b1490a7, 37f435, 16ba) // i = 105
    SINIT(16a13cd14, 37ff92, 168b) // i = 106
    SINIT(1691473a6, 380ad7, 165d) // i = 107
    SINIT(168168164, 381605, 162c) // i = 108
    SINIT(16719f35b, 38211b, 15fe) // i = 109
    SINIT(1661ec6a0, 382c1a, 15cf) // i = 110
    SINIT(16524f84e, 383702, 15a1) // i = 111
    SINIT(1642c858c, 3841d3, 1574) // i = 112
    SINIT(163356b85, 384c8d, 154a) // i = 113
    SINIT(1623fa76b, 385732, 151b) // i = 114
    SINIT(1614b3680, 3861c0, 14f1) // i = 115
    SINIT(160581600, 386c39, 14c5) // i = 116
    SINIT(15f66433e, 38769c, 149a) // i = 117
    SINIT(15e75bb8b, 3880e9, 1473) // i = 118
    SINIT(15d867c3c, 388b22, 1448) // i = 119
    SINIT(15c9882b6, 389546, 141e) // i = 120
    SINIT(15babcc63, 389f55, 13f6) // i = 121
    SINIT(15ac056ad, 38a950, 13cd) // i = 122
    SINIT(159d61f0d, 38b337, 13a4) // i = 123
    SINIT(158ed2307, 38bd09, 137e) // i = 124
    SINIT(158056011, 38c6c8, 1357) // i = 125
    SINIT(1571ed3c4, 38d073, 1331) // i = 126
    SINIT(156397ba7, 38da0b, 130a) // i = 127
    SINIT(155555551, 38e390, 12e4) // i = 128
    SINIT(154725e68, 38ed02, 12bd) // i = 129
    SINIT(15390948c, 38f661, 1298) // i = 130
    SINIT(152aff56a, 38ffad, 1275) // i = 131
    SINIT(151d07ead, 3908e7, 1251) // i = 132
    SINIT(150f22e0d, 39120f, 122d) // i = 133
    SINIT(15015014f, 391b25, 1208) // i = 134
    SINIT(14f38f62d, 392429, 11e5) // i = 135
    SINIT(14e5e0a6f, 392d1c, 11c0) // i = 136
    SINIT(14d843bee, 3935fc, 11a1) // i = 137
    SINIT(14cab886e, 393ecc, 117e) // i = 138
    SINIT(14bd3edd7, 39478b, 115a) // i = 139
    SINIT(14afd6a03, 395038, 113b) // i = 140
    SINIT(14a27fad6, 3958d5, 1119) // i = 141
    SINIT(149539e3c, 396161, 10f9) // i = 142
    SINIT(14880521e, 3969dd, 10d8) // i = 143
    SINIT(147ae1478, 397249, 10b6) // i = 144
    SINIT(146dce343, 397aa4, 1098) // i = 145
    SINIT(1460cbc7c, 3982f0, 1076) // i = 146
    SINIT(1453d9e2a, 398b2b, 105a) // i = 147
    SINIT(1446f8656, 399357, 103b) // i = 148
    SINIT(143a27308, 399b74, 101b) // i = 149
    SINIT(142d6625a, 39a381, 0ffe) // i = 150
    SINIT(1420b5265, 39ab7f, 0fe0) // i = 151
    SINIT(14141413d, 39b36f, 0fbf) // i = 152
    SINIT(140782d0e, 39bb4f, 0fa2) // i = 153
    SINIT(13fb013f7, 39c320, 0f87) // i = 154
    SINIT(13ee8f429, 39cae3, 0f69) // i = 155
    SINIT(13e22cbce, 39d297, 0f4e) // i = 156
    SINIT(13d5d9917, 39da3e, 0f2e) // i = 157
    SINIT(13c995a44, 39e1d6, 0f12) // i = 158
    SINIT(13bd60d8f, 39e95f, 0efa) // i = 159
    SINIT(13b13b13b, 39f0db, 0ede) // i = 160
    SINIT(13a524384, 39f84a, 0ec0) // i = 161
    SINIT(13991c2c1, 39ffaa, 0ea7) // i = 162
    SINIT(138d22d34, 3a06fd, 0e8d) // i = 163
    SINIT(138138136, 3a0e43, 0e71) // i = 164
    SINIT(13755bd19, 3a157c, 0e55) // i = 165
    SINIT(13698df3b, 3a1ca7, 0e3c) // i = 166
    SINIT(135dce5f6, 3a23c5, 0e24) // i = 167
    SINIT(13521cfaf, 3a2ad7, 0e08) // i = 168
    SINIT(134679acf, 3a31db, 0df1) // i = 169
    SINIT(133ae45b3, 3a38d3, 0dd9) // i = 170
    SINIT(132f5ced4, 3a3fbf, 0dbe) // i = 171
    SINIT(1323e34a1, 3a469e, 0da6) // i = 172
    SINIT(13187758c, 3a4d71, 0d8d) // i = 173
    SINIT(130d19010, 3a5438, 0d74) // i = 174
    SINIT(1301c82ac, 3a5af2, 0d5e) // i = 175
    SINIT(12f684bd8, 3a61a1, 0d45) // i = 176
    SINIT(12eb4ea1d, 3a6844, 0d2d) // i = 177
    SINIT(12e025c02, 3a6edb, 0d16) // i = 178
    SINIT(12d50a013, 3a7566, 0d01) // i = 179
    SINIT(12c9fb4d8, 3a7be6, 0cea) // i = 180
    SINIT(12bef98e3, 3a825b, 0cd2) // i = 181
    SINIT(12b404ad0, 3a88c4, 0cbc) // i = 182
    SINIT(12a91c92f, 3a8f22, 0ca6) // i = 183
    SINIT(129e4129d, 3a9575, 0c90) // i = 184
    SINIT(1293725ba, 3a9bbd, 0c7a) // i = 185
    SINIT(1288b0125, 3aa1fa, 0c65) // i = 186
    SINIT(127dfa38a, 3aa82c, 0c50) // i = 187
    SINIT(127350b86, 3aae54, 0c39) // i = 188
    SINIT(1268b37cb, 3ab471, 0c24) // i = 189
    SINIT(125e22705, 3aba83, 0c11) // i = 190
    SINIT(12539d7e7, 3ac08b, 0bfc) // i = 191
    SINIT(124924922, 3ac689, 0be6) // i = 192
    SINIT(123eb7972, 3acc7c, 0bd3) // i = 193
    SINIT(12345678c, 3ad265, 0bc0) // i = 194
    SINIT(122a01228, 3ad845, 0ba9) // i = 195
    SINIT(121fb780f, 3ade1a, 0b97) // i = 196
    SINIT(121579802, 3ae3e5, 0b85) // i = 197
    SINIT(120b470c3, 3ae9a7, 0b70) // i = 198
    SINIT(12012011e, 3aef5f, 0b5c) // i = 199
    SINIT(11f7047da, 3af50d, 0b4a) // i = 200
    SINIT(11ecf43c6, 3afab2, 0b36) // i = 201
    SINIT(11e2ef3b4, 3b004d, 0b24) // i = 202
    SINIT(11d8f5672, 3b05df, 0b11) // i = 203
    SINIT(11cf06ad8, 3b0b68, 0afd) // i = 204
    SINIT(11c522fbf, 3b10e7, 0aed) // i = 205
    SINIT(11bb4a402, 3b165d, 0adc) // i = 206
    SINIT(11b17c67c, 3b1bcb, 0ac7) // i = 207
    SINIT(11a7b9610, 3b212f, 0ab6) // i = 208
    SINIT(119e0119b, 3b268a, 0aa6) // i = 209
    SINIT(119453807, 3b2bdd, 0a93) // i = 210
    SINIT(118ab083b, 3b3126, 0a84) // i = 211
    SINIT(118118115, 3b3668, 0a70) // i = 212
    SINIT(11778a18f, 3b3ba0, 0a61) // i = 213
    SINIT(116e06892, 3b40d0, 0a50) // i = 214
    SINIT(11648d50e, 3b45f8, 0a3d) // i = 215
    SINIT(115b1e5f7, 3b4b17, 0a2d) // i = 216
    SINIT(1151b9a3f, 3b502e, 0a1c) // i = 217
    SINIT(11485f0df, 3b553c, 0a0f) // i = 218
    SINIT(113f0e8d2, 3b5a43, 09fc) // i = 219
    SINIT(1135c8112, 3b5f41, 09ee) // i = 220
    SINIT(112c8b89c, 3b6438, 09db) // i = 221
    SINIT(112358e74, 3b6926, 09cd) // i = 222
    SINIT(111a3019c, 3b6e0c, 09bf) // i = 223
    SINIT(111111111, 3b72eb, 09ae) // i = 224
    SINIT(1107fbbe0, 3b77c2, 099e) // i = 225
    SINIT(10fef010e, 3b7c91, 0990) // i = 226
    SINIT(10f5edfa9, 3b8159, 097f) // i = 227
    SINIT(10ecf56bb, 3b8619, 0970) // i = 228
    SINIT(10e406559, 3b8ad1, 0962) // i = 229
    SINIT(10db20a86, 3b8f82, 0954) // i = 230
    SINIT(10d244562, 3b942c, 0943) // i = 231
    SINIT(10c9714fa, 3b98ce, 0936) // i = 232
    SINIT(10c0a7866, 3b9d69, 0928) // i = 233
    SINIT(10b7e6ec1, 3ba1fd, 0918) // i = 234
    SINIT(10af2f722, 3ba689, 090d) // i = 235
    SINIT(10a6810a5, 3bab0f, 08fd) // i = 236
    SINIT(109ddba6b, 3baf8d, 08f1) // i = 237
    SINIT(10953f38f, 3bb405, 08e1) // i = 238
    SINIT(108cabb35, 3bb876, 08d2) // i = 239
    SINIT(108421085, 3bbcdf, 08c7) // i = 240
    SINIT(107b9f29d, 3bc142, 08b9) // i = 241
    SINIT(1073260a3, 3bc59e, 08ad) // i = 242
    SINIT(106ab59c6, 3bc9f4, 089d) // i = 243
    SINIT(10624dd2d, 3bce43, 088f) // i = 244
    SINIT(1059eea06, 3bd28b, 0883) // i = 245
    SINIT(105197f7b, 3bd6cd, 0875) // i = 246
    SINIT(104949cbf, 3bdb08, 0869) // i = 247
    SINIT(104104101, 3bdf3d, 085b) // i = 248
    SINIT(1038c6b78, 3be36b, 084f) // i = 249
    SINIT(103091b51, 3be793, 0843) // i = 250
    SINIT(102864fc6, 3bebb5, 0835) // i = 251
    SINIT(10204080f, 3befd0, 082b) // i = 252
    SINIT(101824364, 3bf3e5, 0820) // i = 253
    SINIT(1010100fe, 3bf7f5, 0810) // i = 254
    SINIT(10080401e, 3bfbfe, 0805) // i = 255
};

#undef _K
#undef _M
#undef _T
#undef _S
#undef _N

// =============================================================================

#define _S	 3
#define _M	22
#define _K	 8
#define _T	20

#define _N _Na
#if (_Nb > _Na)
#   undef  _N
#   define _N _Nb
#endif
#if (_Nc > _N)
#   undef  _N
#   define _N _Nb
#endif

EmulationCoefficients_t _rsqrt28Table[] = {
    SINIT(16a09e665, 3a57da, 10e5) // i = 0
    SINIT(1695568c3, 3a604c, 10ba) // i = 1
    SINIT(168a1f809, 3a68a9, 108f) // i = 2
    SINIT(167ef919c, 3a70f1, 1067) // i = 3
    SINIT(1673e32eb, 3a7925, 103d) // i = 4
    SINIT(1668dd972, 3a8144, 1018) // i = 5
    SINIT(165de82ad, 3a8950, 0ff0) // i = 6
    SINIT(165302c27, 3a9148, 0fcb) // i = 7
    SINIT(16482d379, 3a992d, 0fa4) // i = 8
    SINIT(163d67634, 3aa0ff, 0f7e) // i = 9
    SINIT(1632b1200, 3aa8be, 0f59) // i = 10
    SINIT(16280a483, 3ab06b, 0f33) // i = 11
    SINIT(161d72b76, 3ab805, 0f10) // i = 12
    SINIT(1612ea48e, 3abf8d, 0eed) // i = 13
    SINIT(160870d8f, 3ac703, 0ecb) // i = 14
    SINIT(15fe0643f, 3ace68, 0ea7) // i = 15
    SINIT(15f3aa672, 3ad5bb, 0e86) // i = 16
    SINIT(15e95d1fa, 3adcfe, 0e61) // i = 17
    SINIT(15df1e4bb, 3ae42f, 0e40) // i = 18
    SINIT(15d4edc9b, 3aeb4f, 0e21) // i = 19
    SINIT(15cacb77d, 3af25f, 0e01) // i = 20
    SINIT(15c0b735f, 3af95f, 0ddf) // i = 21
    SINIT(15b6b0e32, 3b004f, 0dbe) // i = 22
    SINIT(15acb8601, 3b072e, 0da1) // i = 23
    SINIT(15a2cd8c7, 3b0dfe, 0d82) // i = 24
    SINIT(1598f0492, 3b14bf, 0d62) // i = 25
    SINIT(158f2077e, 3b1b70, 0d44) // i = 26
    SINIT(15855df9a, 3b2212, 0d27) // i = 27
    SINIT(157ba8b0d, 3b28a5, 0d0a) // i = 28
    SINIT(1572007f6, 3b2f2a, 0cea) // i = 29
    SINIT(156865485, 3b359f, 0cd1) // i = 30
    SINIT(155ed6ee7, 3b3c07, 0cb2) // i = 31
    SINIT(155555552, 3b4260, 0c97) // i = 32
    SINIT(154be0605, 3b48ab, 0c7c) // i = 33
    SINIT(154277f3e, 3b4ee9, 0c5d) // i = 34
    SINIT(15391bf44, 3b5518, 0c45) // i = 35
    SINIT(152fcc467, 3b5b3a, 0c2a) // i = 36
    SINIT(152688cf2, 3b614f, 0c0e) // i = 37
    SINIT(151d5173e, 3b6756, 0bf6) // i = 38
    SINIT(1514261a7, 3b6d51, 0bd9) // i = 39
    SINIT(150b06a8f, 3b733e, 0bc1) // i = 40
    SINIT(1501f3054, 3b791f, 0ba7) // i = 41
    SINIT(14f8eb168, 3b7ef3, 0b8e) // i = 42
    SINIT(14efeec37, 3b84ba, 0b78) // i = 43
    SINIT(14e6fdf30, 3b8a76, 0b5d) // i = 44
    SINIT(14de188d3, 3b9025, 0b45) // i = 45
    SINIT(14d53e796, 3b95c8, 0b2e) // i = 46
    SINIT(14cc6f9ff, 3b9b5f, 0b17) // i = 47
    SINIT(14c3abe93, 3ba0ea, 0b01) // i = 48
    SINIT(14baf33d8, 3ba66a, 0ae9) // i = 49
    SINIT(14b24585c, 3babde, 0ad4) // i = 50
    SINIT(14a9a2ab6, 3bb147, 0abd) // i = 51
    SINIT(14a10a97b, 3bb6a5, 0aa5) // i = 52
    SINIT(14987d340, 3bbbf8, 0a8e) // i = 53
    SINIT(148ffa6ac, 3bc13f, 0a7b) // i = 54
    SINIT(14878225f, 3bc67c, 0a64) // i = 55
    SINIT(147f144fd, 3bcbae, 0a4f) // i = 56
    SINIT(1476b0d31, 3bd0d5, 0a3c) // i = 57
    SINIT(146e579aa, 3bd5f2, 0a27) // i = 58
    SINIT(14660891c, 3bdb05, 0a10) // i = 59
    SINIT(145dc3a3b, 3be00d, 09fd) // i = 60
    SINIT(145588bc1, 3be50b, 09e9) // i = 61
    SINIT(144d57c66, 3be9ff, 09d6) // i = 62
    SINIT(144530aef, 3beeea, 09bf) // i = 63
    SINIT(143d13621, 3bf3ca, 09ad) // i = 64
    SINIT(1434ffcc4, 3bf8a0, 099c) // i = 65
    SINIT(142cf5d9f, 3bfd6d, 0989) // i = 66
    SINIT(1424f5782, 3c0231, 0974) // i = 67
    SINIT(141cfe93d, 3c06eb, 0962) // i = 68
    SINIT(1415111a9, 3c0b9c, 094e) // i = 69
    SINIT(140d2cf99, 3c1043, 093f) // i = 70
    SINIT(1405521ea, 3c14e2, 092b) // i = 71
    SINIT(13fd8077c, 3c1977, 091b) // i = 72
    SINIT(13f5b7f30, 3c1e04, 0907) // i = 73
    SINIT(13edf87e6, 3c2288, 08f5) // i = 74
    SINIT(13e64208a, 3c2703, 08e4) // i = 75
    SINIT(13de94808, 3c2b75, 08d4) // i = 76
    SINIT(13d6efd45, 3c2fdf, 08c3) // i = 77
    SINIT(13cf53f39, 3c3440, 08b4) // i = 78
    SINIT(13c7c0cd8, 3c3899, 08a3) // i = 79
    SINIT(13c03650e, 3c3cea, 0892) // i = 80
    SINIT(13b8b46da, 3c4133, 0880) // i = 81
    SINIT(13b13b13b, 3c4573, 0872) // i = 82
    SINIT(13a9ca327, 3c49ac, 0860) // i = 83
    SINIT(13a261ba8, 3c4ddc, 0852) // i = 84
    SINIT(139b019b7, 3c5205, 0841) // i = 85
    SINIT(1393a9c5e, 3c5626, 0832) // i = 86
    SINIT(138c5a2ab, 3c5a3f, 0823) // i = 87
    SINIT(138512ba0, 3c5e51, 0812) // i = 88
    SINIT(137dd364f, 3c625b, 0804) // i = 89
    SINIT(13769c1cb, 3c665d, 07f7) // i = 90
    SINIT(136f6cd21, 3c6a58, 07e9) // i = 91
    SINIT(136845766, 3c6e4c, 07da) // i = 92
    SINIT(136125fb2, 3c7239, 07ca) // i = 93
    SINIT(135a0e51f, 3c761e, 07be) // i = 94
    SINIT(1352fe6ca, 3c79fd, 07ad) // i = 95
    SINIT(134bf63d0, 3c7dd4, 07a0) // i = 96
    SINIT(1344f5b50, 3c81a4, 0794) // i = 97
    SINIT(133dfcc6b, 3c856e, 0784) // i = 98
    SINIT(13370b64c, 3c8930, 0779) // i = 99
    SINIT(133021813, 3c8cec, 076b) // i = 100
    SINIT(13293f0ea, 3c90a1, 075f) // i = 101
    SINIT(132263ffe, 3c9450, 0750) // i = 102
    SINIT(131b9047c, 3c97f8, 0743) // i = 103
    SINIT(1314c3d90, 3c9b9a, 0734) // i = 104
    SINIT(130dfea70, 3c9f35, 0728) // i = 105
    SINIT(130740a50, 3ca2c9, 071e) // i = 106
    SINIT(130089c5c, 3ca658, 070f) // i = 107
    SINIT(12f9d9fd5, 3ca9e0, 0703) // i = 108
    SINIT(12f3313ed, 3cad62, 06f7) // i = 109
    SINIT(12ec8f7e3, 3cb0de, 06eb) // i = 110
    SINIT(12e5f4af7, 3cb453, 06e1) // i = 111
    SINIT(12df60c5f, 3cb7c3, 06d4) // i = 112
    SINIT(12d8d3b5e, 3cbb2d, 06c8) // i = 113
    SINIT(12d24d73a, 3cbe91, 06bc) // i = 114
    SINIT(12cbcdf37, 3cc1ef, 06b0) // i = 115
    SINIT(12c555298, 3cc547, 06a6) // i = 116
    SINIT(12bee30a4, 3cc89a, 0699) // i = 117
    SINIT(12b8778a7, 3ccbe7, 068d) // i = 118
    SINIT(12b2129ec, 3ccf2e, 0683) // i = 119
    SINIT(12abb43c2, 3cd26f, 067a) // i = 120
    SINIT(12a55c56d, 3cd5ac, 066c) // i = 121
    SINIT(129f0ae4c, 3cd8e2, 0664) // i = 122
    SINIT(1298bfda6, 3cdc13, 0659) // i = 123
    SINIT(12927b2ce, 3cdf3f, 064e) // i = 124
    SINIT(128c3cd1b, 3ce266, 0642) // i = 125
    SINIT(128604bea, 3ce587, 0638) // i = 126
    SINIT(127fd2e8a, 3ce8a3, 062e) // i = 127
    SINIT(1279a7458, 3cebba, 0623) // i = 128
    SINIT(127381caf, 3ceecc, 0618) // i = 129
    SINIT(126d626ef, 3cf1d8, 0610) // i = 130
    SINIT(12674926c, 3cf4e0, 0605) // i = 131
    SINIT(126135e93, 3cf7e2, 05fd) // i = 132
    SINIT(125b28ab8, 3cfae0, 05f2) // i = 133
    SINIT(125521645, 3cfdd9, 05e7) // i = 134
    SINIT(124f2009c, 3d00cd, 05dd) // i = 135
    SINIT(124924923, 3d03bc, 05d4) // i = 136
    SINIT(12432ef41, 3d06a6, 05cb) // i = 137
    SINIT(123d3f258, 3d098c, 05c1) // i = 138
    SINIT(1237551dd, 3d0c6c, 05ba) // i = 139
    SINIT(123170d2a, 3d0f49, 05af) // i = 140
    SINIT(122b923bc, 3d1220, 05a8) // i = 141
    SINIT(1225b94f6, 3d14f3, 059e) // i = 142
    SINIT(121fe6046, 3d17c2, 0593) // i = 143
    SINIT(121a1851e, 3d1a8c, 058b) // i = 144
    SINIT(1214502f6, 3d1d51, 0584) // i = 145
    SINIT(120e8d932, 3d2013, 0578) // i = 146
    SINIT(1208d0752, 3d22cf, 0573) // i = 147
    SINIT(120318cc5, 3d2588, 0568) // i = 148
    SINIT(11fd66903, 3d283c, 0560) // i = 149
    SINIT(11f7b9b82, 3d2aec, 0557) // i = 150
    SINIT(11f2123b8, 3d2d98, 054e) // i = 151
    SINIT(11ec70125, 3d303f, 0548) // i = 152
    SINIT(11e6d333f, 3d32e2, 0540) // i = 153
    SINIT(11e13b97c, 3d3582, 0535) // i = 154
    SINIT(11dba9363, 3d381d, 052e) // i = 155
    SINIT(11d61c070, 3d3ab4, 0526) // i = 156
    SINIT(11d094020, 3d3d47, 051e) // i = 157
    SINIT(11cb111f0, 3d3fd6, 0517) // i = 158
    SINIT(11c593566, 3d4261, 0510) // i = 159
    SINIT(11c01aa02, 3d44e9, 0505) // i = 160
    SINIT(11baa6f4a, 3d476c, 04ff) // i = 161
    SINIT(11b5384c3, 3d49eb, 04f9) // i = 162
    SINIT(11afce9e9, 3d4c67, 04f1) // i = 163
    SINIT(11aa69e4f, 3d4edf, 04e9) // i = 164
    SINIT(11a50a177, 3d5153, 04e2) // i = 165
    SINIT(119faf2e5, 3d53c4, 04d9) // i = 166
    SINIT(119a59229, 3d5631, 04d1) // i = 167
    SINIT(119507ece, 3d589a, 04ca) // i = 168
    SINIT(118fbb85b, 3d5aff, 04c5) // i = 169
    SINIT(118a73e61, 3d5d61, 04bd) // i = 170
    SINIT(11853106a, 3d5fbf, 04b7) // i = 171
    SINIT(117ff2e02, 3d621a, 04af) // i = 172
    SINIT(117ab96ba, 3d6471, 04aa) // i = 173
    SINIT(117584a26, 3d66c5, 04a2) // i = 174
    SINIT(1170547d3, 3d6916, 0498) // i = 175
    SINIT(116b28f54, 3d6b63, 0491) // i = 176
    SINIT(11660203c, 3d6dac, 048d) // i = 177
    SINIT(1160dfa23, 3d6ff2, 0486) // i = 178
    SINIT(115bc1c94, 3d7235, 0480) // i = 179
    SINIT(1156a872e, 3d7474, 047a) // i = 180
    SINIT(11519397b, 3d76b1, 0471) // i = 181
    SINIT(114c83320, 3d78ea, 046a) // i = 182
    SINIT(1147773b1, 3d7b1f, 0466) // i = 183
    SINIT(11426fabf, 3d7d52, 045e) // i = 184
    SINIT(113d6c7ec, 3d7f81, 0459) // i = 185
    SINIT(11386dad2, 3d81ad, 0453) // i = 186
    SINIT(113373306, 3d83d6, 044d) // i = 187
    SINIT(112e7d02c, 3d85fc, 0446) // i = 188
    SINIT(11298b1db, 3d881f, 043f) // i = 189
    SINIT(11249d7b1, 3d8a3f, 0438) // i = 190
    SINIT(111fb4151, 3d8c5b, 0435) // i = 191
    SINIT(111acee57, 3d8e75, 042d) // i = 192
    SINIT(1115ede5f, 3d908c, 0427) // i = 193
    SINIT(111111113, 3d929f, 0423) // i = 194
    SINIT(110c3860b, 3d94b0, 041c) // i = 195
    SINIT(110763cec, 3d96be, 0416) // i = 196
    SINIT(11029355b, 3d98c9, 0410) // i = 197
    SINIT(10fdc6ef9, 3d9ad1, 040b) // i = 198
    SINIT(10f8fe96f, 3d9cd6, 0406) // i = 199
    SINIT(10f43a45b, 3d9ed9, 03fe) // i = 200
    SINIT(10ef79f6b, 3da0d8, 03fa) // i = 201
    SINIT(10eabda3c, 3da2d5, 03f4) // i = 202
    SINIT(10e605479, 3da4cf, 03ef) // i = 203
    SINIT(10e150dcf, 3da6c6, 03ea) // i = 204
    SINIT(10dca05dd, 3da8bb, 03e3) // i = 205
    SINIT(10d7f3c51, 3daaad, 03dd) // i = 206
    SINIT(10d34b0d7, 3dac9c, 03d8) // i = 207
    SINIT(10cea6315, 3dae88, 03d5) // i = 208
    SINIT(10ca052b8, 3db072, 03cf) // i = 209
    SINIT(10c567f6f, 3db259, 03ca) // i = 210
    SINIT(10c0ce8df, 3db43e, 03c3) // i = 211
    SINIT(10bc38eb8, 3db620, 03bf) // i = 212
    SINIT(10b7a70aa, 3db7ff, 03bb) // i = 213
    SINIT(10b318e61, 3db9dc, 03b5) // i = 214
    SINIT(10ae8e788, 3dbbb7, 03ae) // i = 215
    SINIT(10aa07bd9, 3dbd8e, 03ac) // i = 216
    SINIT(10a584af7, 3dbf64, 03a5) // i = 217
    SINIT(10a10549c, 3dc137, 03a0) // i = 218
    SINIT(109c89876, 3dc307, 039c) // i = 219
    SINIT(109811636, 3dc4d5, 0397) // i = 220
    SINIT(10939cd8d, 3dc6a1, 0391) // i = 221
    SINIT(108f2be31, 3dc86a, 038d) // i = 222
    SINIT(108abe7d5, 3dca31, 0387) // i = 223
    SINIT(108654a2c, 3dcbf5, 0384) // i = 224
    SINIT(1081ee4ed, 3dcdb7, 037f) // i = 225
    SINIT(107d8b7c5, 3dcf77, 037a) // i = 226
    SINIT(10792c272, 3dd134, 0377) // i = 227
    SINIT(1074d04a7, 3dd2ef, 0373) // i = 228
    SINIT(107077e1f, 3dd4a8, 036d) // i = 229
    SINIT(106c22e87, 3dd65f, 0367) // i = 230
    SINIT(1067d15a0, 3dd813, 0364) // i = 231
    SINIT(10638331f, 3dd9c5, 0360) // i = 232
    SINIT(105f386bd, 3ddb75, 035b) // i = 233
    SINIT(105af1033, 3ddd23, 0355) // i = 234
    SINIT(1056acf3c, 3ddece, 0353) // i = 235
    SINIT(10526c391, 3de077, 0350) // i = 236
    SINIT(104e2ecef, 3de21e, 034c) // i = 237
    SINIT(1049f4b0c, 3de3c3, 0348) // i = 238
    SINIT(1045bddaa, 3de566, 0343) // i = 239
    SINIT(10418a480, 3de707, 033e) // i = 240
    SINIT(103d59f4f, 3de8a6, 0338) // i = 241
    SINIT(10392cdd0, 3dea42, 0337) // i = 242
    SINIT(103502fc4, 3debdd, 0331) // i = 243
    SINIT(1030dc4ec, 3ded75, 032e) // i = 244
    SINIT(102cb8cfd, 3def0c, 0328) // i = 245
    SINIT(1028987be, 3df0a0, 0326) // i = 246
    SINIT(10247b4ec, 3df233, 0320) // i = 247
    SINIT(102061447, 3df3c3, 031d) // i = 248
    SINIT(101c4a58d, 3df552, 0317) // i = 249
    SINIT(101836886, 3df6de, 0315) // i = 250
    SINIT(101425cee, 3df868, 0313) // i = 251
    SINIT(101018286, 3df9f1, 030d) // i = 252
    SINIT(100c0d90e, 3dfb78, 0309) // i = 253
    SINIT(100806052, 3dfcfc, 0307) // i = 254
    SINIT(10040180a, 3dfe7f, 0303) // i = 255
    SINIT(1fffffffb, 380003, 17e1) // i = 256
    SINIT(1ff00bf5b, 380bf4, 17a6) // i = 257
    SINIT(1fe02fb02, 3817c7, 176e) // i = 258
    SINIT(1fd06af46, 38237e, 1731) // i = 259
    SINIT(1fc0bd885, 382f17, 16fa) // i = 260
    SINIT(1fb12732b, 383a94, 16c2) // i = 261
    SINIT(1fa1a7bb0, 3845f5, 168b) // i = 262
    SINIT(1f923ee9f, 38513a, 1655) // i = 263
    SINIT(1f82ec87c, 385c64, 1620) // i = 264
    SINIT(1f73b05f1, 386773, 15eb) // i = 265
    SINIT(1f648a39f, 387268, 15b4) // i = 266
    SINIT(1f5579e37, 387d42, 1582) // i = 267
    SINIT(1f467f27c, 388803, 154c) // i = 268
    SINIT(1f3799d36, 3892a9, 151d) // i = 269
    SINIT(1f28c9b32, 389d37, 14ea) // i = 270
    SINIT(1f1a0e957, 38a7ac, 14b7) // i = 271
    SINIT(1f0b68489, 38b208, 1487) // i = 272
    SINIT(1efcd69b9, 38bc4c, 1457) // i = 273
    SINIT(1eee595e7, 38c678, 1427) // i = 274
    SINIT(1edff0618, 38d08c, 13f9) // i = 275
    SINIT(1ed19b75a, 38da89, 13cb) // i = 276
    SINIT(1ec35a6c8, 38e46f, 139d) // i = 277
    SINIT(1eb52d187, 38ee3e, 1370) // i = 278
    SINIT(1ea7134c2, 38f7f6, 1346) // i = 279
    SINIT(1e990cda9, 390199, 1318) // i = 280
    SINIT(1e8b19983, 390b25, 12ee) // i = 281
    SINIT(1e7d3958e, 39149c, 12c3) // i = 282
    SINIT(1e6f6bf1d, 391dfd, 129b) // i = 283
    SINIT(1e61b138b, 39274a, 126e) // i = 284
    SINIT(1e5409035, 393081, 1247) // i = 285
    SINIT(1e4673286, 3939a4, 121d) // i = 286
    SINIT(1e38ef7e8, 3942b3, 11f3) // i = 287
    SINIT(1e2b7dddd, 394bad, 11cd) // i = 288
    SINIT(1e1e1e1dd, 395494, 11a4) // i = 289
    SINIT(1e10d0177, 395d66, 1181) // i = 290
    SINIT(1e0393a38, 396626, 1158) // i = 291
    SINIT(1df6689b6, 396ed2, 1134) // i = 292
    SINIT(1de94ed8e, 39776c, 110c) // i = 293
    SINIT(1ddc4636f, 397ff2, 10ea) // i = 294
    SINIT(1dcf4e8f6, 398867, 10c3) // i = 295
    SINIT(1dc267be5, 3990c9, 109f) // i = 296
    SINIT(1db5919f4, 399919, 107b) // i = 297
    SINIT(1da8cc0e3, 39a157, 1058) // i = 298
    SINIT(1d9c16e7b, 39a983, 1037) // i = 299
    SINIT(1d8f7208e, 39b19e, 1014) // i = 300
    SINIT(1d82dd4e8, 39b9a8, 0ff2) // i = 301
    SINIT(1d7658970, 39c1a1, 0fcf) // i = 302
    SINIT(1d69e3c04, 39c989, 0fae) // i = 303
    SINIT(1d5d7ea90, 39d160, 0f8e) // i = 304
    SINIT(1d51292fe, 39d927, 0f6d) // i = 305
    SINIT(1d44e3342, 39e0de, 0f4c) // i = 306
    SINIT(1d38ac95b, 39e884, 0f2f) // i = 307
    SINIT(1d2c8534c, 39f01b, 0f0e) // i = 308
    SINIT(1d206cf16, 39f7a2, 0eef) // i = 309
    SINIT(1d1463aca, 39ff19, 0ed2) // i = 310
    SINIT(1d0869473, 3a0682, 0eb0) // i = 311
    SINIT(1cfc7da2f, 3a0ddb, 0e92) // i = 312
    SINIT(1cf0a0a1b, 3a1524, 0e78) // i = 313
    SINIT(1ce4d2253, 3a1c60, 0e57) // i = 314
    SINIT(1cd912107, 3a238c, 0e3b) // i = 315
    SINIT(1ccd6045c, 3a2aaa, 0e1e) // i = 316
    SINIT(1cc1bca8b, 3a31b9, 0e04) // i = 317
    SINIT(1cb6271c4, 3a38bb, 0de5) // i = 318
    SINIT(1caa9f848, 3a3fae, 0dcb) // i = 319
    SINIT(1c9f25c59, 3a4693, 0db1) // i = 320
    SINIT(1c93b9c3b, 3a4d6b, 0d94) // i = 321
    SINIT(1c885b638, 3a5435, 0d79) // i = 322
    SINIT(1c7d0a89d, 3a5af2, 0d5d) // i = 323
    SINIT(1c71c71c6, 3a61a1, 0d44) // i = 324
    SINIT(1c6691005, 3a6843, 0d2b) // i = 325
    SINIT(1c5b681b9, 3a6ed8, 0d12) // i = 326
    SINIT(1c504c542, 3a7561, 0cf6) // i = 327
    SINIT(1c453d90c, 3a7bdc, 0cdf) // i = 328
    SINIT(1c3a3bb7c, 3a824b, 0cc6) // i = 329
    SINIT(1c2f46b05, 3a88ae, 0cab) // i = 330
    SINIT(1c245e618, 3a8f04, 0c93) // i = 331
    SINIT(1c1982b2b, 3a954e, 0c7b) // i = 332
    SINIT(1c0eb38bc, 3a9b8c, 0c62) // i = 333
    SINIT(1c03f0d4e, 3aa1bd, 0c4e) // i = 334
    SINIT(1bf93a757, 3aa7e4, 0c33) // i = 335
    SINIT(1bee9056c, 3aadfe, 0c1e) // i = 336
    SINIT(1be3f2615, 3ab40d, 0c06) // i = 337
    SINIT(1bd9607e2, 3aba10, 0bf0) // i = 338
    SINIT(1bceda961, 3ac008, 0bda) // i = 339
    SINIT(1bc46092c, 3ac5f5, 0bc3) // i = 340
    SINIT(1bb9f25de, 3acbd7, 0bac) // i = 341
    SINIT(1baf8fe1b, 3ad1ad, 0b98) // i = 342
    SINIT(1ba539078, 3ad779, 0b82) // i = 343
    SINIT(1b9aedba2, 3add3a, 0b6d) // i = 344
    SINIT(1b90ade45, 3ae2f0, 0b59) // i = 345
    SINIT(1b8679708, 3ae89c, 0b43) // i = 346
    SINIT(1b7c5049c, 3aee3d, 0b30) // i = 347
    SINIT(1b72325b7, 3af3d4, 0b1b) // i = 348
    SINIT(1b681f909, 3af961, 0b06) // i = 349
    SINIT(1b5e17d54, 3afee4, 0af0) // i = 350
    SINIT(1b541b153, 3b045c, 0ade) // i = 351
    SINIT(1b4a293c0, 3b09cb, 0ac9) // i = 352
    SINIT(1b4042364, 3b0f30, 0ab5) // i = 353
    SINIT(1b3665f07, 3b148b, 0aa1) // i = 354
    SINIT(1b2c94570, 3b19dc, 0a90) // i = 355
    SINIT(1b22cd56c, 3b1f24, 0a7c) // i = 356
    SINIT(1b1910dcc, 3b2462, 0a6b) // i = 357
    SINIT(1b0f5ed62, 3b2997, 0a58) // i = 358
    SINIT(1b05b7300, 3b2ec3, 0a45) // i = 359
    SINIT(1afc19d86, 3b33e5, 0a35) // i = 360
    SINIT(1af286bc7, 3b38ff, 0a21) // i = 361
    SINIT(1ae8fdcae, 3b3e0f, 0a10) // i = 362
    SINIT(1adf7ef0b, 3b4317, 09fd) // i = 363
    SINIT(1ad60a1ce, 3b4816, 09ea) // i = 364
    SINIT(1acc9f3e0, 3b4d0b, 09dc) // i = 365
    SINIT(1ac33e41c, 3b51f9, 09c8) // i = 366
    SINIT(1ab9e717c, 3b56dd, 09ba) // i = 367
    SINIT(1ab099ae6, 3b5bba, 09a6) // i = 368
    SINIT(1aa755f52, 3b608d, 0998) // i = 369
    SINIT(1a9e1bdad, 3b6559, 0985) // i = 370
    SINIT(1a94eb4f2, 3b6a1c, 0975) // i = 371
    SINIT(1a8bc4417, 3b6ed7, 0965) // i = 372
    SINIT(1a82a6a1a, 3b738a, 0954) // i = 373
    SINIT(1a79925f9, 3b7834, 0947) // i = 374
    SINIT(1a70876ac, 3b7cd7, 0937) // i = 375
    SINIT(1a6785b40, 3b8172, 0927) // i = 376
    SINIT(1a5e8d2b5, 3b8605, 0918) // i = 377
    SINIT(1a559dc10, 3b8a91, 0906) // i = 378
    SINIT(1a4cb7661, 3b8f14, 08fa) // i = 379
    SINIT(1a43da0ab, 3b9391, 08e7) // i = 380
    SINIT(1a3b05a04, 3b9805, 08db) // i = 381
    SINIT(1a323a177, 3b9c72, 08cd) // i = 382
    SINIT(1a297761a, 3ba0d8, 08bd) // i = 383
    SINIT(1a20bd702, 3ba536, 08b0) // i = 384
    SINIT(1a180c33d, 3ba98e, 089e) // i = 385
    SINIT(1a0f639ed, 3badde, 0890) // i = 386
    SINIT(1a06c3a2f, 3bb226, 0885) // i = 387
    SINIT(19fe2c316, 3bb668, 0876) // i = 388
    SINIT(19f59d3c4, 3bbaa3, 0868) // i = 389
    SINIT(19ed16b61, 3bbed7, 0859) // i = 390
    SINIT(19e498907, 3bc304, 084c) // i = 391
    SINIT(19dc22be2, 3bc72a, 083f) // i = 392
    SINIT(19d3b5318, 3bcb49, 0833) // i = 393
    SINIT(19cb4fdcb, 3bcf62, 0825) // i = 394
    SINIT(19c2f2b2f, 3bd374, 0818) // i = 395
    SINIT(19ba9da6a, 3bd780, 0809) // i = 396
    SINIT(19b250aae, 3bdb85, 07fc) // i = 397
    SINIT(19aa0bb2d, 3bdf83, 07f1) // i = 398
    SINIT(19a1ceb14, 3be37b, 07e5) // i = 399
    SINIT(199999997, 3be76d, 07d8) // i = 400
    SINIT(19916c5f1, 3beb59, 07c9) // i = 401
    SINIT(198946f57, 3bef3e, 07be) // i = 402
    SINIT(198129503, 3bf31d, 07b2) // i = 403
    SINIT(197913630, 3bf6f6, 07a5) // i = 404
    SINIT(197105217, 3bfac9, 0799) // i = 405
    SINIT(1968fe7fa, 3bfe96, 078d) // i = 406
    SINIT(1960ff71c, 3c025c, 0784) // i = 407
    SINIT(195907eb8, 3c061d, 0778) // i = 408
    SINIT(195117e12, 3c09d9, 0769) // i = 409
    SINIT(19492f476, 3c0d8e, 075e) // i = 410
    SINIT(19414e124, 3c113d, 0755) // i = 411
    SINIT(193974369, 3c14e7, 0748) // i = 412
    SINIT(1931a1a8a, 3c188b, 073d) // i = 413
    SINIT(1929d65d0, 3c1c2a, 0730) // i = 414
    SINIT(192212491, 3c1fc2, 0729) // i = 415
    SINIT(191a55614, 3c2356, 071b) // i = 416
    SINIT(19129f9aa, 3c26e4, 0710) // i = 417
    SINIT(190af0ea6, 3c2a6c, 0707) // i = 418
    SINIT(19034945d, 3c2def, 06fc) // i = 419
    SINIT(18fba8a1c, 3c316d, 06f1) // i = 420
    SINIT(18f40ef3f, 3c34e5, 06e7) // i = 421
    SINIT(18ec7c315, 3c3858, 06de) // i = 422
    SINIT(18e4f0501, 3c3bc6, 06d3) // i = 423
    SINIT(18dd6b454, 3c3f2f, 06c8) // i = 424
    SINIT(18d5ed06f, 3c4293, 06bc) // i = 425
    SINIT(18ce758ad, 3c45f1, 06b4) // i = 426
    SINIT(18c704c69, 3c494b, 06a8) // i = 427
    SINIT(18bf9ab05, 3c4c9f, 06a0) // i = 428
    SINIT(18b8373e1, 3c4fef, 0694) // i = 429
    SINIT(18b0da661, 3c5339, 068c) // i = 430
    SINIT(18a9841df, 3c567f, 0682) // i = 431
    SINIT(18a2345cb, 3c59c0, 0677) // i = 432
    SINIT(189aeb184, 3c5cfc, 066e) // i = 433
    SINIT(1893a8471, 3c6033, 0666) // i = 434
    SINIT(188c6bdfe, 3c6366, 065a) // i = 435
    SINIT(188535d8e, 3c6694, 0651) // i = 436
    SINIT(187e06292, 3c69bd, 0649) // i = 437
    SINIT(1876dcc75, 3c6ce1, 0642) // i = 438
    SINIT(186fb9a9f, 3c7001, 0638) // i = 439
    SINIT(18689cc7d, 3c731d, 062d) // i = 440
    SINIT(186186185, 3c7634, 0625) // i = 441
    SINIT(185a75927, 3c7946, 061d) // i = 442
    SINIT(18536b2cc, 3c7c54, 0615) // i = 443
    SINIT(184c66df1, 3c7f5e, 060b) // i = 444
    SINIT(184568a03, 3c8263, 0603) // i = 445
    SINIT(183e7067a, 3c8564, 05fa) // i = 446
    SINIT(18377e2c9, 3c8861, 05f1) // i = 447
    SINIT(183091e6c, 3c8b59, 05e9) // i = 448
    SINIT(1829ab8d3, 3c8e4d, 05e2) // i = 449
    SINIT(1822cb181, 3c913d, 05d9) // i = 450
    SINIT(181bf07e8, 3c9429, 05d0) // i = 451
    SINIT(18151bb84, 3c9711, 05c7) // i = 452
    SINIT(180e4cbd8, 3c99f4, 05c1) // i = 453
    SINIT(18078385b, 3c9cd4, 05b7) // i = 454
    SINIT(1800c0092, 3c9faf, 05b0) // i = 455
    SINIT(17fa023f0, 3ca287, 05a6) // i = 456
    SINIT(17f34a201, 3ca55a, 05a0) // i = 457
    SINIT(17ec97a3f, 3ca82a, 0597) // i = 458
    SINIT(17e5eac32, 3caaf5, 0591) // i = 459
    SINIT(17df43758, 3cadbd, 0588) // i = 460
    SINIT(17d8a1b36, 3cb081, 0580) // i = 461
    SINIT(17d205755, 3cb341, 0578) // i = 462
    SINIT(17cb6eb36, 3cb5fd, 0572) // i = 463
    SINIT(17c4dd662, 3cb8b6, 0568) // i = 464
    SINIT(17be51862, 3cbb6a, 0564) // i = 465
    SINIT(17b7cb0c1, 3cbe1b, 055c) // i = 466
    SINIT(17b149efe, 3cc0c9, 0552) // i = 467
    SINIT(17aace2ae, 3cc372, 054e) // i = 468
    SINIT(17a457b5d, 3cc618, 0546) // i = 469
    SINIT(179de688c, 3cc8bb, 053d) // i = 470
    SINIT(17977a9d2, 3ccb5a, 0536) // i = 471
    SINIT(179113ebc, 3ccdf5, 052f) // i = 472
    SINIT(178ab26d5, 3cd08d, 0527) // i = 473
    SINIT(1784561ad, 3cd321, 0522) // i = 474
    SINIT(177dfeed9, 3cd5b2, 051a) // i = 475
    SINIT(1777acde9, 3cd83f, 0514) // i = 476
    SINIT(17715fe6a, 3cdac9, 050d) // i = 477
    SINIT(176b17ff0, 3cdd50, 0505) // i = 478
    SINIT(1764d5214, 3cdfd3, 04ff) // i = 479
    SINIT(175e97468, 3ce253, 04f8) // i = 480
    SINIT(17585e683, 3ce4cf, 04f2) // i = 481
    SINIT(17522a7f3, 3ce748, 04ed) // i = 482
    SINIT(174bfb857, 3ce9be, 04e6) // i = 483
    SINIT(1745d1744, 3cec31, 04de) // i = 484
    SINIT(173fac455, 3ceea0, 04d9) // i = 485
    SINIT(17398bf1d, 3cf10c, 04d4) // i = 486
    SINIT(173370739, 3cf375, 04ce) // i = 487
    SINIT(172d59c46, 3cf5db, 04c7) // i = 488
    SINIT(172747dda, 3cf83e, 04c0) // i = 489
    SINIT(17213ab96, 3cfa9e, 04b8) // i = 490
    SINIT(171b32516, 3cfcfa, 04b4) // i = 491
    SINIT(17152e9f3, 3cff54, 04ac) // i = 492
    SINIT(170f2f9d2, 3d01aa, 04a7) // i = 493
    SINIT(170935448, 3d03fe, 049f) // i = 494
    SINIT(17033f901, 3d064e, 049b) // i = 495
    SINIT(16fd4e794, 3d089b, 0496) // i = 496
    SINIT(16f761fa2, 3d0ae6, 048e) // i = 497
    SINIT(16f17a0d3, 3d0d2d, 048a) // i = 498
    SINIT(16eb96ac0, 3d0f72, 0483) // i = 499
    SINIT(16e5b7d15, 3d11b3, 0480) // i = 500
    SINIT(16dfdd772, 3d13f2, 0479) // i = 501
    SINIT(16da07979, 3d162e, 0473) // i = 502
    SINIT(16d4362d0, 3d1867, 046e) // i = 503
    SINIT(16ce6931c, 3d1a9e, 0465) // i = 504
    SINIT(16c8a0a04, 3d1cd1, 0462) // i = 505
    SINIT(16c2dc730, 3d1f02, 045b) // i = 506
    SINIT(16bd1ca44, 3d2130, 0455) // i = 507
    SINIT(16b7612ec, 3d235b, 0450) // i = 508
    SINIT(16b1aa0cb, 3d2583, 044d) // i = 509
    SINIT(16abf7390, 3d27a9, 0446) // i = 510
    SINIT(16a648ae1, 3d29cc, 0441) // i = 511
};

#undef _K
#undef _M
#undef _T
#undef _S
#undef _N

// ============================================================================

#undef  _SA
#undef  _SB
#undef  _SC

#define _SA	29
#define _SB	16
#define _SC	12

#define _S	 9
#define _M	22
#define _K	 6
#define _T	20

#define _N _Na
#if (_Nb > _Na)
#   undef  _N
#   define _N _Nb
#endif
#if (_Nc > _N)
#   undef  _N
#   define _N _Nb
#endif

EmulationCoefficients_t _exp223Table[] = {
        // 2^29        2^16      2^12
        UINIT(1ffffff8, 0b172, 03dc) // index = 0
        UINIT(20593484, 0b360, 03e9) // index = 1
        UINIT(20b361ae, 0b554, 03f4) // index = 2
        UINIT(210e8a33, 0b74e, 03fd) // index = 3
        UINIT(216ab0de, 0b94d, 0408) // index = 4
        UINIT(21c7d86a, 0bb51, 0416) // index = 5
        UINIT(222603a9, 0bd5b, 0422) // index = 6
        UINIT(22853563, 0bf6b, 042d) // index = 7
        UINIT(22e57079, 0c181, 0437) // index = 8
        UINIT(2346b7e0, 0c39c, 0444) // index = 9
        UINIT(23a90e67, 0c5be, 044d) // index = 10
        UINIT(240c7717, 0c7e5, 045a) // index = 11
        UINIT(2470f4e4, 0ca12, 0467) // index = 12
        UINIT(24d68ad5, 0cc45, 0475) // index = 13
        UINIT(253d3bea, 0ce7f, 047f) // index = 14
        UINIT(25a50b4a, 0d0bf, 048a) // index = 15
        UINIT(260dfc1f, 0d304, 049a) // index = 16
        UINIT(26781166, 0d551, 04a4) // index = 17
        UINIT(26e34e74, 0d7a3, 04b4) // index = 18
        UINIT(274fb675, 0d9fc, 04c1) // index = 19
        UINIT(27bd4c9e, 0dc5c, 04cd) // index = 20
        UINIT(282c144c, 0dec2, 04dc) // index = 21
        UINIT(289c10cb, 0e12f, 04e9) // index = 22
        UINIT(290d456f, 0e3a3, 04f6) // index = 23
        UINIT(297fb5a6, 0e61e, 0502) // index = 24
        UINIT(29f364f3, 0e89f, 0512) // index = 25
        UINIT(2a6856a8, 0eb28, 051e) // index = 26
        UINIT(2ade8e75, 0edb7, 052e) // index = 27
        UINIT(2b560fc3, 0f04e, 053b) // index = 28
        UINIT(2bcede35, 0f2ec, 0549) // index = 29
        UINIT(2c48fd62, 0f591, 0559) // index = 30
        UINIT(2cc47110, 0f83d, 056a) // index = 31
        UINIT(2d413cd9, 0faf1, 0579) // index = 32
        UINIT(2dbf6485, 0fdad, 0586) // index = 33
        UINIT(2e3eebd5, 10070, 0597) // index = 34
        UINIT(2ebfd6ae, 1033b, 05a6) // index = 35
        UINIT(2f4228e6, 1060e, 05b4) // index = 36
        UINIT(2fc5e675, 108e8, 05c6) // index = 37
        UINIT(304b132f, 10bcb, 05d4) // index = 38
        UINIT(30d1b33f, 10eb5, 05e6) // index = 39
        UINIT(3159ca8a, 111a8, 05f5) // index = 40
        UINIT(31e35d37, 114a3, 0605) // index = 41
        UINIT(326e6f60, 117a6, 0617) // index = 42
        UINIT(32fb054b, 11ab1, 062a) // index = 43
        UINIT(33892312, 11dc5, 063b) // index = 44
        UINIT(3418ccfe, 120e2, 064b) // index = 45
        UINIT(34aa0770, 12407, 065e) // index = 46
        UINIT(353cd6b7, 12735, 0670) // index = 47
        UINIT(35d13f3c, 12a6c, 0682) // index = 48
        UINIT(3667457c, 12dac, 0693) // index = 49
        UINIT(36feedf3, 130f5, 06a5) // index = 50
        UINIT(37983d2e, 13447, 06b8) // index = 51
        UINIT(383337b9, 137a3, 06c9) // index = 52
        UINIT(38cfe254, 13b08, 06db) // index = 53
        UINIT(396e41c6, 13e76, 06ee) // index = 54
        UINIT(3a0e5aa1, 141ed, 0705) // index = 55
        UINIT(3ab031c0, 1456f, 0716) // index = 56
        UINIT(3b53cc0c, 148fa, 072b) // index = 57
        UINIT(3bf92e70, 14c8f, 073f) // index = 58
        UINIT(3ca05dda, 1502e, 0753) // index = 59
        UINIT(3d495f52, 153d7, 0768) // index = 60
        UINIT(3df437ea, 1578b, 077a) // index = 61
        UINIT(3ea0ecc3, 15b48, 0792) // index = 62
        UINIT(3f4f830f, 15f11, 07a3) // index = 63
};

#undef _K
#undef _S
#undef _O

static unsigned int ClassifyType32(type32 val) {
    int  exp, sig, sign;
    unsigned int class;

    exp  = (val.u & FP32_EXP_MASK) >> FP32_EXP_POS;
    sign = val.u >> FP32_SIGN_POS;
    // Assume is a normal argument and then check if the exponent field
    // is either all zeros or all ones
    class = avx3PosNormal;
    if (((exp + 1) & FP32_NON_NORMAL_MASK) == 0) {
         // This is a non-normal argument. Set the base class to zero or
         // Infinity depending on the low bit of the exponent.
         class = (exp & 1) ? avx3PosInfinity : avx3PosZero;
        // If the signifcand field is non-zero, then map zero and Infinity
        // classes to denormal and NaN respectively by Incrementing the base
        // class by 2.
        sig = val.u & FP32_MANT_MASK;
        if (sig) {
            class += 2;
        }
        // For NaNs, the sign is irrelevant. However, we need to distinguish
        // between sNaN and qNaN. So for a NaN base class, use the NaN "quiet"
        // bit, rather than the sign bit for the next adjustment
        if (class == avx3SNan) {
            sign = (sig >> FP32_QNAN_BIT_POS);
        }
    }
    // Add the sign information to the platform dependent classes
    class += (unsigned int)sign;
    return class;
}

static unsigned int ClassifyType64(type64 val) {
    long long int sig, exp, sign;
    unsigned int class;

    // See comments in ClassifyType32 for algorithm description
    exp  = (val.u & FP64_EXP_MASK) >> FP64_EXP_POS;
    sign = val.u >> FP64_SIGN_POS;
    class = avx3PosNormal;
    if (((exp + 1) & FP64_NON_NORMAL_MASK) == 0) {
         class = (exp & 1) ? avx3PosInfinity : avx3PosZero;
         sig  = val.u & FP64_MANT_MASK;
         if (sig) {
             class += 2;
         }
         if (class == avx3SNan) {
             sign = (sig >> FP64_QNAN_BIT_POS);
         }
    }
    class += sign;
    return class;
}

static unsigned int _sCvtdl(int64_t *dst, type64 src) {
    double tmp;
    int64_t tmpDst;
    double diff;
    unsigned int flags = inexactFlag;

    tmp    = floor(src.f);
    diff   = src.f - tmp;
    tmpDst = (int64_t)tmp;
    if (diff > .5) {
        tmpDst += 1;
    } else if (diff < .5) {
        if (diff == 0) {
            flags = 0;
        }
    } else {
        tmpDst += (tmpDst & 1);
    }
    *dst = tmpDst;
    return flags;
}

static unsigned int _sCvtfl(int64_t *dst, type32 src) {
    type64 tmp;
    tmp.f = (double) src.f;
    return _sCvtdl(dst, tmp);
}

static unsigned long long int EmulSquare(unsigned int x) {
    unsigned long long int xx = (unsigned long long int) x;
    xx = xx * xx;
    return xx >> 24;
}

static int64_t SignedBitsShiftRight(uint64_t bits, unsigned int shift) {
    if ((bits & UINT64_C(0x8000000000000000)) == 0) {
        return (int64_t)(bits >> shift);
    }
    return -INT64_C(1) - (int64_t)((~bits) >> shift);
}

static int FloorDivideByTwo(int value) {
    return value >= 0 ? value / 2 : -((-value + 1) / 2);
}

// =============================================================================
// Emulation() takes 'scale', 'adj' and 'arg' as inputs; 'arg' is an integer
// value that is logically divided into two fields: the low n bits identified
// as Z, and the high m bits identified as i.
//
// Emulation() performs the following operation:
//
//	res <-- adj + a[i] + b[i]*z + c[i]*sqr(z)
//
// where:
//
//	z       equals Z/2^(k+m). (The value of k depends on the function being
//		evaluated)
//	sqr(z)	is an approximation to z*z.
//	adj	is a 64-bit integer value with an assumed binary point between
//              bits 55 and 56
//	a[i],	Coefficients obtained from a ROM table. The index i
//	b[i],	     is taken as the high "m" bits
//	c[i]              of the input argument "arg"
//
// Examine next the computation
//
//	p = a[i] + b[i]*z + c[i]*z^2
//
// The coefficients a[i], b[i] and c[i] are stored as integer values with
// implied scale factors of 1/2^sa, 1/2^sb and 1/2^sc respectively.
// Since z = Z/2^(k+m), the above can be written as:
//
//           A         B*Z           C*Z^2
//	p = ----  + ---------- + --------------
//          2^sa    2^(sb+k+m)   2^(sc+2*k+2*m)
//
// Emulation() computes the high t bits of an approximation to Z^2, call it S.
// i.e. Z^2 ~ 2^(2*m-t)*S, so the final polynomial calculation is:
//
//           A          B*Z         C*S
//	p = ----  + ---------- + -----------
//          2^sa    2^(sb+k+m)   2^(sc+2*k+t)
//
// Let N = max(sa, sb+k+m, sc+2*k+t). Then above can be writen as:
//
//	p = {[2^(N-sa)*A] + [2^(N-sb-k-m)*B]*Z + [2^(N-sc-2*k-t)*C]*S}/2^N
//	p = {[2^n1*A] + [2^n2*B]*Z + [2^n3*C]*S}/2^N
//	  = {A' + B'*Z + C'*S}/2^51
//
// Note that n1, n2 and n3 are all greater than or equal to zero, so that A',
// B' and C' are A, B and C shifted left by some number of bits. In particular
// this means that the function-specific scaling value, k, can be incorporated
// into the stored (table) constants and therefore need not be an explicitly
// part of the of the calculation.
//
// Finally, the integer value P = A' + B'*Z + C'*S will ultimatately be used
// as the fraction field of a double precision value. Consequently, it would be
// convenient if  2^52 <= P < 2^53. We note here, that there is a function
// specific value, s, such that 2^s*P is appropriately normalized. Therefore,
// the actual coefficients used by the Emulation routine are defined by:
//
//	A' = 2^(n1 + s)*A
//	B' = 2^(n2 + s)*B
//	C' = 2^(n3 + s)*C
//
// As noted above, for the current EMU implementation, the ROM coefficients A, B
// and C are 33, 20 and 14 bits long respectively (not including a sign bit). So
// as long as 19 <= k - s, B' and C' will fit in a 32-bit integer and A' will
// fit in a 64-bit integer
// =============================================================================

static uint64_t Emulation(type64 *dst, uint64_t x, uint64_t addend,
                          EmulationCoefficients_t *table) {
    int            index, scale;
    int64_t         a, b, c;
    uint64_t        emuRes, dstVal;
    unsigned int         xl;
    EmulationCoefficients_t * coefPtr;

    index   = (int) (x >> 22);
    xl      = (unsigned int)(x & 0x3fffff);
    coefPtr = &table[index];
    a = (long long int) coefPtr->a;
    b = (long long int) coefPtr->b;
    c = (long long int) coefPtr->c;
    // The published reference relies on two's-complement signed overflow while
    // normalizing this fixed-point polynomial. Express the same modular
    // arithmetic with unsigned integers so optimizing compilers cannot treat
    // the overflow path as unreachable.
    emuRes = addend + (uint64_t)a + (uint64_t)b * xl +
             (uint64_t)c * EmulSquare(xl);
    // Detemine the sign of the result and normalize it. Record the sign and
    // scale factor in the sign and expoent fields of dst.
    dstVal = dst->u;
    if ((emuRes & UINT64_C(0x8000000000000000)) != 0) {
        dstVal ^= FP64_SIGN_BIT;
        emuRes = UINT64_C(0) - emuRes;
    }
    scale = 10 + FP64_EXP_BIAS;
    while (emuRes != 0 &&
           (emuRes & UINT64_C(0x8000000000000000)) == 0) {
        scale--;
        emuRes <<= 1;
    }
    dstVal = (dstVal & ~FP64_EXP_MASK) | (((unsigned long long int) scale) << FP64_EXP_POS);
    dst->u = dstVal;
    return emuRes;
}

// ============================================================================
// Given the (64-bit) result from Emulation(), EmulationFinish{f,d} scales
// the result and packs it into a single or double precision floating-point
// result. These routines also handle overflow and underflow conditions.
// ============================================================================
static unsigned int EmulationFinishd (type64 * dst,
        uint64_t emuRes, int scale, unsigned int flags) {

    unsigned long long int  res, sign;
    // Take care of "extreme"  underflows
    scale += (int)((dst->u & FP64_EXP_MASK) >> FP64_EXP_POS);
    if (scale < -54) scale = -54;
    while (scale <= 0) {
        scale++;
        emuRes = (emuRes >> 1) | (emuRes & 1);
    }
    // Convert to double precision floating-point with no rounding
    res = emuRes >> 11;
    // Check for final overflow or underflow.
    sign = dst->u & FP64_SIGN_BIT;
    if (res < FP64_HIDDEN_BIT) {
        res = sign;
    } else if (scale > (FP64_MAX_EXP - 1)) {
        flags |= overflowFlag;
        res = sign | FP64_PLUS_INF_AS_UINT64;
    } else {
        res = sign | ((unsigned long long int) scale << FP64_EXP_POS)
                   | (res & FP64_MANT_MASK);
    }
    dst->u = res;
    return flags;
}

static unsigned int EmulationFinishf (type32 * dst,
        type64 tmp, uint64_t emuRes, int scale, unsigned int flags) {

    unsigned long long int  res, sign, incr, rndIt;

    // Take care of "extreme"  underflows.
    scale += (int)((tmp.u & FP64_EXP_MASK) >> FP64_EXP_POS) +
             FP32_EXP_BIAS - FP64_EXP_BIAS;
    if (scale < -25) scale = -25;
    while (scale <= 0) {
        scale++;
        emuRes = (emuRes >> 1) | (emuRes & 1);
    }
    // Convert to single precision floating-point with rounding-to-nearest
    incr    = 0x1ull << 39;
    rndIt   = (emuRes >> 39) & 3;		// lowBit:rndBit
    if (emuRes & (incr - 1)) {
        rndIt |= 2;			// 'OR' sticky bit with lowBit
    }
    if (3 == rndIt) {
        emuRes += incr;
    }
    if (emuRes < incr) {
        // Carry-out from rounding; adjust exponent
        scale++;
        emuRes = (emuRes >> 1) | 0x8000000000000000ull;
    }
    res = emuRes >> 40;
    // Check for final overflow or underflow.
    sign = (tmp.u & FP64_SIGN_BIT) >> 32;
    if (res < FP32_HIDDEN_BIT) {
        res = sign;
    } else if (scale > (FP32_MAX_EXP - 1)) {
        flags |= overflowFlag;
        res = sign | FP32_PLUS_INF_AS_UINT32;
    } else {
        res = sign | ((unsigned long long int) scale << FP32_EXP_POS)
                   | (res & FP32_MANT_MASK);
    }
    dst->u = (unsigned int) res;
    return flags;
}

/*
    ****************************************************************************
    ****************************************************************************
    ********************** EMULATION ROUTINES FOR RCP28 ************************
    ****************************************************************************
    ****************************************************************************
*/

// These reference functions have to be compiled in Linux with DAZ and FTZ
// off (e.g. with the Intel compiler: icc -c -no-ftz RECIP28EXP2.c). They have to
// be run with the rounding mode set to round-to-nearest, and with
// floating-point exceptions masked.

/* EXAMPLE: icc -no-ftz main.c RECIP28EXP2.c, where main.c is shown below

#include <stdio.h>
typedef union {
    unsigned int u;
    float f;
} type32;
typedef union {
    unsigned long long u;
    double f;
} type64;
extern unsigned int RCP28S (type32 *dst, type32 src);
extern unsigned int RCP28D (type64 *dst, type64 src);

main () {
  type32 dst32, src32;
  type64 dst64, src64;
  unsigned int flags = 0x00000000; // PUOZDI

  printf ("FLAGS = %2.2x\n", flags);
  src32.f = 3.0;
  flags = RCP28S (&dst32, src32);
  printf ("RCP28S(%f = %8.8x HEX) = (%f = %8.8x HEX) flags = %2.2x\n",
      src32.f, src32.u, dst32.f, dst32.u, flags);
  src64.f = 3.0;
  flags = RCP28D (&dst64, src64);
  printf ("RCP28D(%f = %16.16llx HEX) = (%f = %16.16llx HEX) flags = %2.2x\n",
      src64.f, src64.u, dst64.f, dst64.u, flags);
  return (0);
}

*/
// ============================================================================
// Single precision reciprocal approximation with less than 2^-28 relative error.
// Uses Emulation() to produce (at least) a 28-bit approximation to 1/x and then
// rounds the result to 24 bits.
// ============================================================================
unsigned int RCP28S (type32 *dst, type32 src) {
    int      scale;
    unsigned int   class;
    unsigned long long int   emuRes;
    unsigned int  flags = 0;
    type64   tmp;
    unsigned int   iSrc = src.u;

    // Classify the input and process "special" values
    scale = (int) FP32_UNBIASED_EXP(iSrc);
    class = ClassifyType32(src);
    switch(class) {
        case avx3NegInfinity:  RET_DST      (FP32_MINUS_ZERO_AS_UINT32);
        case avx3NegZero:      SET_Z_RET_DST(FP32_MINUS_INF_AS_UINT32 );
        case avx3PosZero:      SET_Z_RET_DST(FP32_PLUS_INF_AS_UINT32  );
        case avx3PosInfinity:  RET_DST      (FP32_PLUS_ZERO_AS_UINT32 );
        case avx3QNan:         RET_DST      (iSrc                     );
        case avx3SNan:         SET_V_RET_DST(FP32_QUIETIZE_NAN(iSrc)  );
        case avx3NegDenorm:
        case avx3PosDenorm:    // Treat denormals as zero
                               SET_Z_RET_DST(class & 1 ?
                                       FP32_MINUS_INF_AS_UINT32 :
                                       FP32_PLUS_INF_AS_UINT32);
    }
    // For x = s*2^n*f, where s is +/- 1 and f is in [1,2), compute
    // recip(x) = s*(2^-n)*recip(f). Also ensure that recip(1) = 1
    tmp.u = ((unsigned long long int) (iSrc & FP32_SIGN_BIT)) << 32;
    if ((iSrc & FP32_MANT_MASK) == 0) {
        // src is an exact power of 2. Force result mantissa to 1.
        emuRes     = 0x1ull << 63;
        tmp.u |= FP64_PLUS_ONE_AS_UINT64;
    } else {
        // index = bits(22:15) fraction = bits(14:0):00000000, total = 30 bits
        emuRes = Emulation(&tmp, (iSrc << 7) & 0x3fffffff, 0, _rcp28Table);
    }
    return EmulationFinishf (dst, tmp, emuRes, -scale, flags);
}

// ============================================================================
// Double precision reciprocal approximation with less than 2^-28 relative error.
// Uses Emulation() to produce (at least) a 28-bit approximation to 1/x
// ============================================================================
unsigned int RCP28D (type64 *dst, type64 src) {
    int     scale;
    unsigned int  class;
    unsigned long long int  emuRes;
    unsigned int flags = 0;
    unsigned long long int  iSrc  = src.u;

    // Classify the input and process "special" values
    class = ClassifyType64(src);
    scale = (int) FP64_UNBIASED_EXP(iSrc);
    switch(class) {
        case avx3NegInfinity:  RET_DST      (FP64_MINUS_ZERO_AS_UINT64);
        case avx3NegZero:      SET_Z_RET_DST(FP64_MINUS_INF_AS_UINT64 );
        case avx3PosZero:      SET_Z_RET_DST(FP64_PLUS_INF_AS_UINT64  );
        case avx3PosInfinity:  RET_DST      (FP64_PLUS_ZERO_AS_UINT64 );
        case avx3QNan:         RET_DST      (iSrc                     );
        case avx3SNan:         SET_V_RET_DST(FP64_QUIETIZE_NAN(iSrc)  );
        case avx3NegDenorm:
        case avx3PosDenorm:    // Treat denormals as zero
                               SET_Z_RET_DST(class & 1 ?
                                       FP64_MINUS_INF_AS_UINT64 :
                                       FP64_PLUS_INF_AS_UINT64);
    }
    // For x = s*2^n*f, where s is +/- 1 and f is in [1,2), compute
    // recip(x) = s*(2^-n)*recip(f). Also ensure that recip(1) = 1
    dst->u = iSrc & FP64_SIGN_BIT;
    if ((iSrc & FP64_MANT_MASK) == 0) {
        // src is an exact power of 2. Force result mantissa to 1.
        emuRes     = 0x1ull << 63;
        dst->u |= FP64_PLUS_ONE_AS_UINT64;
    } else {
        // index = bits(51:44) fract = bits(43:22):0, tolal = 30 bits
        // emuRes = Emulation(dst, (iSrc >> 22) & 0x3ffffffe, 0, _rcp28Table);
        emuRes = Emulation(dst, (iSrc >> 22) & 0x3fffffff, 0, _rcp28Table);
    }
    return EmulationFinishd (dst, emuRes, -scale, flags);
}

/*
    ****************************************************************************
    ****************************************************************************
    ********************** EMULATION ROUTINES FOR RSQRT28 **********************
    ****************************************************************************
    ****************************************************************************
*/

// These reference functions have to be compiled in Linux with DAZ and FTZ
// off (e.g. with the Intel compiler: icc -c -no-ftz RECIP28EXP2.c). They have to
// be run with the rounding mode set to round-to-nearest, and with
// floating-point exceptions masked.

/* EXAMPLE: icc -no-ftz main.c RECIP28EXP2.c, where main.c is shown below

#include <stdio.h>
typedef union {
    unsigned int u;
    float f;
} type32;
typedef union {
    unsigned long long u;
    double f;
} type64;
extern unsigned int RSQRT28S (type32 *dst, type32 src);
extern unsigned int RSQRT28D (type64 *dst, type64 src);

main () {
  type32 dst32, src32;
  type64 dst64, src64;
  unsigned int flags = 0x00000000; // PUOZDI

  printf ("FLAGS = %2.2x\n", flags);
  src32.f = 2.0;
  flags = RSQRT28S (&dst32, src32);
  printf ("RSQRT28S(%f = %8.8x HEX) = (%f = %8.8x HEX) flags = %2.2x\n",
      src32.f, src32.u, dst32.f, dst32.u, flags);
  src64.f = 2.0;
  flags = RSQRT28D (&dst64, src64);
  printf ("RSQRT28D(%f = %16.16llx HEX) = (%f = %16.16llx HEX) flags = %2.2x\n",
      src64.f, src64.u, dst64.f, dst64.u, flags);
  return (0);
}

*/
// ============================================================================
// Single precision reciprocal square root approximation with less than 2^-28
// relative error.
// Uses Emulation() to produce (at least) a 28-bit approximation to 1/sqrt(x) and
// then rounds the result to 24 bits.
// ============================================================================
unsigned int RSQRT28S(type32 *dst, type32 src) {
    int      scale;
    unsigned long long int   emuRes;
    type64 tmp;
    unsigned int   class;
    unsigned int  flags = 0;
    unsigned int   iSrc = src.u;

    // Classify the input and process "special" values
    scale =  (int) FP32_UNBIASED_EXP(iSrc);
    class = ClassifyType32(src);
    switch(class) {
        case avx3QNan:         RET_DST      (iSrc                      );
        case avx3SNan:         SET_V_RET_DST(FP32_QUIETIZE_NAN(iSrc)   );
        case avx3NegNormal:
        case avx3NegInfinity:  SET_V_RET_DST(FP32_DEFAULT_NAN_AS_UINT32);
        case avx3PosDenorm:
        case avx3PosZero:      SET_Z_RET_DST(FP32_PLUS_INF_AS_UINT32   );
        case avx3NegDenorm:
        case avx3NegZero:      SET_Z_RET_DST(FP32_MINUS_INF_AS_UINT32  );
        case avx3PosInfinity:  RET_DST(      FP32_PLUS_ZERO_AS_UINT32  );
    }

    tmp.u = 0;
    if ((iSrc & (FP32_MANT_MASK | FP32_HIDDEN_BIT)) == FP32_HIDDEN_BIT){
        // src is exactly 2^(2k), force result mantissa to 1.
        emuRes     = 0x1ull << 63;
        tmp.u |= FP64_PLUS_ONE_AS_UINT64;
    } else {
        // index = bits(23:15), fract = bits(14:0):0000000, total = 31
        emuRes = Emulation(&tmp, (iSrc << 7) & 0x7fffffff, 0, _rsqrt28Table);
    }
    return EmulationFinishf(dst, tmp, emuRes, -FloorDivideByTwo(scale),
                            flags);
}

// ============================================================================
// Double precision reciprocal square root approximation with less than 2^-28
// relative error.
// Uses Emulation() to produce (at least) a 28-bit approximation to 1/sqrt(x)
// ============================================================================
unsigned int RSQRT28D(type64 *dst, type64 src) {
    int     scale;
    unsigned long long int  emuRes;
    unsigned int  class;
    unsigned int flags = 0;
    unsigned long long int  iSrc  = src.u;

    // Classify the input and process "special" values
    scale =  (int) FP64_UNBIASED_EXP(iSrc);
    class = ClassifyType64(src);
    switch(class) {
        case avx3QNan:         RET_DST      (iSrc                     );
        case avx3SNan:         SET_V_RET_DST(FP64_QUIETIZE_NAN(iSrc)  );
        case avx3NegNormal:
        case avx3NegInfinity:  SET_V_RET_DST(FP64_DEFAULT_NAN_AS_UINT64);
        case avx3PosDenorm:
        case avx3PosZero:      SET_Z_RET_DST(FP64_PLUS_INF_AS_UINT64   );
        case avx3NegDenorm:
        case avx3NegZero:      SET_Z_RET_DST(FP64_MINUS_INF_AS_UINT64  );
        case avx3PosInfinity:  RET_DST(      FP64_PLUS_ZERO_AS_UINT64  );
    }
    dst->u = 0;
    if ((iSrc & (FP64_MANT_MASK | FP64_HIDDEN_BIT)) == FP64_HIDDEN_BIT){
        // src is exactly 2^(2k), force result mantissa to 1.
        emuRes     = 0x1ull << 63;
        dst->u |= FP64_PLUS_ONE_AS_UINT64;
    } else {
        // index = bits(52:44), fract = bits(43:22), total = 31
        emuRes = Emulation(dst, (iSrc >> 22) & 0x7fffffff, 0, _rsqrt28Table);
    }
    return EmulationFinishd(dst, emuRes, -FloorDivideByTwo(scale), flags);
}

/*
    ****************************************************************************
    ****************************************************************************
    ************************ EMULATION ROUTINES FOR EXP2 ***********************
    ****************************************************************************
    ****************************************************************************
*/

// These reference functions have to be compiled in Linux with DAZ and FTZ
// off (e.g. with the Intel compiler: icc -c -no-ftz RECIP28EXP2.c). They have to
// be run with the rounding mode set to round-to-nearest, and with
// floating-point exceptions masked.

/* EXAMPLE: icc -no-ftz main.c RECIP28EXP2.c, where main.c is shown below

#include <stdio.h>
typedef union {
    unsigned int u;
    float f;
} type32;
typedef union {
    unsigned long long u;
    double f;
} type64;
extern unsigned int EXP2S (type32 *dst, type32 src);
extern unsigned int EXP2D (type64 *dst, type64 src);

main () {
  type32 dst32, src32;
  type64 dst64, src64;
  unsigned int flags = 0x00000000; // PUOZDI

  printf ("FLAGS = %2.2x\n", flags);
  src32.f = 1.5;
  flags = EXP2S (&dst32, src32);
  printf ("EXP2S(%f = %8.8x HEX) = (%f = %8.8x HEX) flags = %2.2x\n",
      src32.f, src32.u, dst32.f, dst32.u, flags);
  src64.f = 1.5;
  flags = EXP2D (&dst64, src64);
  printf ("EXP2D(%f = %16.16llx HEX) = (%f = %16.16llx HEX) flags = %2.2x\n",
      src64.f, src64.u, dst64.f, dst64.u, flags);
  return (0);
}

*/
// ============================================================================
// Single precision base-2 exponential approximation with less than 2^-23
// relative error.
// Uses Emulation() to produce (at least) a 23-bit approximation to 1/sqrt(x)
// ============================================================================
unsigned int EXP2S(type32 *dst, type32 src) {
    type64  tmp;
    int64_t iTmp;
    uint64_t fixed, emuRes;
    int  scale;
    unsigned int flags = 0;
    unsigned int  iSrc = src.u;

    switch (ClassifyType32(src)) {
        case avx3NegDenorm:
        case avx3PosDenorm:
        case avx3NegZero:
        case avx3PosZero:      RET_DST      (FP32_PLUS_ONE_AS_UINT32 );
        case avx3NegInfinity:  RET_DST      (FP32_PLUS_ZERO_AS_UINT32);
        case avx3PosInfinity:  RET_DST      (FP32_PLUS_INF_AS_UINT32 );
        case avx3QNan:         RET_DST      (iSrc                    );
        case avx3SNan:         SET_V_RET_DST(FP32_QUIETIZE_NAN(iSrc) );
        case avx3PosNormal:    if (iSrc > 0x42ffffff) {
                                   flags |= overflowFlag;
                                   RET_DST  (FP32_PLUS_INF_AS_UINT32 );
                               }
                               break;
        case avx3NegNormal:    // NOTE: denormal results not supported
                               if (iSrc > 0xc2fc0000) {
                                   RET_DST  (0 );
                               }
                               break;
    }
    // Convert 2^24*src to an integer in round-to-nearest mode
    src.u = iSrc + (24 << FP32_EXP_POS);	// src <-- 2^24*src
    flags = _sCvtfl(&iTmp, src);
    fixed = (uint64_t)iTmp;
    scale = (int)SignedBitsShiftRight(fixed, 24);
    tmp.u = 0;
    if ((fixed << 11) == 0 && !(flags & inexactFlag)) {
        // Input was an exact integer, set mantissa result to 1
        emuRes    = 0x1ull << 63;
        tmp.u |= FP64_PLUS_ONE_AS_UINT64;
    } else {
        flags &= ~(unsigned int)inexactFlag;
        // index = bits(23:18) fract = bits(17:0):0000, total of 28 bits
        emuRes = Emulation(&tmp, (fixed << 4) & 0xfffffff,
                            0, _exp223Table);
    }
    return EmulationFinishf (dst, tmp, emuRes, scale, flags);
}

// ============================================================================
// Double precision base-2 exponential approximation with less than 2^-23
// relative error.
// Uses Emulation() to produce (at least) a 23-bit approximation to 1/sqrt(x)
// ============================================================================
unsigned int EXP2D(type64 *dst, type64 src) {
    int64_t rounded;
    uint64_t fixed, emuRes, roundingIncrement;
    int   scale;
    unsigned int  flags = 0;
    unsigned long long int   iSrc  = src.u;
    int   bExp  = (iSrc & FP64_EXP_MASK) >> FP64_EXP_POS;

    switch (ClassifyType64(src)) {
        case avx3NegDenorm:
        case avx3PosDenorm:
        case avx3NegZero:
        case avx3PosZero:      RET_DST      (FP64_PLUS_ONE_AS_UINT64 );
        case avx3NegInfinity:  RET_DST      (FP64_PLUS_ZERO_AS_UINT64);
        case avx3PosInfinity:  RET_DST      (FP64_PLUS_INF_AS_UINT64 );
        case avx3QNan:         RET_DST      (iSrc                    );
        case avx3SNan:         SET_V_RET_DST(FP64_QUIETIZE_NAN(iSrc) );
        case avx3PosNormal:    if (bExp >= (FP64_EXP_BIAS + 10)) {
                                   flags |= overflowFlag;
                                   RET_DST  (FP64_PLUS_INF_AS_UINT64 );
                               }
                               break;
        case avx3NegNormal:    // NOTE: denormal results not supported
                               if (bExp >= (FP64_EXP_BIAS + 10)) {
                                   RET_DST  (0 );
                               }
                               break;
    }
    // Convert 2^24*src to an integer in round-to-nearest mode
    src.u = iSrc + (((unsigned long long int) 24) << FP64_EXP_POS);	// src <-- 2^24*src
    flags = _sCvtdl(&rounded, src);
    fixed = (uint64_t)rounded << 29;
    dst->u = 0;
    scale = (int)SignedBitsShiftRight(fixed, 53);
    if ((fixed << 11) == 0 && !(flags & inexactFlag)) {
        // Input was an exact integer, set mantissa result to 1
        emuRes     = 0x1ull << 63;
        dst->u |= FP64_PLUS_ONE_AS_UINT64;
    } else {
        flags &= ~(unsigned int)inexactFlag;
        // index = bits(52:47) fract = bits(46:25), total of 28 bits
        emuRes = Emulation(dst, (fixed >> 25) & 0xfffffff, 0,
                            _exp223Table);
    }
    // Round the mantissa to single precision
    roundingIncrement = UINT64_C(1) << 39;
    if ((emuRes & (roundingIncrement - 1)) == 0) {
        // 00 --> 0 01 --> 0 10 --> 0 11 --> 1
        if (((emuRes >> 39) & 0x3) != 3) {
            roundingIncrement = 0;
        }
    }
    emuRes = (emuRes + roundingIncrement) & ~UINT64_C(0xffffffffff);
    if (emuRes == 0) {
        emuRes = 0x1ull << 63;
        scale++;
    }
    return EmulationFinishd (dst, emuRes, scale, flags);
}

uint32_t neverd_x86_rcp28_f32(uint32_t source, uint32_t *flags) {
  type32 argument;
  type32 result;
  argument.u = source;
  *flags = RCP28S(&result, argument);
  return result.u;
}

uint64_t neverd_x86_rcp28_f64(uint64_t source, uint32_t *flags) {
  type64 argument;
  type64 result;
  argument.u = source;
  *flags = RCP28D(&result, argument);
  return result.u;
}

uint32_t neverd_x86_rsqrt28_f32(uint32_t source, uint32_t *flags) {
  type32 argument;
  type32 result;
  argument.u = source;
  *flags = RSQRT28S(&result, argument);
  return result.u;
}

uint64_t neverd_x86_rsqrt28_f64(uint64_t source, uint32_t *flags) {
  type64 argument;
  type64 result;
  argument.u = source;
  *flags = RSQRT28D(&result, argument);
  return result.u;
}

uint32_t neverd_x86_exp2_f32(uint32_t source, uint32_t *flags) {
  type32 argument;
  type32 result;
  argument.u = source;
  *flags = EXP2S(&result, argument);
  return result.u;
}

uint64_t neverd_x86_exp2_f64(uint64_t source, uint32_t *flags) {
  type64 argument;
  type64 result;
  argument.u = source;
  *flags = EXP2D(&result, argument);
  return result.u;
}
