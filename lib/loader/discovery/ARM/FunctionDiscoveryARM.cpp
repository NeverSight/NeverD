//===- FunctionDiscoveryARM.cpp - ARM32 import thunk scan --------*- C++
//-*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// ARM32 import thunk recognition and reachable ARM/Thumb mode discovery.
///
//===----------------------------------------------------------------------===//

#include "../FunctionDiscoveryDetail.h"

#include "neverd/libc/LibCNames.h"
#include "neverd/loader/ELF/ELFLoaderUtils.h"
#include "neverd/loader/FunctionDiscovery.h"
#include "neverd/loader/Limits.h"
#include "neverd/support/ARM/ARMTrap.h"
#include "neverd/support/BinaryEncoding.h"
#include "neverd/support/ISAEncoding.h"

#include "llvm/ADT/DenseMap.h"
#include "llvm/ADT/STLExtras.h"
#include "llvm/ADT/StringExtras.h"
#include "llvm/ADT/bit.h"
#include "llvm/Support/Error.h"

#include <array>
#include <capstone/arm.h>
#include <capstone/capstone.h>
#include <cassert>
#include <deque>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace neverd {

namespace {

bool readThumbMovImm16(const uint8_t *P, bool IsMovT, uint16_t &Imm) {
  uint16_t Op1 = readLE<uint16_t>(P);
  uint16_t Op2 = readLE<uint16_t>(P + 2);
  if ((Op1 & arm::kThumbMovImmOp1Mask) !=
          (IsMovT ? arm::kThumbMovTIPOp1 : arm::kThumbMovWIPOp1) ||
      (Op2 & arm::kThumbMovIPOp2Mask) != arm::kThumbMovIPOp2)
    return false;
  Imm = uint16_t((Op2 & 0x00ff) | ((Op2 >> 4) & 0x0700) |
                 ((Op1 << 1) & 0x0800) | ((Op1 & 0x000f) << 12));
  return true;
}

size_t scanThumbImportThunks(BinaryImage &Img, const Segment &Seg,
                             const std::map<va_t, size_t> &Targets,
                             std::set<va_t> &Existing,
                             const ImportThunkCandidates &Candidates) {
  const uint8_t *D = Seg.Data.data();
  size_t N = Seg.Data.size();
  if (N < arm::kThumbImportThunkLen)
    return 0;

  size_t Added = 0;
  for (size_t I = 0; I + arm::kThumbImportThunkLen <= N;
       I += arm::kThumbInsnSize) {
    uint16_t Low;
    uint16_t High;
    if (!readThumbMovImm16(D + I, false, Low) ||
        !readThumbMovImm16(D + I + arm::kThumbMovImmInsnSize, true, High) ||
        readLE<uint32_t>(D + I + 2 * arm::kThumbMovImmInsnSize) !=
            arm::kThumbLdrPCFromIP)
      continue;

    va_t Target = normalizeCodeAddress(uint32_t(Low) | (uint32_t(High) << 16),
                                       Img.Arch, Img.Mode);
    auto TargetIt = Targets.find(Target);
    if (TargetIt == Targets.end())
      continue;
    va_t ThunkVA = normalizeCodeAddress(Seg.VA + I, Img.Arch, Img.Mode);
    if (!Img.isCodeRange(ThunkVA, arm::kThumbImportThunkLen) ||
        !Candidates.allows(ThunkVA, arm::kThumbImportThunkLen) ||
        Img.instructionModeAt(ThunkVA) != InstructionMode::Thumb ||
        Img.instructionModeAt(ThunkVA + arm::kThumbImportThunkLen - 1) !=
            InstructionMode::Thumb)
      continue;
    Img.recordImportStub(ThunkVA, TargetIt->second);
    if (!Existing.insert(ThunkVA).second)
      continue;
    Img.Symbols.push_back(Symbol::makeFunc(ThunkVA, arm::kThumbImportThunkLen));
    ++Added;
  }
  return Added;
}

/// Disjoint byte ranges proven to hold data.
class LiteralRanges {
public:
  bool overlaps(va_t Begin, va_t End) const {
    auto It = Ranges.lower_bound(End);
    if (It == Ranges.begin())
      return false;
    return std::prev(It)->second > Begin;
  }

  /// Whether \p Pred holds for any of the ranges, merged where they touch.
  template <typename Fn> bool any(Fn Pred) const {
    for (const auto &[Begin, End] : Ranges)
      if (Pred(Begin, End))
        return true;
    return false;
  }

