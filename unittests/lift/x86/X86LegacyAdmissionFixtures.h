//===- X86LegacyAdmissionFixtures.h - Explicit legacy gaps ------*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Independent encoding expectations shared by the focused legacy admission
/// tests and decoder corpus census. This is not the lifter's dispatch table.
//===----------------------------------------------------------------------===//

#ifndef NEVERD_UNITTESTS_LIFT_X86_X86LEGACYADMISSIONFIXTURES_H
#define NEVERD_UNITTESTS_LIFT_X86_X86LEGACYADMISSIONFIXTURES_H

#include "capstone/x86.h"

#include <cstdint>

namespace neverd::test {

struct X86FarPointerEncoding {
  unsigned Id;
  uint8_t Opcode;
  bool Escape;
};
inline constexpr X86FarPointerEncoding X86FarPointerEncodings[] = {
    {X86_INS_LDS, 0xc5, false},
    {X86_INS_LES, 0xc4, false},
    {X86_INS_LSS, 0xb2, true},
    {X86_INS_LFS, 0xb4, true},
    {X86_INS_LGS, 0xb5, true}};

struct X86ThreeDNowEncoding {
  unsigned Id;
  uint8_t Opcode;
};
// AMD APM volume 5: the final opcode selects the packed operation.
inline constexpr X86ThreeDNowEncoding X86ThreeDNowEncodings[] = {
    {X86_INS_PFADD, 0x9e},    {X86_INS_PFACC, 0xae},
    {X86_INS_PFNACC, 0x8a},   {X86_INS_PFPNACC, 0x8e},
    {X86_INS_PFSUB, 0x9a},    {X86_INS_PFSUBR, 0xaa},
    {X86_INS_PFMUL, 0xb4},    {X86_INS_PFMAX, 0xa4},
    {X86_INS_PFMIN, 0x94},    {X86_INS_PFCMPEQ, 0xb0},
    {X86_INS_PFCMPGE, 0x90},  {X86_INS_PFCMPGT, 0xa0},
    {X86_INS_PFRCP, 0x96},    {X86_INS_PFRCPIT1, 0xa6},
    {X86_INS_PFRCPIT2, 0xb6}, {X86_INS_PFRSQIT1, 0xa7},
    {X86_INS_PFRSQRT, 0x97},  {X86_INS_PF2ID, 0x1d},
    {X86_INS_PF2IW, 0x1c},    {X86_INS_PI2FD, 0x0d},
    {X86_INS_PI2FW, 0x0c},    {X86_INS_PSWAPD, 0xbb},
    {X86_INS_PMULHRW, 0xb7},  {X86_INS_PAVGUSB, 0xbf}};

} // namespace neverd::test

#endif // NEVERD_UNITTESTS_LIFT_X86_X86LEGACYADMISSIONFIXTURES_H
