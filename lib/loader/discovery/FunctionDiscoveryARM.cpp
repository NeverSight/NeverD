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

#include "neverd/support/BinaryEncoding.h"
#include "neverd/support/ISAEncoding.h"

#include "llvm/ADT/StringExtras.h"
#include "llvm/Support/Error.h"

#include <capstone/arm.h>
#include <capstone/capstone.h>
#include <deque>
#include <map>
#include <optional>

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
                             std::set<va_t> &Existing) {
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

} // anonymous namespace

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
  // One entry per decoded instruction until the final adjacent-span merge.
  // Keeping the extents separate makes a branch into an instruction interior
  // a detectable conflict, rather than accidentally decoding from there.
  std::map<va_t, ARMCodeRegion> Decoded;
  std::map<va_t, InstructionMode> VeneerCandidates;
  constexpr size_t kMaxDiscoveredInstructions = 4'000'000;
  while (true) {
    Decoded.clear();
    VeneerCandidates.clear();
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
        if (IsCall || IsJump) {
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
            if (Img.isCodeAddress(Target)) {
              Pending.emplace_back(Target, TargetMode);
              Origins.try_emplace({Target, TargetMode}, Cur);
            }
            break;
          }
        }
        uint16_t Read[64], Written[64];
        uint8_t ReadCount = 0, WriteCount = 0;
        const bool WritesPC =
            cs_regs_access(Handle, Insn, Read, &ReadCount, Written,
                           &WriteCount) == CS_ERR_OK &&
            std::find(Written, Written + WriteCount, ARM_REG_PC) !=
                Written + WriteCount;
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
    if (Img.Mode != Reached)
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
                           std::set<va_t> &Existing) {
  if (Img.Mode == InstructionMode::Thumb)
    return scanThumbImportThunks(Img, Seg, Targets, Existing);

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
        Img.instructionModeAt(InsnVA) != InstructionMode::ARM)
      continue;
    Img.recordImportStub(InsnVA, TargetIt->second);
    if (!Existing.insert(InsnVA).second)
      continue;
    Img.Symbols.push_back(Symbol::makeFunc(InsnVA, arm::kLdrPCTrampLen));
    ++Added;
  }
  if (Img.Mode == InstructionMode::MixedARMThumb)
    Added += scanThumbImportThunks(Img, Seg, Targets, Existing);
  return Added;
}

} // namespace neverd