  /// Adds [Begin, End), and says whether any of it was new.
  bool insert(va_t Begin, va_t End) {
    if (Begin >= End)
      return false;
    auto It = Ranges.upper_bound(Begin);
    if (It != Ranges.begin() && std::prev(It)->second >= End)
      return false;
    if (It != Ranges.begin() && std::prev(It)->second >= Begin)
      --It;
    while (It != Ranges.end() && It->first <= End) {
      Begin = std::min(Begin, It->first);
      End = std::max(End, It->second);
      It = Ranges.erase(It);
    }
    Ranges.emplace(Begin, End);
    return true;
  }

private:
  std::map<va_t, va_t> Ranges;
};

/// The bytes a PC-relative load reads: its literal, which is data.
std::optional<std::pair<va_t, va_t>> literalRead(const cs_insn &Insn,
                                                 InstructionMode Mode) {
  uint64_t Size = 0;
  switch (Insn.id) {
  case ARM_INS_LDR:
  case ARM_INS_VLDR:
    Size = 4;
    break;
  case ARM_INS_LDRB:
  case ARM_INS_LDRSB:
    Size = 1;
    break;
  case ARM_INS_LDRH:
  case ARM_INS_LDRSH:
    Size = 2;
    break;
  case ARM_INS_LDRD:
    Size = 8;
    break;
  default:
    return std::nullopt;
  }
  const cs_arm &Arm = Insn.detail->arm;
  for (uint8_t Index = 0; Index < Arm.op_count; ++Index) {
    const cs_arm_op &Op = Arm.operands[Index];
    if (Insn.id == ARM_INS_VLDR && Op.type == ARM_OP_REG &&
        Op.reg >= ARM_REG_D0 && Op.reg <= ARM_REG_D31)
      Size = 8;
    if (Op.type != ARM_OP_MEM || Op.mem.base != ARM_REG_PC ||
        Op.mem.index != ARM_REG_INVALID)
      continue;
    // Thumb reads relative to the word-aligned PC, ARM to the instruction
    // plus 8; an ARM offset below the PC is a subtracted one.
    const int64_t Disp = Op.subtracted ? -static_cast<int64_t>(Op.mem.disp)
                                       : static_cast<int64_t>(Op.mem.disp);
    const va_t Base = Mode == InstructionMode::Thumb
                          ? (Insn.address + 4) & ~va_t(3)
                          : Insn.address + arm::kPCBias;
    const va_t Begin = Base + static_cast<va_t>(Disp);
    return std::make_pair(Begin, Begin + Size);
  }
  return std::nullopt;
}

/// What a decoded instruction does with control, for telling the routines
/// that return to their callers from those that do not.
struct ControlStep {
  uint32_t Size = 0;
  /// A direct branch's or call's target.
  va_t Target = InvalidVA;
  bool Call = false;
  bool Jump = false;
  bool Conditional = false;
  /// Writes the PC other than by a direct branch or call: a return, or a
  /// jump through a register or memory, which goes nowhere this pass knows.
  bool Leaves = false;
  /// Never goes on: a trap, or a system call that ends the process or the
  /// thread.
  bool Stops = false;
};

/// The Linux system calls a 32-bit ARM EABI program ends with: exit and
/// exit_group.  Neither returns.
bool isExitSyscall(uint32_t Number) { return Number == 1 || Number == 248; }

/// The value a `mov`/`movw` or a PC-relative `ldr` gives register r7, the
/// system call number, or nothing when it gives r7 some other value.
std::optional<uint32_t> r7Value(const cs_insn &Insn, InstructionMode Mode,
                                const BinaryImage &Img) {
  const cs_arm &Arm = Insn.detail->arm;
  if (Arm.op_count < 2 || Arm.operands[0].type != ARM_OP_REG ||
      Arm.operands[0].reg != ARM_REG_R7 || Arm.cc < ARMCC_AL)
    return std::nullopt;
  if ((Insn.id == ARM_INS_MOV || Insn.id == ARM_INS_MOVW) &&
      Arm.op_count == 2 && Arm.operands[1].type == ARM_OP_IMM)
    return static_cast<uint32_t>(Arm.operands[1].imm);
  if (Insn.id == ARM_INS_LDR)
    if (const auto Literal = literalRead(Insn, Mode))
      if (const uint8_t *Bytes = Img.readVA(Literal->first, 4))
        return readLE<uint32_t>(Bytes);
  return std::nullopt;
}

/// The register a PC-relative word load writes and the word it loads.
std::optional<std::pair<unsigned, uint32_t>>
pcRelativeWord(const cs_insn &Insn, InstructionMode Mode,
               const BinaryImage &Img) {
  const cs_arm &Arm = Insn.detail->arm;
  if (Insn.id != ARM_INS_LDR || Arm.cc < ARMCC_AL || Arm.op_count != 2 ||
      Arm.operands[0].type != ARM_OP_REG)
    return std::nullopt;
  const auto Literal = literalRead(Insn, Mode);
  if (!Literal || Literal->second - Literal->first != 4)
    return std::nullopt;
  const uint8_t *Bytes = Img.readVA(Literal->first, 4);
  if (!Bytes)
    return std::nullopt;
  return std::make_pair(Arm.operands[0].reg, readLE<uint32_t>(Bytes));
}

/// The register `add rN, pc` (Thumb) or `add rN, pc, rN` (ARM, either
/// operand order) adds the PC to: the second half of the position-independent
/// `ldr rN, [pc, #k]` that takes an address.
std::optional<unsigned> pcAddedRegister(const cs_insn &Insn) {
  const cs_arm &Arm = Insn.detail->arm;
  if (Insn.id != ARM_INS_ADD || Arm.cc < ARMCC_AL || Arm.update_flags)
    return std::nullopt;
  for (uint8_t Index = 0; Index < Arm.op_count; ++Index)
    if (Arm.operands[Index].type != ARM_OP_REG ||
        Arm.operands[Index].shift.type != ARM_SFT_INVALID)
      return std::nullopt;
  const unsigned Dest = Arm.operands[0].reg;
  if (Arm.op_count == 2 && Arm.operands[1].reg == ARM_REG_PC)
    return Dest;
  if (Arm.op_count == 3 &&
      ((Arm.operands[1].reg == ARM_REG_PC && Arm.operands[2].reg == Dest) ||
       (Arm.operands[1].reg == Dest && Arm.operands[2].reg == ARM_REG_PC)))
    return Dest;
  return std::nullopt;
}

/// An address and the instruction set its bytes are decoded in.  Bytes a
/// path reached in the wrong mode decode as something else, so what a
/// routine does is read only from its own mode.
using CodeKey = std::pair<va_t, InstructionMode>;

/// Whether a routine entered at \p Entry can return to its caller, as far as
/// the instructions \p StepAt describes show: some path from it reaches a
/// return or a jump nobody can follow, falls into or jumps to another routine
/// that can return, or runs into code that was not decoded.  Only a routine
/// every path of which ends in a trap, an exiting system call, or a call to a
/// routine \p IsNoReturn names cannot.
bool mayReturn(
    const CodeKey &Entry,
    llvm::function_ref<std::optional<ControlStep>(const CodeKey &)> StepAt,
    const std::set<CodeKey> &Entries,
    llvm::function_ref<bool(va_t)> IsNoReturn) {
  const InstructionMode Mode = Entry.second;
  std::vector<va_t> Pending{Entry.first};
  std::unordered_set<va_t> Seen;
  while (!Pending.empty()) {
    const va_t Address = Pending.back();
    Pending.pop_back();
    if (!Seen.insert(Address).second)
      continue;
    // A jump to another routine, or a path running into one, is a tail
    // call: it returns if that routine does.
    if (Address != Entry.first && Entries.count({Address, Mode})) {
      if (!IsNoReturn(Address))
        return true;
      continue;
    }
    const std::optional<ControlStep> Found = StepAt({Address, Mode});
    if (!Found)
      return true;
    const ControlStep &Step = *Found;
    const va_t Next = Address + Step.Size;
    if (Step.Stops) {
      if (Step.Conditional)
        Pending.push_back(Next);
      continue;
    }
    if (Step.Leaves)
      return true;
    if (Step.Call) {
      if (Step.Conditional || Step.Target == InvalidVA ||
          !IsNoReturn(Step.Target))
        Pending.push_back(Next);
      continue;
    }
    if (Step.Jump) {
      if (Step.Target == InvalidVA)
        return true;
      Pending.push_back(Step.Target);
      if (Step.Conditional)
        Pending.push_back(Next);
      continue;
    }
    Pending.push_back(Next);
  }
  return false;
}

} // anonymous namespace

llvm::Error
applyARMFunctionModeHints(BinaryImage &Img,
                          const std::map<va_t, InstructionMode> &Hints) {
  if (Hints.empty())
    return llvm::Error::success();
  if (Img.Arch != Arch::ARM)
    return llvm::createStringError(
        llvm::inconvertibleErrorCode(),
        "ARM function mode hints require a 32-bit ARM image");

  for (const auto &[Address, Mode] : Hints) {
    const auto Fail = [&](llvm::StringRef Reason) {
      return llvm::createStringError(llvm::inconvertibleErrorCode(),
                                     "ARM function mode hint at 0x" +
                                         llvm::utohexstr(Address) + ": " +
                                         Reason);
    };
    if (Mode != InstructionMode::ARM && Mode != InstructionMode::Thumb)
      return Fail("mode must be ARM or Thumb");
    if (Address > UINT32_MAX ||
        (Address & (Mode == InstructionMode::Thumb ? 1u : 3u)) != 0)
      return Fail("entry address is outside or misaligned for the mode");
    if (Img.ARMRequiredMode != InstructionMode::Default &&
        Img.ARMRequiredMode != Mode)
      return Fail("mode conflicts with the image's required instruction state");
    if (const ARMCodeRegion *Region = Img.armMappingRegionAt(Address)) {
      if (Region->Kind == ARMCodeRegionKind::Data ||
          (Region->Kind == ARMCodeRegionKind::ARM) !=
              (Mode == InstructionMode::ARM))
        return Fail("mode conflicts with an ARM mapping region");
    }
    const auto Existing = Img.ARMCodeModeEntries.find(Address);
    if (Existing != Img.ARMCodeModeEntries.end() && Existing->second != Mode)
      return Fail("mode conflicts with an exact symbol or relocation");
    const uint64_t Width = Mode == InstructionMode::Thumb ? 2 : 4;
    if (!Img.isCodeRange(Address, Width) || !Img.readVA(Address, Width))
      return Fail("entry has no materialized executable instruction");
  }

  // Commit only after validating every hint, so no partial caller assertion
  // can affect subsequent loader work on a rejected input.
  for (const auto &[Address, Mode] : Hints) {
    Img.ARMCodeModeEntries.try_emplace(Address, Mode);
    if (std::none_of(Img.Symbols.begin(), Img.Symbols.end(),
                     [Address](const Symbol &Sym) {
                       return Sym.IsFunc && Sym.Addr == Address;
                     }))
      Img.Symbols.push_back(Symbol::makeFunc(Address));
  }
  return llvm::Error::success();
}

