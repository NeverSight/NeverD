//===- RegistrationSEH4PhysicalFrame.h -------------------------*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Keep the original source function separate from physical helper expansion.
//===----------------------------------------------------------------------===//

#ifndef NEVERD_IR_MED_X86_REGISTRATIONSEH4PHYSICALFRAME_H
#define NEVERD_IR_MED_X86_REGISTRATIONSEH4PHYSICALFRAME_H

#include "neverd/ir/low/LowIR.h"
#include "neverd/ir/med/X86/X86RegistrationPhysicalFrame.h"
#include "neverd/ir/med/X86RegistrationSEH4PhysicalFrame.h"
#include "neverd/loader/COFF/COFFSEH4Entry.h"

#include <memory>
#include <optional>
#include <vector>

namespace neverd {
namespace x86_registration {
struct SEH4PhysicalProjection {
  LowFunc Function;
  std::shared_ptr<const X86RegistrationSEH4PhysicalFrame> Frame;
};
std::optional<SEH4PhysicalProjection>
projectSEH4PhysicalFrame(const BinaryImage &Image, const LowFunc &Source);
} // namespace x86_registration

class X86RegistrationSEH4PhysicalFrame {
  RegistrationSEH4PrologueEffect Entry;
  coff_loader::X86SEH4PrologueCall EntryCall;
  std::vector<X86RegistrationPhysicalExit> Exits;
  RegistrationStateAnalysis States;

  friend std::optional<x86_registration::SEH4PhysicalProjection>
  x86_registration::projectSEH4PhysicalFrame(const BinaryImage &,
                                             const LowFunc &);
  friend const RegistrationStateAnalysis *
  getX86RegistrationSEHPhysicalStates(const MedFunc &);
  friend bool hasCheckedX86RegistrationSEHPhysicalFrame(const BinaryImage &,
                                                        const MedFunc &);
  friend std::optional<uint64_t>
  getX86RegistrationSEHFrameStorage(const MedFunc &);
};

} // namespace neverd

#endif // NEVERD_IR_MED_X86_REGISTRATIONSEH4PHYSICALFRAME_H
