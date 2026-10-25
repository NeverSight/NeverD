//===- RegistrationStackOwner.h - Scoped PE32 stack proofs ------*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Keep subsidiary returning-stack queries in their owning callee index.
//===----------------------------------------------------------------------===//

#ifndef NEVERD_IR_LOW_X86_REGISTRATIONSTACKOWNER_H
#define NEVERD_IR_LOW_X86_REGISTRATIONSTACKOWNER_H

#include <cstddef>

namespace neverd {
struct BinaryImage;
class RegistrationCallCalleeIndex;
} // namespace neverd

namespace neverd::registration_abi {

/// Reuse only this immutable-image index and its exact work counter. Other
/// SEH levels and active C++ callee proofs keep their independent queries.
class StackProofOwnerScope {
public:
  StackProofOwnerScope(RegistrationCallCalleeIndex &Index, std::size_t &Work);
  ~StackProofOwnerScope();
  StackProofOwnerScope(const StackProofOwnerScope &) = delete;
  StackProofOwnerScope &operator=(const StackProofOwnerScope &) = delete;

  static RegistrationCallCalleeIndex *find(const BinaryImage &Image,
                                           std::size_t *CumulativeWork);

private:
  RegistrationCallCalleeIndex &Index;
  std::size_t &Work;
  unsigned Level;
  StackProofOwnerScope *Previous;
  static thread_local StackProofOwnerScope *Current;
};

} // namespace neverd::registration_abi

#endif // NEVERD_IR_LOW_X86_REGISTRATIONSTACKOWNER_H
