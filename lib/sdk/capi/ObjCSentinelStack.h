#ifndef NEVERD_SDK_CAPI_OBJCSENTINELSTACK_H
#define NEVERD_SDK_CAPI_OBJCSENTINELSTACK_H

#include "../../ir/high/pass/HighFrameAddress.h"

#include <map>

namespace neverd::sdk {

/// Recover only the immutable object/null identity read by each concrete load
/// in a straight-line private-frame prefix. Identities belong to load time,
/// not to the later call or the final contents of the slot. The callbacks
/// validate source objects and the no-argument relocation queries separately.
template <typename ObjectIdentity, typename VerifiedQuery>
std::map<const HighExpr *, va_t>
sentinelPrivateStackLoads(const HighFunc &Function, const HighExpr &Call,
                          ObjectIdentity Identify, VerifiedQuery Query) {
  std::map<const HighExpr *, va_t> Loads;
  if (Function.FrameSize <= 0 || Function.StructuredExceptionRegions ||
      Function.UnstructuredExceptionRegions)
    return Loads;
  VarKeyMap<unsigned> DefinitionCounts;
  walkStmts(Function.Body, [&](const HighStmt &Statement) {
    if (Statement.Kind == StmtKind::Assign && Statement.Dst &&
        Statement.Dst->Kind == ExprKind::Var)
      ++DefinitionCounts[varKey(Statement.Dst->Var)];
  });
  size_t Budget = 10000;
  VarKeyMap<ExprPtr> FrameAliases;
  std::map<int64_t, va_t> Slots;
  unsigned Calls = 0;
  const auto Address = [&](const ExprPtr &Value,
                           unsigned Bytes) -> std::optional<int64_t> {
    auto Offset = high_detail::frameAddressOffset(
        Value, Function, Arch::AArch64, Budget, 0, &FrameAliases);
    if (!Offset || !Bytes || *Offset < -Function.FrameSize || *Offset >= 0 ||
        uint64_t(Bytes) > uint64_t(-*Offset))
      return std::nullopt;
    return Offset;
  };
  const auto Read = [&](auto &&Self, const ExprPtr &Value,
                        unsigned Depth) -> bool {
    if (!Value || !Budget || Depth > 64 ||
        Value->IntrinsicId != Intrinsic::None ||
        !Value->IntrinsicOutputs.empty() ||
        Value->MemoryOrdering != NdMemoryOrdering::None ||
        Value->MemoryAddressSpace != NdMemoryAddressSpace::Default)
      return false;
    --Budget;
    if (Value->Kind == ExprKind::Load) {
      if (!Value->Type || Value->Type->Size < 8 || Value->Type->Size > 16 ||
          Value->Operands.size() != 1)
        return false;
      // A compiler-declared external Objective-C object value is already a
      // complete object identity; only private-frame loads use the slot map.
      if (Identify(Value))
        return Self(Self, Value->Operands.front(), Depth + 1);
      const auto Offset = Address(Value->Operands[0], Value->Type->Size);
      if (!Offset)
        return false;
      const auto Slot = Slots.find(*Offset);
      if (Slot == Slots.end())
        return false;
      const auto [Found, Inserted] = Loads.emplace(Value.get(), Slot->second);
      return Inserted || Found->second == Slot->second;
    }
    if (Value->Kind == ExprKind::Var) {
      return Value->Var.Kind != MedVar::Stack &&
             !isSyntheticEntryStackPointer(Value->Var, Function,
                                           Arch::AArch64) &&
             !FrameAliases.count(varKey(Value->Var));
    }
    if (Value->Kind == ExprKind::Call) {
      if (Value.get() == &Call) {
        if (++Calls != 1)
          return false;
      } else if (!Value->Operands.empty() || !Query(*Value)) {
        // A call that cannot receive a private-frame address cannot mutate
        // that storage. It can still clobber outgoing argument words, so
        // discard every earlier slot identity and continue validating its
        // operands for an address escape. Stores after the call may establish
        // fresh identities for the target sentinel call.
        Slots.clear();
      }
    } else if (Value->Kind != ExprKind::Const &&
               Value->Kind != ExprKind::Cast &&
               Value->Kind != ExprKind::BitCast &&
               Value->Kind != ExprKind::BinOp &&
               Value->Kind != ExprKind::UnaryOp) {
      return false;
    }
    for (const auto &Operand : Value->Operands)
      if (!Self(Self, Operand, Depth + 1))
        return false;
    return true;
  };
  for (const auto &Statement : Function.Body) {
    if (!Budget || !Statement.Body.empty() || !Statement.ElseBody.empty() ||
        !Statement.Cases.empty() || !Statement.DefaultBody.empty() ||
        !Statement.EHClauseBodies.empty() ||
        Statement.MemoryOrdering != NdMemoryOrdering::None ||
        Statement.MemoryAddressSpace != NdMemoryAddressSpace::Default)
      return {};
    --Budget;
    if (Statement.Kind == StmtKind::Store) {
      if (!Statement.StoreVal || !Statement.StoreVal->Type)
        return {};
      const auto Bytes = Statement.StoreVal->Type->Size;
      const auto Offset = Address(Statement.StoreAddr, Bytes);
      if (!Offset || !Read(Read, Statement.StoreVal, 0))
        return {};
      for (auto It = Slots.begin(); It != Slots.end();)
        if (It->first < *Offset + Bytes && *Offset < It->first + 8)
          It = Slots.erase(It);
        else
          ++It;
      if (Bytes >= 8 && Bytes <= 16)
        if (const auto Object = Identify(Statement.StoreVal))
          Slots.emplace(*Offset, *Object);
    } else if (Statement.Kind == StmtKind::Assign) {
      if (!Statement.Dst || Statement.Dst->Kind != ExprKind::Var ||
          (Statement.Dst->Var.Kind != MedVar::Reg &&
           Statement.Dst->Var.Kind != MedVar::Temp) ||
          !Statement.Val ||
          isSyntheticEntryStackPointer(Statement.Dst->Var, Function,
                                       Arch::AArch64))
        return {};
      const auto Key = varKey(Statement.Dst->Var);
      if (high_detail::frameAddressOffset(Statement.Val, Function,
                                          Arch::AArch64, Budget, 0,
                                          &FrameAliases)) {
        if (DefinitionCounts[Key] != 1 || !Statement.Dst->Type ||
            Statement.Dst->Type->Size != 8 || Statement.Dst->Var.Size != 8)
          return {};
        FrameAliases.emplace(Key, Statement.Val);
      } else if (!Read(Read, Statement.Val, 0)) {
        return {};
      }
    } else if (Statement.Kind == StmtKind::ExprStmt) {
      if (!Read(Read, Statement.Val, 0))
        return {};
    } else if (Statement.Kind == StmtKind::Call) {
      if (!Read(Read, Statement.CallExpr, 0))
        return {};
    } else if (Statement.Kind == StmtKind::Return) {
      if (!Read(Read, Statement.RetVal, 0))
        return {};
    } else {
      return {};
    }
    if (Calls == 1)
      return Loads;
  }
  return {};
}
} // namespace neverd::sdk
#endif
