//===- X87PortableExecution.h - Host-owned portable call context -*- C++
//-*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Trusted lifetime and current-instruction result ownership for generated
/// calls.
//===----------------------------------------------------------------------===//

#ifndef NEVERD_TRANSLATE_X86_X87PORTABLEEXECUTION_H
#define NEVERD_TRANSLATE_X86_X87PORTABLEEXECUTION_H

#include "neverd/runtime/X87PortableRuntimeC.h"
#include "neverd/translate/X86/X87PortableRuntime.h"

namespace neverd::translate {

/// Trusted embedding state for one confined execution thread. The state and
/// memory owners must outlive this context. Generated code receives only its
/// opaque address; its memory addresses never alias this object or its owners.
class PortableX87ExecutionV1 final {
public:
  PortableX87ExecutionV1(PortableX87StateV1 &State, GuestMemoryRuntime &Memory)
      : State(State), Memory(Memory) {}
  PortableX87ExecutionV1(const PortableX87ExecutionV1 &) = delete;
  PortableX87ExecutionV1 &operator=(const PortableX87ExecutionV1 &) = delete;

  const PortableX87StepResultV1 &lastStep() const { return LastStep; }
  uint64_t lastInstructionAddress() const { return LastIP; }
  uint64_t lastDataAddress() const { return LastDP; }

private:
  friend auto ::nvd_x87_v1_step(void *, uint32_t, uint64_t, uint64_t) noexcept
      -> uint32_t;
  friend auto ::nvd_x87_v1_result(void *) noexcept -> uint64_t;

  PortableX87StateV1 &State;
  GuestMemoryRuntime &Memory;
  PortableX87StepResultV1 LastStep;
  uint64_t LastIP = 0;
  uint64_t LastDP = 0;
  bool ResultAvailable = false;
};

} // namespace neverd::translate

#endif // NEVERD_TRANSLATE_X86_X87PORTABLEEXECUTION_H
