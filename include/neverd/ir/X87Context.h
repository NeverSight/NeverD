//===- X87Context.h - Complete x87 context instruction contracts -*- C++
//-*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// One owner for admitted x87 encodings and complete mutable FP context.
//===----------------------------------------------------------------------===//

#ifndef NEVERD_IR_X87CONTEXT_H
#define NEVERD_IR_X87CONTEXT_H

#include "neverd/ir/intrinsics/Intrinsics.h"
#include "neverd/ir/low/LowIR.h"

#include <optional>
#include <string>

namespace neverd {
inline constexpr unsigned X87ContextBytes = 1024;
inline constexpr unsigned X87ContextHostOffset = 512;
inline constexpr uint32_t X87ContextMemory = 1U << 16;
inline constexpr uint32_t X87ContextOperand16 = 1U << 17;
inline constexpr uint32_t X87ContextWaitPrefix = 1U << 18;
inline constexpr unsigned X87ContextOriginalModRMShift = 19;

/// Whether lifted x86-64 code owns the x87 register stack as one mutable
/// context (the X87Context* intrinsics) instead of keeping typed binary80
/// values on the static stack model. The calling convention \p Convention
/// decides. Win64 never passes an x87 value across a call (MSVC has no
/// binary80 type, and MinGW passes long double by reference), so the stack is
/// private machine state there, which the context preserves bit for bit. The
/// System V ABI passes long double arguments in memory and returns them in
/// st0; they stay typed values so that those parameter and return contracts
/// survive. \p Override replaces the convention's choice
/// (BinaryImage::X87ContextOverride).
constexpr bool x87ContextOwnsStack(Arch A, BinaryFormat Convention,
                                   std::optional<bool> Override) {
  if (A != Arch::X64)
    return false;
  return Override ? *Override : Convention == BinaryFormat::COFF;
}

constexpr bool isX87ContextIntrinsic(Intrinsic Id) {
  return Id == Intrinsic::X87ContextBegin ||
         Id == Intrinsic::X87ContextRefresh ||
         Id == Intrinsic::X87ContextExport || Id == Intrinsic::X87ContextStep ||
         Id == Intrinsic::X87ContextMMXRead ||
         Id == Intrinsic::X87ContextMMXWrite ||
         Id == Intrinsic::X87ContextMMXEnter;
}

constexpr bool x87ContextReturnsValue(Intrinsic Id) {
  return Id == Intrinsic::X87ContextStep || Id == Intrinsic::X87ContextMMXRead;
}

constexpr bool x87ContextSynchronizesExternalState(Intrinsic Id) {
  switch (Id) {
#define NEVERD_X87_EXTERNAL_STATE(ID) case Intrinsic::ID:
#include "neverd/ir/X87StateBoundaries.def"
#undef NEVERD_X87_EXTERNAL_STATE
    return true;
  default:
    return false;
  }
}

struct X87ContextInstruction {
  unsigned MemoryBytes = 0;
  bool ReadsMemory = false, WritesMemory = false;
  bool ReadsFlags = false, WritesFlags = false, WritesAX = false;
  bool Waiting = true;
};

// Memory instructions use the opcode's group, never the original ModRM's
// address bits. Effective-address calculation remains with the x86 lifter.
inline std::optional<X87ContextInstruction>
x87ContextInstruction(uint32_t Selector) {
  if (Selector & ~UINT32_C(0x7ffffff))
    return std::nullopt;
  const unsigned Escape = Selector & 0xff, ModRM = (Selector >> 8) & 0xff;
  X87ContextInstruction Result;
  if (Selector & X87ContextMemory) {
    if (Escape < 0xd8 || Escape > 0xdf || ModRM & ~0x38U)
      return std::nullopt;
    const unsigned Original = (Selector >> X87ContextOriginalModRMShift) & 0xff;
    if ((Original & 0xc0) == 0xc0 || (Original & 0x38) != ModRM)
      return std::nullopt;
    const unsigned Group = ModRM >> 3;
    const bool Short = Selector & X87ContextOperand16;
    if (Escape == 0xd8 || Escape == 0xda || Escape == 0xdc || Escape == 0xde) {
      Result.MemoryBytes = Escape == 0xdc ? 8 : Escape == 0xde ? 2 : 4;
      Result.ReadsMemory = true;
    } else if (Escape == 0xd9) {
      if (Group == 1)
        return std::nullopt;
      Result.MemoryBytes = Group == 4 || Group == 6   ? (Short ? 14 : 28)
                           : Group == 5 || Group == 7 ? 2
                                                      : 4;
      Result.ReadsMemory = Group == 0 || Group == 4 || Group == 5;
      Result.WritesMemory = !Result.ReadsMemory;
      Result.Waiting = Group != 6 && Group != 7;
    } else if (Escape == 0xdb) {
      if (Group == 4 || Group == 6)
        return std::nullopt;
      Result.MemoryBytes = Group == 5 || Group == 7 ? 10 : 4;
      Result.ReadsMemory = Group == 0 || Group == 5;
      Result.WritesMemory = !Result.ReadsMemory;
    } else if (Escape == 0xdd) {
      if (Group == 5)
        return std::nullopt;
      Result.MemoryBytes = Group == 4 || Group == 6 ? (Short ? 94 : 108)
                           : Group == 7             ? 2
                                                    : 8;
      Result.ReadsMemory = Group == 0 || Group == 4;
      Result.WritesMemory = !Result.ReadsMemory;
      Result.Waiting = Group != 6 && Group != 7;
    } else {
      Result.MemoryBytes = Group == 4 || Group == 6 ? 10 : Group >= 5 ? 8 : 2;
      Result.ReadsMemory = Group == 0 || Group == 4 || Group == 5;
      Result.WritesMemory = !Result.ReadsMemory;
    }
  } else {
    if (Selector >> X87ContextOriginalModRMShift)
      return std::nullopt;
    bool Valid = false;
    switch (Escape) {
    case 0x0f:
      Valid = Selector == 0x770f;
      break;
    case 0x9b:
      Valid = ModRM == 0 && Selector == 0x9b;
      break;
    case 0xd8:
      Valid = ModRM >= 0xc0;
      break;
    case 0xd9:
      Valid = (ModRM >= 0xc0 && ModRM <= 0xcf) || ModRM == 0xd0 ||
              ModRM == 0xe0 || ModRM == 0xe1 || ModRM == 0xe4 ||
              ModRM == 0xe5 || (ModRM >= 0xe8 && ModRM <= 0xee) ||
              ModRM >= 0xf0;
      break;
    case 0xda:
      Valid = (ModRM >= 0xc0 && ModRM <= 0xdf) || ModRM == 0xe9;
      break;
    case 0xdb:
      Valid = (ModRM >= 0xc0 && ModRM <= 0xdf) || ModRM == 0xe2 ||
              ModRM == 0xe3 || (ModRM >= 0xe8 && ModRM <= 0xf7);
      Result.Waiting = ModRM != 0xe2 && ModRM != 0xe3;
      break;
    case 0xdc:
      Valid = (ModRM >= 0xc0 && ModRM <= 0xcf) || ModRM >= 0xe0;
      break;
    case 0xdd:
      Valid =
          (ModRM >= 0xc0 && ModRM <= 0xc7) || (ModRM >= 0xd0 && ModRM <= 0xef);
      break;
    case 0xde:
      Valid =
          (ModRM >= 0xc0 && ModRM <= 0xcf) || ModRM == 0xd9 || ModRM >= 0xe0;
      break;
    case 0xdf:
      Valid = (ModRM >= 0xc0 && ModRM <= 0xc7) || ModRM == 0xe0 ||
              (ModRM >= 0xe8 && ModRM <= 0xf7);
      Result.Waiting = ModRM != 0xe0;
      Result.WritesAX = ModRM == 0xe0;
      break;
    }
    if (!Valid || ((Selector & X87ContextOperand16) && Escape == 0x9b))
      return std::nullopt;
    Result.ReadsFlags =
        (Escape == 0xda || Escape == 0xdb) && ModRM >= 0xc0 && ModRM <= 0xdf;
    Result.WritesFlags =
        (Escape == 0xdb || Escape == 0xdf) && ModRM >= 0xe8 && ModRM <= 0xf7;
    Result.ReadsFlags |= Result.WritesFlags; // Faults preserve incoming flags.
  }
  if (Selector & X87ContextWaitPrefix)
    Result.Waiting = true;
  return Result;
}

inline bool x87ContextUpdatesInstructionPointer(uint32_t Selector) {
  const unsigned Escape = Selector & 0xff, ModRM = (Selector >> 8) & 0xff;
  if (Escape == 0x0f)
    return false;
  if (Selector & X87ContextMemory)
    return !(Escape == 0xd9 && (ModRM >> 3) >= 4) &&
           !(Escape == 0xdd && ((ModRM >> 3) == 4 || (ModRM >> 3) >= 6));
  return Escape != 0x9b &&
         !(Escape == 0xdb && (ModRM == 0xe2 || ModRM == 0xe3)) &&
         !(Escape == 0xdf && ModRM == 0xe0);
}

inline bool x87ContextLowShapeIsValid(const LowOp &Op, Arch A) {
  if (A != Arch::Unknown && A != Arch::X64)
    return false;
  if (!Op.NumInputs || !Op.Inputs[0].isConst() || Op.Inputs[0].Size != 2 ||
      Op.MemoryOrdering != NdMemoryOrdering::None ||
      (Op.MemoryAddressSpace != NdMemoryAddressSpace::Default &&
       Op.MemoryAddressSpace != NdMemoryAddressSpace::X86FS &&
       Op.MemoryAddressSpace != NdMemoryAddressSpace::X86GS))
    return false;
  const auto Id = static_cast<Intrinsic>(Op.Inputs[0].Offset);
  const auto Value = [](const NdVar &V) {
    return V.isReg() || V.isTemp() || V.isConst();
  };
  const auto Handle = [](const NdVar &V) {
    return (V.isReg() || V.isTemp()) && V.Size == 8;
  };
  if (Id != Intrinsic::X87ContextStep) {
    if (Op.MemoryAddressSpace != NdMemoryAddressSpace::Default)
      return false;
    if (Id == Intrinsic::X87ContextBegin)
      return Op.NumInputs == 1 && Op.Output.Size == 8 &&
             (Op.Output.isReg() || Op.Output.isTemp());
    if (Id == Intrinsic::X87ContextMMXRead ||
        Id == Intrinsic::X87ContextMMXWrite) {
      const bool Write = Id == Intrinsic::X87ContextMMXWrite;
      return Op.NumInputs == (Write ? 4 : 3) && Handle(Op.Inputs[1]) &&
             Op.Inputs[2].isConst() && Op.Inputs[2].Size == 4 &&
             Op.Inputs[2].Offset < 8 &&
             (Write ? Op.Output.Size == 0 && Value(Op.Inputs[3]) &&
                          Op.Inputs[3].Size == 8
                    : Op.Output.Size == 8 &&
                          (Op.Output.isReg() || Op.Output.isTemp()));
    }
    return (Id == Intrinsic::X87ContextRefresh ||
            Id == Intrinsic::X87ContextExport ||
            Id == Intrinsic::X87ContextMMXEnter) &&
           Op.NumInputs == 2 && Op.Output.Size == 0 && Handle(Op.Inputs[1]);
  }
  if (Op.NumInputs != 6 || !Handle(Op.Inputs[1]) || !Op.Inputs[2].isConst() ||
      Op.Inputs[2].Size != 4 || Op.Inputs[2].Offset > UINT32_MAX ||
      !Value(Op.Inputs[3]) || Op.Inputs[3].Size != 8 || !Value(Op.Inputs[4]) ||
      Op.Inputs[4].Size != 8 || !Op.Inputs[5].isConst() ||
      Op.Inputs[5].Size != 8)
    return false;
  const auto Spec = x87ContextInstruction(Op.Inputs[2].Offset);
  if (!Spec)
    return false;
  if (!(Op.Inputs[2].Offset & X87ContextMemory) &&
      (!Op.Inputs[3].isConst() || Op.Inputs[3].Offset ||
       Op.MemoryAddressSpace != NdMemoryAddressSpace::Default))
    return false;
  const unsigned Bytes = Spec->WritesAX ? 2 : Spec->WritesFlags ? 8 : 0;
  return Op.Output.Size == Bytes &&
         (!Bytes || Op.Output.isReg() || Op.Output.isTemp());
}

/// The context handle of a well-formed X87Context intrinsic: the value
/// X87ContextBegin writes, and the context operand every other one reads.
/// Null for any other operation.
inline const NdVar *x87ContextHandleOperand(const LowOp &Op) {
  if (Op.Opcode != NdOp::INTRINSIC || !Op.NumInputs ||
      !Op.Inputs[0].isConst() ||
      !isX87ContextIntrinsic(static_cast<Intrinsic>(Op.Inputs[0].Offset)) ||
      !x87ContextLowShapeIsValid(Op, Arch::Unknown))
    return nullptr;
  return static_cast<Intrinsic>(Op.Inputs[0].Offset) ==
                 Intrinsic::X87ContextBegin
             ? &Op.Output
             : &Op.Inputs[1];
}
} // namespace neverd
#endif
