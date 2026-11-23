//===- X64KeptReturnRegister.cpp - RAX a callee never writes -------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Read the caller's own value where it reads RAX after a call to a callee
/// that never writes RAX.
//===----------------------------------------------------------------------===//

#include "../PipelineReturnModelingDetail.h"

#include "neverd/ir/CallRegisterContract.h"
#include "neverd/ir/TargetRegInfo.h"
#include "neverd/ir/low/CallRegisterEffects.h"
#include "neverd/ir/med/MedByteDemand.h"
#include "neverd/ir/med/MedIR.h"
#include "neverd/loader/BinaryImage.h"
#include "neverd/pipeline/Pipeline.h"

#include "llvm/ADT/STLExtras.h"

#include <cstddef>
#include <cstdint>
#include <map>
#include <optional>
#include <utility>
#include <vector>

namespace neverd {

namespace {
/// The value RAX holds at a point of one function, where one definition
/// reaches that point on every path.
class ValueOfReturnRegister {
public:
  ValueOfReturnRegister(const MedFunc &Func, const TargetRegInfo &TRI)
      : Func(Func), TRI(TRI) {
    for (size_t I = 0; I < Func.Blocks.size(); ++I)
      IndexOf.emplace(Func.Blocks[I].Id, I);
  }

  /// Whether \p Op is a call the register keeps its value across: one
  /// whose callee's summary shows it never writes RAX, made by platform
  /// code or by Go code of the register ABI, whose first argument RAX
  /// holds.  A Go write barrier returns its buffer space in a register of
  /// its own, which the lift hands back as the result, and code of Go's
  /// stack ABI returns in stack slots nothing models yet: the output of a
  /// call under either contract stays as it is.
  bool keptAcross(const MedOp &Op) const {
    return (Op.Opcode == NdOp::CALL || Op.Opcode == NdOp::INDIR_CALL) &&
           !Op.SourceCallHint &&
           (Op.RegisterContract == CallRegisterContract::Platform ||
            Op.RegisterContract == CallRegisterContract::GoInternal) &&
           callPreservesRegisterBytes(Arch::X64, Op.CallPreservedGPRs,
                                      TRI.IntReturnReg, TRI.FullRegWidth);
  }

  /// What such a call gives the register by the lift's default: its output,
  /// or the leftover that replaced it when the result moved to the vector
  /// register (keepReplacedCallOutput).  Nullopt where it gives none.
  std::optional<MedVar> givenBy(const MedOp &Call) const {
    if (holds(Call.Output))
      return Call.Output;
    for (const MedCallClobber &Clobber : Func.CallClobbers)
      if (Clobber.CallSiteId == Call.CallSiteId && holds(Clobber.Value))
        return Clobber.Value;
    return std::nullopt;
  }

  /// The value before op \p Index of block \p Block.  Nullopt where paths
  /// bring different values no PHI merges (the register was dead there
  /// while the call still defined it), where the function was entered with
  /// it, and where a callee left it and nothing names that.
  std::optional<MedVar> before(size_t Block, size_t Index) {
    Visits = 0;
    State.assign(Func.Blocks.size(), {});
    return scan(Block, Index);
  }

private:
  enum class Progress : uint8_t { None, Visiting, Done };
  struct BlockState {
    Progress At = Progress::None;
    std::optional<MedVar> AtEnd;
  };

  bool holds(const MedVar &Value) const {
    return Value.Kind == MedVar::Reg && Value.RegOff == TRI.IntReturnReg &&
           Value.Size == TRI.FullRegWidth;
  }

