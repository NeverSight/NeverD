//===- X64CallRCXNative.h - Win64 native call assembly ---------*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Shared assembly and register ownership for both source routes. RSP is an
/// explicit clobber, forcing a stable compiler frame; R12 holds the original
/// stack across the ordinary Win64 callee. Explicit alignment also covers
/// compiler leaf frames that were not aligned for a C call. The unwind flag
/// is mandatory, including in generated C's clobber list.
//===----------------------------------------------------------------------===//

#ifndef NEVERD_BACKEND_LLVM_X86_X64CALLRCXNATIVE_H
#define NEVERD_BACKEND_LLVM_X86_X64CALLRCXNATIVE_H

#include "neverd/ir/intrinsics/X86/X64CallRCX.h"

namespace neverd {
inline constexpr const char *X64CallRCXProfileAttribute = "neverd.win64.rcx";
inline constexpr const char *X64CallRCXTargetCPU = "x86-64";
inline constexpr const char *X64CallRCXTargetFeatures = "-avx,-avx2,-avx512f";
inline constexpr const char *X64CallRCXAsm = "# neverd.win64.call.rcx.v1\n\t"
                                             "movq %rsp, %r12\n\t"
                                             "andq $$-16, %rsp\n\t"
                                             "subq $$32, %rsp\n\t"
                                             "callq *%rax\n\t"
                                             "movq %r12, %rsp";
inline constexpr const char *X64CallRCXConstraints =
    "={ax},={cx},={dx},={r8},={r9},0,1,2,3,4,"
    "~{rsp},~{r12},~{r10},~{r11},~{xmm0},~{xmm1},~{xmm2},"
    "~{xmm3},~{xmm4},~{xmm5},~{st},~{st(1)},~{st(2)},~{st(3)},"
    "~{st(4)},~{st(5)},~{st(6)},~{st(7)},~{mm0},~{mm1},~{mm2},~{mm3},"
    "~{mm4},~{mm5},~{mm6},~{mm7},~{dirflag},~{fpsr},~{flags},~{memory}";
inline constexpr const char *X64CallRCXCClobbers =
    "\"rsp\", \"r12\", \"r10\", \"r11\", \"xmm0\", \"xmm1\", "
    "\"xmm2\", \"xmm3\", \"xmm4\", \"xmm5\", \"st\", \"st(1)\", "
    "\"st(2)\", \"st(3)\", \"st(4)\", \"st(5)\", \"st(6)\", \"st(7)\", "
    "\"mm0\", \"mm1\", \"mm2\", \"mm3\", \"mm4\", \"mm5\", \"mm6\", "
    "\"mm7\", \"cc\", \"memory\", \"unwind\"";
inline constexpr const char *X64CallRCXCompilerContract =
    "#ifndef NEVERD_X64_CALL_RCX\n"
    "#define NEVERD_X64_CALL_RCX 1\n"
    "#if !defined(_WIN32) || !defined(__x86_64__) || !defined(__clang__) || "
    "!defined(__EXCEPTIONS)\n"
    "#error \"Native Win64 register results require Windows x64 Clang and "
    "-fexceptions\"\n"
    "#endif\n"
    "#endif\n";
} // namespace neverd

#endif // NEVERD_BACKEND_LLVM_X86_X64CALLRCXNATIVE_H
