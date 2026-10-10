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

#include "FunctionDiscoveryDetail.h"

#include "neverd/libc/LibCNames.h"
#include "neverd/loader/ELF/ELFLoaderUtils.h"
#include "neverd/support/BinaryEncoding.h"
#include "neverd/support/ISAEncoding.h"

#include "llvm/ADT/StringExtras.h"
#include "llvm/Support/Error.h"

#include <capstone/arm.h>
#include <capstone/capstone.h>
#include <deque>
#include <functional>
#include <map>
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

/// An address and the instruction set its bytes are decoded in.  Bytes a
/// path reached in the wrong mode decode as something else, so what a
/// routine does is read only from its own mode.
using CodeKey = std::pair<va_t, InstructionMode>;

struct CodeKeyHash {
  size_t operator()(const CodeKey &Key) const {
    return std::hash<va_t>()(Key.first * 2 +
                             (Key.second == InstructionMode::Thumb));
  }
};

/// Whether a routine entered at \p Entry can return to its caller, as far as
/// the instructions of \p Steps show: some path from it reaches a return or
/// a jump nobody can follow, falls into or jumps to another routine that can
/// return, or runs into code that was not decoded.  Only a routine every path
/// of which ends in a trap, an exiting system call, or a call to a routine
/// \p IsNoReturn names cannot.
bool mayReturn(
    const CodeKey &Entry,
    const std::unordered_map<CodeKey, ControlStep, CodeKeyHash> &Steps,
    const std::set<CodeKey> &Entries,
    const std::function<bool(va_t)> &IsNoReturn) {
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
    const auto It = Steps.find({Address, Mode});
    if (It == Steps.end())
      return true;
    const ControlStep &Step = It->second;
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

llvm::Error discoverARMReachableModes(BinaryImage &Img) {
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
  const char *Format = Img.Format == BinaryFormat::MachO ? "macho" : "elf";

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
  } Dec;
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

  const auto Hex = [](va_t Address) { return "0x" + llvm::utohexstr(Address); };
  const auto ModeName = [](InstructionMode Mode) {
    return Mode == InstructionMode::Thumb ? "Thumb" : "ARM";
  };

  using ModeAt = std::pair<va_t, InstructionMode>;

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
  // A veneer names its import even where the image has not recorded it: a
  // mixed image keeps its PLT unpaired.  And a shared library's veneer for a
  // routine the library defines itself goes to that definition unless
  // another module interposes one, so the definition's own code settles
  // whether it returns: the throw helpers of a C++ runtime are no-return
  // without being on any list.
  const libc::NoReturnTargetIndex NamedNoReturn(Img);
  std::set<va_t> NoReturnVeneers;
  std::map<va_t, va_t> VeneerDefinitions;
  {
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
  }
  std::set<va_t> InferredNoReturn;
  const auto IsNoReturn = [&](va_t Target) {
    if (const auto It = VeneerDefinitions.find(Target);
        It != VeneerDefinitions.end())
      Target = It->second;
    return InferredNoReturn.count(Target) || NoReturnVeneers.count(Target) ||
           NamedNoReturn.contains(Img, Target);
  };
  // One entry per decoded instruction until the final adjacent-span merge.
  // Keeping the extents separate makes a branch into an instruction interior
  // a detectable conflict, rather than accidentally decoding from there.
  std::map<va_t, ARMCodeRegion> Decoded;
  std::map<va_t, InstructionMode> VeneerCandidates;
  constexpr size_t kMaxDiscoveredInstructions = 4'000'000;
  std::unordered_map<CodeKey, ControlStep, CodeKeyHash> Steps;
  std::set<CodeKey> Entries;
  while (true) {
    Decoded.clear();
    VeneerCandidates.clear();
    Steps.clear();
    Entries.clear();
    for (const auto &[Address, Mode] : Img.ARMCodeModeEntries)
      if (Mode == InstructionMode::ARM || Mode == InstructionMode::Thumb)
        Entries.emplace(Address, Mode);
    std::deque<ModeAt> Pending;
    for (const auto &[Address, Mode] : Img.ARMCodeModeEntries)
      if (Mode == InstructionMode::ARM || Mode == InstructionMode::Thumb)
        Pending.emplace_back(Address, Mode);
    // The first conflict of this pass, which fails only if the pass found no
    // literal it did not start with.
    std::optional<std::string> Conflict;
    bool FoundLiterals = false;
    // The branch each target was first reached from, for the diagnostics.
    std::map<ModeAt, va_t> Origins;
    const auto From = [&](va_t Start, InstructionMode Mode) {
      const auto It = Origins.find({Start, Mode});
      return Hex(Start) +
             (It == Origins.end()
                  ? std::string(", an entry")
                  : ", the target of the branch at " + Hex(It->second));
    };
    while (!Pending.empty()) {
      const auto [Start, Mode] = Pending.front();
      Pending.pop_front();
      va_t Cur = Start;
      // The system call number, while this path has set it.
      std::optional<uint32_t> R7;
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
                       Hex(Cur) + ", reached as " + ModeName(Mode) + " from " +
                       From(Start, Mode);
          break;
        }
        const auto Existing = Decoded.lower_bound(Cur);
        if (Existing != Decoded.end() && Existing->first == Cur) {
          if (Existing->second.Kind != (Mode == InstructionMode::Thumb
                                            ? ARMCodeRegionKind::Thumb
                                            : ARMCodeRegionKind::ARM) &&
              !Conflict)
            Conflict = std::string(Format) +
                       ": conflicting reachable ARM/Thumb instructions at " +
                       Hex(Cur) + ", reached as " + ModeName(Mode) + " from " +
                       From(Start, Mode);
          break;
        }
        if (Existing != Decoded.begin() &&
            std::prev(Existing)->second.End > Cur) {
          if (!Conflict)
            Conflict = std::string(Format) + ": " + ModeName(Mode) +
                       " code reached at " + Hex(Cur) + " from " +
                       From(Start, Mode) +
                       " enters the middle of the instruction at " +
                       Hex(std::prev(Existing)->first);
          break;
        }

        const size_t Offset = static_cast<size_t>(Cur - Seg->VA);
        const uint8_t *Bytes = Seg->Data.data() + Offset;
        size_t Remaining = Seg->Data.size() - Offset;
        uint64_t DecodeAddress = Cur;
        const csh Handle = Mode == InstructionMode::Thumb ? Dec.Thumb : Dec.ARM;
        cs_insn *Insn =
            Mode == InstructionMode::Thumb ? Dec.ThumbInsn : Dec.ARMInsn;
        if (!cs_disasm_iter(Handle, &Bytes, &Remaining, &DecodeAddress, Insn) ||
            !Img.isCodeRange(Cur, Insn->size) ||
            Literals.overlaps(Cur, Cur + Insn->size))
          break;
        if (Insn->size > InvalidVA - Cur ||
            (Existing != Decoded.end() && Existing->first < Cur + Insn->size)) {
          if (!Conflict)
            Conflict = std::string(Format) + ": the " + ModeName(Mode) +
                       " instruction at " + Hex(Cur) + ", reached from " +
                       From(Start, Mode) + ", overlaps the instruction at " +
                       Hex(Existing == Decoded.end() ? Cur : Existing->first);
          break;
        }
        if (Decoded.size() == kMaxDiscoveredInstructions)
          return llvm::make_error<llvm::StringError>(
              std::string(Format) +
                  ": ARM/Thumb reachable decode limit exceeded",
              llvm::inconvertibleErrorCode());

        const ARMCodeRegionKind Kind = Mode == InstructionMode::Thumb
                                           ? ARMCodeRegionKind::Thumb
                                           : ARMCodeRegionKind::ARM;
        Decoded.emplace(Cur, ARMCodeRegion{Cur, Cur + Insn->size, Kind});
        if (const auto Literal = literalRead(*Insn, Mode))
          FoundLiterals |= Literals.insert(Literal->first, Literal->second);
        // This exact instruction reads the following literal and transfers
        // control to its tagged code pointer. The literal itself is never an
        // instruction. Linked code and rewritten functions both use this
        // veneer, including when no section table survives.
        if (Mode == InstructionMode::ARM && Insn->size == arm::kInsnSize &&
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
        const auto HasGroup = [&](uint8_t Group) {
          for (uint8_t Index = 0; Index < Insn->detail->groups_count; ++Index)
            if (Insn->detail->groups[Index] == Group)
              return true;
          return false;
        };
        const bool IsCall = HasGroup(CS_GRP_CALL);
        const bool IsJump = HasGroup(CS_GRP_JUMP);
        const bool Conditional = Insn->detail->arm.cc < ARMCC_AL ||
                                 Insn->id == ARM_INS_CBZ ||
                                 Insn->id == ARM_INS_CBNZ;
        ControlStep Step;
        Step.Size = Insn->size;
        Step.Call = IsCall;
        Step.Jump = IsJump;
        Step.Conditional = Conditional;
        // A supervisor call's immediate is an argument, not a target.
        if ((IsCall || IsJump) && Insn->id != ARM_INS_SVC) {
          for (uint8_t Index = 0; Index < Insn->detail->arm.op_count; ++Index) {
            const cs_arm_op &Op = Insn->detail->arm.operands[Index];
            if (Op.type != ARM_OP_IMM || Op.imm < 0)
              continue;
            InstructionMode TargetMode = Mode;
            if (Insn->id == ARM_INS_BLX)
              TargetMode = Mode == InstructionMode::Thumb
                               ? InstructionMode::ARM
                               : InstructionMode::Thumb;
            const va_t Target = clearThumbBit(static_cast<va_t>(Op.imm));
            Step.Target = Target;
            if (Img.isCodeAddress(Target)) {
              Pending.emplace_back(Target, TargetMode);
              Origins.try_emplace({Target, TargetMode}, Cur);
              if (IsCall)
                Entries.emplace(Target, TargetMode);
            }
            break;
          }
        }
        uint16_t Read[64], Written[64];
        uint8_t ReadCount = 0, WriteCount = 0;
        const bool Accessed = cs_regs_access(Handle, Insn, Read, &ReadCount,
                                             Written, &WriteCount) == CS_ERR_OK;
        const auto Writes = [&](uint16_t Reg) {
          return Accessed && std::find(Written, Written + WriteCount, Reg) !=
                                 Written + WriteCount;
        };
        const bool WritesPC = Writes(ARM_REG_PC);
        Step.Leaves = WritesPC && !IsCall && Step.Target == InvalidVA;
        // A trap does not go on, and neither does exit or exit_group: the
        // EABI passes the call number in r7 and ignores the immediate.
        if (Insn->id == ARM_INS_UDF || Insn->id == ARM_INS_BKPT ||
            Insn->id == ARM_INS_TRAP)
          Step.Stops = true;
        if (Insn->id == ARM_INS_SVC && Insn->detail->arm.op_count == 1 &&
            Insn->detail->arm.operands[0].type == ARM_OP_IMM &&
            Insn->detail->arm.operands[0].imm == 0 && R7 && isExitSyscall(*R7))
          Step.Stops = true;
        if (Writes(ARM_REG_R7))
          R7 = r7Value(*Insn, Mode, Img);
        Steps.emplace(CodeKey(Cur, Mode), Step);
        if (Step.Stops && !Conditional)
          break;
        if (IsCall && !Conditional && Step.Target != InvalidVA &&
            IsNoReturn(Step.Target))
          break;
        if ((IsJump || (WritesPC && !IsCall)) && !Conditional)
          break;
        Cur += Insn->size;
      }
    }
    if (FoundLiterals &&
        (Conflict ||
         std::any_of(Decoded.begin(), Decoded.end(), [&](const auto &Entry) {
           return Literals.overlaps(Entry.second.Start, Entry.second.End);
         })))
      continue;
    // Settle which routines return, each proof possibly enabling the next:
    // a routine whose last call is to one just proven no-return is one too.
    bool FoundNoReturn = false;
    for (bool Changed = true; Changed;) {
      Changed = false;
      for (const CodeKey &Entry : Entries)
        if (!IsNoReturn(Entry.first) && Steps.count(Entry) &&
            !mayReturn(Entry, Steps, Entries, IsNoReturn)) {
          InferredNoReturn.insert(Entry.first);
          Changed = FoundNoReturn = true;
        }
    }
    if (FoundNoReturn)
      continue;
    if (Conflict)
      return llvm::make_error<llvm::StringError>(
          *Conflict, llvm::inconvertibleErrorCode());
    break;
  }

  for (const auto &[Address, Region] : Decoded) {
    (void)Address;
    if (!Img.ARMReachableCodeRegions.empty() &&
        Img.ARMReachableCodeRegions.back().End == Region.Start &&
        Img.ARMReachableCodeRegions.back().Kind == Region.Kind)
      Img.ARMReachableCodeRegions.back().End = Region.End;
    else
      Img.ARMReachableCodeRegions.push_back(Region);
    const InstructionMode Reached = Region.Kind == ARMCodeRegionKind::Thumb
                                        ? InstructionMode::Thumb
                                        : InstructionMode::ARM;
    if (Img.Mode == InstructionMode::Default)
      Img.Mode = Reached;
    else if (Img.Mode != Reached)
      Img.Mode = InstructionMode::MixedARMThumb;
  }
  for (const auto &[Target, Mode] : VeneerCandidates)
    if (const auto It = Decoded.find(Target);
        It != Decoded.end() &&
        It->second.Kind == (Mode == InstructionMode::Thumb
                                ? ARMCodeRegionKind::Thumb
                                : ARMCodeRegionKind::ARM)) {
      Img.ARMVeneerTargets.insert(Target);
      Img.ARMCodeModeEntries.try_emplace(Target, Mode);
    }
  Img.ARMReachabilityConstrained = true;
  return llvm::Error::success();
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
