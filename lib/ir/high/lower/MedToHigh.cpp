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

#include "../../../loader/Swift/SwiftErrorSourceProjection.h"
#include "HighEntryStackOffsets.h"

#include "neverd/Limits.h"
#include "neverd/ir/SourceABI.h"
#include "neverd/ir/TargetRegInfo.h"
#include "neverd/ir/high/HighFlowOracle.h"
#include "neverd/ir/high/HighSourceFlow.h"
#include "neverd/ir/med/I386PicAddress.h"
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

  /// How many conversions of the selected function to trace
  /// (NEVERD_HIGH_TRACE_INVOCATIONS, default 1).  A function with SEH scopes
  /// is converted once per block layout, and the kept one may be the second.
  static unsigned invocationLimit() noexcept {
    const char *Value = std::getenv("NEVERD_HIGH_TRACE_INVOCATIONS");
    unsigned Limit = 0;
    if (!Value || llvm::StringRef(Value).getAsInteger(10, Limit))
      return 1;
    return Limit;
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
      Enabled = Invocation < invocationLimit();
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
  // An entry seed copies the incoming register.  SSA may give the copy a new
  // version (`COPY RCX.1 = RCX` when CL is seeded too) while later reads
  // still name the incoming one; both sides are the incoming value.
  for (const MedOp &Op : Func.Blocks.front().Ops) {
    if (Op.Opcode != NdOp::COPY)
      break;
    if (Op.Output.Kind == MedVar::Reg && Op.Output.RegOff == RegOff &&
        Op.NumInputs >= 1 && Op.Inputs[0].Kind == MedVar::Reg &&
        Op.Inputs[0].Id == Op.Output.Id && Op.Inputs[0].SSAVer == 0 &&
        (Op.Output == V || Op.Inputs[0] == V))
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
      It->second->Kind == ExprKind::EntryRegister ||
      It->second->MemoryOrdering != NdMemoryOrdering::None ||
      It->second->MemoryAddressSpace != NdMemoryAddressSpace::Default)
    return nullptr;
  return It->second;
}

TypeRef MedToHighConverter::sourceCallResultType(const MedOp &Op) const {
  if (Op.SourceCallHint &&
      isNativeSwiftErrorSourceCall(*Op.SourceCallHint, TargetArch))
    if (auto Type = swiftErrorCallResultType(Op.SourceCallHint->Signature);
        Type && Type->Size == Op.Output.Size)
      return Type;
  if (Op.SourceCallHint)
    if (auto Type = sourceABICallResultType(Op.SourceCallHint->Signature);
        Type && Type->Size == Op.Output.Size)
      return Type;
  if (Op.Output.Kind == MedVar::Reg && Op.Output.Size == 10 &&
      getTargetRegInfo(TargetArch).isX87StackReg(Op.Output.RegOff))
    return NdType::makeFloat(10);
  return NdType::makeInt(Op.Output.Size, false);
}

