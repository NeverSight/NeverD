//===- COFFRegistrationEHBodyRange.cpp - Closed PE32 body extents --------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Exclude trailing callees from a registration body's inferred code range.

#include "../../ReadOnlyBytesIndex.h"
#include "COFFRegistrationEHDetail.h"

#include "neverd/loader/COFF/COFFRegistrationEH.h"
#include "neverd/loader/Limits.h"

#include "llvm/ADT/ScopeExit.h"

#include <algorithm>
#include <capstone/capstone.h>
#include <map>
#include <set>
#include <vector>

namespace neverd::coff_loader::registration_detail {
namespace {

std::optional<va_t> checkBodyRange(const BinaryImage &Img,
                                   const ExceptionAddressRange &Range,
                                   const std::vector<va_t> &Roots, size_t &Work,
                                   std::optional<va_t> RequiredInstruction,
                                   IndexedPE32CodeBytes *CodeBytes = nullptr,
                                   bool JumpsMayLeave = false) {
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
        if (Range.contains(Target))
          Pending.push_back(Target);
        else if (!JumpsMayLeave)
          return std::nullopt;
        if (Instruction->id == X86_INS_JMP)
          break;
      }
      // A compiler's `int3` after a call that does not return runs nothing on,
      // which is all a body closing before a frame's own entry has to say.
      if (JumpsMayLeave && Instruction->id == X86_INS_INT3)
        break;
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

std::optional<std::vector<RegistrationFrameWord>>
checkedRegistrationStraightPrologue(const BinaryImage &Img, va_t Begin,
                                    va_t End, size_t &Work) {
  if (Img.Arch != Arch::X86 || Img.Bits != Bitness::Bits32 || Begin >= End)
    return std::nullopt;
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
  std::vector<RegistrationFrameWord> Words;
  for (va_t Address = Begin; Address < End;) {
    if (Work >= limits::kMaxRegistrationEHStateWork)
      return std::nullopt;
    ++Work;
    const va_t At = Address;
    size_t Remaining = std::min<uint64_t>(15, End - Address);
    const uint8_t *Cursor = Img.readVA(Address, Remaining);
    if (!Cursor ||
        !cs_disasm_iter(Handle, &Cursor, &Remaining, &Address, Instruction) ||
        !Instruction->detail ||
        !Img.hasExecutableCodeOwnerRange(At, Instruction->size) ||
        cs_insn_group(Handle, Instruction, CS_GRP_JUMP) ||
        cs_insn_group(Handle, Instruction, CS_GRP_CALL) ||
        cs_insn_group(Handle, Instruction, CS_GRP_RET) ||
        cs_insn_group(Handle, Instruction, CS_GRP_IRET) ||
        cs_insn_group(Handle, Instruction, CS_GRP_INT))
      return std::nullopt;
    const auto &X86 = Instruction->detail->x86;
    if (Instruction->id == X86_INS_MOV && X86.op_count == 2 &&
        X86.operands[0].type == X86_OP_MEM &&
        X86.operands[1].type == X86_OP_IMM && X86.operands[0].size == 4 &&
        X86.operands[0].mem.base == X86_REG_EBP &&
        X86.operands[0].mem.index == X86_REG_INVALID &&
        X86.operands[0].mem.segment == X86_REG_INVALID)
      Words.push_back(
          {int32_t(X86.operands[0].mem.disp), uint32_t(X86.operands[1].imm)});
  }
  return Words;
}

} // namespace neverd::coff_loader::registration_detail

namespace neverd::coff_loader {

std::set<va_t>
getX86RegistrationBodyInstructions(const BinaryImage &Image,
                                   const ExceptionFunction &Function) {
  std::set<va_t> Reached;
  if (Image.Arch != Arch::X86 || Image.Bits != Bitness::Bits32 ||
      !Function.Registration)
    return Reached;
  csh Handle = 0;
  if (cs_open(CS_ARCH_X86, CS_MODE_32, &Handle) != CS_ERR_OK)
    return Reached;
  const auto Close = llvm::scope_exit([&] { cs_close(&Handle); });
  if (cs_option(Handle, CS_OPT_DETAIL, CS_OPT_ON) != CS_ERR_OK)
    return Reached;
  auto *Instruction = cs_malloc(Handle);
  if (!Instruction)
    return Reached;
  const auto Free = llvm::scope_exit([&] { cs_free(Instruction, 1); });
  std::vector<va_t> Pending{Function.CodeRange.Begin};
  std::vector<va_t> Finally;
  for (const RegistrationScopeRecord &Scope : Function.Registration->Scopes) {
    if (Scope.HandlerVA)
      Pending.push_back(Scope.HandlerVA);
    if (Scope.FilterVA)
      Pending.push_back(Scope.FilterVA);
    if (Scope.IsFinally && Scope.HandlerVA)
      Finally.push_back(Scope.HandlerVA);
  }
  size_t Work = 0;
  while (!Pending.empty()) {
    va_t Address = Pending.back();
    Pending.pop_back();
    while (Function.ownsCode(Address) && !Reached.count(Address) &&
           Work < limits::kMaxRegistrationEHStateWork) {
      ++Work;
      const va_t Begin = Address;
      size_t Remaining = 15;
      const uint8_t *Cursor = Image.readVA(Address, Remaining);
      if (!Cursor ||
          !cs_disasm_iter(Handle, &Cursor, &Remaining, &Address, Instruction) ||
          !Instruction->detail)
        break;
      Reached.insert(Begin);
      if (cs_insn_group(Handle, Instruction, CS_GRP_RET) ||
          cs_insn_group(Handle, Instruction, CS_GRP_IRET))
        break;
      const auto &X86 = Instruction->detail->x86;
      const bool Direct =
          X86.op_count == 1 && X86.operands[0].type == X86_OP_IMM;
      if (cs_insn_group(Handle, Instruction, CS_GRP_JUMP)) {
        if (Direct)
          Pending.push_back(uint32_t(X86.operands[0].imm));
        if (Instruction->id == X86_INS_JMP)
          break;
      } else if (cs_insn_group(Handle, Instruction, CS_GRP_CALL) && Direct) {
        // The normal path's call of a `__finally` body, past what the runtime's
        // way in reloads first.
        const va_t Target = uint32_t(X86.operands[0].imm);
        if (llvm::any_of(Finally, [&](va_t Root) {
              return Target >= Root &&
                     Target - Root <=
                         ::neverd::limits::loader::kMaxRegistrationBodyReload;
            }))
          Pending.push_back(Target);
      }
    }
  }
  return Reached;
}

} // namespace neverd::coff_loader

namespace neverd::coff_loader::registration_detail {

bool checkedRegistrationBodyClosesBefore(const BinaryImage &Img, va_t Begin,
                                         va_t Boundary, size_t &Work) {
  return checkBodyRange(Img, {Begin, Boundary}, {Begin}, Work, std::nullopt,
                        nullptr, /*JumpsMayLeave=*/true)
      .has_value();
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
