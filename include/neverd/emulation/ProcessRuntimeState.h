//===- ProcessRuntimeState.h - Owned stopped OS state ------------*- C++
//-*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
#ifndef NEVERD_EMULATION_PROCESSRUNTIMESTATE_H
#define NEVERD_EMULATION_PROCESSRUNTIMESTATE_H

namespace neverd::emulation {
/// A profile-owned immutable state description. The generic observer transports
/// this record without interpreting a guest system's objects or byte layouts.
class ProcessRuntimeState {
public:
  enum class Kind { WindowsPE64, WindowsDriverX64 };
  explicit ProcessRuntimeState(Kind Profile) : Profile(Profile) {}
  virtual ~ProcessRuntimeState() = default;
  /// Profile-owned resources beyond heap-reference, encoding and dynamic
  /// local-slot inventories require reconstruction even without an integer
  /// reference in the captured image.
  virtual bool hasAdditionalDependencies() const = 0;
  const Kind Profile;
};
} // namespace neverd::emulation
#endif
