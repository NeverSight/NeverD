//===- X64ExecutionPolicy.cpp - Driver CPU environment checks -------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Reject instructions requiring CPU or Windows environment state absent
/// from the configured driver execution profile.
///
//===----------------------------------------------------------------------===//

#include "X64ExecutionPolicy.h"

#include "llvm/Support/Error.h"

#include <memory>
#include <string>

namespace neverd::emulation {
namespace {
llvm::Error failure(const std::string &Text) {
  return llvm::createStringError(llvm::inconvertibleErrorCode(), Text);
}
} // namespace
X64ExecutionPolicy::~X64ExecutionPolicy() {
  if (Handle)
    cs_close(&Handle);
}
llvm::Error X64ExecutionPolicy::initialize() {
  if (cs_open(CS_ARCH_X86, CS_MODE_64, &Handle) != CS_ERR_OK ||
      cs_option(Handle, CS_OPT_DETAIL, CS_OPT_ON) != CS_ERR_OK)
    return failure("cannot initialize x64 instruction policy");
  return llvm::Error::success();
}

llvm::Error X64ExecutionPolicy::validate(llvm::ArrayRef<uint8_t> Bytes,
                                         uint64_t PC) {
  auto Result = inspect(Bytes, PC);
  if (!Result)
    return Result.takeError();
  return llvm::Error::success();
}

llvm::Expected<std::optional<X64Register>>
X64ExecutionPolicy::inspect(llvm::ArrayRef<uint8_t> Bytes, uint64_t PC) {
  cs_insn *Decoded = nullptr;
  size_t Count = cs_disasm(Handle, Bytes.data(), Bytes.size(), PC, 1, &Decoded);
  if (!Count)
    return failure("instruction cannot be decoded by the execution policy");
  std::unique_ptr<cs_insn, void (*)(cs_insn *)> Insn(
      Decoded, [](cs_insn *P) { cs_free(P, 1); });
  if (Insn->size != Bytes.size() || !Insn->detail)
    return failure("CPU and instruction policy disagree on instruction extent");
  auto Rejected = [&]() {
    return failure(std::string("unmodeled CPU environment instruction: ") +
                   Insn->mnemonic);
  };
  const cs_x86 &X86 = Insn->detail->x86;
  // WDM headers inline KeGetCurrentIrql as a CR8 read on x64. This profile
  // owns CR8 as the current callback IRQL and forbids guest CR8 writes. Pinned
  // Unicorn's MOV CR8 helper reads a missing APIC and returns zero, independently
  // of its register API. Return an exact destination for the executor to write;
  // never let that backend helper guess this modeled environment state.
  if (Insn->id == X86_INS_MOV && X86.op_count == 2 &&
      X86.operands[0].type == X86_OP_REG && X86.operands[0].size == 8 &&
      X86.operands[1].type == X86_OP_REG && X86.operands[1].reg == X86_REG_CR8) {
    if (X86.prefix[0] == X86_PREFIX_LOCK ||
        X86.operands[0].reg == X86_REG_RIP ||
        X86.operands[0].reg == X86_REG_CR8)
      return Rejected();
    switch (X86.operands[0].reg) {
#define NEVERD_X64_REGISTER(Name, DecoderID, BackendID)                        \
    case DecoderID:                                                          \
      return std::optional<X64Register>{X64Register::Name};
#include "X64Registers.def"
#undef NEVERD_X64_REGISTER
    default:
      return failure("CR8 read has an unsupported full-width destination");
    }
  }
  if (cs_insn_group(Handle, Insn.get(), CS_GRP_PRIVILEGE) ||
      cs_insn_group(Handle, Insn.get(), CS_GRP_INT) ||
      cs_insn_group(Handle, Insn.get(), CS_GRP_IRET) ||
      cs_insn_group(Handle, Insn.get(), X86_GRP_VM) ||
      cs_insn_group(Handle, Insn.get(), X86_GRP_SGX) ||
      cs_insn_group(Handle, Insn.get(), X86_GRP_RTM) ||
      cs_insn_group(Handle, Insn.get(), X86_GRP_FSGSBASE))
    return Rejected();
  // These instructions observe or mutate environment state absent from the
  // initialization profile, even where the decoder does not mark privilege.
  switch (Insn->id) {
#define NEVERD_DRIVER_UNSUPPORTED_X64(Instruction) case Instruction:
#include "X64UnsupportedInstructions.def"
#undef NEVERD_DRIVER_UNSUPPORTED_X64
    return Rejected();
  default:
    break;
  }
  // XLAT reads [RBX + zero_extend(AL)] but has no explicit memory operand in
  // Capstone's detail. Its effective segment override still changes the
  // address, so the ordinary operand walk cannot establish this boundary.
  if (Insn->id == X86_INS_XLATB &&
      (X86.prefix[1] == X86_PREFIX_FS || X86.prefix[1] == X86_PREFIX_GS))
    return failure("FS/GS memory access requires an unsupported Windows "
                   "thread profile");
  for (unsigned I = 0; I < X86.op_count; ++I) {
    const auto &Operand = X86.operands[I];
    if (Operand.type == X86_OP_MEM && (Operand.mem.segment == X86_REG_FS ||
                                       Operand.mem.segment == X86_REG_GS))
      return failure("FS/GS memory access requires an unsupported Windows "
                     "thread profile");
    if (Operand.type == X86_OP_REG &&
        (Operand.reg == X86_REG_CS || Operand.reg == X86_REG_DS ||
         Operand.reg == X86_REG_ES || Operand.reg == X86_REG_SS ||
         Operand.reg == X86_REG_FS || Operand.reg == X86_REG_GS))
      return Rejected();
  }
  return std::optional<X64Register>{};
}

} // namespace neverd::emulation
