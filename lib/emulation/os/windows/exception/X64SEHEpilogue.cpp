//===- X64SEHEpilogue.cpp - x64 V1/V2 epilogue unwind ---------------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Validate the V1 instruction grammar or normalized V2 restore positions
/// before reading stack storage.
/// Simulate only the remaining instructions; prologue unwind codes describe
/// the full frame and cannot recover an already partially restored frame.
///
//===----------------------------------------------------------------------===//

#include "X64SEH.h"

#include "neverd/emulation/Registers.h"

#include "capstone/capstone.h"
#include "llvm/ADT/ScopeExit.h"

#include <algorithm>
#include <array>
#include <vector>

namespace neverd::emulation {
namespace {
llvm::Error invalid(const char *Message) {
  return llvm::createStringError(llvm::inconvertibleErrorCode(),
                                 seh::text::EpiloguePrefix +
                                     llvm::Twine(Message));
}

std::optional<unsigned> registerIndex(x86_reg Register) {
  switch (Register) {
#define NEVERD_X64_REGISTER(Name, DecoderID, BackendID)                        \
  case DecoderID:                                                              \
    return unsigned(X64Register::Name);
#include "neverd/emulation/arch/x86_64/X64Registers.def"
#undef NEVERD_X64_REGISTER
  default:
    return {};
  }
}

struct Restore {
  enum class Kind { AddStack, FrameStack, Pop } Operation;
  unsigned Register = 0;
  int64_t Offset = 0;
};
using RestorePlan = std::optional<std::vector<Restore>>;

llvm::Expected<RestorePlan> planV2Restores(const ExceptionFunction &Frame,
                                           const ExceptionFunction &Primary,
                                           uint64_t ControlOffset) {
  const auto Region = std::find_if(
      Frame.Epilogs.begin(), Frame.Epilogs.end(), [&](const UnwindEpilog &E) {
        return ControlOffset >= uint64_t(E.StartOffset) &&
               ControlOffset - uint64_t(E.StartOffset) <=
                   E.LastInstructionOffset;
      });
  if (Region == Frame.Epilogs.end())
    return RestorePlan{};

  const uint64_t Position = ControlOffset - uint64_t(Region->StartOffset);
  const auto &Operations = Primary.UnwindOperations;
  auto FirstPush = std::find_if(
      Operations.begin(), Operations.end(), [](const UnwindOperation &Op) {
        return Op.Kind == UnwindOperationKind::PushNonVolatile;
      });
  uint32_t Offset = 0;
  bool AtInstruction = false;
  std::vector<Restore> Restores;
  const auto Append = [&](Restore Step, unsigned Width) {
    AtInstruction |= Position == Offset;
    if (Position <= Offset)
      Restores.push_back(Step);
    Offset += Width;
  };
  auto I = FirstPush;
  for (;
       I != Operations.end() && I->Kind == UnwindOperationKind::PushNonVolatile;
       ++I)
    Append({Restore::Kind::Pop, I->Register}, I->Register >= 8 ? 2 : 1);
  // A trailing eight-byte allocation can describe PUSHFQ, discarded by a
  // one-byte volatile POP. Windows leaves volatile register values alone.
  if (I != Operations.end() && I->Kind == UnwindOperationKind::AllocateSmall &&
      I->StackOffset == seh::PointerSize) {
    Append({Restore::Kind::AddStack, 0, seh::PointerSize}, 1);
    ++I;
  }
  if (I != Operations.end())
    return invalid(seh::text::UnsupportedV2EpilogueRestores);
  AtInstruction |= Position == Offset;
  if (Offset != Region->LastInstructionOffset || !AtInstruction)
    return invalid(seh::text::InvalidV2EpiloguePosition);
  return RestorePlan{std::move(Restores)};
}

llvm::Expected<RestorePlan> planV1Restores(const ExceptionFunction &Frame,
                                           const X64SEH::ReadCode &Code,
                                           uint64_t PC, uint64_t End) {
  const RestorePlan NotEpilogue;
  if (!Code)
    return NotEpilogue;
  if (PC >= End)
    return NotEpilogue;
  std::array<uint8_t, seh::MaxEpilogueBytes> Bytes;
  const size_t Length = std::min<uint64_t>(Bytes.size(), End - PC);
  if (auto E = Code(PC, llvm::MutableArrayRef(Bytes).take_front(Length)))
    return std::move(E);

  csh Decoder = 0;
  if (cs_open(CS_ARCH_X86, CS_MODE_64, &Decoder) != CS_ERR_OK)
    return invalid(seh::text::CannotInitializeInstructionDecoder);
  const llvm::scope_exit Close([&] { cs_close(&Decoder); });
  if (cs_option(Decoder, CS_OPT_DETAIL, CS_OPT_ON) != CS_ERR_OK)
    return invalid(seh::text::CannotEnableInstructionDetails);
  cs_insn *Instruction = cs_malloc(Decoder);
  if (!Instruction)
    return invalid(seh::text::CannotAllocateInstructionDetails);
  const llvm::scope_exit Free([&] { cs_free(Instruction, 1); });
  const uint8_t *Cursor = Bytes.data();
  size_t Remaining = Length;
  uint64_t Address = PC;
  std::vector<Restore> Restores;
  bool Complete = false;
  while (cs_disasm_iter(Decoder, &Cursor, &Remaining, &Address, Instruction)) {
    const auto &X86 = Instruction->detail->x86;
    // Address/operand-size, segment and LOCK overrides are not part of the
    // canonical 64-bit epilogue grammar. REP RET is a valid return spelling.
    if (X86.prefix[1] || X86.prefix[2] || X86.prefix[3] ||
        (X86.prefix[0] &&
         !(Instruction->id == X86_INS_RET && X86.prefix[0] == X86_PREFIX_REP)))
      return NotEpilogue;
    const auto &Destination = X86.operands[0];
    const auto &Source = X86.operands[1];
    if (Restores.empty() && Instruction->address == PC && X86.op_count == 2 &&
        Destination.type == X86_OP_REG && Destination.reg == X86_REG_RSP &&
        Destination.size == seh::PointerSize) {
      if (Instruction->id == X86_INS_ADD && Source.type == X86_OP_IMM &&
          Source.imm >= 0) {
        Restores.push_back({Restore::Kind::AddStack, 0, Source.imm});
        continue;
      }
      if (Instruction->id == X86_INS_LEA && Source.type == X86_OP_MEM &&
          Source.mem.index == X86_REG_INVALID && Frame.FrameRegister) {
        const auto Base = registerIndex(Source.mem.base);
        if (Base && *Base == Frame.FrameRegister) {
          Restores.push_back(
              {Restore::Kind::FrameStack, *Base, Source.mem.disp});
          continue;
        }
      }
    }
    if (Instruction->id == X86_INS_POP && X86.op_count == 1 &&
        Destination.type == X86_OP_REG &&
        Destination.size == seh::PointerSize) {
      const auto Register = registerIndex(Destination.reg);
      if (!Register || *Register >= seh::RegisterCount ||
          !(seh::NonvolatileRegisterMask & (uint64_t(1) << *Register)))
        return NotEpilogue;
      Restores.push_back({Restore::Kind::Pop, *Register});
      continue;
    }
    const bool Return = Instruction->id == X86_INS_RET && X86.op_count == 0;
    const bool Tail = Instruction->id == X86_INS_JMP && X86.op_count == 1 &&
                      Destination.type == X86_OP_MEM &&
                      !(X86.modrm & seh::ModRMModeMask);
    if (!Return && !Tail)
      return NotEpilogue;
    Complete = true;
    break;
  }
  if (!Complete)
    return NotEpilogue;
  return RestorePlan{std::move(Restores)};
}
} // namespace

llvm::Expected<std::optional<X64SEH::Context>>
X64SEH::unwindEpilogue(const ExceptionFunction &Frame,
                       const ExceptionFunction &Primary, const Context &Current,
                       Stack Bounds, const Image &Owner) const {
  const uint64_t PC = Current.PC + unsigned(Current.FromReturnAddress);
  const uint64_t Begin =
      Owner.ActualBase + (Frame.CodeRange.Begin - Owner.PreferredBase);
  const uint64_t End =
      Owner.ActualBase + (Frame.CodeRange.End - Owner.PreferredBase);
  if (PC < Begin || PC >= End)
    return std::optional<Context>{};
  auto Plan = Frame.UnwindVersion == 2
                  ? planV2Restores(Frame, Primary, PC - Begin)
                  : planV1Restores(Frame, Code, PC, End);
  if (!Plan)
    return Plan.takeError();
  if (!*Plan)
    return std::optional<Context>{};
  const auto &Restores = **Plan;
  Context Caller = Current;
  auto &SP = Caller.GPR[seh::StackRegister];
  const uint64_t StackEnd = Bounds.Base + Bounds.Size;
  const auto Read = [&](uint64_t Location) -> llvm::Expected<uint64_t> {
    if (Location % seh::PointerSize || Location < Bounds.Base ||
        Location >= StackEnd || StackEnd - Location < seh::PointerSize)
      return invalid(seh::text::RestoreExceedsTheCurrentExecutionStack);
    return ReadStack(Location);
  };
  for (const auto &Restore : Restores) {
    if (Restore.Operation == Restore::Kind::Pop) {
      auto Value = Read(SP);
      if (!Value)
        return Value.takeError();
      Caller.GPR[Restore.Register] = *Value;
      SP += seh::PointerSize;
      continue;
    }
    const uint64_t Base = Restore.Operation == Restore::Kind::FrameStack
                              ? Caller.GPR[Restore.Register]
                              : SP;
    const bool Negative = Restore.Offset < 0;
    const uint64_t Magnitude = Negative ? uint64_t(0) - uint64_t(Restore.Offset)
                                        : uint64_t(Restore.Offset);
    if (Negative ? Base < Magnitude : Base > UINT64_MAX - Magnitude)
      return invalid(seh::text::StackAdjustmentOverflows);
    const uint64_t Adjusted = Negative ? Base - Magnitude : Base + Magnitude;
    if (Adjusted < SP || Adjusted % seh::PointerSize || Adjusted > StackEnd)
      return invalid(seh::text::StackAdjustmentExceedsTheCurrentExecutionStack);
    SP = Adjusted;
  }
  // Tail jumps retain the same caller return slot. The target need not be
  // dereferenced or executed to reconstruct that caller's exception context.
  auto Return = Read(SP);
  if (!Return)
    return Return.takeError();
  if (!*Return)
    return invalid(seh::text::NullReturnAddress);
  SP += seh::PointerSize;
  Caller.PC = *Return - 1;
  Caller.FromReturnAddress = true;
  return std::optional<Context>{Caller};
}
} // namespace neverd::emulation
