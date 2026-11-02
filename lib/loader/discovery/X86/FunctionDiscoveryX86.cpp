//===- FunctionDiscoveryX86.cpp - x86/x86-64 import thunk scan ---*- C++
//-*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// x86 and x86-64 import thunk recognition for heuristic function discovery.
/// Recognizes the indirect-jump trampolines emitted for PE IAT entries and
/// ELF PLT slots (jmp [rip+disp32] on x86-64, jmp [abs32] on x86, and the
/// PIC PLT entry's jmp [ebx+disp32] from the DT_PLTGOT base in EBX), including
/// the indirect-branch-tracking form that starts with an endbr marker and may
/// give the jump a bnd prefix (`.plt.sec`).  Also tells the string literals
/// an executable section keeps from the functions a padding scan looks for.
///
//===----------------------------------------------------------------------===//

#include "../FunctionDiscoveryDetail.h"

#include "neverd/loader/Limits.h"
#include "neverd/support/ISAEncoding.h"

#include <capstone/capstone.h>
#include <capstone/x86.h>
#include <cstring>
#include <memory>
#include <vector>

namespace neverd {

size_t scanImportThunksX86(BinaryImage &Img, const Segment &Seg,
                           const std::map<va_t, size_t> &Targets,
                           std::set<va_t> &Existing,
                           const ImportThunkCandidates &Candidates) {
  const uint8_t *D = Seg.Data.data();
  size_t N = Seg.Data.size();
  if (N < x86::kJmpIndirectLen)
    return 0;

  const bool Is64 = Img.Arch == Arch::X64;
  // An i386 PIC PLT entry jumps through its GOT entry by the base the
  // dynamic section names, DT_PLTGOT, which its caller holds in EBX.
  const va_t PltGot = Is64 ? 0 : Img.DynInfo.PltGotAddr;
  // The memory-jump ModRM byte is much rarer than FF (also common in negative
  // displacements). PIC i386 accepts two ModRM forms, so it searches FF.
  const size_t SearchOffset = PltGot ? 0 : 1;
  const uint8_t SearchByte =
      PltGot ? x86::kJmpIndirectOp : x86::kJmpIndirectModRM;
  size_t Added = 0;
  for (size_t I = 0; I + x86::kJmpIndirectLen <= N; ++I) {
    // Keep the final complete instruction and overlapping candidates.
    const auto *Match = static_cast<const uint8_t *>(std::memchr(
        D + I + SearchOffset, SearchByte, N - x86::kJmpIndirectLen + 1 - I));
    if (!Match)
      break;
    I = static_cast<size_t>(Match - D) - SearchOffset;
    if (D[I] != x86::kJmpIndirectOp)
      continue;
    const bool ThroughGOT = PltGot && D[I + 1] == x86::kJmpIndirectGOTModRM;
    if (!ThroughGOT && D[I + 1] != x86::kJmpIndirectModRM)
      continue;
    va_t InsnVA = Seg.VA + I;
    va_t Target;
    if (ThroughGOT) {
      int32_t Disp;
      std::memcpy(&Disp, D + I + x86::kJmpIndirectDispOffset, sizeof(Disp));
      Target = static_cast<uint32_t>(PltGot + static_cast<int64_t>(Disp));
    } else if (Is64) {
      int32_t Disp;
      std::memcpy(&Disp, D + I + x86::kJmpIndirectDispOffset, sizeof(Disp));
      Target = InsnVA + x86::kJmpIndirectLen + static_cast<int64_t>(Disp);
    } else {
      uint32_t AbsAddr;
      std::memcpy(&AbsAddr, D + I + x86::kJmpIndirectDispOffset,
                  sizeof(AbsAddr));
      Target = AbsAddr;
    }
    auto TargetIt = Targets.find(Target);
    if (TargetIt == Targets.end())
      continue;
    // An endbr marker right before the jump starts the thunk, and a bnd
    // prefix between them belongs to the jump.  A lone F2 byte may end the
    // previous instruction, so it only counts after an endbr.
    size_t Start = I;
    const auto EndbrBefore = [&](size_t At) {
      if (At < x86::kEndbrLen)
        return false;
      uint32_t Word;
      std::memcpy(&Word, D + At - x86::kEndbrLen, sizeof(Word));
      return Word == (Is64 ? x86::kEndbr64Word : x86::kEndbr32Word);
    };
    if (EndbrBefore(Start))
      Start -= x86::kEndbrLen;
    else if (Start > 0 && D[Start - 1] == x86::kBndPrefix &&
             EndbrBefore(Start - 1))
      Start -= 1 + x86::kEndbrLen;
    const va_t ThunkVA = Seg.VA + Start;
    const size_t ThunkLen = I + x86::kJmpIndirectLen - Start;
    if (!Img.isCodeRange(ThunkVA, ThunkLen) ||
        !Candidates.allows(ThunkVA, ThunkLen))
      continue;
    Img.recordImportStub(ThunkVA, TargetIt->second);
    if (!Existing.insert(ThunkVA).second)
      continue;
    Img.Symbols.push_back(Symbol::makeFunc(ThunkVA, ThunkLen));
    ++Added;
  }
  return Added;
}

namespace {
/// A character the text of a string literal holds: printable ASCII, tab,
/// line feed or carriage return.  None is a byte x86 begins a return, a call
/// or an unconditional jump with.
bool isLiteralTextByte(uint8_t Byte) {
  return (Byte >= 0x20 && Byte < 0x7f) || Byte == '\t' || Byte == '\n' ||
         Byte == '\r';
}

bool isLegacyPrefix(uint8_t Byte) {
  switch (Byte) {
#define NEVERD_X86_LEGACY_PREFIX(Value) case Value:
#include "X86LiteralRunBytes.def"
    return true;
  default:
    return false;
  }
}

/// Whether 64-bit mode rejects the instruction whose opcode byte is at
/// \p Opcode of \p Data: an opcode invalid there, or an EVEX prefix that
/// selects the reserved map 0.
bool isRejectedIn64BitMode(const uint8_t *Data, size_t Size, size_t Opcode) {
  if (Opcode >= Size)
    return false;
  switch (Data[Opcode]) {
#define NEVERD_X86_INVALID_IN_64BIT(Value) case Value:
#include "X86LiteralRunBytes.def"
    return true;
  default:
    break;
  }
#define NEVERD_X86_EVEX_RESERVED_MAP(Prefix, MapMask)                          \
  if (Data[Opcode] == Prefix)                                                  \
    return Opcode + 1 < Size && (Data[Opcode + 1] & MapMask) == 0;
#include "X86LiteralRunBytes.def"
  return false;
}

} // namespace

struct X86StringLiteralRuns::Decoder {
  csh Handle = 0;
  cs_insn *Insn = nullptr;
  ~Decoder() {
    if (Insn)
      cs_free(Insn, 1);
    if (Handle)
      cs_close(&Handle);
  }
};

X86StringLiteralRuns::X86StringLiteralRuns(const BinaryImage &Image)
    : Img(Image) {}

X86StringLiteralRuns::~X86StringLiteralRuns() = default;

bool X86StringLiteralRuns::at(const Segment &Seg, size_t Offset) {
  const uint8_t *Data = Seg.Data.data();
  const size_t Size = Seg.Data.size();
  // A literal begins there: text, then the zero that ends it.
  size_t TextEnd = Offset;
  while (TextEnd < Size && isLiteralTextByte(Data[TextEnd]))
    ++TextEnd;
  if (TextEnd == Offset || TextEnd >= Size || Data[TextEnd] != 0)
    return false;
  // What execution entered there could run through: literals, the zeros
  // after them, and the int3 padding between them.
  size_t RegionEnd = TextEnd;
  while (RegionEnd < Size &&
         RegionEnd - Offset <
             ::neverd::limits::loader::kFunctionDiscoveryX86MaxLiteralRegion &&
         (isLiteralTextByte(Data[RegionEnd]) || Data[RegionEnd] == 0 ||
          Data[RegionEnd] == x86::kInt3))
    ++RegionEnd;

  if (!Dec) {
    auto Opened = std::make_unique<Decoder>();
    if (cs_open(CS_ARCH_X86, Img.Arch == Arch::X64 ? CS_MODE_64 : CS_MODE_32,
                &Opened->Handle) != CS_ERR_OK ||
        cs_option(Opened->Handle, CS_OPT_DETAIL, CS_OPT_ON) != CS_ERR_OK ||
        !(Opened->Insn = cs_malloc(Opened->Handle)))
      return false;
    Dec = std::move(Opened);
  }
  // Every path execution entered at Offset can take must end in a trap: an
  // int3, or an opcode the processor rejects.  Text holds no return, call or
  // unconditional jump, but a conditional jump may lead out of the region,
  // and an instruction may take bytes past it for operands.
  std::vector<bool> Seen(RegionEnd - Offset, false);
  std::vector<size_t> Pending{Offset};
  size_t Followed = 0;
  while (!Pending.empty()) {
    const size_t At = Pending.back();
    Pending.pop_back();
    if (Seen[At - Offset])
      continue;
    Seen[At - Offset] = true;
    if (Data[At] == x86::kInt3)
      continue;
    if (++Followed >
        ::neverd::limits::loader::kFunctionDiscoveryX86MaxLiteralInstructions)
      return false;
    const uint8_t *Bytes = Data + At;
    size_t Remaining = Size - At;
    uint64_t Address = Seg.VA + At;
    cs_insn *Insn = Dec->Insn;
    if (!cs_disasm_iter(Dec->Handle, &Bytes, &Remaining, &Address, Insn)) {
      // No instruction decodes there: an opcode 64-bit mode rejects, after
      // any prefixes, raises #UD.
      size_t Opcode = At;
      while (Opcode < RegionEnd &&
             (isLegacyPrefix(Data[Opcode]) ||
              (Img.Arch == Arch::X64 && (Data[Opcode] & 0xF0) == 0x40)))
        ++Opcode;
      if (Img.Arch == Arch::X64 && Opcode < RegionEnd &&
          isRejectedIn64BitMode(Data, RegionEnd, Opcode))
        continue;
      return false;
    }
    if (At + Insn->size > RegionEnd)
      return false;
    const cs_detail &Detail = *Insn->detail;
    bool Jump = false;
    for (uint8_t Index = 0; Index < Detail.groups_count; ++Index)
      switch (Detail.groups[Index]) {
      case CS_GRP_JUMP:
        Jump = true;
        break;
      case CS_GRP_CALL:
      case CS_GRP_RET:
      case CS_GRP_INT:
      case CS_GRP_IRET:
        return false;
      default:
        break;
      }
    if (Jump) {
      if (Insn->id == X86_INS_JMP || Insn->id == X86_INS_LJMP ||
          Detail.x86.op_count != 1 || Detail.x86.operands[0].type != X86_OP_IMM)
        return false;
      const uint64_t Target = static_cast<uint64_t>(Detail.x86.operands[0].imm);
      if (Target < Seg.VA + Offset || Target - Seg.VA >= RegionEnd)
        return false;
      Pending.push_back(static_cast<size_t>(Target - Seg.VA));
    }
    const size_t Next = At + Insn->size;
    if (Next >= RegionEnd)
      return false;
    Pending.push_back(Next);
  }
  return true;
}

} // namespace neverd
