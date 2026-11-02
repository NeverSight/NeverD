#ifndef NEVERD_SDK_CAPI_OBJCSYNCHRONIZEDFRAMESOURCE_H
#define NEVERD_SDK_CAPI_OBJCSYNCHRONIZEDFRAMESOURCE_H

#include "ObjCUnwindSource.h"

#include "neverd/decode/Decoder.h"
#include "neverd/lift/AArch64/AArch64Regs.h"
#include "neverd/loader/BinaryImage.h"
#include "neverd/sdk/Limits.h"

#include "llvm/Support/Endian.h"

#include <capstone/arm64.h>
#include <optional>
#include <vector>

namespace neverd::sdk {

struct ObjCSynchronizedFrameProof {
  uint16_t Bytes = 0;
  int64_t ReceiverOffset = -40; // relative to the incoming SP
  va_t Store = 0;
  std::vector<va_t> ReceiverLoads;
};

// A fixed frame can hold self across calls without publishing its address.
// Reject every other use of SP/FP: copies, arithmetic, indexed memory,
// writeback, and stores of frame addresses. Subsequent frame stores may only
// write the outgoing-argument prefix below the receiver and saved registers.
inline std::optional<ObjCSynchronizedFrameProof>
proveObjCSynchronizedPrivateFrame(const BinaryImage &Image, va_t Entry,
                                  va_t NormalExit) {
  if (Image.Arch != Arch::AArch64 || Image.Bits != Bitness::Bits64 ||
      Entry > InvalidVA - 32 || Entry % 4 || NormalExit < Entry + 32 ||
      NormalExit - Entry >
          ::neverd::sdk::
              kObjCSynchronizedFrameSourceProveObjCSynchronizedPrivateFrameNormalExitEntryLimit ||
      NormalExit % 4)
    return std::nullopt;
  const auto Word = [&](va_t Address) -> std::optional<uint32_t> {
    const auto *Bytes = Image.readVA(Address, 4);
    return Bytes ? std::optional(llvm::support::endian::read32le(Bytes))
                 : std::nullopt;
  };
  const auto Allocation = Word(Entry);
  if (!Allocation || (*Allocation & ~0x003ffc00U) != 0xd10003ffU)
    return std::nullopt;
  const uint16_t Bytes = (*Allocation >> 10) & 0xfff;
  if (Bytes < 80 ||
      Bytes >
          ::neverd::sdk::
              kObjCSynchronizedFrameSourceProveObjCSynchronizedPrivateFrameBytesLimit ||
      Bytes % 16 ||
      Word(Entry + 4) != (0xa9004ff4U | ((Bytes - 32) / 8 << 15)) ||
      Word(Entry + 8) != (0xa9007bfdU | ((Bytes - 16) / 8 << 15)) ||
      Word(Entry + 12) != (0x910003fdU | ((Bytes - 16) << 10)) ||
      Word(Entry + 16) != 0xf81e83a0U) // stur x0, [fp, #-24]
    return std::nullopt;
  ObjCSynchronizedFrameProof Proof{Bytes, -40, Entry + 16, {}};
  Decoder Decoder;
  if (!Decoder.init(Arch::AArch64))
    return std::nullopt;
  for (va_t Address = Entry + 20; Address < NormalExit; Address += 4) {
    const auto *Bytes = Image.readVA(Address, 4);
    DecodedInsn Instruction{};
    if (!Bytes || Decoder.decodeOne(Bytes, 4, Address, Instruction) != 4 ||
        !Instruction.Raw || !Instruction.Raw->detail)
      return std::nullopt;
    if (Word(Address) == 0xaa1d03fdU) // mov fp, fp
      continue;
    const auto &Detail = *Instruction.Raw->detail;
    const auto &Operands = Detail.aarch64;
    bool FrameMemory = false;
    for (unsigned I = 0; I < Operands.op_count; ++I) {
      const auto &Operand = Operands.operands[I];
      if (Operand.type == AARCH64_OP_REG &&
          (Operand.reg == AARCH64_REG_SP || Operand.reg == AARCH64_REG_WSP ||
           Operand.reg == AARCH64_REG_X29 || Operand.reg == AARCH64_REG_W29))
        return std::nullopt;
      if (Operand.type != AARCH64_OP_MEM ||
          (Operand.mem.base != AARCH64_REG_SP &&
           Operand.mem.base != AARCH64_REG_X29))
        continue;
      if (FrameMemory || Detail.writeback ||
          Operand.mem.index != AARCH64_REG_INVALID ||
          I + 1 != Operands.op_count || (I != 1 && I != 2))
        return std::nullopt;
      const bool Store = Instruction.Id == ARM64_INS_STR ||
                         Instruction.Id == ARM64_INS_STUR ||
                         Instruction.Id == ARM64_INS_STP;
      const bool Load = Instruction.Id == ARM64_INS_LDR ||
                        Instruction.Id == ARM64_INS_LDUR ||
                        Instruction.Id == ARM64_INS_LDP;
      if ((!Store && !Load) || (I == 2) != (Instruction.Id == ARM64_INS_STP ||
                                            Instruction.Id == ARM64_INS_LDP))
        return std::nullopt;
      for (unsigned J = 0; J < I; ++J) {
        const auto &Register = Operands.operands[J];
        if (Register.type != AARCH64_OP_REG ||
            (Register.reg != AARCH64_REG_XZR &&
             (Register.reg < AARCH64_REG_X0 || Register.reg > AARCH64_REG_X28)))
          return std::nullopt;
      }
      const int64_t Offset =
          Operand.mem.disp +
          (Operand.mem.base == AARCH64_REG_SP ? 0 : Proof.Bytes - 16);
      if (Offset < 0 || Offset > Proof.Bytes - int64_t(8 * I) ||
          (Store && Offset + int64_t(8 * I) > Proof.Bytes - 40) ||
          (Load && (I != 1 || Offset != Proof.Bytes - 40)))
        return std::nullopt;
      if (Load && I == 1 && Offset == Proof.Bytes - 40)
        Proof.ReceiverLoads.push_back(Address);
      FrameMemory = true;
    }
    cs_regs Reads{}, Writes{};
    uint8_t ReadCount = 0, WriteCount = 0;
    if (cs_regs_access(Decoder.getHandle(), Instruction.Raw, Reads, &ReadCount,
                       Writes, &WriteCount) != CS_ERR_OK)
      return std::nullopt;
    for (unsigned I = 0; I < ReadCount; ++I)
      if (!FrameMemory &&
          (Reads[I] == AARCH64_REG_SP || Reads[I] == AARCH64_REG_WSP ||
           Reads[I] == AARCH64_REG_X29 || Reads[I] == AARCH64_REG_W29))
        return std::nullopt;
    for (unsigned I = 0; I < WriteCount; ++I)
      if (Writes[I] == AARCH64_REG_SP || Writes[I] == AARCH64_REG_WSP ||
          Writes[I] == AARCH64_REG_X29 || Writes[I] == AARCH64_REG_W29)
        return std::nullopt;
  }
  return Proof;
}

// Confirm the source operands still denote that private cell. Follow only
// unique scalar definitions and exact frame offsets; never replace a load
// from another cell, a call result, or an unknown value with the formal self.
inline bool projectObjCSynchronizedFrameReceiver(
    HighFunc &Function, const ObjCSynchronizedFrameProof &Proof, va_t Enter,
    va_t Exit, va_t EnterTarget, va_t ExitTarget, va_t Landing) {
  if (Function.Params.empty() || !Function.Params[0].Type ||
      Function.Params[0].Type->Kind != NdTypeKind::Ptr ||
      Function.Params[0].Type->Size != 8)
    return false;
  std::vector<HighStmt *> Statements;
  std::vector<std::pair<MedVar, HighStmt *>> Definitions;
  size_t Budget = kSourceValueWork;
  std::vector<HighStmt *> Pending;
  for (auto &S : Function.Body)
    if (S.Addr < Landing)
      Pending.push_back(&S);
  while (!Pending.empty()) {
    if (!Budget--)
      return false;
    auto *S = Pending.back();
    Pending.pop_back();
    Statements.push_back(S);
    if (S->Kind == StmtKind::Assign && S->Dst &&
        S->Dst->Kind == ExprKind::Var && S->Val) {
      for (const auto &[Variable, Statement] : Definitions)
        if (Variable == S->Dst->Var)
          return false;
      Definitions.emplace_back(S->Dst->Var, S);
    }
    for (auto &Child : S->Body)
      Pending.push_back(&Child);
    for (auto &Child : S->ElseBody)
      Pending.push_back(&Child);
    for (auto &Case : S->Cases)
      for (auto &Child : Case.Body)
        Pending.push_back(&Child);
    for (auto &Child : S->DefaultBody)
      Pending.push_back(&Child);
    for (auto &Clause : S->EHClauseBodies)
      for (auto &Child : Clause)
        Pending.push_back(&Child);
  }
  const auto Definition = [&](const MedVar &Variable) -> HighStmt * {
    for (const auto &[Candidate, Statement] : Definitions)
      if (objcUnwindSameScalarVariable(Candidate, Variable))
        return Statement;
    return nullptr;
  };
  const auto Scalar = [](const ExprPtr &E) {
    return E && E->Type && E->Type->Size == 8 &&
           (E->Type->Kind == NdTypeKind::Int ||
            E->Type->Kind == NdTypeKind::Ptr) &&
           E->IntrinsicId == Intrinsic::None && E->IntrinsicOutputs.empty() &&
           !E->SourceCallHint && E->MemoryOrdering == NdMemoryOrdering::None &&
           E->MemoryAddressSpace == NdMemoryAddressSpace::Default;
  };
  auto FrameOffset = [&](auto &&Self, const ExprPtr &E,
                         unsigned Depth) -> std::optional<int64_t> {
    if (!Budget ||
        Depth >
            ::neverd::sdk::
                kObjCSynchronizedFrameSourceProjectObjCSynchronizedFrameReceiverFrameOffsetDepthLimit ||
        !Scalar(E))
      return std::nullopt;
    --Budget;
    if (E->Kind == ExprKind::Var && E->Operands.empty()) {
      if (E->Var.Kind == MedVar::Reg && E->Var.TheArch == Arch::AArch64 &&
          E->Var.RegOff == a64reg::SP && E->Var.Size == 8 &&
          E->Var.SSAVer == 0 && E->Var.RenameTag == -1)
        return 0;
      const auto *D = Definition(E->Var);
      return D ? Self(Self, D->Val, Depth + 1) : std::nullopt;
    }
    if (E->Kind != ExprKind::BinOp || E->Operands.size() != 2 ||
        (E->Op != NdOp::INT_ADD && E->Op != NdOp::INT_SUB))
      return std::nullopt;
    auto A = E->Operands[0], B = E->Operands[1];
    if (E->Op == NdOp::INT_ADD && A && A->Kind == ExprKind::Const)
      std::swap(A, B);
    if (!B || B->Kind != ExprKind::Const || !B->Operands.empty() || !B->Type ||
        B->Type->Kind != NdTypeKind::Int || !B->Type->Size ||
        B->Type->Size > 8 || B->SourceCallHint ||
        B->IntrinsicId != Intrinsic::None || !B->IntrinsicOutputs.empty() ||
        B->MemoryOrdering != NdMemoryOrdering::None ||
        B->MemoryAddressSpace != NdMemoryAddressSpace::Default ||
        B->AddressOwnerVA != InvalidVA ||
        (B->ConstProvenance != ConstantAddressProvenance::Unknown &&
         B->ConstProvenance != ConstantAddressProvenance::Scalar))
      return std::nullopt;
    const auto Offset = Self(Self, A, Depth + 1);
    int64_t Delta = static_cast<int64_t>(B->ConstVal);
    if (!Offset || Delta < -8192 ||
        Delta >
            ::neverd::sdk::
                kObjCSynchronizedFrameSourceProjectObjCSynchronizedFrameReceiverFrameOffsetDeltaLimit)
      return std::nullopt;
    if (E->Op == NdOp::INT_SUB)
      Delta = -Delta;
    const int64_t Result = *Offset + Delta;
    return Result >= -8192 &&
                   Result <=
                       ::neverd::sdk::
                           kObjCSynchronizedFrameSourceProjectObjCSynchronizedFrameReceiverFrameOffsetResultLimit
               ? std::optional(Result)
               : std::nullopt;
  };
  std::optional<MedVar> Receiver;
  size_t Stores = 0;
  for (const auto *S : Statements) {
    if (S->Kind != StmtKind::Store)
      continue;
    const auto Offset = FrameOffset(FrameOffset, S->StoreAddr, 0);
    const auto Width =
        S->StoreVal && S->StoreVal->Type ? S->StoreVal->Type->Size : 0;
    const bool Overlaps = Offset && Width &&
                          *Offset < Proof.ReceiverOffset + 8 &&
                          Proof.ReceiverOffset < *Offset + Width;
    if (S->Addr != Proof.Store && !Overlaps)
      continue;
    size_t SliceBudget = kSourceSliceWork;
    const auto Value =
        objcUnwindScalarSlice(S->StoreVal, {}, false, SliceBudget);
    if (++Stores != 1 || S->Addr != Proof.Store || !Offset ||
        *Offset != Proof.ReceiverOffset || !Value || Value->Offset ||
        Value->Bytes != 8 || S->MemoryOrdering != NdMemoryOrdering::None ||
        S->MemoryAddressSpace != NdMemoryAddressSpace::Default)
      return false;
    Receiver = Value->Root;
  }
  if (!Receiver || Stores != 1)
    return false;
  auto IsReceiver = [&](auto &&Self, const ExprPtr &E, va_t Address,
                        unsigned Depth) -> bool {
    if (!Budget ||
        Depth >
            ::neverd::sdk::
                kObjCSynchronizedFrameSourceProjectObjCSynchronizedFrameReceiverIsReceiverDepthLimit ||
        !Scalar(E))
      return false;
    --Budget;
    if (E->Kind == ExprKind::Var && E->Operands.empty()) {
      const auto *D = Definition(E->Var);
      return D && Self(Self, D->Val, D->Addr, Depth + 1);
    }
    if (E->Kind != ExprKind::Load || E->Operands.size() != 1 ||
        std::find(Proof.ReceiverLoads.begin(), Proof.ReceiverLoads.end(),
                  Address) == Proof.ReceiverLoads.end())
      return false;
    const auto Offset = FrameOffset(FrameOffset, E->Operands[0], 0);
    return Offset && *Offset == Proof.ReceiverOffset;
  };
  std::vector<std::pair<HighStmt *, ExprPtr>> Edits;
  for (auto *S : Statements) {
    if (S->Addr != Enter && S->Addr != Exit)
      continue;
    const auto Call = objcUnwindStatementCall(*S);
    if (!Call || Call->Kind != ExprKind::Call || Call->IsIndirectCall ||
        Call->IndirectTarget || Call->Operands.size() != 1 ||
        Call->CallAddr != (S->Addr == Enter ? EnterTarget : ExitTarget) ||
        !IsReceiver(IsReceiver, Call->Operands[0], S->Addr - 4, 0))
      return false;
    for (const auto &[Prior, Copy] : Edits)
      if (Prior->Addr == S->Addr)
        return false;
    auto Copy = std::make_shared<HighExpr>(*Call);
    Copy->Operands = {HighExpr::makeVar(*Receiver, Function.Params[0].Type)};
    Edits.emplace_back(S, std::move(Copy));
  }
  if (Edits.size() != 2)
    return false;
  for (auto &[Statement, Call] : Edits)
    if (Statement->Kind == StmtKind::Call)
      Statement->CallExpr = std::move(Call);
    else
      Statement->Val = std::move(Call);
  return true;
}

} // namespace neverd::sdk

#endif
