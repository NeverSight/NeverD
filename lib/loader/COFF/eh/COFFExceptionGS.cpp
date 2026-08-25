//===- COFFExceptionGS.cpp - GS handler decoding and inference ------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#include "COFFExceptionDetail.h"

#include "neverd/loader/BinaryImage.h"
#include "neverd/support/BinaryEncoding.h"

#include <capstone/capstone.h>
#include <cstddef>
#include <cstdint>
#include <iterator>
#include <limits>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <utility>
#include <vector>

namespace neverd::coff_loader::detail {

bool parseGSCookie(ExceptionFunction &F, const BinaryImage &Img,
                   va_t CookieVA) {
  GSCookieInfo Cookie;
  const uint8_t *Header = Img.readVA(CookieVA, sizeof(uint32_t));
  if (!Header) {
    Cookie.ParseStatus = ExceptionParseStatus::Malformed;
    diagnose(F, ExceptionParseStatus::Malformed, "truncated GS handler header");
    F.GSCookie = std::move(Cookie);
    return false;
  }
  // The flags ride in the spare low bits of the cookie's frame offset, so how
  // many of them exist is decided by the alignment of the slot the cookie sits
  // in.  A 64-bit CRT gets three: `__GSHandlerCheckCommon` masks the word with
  // -8, tests bit 2 for an aligned frame, and reads a base and an alignment out
  // of the two words behind it.  A 32-bit CRT gets two, so it spends its one
  // remaining bit on the aligned form and derives the adjustment arithmetically
  // -- the ARM routine masks with -4, tests bit 0, and never looks past the
  // first word.  Reading the 64-bit shape out of a 32-bit record both misreads
  // the offset and then runs off the end of the record into whichever .xdata
  // happens to follow.
  const bool WideFlags = Img.is64Bit();
  const uint32_t Flags = readLE<uint32_t>(Header);
  const uint32_t FlagMask = WideFlags ? 7u : 3u;
  Cookie.HasExceptionHandler = WideFlags && (Flags & 1u) != 0;
  Cookie.HasUnwindHandler = WideFlags && (Flags & 2u) != 0;
  Cookie.HasAlignment = (Flags & (WideFlags ? 4u : 1u)) != 0;
  Cookie.CookieOffset = static_cast<int32_t>(Flags & ~FlagMask);
  size_t Size = sizeof(uint32_t);
  if (Cookie.HasAlignment && WideFlags) {
    const uint8_t *Alignment = Img.readVA(CookieVA, 3 * sizeof(uint32_t));
    if (!Alignment) {
      Cookie.ParseStatus = ExceptionParseStatus::Malformed;
      diagnose(F, ExceptionParseStatus::Malformed,
               "truncated aligned GS handler data");
      F.GSCookie = std::move(Cookie);
      return false;
    }
    Cookie.AlignmentBaseOffset = readLE<int32_t>(Alignment + 4);
    Cookie.Alignment = readLE<uint32_t>(Alignment + 8);
    if (Cookie.Alignment == 0 ||
        (Cookie.Alignment & (Cookie.Alignment - 1)) != 0) {
      Cookie.ParseStatus = ExceptionParseStatus::Malformed;
      diagnose(F, ExceptionParseStatus::Malformed,
               "invalid GS stack alignment");
      F.GSCookie = std::move(Cookie);
      return false;
    }
    Size = 3 * sizeof(uint32_t);
  }
  const uint8_t *Payload = Img.readVA(CookieVA, Size);
  if (!Payload) {
    Cookie.ParseStatus = ExceptionParseStatus::Malformed;
    diagnose(F, ExceptionParseStatus::Malformed,
             "truncated GS handler payload");
    F.GSCookie = std::move(Cookie);
    return false;
  }
  Cookie.Payload.assign(Payload, Payload + Size);
  Cookie.ParseStatus = ExceptionParseStatus::Complete;
  F.GSCookie = std::move(Cookie);
  return true;
}

std::optional<va_t> sehGSCookieAddress(const ExceptionFunction &F,
                                       const BinaryImage &Img) {
  auto Count = readScalar<uint32_t>(Img, F.HandlerDataVA);
  if (!Count || *Count > MaxLanguageRecords)
    return std::nullopt;
  uint64_t ScopeBytes = uint64_t(*Count) * 16;
  if (F.HandlerDataVA > InvalidVA - sizeof(uint32_t) ||
      ScopeBytes > InvalidVA - (F.HandlerDataVA + sizeof(uint32_t)))
    return std::nullopt;
  return F.HandlerDataVA + sizeof(uint32_t) + ScopeBytes;
}

namespace {

constexpr size_t MaxWrapperBytes = 512;
constexpr size_t MaxWrapperInstructions = 64;
constexpr size_t MaxWrapperBlocks = 32;
constexpr size_t MaxWrapperEdges = 128;

class CapstoneSession {
public:
  CapstoneSession(cs_arch A, cs_mode M) {
    if (cs_open(A, M, &Handle) != CS_ERR_OK)
      return;
    if (cs_option(Handle, CS_OPT_DETAIL, CS_OPT_ON) != CS_ERR_OK)
      return;
    Insn = cs_malloc(Handle);
  }

