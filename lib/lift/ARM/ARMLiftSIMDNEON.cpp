//===- ARMLiftSIMDNEON.cpp - ARM32 NEON instruction lifter ----------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// NEON (Advanced SIMD) instruction handlers for ARM32: vector load/store,
/// arithmetic, logic, shifts, conversions, table lookups, zip/unzip, and
/// transpose.
///
//===----------------------------------------------------------------------===//

#include "ARMLiftDetail.h"

#include "neverd/ir/intrinsics/Intrinsics.h"
#include "neverd/lift/ARMLifter.h"

#include "llvm/ADT/StringExtras.h"
#include "llvm/ADT/StringRef.h"
#include "llvm/Support/Debug.h"
#include "llvm/Support/raw_ostream.h"

#define DEBUG_TYPE "neverd-lift-arm"

namespace neverd {

NeonLaneInfo getNeonLaneInfo(arm_vectordata_type VD) {
  NeonLaneInfo I;
  switch (VD) {
  case ARM_VECTORDATA_S8:
    I.LaneSz = 1;
    I.IsSigned = true;
    break;
  case ARM_VECTORDATA_U8:
  case ARM_VECTORDATA_I8:
    I.LaneSz = 1;
    break;
  case ARM_VECTORDATA_S16:
    I.LaneSz = 2;
    I.IsSigned = true;
    break;
  case ARM_VECTORDATA_U16:
  case ARM_VECTORDATA_I16:
    I.LaneSz = 2;
    break;
  case ARM_VECTORDATA_S32:
    I.LaneSz = 4;
    I.IsSigned = true;
    break;
  case ARM_VECTORDATA_U32:
  case ARM_VECTORDATA_I32:
    I.LaneSz = 4;
    break;
  case ARM_VECTORDATA_S64:
    I.LaneSz = 8;
    I.IsSigned = true;
    break;
  case ARM_VECTORDATA_U64:
  case ARM_VECTORDATA_I64:
    I.LaneSz = 8;
    break;
  case ARM_VECTORDATA_F32:
    I.LaneSz = 4;
    I.IsFloat = true;
    break;
  case ARM_VECTORDATA_F64:
    I.LaneSz = 8;
    I.IsFloat = true;
    break;
  default:
    break;
  }
  return I;
}

// Capstone leaves `vector_data` == ARM_VECTORDATA_INVALID for several NEON
// instructions (notably VTST), so the only reliable element-width source is the
// mnemonic suffix (".8"/".16"/".i16"/".s32"/".u8"/".f32").  Parse it.
NeonLaneInfo getNeonLaneInfoFromMnemonic(const char *Mnem) {
  NeonLaneInfo I;
  if (!Mnem)
    return I;
  llvm::StringRef M(Mnem);
  size_t Dot = M.rfind('.');
  if (Dot == llvm::StringRef::npos)
    return I;
  llvm::StringRef Suf = M.substr(Dot + 1);
  if (!Suf.empty()) {
    char C = Suf[0];
    if (C == 's')
      I.IsSigned = true;
    else if (C == 'f')
      I.IsFloat = true;
    if (C == 'i' || C == 's' || C == 'u' || C == 'f' || C == 'p')
      Suf = Suf.drop_front();
  }
  unsigned Bits = 0;
  if (!Suf.getAsInteger(10, Bits) && Bits >= 8)
    I.LaneSz = Bits / 8;
  return I;
}

// Element info from vector_data, falling back to the mnemonic suffix when
// capstone did not populate vector_data.
NeonLaneInfo getNeonLaneInfo(arm_vectordata_type VD, const char *Mnem) {
  NeonLaneInfo I = getNeonLaneInfo(VD);
  if (I.LaneSz == 0)
    I = getNeonLaneInfoFromMnemonic(Mnem);
  return I;
}

bool ARMLifter::liftSIMDNEON(LiftState &S, const cs_insn *Insn,
                             const cs_arm &ARM) {
  return liftSIMDNEONLoadStore(*this, S, Insn, ARM) ||
         liftSIMDNEONLogic(*this, S, Insn, ARM) ||
         liftSIMDNEONArith(*this, S, Insn, ARM) ||
         liftSIMDNEONMul(*this, S, Insn, ARM) ||
         liftSIMDNEONCompare(*this, S, Insn, ARM) ||
         liftSIMDNEONConvert(*this, S, Insn, ARM) ||
         liftSIMDNEONPermute(*this, S, Insn, ARM) ||
         liftSIMDNEONShift(*this, S, Insn, ARM) ||
         liftSIMDNEONSat(*this, S, Insn, ARM) ||
         liftSIMDNEONMisc(*this, S, Insn, ARM);
}

} // namespace neverd
