//===- AArch64UnwindRecovery.cpp - Implicit return provenance ------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#include "neverd/analysis/AArch64UnwindRecovery.h"

#include "neverd/decode/Decoder.h"
#include "neverd/ir/intrinsics/AArch64/AArch64PAuth.h"
#include "neverd/ir/intrinsics/AArch64/AArch64WritebackMemory.h"
#include "neverd/ir/intrinsics/IntrinsicTermination.h"
#include "neverd/libc/LibCNames.h"
#include "neverd/loader/BinaryImage.h"
#include "neverd/loader/ExceptionFunction.h"
#include "neverd/loader/MachO/CompactUnwind.h"
#include "neverd/support/ISAEncoding.h"

#include "llvm/ADT/STLExtras.h"
#include "llvm/Support/Errc.h"
#include "llvm/Support/MathExtras.h"

#include <algorithm>
#include <bitset>
#include <deque>
#include <map>
#include <utility>

namespace neverd {
namespace {
using namespace aarch64_unwind;
using namespace dwarf_eh;
#define NEVERD_UNWIND_TEXT(Name, Text) constexpr char Name[] = Text;
#include "neverd/analysis/AArch64UnwindRecovery.def"

class RecoveryError : public llvm::ErrorInfo<RecoveryError> {
public:
  static char ID;
  AArch64RAQueryResult Result;
  RecoveryError(const char *Message, AArch64RAQueryStatus Status)
      : Result{Status, std::nullopt, Message} {}
  void log(llvm::raw_ostream &OS) const override { OS << Result.Diagnostic; }
  std::error_code convertToErrorCode() const override {
    return Result.Status == AArch64RAQueryStatus::ResourceLimit
               ? llvm::errc::result_out_of_range
               : llvm::errc::not_supported;
  }
};
char RecoveryError::ID;

llvm::Error
fail(const char *Message,
     AArch64RAQueryStatus Status = AArch64RAQueryStatus::Unsupported) {
  return llvm::make_error<RecoveryError>(Message, Status);
}

std::optional<unsigned> reg(unsigned R) {
  if (R >= AARCH64_REG_X0 && R <= AARCH64_REG_X28)
    return R - AARCH64_REG_X0;
  if (R >= AARCH64_REG_W0 && R <= AARCH64_REG_W30)
    return R - AARCH64_REG_W0;
  if (R == AARCH64_REG_FP)
    return kFP;
  if (R == AARCH64_REG_LR)
    return kLR;
  if (R == AARCH64_REG_SP || R == AARCH64_REG_WSP)
    return kSP;
  return std::nullopt;
}

// Capstone reports writes through the smallest view selected by the opcode.
std::optional<unsigned> preservedDSlot(unsigned R) {
  for (unsigned First : {AARCH64_REG_B8, AARCH64_REG_H8, AARCH64_REG_S8,
                         AARCH64_REG_D8, AARCH64_REG_Q8, AARCH64_REG_Z8})
    if (R >= First && R - First < NDArm64ePreservedDCount)
      return kFirstDSlot + R - First;
  return std::nullopt;
}

bool wide(unsigned R) {
  return R == AARCH64_REG_FP || R == AARCH64_REG_LR || R == AARCH64_REG_SP ||
         R == AARCH64_REG_XZR || (R >= AARCH64_REG_X0 && R <= AARCH64_REG_X28);
}

struct Value {
  std::optional<int64_t> StackOffset;
  std::optional<va_t> CodeAddress;
  std::optional<unsigned> IncomingRegister;
  std::optional<AArch64ReturnAddressState> Return;
  bool operator==(const Value &) const = default;
};

struct State {
  std::array<Value, kFrameRegisters> Registers;
  std::map<int64_t, Value> Stack;
  std::optional<bool> PACM = false;
};

template <typename T>
bool intersect(std::optional<T> &A, const std::optional<T> &B) {
  if (!A || A == B)
    return false;
  A.reset();
  return true;
}

bool intersect(Value &A, const Value &B) {
  bool Changed = intersect(A.StackOffset, B.StackOffset);
  Changed |= intersect(A.CodeAddress, B.CodeAddress);
  Changed |= intersect(A.IncomingRegister, B.IncomingRegister);
  Changed |= intersect(A.Return, B.Return);
  return Changed;
}

bool merge(State &A, const State &B) {
  bool Changed = intersect(A.PACM, B.PACM);
  for (unsigned R = 0; R < kFrameRegisters; ++R)
    Changed |= intersect(A.Registers[R], B.Registers[R]);
  for (auto I = A.Stack.begin(); I != A.Stack.end();) {
    const auto Other = B.Stack.find(I->first);
    if (Other != B.Stack.end()) {
      Changed |= intersect(I->second, Other->second);
      if (I->second != Value{}) {
        ++I;
        continue;
      }
    }
    I = A.Stack.erase(I);
    Changed = true;
  }
  return Changed;
}

std::optional<int64_t> add(std::optional<int64_t> Base, int64_t Delta) {
  int64_t Result;
  if (!Base || llvm::AddOverflow(*Base, Delta, Result))
    return std::nullopt;
  return Result;
}

struct FrameRow {
  uint32_t OutgoingArgsSize = 0;
  std::optional<unsigned> CFARegister;
  int64_t CFAOffset = 0;
  std::array<AArch64UnwindRegister, kFrameRegisters> Registers;
  std::bitset<NDArm64ePreservedDCount> DeclaredD;
};

llvm::Expected<std::map<uint64_t, FrameRow>> frameRows(const DwarfCIE &CIE,
                                                       const DwarfFDE &FDE,
                                                       size_t Limit,
                                                       bool HasLandingPads) {
  FrameRow Row, Initial;
  std::vector<FrameRow> Remembered;
  std::map<uint64_t, FrameRow> Rows;
  for (const auto *Program : {&CIE.InitialInstructions, &FDE.Instructions}) {
    for (const auto &I : *Program) {
      switch (I.Kind) {
      case CFIOpKind::DefCFA:
      case CFIOpKind::DefCFARegister:
        if (I.Register != kSP && I.Register != kFP)
          return fail(kCFI);
        if (!Row.CFARegister && I.Kind == CFIOpKind::DefCFARegister)
          return fail(kCFI);
        Row.CFARegister = I.Register;
        if (I.Kind == CFIOpKind::DefCFA)
          Row.CFAOffset = I.Offset;
        break;
      case CFIOpKind::DefCFAOffset:
        if (!Row.CFARegister)
          return fail(kCFI);
        Row.CFAOffset = I.Offset;
        break;
      case CFIOpKind::Offset:
      case CFIOpKind::Restore:
      case CFIOpKind::SameValue:
      case CFIOpKind::Undefined: {
        auto R = I.Register;
        if (R >= kRegisters) {
          constexpr unsigned First = kDwarfV0 + NDArm64eFirstPreservedD;
          if (R < First || R - First >= NDArm64ePreservedDCount)
            break;
          const unsigned D = R - First;
          Row.DeclaredD[D] =
              I.Kind != CFIOpKind::Restore || Initial.DeclaredD[D];
          R = kFirstDSlot + D;
        }
        if (I.Kind == CFIOpKind::Offset)
          Row.Registers[R] = {AArch64UnwindRegister::AtCFA, I.Offset};
        else if (I.Kind == CFIOpKind::Restore)
          Row.Registers[R] = Initial.Registers[R];
        else
          Row.Registers[R] = {I.Kind == CFIOpKind::SameValue
                                  ? AArch64UnwindRegister::SameValue
                                  : AArch64UnwindRegister::Undefined,
                              0};
        break;
      }
      case CFIOpKind::RememberState:
        if (Remembered.size() >= Limit)
          return fail(kBudget, AArch64RAQueryStatus::ResourceLimit);
        Remembered.push_back(Row);
        break;
      case CFIOpKind::RestoreState:
        if (Remembered.empty())
          return fail(kCFI);
        Row = Remembered.back();
        Remembered.pop_back();
        break;
      case CFIOpKind::Nop:
      case CFIOpKind::AdvanceLoc:
      case CFIOpKind::SetLoc:
        break;
      case CFIOpKind::GnuArgsSize:
        if (HasLandingPads) {
          if (!std::in_range<uint32_t>(I.Offset))
            return fail(kCFI);
          Row.OutgoingArgsSize = uint32_t(I.Offset);
        }
        break;
      default:
        return fail(kCFI);
      }
      if (Program == &FDE.Instructions)
        Rows[I.CodeOffset] = Row;
    }
    if (Program == &CIE.InitialInstructions) {
      Initial = Row;
      Rows[0] = Row;
    }
  }
  return Rows;
}

// Signatures are recipes on the incoming LR. AUT proves an unsigned result
// only when both its modifiers match the recorded signing operation.
void updateSignature(State &S, const State &Before, AArch64RAKey Key,
                     bool Authenticate, std::optional<AArch64RARecipe> Recipe,
                     std::optional<va_t> ModifierPC) {
  auto &LR = S.Registers[kLR];
  LR = {};
  const auto &Prior = Before.Registers[kLR].Return;
  if (!Prior || Before.Registers[kSP].StackOffset != 0 ||
      (Recipe && !ModifierPC))
    return;
  AArch64ReturnAddressState Signed;
  Signed.Key = Key;
  Signed.Kind =
      Recipe ? AArch64RASigningKind::SPAndPC : AArch64RASigningKind::SP;
  if (Recipe) {
    Signed.Recipe = *Recipe;
    Signed.SigningPC = ModifierPC;
  }
  if (Authenticate && *Prior == Signed)
    LR.Return = AArch64ReturnAddressState{};
  else if (!Authenticate && Prior->Kind == AArch64RASigningKind::Unsigned)
    LR.Return = Signed;
}

bool pointerOperation(State &S, const State &Before, uint32_t Word, va_t PC) {
  // Use the lifter's complete encodings, including hints presented as NOPs.
  enum class Operand { X15, X16, X17, X30, Zero, SP, PC, PCRel, Rn };
  const auto UpdateHint = [&](Intrinsic Id, Operand Pointer, Operand Modifier) {
    const unsigned Dest = Pointer == Operand::X30 ? kLR : kHintDestination;
    S.Registers[Dest] = {};
    if (Dest != kLR || Modifier != Operand::SP || !Before.PACM)
      return;
    const auto Op = *aarch64PAuthOperation(Id);
    const auto Key =
        Op.Key == kAArch64PAuthKeyIB ? AArch64RAKey::B : AArch64RAKey::A;
    const auto Recipe =
        *Before.PACM ? std::optional(AArch64RARecipe::HintPACM) : std::nullopt;
    const auto ModPC = Op.Authenticate
                           ? Before.Registers[kPACMAuthModifier].CodeAddress
                           : std::optional(PC);
    updateSignature(S, Before, Key, Op.Authenticate, Recipe, ModPC);
  };
  switch (Word) {
#define NEVERD_A64_PAUTH_HINT(Insn, Encoding, Id, Pointer, Mod)                \
  case Encoding:                                                               \
    UpdateHint(Intrinsic::Id, Operand::Pointer, Operand::Mod);                 \
    return true;
#define NEVERD_A64_PAUTH_STRIP_HINT(Insn, Encoding, Id, Pointer)               \
  case Encoding:                                                               \
    S.Registers[kLR] = {};                                                     \
    return true;
#include "neverd/ir/intrinsics/AArch64/AArch64PAuth.def"
  default:
    break;
  }
  const auto UpdateExplicit = [&](Intrinsic Id, Operand Pointer, Operand Mod2) {
    if (Pointer != Operand::X30) {
      S.Registers[kHintDestination] = {};
      return;
    }
    const auto Op = *aarch64PAuthLROperation(Id);
    const auto Key =
        Op.Key == kAArch64PAuthKeyIB ? AArch64RAKey::B : AArch64RAKey::A;
    std::optional<va_t> Address;
    if (Mod2 == Operand::PC)
      Address = PC;
    else if (Mod2 == Operand::PCRel) {
      const auto Distance =
          ((Word >> kAArch64PAuthLRImmShift) & kAArch64PAuthLRImmMask) *
          kAArch64PAuthLRScale;
      if (Distance <= PC)
        Address = PC - Distance;
    } else if (Mod2 == Operand::Rn) {
      const auto R = (Word >> kAArch64PAuthLRImmShift) & kAArch64PAuthLRRegMask;
      Address =
          R == kSP ? std::optional<va_t>(0) : Before.Registers[R].CodeAddress;
    }
    updateSignature(S, Before, Key, Op.Authenticate,
                    AArch64RARecipe::NonHintPAuthLR, Address);
  };
#define NEVERD_A64_PAUTH_LR_INSTRUCTION(Mask, Encoding, Id, Pointer, Mod,      \
                                        Mod2)                                  \
  if ((Word & Mask) == Encoding) {                                             \
    UpdateExplicit(Intrinsic::Id, Operand::Pointer, Operand::Mod2);            \
    return true;                                                               \
  }
#include "neverd/ir/intrinsics/AArch64/AArch64PAuthLR.def"
  return false;
}

// Stack cells describe declared frame saves. Foreign stores cannot establish a
// saved return. Runtime authentication still checks actual memory against the
// resolved recipe, just as the source CFI requires uncorrupted register saves.
void memory(State &S, const State &Before, const DecodedInsn &I) {
  const auto &D = I.Raw->detail->aarch64;
  const auto Word = readLE<uint32_t>(I.Raw->bytes);
  const auto AddressOp = aarch64AddressMemoryInstruction(Word);
  const auto WritebackOp = aarch64WritebackMemoryInstruction(Word);
  const unsigned Bytes = AddressOp     ? AddressOp->accessBytes()
                         : WritebackOp ? WritebackOp->accessBytes()
                                       : 0;
  const bool Load =
      AddressOp ? AddressOp->isLoad() : WritebackOp && WritebackOp->isLoad();
  const bool Store = Bytes && !Load;
  const auto Ops = llvm::ArrayRef(D.operands, D.op_count);
  for (const auto &[Index, Op] : llvm::enumerate(Ops)) {
    if (Op.type != AARCH64_OP_MEM)
      continue;
    const auto Base = reg(Op.mem.base);
    if (!Base)
      continue;
    const auto Address = Op.mem.index == AARCH64_REG_INVALID
                             ? add(Before.Registers[*Base].StackOffset,
                                   D.post_index ? 0 : Op.mem.disp)
                             : std::nullopt;
    if (I.Raw->detail->writeback) {
      S.Registers[*Base] = {};
      if (D.post_index && Index + 1 < Ops.size()) {
        if (Ops[Index + 1].type == AARCH64_OP_IMM)
          S.Registers[*Base].StackOffset =
              add(Before.Registers[*Base].StackOffset, Ops[Index + 1].imm);
      } else if (Op.mem.index == AARCH64_REG_INVALID) {
        S.Registers[*Base].StackOffset =
            add(Before.Registers[*Base].StackOffset, Op.mem.disp);
      }
    }
    if (!Address) {
      if (Before.Registers[*Base].StackOffset &&
          (Store || (Op.access & CS_AC_WRITE)))
        S.Stack.clear();
      continue;
    }
    if (!Load && !Store) {
      if (Op.access & CS_AC_WRITE)
        S.Stack.clear();
      continue;
    }
    // Structure accesses interleave elements or update individual lanes.
    // They cannot be treated as flat D/Q saves at successive offsets.
    const bool Structure = WritebackOp && WritebackOp->isStructure();
    if (Structure || llvm::any_of(Ops.take_front(Index), [](const auto &O) {
          return O.type != AARCH64_OP_REG;
        })) {
      if (Store)
        S.Stack.clear();
      continue;
    }
    for (size_t N = 0; N < Index; ++N) {
      const unsigned Reg = Ops[N].reg;
      const auto Slot = add(Address, N * Bytes);
      if (!Slot)
        continue;
      if (Store) {
        for (auto It = S.Stack.begin(); It != S.Stack.end();) {
          const bool Overlaps =
              It->first <= *Slot
                  ? uint64_t(*Slot) - uint64_t(It->first) < kRegisterBytes
                  : uint64_t(It->first) - uint64_t(*Slot) < Bytes;
          if (Overlaps)
            It = S.Stack.erase(It);
          else
            ++It;
        }
      }
      const auto Transfer = [&](unsigned R, int64_t Offset) {
        if (Store) {
          if (Before.Registers[R] != Value{})
            S.Stack[Offset] = Before.Registers[R];
        } else {
          const auto It = Before.Stack.find(Offset);
          if (It != Before.Stack.end())
            S.Registers[R] = It->second;
        }
      };
      if (const auto R = reg(Reg); R && wide(Reg) && Bytes == kRegisterBytes) {
        Transfer(*R, *Slot);
      } else if (const auto D = preservedDSlot(Reg);
                 D && Bytes >= kRegisterBytes) {
        Transfer(*D, *Slot);
        if (Bytes == 2 * kRegisterBytes)
          if (const auto High = add(Slot, kRegisterBytes))
            Transfer(*D + NDArm64ePreservedDCount, *High);
      }
    }
  }
}

void discardInactiveStack(State &S) {
  if (const auto SP = S.Registers[kSP].StackOffset)
    S.Stack.erase(S.Stack.begin(), S.Stack.lower_bound(*SP));
  else
    S.Stack.clear();
}

void clobberCallState(State &S) {
  for (unsigned R = 0; R < kVolatileEnd; ++R)
    S.Registers[R] = {};
  S.Registers[kLR] = {};
  for (unsigned R = kFirstHighSlot; R < kFrameRegisters; ++R)
    S.Registers[R] = {};
  S.PACM = false;
  // A callee owns the stack below its incoming SP, including its red zone.
  discardInactiveStack(S);
}

bool transfer(State &S, const State &Before, Decoder &Decode,
              const DecodedInsn &I) {
  const auto &D = I.Raw->detail->aarch64;
  const auto Ops = llvm::ArrayRef(D.operands, D.op_count);
  cs_regs Reads, Writes;
  uint8_t NumReads = 0, NumWrites = 0;
  if (cs_regs_access(Decode.getHandle(), I.Raw, Reads, &NumReads, Writes,
                     &NumWrites) != CS_ERR_OK)
    return false;
  for (const auto R : llvm::ArrayRef(Writes, NumWrites)) {
    if (const auto Number = reg(R))
      S.Registers[*Number] = {};
    else if (const auto D = preservedDSlot(R)) {
      S.Registers[*D] = {};
      S.Registers[*D + NDArm64ePreservedDCount] = {};
    }
  }
  S.PACM = false;
  const uint32_t Word = readLE<uint32_t>(I.Raw->bytes);
  if (Word == kAArch64PACMEncoding) {
    S.PACM = true;
    return true;
  }
  if (pointerOperation(S, Before, Word, I.Addr))
    return true;
  if ((I.Id == AARCH64_INS_ADR || I.Id == AARCH64_INS_ADRP) &&
      Ops.size() == 2 && Ops[0].type == AARCH64_OP_REG &&
      Ops[1].type == AARCH64_OP_IMM)
    if (const auto R = reg(Ops[0].reg))
      S.Registers[*R].CodeAddress = static_cast<va_t>(Ops[1].imm);
  if ((I.Id == AARCH64_INS_MOV || I.Raw->alias_id == AARCH64_INS_ALIAS_MOV) &&
      Ops.size() == 2 && Ops[0].type == AARCH64_OP_REG &&
      Ops[1].type == AARCH64_OP_REG && wide(Ops[0].reg) && wide(Ops[1].reg)) {
    const auto Dest = reg(Ops[0].reg), Src = reg(Ops[1].reg);
    if (Dest && Src)
      S.Registers[*Dest] = Before.Registers[*Src];
  }
  if ((I.Id == AARCH64_INS_ADD || I.Id == AARCH64_INS_SUB) && Ops.size() == 3 &&
      Ops[0].type == AARCH64_OP_REG && Ops[1].type == AARCH64_OP_REG &&
      Ops[2].type == AARCH64_OP_IMM && !Ops[2].shift.value &&
      wide(Ops[0].reg) && wide(Ops[1].reg)) {
    const auto Dest = reg(Ops[0].reg), Src = reg(Ops[1].reg);
    if (Dest && Src && Ops[2].imm >= 0) {
      const auto &Base = Before.Registers[*Src];
      const uint64_t Magnitude = Ops[2].imm;
      if (!Magnitude) {
        S.Registers[*Dest] = Base;
      } else {
        const bool Subtract = I.Id == AARCH64_INS_SUB;
        S.Registers[*Dest].StackOffset =
            add(Base.StackOffset, Subtract ? -Ops[2].imm : Ops[2].imm);
        if (Base.CodeAddress &&
            (Subtract ? *Base.CodeAddress >= Magnitude
                      : Magnitude <= UINT64_MAX - *Base.CodeAddress))
          S.Registers[*Dest].CodeAddress = Subtract
                                               ? *Base.CodeAddress - Magnitude
                                               : *Base.CodeAddress + Magnitude;
      }
    }
  }
  memory(S, Before, I);
  if (cs_insn_group(Decode.getHandle(), I.Raw, CS_GRP_CALL))
    clobberCallState(S);
  return true;
}

// Compact unwind describes a completed prologue. Synchronous DWARF can also
// leave that row in effect while the frame is being built or torn down.
// Use its register columns when the declared CFA is the incoming SP. Otherwise
// reconstruct an interrupt row from carriers that agree on every path.
FrameRow instructionFrameRow(const State &S, const FrameRow *Declared) {
  const bool UseCFI =
      Declared && add(S.Registers[*Declared->CFARegister].StackOffset,
                      Declared->CFAOffset) == 0;
  FrameRow Row = UseCFI ? *Declared : FrameRow{};
  const auto SP = S.Registers[kSP].StackOffset;
  if (!UseCFI)
    for (unsigned Base : {kSP, kFP}) {
      const auto Offset = S.Registers[Base].StackOffset;
      if (Offset && *Offset != INT64_MIN) {
        Row.CFARegister = Base;
        Row.CFAOffset = -*Offset;
        break;
      }
    }
  for (unsigned R = UseCFI ? kFirstDSlot : 0; R < kFrameRegisters; ++R) {
    if (R == kSP ||
        (UseCFI && R < kFirstHighSlot && Declared->DeclaredD[R - kFirstDSlot]))
      continue;
    if (Declared &&
        Declared->Registers[R].Location == AArch64UnwindRegister::Undefined) {
      Row.Registers[R] = Declared->Registers[R];
      continue;
    }
    const auto Holds = [&](const Value &V) {
      return R == kLR ? V.Return.has_value() : V.IncomingRegister == R;
    };
    if (Holds(S.Registers[R]))
      continue;
    auto &Rule = Row.Registers[R];
    Rule.Location = AArch64UnwindRegister::Undefined;
    if (R < NDArm64eFirstPreservedGPR || !SP)
      continue;
    std::optional<AArch64ReturnAddressState> SavedReturn;
    for (auto I = S.Stack.lower_bound(*SP);
         I != S.Stack.end() && I->first <= -int(kRegisterBytes); ++I) {
      if (!Holds(I->second))
        continue;
      if (R == kLR && SavedReturn && SavedReturn != I->second.Return) {
        // No declared LR column selects between differently signed copies.
        Rule = {AArch64UnwindRegister::Undefined, 0};
        break;
      }
      if (Rule.Location == AArch64UnwindRegister::Undefined)
        Rule = {AArch64UnwindRegister::AtCFA, I->first};
      if (R != kLR)
        break;
      SavedReturn = I->second.Return;
    }
  }
  return Row;
}

bool implicitProfile(const BinaryImage &Image,
                     const ExceptionFunction &Function,
                     const AArch64UnwindSigningQuery &Query) {
  if (!Image.isArm64e())
    return false;
  if (Function.Dwarf)
    return Query.HasImplicitDarwinSigning;
  if (!Query.CompactRecipe || !Function.Compact)
    return false;
  const auto &Entry = *Function.Compact;
  return (Query.CompactRecipe->Status == AArch64RAQueryStatus::Unknown ||
          Query.CompactRecipe->Status == AArch64RAQueryStatus::Known) &&
         (Entry.Kind == CompactUnwindKind::FramePointer ||
          Entry.Kind == CompactUnwindKind::FramelessImmediate) &&
         !(Entry.NativeEncoding & macho_unwind::kIsNotFunctionStart);
}

// Darwin C++ and Objective-C personalities select a landing pad by its PC
// range. Index only the owning frame's aligned, disjoint ranges. In
// particular, the entry order of an SJLJ table does not describe PC ranges.
llvm::Expected<std::vector<size_t>>
landingPads(const ExceptionFunction &Function, ExceptionAddressRange Range,
            size_t Count, size_t Limit) {
  const va_t DwarfLSDA = Function.Dwarf ? Function.Dwarf->LSDAVA : 0;
  const auto *Compact = Function.Compact ? &*Function.Compact : nullptr;
  const va_t CompactLSDA = Compact ? Compact->LSDAVA : 0;
  const bool HasCompactLSDA =
      Compact &&
      (Compact->HasLSDA || (Compact->NativeEncoding & macho_unwind::kHasLSDA));
  if (!Function.Itanium && !DwarfLSDA && !CompactLSDA && !HasCompactLSDA)
    return std::vector<size_t>{};
  const auto &Info = Function.Itanium;
  if (!Info ||
      (Function.Personality != ExceptionPersonality::GxxPersonalityV0 &&
       Function.Personality != ExceptionPersonality::ObjCPersonalityV0) ||
      !Info->IsCallSiteAddressForm || !Info->LSDAVA ||
      (!DwarfLSDA && !CompactLSDA) ||
      (DwarfLSDA && DwarfLSDA != Info->LSDAVA) ||
      ((HasCompactLSDA || CompactLSDA) && CompactLSDA != Info->LSDAVA))
    return fail(kLandingPad);
  if (Info->CallSites.size() > Limit)
    return fail(kBudget, AArch64RAQueryStatus::ResourceLimit);

  std::vector<size_t> Pads(Count, Count);
  va_t PreviousEnd = Range.Begin;
  for (const auto &Site : Info->CallSites) {
    const auto Guard = Site.GuardedRange;
    if (Guard.Begin == Guard.End)
      continue; // An empty encoded range protects no instructions.
    if (!Range.contains(Guard) || Guard.Begin < PreviousEnd ||
        Guard.Begin % neverd::aarch64::kInsnSize ||
        Guard.End % neverd::aarch64::kInsnSize)
      return fail(kLandingPad);
    PreviousEnd = Guard.End;
    if (!Site.LandingPadVA)
      continue;
    if (!Range.contains(Site.LandingPadVA) ||
        Site.LandingPadVA % neverd::aarch64::kInsnSize)
      return fail(kLandingPad);
    const auto Index = [&](va_t PC) {
      return (PC - Range.Begin) / neverd::aarch64::kInsnSize;
    };
    std::fill(Pads.begin() + Index(Guard.Begin),
              Pads.begin() + Index(Guard.End), Index(Site.LandingPadVA));
  }
  return Pads;
}

llvm::Expected<std::vector<AArch64RecoveredUnwindRow>>
recoverImplicitFrame(const BinaryImage &Image,
                     const ExceptionFunction &Function,
                     const AArch64UnwindRecoveryLimits &Limits) {
  const auto Start = Function.CodeRange.Begin;
  const uint64_t Size = Function.Dwarf ? Function.Dwarf->AddressRange
                                       : Function.Compact->CodeRange.size();
  if (Start % neverd::aarch64::kInsnSize || Size % neverd::aarch64::kInsnSize)
    return fail(kProfile);
  const auto Count = Size / neverd::aarch64::kInsnSize;
  if (Count > Limits.MaxInstructions)
    return fail(kBudget, AArch64RAQueryStatus::ResourceLimit);
  auto Pads = landingPads(Function, {Start, Start + Size}, Count,
                          Limits.MaxInstructions);
  if (!Pads)
    return Pads.takeError();
  std::map<uint64_t, FrameRow> Rules;
  if (Function.Dwarf) {
    const auto &FDE = *Function.Dwarf;
    const auto *CIE = Image.ExceptionMetadata.findCIE(FDE.CIESectionOffset);
    if (FDE.InitialLocation != Start || CIE->IsSignalFrame ||
        CIE->ReturnAddressRegister != kLR)
      return fail(kProfile);
    auto Parsed = frameRows(*CIE, FDE, Limits.MaxInstructions, !Pads->empty());
    if (!Parsed)
      return Parsed.takeError();
    Rules = std::move(*Parsed);
    for (const auto &[Offset, Row] : Rules)
      if (!Row.CFARegister || Offset % neverd::aarch64::kInsnSize ||
          Offset > Size)
        return fail(kCFI);
  } else if (Function.Compact->CodeRange.Begin != Start) {
    return fail(kProfile);
  }
  const auto *Bytes = Image.readVA(Start, Size);
  const auto Data = Image.DataInCode.lower_bound(Start);
  if (!Bytes || Image.nextKnownFunctionEntryAfter(Start) < Start + Size ||
      !Image.isCodeRange(Start, Size) ||
      (Data != Image.DataInCode.end() && *Data - Start < Size))
    return fail(kCode);
  Decoder Decode;
  if (!Decode.init(Image))
    return fail(kCode);
  std::vector<std::optional<State>> States(Count);
  State Entry;
  Entry.Registers[kSP].StackOffset = 0;
  Entry.Registers[kLR].Return = AArch64ReturnAddressState{};
  for (unsigned R = NDArm64eFirstPreservedGPR; R <= kFP; ++R)
    Entry.Registers[R].IncomingRegister = R;
  for (unsigned R = kFirstDSlot; R < kFrameRegisters; ++R)
    Entry.Registers[R].IncomingRegister = R;
  States[0] = std::move(Entry);
  std::deque<size_t> Work = {0};
  size_t Transitions = 0;
  while (!Work.empty()) {
    if (++Transitions > Limits.MaxTransitions)
      return fail(kBudget, AArch64RAQueryStatus::ResourceLimit);
    const size_t Index = Work.front();
    Work.pop_front();
    const uint64_t Offset = Index * neverd::aarch64::kInsnSize;
    DecodedInsn I{};
    if (Decode.decodeOne(Bytes + Offset, Size - Offset, Start + Offset, I) !=
            int(neverd::aarch64::kInsnSize) ||
        !I.Raw || !I.Raw->detail)
      return fail(kCode);
    State After = *States[Index];
    if (!transfer(After, *States[Index], Decode, I))
      return fail(kCode);
    if (After.Stack.size() > Limits.MaxStackCells)
      return fail(kBudget, AArch64RAQueryStatus::ResourceLimit);
    const auto &D = I.Raw->detail->aarch64;
    const auto Ops = llvm::ArrayRef(D.operands, D.op_count);
    const auto Queue = [&](size_t Next, const State &Incoming) {
      if (Next >= Count)
        return;
      if (!States[Next]) {
        States[Next] = Incoming;
        Work.push_back(Next);
      } else if (merge(*States[Next], Incoming)) {
        Work.push_back(Next);
      }
    };
    const bool IsCall = cs_insn_group(Decode.getHandle(), I.Raw, CS_GRP_CALL);
    if (!Pads->empty() && (*Pads)[Index] != Count) {
      // The unwinder preserves the callee-saved registers and frame slots.
      // The personality supplies its exception arguments. No incoming LR or
      // volatile value can be inferred from them. A protected range can also
      // include non-call instructions. Retain only facts common to either
      // side of such an instruction when admitting that exceptional edge.
      State Handler = After;
      if (!IsCall) {
        merge(Handler, *States[Index]);
        clobberCallState(Handler);
      }
      if (!Rules.empty()) {
        const auto PCOffset = Index * neverd::aarch64::kInsnSize;
        const auto Size =
            std::prev(Rules.upper_bound(PCOffset))->second.OutgoingArgsSize;
        if (Size) {
          // Darwin libunwind pops these outgoing arguments when resuming the
          // handler. Ordinary call fallthrough still owns the argument area.
          auto &SP = Handler.Registers[kSP].StackOffset;
          SP = add(SP, Size);
          discardInactiveStack(Handler);
        }
      }
      Queue((*Pads)[Index], Handler);
    }
    if (I.Id == AARCH64_INS_B || I.Id == AARCH64_INS_CBZ ||
        I.Id == AARCH64_INS_CBNZ || I.Id == AARCH64_INS_TBZ ||
        I.Id == AARCH64_INS_TBNZ) {
      if (Ops.empty() || Ops.back().type != AARCH64_OP_IMM)
        return fail(kCode);
      const auto Target = va_t(Ops.back().imm);
      if (Target >= Start && Target - Start < Size) {
        if ((Target - Start) % neverd::aarch64::kInsnSize)
          return fail(kCode);
        Queue((Target - Start) / neverd::aarch64::kInsnSize, After);
      }
      if (I.Id != AARCH64_INS_B ||
          (D.cc != AArch64CC_Invalid && D.cc != AArch64CC_AL &&
           D.cc != AArch64CC_NV)) {
        if (Index + 1 == Count)
          return fail(kCode);
        Queue(Index + 1, After);
      }
    } else if (cs_insn_group(Decode.getHandle(), I.Raw, CS_GRP_RET) ||
               isAArch64PAuthLRReturnWord(readLE<uint32_t>(I.Raw->bytes))) {
      continue;
    } else if (IsCall) {
      if (I.Id == AARCH64_INS_BL && Ops.size() == 1 &&
          Ops.front().type == AARCH64_OP_IMM) {
        const va_t Target = Ops.front().imm;
        if (Target - Start < Size)
          return fail(kCode);
        if (libc::isNoReturnTarget(Image, Target))
          continue;
      }
      if (Index + 1 == Count)
        return fail(kCode);
      Queue(Index + 1, After);
    } else if (I.Id == AARCH64_INS_HLT || I.Id == AARCH64_INS_UDF ||
               (I.Id == AARCH64_INS_BRK && Ops.size() == 1 &&
                Ops.front().type == AARCH64_OP_IMM &&
                intrinsicNeverReturns(Intrinsic::Brk, Ops.front().imm))) {
      // A compiler trap ends this path. The interrupted PC still has a row.
      continue;
    } else if (I.Id == AARCH64_INS_BRK ||
               cs_insn_group(Decode.getHandle(), I.Raw, CS_GRP_INT) ||
               cs_insn_group(Decode.getHandle(), I.Raw, CS_GRP_JUMP) ||
               cs_insn_group(Decode.getHandle(), I.Raw, CS_GRP_IRET)) {
      return fail(kCode);
    } else {
      if (Index + 1 == Count)
        return fail(kCode);
      Queue(Index + 1, After);
    }
  }
  std::vector<AArch64RecoveredUnwindRow> Result;
  for (size_t I = 0; I < Count; ++I) {
    const auto Offset = I * neverd::aarch64::kInsnSize;
    const FrameRow *Declared =
        Rules.empty() ? nullptr : &std::prev(Rules.upper_bound(Offset))->second;
    FrameRow Rule;
    std::optional<AArch64ReturnAddressState> Signing;
    if (States[I]) {
      const auto &S = *States[I];
      Rule = instructionFrameRow(S, Declared);
      if (Rule.CFARegister) {
        const auto &LR = Rule.Registers[kLR];
        if (LR.Location == AArch64UnwindRegister::SameValue)
          Signing = S.Registers[kLR].Return;
        else if (LR.Location == AArch64UnwindRegister::AtCFA) {
          const auto Slot = S.Stack.find(LR.Offset);
          if (Slot != S.Stack.end())
            Signing = Slot->second.Return;
        }
      }
    } else if (Declared) {
      Rule = *Declared;
    }
    if (!Result.empty() && Result.back().CFARegister == Rule.CFARegister &&
        Result.back().CFAOffset == Rule.CFAOffset &&
        Result.back().Registers == Rule.Registers &&
        Result.back().Signing.State == Signing)
      continue;
    Result.push_back(
        {Offset,
         Rule.CFARegister,
         Rule.CFAOffset,
         Rule.Registers,
         {Signing ? AArch64RAQueryStatus::Known : AArch64RAQueryStatus::Unknown,
          Signing, Signing ? nullptr : kUnknown}});
  }
  return Result;
}

} // namespace

