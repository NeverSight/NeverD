//===- COFFRegistrationEHBodyRange.cpp - Closed PE32 body extents --------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Exclude trailing callees from a registration body's inferred code range.

#include "../../ReadOnlyBytesIndex.h"
#include "COFFRegistrationEHDetail.h"

#include "neverd/loader/Limits.h"

#include "llvm/ADT/ScopeExit.h"

#include <algorithm>
#include <capstone/capstone.h>
#include <map>
#include <vector>

namespace neverd::coff_loader::registration_detail {
namespace {

std::optional<va_t> checkBodyRange(const BinaryImage &Img,
                                   const ExceptionAddressRange &Range,
                                   const std::vector<va_t> &Roots, size_t &Work,
                                   std::optional<va_t> RequiredInstruction,
                                   IndexedPE32CodeBytes *CodeBytes = nullptr) {
  if (Img.Arch != Arch::X86 || Img.Bits != Bitness::Bits32 ||
      Img.Format != BinaryFormat::COFF || !Range.isValid() ||
      Range.End > uint64_t(UINT32_MAX) + 1 || Roots.empty() ||
      Roots.size() > limits::kMaxRegistrationEHStateWork -
                         std::min(Work, limits::kMaxRegistrationEHStateWork))
    return std::nullopt;
  Work += Roots.size();
  csh Handle = 0;
  if (cs_open(CS_ARCH_X86, CS_MODE_32, &Handle) != CS_ERR_OK)
    return std::nullopt;
  const auto Close = llvm::scope_exit([&] { cs_close(&Handle); });
  if (cs_option(Handle, CS_OPT_DETAIL, CS_OPT_ON) != CS_ERR_OK)
    return std::nullopt;
  auto *Instruction = cs_malloc(Handle);
  if (!Instruction)
    return std::nullopt;
  const auto Free = llvm::scope_exit([&] { cs_free(Instruction, 1); });
  std::vector<va_t> Pending = Roots;
  std::map<va_t, va_t> Decoded;
  std::map<va_t, std::vector<va_t>> AbsoluteOperands;
  va_t End = Range.Begin;
  while (!Pending.empty()) {
    va_t Address = Pending.back();
    Pending.pop_back();
    while (!Decoded.count(Address)) {
      if (Work >= limits::kMaxRegistrationEHStateWork ||
          !Range.contains(Address))
        return std::nullopt;
      ++Work;
      const va_t Begin = Address;
      size_t Remaining = std::min<uint64_t>(15, Range.End - Address);
      const uint8_t *Cursor = Img.readVA(Address, Remaining);
      if (!Cursor ||
          !cs_disasm_iter(Handle, &Cursor, &Remaining, &Address, Instruction) ||
          !Instruction->detail ||
          !Img.hasExecutableCodeOwnerRange(Begin, Instruction->size))
        return std::nullopt;
      const auto Next = Decoded.lower_bound(Begin);
      if ((Next != Decoded.end() && Next->first < Address) ||
          (Next != Decoded.begin() && std::prev(Next)->second > Begin))
        return std::nullopt;
      Decoded.emplace(Begin, Address);
      if (RequiredInstruction) {
        const bool Call = cs_insn_group(Handle, Instruction, CS_GRP_CALL);
        const bool Jump = cs_insn_group(Handle, Instruction, CS_GRP_JUMP);
        const auto &X86 = Instruction->detail->x86;
        const bool Direct =
            X86.op_count == 1 && X86.operands[0].type == X86_OP_IMM;
        auto &Operands = AbsoluteOperands[Begin];
        if (!Call && !Jump && X86.encoding.imm_size == 4)
          Operands.push_back(Begin + X86.encoding.imm_offset);
        if (X86.encoding.disp_size == 4 && (!(Call || Jump) || !Direct))
          Operands.push_back(Begin + X86.encoding.disp_offset);
      }
      End = std::max(End, Address);
      if (cs_insn_group(Handle, Instruction, CS_GRP_RET)) {
        if (Instruction->id != X86_INS_RET)
          return std::nullopt;
        break;
      }
      if (cs_insn_group(Handle, Instruction, CS_GRP_IRET))
        return std::nullopt;
      if (cs_insn_group(Handle, Instruction, CS_GRP_JUMP)) {
        const auto &X86 = Instruction->detail->x86;
        if (X86.op_count != 1 || X86.operands[0].type != X86_OP_IMM)
          return std::nullopt;
        const va_t Target = uint32_t(X86.operands[0].imm);
        if (!Range.contains(Target))
          return std::nullopt;
        Pending.push_back(Target);
        if (Instruction->id == X86_INS_JMP)
          break;
      }
      // Calls retain their ordinary continuation without absorbing the
      // callee. Traps conservatively retain fallthrough too: this boundary
      // proof must not invent a no-return contract or discard resumed code.
    }
  }
  if (RequiredInstruction) {
    if (!CodeBytes || !Decoded.count(*RequiredInstruction))
      return std::nullopt;
    for (auto It = Decoded.begin(); It != Decoded.end();) {
      const va_t Begin = It->first;
      va_t Limit = Begin;
      std::vector<va_t> Operands;
      do {
        Limit = It->second;
        const auto &Current = AbsoluteOperands.at(It->first);
        Operands.insert(Operands.end(), Current.begin(), Current.end());
        ++It;
      } while (It != Decoded.end() && It->first == Limit &&
               It->second - Begin <=
                   ::neverd::limits::loader::kMaxImageReadBytes);
      if (!CodeBytes->read(Begin, uint32_t(Limit - Begin), Operands))
        return std::nullopt;
    }
  }
  return End;
}
} // namespace

std::optional<va_t>
checkedRegistrationRangeEnd(const BinaryImage &Img,
                            const ExceptionAddressRange &Range,
                            const std::vector<va_t> &Roots, size_t &Work) {
  return checkBodyRange(Img, Range, Roots, Work, std::nullopt);
}

bool checkedRegistrationBodyOwnsInstruction(const BinaryImage &Img,
                                            const ExceptionAddressRange &Range,
                                            va_t Entry, va_t Instruction,
                                            size_t &Work,
                                            IndexedPE32CodeBytes &CodeBytes) {
  if (!Range.contains(Instruction))
    return false;
  return bool(
      checkBodyRange(Img, Range, {Entry}, Work, Instruction, &CodeBytes));
}

} // namespace neverd::coff_loader::registration_detail
