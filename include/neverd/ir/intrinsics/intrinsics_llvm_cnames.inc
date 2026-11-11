// LLVM intrinsic name → standard C name mapping.
// Included by intrinsics.cpp.
// The names are aligned in a column, which clang-format would undo.
// clang-format off

// The thread pointer (ARM's __aeabi_read_tp).
{"llvm.thread.pointer.p0",         "__builtin_thread_pointer"},

#include "X86/intrinsics_x86_llvm_cnames.inc"

// Generic LLVM intrinsics
{"llvm.ctpop.i8",                  "__builtin_popcount"},
{"llvm.ctpop.i16",                 "__builtin_popcount"},
{"llvm.ctpop.i32",                 "__builtin_popcount"},
{"llvm.ctpop.i64",                 "__builtin_popcountll"},
{"llvm.ctlz.i32",                  "__builtin_clz"},
{"llvm.ctlz.i64",                  "__builtin_clzll"},
{"llvm.cttz.i32",                  "__builtin_ctz"},
{"llvm.cttz.i64",                  "__builtin_ctzll"},
{"llvm.bswap.i16",                 "__builtin_bswap16"},
{"llvm.bswap.i32",                 "__builtin_bswap32"},
{"llvm.bswap.i64",                 "__builtin_bswap64"},
// Floating-point operations are the compiler's elementwise builtins: each is
// exactly the LLVM intrinsic of its name for whatever width its operands
// have, so a square root sets no errno as C's sqrt may, the four IEEE 754
// choices of the lesser or greater of two numbers keep their own rules for a
// NaN and for zeros of either sign (fmin and fmax leave the zeros open), and
// a fused multiply-add rounds once. None needs a C library header, which a
// build for another target may not have.
{"llvm.fabs.f16",                  "__builtin_elementwise_abs"},
{"llvm.fabs.f32",                  "__builtin_elementwise_abs"},
{"llvm.fabs.f64",                  "__builtin_elementwise_abs"},
// x87 values reach C as long double; this clears only the sign bit, as FABS
// does.
{"llvm.fabs.f80",                  "__builtin_elementwise_abs"},
{"llvm.sqrt.f16",                  "__builtin_elementwise_sqrt"},
{"llvm.sqrt.f32",                  "__builtin_elementwise_sqrt"},
{"llvm.sqrt.f64",                  "__builtin_elementwise_sqrt"},
{"llvm.ceil.f16",                  "__builtin_elementwise_ceil"},
{"llvm.ceil.f32",                  "__builtin_elementwise_ceil"},
{"llvm.ceil.f64",                  "__builtin_elementwise_ceil"},
{"llvm.floor.f16",                 "__builtin_elementwise_floor"},
{"llvm.floor.f32",                 "__builtin_elementwise_floor"},
{"llvm.floor.f64",                 "__builtin_elementwise_floor"},
{"llvm.round.f16",                 "__builtin_elementwise_round"},
{"llvm.round.f32",                 "__builtin_elementwise_round"},
{"llvm.round.f64",                 "__builtin_elementwise_round"},
{"llvm.roundeven.f16",             "__builtin_elementwise_roundeven"},
{"llvm.roundeven.f32",             "__builtin_elementwise_roundeven"},
{"llvm.roundeven.f64",             "__builtin_elementwise_roundeven"},
{"llvm.trunc.f16",                 "__builtin_elementwise_trunc"},
{"llvm.trunc.f32",                 "__builtin_elementwise_trunc"},
{"llvm.trunc.f64",                 "__builtin_elementwise_trunc"},
{"llvm.rint.f16",                  "__builtin_elementwise_rint"},
{"llvm.rint.f32",                  "__builtin_elementwise_rint"},
{"llvm.rint.f64",                  "__builtin_elementwise_rint"},
{"llvm.nearbyint.f16",             "__builtin_elementwise_nearbyint"},
{"llvm.nearbyint.f32",             "__builtin_elementwise_nearbyint"},
{"llvm.nearbyint.f64",             "__builtin_elementwise_nearbyint"},
{"llvm.copysign.f16",              "__builtin_elementwise_copysign"},
{"llvm.copysign.f32",              "__builtin_elementwise_copysign"},
{"llvm.copysign.f64",              "__builtin_elementwise_copysign"},
{"llvm.fma.f16",                   "__builtin_elementwise_fma"},
{"llvm.fma.f32",                   "__builtin_elementwise_fma"},
{"llvm.fma.f64",                   "__builtin_elementwise_fma"},
{"llvm.minnum.f16",                "__builtin_elementwise_minnum"},
{"llvm.minnum.f32",                "__builtin_elementwise_minnum"},
{"llvm.minnum.f64",                "__builtin_elementwise_minnum"},
{"llvm.maxnum.f16",                "__builtin_elementwise_maxnum"},
{"llvm.maxnum.f32",                "__builtin_elementwise_maxnum"},
{"llvm.maxnum.f64",                "__builtin_elementwise_maxnum"},
{"llvm.minimum.f16",               "__builtin_elementwise_minimum"},
{"llvm.minimum.f32",               "__builtin_elementwise_minimum"},
{"llvm.minimum.f64",               "__builtin_elementwise_minimum"},
{"llvm.maximum.f16",               "__builtin_elementwise_maximum"},
{"llvm.maximum.f32",               "__builtin_elementwise_maximum"},
{"llvm.maximum.f64",               "__builtin_elementwise_maximum"},
// FJCVTZS, the JavaScript conversion of a double to a 32-bit integer.
{"llvm.aarch64.fjcvtzs",           "__builtin_arm_jcvt"},
// A CRC step is the compiler's builtin on either ARM, which takes the data
// as the instruction does and needs the target feature alone, no header.
{"llvm.arm.crc32b",                "__builtin_arm_crc32b"},
{"llvm.arm.crc32h",                "__builtin_arm_crc32h"},
{"llvm.arm.crc32w",                "__builtin_arm_crc32w"},
{"llvm.arm.crc32cb",               "__builtin_arm_crc32cb"},
{"llvm.arm.crc32ch",               "__builtin_arm_crc32ch"},
{"llvm.arm.crc32cw",               "__builtin_arm_crc32cw"},
{"llvm.aarch64.crc32b",            "__builtin_arm_crc32b"},
{"llvm.aarch64.crc32h",            "__builtin_arm_crc32h"},
{"llvm.aarch64.crc32w",            "__builtin_arm_crc32w"},
{"llvm.aarch64.crc32x",            "__builtin_arm_crc32d"},
{"llvm.aarch64.crc32cb",           "__builtin_arm_crc32cb"},
{"llvm.aarch64.crc32ch",           "__builtin_arm_crc32ch"},
{"llvm.aarch64.crc32cw",           "__builtin_arm_crc32cw"},
{"llvm.aarch64.crc32cx",           "__builtin_arm_crc32cd"},
// SHA-1's fixed rotation of one word, as arm_neon.h names it.
{"llvm.arm.neon.sha1h",            "vsha1h_u32"},
{"llvm.aarch64.crypto.sha1h",      "vsha1h_u32"},
{"llvm.bitreverse.i8",             "__rbit"},
{"llvm.bitreverse.i16",            "__rbit"},
{"llvm.bitreverse.i32",            "__rbit"},
{"llvm.bitreverse.i64",            "__rbitll"},
{"llvm.readcyclecounter",          "__rdtsc"},
{"llvm.debugtrap",                 "__debugbreak"},
{"llvm.trap",                      "__builtin_trap"},