ExprPtr MedToHighConverter::medvarToExpr(const MedVar &V) {
  if (V.Kind == MedVar::Unspecified)
    return HighExpr::makeUndef(V.Size);
  if (V.isConst()) {
    return HighExpr::makeConst(V.ConstVal, V.Size, V.Provenance,
                               V.AddressOwnerVA);
  }
  // The proven GOT base of an unlinked i386 object is address zero.
  if (I386GotBase.count(varKey(V)))
    return HighExpr::makeConst(0, V.Size, ConstantAddressProvenance::Scalar);

  auto SourceParameter = [&](MedVar Parameter, size_t Index) -> ExprPtr {
    const auto &Bindings = SourceParameters;
    if (Index >= Bindings.size())
      return HighExpr::makeUndef(V.Size);
    const auto &Binding = Bindings[Index];
    const auto &Declared =
        CurMed->SourceTypeHint->Parameters[Binding.ParameterIndex];
    if (Declared.TheRole == SourceParameterTypeHint::Role::SwiftErrorResult) {
      // The machine input is the slot's value, not its address. A dedicated
      // entry capture prevents a later memory effect from causing a reload.
      if (!SwiftErrorEntryInput)
        return HighExpr::makeUndef(V.Size);
      return sourceBitSlice(
          HighExpr::makeVar(*SwiftErrorEntryInput, Binding.Type), 0, V.Size);
    }
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
  // A float or double parameter fills the low lane of its vector register;
  // the register's other bytes, which the function does not read, are zero.
  auto ScalarParameter = [&](MedVar Param, size_t Index) -> ExprPtr {
    if (!CurMed || CurMed->SourceTypeHint ||
        Index >= CurMed->TypedParams.size())
      return nullptr;
    const TypeRef &Scalar = CurMed->TypedParams[Index].Type;
    if (!Scalar || Scalar->Kind != NdTypeKind::Float || !V.Size)
      return nullptr;
    Param.Size = Scalar->Size;
    auto Bits = HighExpr::makeBitCast(HighExpr::makeVar(Param, Scalar),
                                      NdType::makeInt(Scalar->Size, false));
    if (V.Size == Scalar->Size)
      return Bits;
    ExprPtr Carrier = V.Size > Scalar->Size
                          ? HighExpr::makeUnary(NdOp::INT_ZEXT, Bits)
                          : HighExpr::makeBinop(NdOp::SUBBYTES, Bits,
                                                HighExpr::makeConst(0, 4));
    Carrier->Type = NdType::makeInt(V.Size, false);
    return Carrier;
  };
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
      if (auto Scalar = ScalarParameter(Param, static_cast<size_t>(Slot)))
        return Scalar;
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
      if (auto Scalar = ScalarParameter(Param, I))
        return Scalar;
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
      Image->abiFormat() == BinaryFormat::COFF &&
      !PhiOutputVars.count(varKey(V))) {
    if (ParamCopyIndexFunc != CurMed) {
      // One pass over the function instead of one per variable reference.
      ParamCopyIndexFunc = CurMed;
      ParamSourceCopies.clear();
      ComputedVersions.clear();
      for (const auto &Blk : CurMed->Blocks)
        for (const auto &Op : Blk.Ops) {
          // Any def but a COPY of a register or parameter computes a value.
          if (Op.Opcode != NdOp::COPY ||
              (Op.NumInputs >= 1 && Op.Inputs[0].Kind != MedVar::Reg &&
               Op.Inputs[0].Kind != MedVar::Param))
            ComputedVersions.insert({Op.Output.Id, Op.Output.SSAVer});
          if (Op.Opcode != NdOp::COPY || Op.NumInputs < 1 ||
              Op.Output.Kind != MedVar::Reg)
            continue;
          const MedVar &Src = Op.Inputs[0];
          int Idx = -1;
          if (Src.Kind == MedVar::Param)
            Idx = abiParamIndex(Src);
          else if (Src.Kind == MedVar::Reg && Src.SSAVer == 0)
            Idx = regToArgIdx(Src.RegOff);
          if (Idx >= 0 && static_cast<size_t>(Idx) < CurMed->Params.size())
            ParamSourceCopies.push_back({&Op, Idx});
        }
    }
    int Fallback = -1;
    for (const auto &[OpPtr, Idx] : ParamSourceCopies) {
      {
        const MedOp &Op = *OpPtr;
        if (Op.Output.Id != V.Id && Op.Output.RegOff != V.RegOff)
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
          const bool Computed = PhiOutputVars.count(varKey(V)) ||
                                ComputedVersions.count({V.Id, V.SSAVer});
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
      // Consecutive calls preserve the intersection of their known prefixes.
      // Follow exact SSA inputs before building an expression: nesting one
      // SUBBYTES/extension pair per call exhausts the ordinary slice budget
      // even when a long chain preserves the same D8-D15 value throughout.
      MedVar Input = V;
      uint16_t Known = V.Size;
      size_t Steps = 0;
      do {
        const auto &Before = Clobber->PreservedInput;
        if (++Steps > CurMed->CallClobbers.size() ||
            Clobber->Value.Kind != MedVar::Reg ||
            Clobber->Value.RegOff != Input.RegOff ||
            Clobber->Value.TheArch != Input.TheArch ||
            Input.Size > Clobber->Value.Size ||
            Clobber->PreservedPrefixSize == 0 ||
            Clobber->PreservedPrefixSize >= Clobber->Value.Size ||
            Before.Kind != MedVar::Reg || Before.Id != Input.Id ||
            Before.RegOff != Input.RegOff || Before.TheArch != Input.TheArch ||
            Before.Size != Clobber->Value.Size || Before == Input)
          return HighExpr::makeUndef(V.Size);
        Known = std::min(Known, Clobber->PreservedPrefixSize);
        Input = Before;
        Clobber = findCallClobber(*CurMed, Input);
      } while (Clobber);
      auto Low = sourceBitSlice(medvarToExpr(Input), 0, Known);
      // Calls do not zero the unpreserved suffix. Retain unknown bytes so a
      // later low slice can discard them without making a wide read defined.
      return sourceBitSlice(Low, 0, V.Size);
    }
  }

  auto Key = varKey(V);

  // A single counted use takes its definition inline.  So does a use no
  // count saw, such as a register argument the call-site scan finds after
  // counting: lowerGenericAssign emitted no assignment for that definition.
  auto UIt = UseCount.find(Key);
  const int Uses = UIt == UseCount.end() ? 0 : UIt->second;
  if (Uses == 1 || (Uses == 0 && !CallOutputs.count(Key)))
    if (auto Definition = inlineableDefinition(Key))
      return Definition;

  return HighExpr::makeVar(V);
}

void MedToHighConverter::indexMedDefinitions() {
  if (!CurMed || EntryOffsetDefsFor == CurMed)
    return;
  EntryStackOffsets =
      std::make_shared<detail::HighEntryStackOffsets>(*CurMed, TargetArch);
  EntryOffsetDefsFor = CurMed;
}

const MedOp *MedToHighConverter::uniqueMedDefinition(const MedVar &V) {
  if (!CurMed)
    return nullptr;
  indexMedDefinitions();
  return EntryStackOffsets->uniqueDefinition(V);
}

ExprPtr MedToHighConverter::memoryAddressExpr(const MedVar &V,
                                              bool InlineDefinition) {
  if (CurMed && TargetArch == Arch::X86 && Image &&
      (!Image->isELF() || !Image->IsRelocatable))
    if (auto Address = foldI386PicAddress(*CurMed, V, [&](const MedVar &Value) {
          return uniqueMedDefinition(Value);
        }))
      return HighExpr::makeConst(*Address, V.Size,
                                 ConstantAddressProvenance::DataAddress);
  ExprPtr Address;
  if (InlineDefinition && V.Id >= 0)
    Address = inlineableDefinition(varKey(V));
  if (!Address)
    Address = medvarToExpr(V);

  // LowIR represents x86 effective addresses as 8-byte VAs even when their
  // arithmetic wraps at the target pointer width. Recover that width at the
  // memory boundary; do not narrow arbitrary wide arithmetic or sign-extended
  // addresses. The unsigned bit view preserves zero extension when bit 31 is
  // set, including when the emitted C runs on a 64-bit host.
  const uint16_t PointerBytes = getTargetRegInfo(TargetArch).PointerSize;
  if (!Address || !PointerBytes || PointerBytes >= 8 ||
      Address->Kind != ExprKind::UnaryOp || Address->Op != NdOp::INT_ZEXT ||
      Address->Operands.size() != 1 || !Address->Operands[0] ||
      !Address->Type || Address->Type->Kind != NdTypeKind::Int ||
      Address->Type->Size != 8 ||
      Address->MemoryOrdering != NdMemoryOrdering::None ||
      Address->MemoryAddressSpace != NdMemoryAddressSpace::Default)
    return Address;
  const ExprPtr &Source = Address->Operands[0];
  if (!Source->Type || Source->Type->Kind != NdTypeKind::Int ||
      Source->Type->Size != PointerBytes)
    return Address;
  return HighExpr::makeBitCast(Source, NdType::makeInt(PointerBytes, false));
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
  if (ExpressionCloneObserver)
    ExpressionCloneObserver(E, Result);
  return Result;
}

bool MedToHighConverter::memoryReadReachesCallTarget(
    const MedVar &Value) const {
  const MedBlock *Block = CallTargetUse.Block;
  if (!Block || CallTargetUse.CallIdx > Block->Ops.size())
    return false;
  size_t ReadIdx = CallTargetUse.CallIdx;
  for (size_t I = CallTargetUse.CallIdx; I-- > 0;) {
    const MedOp &Op = Block->Ops[I];
    if (Op.Opcode == NdOp::LOAD && Op.Output.Kind == Value.Kind &&
        Op.Output.Id == Value.Id && Op.Output.SSAVer == Value.SSAVer) {
      ReadIdx = I;
      break;
    }
  }
  if (ReadIdx == CallTargetUse.CallIdx)
    return false;
  const MedOp &Read = Block->Ops[ReadIdx];
  const uint64_t StackPointer = getTargetRegInfo(TargetArch).StackPointer;
  // Whether \p Address, used by the op at \p Before, is this function's own
  // stack: the stack pointer through copies, zero extensions and constant
  // offsets.
  auto frameAddress = [&](size_t Before, MedVar Address) {
    for (size_t I = Before; I-- > 0;) {
      if (Address.Kind == MedVar::Reg && Address.RegOff == StackPointer)
        return true;
      const MedOp &Def = Block->Ops[I];
      if (Def.Output.Kind != Address.Kind || Def.Output.Id != Address.Id ||
          Def.Output.SSAVer != Address.SSAVer || Def.NumInputs < 1)
        continue;
      const bool Offset =
          (Def.Opcode == NdOp::INT_ADD || Def.Opcode == NdOp::INT_SUB) &&
          Def.NumInputs == 2 && Def.Inputs[1].isConst();
      if (Def.Opcode != NdOp::COPY && Def.Opcode != NdOp::INT_ZEXT && !Offset)
        return false;
      Address = Def.Inputs[0];
    }
    return Address.Kind == MedVar::Reg && Address.RegOff == StackPointer;
  };
  if (Read.NumInputs < 1)
    return false;
  // A store to this function's stack (outgoing arguments, spills) and a read
  // of image or heap memory name different storage, and so do a heap store
  // and a stack read.  A pointer the function loaded is assumed not to point
  // into its own frame.
  const bool ReadsFrame = frameAddress(ReadIdx, Read.Inputs[0]);
  for (size_t I = ReadIdx + 1; I < CallTargetUse.CallIdx; ++I) {
    const MedOp &Op = Block->Ops[I];
    if (Op.MemoryOrdering != NdMemoryOrdering::None)
      return false;
    // A store, call or atomic can change the memory the read observed.
    switch (Op.Opcode) {
    case NdOp::STORE:
      if (Op.NumInputs >= 1 &&
          Op.MemoryAddressSpace == NdMemoryAddressSpace::Default &&
          Read.MemoryAddressSpace == NdMemoryAddressSpace::Default &&
          frameAddress(I, Op.Inputs[0]) != ReadsFrame)
        continue;
      return false;
    case NdOp::CALL:
    case NdOp::INDIR_CALL:
    case NdOp::INTRINSIC:
    case NdOp::ATOMIC_XCHG:
    case NdOp::ATOMIC_ADD:
    case NdOp::ATOMIC_CMPXCHG:
      return false;
    default:
      break;
    }
  }
  return true;
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
    if (!PhiOutputVars.count(Key) && (!MemoryReadOutputs.count(Key) ||
                                      memoryReadReachesCallTarget(E->Var))) {
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
  if (ExpressionCloneObserver)
    ExpressionCloneObserver(E, Result);
  return Result;
}

//===----------------------------------------------------------------------===//
// buildExpressions
//===----------------------------------------------------------------------===//

void MedToHighConverter::buildExpressions(const MedFunc &Med) {
  UseCount.clear();
  DefExpr.clear();
  SourceRecordValues.clear();
  collectI386GotBase(Med);
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
  SwiftErrorEntryInput.reset();
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

  if (Med.SourceParametersBound && Med.SourceTypeHint)
    if (const auto Error = sourceABIErrorResult(*Med.SourceTypeHint)) {
      MedVar Input;
      Input.Kind = MedVar::Temp;
      Input.Id = NextHighTempId++;
      Input.Size = Error->Type->Size;
      Input.TheArch = TargetArch;
      SwiftErrorEntryInput = Input;
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
  // ABI recovery binds call operands after SSA, outside the MedOp input
  // array. They are real uses of their reaching definitions: omitting them
  // makes a computed outgoing register look dead before HighIR builds calls.
  // An operand the call's block also stores before the call (an i386 push)
  // is read once: that store is the argument itself, which dead-store
  // elimination drops, so a single-use definition still prints inline.
  std::map<int, const MedBlock *> BlockById;
  for (const auto &Blk : Med.Blocks)
    BlockById.emplace(Blk.Id, &Blk);
  for (const MedCallInfo &Call : Med.CallInfos) {
    VarKeySet Stored;
    if (auto It = BlockById.find(Call.BlockId); It != BlockById.end())
      for (int J = Call.OpIdx - 1;
           J >= 0 && J < static_cast<int>(It->second->Ops.size()); --J) {
        const MedOp &Op = It->second->Ops[static_cast<size_t>(J)];
        if (Op.Opcode == NdOp::CALL || Op.Opcode == NdOp::INDIR_CALL ||
            Op.Opcode == NdOp::INTRINSIC)
          break;
        if (Op.Opcode == NdOp::STORE && Op.NumInputs >= 2 &&
            Op.Inputs[1].Id >= 0)
          Stored.insert(varKey(Op.Inputs[1]));
      }
    for (const MedVar &Arg : Call.Args)
      if (Arg.Id >= 0 && !Stored.count(varKey(Arg)))
        UseCount[varKey(Arg)]++;
  }

  // Build each definition before its uses: a use builds its single-use
  // definition inline, and a definition that is not yet built leaves a name
  // whose assignment is never emitted. Block order is not dominance order (a
  // loop entered at its bottom test lays that test out after the body that
  // uses its values), so walk each root's blocks in reverse post-order.
  std::vector<const MedBlock *> Order;
  {
    std::unordered_map<int, size_t> IndexOf;
    for (size_t I = 0; I < Med.Blocks.size(); ++I)
      IndexOf[Med.Blocks[I].Id] = I;
    std::vector<char> Seen(Med.Blocks.size(), 0);
    for (size_t Root = 0; Root < Med.Blocks.size(); ++Root) {
      if (Seen[Root])
        continue;
      std::vector<const MedBlock *> Post;
      std::vector<std::pair<size_t, size_t>> Stack{{Root, 0}};
      Seen[Root] = 1;
      while (!Stack.empty()) {
        const size_t B = Stack.back().first;
        const std::vector<int> &Succs = Med.Blocks[B].Succs;
        if (Stack.back().second < Succs.size()) {
          auto It = IndexOf.find(Succs[Stack.back().second++]);
          if (It != IndexOf.end() && !Seen[It->second]) {
            Seen[It->second] = 1;
            Stack.push_back({It->second, 0});
          }
          continue;
        }
        Post.push_back(&Med.Blocks[B]);
        Stack.pop_back();
      }
      Order.insert(Order.end(), Post.rbegin(), Post.rend());
    }
  }
  for (const MedBlock *Block : Order) {
    const MedBlock &Blk = *Block;
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

void MedToHighConverter::attachSEHHandlerEntryCopies(HighFunc &Func,
                                                     const MedFunc &Med) {
  // A handler block ordinary flow also enters merges RAX in PHIs. Their
  // ordinary edges carry copies already; on the dispatcher's entry the arm
  // assigns the exception code before it jumps to the handler.
  std::map<va_t, std::vector<HighStmt>> Copies;
  for (const MedBlock &Block : Med.Blocks)
    for (const PhiNode &Phi : Block.Phis) {
      if (!Phi.ExceptionalEntry)
        continue;
      HighStmt Copy;
      Copy.Kind = StmtKind::Assign;
      Copy.Dst = HighExpr::makeVar(Phi.Output);
      ExprPtr Code = HighExpr::makeVar(*Phi.ExceptionalEntry);
      if (Phi.Output.Size > Phi.ExceptionalEntry->Size) {
        Code = HighExpr::makeUnary(NdOp::INT_ZEXT, Code);
        Code->Type = NdType::makeInt(Phi.Output.Size, false);
      }
      Copy.Val = std::move(Code);
      Copies[Block.StartAddr].push_back(std::move(Copy));
    }
  if (Copies.empty())
    return;
  walkStmts(Func.Body, [&](HighStmt &S) {
    if (S.Kind != StmtKind::SEHTry)
      return;
    for (size_t C = 0; C < S.EHClauses.size(); ++C) {
      if (S.EHClauses[C].Kind != HighEHClauseKind::SEHExcept)
        continue;
      auto It = Copies.find(S.EHClauses[C].HandlerVA);
      if (It == Copies.end())
        continue;
      if (S.EHClauseBodies.size() <= C)
        S.EHClauseBodies.resize(C + 1);
      auto &Arm = S.EHClauseBodies[C];
      Arm.insert(Arm.begin(), It->second.begin(), It->second.end());
      if (Arm.size() == It->second.size()) {
        HighStmt Jump;
        Jump.Kind = StmtKind::Goto;
        Jump.GotoTarget = S.EHClauses[C].HandlerVA;
        Arm.push_back(std::move(Jump));
      }
    }
  });
}

void MedToHighConverter::reduceLateGotos(HighFunc &Func) {
  if (Func.Body.size() > limits::kMaxLateGotoReductionStmts)
    return;
  // The join-default sink models Win64 register joins (structureIfElse).
  const bool LateJoinSink = !CurMed || CurMed->CC == CallingConv::Win64;
  bool Dirty = hoistTryExitJumps(Func.Body);
  // Cases that only jump to the same tail become one case first; copying
  // the tail into each of them would multiply it.
  Dirty |= groupSwitchCases(Func.Body);
  Dirty |= duplicateSmallReturnTails(Func.Body);
  // Region splices nest whole multi-block regions, so they run only after
  // the local rewrites have settled.  The late rewrites can leave new jumps
  // to a small return tail; those get one more tail-duplication pass.
  // Backward jumps become loops late, once fall-through joins no longer need
  // explicit jumps. Jumps from a loop to what follows it become breaks last:
  // until then they are joins that the rewrites turn into if/else.
  for (int Phase = 0; Phase < 6; ++Phase) {
    // Dead copies left by earlier rewrites can sit between a jump and
    // its label; clear them before the next phase looks.
    if (Phase != 0 && Dirty) {
      eliminateDeadStmts(Func);
      Dirty = false;
    }
    if (Phase >= 2) {
      bool Rewritten;
      if (Phase == 3)
        Rewritten = loopifyBackwardGotos(Func.Body);
      else if (Phase == 5) {
        // A switch first takes the exit most of its jumps go to as what
        // follows it, so those jumps become its breaks.
        Rewritten = busiestExitFollowsTheSwitch(Func.Body);
        Rewritten |= breakToTheLoopFollow(Func.Body);
      } else
        Rewritten = duplicateSmallReturnTails(Func.Body) |
                    duplicateSmallJumpTails(Func.Body);
      // The last phase always runs its rounds: the dead-code cleanup above
      // can leave a jump that now just falls through.
      if (!Rewritten && Phase != 5)
        continue;
      Dirty |= Rewritten;
    }
    for (int Round = 0; Round < 8; ++Round) {
      const bool Grouped =
          groupSwitchCases(Func.Body) | dropJumpsToTheNextStatement(Func.Body) |
          hoistTryExitJumps(Func.Body) |
          (Phase != 0 && rotateLoopsToTheirEntry(Func.Body)) |
          (Phase != 0 && hoistLoopExitTests(Func.Body)) |
          (Phase != 0 && moveLoopTailsToTheirBreak(Func.Body)) |
          (Phase != 0 && moveSwitchTailsToTheirExit(Func.Body)) |
          (Phase != 0 && absorbSwitchRangeGuards(Func.Body)) |
          (Phase != 0 && unwrapLoopsThatNeverRepeat(Func.Body)) |
          (Phase != 0 && LateJoinSink && sinkJoinDefaultsLate(Func)) |
          (Phase != 0 && hoistLoopEntryLabels(Func.Body)) |
          (Phase != 0 && flattenBlocks(Func.Body)) |
          (Phase != 0 && loopifyTrailingArmBodies(Func.Body)) |
          (Phase != 0 && hoistSharedArmTails(Func.Body));
      if (!reduceSingleUseGotos(Func.Body, /*SpliceRegions=*/Phase != 0) &&
          !Grouped)
        break;
      Dirty = true;
    }
  }
  if (Dirty)
    eliminateDeadStmts(Func);
}

HighFunc MedToHighConverter::convert(const MedFunc &Med, Arch TheArch) {
  const ExceptionFunction *EH =
      Med.ExceptionMetadata ? &*Med.ExceptionMetadata : nullptr;
  if (!EH || !EH->SEH || EH->SEH->Scopes.empty() || EH->Cxx || EH->Itanium ||
      EH->Registration)
    return convertOnce(Med, TheArch);
  // Reverse postorder can scatter a guarded range so that no __try holds it,
  // which address order may keep whole.  The layout that leaves fewer guarded
  // ranges unstructured is kept; on a tie reverse postorder stays.  How many
  // gotos either prints never enters the choice.  The observers see only the
  // conversion that is kept.
  auto SavedExpression = std::move(ExpressionObserver);
  auto SavedClone = std::move(ExpressionCloneObserver);
  auto SavedStatement = std::move(StatementObserver);
  ExpressionObserver = {};
  ExpressionCloneObserver = {};
  StatementObserver = {};
  const bool Observed = SavedExpression || SavedClone || SavedStatement;
  SEHAddressOrder = false;
  HighFunc Func = convertOnce(Med, TheArch);
  if (Func.UnstructuredExceptionRegions) {
    SEHAddressOrder = true;
    HighFunc Ordered = convertOnce(Med, TheArch);
    if (Ordered.UnstructuredExceptionRegions <
        Func.UnstructuredExceptionRegions)
      Func = std::move(Ordered);
    else
      SEHAddressOrder = false;
  }
  ExpressionObserver = std::move(SavedExpression);
  ExpressionCloneObserver = std::move(SavedClone);
  StatementObserver = std::move(SavedStatement);
  if (Observed)
    Func = convertOnce(Med, TheArch);
  SEHAddressOrder = false;
  return Func;
}

HighFunc MedToHighConverter::convertOnce(const MedFunc &Med, Arch TheArch) {
  HighConversionTrace Trace(Med, TheArch);
  Trace.med();
  auto TStart = std::chrono::steady_clock::now();
  TargetArch = TheArch;
  CurMed = &Med;
  ParamCopyIndexFunc = nullptr;
  LoadedEntrySlotsFor = nullptr;
  EntryOffsetDefsFor = nullptr;
  EntryStackOffsets.reset();
  SourceParameters = Med.SourceTypeHint
                         ? sourceABIParameters(*Med.SourceTypeHint)
                         : std::vector<SourceABIParameter>{};
  HighFunc Func;
  Func.Entry = Med.Entry;
  Func.FrameSize = Med.FrameSize;
  Func.FrameHeadroom = Med.FrameHeadroom;
  Func.Name = Med.Name;
  Func.DoesNotReturn = Med.DoesNotReturn;
  Func.ReturnsNoValue = Med.ReturnsNoValue;
  Func.EntryKind = Med.EntryKind;
  Func.ExceptionMetadata = Med.ExceptionMetadata;
  for (const auto &Entry : Med.CxxContinuationEntries)
    Func.CxxContinuationTargets.insert(Entry.Target);
  // Catch returns enter the parent through edges absent from its ordinary
  // CFG. Publish those edges before control-flow cleanup can discard or move
  // their target statements; the funclet bodies are attached module-wide.
  const bool EarlyCxxRegions =
      !Func.CxxContinuationTargets.empty() ||
      (Med.ExceptionMetadata && Med.ExceptionMetadata->Registration &&
       Med.ExceptionMetadata->Registration->hasCxxCallbackStack());
  Func.ReturnType = Med.ReturnType ? Med.ReturnType : inferReturnType(Med);
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

  auto CaptureSwiftError = [&] {
    if (!SwiftErrorEntryInput || !Med.SourceTypeHint)
      return;
    const auto Error = sourceABIErrorResult(*Med.SourceTypeHint);
    if (!Error)
      return;
    MedVar Slot;
    Slot.Kind = MedVar::Param;
    Slot.Id = static_cast<int>(Error->ParameterIndex);
    Slot.RegOff = Error->Location.RegisterOffset;
    Slot.Size = 8;
    Slot.TheArch = TheArch;
    HighStmt Capture;
    Capture.Kind = StmtKind::Assign;
    Capture.Dst = HighExpr::makeVar(*SwiftErrorEntryInput, Error->Type);
    Capture.Val = HighExpr::makeLoad(
        HighExpr::makeVar(
            Slot, Med.SourceTypeHint->Parameters[Error->ParameterIndex].Type),
        Error->Type);
    Func.Body.insert(Func.Body.begin(), std::move(Capture));
    Func.SwiftErrorEntry = HighFunc::SwiftErrorEntryProjection{
        *Med.SourceTypeHint, *SwiftErrorEntryInput};
  };

  // MedIR keeps the loads and stores of an incoming stack argument the
  // function writes in place as memory accesses to its home slot. Seed each
  // home with its parameter at entry, as the LLVM emitter does, so the first
  // read sees the argument rather than an uninitialized local.
  auto SeedMutableStackParamHomes = [&] {
    if (Med.MutableStackParamHomes.empty() || Med.SourceTypeHint)
      return;
    std::optional<MedVar> EntrySP;
    for (const MedBlock &Block : Med.Blocks) {
      for (const MedOp &Op : Block.Ops)
        for (uint8_t I = 0; I < Op.NumInputs && !EntrySP; ++I)
          if (Op.Inputs[I].Kind == MedVar::Reg &&
              Op.Inputs[I].RegOff == TRI.StackPointer &&
              Op.Inputs[I].SSAVer == 0 && Op.Inputs[I].RenameTag < 0)
            EntrySP = Op.Inputs[I];
      if (EntrySP)
        break;
    }
    if (!EntrySP)
      return;
    std::vector<HighStmt> Seeds;
    for (const auto &[ParamIdx, Offset] : Med.MutableStackParamHomes) {
      if (ParamIdx < 0 || static_cast<size_t>(ParamIdx) >= Med.Params.size())
        continue;
      HighStmt Seed;
      Seed.Kind = StmtKind::Store;
      Seed.Addr = Med.Entry;
      Seed.StoreAddr = HighExpr::makeBinop(
          NdOp::INT_ADD, HighExpr::makeVar(*EntrySP),
          HighExpr::makeConst(static_cast<uint64_t>(Offset), EntrySP->Size));
      MedVar Param = Med.Params[ParamIdx];
      Param.Kind = MedVar::Param;
      Param.Id = ParamIdx;
      Seed.StoreVal = medvarToExpr(Param);
      Seeds.push_back(std::move(Seed));
    }
    Func.Body.insert(Func.Body.begin(), Seeds.begin(), Seeds.end());
  };

  if (Med.SourceTypeHint) {
    for (const auto &P : Med.SourceTypeHint->Parameters)
      Func.Params.push_back({P.Name, P.Type});
  } else
    for (size_t PI = 0; PI < Med.Params.size(); ++PI) {
      auto &MP = Med.Params[PI];
      HighParam HP;
      HP.Name = "arg" + std::to_string(PI);
      HP.RegOff = MP.RegOff;
      HP.MedIndex = MP.Id;
      if (Med.SourceTypeHint && PI < Med.TypedParams.size()) {
        HP.Name = Med.TypedParams[PI].Name;
        HP.Type = Med.TypedParams[PI].Type;
      } else if (PI < Med.TypedParams.size() && Med.TypedParams[PI].Type &&
                 (Med.TypedParams[PI].Type->Kind == NdTypeKind::Ptr ||
                  Med.TypedParams[PI].Type->Kind == NdTypeKind::Float)) {
        // Direct memory uses and exact forwarded call roles share the MedIR
        // parameter certificate with the LLVM backend.
        HP.Type = Med.TypedParams[PI].Type;
      } else if ((PtrParamRegOffs.count(MP.RegOff) &&
                  MP.RegOff != kNoParamReg &&
                  !TRI.isFrameOrLinkReg(MP.RegOff)) ||
                 PtrParamIds.count(MP.Id) ||
                 PtrParamIds.count(static_cast<int>(PI)))
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
    // Too large to structure: still lower every operation, block by block,
    // with explicit gotos between blocks.  MedIR that skipped SSA cannot be
    // lowered this way (a register has no unique definition), so it keeps the
    // skeleton.
    if (Med.SkippedSSA) {
      fillUnstructuredGotoSkeleton(Func, Med);
      return Func;
    }
    buildExpressions(Med);
    structureControlFlow(Func, Med);
    CaptureSwiftError();
    SeedMutableStackParamHomes();
    if (EarlyCxxRegions)
      structureExceptionRegions(Func, Med);
    Trace.high(Func, "structured");
    inferTypes(Func);
    eraseKeepingBranchEntries(
        Func.Body, gotoTargets(Func.Body), [](const HighStmt &S) {
          return S.Kind == StmtKind::Assign && S.Dst && S.Val &&
                 S.Dst->Kind == ExprKind::Var && S.Val->Kind == ExprKind::Var &&
                 S.Dst->Var == S.Val->Var;
        });
    ensureTrailingReturn(Func, Med);
    if (!EarlyCxxRegions)
      structureExceptionRegions(Func, Med);
    attachSEHHandlerEntryCopies(Func, Med);
    reduceLateGotos(Func);
    Trace.high(Func, "after-exceptions");
    return Func;
  }

  auto TExpr = std::chrono::steady_clock::now();
  buildExpressions(Med);
  auto TStruct = std::chrono::steady_clock::now();
  structureControlFlow(Func, Med);
  CaptureSwiftError();
  SeedMutableStackParamHomes();
  if (EarlyCxxRegions)
    structureExceptionRegions(Func, Med);
  Trace.high(Func, "structured");
  auto TSimp = std::chrono::steady_clock::now();
  simplifyControlFlow(Func, Med);
  Trace.high(Func, "simplified");
  auto TTypes = std::chrono::steady_clock::now();
  inferTypes(Func);
  auto TPost = std::chrono::steady_clock::now();

  eraseKeepingBranchEntries(
      Func.Body, gotoTargets(Func.Body), [](const HighStmt &S) {
        return S.Kind == StmtKind::Assign && S.Dst && S.Val &&
               S.Dst->Kind == ExprKind::Var && S.Val->Kind == ExprKind::Var &&
               S.Dst->Var == S.Val->Var;
      });

  stripPrologueEpilogue(Func);
  ensureTrailingReturn(Func, Med);

  auto TDceStart = std::chrono::steady_clock::now();
  Trace.high(Func, "before-dce");
  // Nest handler/filter bodies into try clauses before DCE.  Those blocks are
  // entered by the personality, so ordinary reachability would delete them
  // and leave empty __except/__catch arms.
  if (!EarlyCxxRegions)
    structureExceptionRegions(Func, Med);
  attachSEHHandlerEntryCopies(Func, Med);
  auto TEh = std::chrono::steady_clock::now();
  eliminateDeadStmts(Func);
  auto TDead = std::chrono::steady_clock::now();
  invertSkipGotos(Func);
  Trace.high(Func, "after-dce");
  auto TInvert = std::chrono::steady_clock::now();
  foldStructuredContinuations(Func, &Med);
  coalesceBranchEntryStatements(Func);
  eliminateHighDeadPhiCopies(Func);
  reduceLateGotos(Func);
  // The last skip-goto inversion runs here rather than in the C emitter, so
  // that the flow check below sees the body that is printed.
  invertSkipGotos(Func);
  elseArmsForFallthroughJumps(Func);
  loopsForArmsJumpingBack(Func.Body);
  loopsForNestedJumpsBack(Func.Body);
  loopJumpsAsBreakAndContinue(Func.Body);
  // Jumps to a return tail that structuring left become copies of it when
  // the tail prints short; earlier, such a jump may still become a break.
  duplicateSmallReturnTails(Func.Body, /*PrintedSize=*/true);
  foldTempsInReturnTails(Func.Body);
  mergeJumpsIntoNextIfArms(Func);
  // Names merge last: every earlier pass may still move statements as if
  // each local had the definitions it had in SSA.
  coalesceHighPhiCopies(Func);
  // A read used once by the next statement moves into it, and a copy back
  // from a temporary joins its definition, once names merged.
  inlineAdjacentLoads(Func);
  foldCopiesIntoDefinitions(Func);
  // Width and signedness follow the merged names: one declaration, one type.
  narrowLocals(Func);
  chooseIntegerSignedness(Func);
  nameRepeatedValues(Func);
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
  reportHighFlowOracle(Func, Med, "final");
  return Func;
}

} // namespace neverd
