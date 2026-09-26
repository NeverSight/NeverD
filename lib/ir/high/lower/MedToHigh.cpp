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
#include "neverd/ir/SourceABI.h"
#include "neverd/ir/TargetRegInfo.h"
#include "neverd/ir/high/HighSourceFlow.h"
#include "neverd/loader/BinaryImage.h"
#include "neverd/support/Diagnostic.h"

#include "llvm/ADT/ScopeExit.h"
#include "llvm/ADT/SmallString.h"
#include "llvm/ADT/StringExtras.h"
#include "llvm/Support/Debug.h"
#include "llvm/Support/raw_ostream.h"

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <cstdlib>
#include <set>
#include <string_view>
#include <unordered_set>

#define DEBUG_TYPE "neverd-med-to-high"

namespace neverd {

void coalesceBranchEntryStatements(HighFunc &Func);

namespace {

// Temporary, opt-in observation of the actual source-recovery conversion.
// Keep this independent of the analysis state and remove it with the CI probe.
class HighConversionTrace {
  const MedFunc &Med;
  Arch Architecture;
  unsigned Invocation = 0;
  bool Enabled = false;
  static constexpr size_t MaxBytes = 98304;
  static constexpr unsigned MaxRecords = 4096;

  static void variable(llvm::raw_ostream &OS, const MedVar &V) {
    OS << static_cast<unsigned>(V.Kind) << ':' << V.Id << ':' << V.SSAVer << ':'
       << V.Size;
    if (V.Kind == MedVar::Const)
      OS << ":constant=" << V.ConstVal;
    else if (V.Kind == MedVar::Reg)
      OS << ":register=" << V.RegOff;
    else if (V.Kind == MedVar::Stack)
      OS << ":stack=" << V.StackOff;
  }

  static void expression(llvm::raw_ostream &OS, const char *Name,
                         const ExprPtr &E) {
    if (!E)
      return;
    OS << ' ' << Name << "={kind=" << static_cast<unsigned>(E->Kind)
       << " op=" << ndOpName(E->Op);
    if (E->Kind == ExprKind::Var) {
      OS << " var=";
      variable(OS, E->Var);
    } else if (E->Kind == ExprKind::Const) {
      OS << " constant=" << E->ConstVal;
    }
    OS << " size=" << (E->Type ? E->Type->Size : 0) << '}';
  }

  template <typename F> void record(const char *Phase, F &&Build) noexcept {
    if (!Enabled)
      return;
    const int SavedErrno = errno;
    try {
      llvm::SmallString<4096> Buffer;
      llvm::raw_svector_ostream OS(Buffer);
      OS << "[neverd-high-trace] begin invocation=" << Invocation
         << " phase=" << Phase
         << " arch=" << static_cast<unsigned>(Architecture) << " entry=0x"
         << llvm::utohexstr(Med.Entry) << " function=";
      OS.write_escaped(Med.Name);
      OS << '\n';
      unsigned Records = 0;
      bool Truncated = false;
      auto More = [&]() {
        if (Truncated || Records >= MaxRecords || Buffer.size() >= MaxBytes) {
          Truncated = true;
          return false;
        }
        ++Records;
        return true;
      };
      Build(OS, More, Truncated);
      if (Buffer.size() > MaxBytes) {
        Buffer.resize(MaxBytes);
        Truncated = true;
      }
      OS << "\n[neverd-high-trace] end invocation=" << Invocation
         << " phase=" << Phase << " records=" << Records
         << " truncated=" << (Truncated ? 1 : 0) << '\n';
      std::lock_guard<std::mutex> Lock(diagnosticMutex());
      llvm::raw_fd_ostream Sink(2, false, true);
      llvm::scope_exit ClearError([&Sink]() noexcept { Sink.clear_error(); });
      Sink.write(Buffer.data(), Buffer.size());
      Sink.flush();
    } catch (...) {
      // Failed diagnostics must not replace conversion results or errors.
    }
    errno = SavedErrno;
  }

public:
  HighConversionTrace(const MedFunc &Med, Arch Architecture) noexcept
      : Med(Med), Architecture(Architecture) {
    const int SavedErrno = errno;
    const char *Selected = std::getenv("NEVERD_HIGH_TRACE_FUNCTION");
    if (Selected && !Med.Name.empty() && Med.Name.size() <= 256 &&
        std::string_view(Selected) == std::string_view(Med.Name)) {
      static std::atomic<unsigned> Count{0};
      Invocation = Count.fetch_add(1, std::memory_order_relaxed);
      Enabled = Invocation < 1;
    }
    errno = SavedErrno;
  }