llvm::Error
verifyARMFunctionModeHints(const BinaryImage &Img,
                           const std::map<va_t, InstructionMode> &Hints) {
  for (const auto &[Address, Mode] : Hints) {
    if (!Img.ARMReachabilityConstrained) {
      const Segment *Seg = Img.getSegmentFor(Address);
      if (!Seg || Address < Seg->VA || Address - Seg->VA >= Seg->Data.size())
        return llvm::createStringError(
            llvm::inconvertibleErrorCode(),
            "ARM function mode hint has no materialized entry instruction");
      csh Handle = 0;
      const cs_mode DecodeMode =
          Mode == InstructionMode::Thumb ? CS_MODE_THUMB : CS_MODE_ARM;
      if (cs_open(CS_ARCH_ARM, DecodeMode, &Handle) != CS_ERR_OK)
        return llvm::createStringError(
            llvm::inconvertibleErrorCode(),
            "ARM function mode hint could not initialize the disassembler");
      const size_t Offset = static_cast<size_t>(Address - Seg->VA);
      cs_insn *Insn = nullptr;
      const size_t Count = cs_disasm(
          Handle, Seg->Data.data() + Offset,
          std::min<size_t>(4, Seg->Data.size() - Offset), Address, 1, &Insn);
      const bool Valid = Count == 1 && Img.isCodeRange(Address, Insn->size);
      if (Insn)
        cs_free(Insn, Count);
      cs_close(&Handle);
      if (!Valid)
        return llvm::createStringError(
            llvm::inconvertibleErrorCode(),
            "ARM function mode hint at 0x" + llvm::utohexstr(Address) +
                " does not decode an executable instruction in the asserted "
                "mode");
      continue;
    }
    const auto It = std::upper_bound(
        Img.ARMReachableCodeRegions.begin(), Img.ARMReachableCodeRegions.end(),
        Address, [](va_t Entry, const ARMCodeRegion &Region) {
          return Entry < Region.Start;
        });
    if (It != Img.ARMReachableCodeRegions.begin()) {
      const ARMCodeRegion &Region = *std::prev(It);
      if (Address < Region.End && (Region.Kind == ARMCodeRegionKind::Thumb) ==
                                      (Mode == InstructionMode::Thumb))
        continue;
    }
    return llvm::createStringError(
        llvm::inconvertibleErrorCode(),
        "ARM function mode hint at 0x" + llvm::utohexstr(Address) +
            " did not decode a reachable instruction in the asserted mode");
  }
  return llvm::Error::success();
}

void recordARMCodePointerMode(BinaryImage &Img, uint64_t Pointer,
                              ARMCodePointerUse Use) {
  if (Img.Arch != Arch::ARM || Img.IsRelocatable || Pointer > UINT32_MAX)
    return;
  const bool Thumb = (Pointer & 1u) != 0;
  if (!Thumb && Use == ARMCodePointerUse::Stored)
    return;
  const va_t Address = clearThumbBit(static_cast<va_t>(Pointer));
  const InstructionMode Mode =
      Thumb ? InstructionMode::Thumb : InstructionMode::ARM;
  const uint64_t Width = Thumb ? 2 : 4;
  if (!Img.isCodeRange(Address, Width) || !Img.readVA(Address, Width))
    return;
  // Without a section table a whole executable segment reads as code,
  // including any read-only data the linker placed in it, and a pointer to
  // an odd byte of a string there has bit 0 set too.
  if (Use == ARMCodePointerUse::Stored && !Img.getSectionFor(Address))
    return;
  const auto Contradiction = [&](llvm::StringRef What) {
    if (Use == ARMCodePointerUse::Called)
      Img.addLoadDiagnostic("elf.arm_code_pointer_mode",
                            "the " + std::string(Thumb ? "Thumb" : "ARM") +
                                " code pointer the runtime calls at 0x" +
                                llvm::utohexstr(Address) + " contradicts " +
                                What.str());
  };
  // `blx` to an ARM address with bit 1 set is unpredictable.
  if ((Address & (Width - 1)) != 0)
    return Contradiction("the alignment of ARM code");
  if (Img.ARMRequiredMode != InstructionMode::Default &&
      Img.ARMRequiredMode != Mode)
    return Contradiction("the image's required instruction state");
  if (const ARMCodeRegion *Region = Img.armMappingRegionAt(Address)) {
    if (Region->Kind !=
        (Thumb ? ARMCodeRegionKind::Thumb : ARMCodeRegionKind::ARM))
      Contradiction("its mapping symbol");
    return;
  }
  const auto [Existing, Inserted] =
      Img.ARMCodeModeEntries.try_emplace(Address, Mode);
  if (!Inserted && Existing->second != Mode)
    Contradiction("the mode its symbol or the entry point states");
}

void recordARMStoredCodePointerModes(BinaryImage &Img) {
  if (Img.Arch != Arch::ARM || Img.IsRelocatable)
    return;
  for (const BaseRelocation &Slot : Img.BaseRelocations)
    if (const uint8_t *Bytes = Img.readVA(Slot.Address, sizeof(uint32_t)))
      recordARMCodePointerMode(Img, readLE<uint32_t>(Bytes),
                               ARMCodePointerUse::Stored);
}

namespace {
/// The guarded ranges of an image's exception call sites and their landing
/// pads, by range start.
struct CallSitePads {
  struct Site {
    va_t Begin = 0;
    va_t End = 0;
    va_t Pad = 0;
  };
  std::vector<Site> Sites;
  /// The longest guarded range, which bounds the search for the ranges an
  /// address lies in.
  va_t Longest = 0;

  explicit CallSitePads(const BinaryImage &Img) {
    for (const ExceptionFunction &Function : Img.ExceptionMetadata.Functions)
      if (Function.Itanium)
        for (const ItaniumCallSite &Call : Function.Itanium->CallSites)
          if (Call.LandingPadVA && Call.GuardedRange.isValid()) {
            Sites.push_back({Call.GuardedRange.Begin, Call.GuardedRange.End,
                             Call.LandingPadVA});
            Longest = std::max(Longest, Call.GuardedRange.size());
          }
    llvm::sort(Sites, [](const Site &Left, const Site &Right) {
      return Left.Begin < Right.Begin;
    });
  }

