//===- COFFUnwindARMDetail.h - Private ARM unwind helpers -------*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Implementation detail of the ARM and ARM64 unwind decoders.  The register
/// numbers and operation builders here are shared between the
/// `COFFUnwindARM*.cpp` translation units under `lib/loader/COFF/unwind` and
/// are not part of any public interface; nothing outside that directory may
/// include this header.
///
//===----------------------------------------------------------------------===//

#ifndef NEVERD_LIB_LOADER_COFF_UNWIND_COFFUNWINDARMDETAIL_H
#define NEVERD_LIB_LOADER_COFF_UNWIND_COFFUNWINDARMDETAIL_H

#include "neverd/loader/COFF/COFFUnwindARM.h"

#include "llvm/Support/Compiler.h"

#include <cstddef>
#include <cstdint>
#include <string>
#include <utility>

namespace neverd::coff_loader {
namespace LLVM_LIBRARY_VISIBILITY_NAMESPACE arm_unwind_detail {

/// ARM64 register numbers the unwind codes name relative to a fixed base.
inline constexpr uint16_t kFirstSavedIntReg = 19; // x19
inline constexpr uint16_t kFramePointer = 29;     // x29
inline constexpr uint16_t kLinkRegister = 30;     // x30, spelled `lr`
inline constexpr uint16_t kFirstSavedFPReg = 8;   // d8

/// ARM32 register numbers.
inline constexpr uint16_t kARMFramePointer = 11; // r11
inline constexpr uint16_t kARMStackPointer = 13; // r13, spelled `sp`
inline constexpr uint16_t kARMLinkRegister = 14; // r14, spelled `lr`

/// Every ARM64 instruction is one word, and every unwind code that stands
/// against an instruction therefore stands against four bytes.
inline constexpr uint8_t kARM64InstructionSize = 4;

/// A register number is only representable in \ref UnwindOperation's mask if
/// it fits the 32 bits the mask has.  Every register file these codes name is
/// smaller than that, so a number that does not fit came from a malformed
/// code rather than from a register that exists.
bool isMaskableRegister(uint16_t Reg);

void addRegister(UnwindOperation &Op, uint16_t Reg);

/// Record that \p Op acts on the single register \p Reg of \p Class.
void setRegister(UnwindOperation &Op, UnwindRegisterClass Class, uint16_t Reg);

/// Record that \p Op acts on \p First and \p Second, which need not be
/// adjacent: `save_lrpair` pairs a callee-saved register with `lr`.
void setRegisterPair(UnwindOperation &Op, UnwindRegisterClass Class,
                     uint16_t First, uint16_t Second);

/// Record that \p Op acts on the inclusive range [\p First, \p Last].
void setRegisterRange(UnwindOperation &Op, UnwindRegisterClass Class,
                      uint16_t First, uint16_t Last);

/// Accumulates operations and the running prologue length.
class DecodeBuilder {
public:
  explicit DecodeBuilder(ARMUnwindDecode &Result) : Result(Result) {}

  UnwindOperation &add(UnwindOperationKind Kind, uint32_t CodeOffset,
                       uint8_t CodeLength, uint8_t InstructionSize) {
    UnwindOperation Op;
    Op.Kind = Kind;
    Op.CodeOffset = CodeOffset;
    Op.SlotCount = CodeLength;
    Op.InstructionSize = InstructionSize;
    if (CountsTowardPhysicalPrologue)
      Result.PrologueSize += InstructionSize;
    Result.Operations.push_back(std::move(Op));
    return Result.Operations.back();
  }

  /// Operations following `end_c` belong to a parent/phantom scope.  They are
  /// required to unwind the body, but they do not stand against instructions
  /// in the current fragment's physical prologue.
  void enterChainedParentScope() { CountsTowardPhysicalPrologue = false; }

  void degrade(ExceptionParseStatus Status, std::string Message) {
    Result.Status = mergeExceptionParseStatus(Result.Status, Status);
    Result.Diagnostics.push_back(std::move(Message));
  }

private:
  ARMUnwindDecode &Result;
  bool CountsTowardPhysicalPrologue = true;
};

std::string atOffset(const char *What, size_t Offset);

} // namespace LLVM_LIBRARY_VISIBILITY_NAMESPACE arm_unwind_detail
} // namespace neverd::coff_loader

#endif // NEVERD_LIB_LOADER_COFF_UNWIND_COFFUNWINDARMDETAIL_H