  ~CapstoneSession() {
    if (Insn)
      cs_free(Insn, 1);
    if (Handle)
      cs_close(&Handle);
  }

  CapstoneSession(const CapstoneSession &) = delete;
  CapstoneSession &operator=(const CapstoneSession &) = delete;

  explicit operator bool() const { return Handle != 0 && Insn != nullptr; }
  csh handle() const { return Handle; }
  cs_insn *instruction() const { return Insn; }

private:
  csh Handle = 0;
  cs_insn *Insn = nullptr;
};

enum class ControlKind : uint8_t {
  None,
  Call,
  Branch,
  Return,
  Trap,
};

struct DirectControl {
  ControlKind Kind = ControlKind::None;
  bool Conditional = false;
  bool SwitchesMode = false;
  std::optional<va_t> Target;
  std::optional<std::string> RuntimeName;
};

bool isTerminalTrap(Arch A, const cs_insn &Insn) {
  switch (A) {
  case Arch::X64:
  case Arch::X86:
    return Insn.id == X86_INS_UD0 || Insn.id == X86_INS_UD1 ||
           Insn.id == X86_INS_UD2;
  case Arch::AArch64:
    return Insn.id == AARCH64_INS_BRK || Insn.id == AARCH64_INS_UDF;
  case Arch::ARM:
    return Insn.id == ARM_INS_UDF;
  default:
    return false;
  }
}

bool isUnconditional(AArch64CC_CondCode CC) {
  return CC == AArch64CC_Invalid || CC == AArch64CC_AL || CC == AArch64CC_NV;
}

bool isUnconditional(ARMCC_CondCodes CC) {
  return CC == ARMCC_Invalid || CC == ARMCC_AL;
}

bool exactIATNameAt(const BinaryImage &Img, va_t Slot, std::string &Name) {
  const Import *Match = nullptr;
  for (const Import &Imp : Img.Imports) {
    if (Imp.IATAddr != Slot)
      continue;
    if (Match && Match->Name != Imp.Name)
      return false;
    Match = &Imp;
  }
  if (!Match || Slot == 0)
    return false;
  Name = Match->Name;
  return true;
}

bool classifyX86Control(const BinaryImage &Img, Arch A, csh Handle,
                        const cs_insn &Insn, DirectControl &Control) {
  const bool IsCall = cs_insn_group(Handle, &Insn, CS_GRP_CALL);
  const bool IsLoop = Insn.id == X86_INS_LOOP || Insn.id == X86_INS_LOOPE ||
                      Insn.id == X86_INS_LOOPNE;
  const bool IsBranch = cs_insn_group(Handle, &Insn, CS_GRP_JUMP) || IsLoop;
  const bool IsReturn = cs_insn_group(Handle, &Insn, CS_GRP_RET);
  if (IsReturn) {
    if (IsCall)
      return false;
    Control.Kind = ControlKind::Return;
    return true;
  }
  if (IsCall == IsBranch && IsCall)
    return false;
  if (!IsCall && !IsBranch)
    return true;
  if (IsCall && Insn.id != X86_INS_CALL)
    return false;

  Control.Kind = IsCall ? ControlKind::Call : ControlKind::Branch;
  Control.Conditional = IsBranch && Insn.id != X86_INS_JMP;
  const cs_x86 &X = Insn.detail->x86;
  if (X.op_count != 1)
    return false;
  const cs_x86_op &Op = X.operands[0];
  if (Op.type == X86_OP_IMM) {
    if (A == Arch::X64 && Op.imm < 0)
      return false;
    Control.Target = A == Arch::X86
                         ? static_cast<va_t>(static_cast<uint32_t>(Op.imm))
                         : static_cast<va_t>(Op.imm);
    return true;
  }

  if (Op.type != X86_OP_MEM ||
      (Insn.id != X86_INS_CALL && Insn.id != X86_INS_JMP) ||
      Op.mem.segment != X86_REG_INVALID || Op.mem.index != X86_REG_INVALID)
    return false;

  std::optional<va_t> Slot;
  if (A == Arch::X64 && Op.mem.base == X86_REG_RIP) {
    if (Insn.address > InvalidVA - Insn.size)
      return false;
    Slot = addSignedOffset(Insn.address + Insn.size, Op.mem.disp);
  } else if (A == Arch::X86 && Op.mem.base == X86_REG_INVALID) {
    Slot = static_cast<va_t>(static_cast<uint32_t>(Op.mem.disp));
  }
  const size_t PointerSize = A == Arch::X64 ? 8 : 4;
  std::string Name;
  if (!Slot || Op.size != PointerSize || (*Slot % PointerSize) != 0 ||
      !exactIATNameAt(Img, *Slot, Name))
    return false;
  Control.RuntimeName = std::move(Name);
  return true;
}

bool classifyAArch64Control(csh Handle, const cs_insn &Insn,
                            DirectControl &Control) {
  const bool IsCall = cs_insn_group(Handle, &Insn, CS_GRP_CALL);
  const bool IsBranch = cs_insn_group(Handle, &Insn, CS_GRP_JUMP);
  const bool IsReturn = cs_insn_group(Handle, &Insn, CS_GRP_RET);
  if (IsReturn) {
    if (IsCall)
      return false;
    Control.Kind = ControlKind::Return;
    return true;
  }
  if (IsCall == IsBranch && IsCall)
    return false;
  if (!IsCall && !IsBranch)
    return true;
  if (IsCall && Insn.id != AARCH64_INS_BL)
    return false;

  const cs_aarch64 &Arm64 = Insn.detail->aarch64;
  Control.Kind = IsCall ? ControlKind::Call : ControlKind::Branch;
  Control.Conditional =
      IsBranch && (Insn.id != AARCH64_INS_B || !isUnconditional(Arm64.cc));
  if (Arm64.op_count == 0)
    return false;
  const cs_aarch64_op &Target = Arm64.operands[Arm64.op_count - 1];
  if (Target.type != AARCH64_OP_IMM || Target.imm < 0)
    return false;
  Control.Target = static_cast<va_t>(Target.imm);
  return true;
}

bool classifyThumbControl(csh Handle, const cs_insn &Insn,
                          DirectControl &Control) {
  if (Insn.id == ARM_INS_IT)
    return false;

  const cs_arm &Arm = Insn.detail->arm;
  const bool IsCall = cs_insn_group(Handle, &Insn, CS_GRP_CALL);
  const bool IsBranch = cs_insn_group(Handle, &Insn, CS_GRP_JUMP);
  const bool IsReturn = cs_insn_group(Handle, &Insn, CS_GRP_RET);
  const bool IsBXReturn = Insn.id == ARM_INS_BX && Arm.op_count == 1 &&
                          Arm.operands[0].type == ARM_OP_REG &&
                          Arm.operands[0].reg == ARM_REG_LR &&
                          isUnconditional(Arm.cc);
  if (IsReturn || IsBXReturn) {
    if (IsCall || !isUnconditional(Arm.cc))
      return false;
    Control.Kind = ControlKind::Return;
    return true;
  }
  if (IsCall == IsBranch && IsCall)
    return false;
  if (!IsCall && !IsBranch)
    return true;
  if (IsCall && Insn.id != ARM_INS_BL && Insn.id != ARM_INS_BLX)
    return false;
  if (IsBranch && Insn.id != ARM_INS_B && Insn.id != ARM_INS_CBZ &&
      Insn.id != ARM_INS_CBNZ)
    return false;

  if (IsCall && !isUnconditional(Arm.cc))
    return false;
  Control.Kind = IsCall ? ControlKind::Call : ControlKind::Branch;
  Control.Conditional =
      IsBranch && (Insn.id != ARM_INS_B || !isUnconditional(Arm.cc));
  Control.SwitchesMode = IsCall && Insn.id == ARM_INS_BLX;
  if (Arm.op_count == 0)
    return false;
  const cs_arm_op &Target = Arm.operands[Arm.op_count - 1];
  if (Target.type != ARM_OP_IMM || Target.imm < 0)
    return false;
  Control.Target = static_cast<va_t>(Target.imm);
  return true;
}

bool classifyControl(const BinaryImage &Img, Arch A, csh Handle,
                     const cs_insn &Insn, DirectControl &Control) {
  if (isTerminalTrap(A, Insn)) {
    Control.Kind = ControlKind::Trap;
    return true;
  }

  bool Valid = false;
  switch (A) {
  case Arch::X64:
  case Arch::X86:
    Valid = classifyX86Control(Img, A, Handle, Insn, Control);
    break;
  case Arch::AArch64:
    Valid = classifyAArch64Control(Handle, Insn, Control);
    break;
  case Arch::ARM:
    Valid = classifyThumbControl(Handle, Insn, Control);
    break;
  default:
    return false;
  }
  if (!Valid)
    return false;
  if (Control.Kind == ControlKind::None &&
      (cs_insn_group(Handle, &Insn, CS_GRP_INT) ||
       cs_insn_group(Handle, &Insn, CS_GRP_IRET)))
    return false;
  return true;
}

bool validInstructionShape(Arch A, const cs_insn &Insn) {
  switch (A) {
  case Arch::X64:
  case Arch::X86:
    return Insn.size <= 15;
  case Arch::AArch64:
    return (Insn.address % 4) == 0 && Insn.size == 4;
  case Arch::ARM:
    return (Insn.address % 2) == 0 && (Insn.size == 2 || Insn.size == 4);
  default:
    return false;
  }
}

enum class DelegationState : uint8_t {
  None,
  CSpecific,
  CxxFH3,
  CxxFH4,
  Conflict,
};

std::optional<DelegationState> delegationStateForName(const std::string &Name) {
  switch (classifyPersonality(Name)) {
  case ExceptionPersonality::CSpecificHandler:
    return DelegationState::CSpecific;
  case ExceptionPersonality::CxxFrameHandler3:
    return DelegationState::CxxFH3;
  case ExceptionPersonality::CxxFrameHandler4:
    return DelegationState::CxxFH4;
  default:
    return std::nullopt;
  }
}

} // namespace

/// Collect direct runtime targets from the reachable CFG of a small wrapper.
/// Any ambiguity invalidates the complete candidate: a branch into an
/// instruction, an overlapping decode, an indirect transfer not backed by an
/// exact IAT slot, or an exhausted analysis budget must not contribute a name.
bool collectDirectCallTargets(const BinaryImage &Img, Arch A, va_t BodyVA,
                              const uint8_t *Code, size_t CodeSize,
                              std::vector<std::string> &Names) {
  Names.clear();
  if (!Code || CodeSize == 0 || CodeSize > MaxWrapperBytes ||
      BodyVA > InvalidVA - CodeSize || !Img.isCodeRange(BodyVA, CodeSize))
    return false;
  if ((A == Arch::AArch64 && ((BodyVA % 4) != 0 || (CodeSize % 4) != 0)) ||
      (A == Arch::ARM && ((BodyVA % 2) != 0 || (CodeSize % 2) != 0)))
    return false;

  cs_arch CSArch;
  cs_mode CSMode;
  switch (A) {
  case Arch::X64:
    CSArch = CS_ARCH_X86;
    CSMode = CS_MODE_64;
    break;
  case Arch::X86:
    CSArch = CS_ARCH_X86;
    CSMode = CS_MODE_32;
    break;
  case Arch::AArch64:
    CSArch = CS_ARCH_AARCH64;
    CSMode = CS_MODE_ARM;
    break;
  case Arch::ARM:
    CSArch = CS_ARCH_ARM;
    CSMode = static_cast<cs_mode>(CS_MODE_THUMB | CS_MODE_V8);
    break;
  default:
    return false;
  }

  CapstoneSession Disassembler(CSArch, CSMode);
  if (!Disassembler)
    return false;

  std::map<size_t, size_t> Instructions;
  std::map<size_t, DirectControl> Controls;
  std::set<size_t> BlockStarts;
  std::vector<size_t> Worklist;
  size_t InstructionCount = 0;
  size_t DecodedBytes = 0;
  size_t EdgeCount = 0;

  auto consumeEdge = [&]() {
    if (EdgeCount == MaxWrapperEdges)
      return false;
    ++EdgeCount;
    return true;
  };

  auto isInstructionInterior = [&](size_t Offset) {
    auto It = Instructions.upper_bound(Offset);
    if (It == Instructions.begin())
      return false;
    --It;
    return Offset > It->first && Offset < It->second;
  };

  auto scheduleBlock = [&](size_t Offset) {
    if (Offset >= CodeSize || isInstructionInterior(Offset))
      return false;
    auto [It, Inserted] = BlockStarts.insert(Offset);
    (void)It;
    if (BlockStarts.size() > MaxWrapperBlocks)
      return false;
    if (Inserted && Instructions.find(Offset) == Instructions.end())
      Worklist.push_back(Offset);
    return true;
  };

  auto scheduleEdge = [&](size_t Offset) {
    return consumeEdge() && scheduleBlock(Offset);
  };

  auto normalizedTarget = [&](va_t Target) {
    return A == Arch::ARM ? (Target & ~va_t(1)) : Target;
  };

  auto validTargetAlignment = [&](va_t Target, bool SwitchesMode) {
    if (A == Arch::AArch64)
      return (Target % 4) == 0;
    if (A == Arch::ARM)
      return (Target % (SwitchesMode ? 4 : 2)) == 0;
    return true;
  };

  auto resolveTargetName = [&](va_t Target, std::string &Name) {
    if (!isExecutableAddress(Img, Target))
      return false;
    Name = resolvePersonality(Img, Target).second;
    if (A == Arch::ARM && Name.empty())
      Name = resolvePersonality(Img, Target | 1).second;
    return true;
  };

  if (!scheduleBlock(0))
    return false;

  for (size_t WorkIndex = 0; WorkIndex < Worklist.size(); ++WorkIndex) {
    const size_t BlockStart = Worklist[WorkIndex];
    if (Instructions.find(BlockStart) != Instructions.end())
      continue;
    if (isInstructionInterior(BlockStart))
      return false;

    size_t Offset = BlockStart;
    while (true) {
      if (Offset >= CodeSize)
        return false;
      if (Offset != BlockStart && BlockStarts.count(Offset) != 0) {
        if (!consumeEdge())
          return false;
        break;
      }
      if (Instructions.find(Offset) != Instructions.end()) {
        if (!consumeEdge())
          return false;
        break;
      }
      if (isInstructionInterior(Offset))
        return false;

      const uint8_t *Cursor = Code + Offset;
      size_t Remaining = CodeSize - Offset;
      const va_t AddressBefore = BodyVA + Offset;
      uint64_t Address = AddressBefore;
      cs_insn *Insn = Disassembler.instruction();
      if (!cs_disasm_iter(Disassembler.handle(), &Cursor, &Remaining, &Address,
                          Insn) ||
          !Insn->detail || Insn->address != AddressBefore || Insn->size == 0 ||
          Insn->size > CodeSize - Offset || !validInstructionShape(A, *Insn) ||
          Cursor != Code + Offset + Insn->size ||
          Address != AddressBefore + Insn->size)
        return false;

      const size_t End = Offset + Insn->size;
      auto Next = Instructions.lower_bound(Offset);
      if ((Next != Instructions.end() && Next->first < End) ||
          (Next != Instructions.begin() && std::prev(Next)->second > Offset))
        return false;
      auto Boundary = BlockStarts.upper_bound(Offset);
      if (Boundary != BlockStarts.end() && *Boundary < End)
        return false;

      if (++InstructionCount > MaxWrapperInstructions ||
          Insn->size > MaxWrapperBytes - DecodedBytes)
        return false;
      DecodedBytes += Insn->size;
      Instructions.emplace(Offset, End);

      DirectControl Control;
      if (!classifyControl(Img, A, Disassembler.handle(), *Insn, Control))
        return false;
      if (Control.Kind == ControlKind::None) {
        Controls.emplace(Offset, std::move(Control));
        if (End == CodeSize)
          return false;
        Offset = End;
        continue;
      }
      if (Control.Kind == ControlKind::Return ||
          Control.Kind == ControlKind::Trap) {
        Controls.emplace(Offset, std::move(Control));
        if (!consumeEdge())
          return false;
        break;
      }

      if (Control.RuntimeName) {
        if (Control.Conditional || !consumeEdge())
          return false;
        if (Control.Kind == ControlKind::Call && !scheduleEdge(End))
          return false;
        Controls.emplace(Offset, std::move(Control));
        break;
      }
      if (!Control.Target)
        return false;

      va_t Target = normalizedTarget(*Control.Target);
      if (!validTargetAlignment(Target, Control.SwitchesMode))
        return false;
      Control.Target = Target;
      const bool IsInternal = Target >= BodyVA && Target < BodyVA + CodeSize;
      if (IsInternal) {
        // Correctly propagating a callee's delegation through an internal
        // call requires a return-sensitive summary.  Treating its target and
        // continuation as independent paths is unsound, so stripped-wrapper
        // inference fails closed for this uncommon shape.
        if (Control.Kind == ControlKind::Call || Control.SwitchesMode)
          return false;
        if (!scheduleEdge(Target - BodyVA))
          return false;
        if (Control.Conditional && !scheduleEdge(End))
          return false;
        Controls.emplace(Offset, std::move(Control));
        break;
      }

      if (Control.Conditional || !consumeEdge())
        return false;
      std::string RuntimeName;
      if (!resolveTargetName(Target, RuntimeName))
        return false;
      Control.RuntimeName = std::move(RuntimeName);
      if (Control.Kind == ControlKind::Call && !scheduleEdge(End))
        return false;
      Controls.emplace(Offset, std::move(Control));
      break;
    }
  }

  if (Controls.size() != Instructions.size())
    return false;

  // The structural pass above proves that every reachable instruction has a
  // single bounded decode.  This second pass requires a unique base
  // personality to be established on at least one normal exit.  A local
  // return without delegation may be the flags-gated cookie-check bypass, but
  // an undelegated external tail exit or conflicting personality still fails
  // closed.  Terminal traps do not satisfy the delegated-exit obligation,
  // although any personality reachable on a trap path still contributes to
  // ambiguity detection.
  using FlowState = std::pair<size_t, DelegationState>;
  std::set<FlowState> SeenStates;
  std::vector<FlowState> FlowWorklist;
  std::map<DelegationState, std::string> DelegationNames;
  std::optional<DelegationState> ExitDelegation;

  auto scheduleFlow = [&](size_t Offset, DelegationState State) {
    if (Controls.find(Offset) == Controls.end())
      return false;
    if (SeenStates.emplace(Offset, State).second)
      FlowWorklist.emplace_back(Offset, State);
    return true;
  };

  auto advanceDelegation = [&](DelegationState State,
                               const std::optional<std::string> &Name) {
    if (!Name)
      return State;
    std::optional<DelegationState> Candidate = delegationStateForName(*Name);
    if (!Candidate)
      return State;
    DelegationNames.try_emplace(*Candidate, *Name);
    if (State == DelegationState::None || State == *Candidate)
      return *Candidate;
    return DelegationState::Conflict;
  };

  auto completeNormalExit = [&](DelegationState State,
                                bool IsLocalReturn) {
    if (State == DelegationState::Conflict)
      return false;
    // The CRT wrapper first validates the cookie and then conditionally calls
    // the base personality.  When the handler-data flags say that the current
    // dispatch kind has no language work, the wrapper reaches its own RET
    // without delegating.  That local bypass is not an alternative
    // personality: keep looking for a delegated normal exit.  An external
    // branch with no delegation remains unproven and fails closed.
    if (State == DelegationState::None)
      return IsLocalReturn;
    if (ExitDelegation && *ExitDelegation != State)
      return false;
    ExitDelegation = State;
    return true;
  };

  if (!scheduleFlow(0, DelegationState::None))
    return false;
  for (size_t WorkIndex = 0; WorkIndex < FlowWorklist.size(); ++WorkIndex) {
    auto [Offset, State] = FlowWorklist[WorkIndex];
    const DirectControl &Control = Controls.at(Offset);
    const size_t End = Instructions.at(Offset);
    State = advanceDelegation(State, Control.RuntimeName);

    switch (Control.Kind) {
    case ControlKind::None:
    case ControlKind::Call:
      if (!scheduleFlow(End, State))
        return false;
      break;
    case ControlKind::Branch:
      if (Control.Target && *Control.Target >= BodyVA &&
          *Control.Target < BodyVA + CodeSize) {
        if (!scheduleFlow(*Control.Target - BodyVA, State) ||
            (Control.Conditional && !scheduleFlow(End, State)))
          return false;
        break;
      }
      if (!completeNormalExit(State, /*IsLocalReturn=*/false))
        return false;
      break;
    case ControlKind::Return:
      if (!completeNormalExit(State, /*IsLocalReturn=*/true))
        return false;
      break;
    case ControlKind::Trap:
      break;
    }
  }

  // Every reachable base-personality call contributes a name, including calls
  // on terminal-trap paths.  Requiring the whole wrapper to name exactly one
  // base prevents a conflicting trap arm from being hidden by the personality
  // selected on the ordinary return path.
  if (!ExitDelegation || DelegationNames.size() != 1)
    return false;
  auto Name = DelegationNames.find(*ExitDelegation);
  if (Name == DelegationNames.end())
    return false;
  Names.push_back(Name->second);
  return true;
}

std::optional<ExceptionPersonality>
inferGSPersonality(const ExceptionFunction &F, const BinaryImage &Img) {
  if (F.PersonalityVA == 0 || F.HandlerDataVA == 0)
    return std::nullopt;

  // On ARM the handler RVA carries the Thumb interworking bit but the runtime
  // function it names does not, so the two spellings have to meet in the
  // middle before the wrapper can be found at all.
  const va_t WrapperVA =
      Img.Arch == Arch::ARM ? (F.PersonalityVA & ~va_t(1)) : F.PersonalityVA;
  const ExceptionFunction *Wrapper = nullptr;
  for (const ExceptionFunction &Candidate : Img.ExceptionMetadata.Functions) {
    if (Candidate.Kind != RuntimeFunctionKind::Primary ||
        !Candidate.CodeRange.isValid())
      continue;
    const va_t CandidateVA = Img.Arch == Arch::ARM
                                 ? (Candidate.CodeRange.Begin & ~va_t(1))
                                 : Candidate.CodeRange.Begin;
    if (CandidateVA == WrapperVA) {
      Wrapper = &Candidate;
      break;
    }
  }
  if (!Wrapper ||
      (Img.Arch == Arch::ARM && (Wrapper->CodeRange.Begin & 1u) != 0) ||
      Wrapper->CodeRange.size() > std::numeric_limits<size_t>::max())
    return std::nullopt;

  // The body starts at the untagged address; reading from the tagged one would
  // shift every instruction by a byte.
  const va_t BodyVA = Img.Arch == Arch::ARM
                          ? (Wrapper->CodeRange.Begin & ~va_t(1))
                          : Wrapper->CodeRange.Begin;
  const size_t CodeSize = static_cast<size_t>(Wrapper->CodeRange.size());
  const uint8_t *Code = Img.readVA(BodyVA, CodeSize);
  if (!Code)
    return std::nullopt;

  // Static runtime wrappers may be stripped of their COFF names.  Require two
  // independent signals before recovering GS provenance: a bounded call from
  // the wrapper runtime function to a named base handler, and a payload that
  // is valid for that handler followed by valid GS cookie data.
  std::vector<std::string> Names;
  if (!collectDirectCallTargets(Img, Img.Arch, BodyVA, Code, CodeSize, Names))
    return std::nullopt;

  ExceptionPersonality BasePersonality = ExceptionPersonality::Unknown;
  for (const std::string &Name : Names) {
    ExceptionPersonality Candidate = classifyPersonality(Name);
    if (Candidate != ExceptionPersonality::CSpecificHandler &&
        Candidate != ExceptionPersonality::CxxFrameHandler3 &&
        Candidate != ExceptionPersonality::CxxFrameHandler4)
      continue;
    if (BasePersonality != ExceptionPersonality::Unknown &&
        BasePersonality != Candidate)
      return std::nullopt;
    BasePersonality = Candidate;
  }
  if (BasePersonality == ExceptionPersonality::Unknown)
    return std::nullopt;

  ExceptionFunction Probe = F;
  Probe.ParseStatus = ExceptionParseStatus::Complete;
  Probe.Diagnostics.clear();
  Probe.SEH.reset();
  Probe.Cxx.reset();
  Probe.GSCookie.reset();
  bool PayloadMatches = false;
  switch (BasePersonality) {
  case ExceptionPersonality::CSpecificHandler:
    if (parseSEH(Probe, Img)) {
      std::optional<va_t> CookieVA = sehGSCookieAddress(Probe, Img);
      PayloadMatches = CookieVA && parseGSCookie(Probe, Img, *CookieVA);
    }
    if (PayloadMatches && Probe.ParseStatus == ExceptionParseStatus::Complete)
      return ExceptionPersonality::GSHandlerCheckSEH;
    break;
  case ExceptionPersonality::CxxFrameHandler3:
    PayloadMatches =
        parseFH3(Probe, Img) &&
        F.HandlerDataVA <= InvalidVA - sizeof(uint32_t) &&
        parseGSCookie(Probe, Img, F.HandlerDataVA + sizeof(uint32_t));
    if (PayloadMatches && Probe.ParseStatus == ExceptionParseStatus::Complete)
      return ExceptionPersonality::GSHandlerCheckEH;
    break;
  case ExceptionPersonality::CxxFrameHandler4:
    PayloadMatches =
        parseFH4(Probe, Img) &&
        F.HandlerDataVA <= InvalidVA - sizeof(uint32_t) &&
        parseGSCookie(Probe, Img, F.HandlerDataVA + sizeof(uint32_t));
    if (PayloadMatches && Probe.ParseStatus == ExceptionParseStatus::Complete)
      return ExceptionPersonality::GSHandlerCheckEH4;
    break;
  default:
    break;
  }
  return std::nullopt;
}

} // namespace neverd::coff_loader::detail
