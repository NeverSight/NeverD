//===- ProcessRuntimeState.h - Owned stopped OS state ------------*- C++
//-*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
#ifndef NEVERD_EMULATION_PROCESSRUNTIMESTATE_H
#define NEVERD_EMULATION_PROCESSRUNTIMESTATE_H

#include <string>
#include <vector>

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
  /// Profile-owned explanations, when available. An empty list does not
  /// override the dependency flag or certify that a profile can restore state.
  virtual std::vector<std::string> additionalDependencyReasons() const {
    return {};
  }
  const Kind Profile;
};
} // namespace neverd::emulation
#endif
