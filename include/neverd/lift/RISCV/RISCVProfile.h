//===- RISCVProfile.h - RISC-V integer extension evidence -------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// An image with no extension evidence admits only the base I instruction set.
///
//===----------------------------------------------------------------------===//

#ifndef NEVERD_LIFT_RISCV_RISCVPROFILE_H
#define NEVERD_LIFT_RISCV_RISCVPROFILE_H

namespace neverd {

struct RISCVProfile {
#define NEVERD_RISCV_BASE(Name, Major, MaxMinor)                               \
  static constexpr char BaseFeature[] = "+" Name;
#define NEVERD_RISCV_EXTENSION(Member, ...) bool Member = false;
#include "neverd/lift/RISCV/RISCVExtensions.def"

  bool hasCompressed() const { return Compressed || Zca; }
};

} // namespace neverd

#endif // NEVERD_LIFT_RISCV_RISCVPROFILE_H