  /// Calls \p Visit with the pad of each call site guarding \p Address.
  template <typename Fn> void forEachPadAt(va_t Address, Fn Visit) const {
    auto It = std::upper_bound(
        Sites.begin(), Sites.end(), Address,
        [](va_t Value, const Site &Entry) { return Value < Entry.Begin; });
    while (It != Sites.begin()) {
      --It;
      if (Address - It->Begin >= Longest)
        break;
      if (Address < It->End)
        Visit(It->Pad);
    }
  }
};

using ModeAt = std::pair<va_t, InstructionMode>;

struct Disassemblers {
  csh ARM = 0;
  csh Thumb = 0;
  cs_insn *ARMInsn = nullptr;
  cs_insn *ThumbInsn = nullptr;
  ~Disassemblers() {
    if (ARMInsn)
      cs_free(ARMInsn, 1);
    if (ThumbInsn)
      cs_free(ThumbInsn, 1);
    if (ARM)
      cs_close(&ARM);
    if (Thumb)
      cs_close(&Thumb);
  }
};

std::string hexAddress(va_t Address) { return "0x" + llvm::utohexstr(Address); }

const char *modeName(InstructionMode Mode) {
  return Mode == InstructionMode::Thumb ? "Thumb" : "ARM";
}

ARMCodeRegionKind regionKind(InstructionMode Mode) {
  return Mode == InstructionMode::Thumb ? ARMCodeRegionKind::Thumb
                                        : ARMCodeRegionKind::ARM;
}

/// The bit of general-purpose register \p Reg in InstructionFacts::Written,
/// or zero for any other register.
uint16_t gprBit(unsigned Reg) {
  if (Reg >= ARM_REG_R0 && Reg <= ARM_REG_R12)
    return uint16_t(1u << (Reg - ARM_REG_R0));
  switch (Reg) {
  case ARM_REG_SP:
    return uint16_t(1u << 13);
  case ARM_REG_LR:
    return uint16_t(1u << 14);
  case ARM_REG_PC:
    return uint16_t(1u << 15);
  default:
    return 0;
  }
}

constexpr uint8_t kNoRegister = 0xff;

/// The number of general-purpose register \p Reg, or kNoRegister.
uint8_t gprIndex(unsigned Reg) {
  const uint16_t Bit = gprBit(Reg);
  return Bit ? static_cast<uint8_t>(llvm::countr_zero(Bit)) : kNoRegister;
}

/// What the walk reads of the instruction at one address, decoded in one
/// mode.  It depends on the bytes and the mode alone, so a pass that starts
/// again reads it instead of decoding the instruction again.
struct InstructionFacts {
  enum : uint16_t {
    // Decoded as Thumb.
    Thumb = 1u << 0,
    Call = 1u << 1,
    Jump = 1u << 2,
    Conditional = 1u << 3,
    // `blx #imm`: the target runs in the other instruction set.
    SwitchesMode = 1u << 4,
    Trap = 1u << 5,
    // `svc #0`: a system call, which may end the program.
    SupervisorCall = 1u << 6,
    WritesR7 = 1u << 7,
    // The value it gives r7, in R7, is known.
    KnownR7 = 1u << 8,
    ReadsLiteral = 1u << 9,
    LoadsWord = 1u << 10,
    AddsPC = 1u << 11,
  };
  /// The first non-negative immediate of a call or jump: its target.
  va_t Target = InvalidVA;
  /// Where the literal a PC-relative load reads starts, from the instruction.
  int32_t LiteralOffset = 0;
  uint32_t R7 = 0;
  /// The word a PC-relative load gives LoadedRegister.
  uint32_t LoadedWord = 0;
  uint16_t Flags = 0;
  /// The general-purpose registers it writes, as gprBit.
  uint16_t Written = 0;
  /// Two or four; zero where no instruction decodes.
  uint8_t Size = 0;
  uint8_t LiteralSize = 0;
  /// The registers, as gprIndex, a PC-relative word load writes and
  /// `add rN, pc` adds the PC to.
  uint8_t LoadedRegister = kNoRegister;
  uint8_t AddedRegister = kNoRegister;

  bool has(uint16_t Flag) const { return (Flags & Flag) != 0; }
};

/// Where the facts of an instruction are kept.  Most instructions do nothing
/// the walk follows besides writing registers, and their facts fit in the
/// slot itself; the others are kept in a list the slot indexes.  A zero slot
/// has no facts yet.
namespace fact_slot {
constexpr uint32_t Inline = 1u << 31;
constexpr uint32_t InlineThumb = 1u << 30;
constexpr uint32_t InlineWide = 1u << 29;
constexpr uint32_t InlineConditional = 1u << 28;
// No instruction decodes there.
constexpr uint32_t InlineInvalid = 1u << 27;
constexpr uint16_t InlineFlags =
    InstructionFacts::Thumb | InstructionFacts::Conditional;

bool fitsInline(const InstructionFacts &F) {
  return (F.Flags & ~InlineFlags) == 0;
}

uint32_t encode(const InstructionFacts &F) {
  uint32_t Slot = Inline | F.Written;
  if (F.has(InstructionFacts::Thumb))
    Slot |= InlineThumb;
  if (F.has(InstructionFacts::Conditional))
    Slot |= InlineConditional;
  if (F.Size == 0)
    Slot |= InlineInvalid;
  else if (F.Size == 4)
    Slot |= InlineWide;
  return Slot;
}

InstructionFacts decode(uint32_t Slot) {
  InstructionFacts F;
  F.Written = static_cast<uint16_t>(Slot);
  if (Slot & InlineThumb)
    F.Flags |= InstructionFacts::Thumb;
  if (Slot & InlineConditional)
    F.Flags |= InstructionFacts::Conditional;
  if (!(Slot & InlineInvalid))
    F.Size = (Slot & InlineWide) ? 4 : 2;
  return F;
}
} // namespace fact_slot

/// Per-halfword state of the code the walk reaches, in pages allocated as it
/// reaches them.  Every ARM and Thumb instruction starts on a halfword and is
/// two or four bytes long, so one entry per halfword describes them all.
class HalfwordMap {
public:
  /// What the current pass decoded at a halfword.
  enum : uint8_t {
    // An instruction starts here.
    Start = 1u << 0,
    // It was decoded as Thumb.
    ThumbStart = 1u << 1,
    // It is four bytes long.
    Wide = 1u << 2,
    // An `svc #0` its path knew to call exit or exit_group.
    Exits = 1u << 3,
  };

  /// One plus the index of the facts of the instruction at \p Address in the
  /// mode first decoded there, or zero before it is decoded.
  uint32_t &factSlot(va_t Address) {
    return page(Address).FactSlots[index(Address)];
  }
  uint32_t factSlotIfAny(va_t Address) {
    const Page *P = find(Address);
    return P ? P->FactSlots[index(Address)] : 0;
  }

  /// What the current pass decoded at \p Address, which starts an
  /// instruction only when even.
  uint8_t decoded(va_t Address) {
    if (Address & 1)
      return 0;
    const Page *P = find(Address);
    return P ? P->Decoded[index(Address)] : 0;
  }
  uint8_t &decodedRef(va_t Address) {
    return page(Address).Decoded[index(Address)];
  }

  void clearDecoded() {
    for (auto &Entry : Pages)
      Entry.second->Decoded.fill(0);
  }

  /// Calls \p Visit with the address and state of every instruction the
  /// current pass decoded, in address order.
  template <typename Fn> void forEachDecoded(Fn Visit) const {
    std::vector<va_t> Keys;
    Keys.reserve(Pages.size());
    for (const auto &Entry : Pages)
      Keys.push_back(Entry.first);
    llvm::sort(Keys);
    for (const va_t Key : Keys) {
      const Page &P = *Pages.find(Key)->second;
      for (size_t Index = 0; Index < kPageHalfwords; ++Index)
        if (P.Decoded[Index] & Start)
          Visit((Key << kPageShift) | (va_t(Index) << 1), P.Decoded[Index]);
    }
  }

private:
  static constexpr unsigned kPageShift = 12;
  static constexpr size_t kPageHalfwords = size_t(1) << (kPageShift - 1);
  struct Page {
    std::array<uint32_t, kPageHalfwords> FactSlots{};
    std::array<uint8_t, kPageHalfwords> Decoded{};
  };
  llvm::DenseMap<va_t, std::unique_ptr<Page>> Pages;
  va_t LastKey = InvalidVA;
  Page *Last = nullptr;

  static size_t index(va_t Address) {
    return static_cast<size_t>(Address >> 1) & (kPageHalfwords - 1);
  }
  Page *find(va_t Address) {
    const va_t Key = Address >> kPageShift;
    if (Key == LastKey)
      return Last;
    const auto It = Pages.find(Key);
    if (It == Pages.end())
      return nullptr;
    LastKey = Key;
    Last = It->second.get();
    return Last;
  }
  Page &page(va_t Address) {
    if (Page *P = find(Address))
      return *P;
    const va_t Key = Address >> kPageShift;
    std::unique_ptr<Page> &Slot = Pages[Key];
    Slot = std::make_unique<Page>();
    LastKey = Key;
    Last = Slot.get();
    return *Last;
  }
};

InstructionMode decodedMode(uint8_t Bits) {
  return (Bits & HalfwordMap::ThumbStart) ? InstructionMode::Thumb
                                          : InstructionMode::ARM;
}

va_t decodedEnd(va_t Address, uint8_t Bits) {
  return Address + ((Bits & HalfwordMap::Wide) ? 4 : 2);
}
} // namespace

