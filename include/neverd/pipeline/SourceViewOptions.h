//===- SourceViewOptions.h - Optional source transformations --*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Request-local source transformations, independent of machine analysis and
/// the optimizer used for lifting, translation or patching.
//===----------------------------------------------------------------------===//

#ifndef NEVERD_PIPELINE_SOURCEVIEWOPTIONS_H
#define NEVERD_PIPELINE_SOURCEVIEWOPTIONS_H

#include "neverd/ir/SourceCallEdits.h"
#include "neverd/ir/SourceStackFrame.h"
#include "neverd/ir/SourceVariableEdits.h"
#include "neverd/ir/X86/Win64PrecallMemory.h"

#include <cstdint>
#include <string>
#include <vector>

namespace neverd {

struct SourceViewOptions {
  struct VariableType {
    uint64_t Function = 0;
    uint64_t Address = 0;
    uint16_t Bytes = 0;
    bool Floating = false;
    std::string Id;
    std::string Snapshot;
    std::string Type;
    bool operator==(const VariableType &) const = default;
  };
  enum class LLVMOptimization { Basic, O0, O1, O2, O3, Custom };
  enum Pass : uint32_t {
    InstCombine = 1,
    EarlyCSE = 2,
    SimplifyCFG = 4,
    DCE = 8,
  };
  static constexpr uint32_t BasicPasses =
      InstCombine | EarlyCSE | SimplifyCFG | DCE;

  /// Exact MBA derivations plus bounded, full-width-proved semantic synthesis
  /// in both HighC and LLVM-to-C. CLI and GUI requests share this switch.
  bool Symbolic = false;
  bool RecoverControlFlow = true;
  LLVMOptimization LLVM = LLVMOptimization::O2;
  uint32_t Passes = BasicPasses;
  /// Analyst assertions belong only to this source projection. They never
  /// enter the image, machine IR or native ABI recovery.
  std::vector<VariableType> VariableTypes;
  std::vector<SourceVariableEdit> VariableEdits;
  std::vector<SourceCallEdit> CallTypes;
  SourceStackFrame StackFrame;
  Win64PrecallMemory PrecallMemory = Win64PrecallMemory::None;
  bool operator==(const SourceViewOptions &) const = default;
};

} // namespace neverd

#endif // NEVERD_PIPELINE_SOURCEVIEWOPTIONS_H
