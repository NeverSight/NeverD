//===- RegistrationRoots.h - x86 SSA runtime definitions --------*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#ifndef NEVERD_IR_MED_X86_REGISTRATIONROOTS_H
#define NEVERD_IR_MED_X86_REGISTRATIONROOTS_H

#include "neverd/ir/med/MedIR.h"

#include <map>
#include <optional>

namespace neverd::x86_registration {

/// Classification and source-proved runtime coordinates for SSA live-ins.
/// Ordinary module entries retain their independent calling convention.
class RegistrationRoots {
public:
  RegistrationRoots(const LowFunc &Low, Arch Architecture, BinaryFormat Format);

  bool hasSEHFrame() const { return HasSEHFrame; }
  bool hasFrame() const { return HasSEHFrame || HasCxxFrame; }
  /// Remove ordinary fallthrough only at an exact, checked no-return source
  /// call. Keep exceptional edges and continuation blocks for SSA rooting.
  void disconnectNoReturnFallthroughs(MedFunc &Func) const;
  bool isCxxHandler(const MedBlock &Block) const;
  std::optional<int32_t> restoredStackOffset(const MedBlock &Block) const;
  void initialize(MedOp &Op, bool RuntimeRoot, bool Callback,
                  std::optional<int32_t> RestoredSP) const;

private:
  const LowFunc &Low;
  bool HasSEHFrame = false;
  bool HasCxxFrame = false;
  bool Direct = false;
  bool Realigned = false;
  std::map<va_t, std::optional<int32_t>> RestoredStacks;
};

} // namespace neverd::x86_registration

#endif // NEVERD_IR_MED_X86_REGISTRATIONROOTS_H
