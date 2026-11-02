//===- LLVMDataFlow.cpp - Read-only optimized integer SSA projection ------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Preserve exact typed operations and direct emission observations. Memory,
/// ordinary calls and unmodelled annotations are explicit admission failures.
//===----------------------------------------------------------------------===//

#include "neverd/diff/LLVMDataFlow.h"

#include "FlowParameterViews.h"
#include "FlowProgress.h"
#include "LLVMFlowSemantics.h"

#include "neverd/backend/ExceptionRewriteContract.h"
#include "neverd/backend/ParameterSites.h"
#include "neverd/backend/RewriteSourceIdentity.h"
#include "neverd/backend/llvm/LLVMSourceMap.h"

#include "llvm/ADT/STLExtras.h"
#include "llvm/Config/llvm-config.h"
#include "llvm/IR/Instructions.h"
#include "llvm/IR/IntrinsicInst.h"
#include "llvm/IR/Verifier.h"

#include <map>

namespace neverd::diff {
namespace {
using namespace llvm;
using llvm::Function;
using llvm::Instruction;
class Projection {
  const Function &F;
  const LLVMSourceMap &Sources;
  const DataFlowOptions &Options;
  const ComparisonControl &Control;
  const bool ParameterViews;
  const StringRef ProgressProfile;
  DataFlowGraph G;
  uint64_t Work = 0;
  std::map<const BasicBlock *, uint32_t> Blocks;
  std::map<const Value *, uint32_t> Nodes;
  std::map<const Value *, uint64_t> Unknowns;
  std::optional<std::vector<parameter_sites::Site>> Sites;
  std::string FunctionContract;
  SmallVector<StringRef, 32> MetadataNames;

