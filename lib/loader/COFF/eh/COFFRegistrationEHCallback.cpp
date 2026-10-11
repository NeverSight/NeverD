//===- COFFRegistrationEHCallback.cpp - PE32 callback boundaries ---------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Disprove padding guesses inside decoded registration callback instructions.
//===----------------------------------------------------------------------===//
#include "COFFRegistrationEHDetail.h"

#include "llvm/ADT/ScopeExit.h"

#include <capstone/capstone.h>

namespace neverd::coff_loader::registration_detail {
bool callbackFallsThroughBoundary(const BinaryImage &Img, va_t Entry,
                                  va_t Boundary, size_t &Work) {
  if (Entry >= Boundary || Work >= limits::kMaxRegistrationEHStateWork)
    return false;
  const auto *Segment = Img.getSegmentFor(Entry);
  if (!Segment || !Segment->isExecutable())
    return false;
  const auto Size = std::min<uint64_t>(Segment->Size, Segment->Data.size());
  if (Entry - Segment->VA >= Size || Boundary - Segment->VA >= Size)
    return false;
  csh Handle = 0;
  if (cs_open(CS_ARCH_X86, CS_MODE_32, &Handle) != CS_ERR_OK)
    return false;
  const auto Close = llvm::make_scope_exit([&] { cs_close(&Handle); });
  if (cs_option(Handle, CS_OPT_DETAIL, CS_OPT_ON) != CS_ERR_OK)
    return false;
  auto *Instruction = cs_malloc(Handle);
  if (!Instruction)
    return false;
  const auto Free = llvm::make_scope_exit([&] { cs_free(Instruction, 1); });
  const uint8_t *Cursor = Segment->Data.data() + Entry - Segment->VA;
  size_t Remaining = Size - (Entry - Segment->VA);
  uint64_t Address = Entry;
  // Regenerated register-state initialization can exceed 4096 instructions.
  // Bound all queries by the shared work counter; a second per-query cutoff
  // would turn an immediate byte in that initializer into a function entry.
  while (Address < Boundary) {
    if (Work >= limits::kMaxRegistrationEHStateWork)
      return false;
    ++Work;
    if (!cs_disasm_iter(Handle, &Cursor, &Remaining, &Address, Instruction) ||
        !Img.hasExecutableCodeOwnerRange(Instruction->address,
                                         Instruction->size))
      return false;
    // This only establishes straight-line ownership, not a new CFG. Calls,
    // branches and terminators require the later instruction/state analysis;
    // none may make a separate function disappear during loading.
    for (unsigned Group :
         {CS_GRP_JUMP, CS_GRP_CALL, CS_GRP_RET, CS_GRP_INT, CS_GRP_IRET})
      if (cs_insn_group(Handle, Instruction, Group))
        return false;
    if (Instruction->id == X86_INS_UD0 || Instruction->id == X86_INS_UD1 ||
        Instruction->id == X86_INS_UD2 || Instruction->id == X86_INS_HLT)
      return false;
  }
  return Address >= Boundary;
}
} // namespace neverd::coff_loader::registration_detail