struct ARMModeDiscovery::State {
  BinaryImage &Img;
  const char *Format;
  Disassemblers Dec;
  // A PC-relative load proves that what it reads is data: the literal pool a
  // function keeps after its code. The bytes after a call are code only when
  // the callee returns, and after one that does not, such as `bl abort`, the
  // pool follows; decoded as instructions, it runs across the code after it.
  // A path therefore stops where it would enter proven data. A pass that
  // finds literals it did not start with, and meets a conflict or decoded
  // across one of them, is repeated with every literal known from the start.
  // A conflict no new literal explains still fails.
  LiteralRanges Literals;
  // Nor does a path go on after a call to a routine that never returns: an
  // import or symbol the no-return list names, or a routine every decoded
  // path of which ends in a trap, an exiting system call, or such a call.
  // The bytes after the call belong to whatever follows -- often a routine in
  // the other instruction set.  A pass that proves routines no-return it did
  // not know of is repeated without the paths it followed past their calls.
  // A veneer names its import from its own instructions, which do not depend
  // on what the loader has recorded.  And a shared library's veneer for a
  // routine the library defines itself goes to that definition unless
  // another module interposes one, so the definition's own code settles
  // whether it returns: the throw helpers of a C++ runtime are no-return
  // without being on any list.
  std::optional<libc::NoReturnTargetIndex> NamedNoReturn;
  std::set<va_t> NoReturnVeneers;
  std::map<va_t, va_t> VeneerDefinitions;
  std::set<va_t> InferredNoReturn;
  // What each pass decoded, one entry per instruction: a branch into an
  // instruction's interior is a conflict rather than code decoded from
  // there.  And the facts of every instruction any pass decoded.
  HalfwordMap Halfwords;
  size_t DecodedCount = 0;
  std::deque<InstructionFacts> Facts;
  // The fact slots of the second mode decoded at an address, by address.
  llvm::DenseMap<va_t, uint32_t> OtherModeFactSlots;
  std::map<va_t, InstructionMode> VeneerCandidates;
  // Thumb code addresses the code takes: position-independent code loads
  // `routine + 1 - (here + 4)` and adds the PC, and what it gets is the
  // routine's address with bit 0 set.
  std::set<va_t> TakenThumbAddresses;
  std::set<CodeKey> Entries;
  // The first conflict of a pass, which fails only if the pass found no
  // literal it did not start with.
  std::optional<std::string> Conflict;
  bool FoundLiterals = false;
  // The branch each target was first reached from, for the diagnostics.
  std::map<ModeAt, va_t> Origins;
  // The landing pads a pass follows, once the exception tables are read.
  std::optional<CallSitePads> Pads;
  // Whether the discovery decoded the image and published what it reached.
  bool Published = false;

  // The owner's count of the instructions disassembled.
  size_t &Disassembled;

  State(BinaryImage &Image, size_t &DisassembledCount)
      : Img(Image),
        Format(Image.Format == BinaryFormat::MachO ? "macho" : "elf"),
        Disassembled(DisassembledCount) {}

  static constexpr size_t kMaxDiscoveredInstructions =
      ::neverd::limits::loader::kFunctionDiscoveryARMMaxDiscoveredInstructions;

  bool isNoReturn(va_t Target) const {
    if (const auto It = VeneerDefinitions.find(Target);
        It != VeneerDefinitions.end())
      Target = It->second;
    return InferredNoReturn.count(Target) || NoReturnVeneers.count(Target) ||
           NamedNoReturn->contains(Img, Target);
  }

  std::string from(va_t Start, InstructionMode Mode) const {
    const auto It = Origins.find({Start, Mode});
    return hexAddress(Start) +
           (It == Origins.end()
                ? std::string(", an entry")
                : ", the target of the branch at " + hexAddress(It->second));
  }

  llvm::Error open() {
    if (cs_open(CS_ARCH_ARM, CS_MODE_ARM, &Dec.ARM) != CS_ERR_OK ||
        cs_open(CS_ARCH_ARM, CS_MODE_THUMB, &Dec.Thumb) != CS_ERR_OK ||
        cs_option(Dec.ARM, CS_OPT_DETAIL, CS_OPT_ON) != CS_ERR_OK ||
        cs_option(Dec.Thumb, CS_OPT_DETAIL, CS_OPT_ON) != CS_ERR_OK ||
        !(Dec.ARMInsn = cs_malloc(Dec.ARM)) ||
        !(Dec.ThumbInsn = cs_malloc(Dec.Thumb)))
      return llvm::make_error<llvm::StringError>(
          std::string(Format) +
              ": ARM mode discovery could not initialize the disassembler",
          llvm::inconvertibleErrorCode());
    NamedNoReturn.emplace(Img);
    std::unordered_map<std::string_view, va_t> Defined;
    for (const Symbol &Sym : Img.Symbols)
      if (Sym.IsFunc && !Sym.Name.empty() && Img.isCodeAddress(Sym.Addr))
        Defined.try_emplace(Sym.Name, Sym.Addr);
    for (const auto &[Veneer, Import] : elf_loader::findARMPLTVeneers(Img)) {
      const std::string &Name = Img.Imports[Import].Name;
      if (libc::isNoReturnFunction(Name))
        NoReturnVeneers.insert(Veneer);
      else if (const auto It = Defined.find(Name); It != Defined.end())
        VeneerDefinitions.emplace(Veneer, It->second);
    }
    return llvm::Error::success();
  }

  /// Decode the instruction at \p Cur of \p Seg in \p Mode.
  InstructionFacts decodeFacts(va_t Cur, InstructionMode Mode,
                               const Segment &Seg);

  bool slotIsThumb(uint32_t Slot) const {
    return (Slot & fact_slot::Inline)
               ? (Slot & fact_slot::InlineThumb) != 0
               : Facts[Slot - 1].has(InstructionFacts::Thumb);
  }

  InstructionFacts factsOf(uint32_t Slot) const {
    return (Slot & fact_slot::Inline) ? fact_slot::decode(Slot)
                                      : Facts[Slot - 1];
  }

  /// The facts of the instruction at \p Cur of \p Seg in \p Mode, decoded
  /// once; a zero Size where no instruction decodes.
  InstructionFacts factsAt(va_t Cur, InstructionMode Mode, const Segment &Seg) {
    const bool Thumb = Mode == InstructionMode::Thumb;
    uint32_t *Slot = &Halfwords.factSlot(Cur);
    if (*Slot) {
      if (slotIsThumb(*Slot) == Thumb)
        return factsOf(*Slot);
      Slot = &OtherModeFactSlots[Cur];
      if (*Slot)
        return factsOf(*Slot);
    }
    const InstructionFacts F = decodeFacts(Cur, Mode, Seg);
    if (fact_slot::fitsInline(F)) {
      *Slot = fact_slot::encode(F);
    } else {
      Facts.push_back(F);
      assert(Facts.size() < fact_slot::Inline && "fact slots overflow");
      *Slot = static_cast<uint32_t>(Facts.size());
    }
    return F;
  }

  /// The facts of an instruction a pass decoded.
  InstructionFacts decodedFacts(va_t Address, InstructionMode Mode) {
    const uint32_t Slot = Halfwords.factSlotIfAny(Address);
    assert(Slot && "a decoded instruction has facts");
    if (slotIsThumb(Slot) == (Mode == InstructionMode::Thumb))
      return factsOf(Slot);
    return factsOf(OtherModeFactSlots.lookup(Address));
  }

  /// What the instruction this pass decoded at \p Key does with control, or
  /// nothing when the pass decoded none there in that mode.
  std::optional<ControlStep> stepAt(const CodeKey &Key) {
    const uint8_t Bits = Halfwords.decoded(Key.first);
    if (!(Bits & HalfwordMap::Start) || decodedMode(Bits) != Key.second)
      return std::nullopt;
    const InstructionFacts F = decodedFacts(Key.first, Key.second);
    ControlStep Step;
    Step.Size = F.Size;
    Step.Target = F.Target;
    Step.Call = F.has(InstructionFacts::Call);
    Step.Jump = F.has(InstructionFacts::Jump);
    Step.Conditional = F.has(InstructionFacts::Conditional);
    Step.Leaves = (F.Written & gprBit(ARM_REG_PC)) && !Step.Call &&
                  Step.Target == InvalidVA;
    Step.Stops =
        F.has(InstructionFacts::Trap) || (Bits & HalfwordMap::Exits) != 0;
    return Step;
  }