  void med() noexcept {
    record("med-input", [&](auto &OS, auto &More, bool &Truncated) {
      for (const auto &Block : Med.Blocks) {
        if (!More())
          return;
        OS << "block id=" << Block.Id << " start=0x"
           << llvm::utohexstr(Block.StartAddr) << " end=0x"
           << llvm::utohexstr(Block.EndAddr) << '\n';
        for (int Pred : Block.Preds) {
          if (!More())
            return;
          OS << " pred=" << Pred << '\n';
        }
        for (int Succ : Block.Succs) {
          if (!More())
            return;
          OS << " succ=" << Succ << '\n';
        }
        for (const auto &Edge : Block.ExceptionalPreds) {
          if (!More())
            return;
          OS << " exceptional-pred=" << Edge.BlockId << " target=0x"
             << llvm::utohexstr(Edge.TargetVA) << '\n';
        }
        for (const auto &Edge : Block.ExceptionalSuccs) {
          if (!More())
            return;
          OS << " exceptional-succ=" << Edge.BlockId << " target=0x"
             << llvm::utohexstr(Edge.TargetVA) << '\n';
        }
        for (const auto &Phi : Block.Phis) {
          if (!More())
            return;
          OS << " phi=";
          variable(OS, Phi.Output);
          OS << '\n';
          for (const auto &[Pred, Value] : Phi.Args) {
            if (!More())
              return;
            OS << "  incoming=" << Pred << " value=";
            variable(OS, Value);
            OS << '\n';
          }
        }
        for (const auto &Op : Block.Ops) {
          if (!More())
            return;
          OS << " op address=0x" << llvm::utohexstr(Op.Addr)
             << " code=" << ndOpName(Op.Opcode) << " output=";
          variable(OS, Op.Output);
          for (size_t I = 0; I < Op.NumInputs && I < Op.Inputs.size(); ++I) {
            OS << " input=";
            variable(OS, Op.Inputs[I]);
          }
          OS << '\n';
          if (Op.NumInputs > Op.Inputs.size()) {
            OS << " invalid-input-count=" << static_cast<unsigned>(Op.NumInputs)
               << " available-inputs=" << Op.Inputs.size() << '\n';
            Truncated = true;
            return;
          }
        }
      }
    });
  }

