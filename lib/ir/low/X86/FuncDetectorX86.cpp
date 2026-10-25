//===- FuncDetectorX86.cpp - x86 / x86-64 function-entry scanning ---------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// x86 and x86-64 scanners used by FuncDetector: direct CALL targets via a
/// lightweight decode walk, and VC6 / MSVC unsymbolized prologue recovery.
///
//===----------------------------------------------------------------------===//

#include "../FuncDetectorDetail.h"

#include "neverd/ir/low/FuncDetector.h"
#include "neverd/libc/LibCNames.h"
#include "neverd/libc/NoReturnLoadedTarget.h"
#include "neverd/support/BinaryEncoding.h"
#include "neverd/support/ProloguePatterns.h"
#include "neverd/support/X86AddressReference.h"

#include "llvm/ADT/ArrayRef.h"

#include <algorithm>
#include <capstone/x86.h>
#include <cstring>
#include <map>
#include <optional>
#include <set>
#include <vector>

namespace neverd {
namespace func_detect_detail {

CodeInterval codeIntervalAround(const BinaryImage &Img, va_t Addr) {
  if (Img.Arch != Arch::X86 && Img.Arch != Arch::X64)
    return {};
  const Segment *Seg = Img.getSegmentFor(Addr);
  if (!Seg || !Seg->isExecutable() || !Img.isCodeAddress(Addr) ||
      Seg->Size > InvalidVA - Seg->VA)
    return {};
  CodeInterval Known{Seg->VA, Seg->VA + Seg->Size};
  // The first segment and section containing an address own it, so every
  // earlier one that overlaps the stretch cuts it short.
  bool Inside = false;
  auto Exclude = [&](va_t Begin, uint64_t Size) {
    const va_t End = Size > InvalidVA - Begin ? InvalidVA : Begin + Size;
    if (End <= Known.Lo || Begin >= Known.Hi)
      return;
    if (Addr < Begin)
      Known.Hi = Begin;
    else if (Addr >= End)
      Known.Lo = End;
    else
      Inside = true;
  };
  for (const Segment &Other : Img.Segments) {
    if (&Other == Seg)
      break;
    Exclude(Other.VA, Other.Size);
  }
  if (const Section *Sec = Img.getSectionFor(Addr)) {
    if (Sec->Size > InvalidVA - Sec->VA)
      return {};
    Known.Lo = std::max(Known.Lo, Sec->VA);
    Known.Hi = std::min(Known.Hi, Sec->VA + Sec->Size);
    for (const Section &Other : Img.Sections) {
      if (&Other == Sec)
        break;
      if (Other.isReadable())
        Exclude(Other.VA, Other.Size);
    }
  } else if (Img.segmentHasReadableSectionMetadata(*Seg)) {
    return {};
  }
  if (Inside || Known.Lo >= Known.Hi)
    return {};
  return Known;
}

std::optional<CallScanStep> stepCallsX86(const BinaryImage &Img, Decoder &Dec,
                                         const Segment *Seg, va_t Cur, va_t End,
                                         CodeInterval &Known) {
  const size_t Off = static_cast<size_t>(Cur - Seg->VA);
  if (Off >= Seg->Data.size())
    return std::nullopt;
  CallScanStep Step;
  Step.Addr = Cur;
  DecodedInsn DI;
  const size_t Remain =
      static_cast<size_t>(std::min<va_t>(Seg->Data.size() - Off, End - Cur));
  const int Sz = Dec.decodeOneLight(Seg->Data.data() + Off, Remain, Cur, DI);
  if (Sz <= 0) {
    Step.Next = Cur + 1;
    return Step;
  }
  Step.Next = Cur + static_cast<va_t>(Sz);
  if (Known.Lo != Known.Hi && Cur >= Known.Hi)
    Known = codeIntervalAround(Img, Cur);
  const bool Owned = Known.Lo != Known.Hi && Cur >= Known.Lo &&
                     Step.Next <= Known.Hi && Step.Next > Cur;
  if (!Owned &&
      !Img.hasExecutableCodeOwnerRange(Cur, static_cast<uint64_t>(Sz)))
    return Step;
  // Linked PE leaf callbacks need not have unwind metadata or a prologue.
  // The size/id sweep already proves the instruction boundary. Its canonical
  // RIP-relative encoding supplies the target without a second decode.
  if (DI.Id == X86_INS_LEA && Img.Arch == Arch::X64 &&
      Img.Format == BinaryFormat::COFF && !Img.IsRelocatable) {
    const auto Bytes = llvm::ArrayRef(Seg->Data).slice(Off, Sz);
    if (x86::isRipRelativeAddressInstruction(Bytes)) {
      const va_t Target =
          Step.Next + static_cast<va_t>(readLE<int32_t>(
                          Bytes.data() + x86::kAddressDisplacementOffset));
      if (Img.hasExecutableCodeOwnerAt(Target))
        Step.CodeTarget = Target;
    }
    return Step;
  }
  // Sizes and ids do not depend on operand detail or printed text. Only
  // calls need a detailed re-decode.
  if (!Dec.detailEnabled()) {
    if (DI.Id != X86_INS_CALL)
      return Step;
    const bool PreviousText = Dec.textEnabled();
    Dec.setText(true);
    Dec.setDetail(true);
    const int Again =
        Dec.decodeOneLight(Seg->Data.data() + Off, Remain, Cur, DI);
    Dec.setDetail(false);
    Dec.setText(PreviousText);
    if (Again != Sz)
      return Step;
  }
  const va_t Tgt = Dec.directCallTarget(DI);
  if (Tgt != InvalidVA && Img.hasExecutableCodeOwnerAt(Tgt))
    Step.Target = Tgt;
  return Step;
}

void scanCodePointersX64(const BinaryImage &Img, Decoder &Dec,
                         std::set<va_t> &Out, const Cancellation *Control) {
  const bool KeptDetail = Dec.detailEnabled(), KeptText = Dec.textEnabled();
  Dec.setText(false);
  for (const Segment &Seg : Img.Segments) {
    func_detect_detail::checkCancellation(Control);
    if (!Seg.isExecutable() || Seg.Data.empty())
      continue;
    for (size_t Off = 0; Off < Seg.Data.size();) {
      func_detect_detail::checkCancellation(Control);
      const va_t Cur = Seg.VA + Off;
      const uint8_t *Bytes = Seg.Data.data() + Off;
      const size_t Remain = Seg.Data.size() - Off;
      DecodedInsn DI;
      Dec.setText(false);
      Dec.setDetail(false);
      const int Size = Dec.decodeOneLight(Bytes, Remain, Cur, DI);
      if (Size <= 0) {
        ++Off;
        continue;
      }
      Off += static_cast<size_t>(Size);
      // Only a lea's operand detail, which comes with text, says where it
      // points.
      if (DI.Id != X86_INS_LEA)
        continue;
      Dec.setText(true);
      Dec.setDetail(true);
      if (Dec.decodeOneLight(Bytes, Remain, Cur, DI) != Size || !DI.Raw ||
          !DI.Raw->detail)
        continue;
      const cs_x86 &X86 = DI.Raw->detail->x86;
      if (X86.op_count != 2 || X86.operands[1].type != X86_OP_MEM ||
          X86.operands[1].mem.base != X86_REG_RIP ||
          X86.operands[1].mem.index != X86_REG_INVALID)
        continue;
      const va_t Target = Cur + static_cast<va_t>(Size) +
                          static_cast<va_t>(X86.operands[1].mem.disp);
      const Segment *Owner = Img.getSegmentFor(Target);
      if (!Owner || !Img.hasExecutableCodeOwnerAt(Target))
        continue;
      const size_t At = static_cast<size_t>(Target - Owner->VA);
      if (At >= Owner->Data.size())
        continue;
      const uint8_t *Start = Owner->Data.data() + At;
      const size_t Left = Owner->Data.size() - At;
      const size_t Marker = x86BranchTargetMarkerLength(Start, Left);
      if (isPrologueAt(Start + Marker, Left - Marker, Img.Arch))
        Out.insert(Target);
    }
  }
  Dec.setDetail(KeptDetail);
  Dec.setText(KeptText);
}

void scanCodeAddressImmediatesX86(const BinaryImage &Img, Decoder &Dec,
                                  std::vector<std::pair<va_t, va_t>> &Out,
                                  const Cancellation *Control) {
  const auto &Fields = Img.CodeAddressRelocOperands;
  if (Fields.empty())
    return;
  const bool KeptDetail = Dec.detailEnabled(), KeptText = Dec.textEnabled();
  for (const Section &Sec : Img.Sections) {
    func_detect_detail::checkCancellation(Control);
    if (Sec.Size == 0 || !Sec.isExecutable() || !Img.isCodeAddress(Sec.VA))
      continue;
    const Segment *Seg = Img.getSegmentFor(Sec.VA);
    if (!Seg || Seg->Data.empty() || Sec.VA < Seg->VA ||
        Sec.VA - Seg->VA >= Seg->Data.size())
      continue;
    const va_t End =
        std::min<va_t>(Sec.VA + Sec.Size, Seg->VA + Seg->Data.size());
    auto Field = Fields.lower_bound(Sec.VA);
    for (va_t Cur = Sec.VA; Cur < End;) {
      func_detect_detail::checkCancellation(Control);
      while (Field != Fields.end() && Field->first < Cur)
        ++Field;
      if (Field == Fields.end() || Field->first >= End)
        break;
      const uint8_t *Bytes = Seg->Data.data() + (Cur - Seg->VA);
      const size_t Remain = static_cast<size_t>(End - Cur);
      DecodedInsn DI;
      Dec.setText(false);
      Dec.setDetail(false);
      const int Size = Dec.decodeOneLight(Bytes, Remain, Cur, DI);
      if (Size <= 0) {
        ++Cur;
        continue;
      }
      const va_t Next = Cur + static_cast<va_t>(Size);
      if (Field->first < Next) {
        // Only operand detail, which comes with text, says which field of
        // the instruction is its immediate.
        Dec.setText(true);
        Dec.setDetail(true);
        if (Dec.decodeOneLight(Bytes, Remain, Cur, DI) == Size && DI.Raw &&
            DI.Raw->detail) {
          const cs_x86_encoding &Encoding = DI.Raw->detail->x86.encoding;
          for (auto It = Field; It != Fields.end() && It->first < Next; ++It)
            if (Encoding.imm_size == 4 &&
                It->first == Cur + Encoding.imm_offset &&
                It->second.Width == 4 &&
                !It->second.PCRelativeFromInstructionEnd &&
                It->second.Kind == RelocatedAddressFieldKind::Generic)
              Out.emplace_back(Cur, normalizeCodeAddress(It->second.TargetVA,
                                                         Img.Arch, Img.Mode));
        }
      }
      Cur = Next;
    }
  }
  Dec.setDetail(KeptDetail);
  Dec.setText(KeptText);
}

bool isX86EscapingCodeImmediate(const BinaryImage &Img, va_t Insn) {
  const uint8_t *Bytes = Img.readVA(Insn, 2);
  if (!Bytes)
    return false;
  return Bytes[0] == 0x68 || (Bytes[0] == 0xC7 && (Bytes[1] >> 6) != 3);
}

void scanSegmentCallsX86(const BinaryImage &Img, Decoder &Dec,
                         const Segment *Seg, va_t Start, va_t End,
                         std::set<va_t> &Out,
                         std::vector<std::pair<va_t, va_t>> &CodeAddresses,
                         const Cancellation *Control) {
  CodeInterval Known = codeIntervalAround(Img, Start);
  for (va_t Cur = Start; Cur < End;) {
    func_detect_detail::checkCancellation(Control);
    const std::optional<CallScanStep> Step =
        stepCallsX86(Img, Dec, Seg, Cur, End, Known);
    if (!Step)
      break;
    if (Step->Target != InvalidVA)
      Out.insert(Step->Target);
    if (Step->CodeTarget != InvalidVA)
      CodeAddresses.emplace_back(Step->Addr, Step->CodeTarget);
    Cur = Step->Next;
  }
}

} // namespace func_detect_detail

namespace {

bool isX86StrongFramePrologue(llvm::ArrayRef<uint8_t> Bytes) {
  if (Bytes.size() < 2)
    return false;
  // push ebp / mov ebp, esp  (55 8B EC or 55 89 E5)
  if (Bytes[0] == 0x55 && (Bytes[1] == 0x8B || Bytes[1] == 0x89))
    return true;
  // hotpatch: mov edi, edi; push ebp
  if (Bytes.size() >= 3 && Bytes[0] == 0x8B && Bytes[1] == 0xFF &&
      Bytes[2] == 0x55)
    return true;
  return false;
}

/// `push ebp; mov ebp, esp` (55 8B EC or 55 89 E5): the frame setup itself,
/// not just a push of EBP followed by some move.
bool isX86EbpFrameSetup(llvm::ArrayRef<uint8_t> Bytes) {
  return Bytes.size() >= 3 && Bytes[0] == 0x55 &&
         ((Bytes[1] == 0x8B && Bytes[2] == 0xEC) ||
          (Bytes[1] == 0x89 && Bytes[2] == 0xE5));
}

bool isX86StackAdjustPrologue(llvm::ArrayRef<uint8_t> Bytes) {
  return Bytes.size() >= 2 && Bytes[1] == 0xEC &&
         (Bytes[0] == 0x81 || Bytes[0] == 0x83);
}

bool isX86MsvcPrologue(llvm::ArrayRef<uint8_t> Bytes) {
  if (isX86StrongFramePrologue(Bytes) || isX86StackAdjustPrologue(Bytes))
    return true;
  if (Bytes.size() < 2)
    return false;
  // SEH: push -1; push handler
  if (Bytes[0] == 0x6A && Bytes[1] == 0xFF)
    return true;
  // Callee-saved push then another push or a mov (VC6 cdecl / thiscall).
  if (Bytes[0] == 0x51 || Bytes[0] == 0x53 || Bytes[0] == 0x56 ||
      Bytes[0] == 0x57) {
    const uint8_t Next = Bytes[1];
    if (Next == 0x51 || Next == 0x53 || Next == 0x55 || Next == 0x56 ||
        Next == 0x57 || Next == 0x8B)
      return true;
  }
  // mov r32, [esp+disp8]
  if (Bytes.size() >= 3 && Bytes[0] == 0x8B && Bytes[2] == 0x24)
    return true;
  // MSVC thiscall vtable-store thunk: mov dword ptr [ecx], imm32
  if (Bytes.size() >= 6 && Bytes[0] == 0xC7 && Bytes[1] == 0x01)
    return true;
  return false;
}

void considerX86Prologue(const BinaryImage &Img, Decoder &Dec,
                         const Segment &Seg, va_t Addr,
                         const std::vector<std::pair<va_t, va_t>> &Occupied,
                         std::set<va_t> &Out,
                         std::set<va_t> &UnsymbolizedX86Entries) {
  auto InteriorOfSizedSymbol = [&](va_t A) {
    auto It =
        std::upper_bound(Occupied.begin(), Occupied.end(), A,
                         [](va_t Needle, const std::pair<va_t, va_t> &Range) {
                           return Needle < Range.first;
                         });
    if (It == Occupied.begin())
      return false;
    --It;
    return A > It->first && A < It->second;
  };
  // The push after MSVC's hotpatch no-op is the second instruction of a
  // function that begins at the no-op, never an entry of its own.
  if (Addr >= Seg.VA + 2 &&
      isX86HotpatchEntryAt(Seg.Data.data(), Seg.Data.size(),
                           static_cast<size_t>(Addr - Seg.VA) - 2))
    Addr -= 2;
  if (InteriorOfSizedSymbol(Addr) || !Img.hasExecutableCodeOwnerAt(Addr))
    return;
  if (Addr < Seg.VA)
    return;
  const size_t Off = static_cast<size_t>(Addr - Seg.VA);
  if (Off >= Seg.Data.size())
    return;
  const size_t Remain = Seg.Data.size() - Off;
  DecodedInsn DI;
  const int Sz = Dec.decodeOneLight(Seg.Data.data() + Off, Remain, Addr, DI);
  if (Sz <= 0 || !DI.Raw)
    return;
  Out.insert(Addr);
  UnsymbolizedX86Entries.insert(Addr);
}

/// A branch taken only on a condition, whose fall-through also executes.
bool isX86ConditionalJump(unsigned Id) {
  switch (Id) {
  case X86_INS_JE:
  case X86_INS_JNE:
  case X86_INS_JA:
  case X86_INS_JAE:
  case X86_INS_JB:
  case X86_INS_JBE:
  case X86_INS_JG:
  case X86_INS_JGE:
  case X86_INS_JL:
  case X86_INS_JLE:
  case X86_INS_JS:
  case X86_INS_JNS:
  case X86_INS_JO:
  case X86_INS_JNO:
  case X86_INS_JP:
  case X86_INS_JNP:
  case X86_INS_JCXZ:
  case X86_INS_JECXZ:
  case X86_INS_JRCXZ:
  case X86_INS_LOOP:
  case X86_INS_LOOPE:
  case X86_INS_LOOPNE:
    return true;
  default:
    return false;
  }
}

} // namespace

void FuncDetector::scanX86UnsymbolizedEntries(const BinaryImage &Img,
                                              Decoder &Dec,
                                              std::set<va_t> &Out) {
  std::vector<std::pair<va_t, va_t>> Occupied;
  for (const Symbol &Sym : Img.Symbols) {
    func_detect_detail::checkCancellation(Cancellation);
    if (!Sym.IsFunc || Sym.Size == 0 || Sym.Size > InvalidVA - Sym.Addr)
      continue;
    Occupied.emplace_back(Sym.Addr, Sym.Addr + Sym.Size);
  }
  std::sort(Occupied.begin(), Occupied.end());

  const bool PrevDetail = Dec.detailEnabled();
  Dec.setDetail(false);
  for (const Section &Sec : Img.Sections) {
    func_detect_detail::checkCancellation(Cancellation);
    if (Sec.Size < 4 || !Sec.isExecutable() || !Img.isCodeAddress(Sec.VA))
      continue;
    const Segment *Seg = Img.getSegmentFor(Sec.VA);
    if (!Seg || Seg->Data.empty() || Sec.VA < Seg->VA)
      continue;
    const va_t SecEnd = Sec.VA + Sec.Size;
    const llvm::ArrayRef<uint8_t> SecBytes(Seg->Data.data(), Seg->Data.size());

    auto At = [&](va_t Addr) {
      const size_t Off = static_cast<size_t>(Addr - Seg->VA);
      if (Off >= Seg->Data.size())
        return llvm::ArrayRef<uint8_t>();
      return llvm::ArrayRef<uint8_t>(Seg->Data.data() + Off,
                                     Seg->Data.size() - Off);
    };

    // 16-byte MSVC alignment: SEH, callee-saved pushes, vtable thunks, frames.
    va_t Aligned = (Sec.VA + 15u) & ~static_cast<va_t>(15u);
    for (; Aligned + 3 < SecEnd; Aligned += 16) {
      func_detect_detail::checkCancellation(Cancellation);
      if (!isX86MsvcPrologue(At(Aligned)))
        continue;
      considerX86Prologue(Img, Dec, *Seg, Aligned, Occupied, Out,
                          UnsymbolizedX86Entries);
    }

    // Unaligned EBP frames: only visit 0x55 bytes instead of every address.
    const size_t DataN = static_cast<size_t>(std::min<va_t>(
        Seg->Data.size(), SecEnd > Seg->VA ? SecEnd - Seg->VA : 0));
    const uint8_t *Base = Seg->Data.data();
    const uint8_t *Cur = Base;
    const uint8_t *Limit = Base + DataN;
    while (Cur + 3 < Limit) {
      func_detect_detail::checkCancellation(Cancellation);
      const void *Hit =
          std::memchr(Cur, 0x55, static_cast<size_t>(Limit - Cur));
      if (!Hit)
        break;
      const size_t Off =
          static_cast<size_t>(static_cast<const uint8_t *>(Hit) - Base);
      const va_t Addr = Seg->VA + Off;
      Cur = static_cast<const uint8_t *>(Hit) + 1;
      if ((Addr & 15u) == 0)
        continue;
      if (!isX86EbpFrameSetup(At(Addr)))
        continue;
      considerX86Prologue(Img, Dec, *Seg, Addr, Occupied, Out,
                          UnsymbolizedX86Entries);
    }

    // Packed `sub esp` immediately after ret / int3 / nop.
    for (size_t Off = 1; Off + 3 < DataN; ++Off) {
      func_detect_detail::checkCancellation(Cancellation);
      if (!isX86FunctionEndBefore(SecBytes.data(), Off))
        continue;
      const va_t Addr = Seg->VA + Off;
      if ((Addr & 15u) == 0)
        continue;
      if (!isX86StackAdjustPrologue(At(Addr)))
        continue;
      considerX86Prologue(Img, Dec, *Seg, Addr, Occupied, Out,
                          UnsymbolizedX86Entries);
    }
  }
  Dec.setDetail(PrevDetail);
}

namespace {

/// How control reaches an address: falling into it or by a conditional
/// branch, falling into it right after a call, or only by a jump.
enum class X86Reach : uint8_t { Inside, AfterCall, Jump };

/// Whether the instruction at [\p Addr, \p Next) reads a table among the code:
/// one of its operands, which the loader records as naming data, names an
/// address in executable code.
bool readsX86TableAmongCode(const BinaryImage &Img, va_t Addr, va_t Next) {
  auto It = Img.DataAddressRelocOperands.lower_bound(Addr);
  return It != Img.DataAddressRelocOperands.end() && It->first < Next &&
         Img.hasExecutableCodeOwnerAt(It->second.TargetVA);
}

/// Walk the code a function at \p Entry reaches in [\p Entry, \p End):
/// falling through, by conditional and direct jumps, past calls that may
/// return, and into the \p Cases its tables among the code name.  \p Take
/// decides whether an address reached \p How is the function's code to walk
/// on.  The walk never enters a callee.  Returns whether the code has an
/// indirect jump the walk cannot follow, through which a computed goto may
/// reach any of the function's labels. Optionally records complete decoded
/// instruction spans; overlapping paths must be checked before using them.
template <typename TakeFn>
bool walkX86OwnCode(const BinaryImage &Img, Decoder &Dec, va_t Entry, va_t End,
                    llvm::ArrayRef<va_t> Cases, std::set<va_t> &Visited,
                    TakeFn Take,
                    const func_detect_detail::Cancellation *Control,
                    std::map<va_t, va_t> *DecodedSpans = nullptr) {
  bool Computed = false;
  std::vector<std::pair<va_t, X86Reach>> Work{{Entry, X86Reach::Inside}};
  for (va_t Case : Cases)
    Work.emplace_back(Case, X86Reach::Inside);
  while (!Work.empty()) {
    func_detect_detail::checkCancellation(Control);
    const auto [Addr, How] = Work.back();
    Work.pop_back();
    if (Addr < Entry || Addr >= End)
      continue;
    const Segment *Seg = Img.getSegmentFor(Addr);
    if (!Seg || Addr < Seg->VA || Addr - Seg->VA >= Seg->Data.size())
      continue;
    const size_t Off = static_cast<size_t>(Addr - Seg->VA);
    const llvm::ArrayRef<uint8_t> Bytes(Seg->Data.data() + Off,
                                        Seg->Data.size() - Off);
    if (!Take(Addr, How, Bytes))
      continue;
    if (!Visited.insert(Addr).second || Img.DataInCode.count(Addr) ||
        Img.CodePtrRelocSlots.count(Addr))
      continue;
    DecodedInsn DI;
    const int Size = Dec.decodeOneLight(Bytes.data(), Bytes.size(), Addr, DI);
    if (Size <= 0 || !DI.Raw || !DI.Raw->detail ||
        static_cast<va_t>(Size) > End - Addr ||
        !Img.hasExecutableCodeOwnerRange(Addr, Size))
      continue;
    const va_t Next = Addr + static_cast<va_t>(Size);
    const auto Data = Img.DataInCode.lower_bound(Addr);
    const auto Slot = Img.CodePtrRelocSlots.lower_bound(Addr);
    if ((Data != Img.DataInCode.end() && *Data < Next) ||
        (Slot != Img.CodePtrRelocSlots.end() && *Slot < Next) ||
        (Slot != Img.CodePtrRelocSlots.begin() &&
         Addr - *std::prev(Slot) < Img.getPointerSize()))
      continue;
    if (DecodedSpans)
      DecodedSpans->emplace(Addr, Next);
    const cs_x86 &X86 = DI.Raw->detail->x86;
    const bool Direct = X86.op_count >= 1 && X86.operands[0].type == X86_OP_IMM;
    const va_t Target =
        Direct ? static_cast<va_t>(X86.operands[0].imm) : InvalidVA;
    // `jmp [slot]` and `call [slot]` name the import the loader binds there.
    const bool ThroughSlot = !Direct && X86.op_count >= 1 &&
                             X86.operands[0].type == X86_OP_MEM &&
                             X86.operands[0].mem.base == X86_REG_INVALID &&
                             X86.operands[0].mem.index == X86_REG_INVALID;
    if (DI.Id == X86_INS_JMP) {
      if (Direct)
        Work.emplace_back(Target, X86Reach::Jump);
      else if (!ThroughSlot && !readsX86TableAmongCode(Img, Addr, Next))
        Computed = true;
      continue;
    }
    if (DI.Id == X86_INS_CALL) {
      va_t Callee = Target;
      if (ThroughSlot)
        Callee =
            static_cast<va_t>(static_cast<uint32_t>(X86.operands[0].mem.disp));
      if (Callee == InvalidVA ||
          !(ThroughSlot ? libc::isNoReturnLoadedTarget(Img, Callee)
                        : libc::isNoReturnTarget(Img, Callee)))
        Work.emplace_back(Next, X86Reach::AfterCall);
      continue;
    }
    if (Dec.isFunctionTerminator(DI))
      continue;
    if (Direct && isX86ConditionalJump(DI.Id))
      Work.emplace_back(Target, X86Reach::Inside);
    Work.emplace_back(Next, X86Reach::Inside);
  }
  return Computed;
}

} // namespace

std::set<va_t> FuncDetector::rejectX86InteriorEntries(
    const BinaryImage &Img, Decoder &Dec, const std::set<va_t> &Evidenced,
    const std::set<va_t> &Guessed,
    const std::vector<std::pair<va_t, va_t>> &TableCases) {
  std::set<va_t> Candidates;
  for (va_t Addr : UnsymbolizedX86Entries)
    if (!Evidenced.count(Addr) && Entries.count(Addr))
      Candidates.insert(Addr);
  for (va_t Addr : Guessed)
    if (!Evidenced.count(Addr) && Entries.count(Addr))
      Candidates.insert(Addr);
  if (Candidates.empty() || Evidenced.empty())
    return {};

  // An entry with other evidence owns the code up to the next one.
  const std::vector<va_t> Starts(Evidenced.begin(), Evidenced.end());
  auto ownerOf = [&](va_t Addr) -> std::optional<std::pair<va_t, va_t>> {
    const auto Next = std::upper_bound(Starts.begin(), Starts.end(), Addr);
    if (Next == Starts.begin())
      return std::nullopt;
    return std::make_pair(*std::prev(Next),
                          Next == Starts.end() ? InvalidVA : *Next);
  };
  // A pointer table among an owner's code names that owner's cases.
  std::map<va_t, std::vector<va_t>> Cases;
  for (const auto &[Slot, Target] : TableCases)
    if (const auto Owner = ownerOf(Slot);
        Owner && Target > Owner->first && Target < Owner->second)
      Cases[Owner->first].push_back(Target);
  std::set<va_t> Owners;
  for (va_t Addr : Candidates)
    if (const auto Owner = ownerOf(Addr))
      Owners.insert(Owner->first);

  std::set<va_t> Interior;
  const bool KeptDetail = Dec.detailEnabled(), KeptText = Dec.textEnabled();
  Dec.setText(true);
  Dec.setDetail(true);
  for (va_t Owner : Owners) {
    func_detect_detail::checkCancellation(Cancellation);
    const auto It = Cases.find(Owner);
    std::set<va_t> Visited;
    std::set<va_t> Reached;
    std::map<va_t, va_t> DecodedSpans;
    const bool Computed = walkX86OwnCode(
        Img, Dec, Owner, ownerOf(Owner)->second,
        It == Cases.end() ? llvm::ArrayRef<va_t>()
                          : llvm::ArrayRef<va_t>(It->second),
        Visited,
        [&](va_t Addr, X86Reach How, llvm::ArrayRef<uint8_t> Bytes) {
          Reached.insert(Addr);
          if (Addr == Owner || !Candidates.count(Addr))
            return true;
          // Only a jump can be a tail call, and a frame prologue right after
          // a call can follow one that does not return.
          if (How == X86Reach::Jump ||
              (How == X86Reach::AfterCall && isX86StrongFramePrologue(Bytes)))
            return false;
          Interior.insert(Addr);
          return true;
        },
        Cancellation, &DecodedSpans);
    // An unresolved computed transfer may enter an overlapping instruction
    // stream that the walk has not observed.
    if (Computed)
      continue;
    // Prologue and padding scans can also start in an instruction's operand.
    // Reject such guesses only when this owner's reachable decoding is
    // unambiguous. A jump into the operand, including one Take stopped at,
    // or any overlapping decode prevents that span from disproving an entry.
    va_t PreviousEnd = 0;
    for (const auto &[Addr, Next] : DecodedSpans) {
      func_detect_detail::checkCancellation(Cancellation);
      const bool OverlapsBefore = PreviousEnd > Addr;
      PreviousEnd = std::max(PreviousEnd, Next);
      const auto Other = Reached.upper_bound(Addr);
      if (OverlapsBefore || (Other != Reached.end() && *Other < Next))
        continue;
      for (auto Candidate = Candidates.upper_bound(Addr);
           Candidate != Candidates.end() && *Candidate < Next; ++Candidate)
        Interior.insert(*Candidate);
    }
  }
  Dec.setDetail(KeptDetail);
  Dec.setText(KeptText);

  for (va_t Addr : Interior) {
    func_detect_detail::checkCancellation(Cancellation);
    Entries.erase(Addr);
    UnsymbolizedX86Entries.erase(Addr);
  }
  return Interior;
}

std::set<va_t> FuncDetector::x86OwnCodeReached(const BinaryImage &Img,
                                               Decoder &Dec, va_t Entry,
                                               va_t End,
                                               llvm::ArrayRef<va_t> Cases,
                                               bool &Computed) {
  std::set<va_t> Visited;
  const bool KeptDetail = Dec.detailEnabled(), KeptText = Dec.textEnabled();
  Dec.setText(true);
  Dec.setDetail(true);
  Computed = walkX86OwnCode(
      Img, Dec, Entry, End, Cases, Visited,
      [](va_t, X86Reach, llvm::ArrayRef<uint8_t>) { return true; },
      Cancellation);
  Dec.setDetail(KeptDetail);
  Dec.setText(KeptText);
  return Visited;
}

} // namespace neverd