  void poll() {
    if (auto Stop = Control.stop())
      throw DataFlowInterrupted(*Stop);
  }
  void charge(uint64_t Amount = 1) {
    if (Amount > Options.MaxWork - Work)
      throw DataFlowInterrupted(ComparisonState::ResourceLimited);
    const auto Before = Work;
    Work += Amount;
    if (Before / limits::diff::kFlowPollWork !=
        Work / limits::diff::kFlowPollWork)
      poll();
  }
  [[noreturn]] void unsupported(const Twine &Reason) const {
    throw LLVMDataFlowUnsupported(Reason.str());
  }
  void signature(const std::string &S) {
    if (S.size() > limits::diff::kMaxFlowSemanticsBytes)
      throw DataFlowInterrupted(ComparisonState::ResourceLimited);
    charge(S.size());
  }
  unsigned bits(Type *T) const {
    if (T->isVoidTy())
      return 0;
    if (!T->isIntegerTy() || T->getIntegerBitWidth() > 64)
      unsupported("only scalar integer values up to 64 bits are admitted");
    return T->getIntegerBitWidth();
  }
  std::string attributes(AttributeSet Set) {
    for (auto A : Set) {
      charge();
      if (A.isTypeAttribute())
        unsupported("typed ABI attributes are not admitted");
      if (A.isStringAttribute()) {
        if (A.getKindAsString().size() + A.getValueAsString().size() >
            limits::diff::kMaxFlowSemanticsBytes)
          throw DataFlowInterrupted(ComparisonState::ResourceLimited);
        charge(A.getKindAsString().size() + A.getValueAsString().size());
      }
    }
    auto Text = Set.getAsString();
    signature(Text);
    return Text;
  }
  void metadata(const Value &V) {
    SmallVector<std::pair<unsigned, MDNode *>, 8> Records;
    if (auto *I = dyn_cast<Instruction>(&V))
      I->getAllMetadata(Records);
    else
      cast<Function>(V).getAllMetadata(Records);
    if (Records.empty())
      return;
    charge(Records.size());
    if (MetadataNames.empty()) {
      F.getContext().getMDKindNames(MetadataNames);
      charge(MetadataNames.size());
    }
    for (const auto &[Kind, Node] : Records) {
      charge(1 + Node->getNumOperands());
      const auto Name = MetadataNames[Kind];
      charge(Name.size());
      if (Name == "dbg" || Name == "prof" || Name == "annotation" ||
          Name == "unpredictable")
        continue;
      if (isa<Function>(V)) {
        if (Name == rewrite_source::FunctionAttachment) {
          auto Entry = rewrite_source::getOriginalVA(F);
          if (!Entry)
            unsupported(toString(Entry.takeError()));
          if (!*Entry || **Entry != G.Function)
            unsupported(
                "function source identity does not match the native entry");
          continue;
        }
        if (Name == parameter_sites::FunctionAttachment) {
          Sites = parameter_sites::getParameterSites(F);
          if (!Sites)
            unsupported("invalid native parameter-site metadata");
          continue;
        }
        if (Name == exception_rewrite::FunctionAttachment) {
          if (!exception_rewrite::hasUnloweredContract(
                  F, exception_rewrite::SourceState::Absent))
            unsupported("native exception contract is outside this profile");
          continue;
        }
      } else if (Name == "neverd.return.explicit-memory-value" &&
                 isa<ReturnInst>(V) && !Node->getNumOperands()) {
        // An emitter provenance marker, not an additional LLVM operation.
        continue;
      }
      unsupported("unmodelled metadata: " + Name);
    }
  }
  FlowOperand operand(const Value &V) {
    charge();
    FlowOperand Out;
    const auto Width = bits(V.getType());
    if (!Width)
      unsupported("void operand");
    Out.Bytes = (Width + 7) / 8;
    Out.Semantics = "i" + std::to_string(Width);
    if (const auto *A = dyn_cast<Argument>(&V)) {
      if (A->getParent() != &F)
        unsupported("foreign argument");
      Out.Type = FlowOperand::Kind::Parameter;
      Out.Value = A->getArgNo();
      Out.Semantics +=
          ";attrs=" +
          attributes(F.getAttributes().getParamAttrs(A->getArgNo()));
      if (Sites)
        Out.Semantics += ";native-register=" +
                         std::to_string((*Sites)[A->getArgNo()].RegOff);
    } else if (const auto *C = dyn_cast<ConstantInt>(&V)) {
      Out.Type = FlowOperand::Kind::Constant;
      Out.Value = C->getZExtValue();
    } else if (isa<PoisonValue, UndefValue>(V)) {
      Out.Type = FlowOperand::Kind::Unknown;
      Out.Value = Unknowns.try_emplace(&V, Unknowns.size()).first->second;
      Out.Semantics += isa<PoisonValue>(V) ? ";poison" : ";undef";
    } else if (auto At = Nodes.find(&V); At != Nodes.end()) {
      Out.Type = FlowOperand::Kind::Node;
      Out.Node = At->second;
    } else {
      unsupported("unmodelled or foreign operand");
    }
    signature(Out.Semantics);
    return Out;
  }
  void instruction(const Instruction &I, FlowNode &N) {
    metadata(I);
    N.Opcode = I.getOpcodeName();
    N.ResultBytes = (bits(I.getType()) + 7) / 8;
    N.Semantics = FunctionContract + ";result=i" +
                  std::to_string(bits(I.getType())) +
                  ";flags=" + std::to_string(I.getRawSubclassOptionalData());
    N.Commutative = I.isCommutative();
    const auto Add = [&](const Value &V) {
      N.Inputs.push_back(operand(V));
      if (N.Inputs.back().Type == FlowOperand::Kind::Unknown)
        N.Gaps |= FlowUnknownInput;
    };
    if (const auto *P = dyn_cast<PHINode>(&I)) {
      N.Phi = true;
      for (unsigned K = 0; K < P->getNumIncomingValues(); ++K) {
        Add(*P->getIncomingValue(K));
        N.Inputs.back().Predecessor = Blocks.at(P->getIncomingBlock(K));
      }
    } else if (const auto *B = dyn_cast<BranchInst>(&I)) {
      if (B->isConditional())
        Add(*B->getCondition());
    } else if (const auto *S = dyn_cast<SwitchInst>(&I)) {
      Add(*S->getCondition());
      for (const auto &Case : S->cases())
        Add(*Case.getCaseValue());
      N.Semantics += ";default=0;case-successors=1..n";
    } else if (const auto *R = dyn_cast<ReturnInst>(&I)) {
      if (R->getReturnValue())
        Add(*R->getReturnValue());
      N.Semantics +=
          ";return-attrs=" + attributes(F.getAttributes().getRetAttrs());
    } else if (const auto *C = dyn_cast<IntrinsicInst>(&I)) {
      if (intrinsicFlowShape(C->getIntrinsicID()) ==
              LLVMFlowShape::Unsupported ||
          C->hasOperandBundles())
        unsupported("unmodelled intrinsic or operand bundle");
      N.Opcode = C->getCalledFunction()->getName().str();
      N.Semantics += ";cc=" + std::to_string(C->getCallingConv()) +
                     ";tail=" + std::to_string(C->getTailCallKind());
      for (const auto &Attrs :
           {C->getAttributes(), C->getCalledFunction()->getAttributes()}) {
        N.Semantics += ";call-fn=" + attributes(Attrs.getFnAttrs());
        N.Semantics += ";call-ret=" + attributes(Attrs.getRetAttrs());
        for (unsigned K = 0; K < C->arg_size(); ++K)
          N.Semantics += ";call-arg=" + attributes(Attrs.getParamAttrs(K));
      }
      for (const auto &A : C->args())
        Add(*A);
    } else {
      if (integerFlowShape(N.Opcode) == LLVMFlowShape::Unsupported)
        unsupported("unmodelled operation: " + StringRef(I.getOpcodeName()));
      if (const auto *Cmp = dyn_cast<ICmpInst>(&I))
        N.Semantics += ";predicate=" + std::to_string(Cmp->getPredicate());
      if (isa<FreezeInst>(I))
        N.Gaps |= FlowUnknownInput;
      for (const auto &V : I.operands())
        Add(*V);
    }
    signature(N.Semantics);
  }