  /// Decode every path from \p Pending, adding to what earlier paths decoded.
  llvm::Error decodePaths(std::deque<ModeAt> &Pending);

  /// Settle which routines return, each proof possibly enabling the next: a
  /// routine whose last call is to one just proven no-return is one too.
  /// Whether it proved any routine no-return it did not know of.
  bool settleNoReturn() {
    const auto StepAt = [this](const CodeKey &Key) { return stepAt(Key); };
    const auto NoReturn = [this](va_t Target) { return isNoReturn(Target); };
    bool FoundNoReturn = false;
    for (bool Changed = true; Changed;) {
      Changed = false;
      for (const CodeKey &Entry : Entries)
        if (!isNoReturn(Entry.first) && stepAt(Entry) &&
            !mayReturn(Entry, StepAt, Entries, NoReturn)) {
          InferredNoReturn.insert(Entry.first);
          Changed = FoundNoReturn = true;
        }
    }
    return FoundNoReturn;
  }

  /// Whether a pass decoded an instruction across a proven literal.
  bool decodedAcrossALiteral() {
    return Literals.any([&](va_t Begin, va_t End) {
      // An instruction that overlaps the literal starts at most a halfword
      // before it.
      for (va_t At = (Begin < 2 ? 0 : Begin - 2) & ~va_t(1); At < End; At += 2)
        if (const uint8_t Bits = Halfwords.decoded(At);
            (Bits & HalfwordMap::Start) && decodedEnd(At, Bits) > Begin)
          return true;
      return false;
    });
  }

  /// Every pass from the exact entries until one settles: a pass that found
  /// literals or no-return routines it did not start with is repeated.
  llvm::Error run() {
    while (true) {
      Halfwords.clearDecoded();
      DecodedCount = 0;
      VeneerCandidates.clear();
      TakenThumbAddresses.clear();
      Entries.clear();
      Origins.clear();
      Conflict.reset();
      FoundLiterals = false;
      std::deque<ModeAt> Pending;
      for (const auto &[Address, Mode] : Img.ARMCodeModeEntries)
        if (Mode == InstructionMode::ARM || Mode == InstructionMode::Thumb) {
          Entries.emplace(Address, Mode);
          Pending.emplace_back(Address, Mode);
        }
      if (llvm::Error E = decodePaths(Pending))
        return E;
      if (FoundLiterals && (Conflict || decodedAcrossALiteral()))
        continue;
      if (settleNoReturn())
        continue;
      if (Conflict)
        return llvm::make_error<llvm::StringError>(
            *Conflict, llvm::inconvertibleErrorCode());
      return llvm::Error::success();
    }
  }

  /// Publish what the passes decoded: the reachable regions, the image's mode
  /// and the exact entries the passes proved.
  void publish() {
    Img.ARMReachableCodeRegions.clear();
    Halfwords.forEachDecoded([&](va_t Address, uint8_t Bits) {
      const InstructionMode Reached = decodedMode(Bits);
      const ARMCodeRegion Region{Address, decodedEnd(Address, Bits),
                                 regionKind(Reached)};
      if (!Img.ARMReachableCodeRegions.empty() &&
          Img.ARMReachableCodeRegions.back().End == Region.Start &&
          Img.ARMReachableCodeRegions.back().Kind == Region.Kind)
        Img.ARMReachableCodeRegions.back().End = Region.End;
      else
        Img.ARMReachableCodeRegions.push_back(Region);
      if (Img.Mode == InstructionMode::Default)
        Img.Mode = Reached;
      else if (Img.Mode != Reached)
        Img.Mode = InstructionMode::MixedARMThumb;
    });
    const auto DecodedIn = [&](va_t Address, InstructionMode Mode) {
      const uint8_t Bits = Halfwords.decoded(Address);
      return (Bits & HalfwordMap::Start) && decodedMode(Bits) == Mode;
    };
    for (const va_t Code : TakenThumbAddresses)
      if (DecodedIn(Code, InstructionMode::Thumb))
        Img.ARMCodeModeEntries.try_emplace(Code, InstructionMode::Thumb);
    for (const auto &[Target, Mode] : VeneerCandidates)
      if (DecodedIn(Target, Mode)) {
        Img.ARMVeneerTargets.insert(Target);
        Img.ARMCodeModeEntries.try_emplace(Target, Mode);
      }
    Img.ARMReachabilityConstrained = true;
    Published = true;
  }
};

InstructionFacts ARMModeDiscovery::State::decodeFacts(va_t Cur,
                                                      InstructionMode Mode,
                                                      const Segment &Seg) {
  InstructionFacts F;
  if (Mode == InstructionMode::Thumb)
    F.Flags |= InstructionFacts::Thumb;
  ++Disassembled;
  const size_t Offset = static_cast<size_t>(Cur - Seg.VA);
  const uint8_t *Bytes = Seg.Data.data() + Offset;
  size_t Remaining = Seg.Data.size() - Offset;
  uint64_t DecodeAddress = Cur;
  const csh Handle = Mode == InstructionMode::Thumb ? Dec.Thumb : Dec.ARM;
  cs_insn *Insn = Mode == InstructionMode::Thumb ? Dec.ThumbInsn : Dec.ARMInsn;
  // Both instruction sets encode in two or four bytes.
  if (!cs_disasm_iter(Handle, &Bytes, &Remaining, &DecodeAddress, Insn) ||
      (Insn->size != 2 && Insn->size != 4))
    return F;
  F.Size = static_cast<uint8_t>(Insn->size);
  const auto HasGroup = [&](uint8_t Group) {
    for (uint8_t Index = 0; Index < Insn->detail->groups_count; ++Index)
      if (Insn->detail->groups[Index] == Group)
        return true;
    return false;
  };
  const bool IsCall = HasGroup(CS_GRP_CALL);
  const bool IsJump = HasGroup(CS_GRP_JUMP);
  if (IsCall)
    F.Flags |= InstructionFacts::Call;
  if (IsJump)
    F.Flags |= InstructionFacts::Jump;
  if (Insn->detail->arm.cc < ARMCC_AL || Insn->id == ARM_INS_CBZ ||
      Insn->id == ARM_INS_CBNZ)
    F.Flags |= InstructionFacts::Conditional;
  // A supervisor call's immediate is an argument, not a target.
  if ((IsCall || IsJump) && Insn->id != ARM_INS_SVC)
    for (uint8_t Index = 0; Index < Insn->detail->arm.op_count; ++Index) {
      const cs_arm_op &Op = Insn->detail->arm.operands[Index];
      if (Op.type != ARM_OP_IMM || Op.imm < 0)
        continue;
      F.Target = clearThumbBit(static_cast<va_t>(Op.imm));
      if (Insn->id == ARM_INS_BLX)
        F.Flags |= InstructionFacts::SwitchesMode;
      break;
    }
  if (isARMResumableTrapInstruction(Insn->id))
    F.Flags |= InstructionFacts::Trap;
  if (Insn->id == ARM_INS_SVC && Insn->detail->arm.op_count == 1 &&
      Insn->detail->arm.operands[0].type == ARM_OP_IMM &&
      Insn->detail->arm.operands[0].imm == 0)
    F.Flags |= InstructionFacts::SupervisorCall;
  uint16_t Read[64], Written[64];
  uint8_t ReadCount = 0, WriteCount = 0;
  if (cs_regs_access(Handle, Insn, Read, &ReadCount, Written, &WriteCount) ==
      CS_ERR_OK)
    for (uint8_t Index = 0; Index < WriteCount; ++Index)
      F.Written |= gprBit(Written[Index]);
  if (F.Written & gprBit(ARM_REG_R7)) {
    F.Flags |= InstructionFacts::WritesR7;
    if (const auto Value = r7Value(*Insn, Mode, Img)) {
      F.Flags |= InstructionFacts::KnownR7;
      F.R7 = *Value;
    }
  }
  if (const auto Literal = literalRead(*Insn, Mode)) {
    F.Flags |= InstructionFacts::ReadsLiteral;
    // Within the reach of a PC-relative load, which wraps as the address
    // arithmetic does.
    F.LiteralOffset =
        static_cast<int32_t>(static_cast<int64_t>(Literal->first - Cur));
    F.LiteralSize = static_cast<uint8_t>(Literal->second - Literal->first);
  }
  if (const auto Loaded = pcRelativeWord(*Insn, Mode, Img)) {
    F.Flags |= InstructionFacts::LoadsWord;
    F.LoadedRegister = gprIndex(Loaded->first);
    F.LoadedWord = Loaded->second;
  }
  if (const auto Added = pcAddedRegister(*Insn)) {
    F.Flags |= InstructionFacts::AddsPC;
    F.AddedRegister = gprIndex(*Added);
  }
  return F;
}