llvm::Expected<std::vector<AArch64RecoveredUnwindRow>>
recoverDarwinAArch64Unwind(const BinaryImage &Image,
                           const ExceptionFunction &Function,
                           const AArch64UnwindRecoveryLimits &Limits) {
  const auto Query =
      queryAArch64UnwindSigning(Image, Function, Function.CodeRange.Begin);
  if (!implicitProfile(Image, Function, Query))
    return fail(kProfile);
  return recoverImplicitFrame(Image, Function, Limits);
}

const AArch64RecoveredUnwindRow &
aarch64UnwindRowAt(llvm::ArrayRef<AArch64RecoveredUnwindRow> Rows,
                   uint64_t Offset) {
  return *std::prev(
      llvm::upper_bound(Rows, Offset, [](uint64_t PC, const auto &R) {
        return PC < R.CodeOffset;
      }));
}

std::optional<AArch64SystemCallerPlan>
prepareDarwinArm64eSystemCaller(const AArch64RecoveredUnwindRow &Row,
                                ExceptionAddressRange Range,
                                va_t LoadedFrameStart) {
  if (!Row.Signing.State || Row.Signing.Status != AArch64RAQueryStatus::Known ||
      (Row.CFARegister != kSP && Row.CFARegister != kFP) ||
      Row.CodeOffset >= Range.size() ||
      !std::in_range<ptrdiff_t>(Row.CFAOffset))
    return std::nullopt;
  AArch64SystemCallerPlan Plan;
  auto &Frame = Plan.Frame;
  Frame.CFAFromFP = Row.CFARegister == kFP;
  Frame.CFAOffset = Row.CFAOffset;
  for (unsigned R = NDArm64eFirstPreservedGPR; R <= kLR; ++R) {
    const auto &Rule = Row.Registers[R];
    if (Rule.Location == AArch64UnwindRegister::SameValue)
      continue;
    if (Rule.Location != AArch64UnwindRegister::AtCFA ||
        !std::in_range<ptrdiff_t>(Rule.Offset))
      return std::nullopt;
    if (R == kLR) {
      Frame.SavedLR = true;
      Frame.LROffset = Rule.Offset;
    } else if (R == kFP) {
      Frame.SavedFP = true;
      Frame.FPOffset = Rule.Offset;
    } else {
      Plan.Saved[Plan.Count++] = {R, static_cast<ptrdiff_t>(Rule.Offset)};
    }
  }
  auto &Vectors = Plan.Vectors;
  Vectors.Complete = true;
  for (unsigned D = 0; D < NDArm64ePreservedDCount; ++D) {
    const auto &Low = Row.Registers[kFirstDSlot + D];
    const auto &High = Row.Registers[kFirstHighSlot + D];
    if (Low.Location == AArch64UnwindRegister::AtCFA &&
        std::in_range<ptrdiff_t>(Low.Offset)) {
      Vectors.LowMask |= 1u << D;
      Vectors.LowOffsets[D] = Low.Offset;
    } else if (Low.Location != AArch64UnwindRegister::SameValue) {
      Vectors.Complete = false;
    }
    if (High.Location == AArch64UnwindRegister::AtCFA &&
        std::in_range<ptrdiff_t>(High.Offset)) {
      Vectors.HighMask |= 1u << D;
      Vectors.HighOffsets[D] = High.Offset;
    }
  }
  const auto &State = *Row.Signing.State;
  const bool B = State.Key == AArch64RAKey::B;
  switch (State.Kind) {
  case AArch64RASigningKind::Unsigned:
    Frame.Signing = NDArm64eUnsigned;
    break;
  case AArch64RASigningKind::SP:
    Frame.Signing = B ? NDArm64eSignedIBWithCFA : NDArm64eSignedIAWithCFA;
    break;
  case AArch64RASigningKind::SPAndPC: {
    if (!State.SigningPC || *State.SigningPC < Range.Begin)
      return std::nullopt;
    const uint64_t Offset = *State.SigningPC - Range.Begin;
    if (Offset >= Range.size() || LoadedFrameStart > UINTPTR_MAX ||
        Offset > UINTPTR_MAX - LoadedFrameStart)
      return std::nullopt;
    Frame.SigningPC = LoadedFrameStart + Offset;
    if (State.Recipe == AArch64RARecipe::HintPACM)
      Frame.Signing =
          B ? NDArm64eSignedIBWithCFAPACM : NDArm64eSignedIAWithCFAPACM;
    else
      Frame.Signing = B ? NDArm64eSignedIBWithCFAPC : NDArm64eSignedIAWithCFAPC;
    break;
  }
  }
  return Plan;
}

AArch64AnalyzedUnwindSigning
analyzeAArch64UnwindSigning(const BinaryImage &Image,
                            const ExceptionFunction &Function, va_t PC,
                            const AArch64UnwindRecoveryLimits &Limits) {
  AArch64AnalyzedUnwindSigning Result{
      queryAArch64UnwindSigning(Image, Function, PC), {}};
  if (!implicitProfile(Image, Function, Result.Metadata))
    return Result;
  using Status = dwarf_eh::AArch64RAQueryStatus;
  if (PC % neverd::aarch64::kInsnSize) {
    Result.InstructionStateAtPC = {Status::Unsupported, {}, kUnaligned};
    return Result;
  }
  auto Rows = recoverImplicitFrame(Image, Function, Limits);
  if (Rows) {
    Result.InstructionStateAtPC =
        aarch64UnwindRowAt(*Rows, PC - Function.CodeRange.Begin).Signing;
  } else {
    llvm::handleAllErrors(Rows.takeError(), [&](const RecoveryError &E) {
      Result.InstructionStateAtPC = E.Result;
    });
  }
  return Result;
}

} // namespace neverd