  void projectParameters() {
    // The raw graph, metadata and direct observations have already passed
    // admission. Never make unsupported annotations disappear by elision.
    if (F.arg_size() > Options.MaxNodes)
      throw DataFlowInterrupted(ComparisonState::ResourceLimited);
    charge(F.arg_size());
    for (const auto &A : F.args()) {
      FlowParameterView V;
      V.SourceBits = V.ViewBits = bits(A.getType());
      V.Attributes = attributes(F.getAttributes().getParamAttrs(A.getArgNo()));
      if (Sites) {
        const auto &S = (*Sites)[A.getArgNo()];
        V.NativeSite = FlowParameterSite{S.RegOff, S.MedIndex};
      }
      G.ParameterViews.push_back(std::move(V));
    }
    const auto Unambiguous = unambiguousParameterSites(
        G.ParameterViews, [&](uint64_t N) { charge(N); });
    charge(G.Nodes.size());
    std::vector<uint32_t> Parameters(G.Nodes.size(), NoIndex);
    uint32_t Removed = 0;
    for (const auto &A : F.args()) {
      charge();
      auto &V = G.ParameterViews[A.getArgNo()];
      if (V.SourceBits != 64 || !V.Attributes.empty() ||
          !Unambiguous[A.getArgNo()] || A.use_empty())
        continue;
      bool Eligible = true;
      uint32_t Count = 0;
      for (const auto &Use : A.uses()) {
        charge();
        const auto *T = dyn_cast<TruncInst>(Use.getUser());
        if (!T || !Nodes.contains(T) || !T->getType()->isIntegerTy(32) ||
            T->getRawSubclassOptionalData() ||
            G.Nodes[Nodes.at(T)].hasNativeOrigin())
          Eligible = false;
        ++Count;
      }
      if (!Eligible)
        continue;
      V.ViewBits = 32;
      V.ElidedTruncations = Count;
      Removed += Count;
      for (const auto &Use : A.uses()) {
        charge();
        Parameters[Nodes.at(Use.getUser())] = A.getArgNo();
      }
    }
    if (G.Nodes.size() - Removed + G.ParameterViews.size() > Options.MaxNodes)
      throw DataFlowInterrupted(ComparisonState::ResourceLimited);
    charge(G.Nodes.size());
    std::vector<uint32_t> Remap(G.Nodes.size(), NoIndex);
    uint32_t Next = 0;
    for (uint32_t I = 0; I < G.Nodes.size(); ++I) {
      charge();
      if (Parameters[I] == NoIndex)
        Remap[I] = Next++;
    }
    std::vector<FlowNode> Kept;
    Kept.reserve(Next);
    for (uint32_t I = 0; I < G.Nodes.size(); ++I) {
      charge();
      if (Parameters[I] != NoIndex)
        continue;
      auto &N = G.Nodes[I];
      for (auto &Input : N.Inputs) {
        charge();
        if (Input.Type == FlowOperand::Kind::Node) {
          if (Parameters[Input.Node] != NoIndex) {
            const auto Parameter = Parameters[Input.Node];
            const auto Predecessor = Input.Predecessor;
            Input = {};
            Input.Type = FlowOperand::Kind::Parameter;
            Input.Value = Parameter;
            Input.Predecessor = Predecessor;
          } else {
            Input.Node = Remap[Input.Node];
          }
        }
        if (Input.Type == FlowOperand::Kind::Parameter) {
          const auto &V = G.ParameterViews[Input.Value];
          Input.Bytes = (V.ViewBits + 7) / 8;
          Input.Semantics = parameterViewSemantics(V);
          signature(Input.Semantics);
        }
      }
      Kept.push_back(std::move(N));
    }
    G.Nodes = std::move(Kept);
    for (auto &B : G.Blocks) {
      charge(1 + B.Successors.size());
      B.Terminator = Remap[B.Terminator];
    }
    G.Profile = LLVMParameterDataFlowProfile.str();
    // Reserve validation work from the same projection allowance, including
    // the native-site sort. No second projection budget starts after removal.
    if (auto Error =
            validateParameterViews(G, Options, [&](uint64_t N) { charge(N); }))
      throw std::invalid_argument(*Error);
    for (const auto &N : G.Nodes)
      charge(1 + N.Inputs.size() + flowExtraWork(N));
    if (auto Error = G.validate(Options))
      throw std::invalid_argument(*Error);
  }

