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
{"llvm.fabs.f32",                  "fabsf"},
{"llvm.fabs.f64",                  "fabs"},
{"llvm.sqrt.f32",                  "sqrtf"},
{"llvm.sqrt.f64",                  "sqrt"},
{"llvm.ceil.f32",                  "ceilf"},
{"llvm.ceil.f64",                  "ceil"},
{"llvm.floor.f32",                 "floorf"},
{"llvm.floor.f64",                 "floor"},
{"llvm.round.f32",                 "roundf"},
{"llvm.round.f64",                 "round"},
{"llvm.bitreverse.i8",             "__rbit"},
{"llvm.bitreverse.i16",            "__rbit"},
{"llvm.bitreverse.i32",            "__rbit"},
{"llvm.bitreverse.i64",            "__rbitll"},
{"llvm.readcyclecounter",          "__rdtsc"},
{"llvm.debugtrap",                 "__debugbreak"},
{"llvm.trap",                      "__builtin_trap"},
