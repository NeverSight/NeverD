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
  bool Multiply = false;
  bool Atomic = false;
  bool Compressed = false;
};

} // namespace neverd

#endif // NEVERD_LIFT_RISCV_RISCVPROFILE_H
