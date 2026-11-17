//===- DebugParamBinding.h - Debug parameters by location -------*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Which parameter of a function's debug signature each recovered parameter
/// holds, matched by where the convention passes each
/// (SourceParameterPlacement), never by position: System V passes
/// `f(double x, int n)` with n in the first integer register.  Without
/// rules or locations, only an integer prefix binds by position, as in
/// every convention.  HighC and LLVM-C share the binding; an MSVC indirect
/// result keeps their own positions.
///
//===----------------------------------------------------------------------===//

#ifndef NEVERD_LIB_BACKEND_C_DEBUGPARAMBINDING_H
#define NEVERD_LIB_BACKEND_C_DEBUGPARAMBINDING_H

#include "neverd/Common.h"
#include "neverd/debug/DebugContext.h"
#include "neverd/ir/med/MedIR.h"

#include "llvm/ADT/ArrayRef.h"

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace neverd {

/// Where a recovered parameter arrives.
struct RecoveredParamSite {
  /// Its register, or kNoParamReg for a stack argument.
  uint64_t RegOff = kNoParamReg;
  /// Its MedIR parameter index, which numbers a stack argument's slot.
  int MedIndex = -1;
  /// The bytes it holds.
  uint16_t Bytes = 0;
};

/// What a recovered parameter holds of its function's debug signature.
struct DebugParamBinding {
  /// The debug parameter it holds a piece of, or -1.
  int Index = -1;
  /// Where the piece starts in that parameter's value.
  uint16_t Offset = 0;
  /// It holds the whole value the debug type describes.
  bool Whole = false;
  /// It holds the address of a copy of the value, which the convention
  /// passes by reference.
  bool Indirect = false;
  /// It holds the hidden pointer to the result's storage.
  bool Result = false;
};

struct DebugParamBindings {
  /// One per recovered parameter.
  std::vector<DebugParamBinding> Params;
  /// The convention's rules placed the parameters; else at most an integer
  /// prefix bound by position.
  bool Placed = false;
};

/// Where the stack argument \p Index of \p Sites arrives, from the entry
/// stack pointer; none for a register argument or an unknown location.
std::optional<int64_t>
stackParamOffset(Arch A, BinaryFormat F,
                 llvm::ArrayRef<RecoveredParamSite> Sites, size_t Index);

/// Binds \p Signature's parameters to the recovered parameters at \p Sites,
/// completing the signature's types through \p Dbg.  A function of a
/// language with conventions of its own, such as Go, binds none.
DebugParamBindings
bindDebugParams(Arch A, BinaryFormat F, const FunctionSym &Signature,
                llvm::ArrayRef<RecoveredParamSite> Sites,
                const DebugContext *Dbg,
                InstructionMode EntryMode = InstructionMode::Default);

/// The name a recovered parameter reads as by \p Binding: `result`, the
/// debug parameter's, `<name>_<offset>` for a later piece of one, or empty.
std::string debugParamName(const FunctionSym &Signature,
                           const DebugParamBinding &Binding);

} // namespace neverd

#endif // NEVERD_LIB_BACKEND_C_DEBUGPARAMBINDING_H