llvm::Error ARMModeDiscovery::State::decodePaths(std::deque<ModeAt> &Pending) {
  while (!Pending.empty()) {
    const auto [Start, Mode] = Pending.front();
    Pending.pop_front();
    va_t Cur = Start;
    // The system call number, while this path has set it.
    std::optional<uint32_t> R7;
    // The words PC-relative loads on this path gave registers, by gprIndex.
    uint16_t KnownWords = 0;
    std::array<uint32_t, 16> LoadedWords{};
    while (true) {
      const Segment *Seg = Img.getSegmentFor(Cur);
      if (!Seg || !Img.isCodeAddress(Cur) || Cur < Seg->VA ||
          Cur - Seg->VA >= Seg->Data.size())
        break;
      if (Literals.overlaps(Cur, Cur + 1))
        break;
      const uint64_t Align = Mode == InstructionMode::Thumb ? 2 : 4;
      if ((Cur & (Align - 1)) != 0 ||
          Img.instructionModeAt(Cur, Mode) != Mode) {
        if (!Conflict)
          Conflict = std::string(Format) +
                     ": conflicting or misaligned reachable ARM/Thumb mode "
                     "at " +
                     hexAddress(Cur) + ", reached as " + modeName(Mode) +
                     " from " + from(Start, Mode);
        break;
      }
      if (const uint8_t Here = Halfwords.decoded(Cur);
          Here & HalfwordMap::Start) {
        if (decodedMode(Here) != Mode && !Conflict)
          Conflict = std::string(Format) +
                     ": conflicting reachable ARM/Thumb instructions at " +
                     hexAddress(Cur) + ", reached as " + modeName(Mode) +
                     " from " + from(Start, Mode);
        break;
      }
      if (const uint8_t Before = Cur >= 2 ? Halfwords.decoded(Cur - 2) : 0;
          (Before & HalfwordMap::Start) && decodedEnd(Cur - 2, Before) > Cur) {
        if (!Conflict)
          Conflict = std::string(Format) + ": " + modeName(Mode) +
                     " code reached at " + hexAddress(Cur) + " from " +
                     from(Start, Mode) +
                     " enters the middle of the instruction at " +
                     hexAddress(Cur - 2);
        break;
      }

      const InstructionFacts F = factsAt(Cur, Mode, *Seg);
      if (!F.Size || !Img.isCodeRange(Cur, F.Size) ||
          Literals.overlaps(Cur, Cur + F.Size))
        break;
      if (F.Size > 2 && (Halfwords.decoded(Cur + 2) & HalfwordMap::Start)) {
        if (!Conflict)
          Conflict = std::string(Format) + ": the " + modeName(Mode) +
                     " instruction at " + hexAddress(Cur) + ", reached from " +
                     from(Start, Mode) + ", overlaps the instruction at " +
                     hexAddress(Cur + 2);
        break;
      }
      if (DecodedCount == kMaxDiscoveredInstructions)
        return llvm::make_error<llvm::StringError>(
            std::string(Format) + ": ARM/Thumb reachable decode limit exceeded",
            llvm::inconvertibleErrorCode());

      uint8_t &Bits = Halfwords.decodedRef(Cur);
      Bits = HalfwordMap::Start;
      if (Mode == InstructionMode::Thumb)
        Bits |= HalfwordMap::ThumbStart;
      if (F.Size > 2)
        Bits |= HalfwordMap::Wide;
      ++DecodedCount;
      // `_Unwind_SetIP` keeps the Thumb bit of the PC it replaces, so a
      // landing pad runs in the mode of the code its call site guards.
      if (Pads)
        Pads->forEachPadAt(Cur, [&](va_t Pad) {
          if (Origins.try_emplace({Pad, Mode}, Cur).second)
            Pending.emplace_back(Pad, Mode);
        });
      if (F.has(InstructionFacts::ReadsLiteral)) {
        const va_t Literal =
            Cur + static_cast<va_t>(static_cast<int64_t>(F.LiteralOffset));
        FoundLiterals |= Literals.insert(Literal, Literal + F.LiteralSize);
      }
      // This exact instruction reads the following literal and transfers
      // control to its tagged code pointer. The literal itself is never an
      // instruction. Linked code and rewritten functions both use this
      // veneer, including when no section table survives.
      const size_t Offset = static_cast<size_t>(Cur - Seg->VA);
      if (Mode == InstructionMode::ARM && F.Size == arm::kInsnSize &&
          Seg->Data.size() - Offset >= arm::kLdrPCTrampLen &&
          Img.isCodeRange(Cur, arm::kLdrPCTrampLen) &&
          readLE<uint32_t>(Seg->Data.data() + Offset) == arm::kLdrPC) {
        const uint32_t Tagged =
            readLE<uint32_t>(Seg->Data.data() + Offset + arm::kInsnSize);
        const va_t Target = clearThumbBit(Tagged);
        if (Img.isCodeAddress(Target)) {
          const InstructionMode TargetMode =
              (Tagged & 1u) ? InstructionMode::Thumb : InstructionMode::ARM;
          Pending.emplace_back(Target, TargetMode);
          Origins.try_emplace({Target, TargetMode}, Cur);
          VeneerCandidates.emplace(Target, TargetMode);
        }
      }
      const bool IsCall = F.has(InstructionFacts::Call);
      const bool IsJump = F.has(InstructionFacts::Jump);
      const bool Conditional = F.has(InstructionFacts::Conditional);
      if (F.Target != InvalidVA && Img.isCodeAddress(F.Target)) {
        InstructionMode TargetMode = Mode;
        if (F.has(InstructionFacts::SwitchesMode))
          TargetMode = Mode == InstructionMode::Thumb ? InstructionMode::ARM
                                                      : InstructionMode::Thumb;
        Pending.emplace_back(F.Target, TargetMode);
        Origins.try_emplace({F.Target, TargetMode}, Cur);
        if (IsCall)
          Entries.emplace(F.Target, TargetMode);
      }
      const bool WritesPC = (F.Written & gprBit(ARM_REG_PC)) != 0;
      // A trap does not go on, and neither does exit or exit_group: the
      // EABI passes the call number in r7 and ignores the immediate.
      bool Stops = F.has(InstructionFacts::Trap);
      if (F.has(InstructionFacts::SupervisorCall) && R7 && isExitSyscall(*R7)) {
        Bits |= HalfwordMap::Exits;
        Stops = true;
      }
      if (F.has(InstructionFacts::WritesR7))
        R7 = F.has(InstructionFacts::KnownR7) ? std::optional<uint32_t>(F.R7)
                                              : std::nullopt;
      if (F.has(InstructionFacts::AddsPC) && F.AddedRegister != kNoRegister &&
          (KnownWords & (1u << F.AddedRegister))) {
        const va_t PC =
            Cur + (Mode == InstructionMode::Thumb ? branch::ThumbPCOffset
                                                  : branch::ArmPCOffset);
        const uint32_t Address =
            LoadedWords[F.AddedRegister] + static_cast<uint32_t>(PC);
        const va_t Code = clearThumbBit(Address);
        if ((Address & 1u) && Img.getSectionFor(Code) &&
            Img.isCodeRange(Code, 2) &&
            TakenThumbAddresses.insert(Code).second) {
          Pending.emplace_back(Code, InstructionMode::Thumb);
          Origins.try_emplace({Code, InstructionMode::Thumb}, Cur);
        }
      }
      KnownWords &= ~F.Written;
      if (F.has(InstructionFacts::LoadsWord) &&
          F.LoadedRegister != kNoRegister) {
        KnownWords |= uint16_t(1u << F.LoadedRegister);
        LoadedWords[F.LoadedRegister] = F.LoadedWord;
      }
      if (Stops && !Conditional)
        break;
      if (IsCall && !Conditional && F.Target != InvalidVA &&
          isNoReturn(F.Target))
        break;
      if ((IsJump || (WritesPC && !IsCall)) && !Conditional)
        break;
      Cur += F.Size;
    }
  }
  return llvm::Error::success();
}

