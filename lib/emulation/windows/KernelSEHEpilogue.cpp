//===- KernelSEHEpilogue.cpp - x64 V1 epilogue unwind ---------------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Recognize a complete canonical epilogue before reading stack storage.
/// Simulate only the remaining instructions; prologue unwind codes describe
/// the full frame and cannot recover an already partially restored frame.
///
//===----------------------------------------------------------------------===//

#include "../X64Registers.h"
#include "KernelSEH.h"

#include "capstone/capstone.h"
#include "llvm/ADT/ScopeExit.h"

#include <algorithm>
#include <array>
#include <vector>

namespace neverd::emulation {
namespace {
llvm::Error invalid(const char *Message) {
  return llvm::createStringError(llvm::inconvertibleErrorCode(),
                                 "x64 SEH epilogue: %s", Message);
}

std::optional<unsigned> registerIndex(x86_reg Register) {
  switch (Register) {
#define NEVERD_X64_REGISTER(Name, DecoderID, BackendID)                        \
  case DecoderID:                                                              \
    return unsigned(X64Register::Name);
#include "../X64Registers.def"
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
} // namespace

llvm::Expected<std::optional<KernelSEH::Context>>
KernelSEH::unwindEpilogue(const ExceptionFunction &Frame,
                          const Context &Current, Stack Bounds) const {
  const std::optional<Context> NotEpilogue;
  if (!Code)
    return NotEpilogue;
  const uint64_t PC = Current.PC + unsigned(Current.FromReturnAddress);
  const uint64_t End = ActualBase + (Frame.CodeRange.End - PreferredBase);
  if (PC >= End)
    return NotEpilogue;
  std::array<uint8_t, seh::MaxEpilogueBytes> Bytes;
  const size_t Length = std::min<uint64_t>(Bytes.size(), End - PC);
  if (auto E = Code(PC, llvm::MutableArrayRef(Bytes).take_front(Length)))
    return std::move(E);

  csh Decoder = 0;
  if (cs_open(CS_ARCH_X86, CS_MODE_64, &Decoder) != CS_ERR_OK)
    return invalid("cannot initialize instruction decoder");
  const llvm::scope_exit Close([&] { cs_close(&Decoder); });
  if (cs_option(Decoder, CS_OPT_DETAIL, CS_OPT_ON) != CS_ERR_OK)
    return invalid("cannot enable instruction details");
  cs_insn *Instruction = cs_malloc(Decoder);
  if (!Instruction)
    return invalid("cannot allocate instruction details");
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

  Context Caller = Current;
  auto &SP = Caller.GPR[seh::StackRegister];
  const uint64_t StackEnd = Bounds.Base + Bounds.Size;
  const auto Read = [&](uint64_t Location) -> llvm::Expected<uint64_t> {
    if (Location % seh::PointerSize || Location < Bounds.Base ||
        Location >= StackEnd || StackEnd - Location < seh::PointerSize)
      return invalid("restore exceeds the current execution stack");
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
      return invalid("stack adjustment overflows");
    const uint64_t Adjusted = Negative ? Base - Magnitude : Base + Magnitude;
    if (Adjusted < SP || Adjusted % seh::PointerSize || Adjusted > StackEnd)
      return invalid("stack adjustment exceeds the current execution stack");
    SP = Adjusted;
  }
  // Tail jumps retain the same caller return slot. The target need not be
  // dereferenced or executed to reconstruct that caller's exception context.
  auto Return = Read(SP);
  if (!Return)
    return Return.takeError();
  if (!*Return)
    return invalid("null return address");
  SP += seh::PointerSize;
  Caller.PC = *Return - 1;
  Caller.FromReturnAddress = true;
  return std::optional<Context>{Caller};
}
} // namespace neverd::emulation