  void high(const HighFunc &Func, const char *Phase) noexcept {
    record(Phase, [&](auto &OS, auto &More, bool &Truncated) {
      OS << "return-size=" << (Func.ReturnType ? Func.ReturnType->Size : 0)
         << '\n';
      unsigned Next = 0;
      auto Walk = [&](auto &&Self, const std::vector<HighStmt> &Body,
                      int Parent, const char *Edge, unsigned Arm,
                      unsigned Depth) -> void {
        if (Body.empty() || Truncated)
          return;
        if (Depth > 32) {
          Truncated = true;
          return;
        }
        for (size_t I = 0; I < Body.size(); ++I) {
          if (!More())
            return;
          const auto &S = Body[I];
          const unsigned Id = Next++;
          OS << "node id=" << Id << " parent=" << Parent << " edge=" << Edge
             << " arm=" << Arm << " index=" << I
             << " kind=" << static_cast<unsigned>(S.Kind) << " address=0x"
             << llvm::utohexstr(S.Addr) << " target=0x"
             << llvm::utohexstr(S.GotoTarget) << " loop-header=0x"
             << llvm::utohexstr(S.LoopHeaderAddr) << " phi=" << S.IsPhiCopy;
          expression(OS, "destination", S.Dst);
          expression(OS, "value", S.Val);
          expression(OS, "condition", S.Cond);
          expression(OS, "return", S.RetVal);
          OS << '\n';
          Self(Self, S.Body, Id, "body", 0, Depth + 1);
          Self(Self, S.ElseBody, Id, "else", 0, Depth + 1);
          for (size_t C = 0; C < S.Cases.size() && !Truncated; ++C)
            Self(Self, S.Cases[C].Body, Id, "case", C, Depth + 1);
          Self(Self, S.DefaultBody, Id, "default", 0, Depth + 1);
          for (size_t C = 0; C < S.EHClauseBodies.size() && !Truncated; ++C)
            Self(Self, S.EHClauseBodies[C], Id, "exception", C, Depth + 1);
        }
      };
      Walk(Walk, Func.Body, -1, "root", 0, 0);
    });
  }
};

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

void fillUnstructuredGotoSkeleton(HighFunc &Func, const MedFunc &Med) {
  Func.Body.clear();
  for (const auto &Block : Med.Blocks) {
    HighStmt Mark;
    Mark.Kind = StmtKind::Nop;
    Mark.Addr = Block.StartAddr;
    Func.Body.push_back(Mark);
    if (!Block.Succs.empty() && Block.Succs[0] >= 0 &&
        static_cast<size_t>(Block.Succs[0]) < Med.Blocks.size()) {
      HighStmt Jump;
      Jump.Kind = StmtKind::Goto;
      const size_t Succ = static_cast<size_t>(Block.Succs[0]);
      Jump.GotoTarget = Med.Blocks[Succ].StartAddr;
      Func.Body.push_back(Jump);
    } else {
      HighStmt Ret;
      Ret.Kind = StmtKind::Return;
      Func.Body.push_back(Ret);
    }
  }
}

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

TypeRef MedToHighConverter::sourceCallResultType(const MedOp &Op) const {
  if (Op.SourceCallHint) {
    const auto &Signature = Op.SourceCallHint->Signature;
    std::string Error;
    if (Signature.ReturnType &&
        Signature.ReturnType->Kind == NdTypeKind::Struct &&
        Signature.ReturnType->Size == Op.Output.Size &&
        validateSourceABI(Signature, Error))
      return Signature.ReturnType;
  }
  return NdType::makeInt(Op.Output.Size, false);
}

ExprPtr MedToHighConverter::medvarToExpr(const MedVar &V) {
  if (V.isConst()) {
    return HighExpr::makeConst(V.ConstVal, V.Size, V.Provenance,
                               V.AddressOwnerVA);
  }

  auto SourceParameter = [&](MedVar Parameter, size_t Index) -> ExprPtr {
    const auto &Bindings = SourceParameters;
    if (Index >= Bindings.size())
      return HighExpr::makeUndef(V.Size);
    const auto &Binding = Bindings[Index];
    const auto &Declared =
        CurMed->SourceTypeHint->Parameters[Binding.ParameterIndex];
    Parameter.Id = static_cast<int>(Binding.ParameterIndex);
    Parameter.Size = Declared.Type->Size;
    if (!Declared.Components.empty())
      Parameter.RegOff = 0; // Logical record, not any one of its carriers.
    else if (Binding.Location.Kind == SourceABICarrierKind::Stack)
      Parameter.StackOff = Binding.Location.EntryStackOffset;
    auto Value = HighExpr::makeVar(Parameter, Declared.Type);
    if (!Declared.Components.empty())
      Value = HighExpr::makeRecordField(Value, Binding.ByteOffset,
                                        Binding.Type->Size);
    if (Binding.Type->Kind == NdTypeKind::Float)
      Value = HighExpr::makeBitCast(Value,
                                    NdType::makeInt(Binding.Type->Size, false));
    if (Binding.Location.ExtendTo32Bits && V.Size > Binding.Type->Size) {
      // Keep the logical parameter type while exposing only the carrier bytes
      // established by its source ABI. Wider machine reads still have an
      // unknown suffix, which sourceBitSlice preserves through saved copies.
      Value = HighExpr::makeUnary(
          Binding.Type->IsSigned ? NdOp::INT_SEXT : NdOp::INT_ZEXT, Value);
      Value->Type = NdType::makeInt(std::min<uint16_t>(4, V.Size), false);
    }
    // A physical read keeps its requested width, including unknown bytes
    // outside the declared ABI carrier. Preserve those bytes before COPY,
    // PHI, or STORE lowering can turn a narrow expression into a C conversion.
    return sourceBitSlice(Value, 0, V.Size);
  };
  // Typed MedIR parameter IDs index physical source-ABI bindings, including
  // context registers and record leaves. Ordinary argument-register numbering
  // must not reinterpret these IDs.
  if (CurMed && CurMed->SourceTypeHint && V.Kind == MedVar::Param &&
      V.Id >= 0 && static_cast<size_t>(V.Id) < CurMed->TypedParams.size())
    return SourceParameter(V, static_cast<size_t>(V.Id));
  if (CurMed && V.Kind == MedVar::Param) {
    const int Slot = abiParamIndex(V);
    if (Slot >= 0) {
      MedVar Param = V;
      Param.Kind = MedVar::Param;
      Param.Id = Slot;
      if (CurMed->SourceTypeHint &&
          static_cast<size_t>(Slot) < CurMed->TypedParams.size())
        return SourceParameter(Param, static_cast<size_t>(Slot));
      TypeRef Type;
      if (static_cast<size_t>(Slot) < CurMed->Params.size())
        Param.RegOff = CurMed->Params[static_cast<size_t>(Slot)].RegOff;
      return HighExpr::makeVar(Param, Type);
    }
  }

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

  if (auto It = SourceRecordValues.find(varKey(V));
      It != SourceRecordValues.end() && V.Size == It->second->Size)
    return HighExpr::makeVar(V, It->second);

  // Win64: a callee-save that still holds the entry copy of rcx/rdx/r8/r9
  // is that parameter. `mov r14, r8` / `mov r8, r14` around a later call
  // must not keep the clobbered or reused argument register. MedIR may bump
  // the SSA version of rdi without a new def (`COPY r9.2 = rdi.2`); match the
  // register as well as the exact SSA pair.
  // A PHI of this register is a join, not the entry parameter copy.  MSVC
  // `__GSHandlerCheckCommon` saves rcx in r10, then overwrites r10 on the
  // GS_HANDLER_DATA bit-2 align edge; mapping every later r10 SSA to arg0
  // deletes that edge.
  if (CurMed && V.Kind == MedVar::Reg && TargetArch == Arch::X64 && Image &&
      Image->Format == BinaryFormat::COFF && !PhiOutputVars.count(varKey(V))) {
    int Fallback = -1;
    for (const auto &Blk : CurMed->Blocks) {
      for (const auto &Op : Blk.Ops) {
        if (Op.Opcode != NdOp::COPY || Op.NumInputs < 1)
          continue;
        if (Op.Output.Kind != MedVar::Reg)
          continue;
        if (Op.Output.Id != V.Id && Op.Output.RegOff != V.RegOff)
          continue;
        const MedVar &Src = Op.Inputs[0];
        int Idx = -1;
        if (Src.Kind == MedVar::Param)
          Idx = abiParamIndex(Src);
        else if (Src.Kind == MedVar::Reg && Src.SSAVer == 0)
          Idx = regToArgIdx(Src.RegOff);
        if (Idx < 0 || static_cast<size_t>(Idx) >= CurMed->Params.size())
          continue;
        if (Op.Output.Id == V.Id && Op.Output.SSAVer == V.SSAVer) {
          MedVar Param = V;
          Param.Kind = MedVar::Param;
          Param.Id = Idx;
          if (CurMed->SourceTypeHint &&
              static_cast<size_t>(Idx) < CurMed->TypedParams.size())
            return SourceParameter(Param, static_cast<size_t>(Idx));
          return HighExpr::makeVar(Param, TypeRef{});
        }
        // SSA-bumped callee-saves with no new def (`rdi.2` after `mov rdi, r9`)
        // still hold the parameter.  A later computed def (INT_AND of r10 on
        // the GS_HANDLER_DATA bit-2 edge) does not.
        // A COPY from a Temp is also a computed def: `mov eax, edx` then
        // `mov rax, [bins+i]` must not remap RAX.3 onto arg1.  That deleted
        // Typed map bucket walks.
        if (Fallback < 0 && regToArgIdx(V.RegOff) < 0) {
          bool Computed = PhiOutputVars.count(varKey(V));
          for (const auto &B2 : CurMed->Blocks) {
            if (Computed)
              break;
            for (const auto &O2 : B2.Ops) {
              if (O2.Output.Id != V.Id || O2.Output.SSAVer != V.SSAVer)
                continue;
              if (O2.Opcode != NdOp::COPY) {
                Computed = true;
                break;
              }
              if (O2.NumInputs >= 1 && O2.Inputs[0].Kind != MedVar::Reg &&
                  O2.Inputs[0].Kind != MedVar::Param) {
                Computed = true;
                break;
              }
            }
          }
          if (!Computed)
            Fallback = Idx;
        }
      }
    }
    if (Fallback >= 0) {
      MedVar Param = V;
      Param.Kind = MedVar::Param;
      Param.Id = Fallback;
      if (CurMed->SourceTypeHint &&
          static_cast<size_t>(Fallback) < CurMed->TypedParams.size())
        return SourceParameter(Param, static_cast<size_t>(Fallback));
      return HighExpr::makeVar(Param, TypeRef{});
    }
  }

  if (CurMed) {
    if (const MedCallClobber *Clobber = findCallClobber(*CurMed, V)) {
      // Do not remap a caller-saved clobber onto this function's parameters.
      // After GSHandlerCheckCommon, r8 is flags scratch, not ContextRecord;
      // call arguments are recovered from COPYs / homes in collectCallArgs.
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
  // A source scalar describes only part of its machine carrier. Preserve a
  // partially overlapping slice's known bytes: a subsequent narrower read may
  // discard the unknown suffix. An entirely unknown slice stays unknown.
  if (!Value->Type || ByteOffset >= Value->Type->Size)
    return HighExpr::makeUndef(Bytes);
  if (Bytes > Value->Type->Size - ByteOffset) {
    const uint16_t Known = Value->Type->Size - ByteOffset;
    auto Slice = HighExpr::makeBinop(
        NdOp::CONCAT, HighExpr::makeUndef(Bytes - Known),
        sourceBitSlice(Value, ByteOffset, Known, Depth + 1));
    Slice->Type = NdType::makeInt(Bytes, false);
    return Slice;
  }
  if (Value->Type->Kind == NdTypeKind::Struct)
    return HighExpr::makeBitCast(
        HighExpr::makeRecordField(Value, static_cast<uint16_t>(ByteOffset),
                                  Bytes),
        NdType::makeInt(Bytes, false));
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
  return sourceScalarValue(Value, NdType::makeFloat(Bytes));
}

ExprPtr MedToHighConverter::sourceScalarValue(const MedVar &Value,
                                              const TypeRef &Type) {
  auto Bits = sourceBitSlice(medvarToExpr(Value), 0, Type->Size);
  if (equalSourceTypes(Bits->Type, Type))
    return Bits;
  return HighExpr::makeBitCast(Bits, Type);
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
  if (Result->IndirectTarget)
    Result->IndirectTarget = forceInlineExpr(Result->IndirectTarget);
  return Result;
}

ExprPtr MedToHighConverter::forceInlineCallTarget(const ExprPtr &E) {
  struct DepthGuard {
    int &D;
    DepthGuard(int &Depth) : D(Depth) { ++D; }
    ~DepthGuard() { --D; }
  };
  static thread_local int Depth = 0;
  DepthGuard Guard(Depth);
  if (!E || Depth > 16)
    return E;
  if (E->Kind == ExprKind::Var && E->Var.Id >= 0 && !E->Var.isConst()) {
    auto Key = varKey(E->Var);
    if (!PhiOutputVars.count(Key)) {
      auto It = DefExpr.find(Key);
      if (It != DefExpr.end() && It->second &&
          It->second->Kind != ExprKind::Call &&
          It->second->Kind != ExprKind::Phi &&
          It->second->MemoryOrdering == NdMemoryOrdering::None &&
          It->second.get() != E.get())
        return forceInlineCallTarget(It->second);
    }
  }
  auto Result = std::make_shared<HighExpr>(*E);
  for (size_t I = 0; I < Result->Operands.size(); ++I)
    Result->Operands[I] = forceInlineCallTarget(Result->Operands[I]);
  if (Result->IndirectTarget)
    Result->IndirectTarget = forceInlineCallTarget(Result->IndirectTarget);
  return Result;
}

//===----------------------------------------------------------------------===//
// buildExpressions
//===----------------------------------------------------------------------===//

void MedToHighConverter::buildExpressions(const MedFunc &Med) {
  UseCount.clear();
  DefExpr.clear();
  SourceRecordValues.clear();
  for (const auto &Block : Med.Blocks)
    for (const auto &Op : Block.Ops)
      if ((Op.Opcode == NdOp::CALL || Op.Opcode == NdOp::INDIR_CALL) &&
          Op.Output.Size) {
        const auto Type = sourceCallResultType(Op);
        if (Type->Kind == NdTypeKind::Struct)
          SourceRecordValues.emplace(varKey(Op.Output), Type);
      }
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
  HighConversionTrace Trace(Med, TheArch);
  Trace.med();
  auto TStart = std::chrono::steady_clock::now();
  TargetArch = TheArch;
  CurMed = &Med;
  SourceParameters = Med.SourceTypeHint
                         ? sourceABIParameters(*Med.SourceTypeHint)
                         : std::vector<SourceABIParameter>{};
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
  Func.RegisterCopyProjections = Med.RegisterCopyProjections;
  Func.ClassGetterCallFacts = Med.ClassGetterCallFacts;

  for (auto &ML : Med.Locals) {
    HighLocal HL;
    HL.Name = ML.display();
    HL.StackOff = ML.StackOff;
    HL.Type = NdType::makeInt(ML.Size);
    Func.Locals.push_back(HL);
  }

  auto PtrParamRegOffs = detectPtrParamRegs(Med);
  auto PtrParamIds = detectPtrParamIds(Med);
  const auto &TRI = getTargetRegInfo(TheArch);

  if (Med.SourceTypeHint) {
    for (const auto &P : Med.SourceTypeHint->Parameters)
      Func.Params.push_back({P.Name, P.Type});
  } else
    for (size_t PI = 0; PI < Med.Params.size(); ++PI) {
      auto &MP = Med.Params[PI];
      HighParam HP;
      HP.Name = "arg" + std::to_string(PI);
      if (Med.SourceTypeHint && PI < Med.TypedParams.size()) {
        HP.Name = Med.TypedParams[PI].Name;
        HP.Type = Med.TypedParams[PI].Type;
      } else if ((PtrParamRegOffs.count(MP.RegOff) &&
                  MP.RegOff != kNoParamReg &&
                  !TRI.isFrameOrLinkReg(MP.RegOff)) ||
                 PtrParamIds.count(MP.Id) || PtrParamIds.count(static_cast<int>(PI)))
        HP.Type = NdType::makePtr();
      else
        HP.Type = NdType::makeInt(MP.Size);
      Func.Params.push_back(HP);
    }

  size_t MedOps = 0;
  for (const auto &Block : Med.Blocks)
    MedOps += Block.Ops.size();
  if (Med.Blocks.size() > limits::kMaxStructurableMedBlocks ||
      MedOps > limits::kMaxStructurableMedOps) {
    fillUnstructuredGotoSkeleton(Func, Med);
    return Func;
  }

  auto TExpr = std::chrono::steady_clock::now();
  buildExpressions(Med);
  auto TStruct = std::chrono::steady_clock::now();
  structureControlFlow(Func, Med);
  Trace.high(Func, "structured");
  auto TSimp = std::chrono::steady_clock::now();
  simplifyControlFlow(Func, Med);
  Trace.high(Func, "simplified");
  auto TTypes = std::chrono::steady_clock::now();
  inferTypes(Func);
  auto TPost = std::chrono::steady_clock::now();

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
  Trace.high(Func, "before-dce");
  // Nest handler/filter bodies into try clauses before DCE.  Those blocks are
  // entered by the personality, so ordinary reachability would delete them
  // and leave empty __except/__catch arms.
  structureExceptionRegions(Func, Med);
  auto TEh = std::chrono::steady_clock::now();
  eliminateDeadStmts(Func);
  auto TDead = std::chrono::steady_clock::now();
  invertSkipGotos(Func);
  Trace.high(Func, "after-dce");
  auto TInvert = std::chrono::steady_clock::now();
  foldStructuredContinuations(Func, &Med);
  coalesceBranchEntryStatements(Func);
  eliminateHighDeadPhiCopies(Func);
  Trace.high(Func, "after-exceptions");
  auto TEnd = std::chrono::steady_clock::now();

  {
    auto TotalMs =
        std::chrono::duration_cast<std::chrono::milliseconds>(TEnd - TStart)
            .count();
    const char *Detail = std::getenv("NEVERD_HIGHIR_DETAIL");
    const bool WantDetail = Detail && Detail[0] == '1' && Detail[1] == '\0';
    if (TotalMs > 1000 || WantDetail) {
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
      if (WantDetail)
        syncWarning() << "m2h-dce: " << Med.Name
                      << " [eh=" << ElapsedMs(TDceStart, TEh)
                      << "ms dead=" << ElapsedMs(TEh, TDead)
                      << "ms invert=" << ElapsedMs(TDead, TInvert)
                      << "ms fold=" << ElapsedMs(TInvert, TEnd) << "ms]\n";
    }
  }

  LLVM_DEBUG(llvm::dbgs() << "MedIR -> HighIR: " << Func.Body.size()
                          << " statements for " << Func.Name << "\n");
  return Func;
}

} // namespace neverd