ARMModeDiscovery::ARMModeDiscovery(BinaryImage &Image) : Img(Image) {}

ARMModeDiscovery::~ARMModeDiscovery() = default;

llvm::Error ARMModeDiscovery::discover() {
  // Mapping symbols describe their own section intervals, but a rewritten
  // ELF can append executable program bytes without adding a section-table
  // owner. Mach-O has exact Thumb symbols but no mapping intervals. Follow
  // exact entries when no interval evidence covers all executable bytes.
  const bool HasUnmappedExecutableSegment = std::any_of(
      Img.Segments.begin(), Img.Segments.end(), [&](const Segment &Seg) {
        return Seg.isExecutable() && !Seg.Data.empty() &&
               !Img.segmentHasReadableSectionMetadata(Seg);
      });
  if (Img.Arch != Arch::ARM || Img.ARMCodeModeEntries.empty() ||
      (!Img.ARMCodeRegions.empty() && !HasUnmappedExecutableSegment))
    return llvm::Error::success();
  S = std::make_unique<State>(Img, Disassembled);
  if (llvm::Error E = S->open())
    return E;
  if (llvm::Error E = S->run())
    return E;
  S->publish();
  // What the passes committed needs no keeping until the pads extend them.
  S->Origins.clear();
  S->VeneerCandidates.clear();
  S->TakenThumbAddresses.clear();
  return llvm::Error::success();
}

llvm::Error ARMModeDiscovery::followLandingPads() {
  if (!S || !S->Published)
    return llvm::Error::success();
  // Nothing reads the decode after its last phase.
  const std::unique_ptr<State> Done = std::move(S);
  State &D = *Done;
  const CallSitePads &Pads = D.Pads.emplace(D.Img);
  // The modes of the instructions the passes decoded in a call site's
  // guarded range, by the first of each.
  const auto GuardedModes = [&](const CallSitePads::Site &Site, auto Visit) {
    bool Seen[2] = {false, false};
    for (va_t At = (Site.Begin + 1) & ~va_t(1); At < Site.End; At += 2)
      if (const uint8_t Bits = D.Halfwords.decoded(At);
          Bits & HalfwordMap::Start) {
        const InstructionMode Mode = decodedMode(Bits);
        bool &Visited = Seen[Mode == InstructionMode::Thumb];
        if (!Visited) {
          Visited = true;
          Visit(At, Mode);
        }
      }
  };
  const auto Covered = [&](va_t Address) {
    for (va_t At = (Address < 2 ? 0 : Address - 2) & ~va_t(1); At <= Address;
         At += 2)
      if (const uint8_t Bits = D.Halfwords.decoded(At);
          (Bits & HalfwordMap::Start) && decodedEnd(At, Bits) > Address)
        return true;
    return false;
  };
  // Nothing to follow when every pad of a call site the passes decoded is
  // decoded already.
  if (llvm::none_of(Pads.Sites, [&](const CallSitePads::Site &Site) {
        bool Guarded = false;
        GuardedModes(Site, [&](va_t, InstructionMode) { Guarded = true; });
        return Guarded && !Covered(Site.Pad);
      }))
    return llvm::Error::success();
  // `_Unwind_SetIP` keeps the Thumb bit of the PC it replaces, so a landing
  // pad runs in the mode of the code its call site guards.  The pads of the
  // call sites the passes decoded start paths of their own, which add to what
  // the passes decoded, and the pads of the call sites those paths decode
  // follow in turn.  Mode evidence comes from the entries and the exact
  // decode, not from the regions the passes published.
  D.Img.ARMReachableCodeRegions.clear();
  D.Conflict.reset();
  D.FoundLiterals = false;
  D.Origins.clear();
  // The entries a pass would start from now include those the passes proved.
  for (const auto &[Address, Mode] : D.Img.ARMCodeModeEntries)
    if (Mode == InstructionMode::ARM || Mode == InstructionMode::Thumb)
      D.Entries.emplace(Address, Mode);
  std::deque<ModeAt> Pending;
  for (const CallSitePads::Site &Site : Pads.Sites)
    GuardedModes(Site, [&](va_t At, InstructionMode Mode) {
      if (D.Origins.try_emplace({Site.Pad, Mode}, At).second)
        Pending.emplace_back(Site.Pad, Mode);
    });
  if (llvm::Error E = D.decodePaths(Pending))
    return E;
  // A pad's code that met a conflict, decoded across a literal it found, or
  // proved a routine no-return changes what the passes could decode: decode
  // the image again from its entries, following every pad from the start,
  // as the passes do.
  if (D.Conflict || (D.FoundLiterals && D.decodedAcrossALiteral()) ||
      D.settleNoReturn())
    if (llvm::Error E = D.run())
      return E;
  D.publish();
  return llvm::Error::success();
}

llvm::Error discoverARMReachableModes(BinaryImage &Img) {
  return ARMModeDiscovery(Img).discover();
}

size_t scanImportThunksARM(BinaryImage &Img, const Segment &Seg,
                           const std::map<va_t, size_t> &Targets,
                           std::set<va_t> &Existing,
                           const ImportThunkCandidates &Candidates) {
  if (Img.Mode == InstructionMode::Thumb)
    return scanThumbImportThunks(Img, Seg, Targets, Existing, Candidates);

  const uint8_t *D = Seg.Data.data();
  size_t N = Seg.Data.size();
  if (N < arm::kLdrPCTrampLen)
    return 0;

  size_t Added = 0;
  for (size_t I = 0; I + arm::kLdrPCTrampLen <= N; I += arm::kInsnSize) {
    uint32_t W = readLE<uint32_t>(D + I);
    if (W != arm::kLdrPC)
      continue;
    uint32_t AbsAddr = readLE<uint32_t>(D + I + arm::kInsnSize);
    va_t Target = AbsAddr;
    auto TargetIt = Targets.find(Target);
    if (TargetIt == Targets.end())
      continue;
    va_t InsnVA = Seg.VA + I;
    if (!Img.isCodeRange(InsnVA, arm::kLdrPCTrampLen) ||
        !Candidates.allows(InsnVA, arm::kLdrPCTrampLen) ||
        Img.instructionModeAt(InsnVA) != InstructionMode::ARM)
      continue;
    Img.recordImportStub(InsnVA, TargetIt->second);
    if (!Existing.insert(InsnVA).second)
      continue;
    Img.Symbols.push_back(Symbol::makeFunc(InsnVA, arm::kLdrPCTrampLen));
    ++Added;
  }
  if (Img.Mode == InstructionMode::MixedARMThumb)
    Added += scanThumbImportThunks(Img, Seg, Targets, Existing, Candidates);
  return Added;
}

} // namespace neverd
