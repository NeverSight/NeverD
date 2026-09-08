//===- MedToHigh.cpp - MedIR to HighIR conversion ------------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// MedIR to HighIR conversion: expression tree construction, use-def
/// chain building, and the top-level convert() pipeline.
///
/// Related files:
///   HighExpr.cpp            — HighExpr factory methods and intrinsic helpers
///   MedOpToExpr.cpp         — medOpToExpr (NdOp → expression tree mapping)
///   HighTypeInference.cpp   — type inference and return-size deduction
///   HighIRPrint.cpp         — display methods (HighExpr::str, HighStmt::str)
///
//===----------------------------------------------------------------------===//

#include "neverd/ir/high/MedToHigh.h"

#include "neverd/Limits.h"
#include "neverd/ir/TargetRegInfo.h"
#include "neverd/support/Diagnostic.h"

#include "llvm/ADT/StringExtras.h"
#include "llvm/Support/Debug.h"
#include "llvm/Support/raw_ostream.h"

#include <algorithm>
#include <chrono>
#include <set>
#include <unordered_set>

#define DEBUG_TYPE "neverd-med-to-high"

namespace neverd {

namespace {

bool isEntryLiveInValue(const MedFunc &Func, const MedVar &V, uint64_t RegOff) {
  if (Func.Blocks.empty())
    return false;
  for (const MedOp &Op : Func.Blocks.front().Ops) {
    if (Op.Opcode != NdOp::COPY)
      break;
    if (Op.Output.Kind == MedVar::Reg && Op.Output.RegOff == RegOff &&
        Op.NumInputs >= 1 && Op.Inputs[0].Kind == MedVar::Reg &&
        Op.Inputs[0].Id == Op.Output.Id &&
        Op.Inputs[0].SSAVer == Op.Output.SSAVer && Op.Output == V)
      return true;
  }
  return false;
}

const MedCallClobber *findCallClobber(const MedFunc &Func, const MedVar &V) {
  auto It = std::find_if(
      Func.CallClobbers.begin(), Func.CallClobbers.end(),
      [&](const MedCallClobber &Clobber) { return Clobber.Value == V; });
  return It == Func.CallClobbers.end() ? nullptr : &*It;
}

} // anonymous namespace

//===----------------------------------------------------------------------===//
// MedToHighConverter — expression helpers
//===----------------------------------------------------------------------===//

ExprPtr MedToHighConverter::inlineableDefinition(VarKey Key) const {
  if (PhiOutputVars.count(Key) || MemoryReadOutputs.count(Key))
    return nullptr;
  auto It = DefExpr.find(Key);
  if (It == DefExpr.end() || !It->second ||
      It->second->Kind == ExprKind::Call ||
      It->second->MemoryOrdering != NdMemoryOrdering::None ||
      It->second->MemoryAddressSpace != NdMemoryAddressSpace::Default)
    return nullptr;
  return It->second;
}

ExprPtr MedToHighConverter::medvarToExpr(const MedVar &V) {
  if (V.isConst()) {
    return HighExpr::makeConst(V.ConstVal, V.Size);
  }

  auto SourceParameter = [&](MedVar Parameter, size_t Index) -> ExprPtr {
    const auto Type = CurMed->TypedParams[Index].Type;
    Parameter.Size = Type->Size;
    if (CurMed->SourceTypeHint->HasExplicitABI &&
        Index < CurMed->SourceTypeHint->Parameters.size()) {
      const auto &Location = CurMed->SourceTypeHint->Parameters[Index].Location;
      // MedIR uses RegOff == kNoParamReg to distinguish logical stack
      // arguments during register recovery. RegOff and StackOff share storage;
      // only the source-level expression carries the checked entry-SP offset.
      if (Location.Kind == SourceABICarrierKind::Stack)
        Parameter.StackOff = Location.EntryStackOffset;
    }
    auto Value = HighExpr::makeVar(Parameter, Type);
    if (Type->Kind == NdTypeKind::Float)
      return HighExpr::makeBitCast(Value, NdType::makeInt(Type->Size, false));
    return Value;
  };
  if (CurMed && CurMed->SourceTypeHint && V.Kind == MedVar::Param &&
      V.Id >= 0 && static_cast<size_t>(V.Id) < CurMed->TypedParams.size())
    return SourceParameter(V, static_cast<size_t>(V.Id));

  if (CurMed && V.Kind == MedVar::Reg) {
    for (size_t I = 0; I < CurMed->Params.size(); ++I) {
      const MedVar &P = CurMed->Params[I];
      if (P.RegOff == kNoParamReg || P.Id < 0 || P.RegOff != V.RegOff ||
          !isEntryLiveInValue(*CurMed, V, P.RegOff))
        continue;
      MedVar Param = V;
      Param.Kind = MedVar::Param;
      Param.Id = static_cast<int>(I);
      TypeRef Type;
      if (CurMed->SourceTypeHint && I < CurMed->TypedParams.size())
        return SourceParameter(Param, I);
      return HighExpr::makeVar(Param, Type);
    }
  }

  if (CurMed) {
    if (const MedCallClobber *Clobber = findCallClobber(*CurMed, V)) {
      if (Clobber->PreservedPrefixSize == 0)
        return HighExpr::makeUndef(V.Size);

      auto Low = HighExpr::makeBinop(
          NdOp::SUBBYTES, medvarToExpr(Clobber->PreservedInput),
          HighExpr::makeConst(0, Clobber->PreservedInput.Size));
      Low->Type = NdType::makeInt(Clobber->PreservedPrefixSize, false);
      auto Result = HighExpr::makeUnary(NdOp::INT_ZEXT, Low);
      Result->Type = NdType::makeInt(V.Size, false);
      return Result;
    }
  }

  auto Key = varKey(V);

  auto UIt = UseCount.find(Key);
  if (UIt != UseCount.end() && UIt->second == 1)
    if (auto Definition = inlineableDefinition(Key))
      return Definition;

  return HighExpr::makeVar(V);
}

ExprPtr MedToHighConverter::sourceBitSlice(const ExprPtr &Value,
                                           uint64_t ByteOffset, uint16_t Bytes,
                                           unsigned Depth) {
  if (!Value || !Bytes || Depth > 40)
    return HighExpr::makeUndef(Bytes);
  if (Value->Kind == ExprKind::Var && Value->Var.Kind != MedVar::Param)
    if (auto Definition = inlineableDefinition(varKey(Value->Var)))
      if (Definition.get() != Value.get())
        return sourceBitSlice(Definition, ByteOffset, Bytes, Depth + 1);
  // A source scalar describes only the low lane of its SIMD carrier. Reading
  // an upper lane cannot silently manufacture zeroes or adjacent parameters.
  if (!Value->Type || ByteOffset > Value->Type->Size ||
      Bytes > Value->Type->Size - ByteOffset)
    return HighExpr::makeUndef(Bytes);
  if (Value->Kind == ExprKind::BinOp && Value->Operands.size() >= 2) {
    if (Value->Op == NdOp::CONCAT && Value->Operands[1] &&
        Value->Operands[1]->Type) {
      const auto LowBytes = Value->Operands[1]->Type->Size;
      if (ByteOffset + Bytes <= LowBytes)
        return sourceBitSlice(Value->Operands[1], ByteOffset, Bytes, Depth + 1);
      if (ByteOffset >= LowBytes)
        return sourceBitSlice(Value->Operands[0], ByteOffset - LowBytes, Bytes,
                              Depth + 1);
    }
    if (Value->Op == NdOp::SUBBYTES && Value->Operands[1] &&
        Value->Operands[1]->Kind == ExprKind::Const &&
        Value->Operands[1]->ConstVal <= 16)
      return sourceBitSlice(Value->Operands[0],
                            ByteOffset + Value->Operands[1]->ConstVal, Bytes,
                            Depth + 1);
  }
  if (Value->Kind == ExprKind::UnaryOp &&
      (Value->Op == NdOp::INT_ZEXT || Value->Op == NdOp::INT_SEXT) &&
      Value->Operands.size() == 1 && Value->Operands[0] &&
      Value->Operands[0]->Type &&
      ByteOffset + Bytes <= Value->Operands[0]->Type->Size)
    return sourceBitSlice(Value->Operands[0], ByteOffset, Bytes, Depth + 1);
  if (ByteOffset == 0 && Value->Type->Size == Bytes)
    return Value;
  auto Slice = HighExpr::makeBinop(NdOp::SUBBYTES, Value,
                                   HighExpr::makeConst(ByteOffset, 4));
  Slice->Type = NdType::makeInt(Bytes, false);
  return Slice;
}

ExprPtr MedToHighConverter::sourceFloatValue(const MedVar &Value,
                                             uint16_t Bytes) {
  auto Bits = sourceBitSlice(medvarToExpr(Value), 0, Bytes);
  if (Bits->Type && Bits->Type->Kind == NdTypeKind::Float)
    return Bits;
  return HighExpr::makeBitCast(Bits, NdType::makeFloat(Bytes));
}

ExprPtr MedToHighConverter::forceInlineExpr(const ExprPtr &E) {
  struct DepthGuard {
    int &D;
    int &N;
    DepthGuard(int &Depth, int &Nodes) : D(Depth), N(Nodes) {
      if (D == 0)
        N = 0;
      ++D;
    }
    ~DepthGuard() { --D; }
  };
  static thread_local int Depth = 0;
  static thread_local int NodeCount = 0;
  static constexpr int kMaxNodes = limits::kMaxSSANodes;

  DepthGuard Guard(Depth, NodeCount);
  if (!E || Depth > 30 || NodeCount > kMaxNodes)
    return E;
  ++NodeCount;

  if (E->Kind == ExprKind::Var && E->Var.Id >= 0 && !E->Var.isConst()) {
    auto Key = varKey(E->Var);
    if (auto Definition = inlineableDefinition(Key))
      if (Definition.get() != E.get())
        return forceInlineExpr(Definition);
  }
  auto Result = std::make_shared<HighExpr>(*E);
  for (size_t I = 0; I < Result->Operands.size(); ++I)
    Result->Operands[I] = forceInlineExpr(Result->Operands[I]);
  return Result;
}

//===----------------------------------------------------------------------===//
// buildExpressions
//===----------------------------------------------------------------------===//

void MedToHighConverter::buildExpressions(const MedFunc &Med) {
  UseCount.clear();
  DefExpr.clear();
  CallOutputs.clear();
  PhiOutputVars.clear();
  MemoryReadOutputs.clear();
  NextHighTempId = 0;
  auto ReserveIdentity = [&](const MedVar &Value) {
    if (Value.Id >= NextHighTempId && Value.Id < INT_MAX)
      NextHighTempId = Value.Id + 1;
  };
  for (const auto &Block : Med.Blocks) {
    for (const auto &Operation : Block.Ops) {
      ReserveIdentity(Operation.Output);
      for (unsigned I = 0; I < Operation.NumInputs; ++I)
        ReserveIdentity(Operation.Inputs[I]);
    }
    for (const auto &Phi : Block.Phis) {
      ReserveIdentity(Phi.Output);
      for (const auto &[Pred, Value] : Phi.Args)
        ReserveIdentity(Value);
    }
  }

  for (const MedCallClobber &Clobber : Med.CallClobbers)
    if (Clobber.PreservedPrefixSize > 0 && Clobber.PreservedInput.Id >= 0)
      UseCount[varKey(Clobber.PreservedInput)]++;

  for (auto &Blk : Med.Blocks) {
    for (auto &Op : Blk.Ops) {
      if (Op.Opcode == NdOp::LOAD && Op.Output.Id >= 0 && Op.Output.Size > 0)
        MemoryReadOutputs.insert(varKey(Op.Output));
      for (uint8_t I = 0; I < Op.NumInputs; ++I)
        if (Op.Inputs[I].Id >= 0)
          UseCount[varKey(Op.Inputs[I])]++;
    }
    for (auto &Phi : Blk.Phis) {
      for (auto &[PredId, Arg] : Phi.Args)
        if (Arg.Id >= 0)
          UseCount[varKey(Arg)]++;
      if (Phi.Output.Id >= 0 && Phi.Output.Size > 0)
        PhiOutputVars.insert(varKey(Phi.Output));
    }
  }

  for (auto &Blk : Med.Blocks) {
    for (auto &Op : Blk.Ops) {
      if (Op.Output.Id >= 0 && Op.Output.Size > 0) {
        auto Key = varKey(Op.Output);
        DefExpr[Key] = medOpToExpr(Op);
        if (Op.Opcode == NdOp::CALL || Op.Opcode == NdOp::INDIR_CALL ||
            Op.Opcode == NdOp::INTRINSIC)
          CallOutputs.insert(Key);
      }
    }
    for (auto &Phi : Blk.Phis) {
      if (Phi.Output.Id < 0 || Phi.Output.Size == 0)
        continue;
      auto Key = varKey(Phi.Output);
      if (DefExpr.count(Key))
        continue;
      int BestPred = INT_MAX;
      const MedVar *BestArg = nullptr;
      for (auto &[PredId, Arg] : Phi.Args) {
        if (PredId < BestPred) {
          BestPred = PredId;
          BestArg = &Arg;
        }
      }
      if (BestArg)
        DefExpr[Key] = medvarToExpr(*BestArg);
    }
  }
}

//===----------------------------------------------------------------------===//
// convert — top-level MedIR to HighIR pipeline
//===----------------------------------------------------------------------===//

HighFunc MedToHighConverter::convert(const MedFunc &Med, Arch TheArch) {
  auto TStart = std::chrono::steady_clock::now();
  TargetArch = TheArch;
  CurMed = &Med;
  HighFunc Func;
  Func.Entry = Med.Entry;
  Func.FrameSize = Med.FrameSize;
  Func.FrameHeadroom = Med.FrameHeadroom;
  Func.Name = Med.Name;
  Func.DoesNotReturn = Med.DoesNotReturn;
  Func.ExceptionMetadata = Med.ExceptionMetadata;
  Func.ReturnType =
      Med.ReturnType ? Med.ReturnType : NdType::makeInt(inferReturnSize(Med));
  Func.SourceTypeHint = Med.SourceTypeHint;

  for (auto &ML : Med.Locals) {
    HighLocal HL;
    HL.Name = ML.display();
    HL.StackOff = ML.StackOff;
    HL.Type = NdType::makeInt(ML.Size);
    Func.Locals.push_back(HL);
  }

  auto PtrParamRegOffs = detectPtrParamRegs(Med);
  const auto &TRI = getTargetRegInfo(TheArch);

  for (size_t PI = 0; PI < Med.Params.size(); ++PI) {
    auto &MP = Med.Params[PI];
    HighParam HP;
    HP.Name = "arg" + std::to_string(PI);
    if (Med.SourceTypeHint && PI < Med.TypedParams.size()) {
      HP.Name = Med.TypedParams[PI].Name;
      HP.Type = Med.TypedParams[PI].Type;
    } else if (PtrParamRegOffs.count(MP.RegOff) &&
               !TRI.isFrameOrLinkReg(MP.RegOff))
      HP.Type = NdType::makePtr();
    else
      HP.Type = NdType::makeInt(MP.Size);
    Func.Params.push_back(HP);
  }

  auto TExpr = std::chrono::steady_clock::now();
  buildExpressions(Med);
  auto TStruct = std::chrono::steady_clock::now();
  structureControlFlow(Func, Med);
  auto TSimp = std::chrono::steady_clock::now();
  simplifyControlFlow(Func, Med);
  auto TTypes = std::chrono::steady_clock::now();
  inferTypes(Func);
  auto TPost = std::chrono::steady_clock::now();

  stripStackCanary(Func);

  Func.Body.erase(std::remove_if(Func.Body.begin(), Func.Body.end(),
                                 [](const HighStmt &S) {
                                   if (S.Kind != StmtKind::Assign || !S.Dst ||
                                       !S.Val)
                                     return false;
                                   if (S.Dst->Kind == ExprKind::Var &&
                                       S.Val->Kind == ExprKind::Var)
                                     return S.Dst->Var == S.Val->Var;
                                   return false;
                                 }),
                  Func.Body.end());

  stripPrologueEpilogue(Func);
  ensureTrailingReturn(Func, Med);

  auto TDceStart = std::chrono::steady_clock::now();
  eliminateDeadStmts(Func);
  structureExceptionRegions(Func, Med);
  auto TEnd = std::chrono::steady_clock::now();

  {
    auto TotalMs =
        std::chrono::duration_cast<std::chrono::milliseconds>(TEnd - TStart)
            .count();
    if (TotalMs > 1000) {
      auto ElapsedMs = [](auto Start, auto End) {
        return std::chrono::duration_cast<std::chrono::milliseconds>(End -
                                                                     Start)
            .count();
      };
      syncWarning() << "m2h: " << Med.Name << " took " << TotalMs << "ms ("
                    << Med.Blocks.size() << " blocks, " << Func.Body.size()
                    << " stmts) [expr=" << ElapsedMs(TExpr, TStruct)
                    << "ms struct=" << ElapsedMs(TStruct, TSimp)
                    << "ms simp=" << ElapsedMs(TSimp, TTypes)
                    << "ms types=" << ElapsedMs(TTypes, TPost)
                    << "ms post=" << ElapsedMs(TPost, TDceStart)
                    << "ms dce=" << ElapsedMs(TDceStart, TEnd) << "ms]\n";
    }
  }

  LLVM_DEBUG(llvm::dbgs() << "MedIR -> HighIR: " << Func.Body.size()
                          << " statements for " << Func.Name << "\n");
  return Func;
}

} // namespace neverd