  void projectProgress() {
    auto P = proveFlowProgress(
        G, FunctionContract, Options, [&](uint64_t N) { charge(N); },
        flowProgressMode(ProgressProfile));
    if (P.Kind != FlowProgressKind::Unproven) {
      // Add enum attributes to a copy; strings may contain arbitrary attribute
      // names or delimiters. Source LLVM and all other contracts stay exact.
      auto Attrs = F.getAttributes().getFnAttrs();
      Attrs = Attrs.addAttribute(F.getContext(), Attribute::MustProgress);
      Attrs = Attrs.addAttribute(F.getContext(), Attribute::WillReturn);
      P.ObservedContract = "llvm=" LLVM_VERSION_STRING ";cc=" +
                           std::to_string(F.getCallingConv()) +
                           ";function-attrs=" + attributes(Attrs);
      signature(P.ObservedContract);
      for (auto &N : G.Nodes) {
        charge();
        N.Semantics.replace(0, FunctionContract.size(), P.ObservedContract);
        signature(N.Semantics);
      }
    }
    G.Profile = ProgressProfile.str();
    G.Progress = std::move(P);
    if (auto Error =
            validateFlowProgress(G, Options, [&](uint64_t N) { charge(N); }))
      throw std::invalid_argument(*Error);
    // Structure and parameters were validated before proof. Only the verified
    // prefix and bounded progress record changed; their checks above share
    // this projection's allowance, without another independent proof budget.
  }

public:
  Projection(const Function &F, const LLVMSourceMap &Sources, uint64_t Entry,
             StringRef Revision, StringRef ABI, bool NativeComplete,
             const DataFlowOptions &Options, const ComparisonControl &Control,
             bool ParameterViews = false, StringRef ProgressProfile = {})
      : F(F), Sources(Sources), Options(Options), Control(Control),
        ParameterViews(ParameterViews), ProgressProfile(ProgressProfile) {
    G.Profile = LLVMDataFlowProfile.str();
    G.Function = Entry;
    G.Revision = Revision.str();
    G.ABI = ABI.str();
    G.Gaps = NativeComplete ? 0 : FlowNativeCoverage;
  }
  DataFlowGraph run() {
    if (auto Error = Options.validate())
      throw std::invalid_argument(*Error);
    poll();
    auto Owner = Sources.Functions.find(G.Function);
    if (!F.getParent() || F.empty() || F.isVarArg() || F.hasPersonalityFn() ||
        Owner == Sources.Functions.end() ||
        static_cast<Value *>(Owner->second) != &F)
      unsupported(
          "missing native function binding, body or supported signature");
    bits(F.getReturnType());
    for (const auto &A : F.args()) {
      charge();
      bits(A.getType());
    }
    for (const auto &B : F) {
      charge();
      if (G.Blocks.size() == Options.MaxBlocks)
        throw DataFlowInterrupted(ComparisonState::ResourceLimited);
      Blocks.emplace(&B, G.Blocks.size());
      G.Blocks.emplace_back();
      for (const auto &I : B) {
        charge(1 + I.getNumOperands());
        if (G.Nodes.size() == Options.MaxNodes)
          throw DataFlowInterrupted(ComparisonState::ResourceLimited);
        Nodes.emplace(&I, G.Nodes.size());
        G.Nodes.emplace_back();
      }
    }
    // The caller supplies verified machine IR. This defensive LLVM verifier
    // also visits metadata/attributes and, like pipeline optimization, is not
    // an interruptible projection step. The SDK bounds native function jobs.
    if (verifyFunction(F))
      unsupported("LLVM verification failed");
    metadata(F);
    FunctionContract = "llvm=" LLVM_VERSION_STRING ";cc=" +
                       std::to_string(F.getCallingConv()) + ";function-attrs=" +
                       attributes(F.getAttributes().getFnAttrs());
    signature(FunctionContract);
    G.EntryBlock = Blocks.at(&F.getEntryBlock());
    for (const auto &B : F) {
      auto &Block = G.Blocks[Blocks.at(&B)];
      Block.Terminator = Nodes.at(B.getTerminator());
      for (unsigned K = 0; K < B.getTerminator()->getNumSuccessors(); ++K) {
        charge();
        Block.Successors.push_back(
            Blocks.at(B.getTerminator()->getSuccessor(K)));
      }
      for (const auto &I : B) {
        charge();
        auto &N = G.Nodes[Nodes.at(&I)];
        N.Block = Blocks.at(&B);
        instruction(I, N);
      }
    }
    for (const auto &O : Sources.Observations) {
      charge();
      if (O.Function != G.Function || O.ExpressionDerived)
        continue;
      const auto *I = dyn_cast_or_null<Instruction>(O.Value);
      if (!I)
        continue; // Deleted or folded to a constant/argument: no invented node.
      const auto At = Nodes.find(I);
      if (At == Nodes.end() || O.Occurrence.Sequence < 0)
        unsupported("invalid direct native observation");
      G.Nodes[At->second].Origins.push_back(
          {O.Occurrence.Address, O.Occurrence.Sequence});
    }
    for (auto &N : G.Nodes) {
      llvm::sort(N.Origins, [&](const auto &A, const auto &B) {
        charge();
        return A < B;
      });
      N.Origins.erase(std::unique(N.Origins.begin(), N.Origins.end(),
                                  [&](const auto &A, const auto &B) {
                                    charge();
                                    return A == B;
                                  }),
                      N.Origins.end());
      charge(1 + N.Inputs.size() + flowExtraWork(N));
    }
    if (auto Error = G.validate(Options))
      throw std::invalid_argument(*Error);
    if (ParameterViews)
      projectParameters();
    if (!ProgressProfile.empty())
      projectProgress();
    poll();
    return std::move(G);
  }
};
} // namespace
DataFlowGraph buildLLVMDataFlow(const llvm::Function &F,
                                const LLVMSourceMap &Sources, uint64_t Entry,
                                llvm::StringRef Revision, llvm::StringRef ABI,
                                bool NativeComplete,
                                const DataFlowOptions &Options,
                                const ComparisonControl &Control) {
  return Projection(F, Sources, Entry, Revision, ABI, NativeComplete, Options,
                    Control)
      .run();
}
DataFlowGraph buildLLVMParameterDataFlow(
    const llvm::Function &F, const LLVMSourceMap &Sources, uint64_t Entry,
    llvm::StringRef Revision, llvm::StringRef ABI, bool NativeComplete,
    const DataFlowOptions &Options, const ComparisonControl &Control) {
  return Projection(F, Sources, Entry, Revision, ABI, NativeComplete, Options,
                    Control, true)
      .run();
}
DataFlowGraph buildLLVMProgressDataFlow(
    const llvm::Function &F, const LLVMSourceMap &Sources, uint64_t Entry,
    llvm::StringRef Revision, llvm::StringRef ABI, bool NativeComplete,
    const DataFlowOptions &Options, const ComparisonControl &Control) {
  return Projection(F, Sources, Entry, Revision, ABI, NativeComplete, Options,
                    Control, true, LLVMProgressDataFlowProfile)
      .run();
}
DataFlowGraph buildLLVMMultiBlockProgressDataFlow(
    const llvm::Function &F, const LLVMSourceMap &Sources, uint64_t Entry,
    llvm::StringRef Revision, llvm::StringRef ABI, bool NativeComplete,
    const DataFlowOptions &Options, const ComparisonControl &Control) {
  return Projection(F, Sources, Entry, Revision, ABI, NativeComplete, Options,
                    Control, true, LLVMMultiBlockProgressDataFlowProfile)
      .run();
}
DataFlowGraph buildLLVMNestedProgressDataFlow(
    const llvm::Function &F, const LLVMSourceMap &Sources, uint64_t Entry,
    llvm::StringRef Revision, llvm::StringRef ABI, bool NativeComplete,
    const DataFlowOptions &Options, const ComparisonControl &Control) {
  return Projection(F, Sources, Entry, Revision, ABI, NativeComplete, Options,
                    Control, true, LLVMNestedProgressDataFlowProfile)
      .run();
}
DataFlowGraph buildLLVMBranchProgressDataFlow(
    const llvm::Function &F, const LLVMSourceMap &Sources, uint64_t Entry,
    llvm::StringRef Revision, llvm::StringRef ABI, bool NativeComplete,
    const DataFlowOptions &Options, const ComparisonControl &Control) {
  return Projection(F, Sources, Entry, Revision, ABI, NativeComplete, Options,
                    Control, true, LLVMBranchProgressDataFlowProfile)
      .run();
}
DataFlowGraph buildLLVMModularProgressDataFlow(
    const llvm::Function &F, const LLVMSourceMap &Sources, uint64_t Entry,
    llvm::StringRef Revision, llvm::StringRef ABI, bool NativeComplete,
    const DataFlowOptions &Options, const ComparisonControl &Control) {
  return Projection(F, Sources, Entry, Revision, ABI, NativeComplete, Options,
                    Control, true, LLVMModularProgressDataFlowProfile)
      .run();
}
DataFlowGraph buildLLVMParameterBoundProgressDataFlow(
    const llvm::Function &F, const LLVMSourceMap &Sources, uint64_t Entry,
    llvm::StringRef Revision, llvm::StringRef ABI, bool NativeComplete,
    const DataFlowOptions &Options, const ComparisonControl &Control) {
  return Projection(F, Sources, Entry, Revision, ABI, NativeComplete, Options,
                    Control, true, LLVMParameterBoundProgressDataFlowProfile)
      .run();
}
DataFlowGraph buildLLVMInvariantBoundProgressDataFlow(
    const llvm::Function &F, const LLVMSourceMap &Sources, uint64_t Entry,
    llvm::StringRef Revision, llvm::StringRef ABI, bool NativeComplete,
    const DataFlowOptions &Options, const ComparisonControl &Control) {
  return Projection(F, Sources, Entry, Revision, ABI, NativeComplete, Options,
                    Control, true, LLVMInvariantBoundProgressDataFlowProfile)
      .run();
}
} // namespace neverd::diff
