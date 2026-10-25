//===- SourceStackFrame.h - Entry-relative stack observations -*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Inspect retained memory accesses without granting runtime address or
/// execution-environment evidence.
//===----------------------------------------------------------------------===//

#ifndef NEVERD_IR_SOURCESTACKFRAME_H
#define NEVERD_IR_SOURCESTACKFRAME_H

#include "neverd/ir/low/LowIR.h"

#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <vector>

namespace neverd {
/// Analyst assertions are entry-SP coordinates, never machine lift evidence.
struct SourceStackFrame {
  struct Variable {
    int64_t Offset = 0;
    uint16_t Bytes = 0;
    std::string Name;
    std::string Type;
    bool operator==(const Variable &) const = default;
  };
  std::string Snapshot;
  std::string InputSHA256;
  std::map<va_t, int64_t> Points;
  std::vector<Variable> Variables;
  bool operator==(const SourceStackFrame &) const = default;
  bool empty() const { return Points.empty() && Variables.empty(); }
};
struct StackFrameAccess {
  va_t Instruction = 0;
  int Sequence = 0;
  int64_t Offset = 0;
  uint16_t Bytes = 0;
  bool Write = false;
  /// Original AArch64 memory encoding, when the access is an opaque intrinsic.
  /// Offset describes its nominal entry-SP coordinate, not a no-fault proof.
  std::optional<uint32_t> OriginalWord = std::nullopt;
};
struct StackFrameInstruction {
  std::optional<int64_t> Before, After;
  bool Correctable = false;
};
struct StackFrameAnalysis {
  std::map<va_t, StackFrameInstruction> Instructions;
  std::vector<StackFrameAccess> Accesses;
  std::string Error;
};
/// Bounded must-analysis. Unknown is not zero. No memory or callee guesses.
StackFrameAnalysis analyzeSourceStackFrame(const LowFunc &, Arch,
                                           const SourceStackFrame & = {});
/// Private source interpretation only; refuses roots without an entry carrier.
std::optional<LowFunc> projectSourceStackFrame(const LowFunc &, Arch,
                                               const SourceStackFrame &,
                                               std::string &Error);
} // namespace neverd

#endif // NEVERD_IR_SOURCESTACKFRAME_H