  std::optional<MedVar> scan(size_t Block, size_t Index) {
    const MedBlock &B = Func.Blocks[Block];
    for (size_t I = Index; I-- > 0;) {
      const MedOp &Op = B.Ops[I];
      if (keptAcross(Op))
        continue;
      if (holds(Op.Output))
        return Op.Output;
      if (const auto Other =
              llvm::find_if(Op.IntrinsicOutputs,
                            [&](const MedVar &Output) {
                              return Output.Kind == MedVar::Reg &&
                                     Output.RegOff == TRI.IntReturnReg;
                            });
          Other != Op.IntrinsicOutputs.end())
        return holds(*Other) ? std::optional<MedVar>(*Other) : std::nullopt;
      if (Op.Opcode != NdOp::CALL && Op.Opcode != NdOp::INDIR_CALL)
        continue;
      // A call that may write the register leaves what its callee left.
      if (Op.PreservesCallerSaved)
        continue;
      return givenBy(Op);
    }
    for (const PhiNode &Phi : B.Phis)
      if (holds(Phi.Output))
        return Phi.Output;
    if (B.Preds.empty() || !B.ExceptionalPreds.empty())
      return std::nullopt;
    // No PHI: every path brings one value, or the register was dead here.
    std::optional<MedVar> Common;
    for (const int Pred : B.Preds) {
      const auto From = IndexOf.find(Pred);
      if (From == IndexOf.end())
        return std::nullopt;
      BlockState &S = State[From->second];
      // A cycle with no definition of its own brings what entered it.
      if (S.At == Progress::Visiting)
        continue;
      if (S.At == Progress::None) {
        // Every block is asked for once.
        if (++Visits > Func.Blocks.size())
          return std::nullopt;
        S.At = Progress::Visiting;
        S.AtEnd = scan(From->second, Func.Blocks[From->second].Ops.size());
        S.At = Progress::Done;
      }
      if (!S.AtEnd)
        return std::nullopt;
      if (Common && medValueKey(*Common) != medValueKey(*S.AtEnd))
        return std::nullopt;
      Common = S.AtEnd;
    }
    return Common;
  }

  const MedFunc &Func;
  const TargetRegInfo &TRI;
  std::map<int, size_t> IndexOf;
  std::vector<BlockState> State;
  size_t Visits = 0;
};
} // namespace

void keepCallersValueInReturnRegister(const BinaryImage &Img,
                                      PipelineResult &Result) {
  // On AArch64 the return register is also the first argument's: a callee
  // that never writes it hands that argument back, which the return
  // contracts settle (ReturnedValue::Incoming), and the lift already gives
  // such a call no output where the callee writes no vector register.
  if (Img.Arch != Arch::X64)
    return;
  const TargetRegInfo &TRI = getTargetRegInfo(Img.Arch);
  for (MedFunc &MF : Result.MedFuncs) {
    struct Site {
      size_t Block, Index;
      MedVar Given;
    };
    std::vector<Site> Sites;
    {
      const ValueOfReturnRegister Values(MF, TRI);
      for (size_t B = 0; B < MF.Blocks.size(); ++B)
        for (size_t I = 0; I < MF.Blocks[B].Ops.size(); ++I)
          if (const MedOp &Op = MF.Blocks[B].Ops[I]; Values.keptAcross(Op))
            if (const std::optional<MedVar> Given = Values.givenBy(Op))
              Sites.push_back({B, I, *Given});
    }
    if (Sites.empty())
      continue;
    // What each such call gave is the value the register held before it.
    ValueOfReturnRegister Values(MF, TRI);
    MedValueMap<MedVar> Kept;
    for (const Site &S : Sites)
      if (const std::optional<MedVar> Before = Values.before(S.Block, S.Index))
        Kept.try_emplace(medValueKey(S.Given), *Before);
    if (Kept.empty())
      continue;
    // The value before one such call may be what an earlier one gave.
    const auto Resolved = [&](MedVar Value) {
      for (size_t Step = 0; Step <= Kept.size(); ++Step) {
        const auto Next = Kept.find(medValueKey(Value));
        if (Next == Kept.end())
          break;
        Value = Next->second;
      }
      return Value;
    };
    const auto Rewrite = [&](MedVar &Use) {
      if (Use.isConst() || !Kept.count(medValueKey(Use)))
        return;
      MedVar Value = Resolved(Use);
      // A read of a view keeps its width.
      if (Use.Size && Use.Size < Value.Size)
        Value.Size = Use.Size;
      Use = Value;
    };
    for (MedBlock &Block : MF.Blocks) {
      for (PhiNode &Phi : Block.Phis)
        for (auto &Arm : Phi.Args)
          Rewrite(Arm.second);
      for (MedOp &Op : Block.Ops)
        for (uint8_t I = 0; I < Op.NumInputs; ++I)
          Rewrite(Op.Inputs[I]);
    }
    for (const Site &S : Sites) {
      if (!Kept.count(medValueKey(S.Given)))
        continue;
      MedOp &Call = MF.Blocks[S.Block].Ops[S.Index];
      if (medValueKey(Call.Output) == medValueKey(S.Given)) {
        Call.Output = MedVar{};
        Call.Output.Id = -1;
        Call.Output.Size = 0;
      }
      llvm::erase_if(MF.CallClobbers, [&](const MedCallClobber &Clobber) {
        return Clobber.CallSiteId == Call.CallSiteId &&
               medValueKey(Clobber.Value) == medValueKey(S.Given);
      });
    }
  }
}

} // namespace neverd
