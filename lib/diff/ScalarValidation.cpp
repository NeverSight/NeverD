//===- ScalarValidation.cpp - Bound lifted scalar models ------------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Retains native provenance separately from a read-only scalar proof query.
//===----------------------------------------------------------------------===//

#include "neverd/diff/ScalarValidation.h"

#include "neverd/backend/ExceptionRewriteContract.h"
#include "neverd/backend/ParameterSites.h"
#include "neverd/backend/RewriteSourceIdentity.h"
#include "neverd/backend/llvm/LLVMCallContract.h"
#include "neverd/backend/llvm/WindowsEHMetadata.h"
#include "neverd/backend/llvm/WindowsEHMetadataEncoder.h"
#include "neverd/backend/llvm/WindowsEHNativeSource.h"
#include "neverd/lift/X86/X86Regs.h"
#include "neverd/loader/ExceptionFunction.h"

#include "llvm/ADT/STLExtras.h"
#include "llvm/IR/Instructions.h"
#include "llvm/IR/Module.h"
#include "llvm/Transforms/Utils/Cloning.h"

#include <set>
#include <stdexcept>

namespace neverd::diff {
using Status = analysis::LLVMScalarEquivalenceStatus;

std::optional<std::string> ScalarValidationOptions::validate() const {
  if (!MaxNativeInstructions ||
      MaxNativeInstructions > limits::diff::kMaxValidationNativeInstructions ||
      !MaxWork || MaxWork > limits::diff::kMaxValidationWork)
    return "scalar validation limits are outside their allowance";
  return {};
}

llvm::StringRef scalarValidationStatusName(Status S) {
  switch (S) {
  case Status::Proved:
    return "proved";
  case Status::Unsupported:
    return "unsupported";
  case Status::Unproved:
    return "unproved";
  case Status::BudgetExceeded:
    return "budget_exceeded";
  case Status::Cancelled:
    return "cancelled";
  }
  return "unsupported";
}

std::optional<std::string> scalarValidationNativeGap(const Program &P,
                                                     uint32_t Index) {
  const auto &ID = P.Identity;
  if (ID.Architecture != "x86-64" || ID.Bits != 64 || ID.Endian != "little" ||
      ID.ABI.empty() || !isNativeX86Normalization(ID.Normalization))
    return "scalar validation requires an x64 native ABI profile";
  if (Index >= P.Functions.size())
    return "native function index is absent";
  const auto &F = P.Functions[Index];
  if ((F.Traits & TraitIncomplete) || !P.instructionCount(F))
    return "native function coverage is incomplete or empty";
  for (const auto &B : P.blocks(F))
    for (const auto &I : P.instructions(B))
      if (I.Callee != NoIndex || I.UnresolvedCallee ||
          I.Flow == InstructionFlow::Call ||
          I.Flow == InstructionFlow::IndirectCall ||
          I.Flow == InstructionFlow::IndirectJump ||
          I.Flow == InstructionFlow::Unknown)
        return "native calls, external transfers or unknown flow are "
               "unsupported";
  for (const auto &E : P.edges(F))
    if (E.Kind == EdgeKind::Exception)
      return "native exception edges are unsupported";
  return {};
}

namespace {
struct Refusal {
  Status Code;
  std::string Message;
};
void require(bool OK, llvm::StringRef Why) {
  if (!OK)
    throw Refusal{Status::Unsupported, Why.str()};
}
void poll(const ComparisonControl &Control) {
  if (Control.stop())
    throw Refusal{Status::Cancelled, "scalar validation stopped"};
}

void bindExceptions(const Program &P, const Function &Native,
                    const ScalarValidationLift &Lift,
                    const ComparisonControl &Control) {
  using namespace exception_rewrite;
  const auto &F = Lift.Function;
  if (!Lift.Exceptions) {
    require(hasUnloweredContract(F, SourceState::Absent) &&
                !F.getMetadata(windows_eh_md::FunctionAttachment),
            "absent source exceptions require an absent lift contract");
    return;
  }
  const auto &EH = *Lift.Exceptions;
  require(EH.describesUnwindOnly(),
          "only complete source unwind-only records are supported");
  require(hasUnloweredContract(F, SourceState::Complete),
          "unwind-only source requires complete, unlowered exception metadata");
  require((Lift.ABI == BinaryFormat::ELF || Lift.ABI == BinaryFormat::COFF ||
           Lift.ABI == BinaryFormat::MachO) &&
              P.Identity.ABI ==
                  std::to_string(static_cast<unsigned>(Lift.ABI)) + ":" +
                      P.Identity.Architecture,
          "unwind source ABI does not match snapshot");

  // Bound all variable-size fields consumed by the canonical encoder before
  // re-encoding. Language-table fields have already been excluded above. This
  // is a separate finite allowance, like the subsequent function projection.
  const analysis::LLVMInterpreterModelLimits Limits;
  uint64_t Items = 0;
  const auto Charge = [&](uint64_t N) {
    poll(Control);
    if (N > Limits.MaxInputItems || Items > Limits.MaxInputItems - N)
      throw Refusal{Status::BudgetExceeded,
                    "unwind projection input limit exceeded"};
    Items += N;
  };
  Charge(EH.NativeUnwindBytes.size());
  Charge(EH.Epilogs.size());
  Charge(EH.FragmentRanges.size());
  Charge(EH.Diagnostics.size());
  const auto ChargeOperations = [&](const auto &Operations) {
    Charge(Operations.size());
    for (const auto &Op : Operations)
      Charge(Op.OperandBytes.size());
  };
  ChargeOperations(EH.UnwindOperations);
  for (const auto &Epilog : EH.Epilogs)
    ChargeOperations(Epilog.Operations);
  for (const auto &Message : EH.Diagnostics)
    Charge(Message.size());
  if (EH.DecodeProvenance)
    for (const auto *Part :
         {&EH.DecodeProvenance->Structural, &EH.DecodeProvenance->Language}) {
      Charge(Part->Diagnostics.size());
      for (const auto &Message : Part->Diagnostics)
        Charge(Message.size());
    }
  // The canonical encoder's classifier checks that structural/language parse
  // contributions agree with the compatibility summary. Reuse that decision;
  // NoLanguagePersonality grants no native regeneration capability.
  require(classifyWindowsEHNativeSource(EH, Arch::X64, Lift.ABI).Reason ==
              WindowsEHNativeSourceReason::NoLanguagePersonality,
          "unwind source parse provenance is inconsistent");
  require((EH.FunctionEntry ? EH.FunctionEntry : EH.CodeRange.Begin) ==
              Native.Address,
          "unwind source does not own the original function entry");
  for (const auto &B : P.blocks(Native))
    for (const auto &I : P.instructions(B)) {
      Charge(1 + EH.FragmentRanges.size());
      require(
          I.Size && I.Address <= InvalidVA - I.Size &&
              EH.ownsCode(ExceptionAddressRange{I.Address, I.Address + I.Size}),
          "unwind source does not cover the complete native body");
    }
  require(F.getMetadata(windows_eh_md::FunctionAttachment) ==
              windows_eh_md::getCanonicalFunctionMetadata(F.getContext(), EH,
                                                          Arch::X64, Lift.ABI),
          "lifted unwind metadata does not match its canonical source");
}

std::vector<ScalarValidationArgument>
binding(const Program &P, uint32_t Index, const ScalarValidationLift &Lift,
        const ScalarValidationOptions &Options,
        const ComparisonControl &Control) {
  require(Index < P.Functions.size(), "native function index is absent");
  const auto &Native = P.Functions[Index];
  const auto &F = Lift.Function;
  if (P.instructionCount(Native) > Options.MaxNativeInstructions)
    throw Refusal{Status::BudgetExceeded, "native instruction limit exceeded"};
  if (auto Gap = scalarValidationNativeGap(P, Index))
    throw Refusal{Status::Unsupported, *Gap};
  poll(Control);
  auto Original = rewrite_source::getOriginalVA(F);
  if (!Original)
    throw Refusal{Status::Unsupported, llvm::toString(Original.takeError())};
  require(*Original == Native.Address,
          "LLVM original entry does not match snapshot");
  require(F.getParent() && !F.isDeclaration() && !F.isVarArg(),
          "a nonvariadic lifted definition is required");
  bindExceptions(P, Native, Lift, Control);
  require(F.arg_size() <=
              (x86reg::R15 - x86reg::RAX) / x86reg::GeneralRegStride + 1,
          "too many scalar register parameters");
  const auto Sites = parameter_sites::getParameterSites(F);
  require(Sites.has_value(), "lifted parameter locations are unavailable");
  const auto Banks = F.getFnAttribute(LLVMArgumentBanksAttribute);
  require(Banks.isStringAttribute() &&
              Banks.getValueAsString() == std::to_string(F.arg_size()) + ",0",
          "only a complete integer register bank is supported");
  std::set<uint64_t> Seen;
  std::vector<ScalarValidationArgument> Arguments;
  for (const auto &A : F.args()) {
    const auto &S = (*Sites)[A.getArgNo()];
    require(A.getType()->isIntegerTy() && S.RegOff <= x86reg::R15 &&
                S.RegOff % x86reg::GeneralRegStride == 0 &&
                S.RegOff != x86reg::RSP && Seen.insert(S.RegOff).second,
            "parameters need distinct complete GPR locations");
    Arguments.push_back({S.RegOff, A.getType()->getIntegerBitWidth()});
  }
  return Arguments;
}

std::unique_ptr<llvm::Module> project(const ScalarValidationLift &Lift,
                                      const ComparisonControl &Control) {
  const auto &F = Lift.Function;
  require(F.getCallingConv() == llvm::CallingConv::C && !F.hasGC() &&
              !F.hasPersonalityFn() && !F.hasPrefixData() &&
              !F.hasPrologueData(),
          "unsupported lifted function execution contract");
  // Bound the copy independently of the subsequent model construction. Only
  // exact function attachments validated by binding() may be copied.
  const analysis::LLVMInterpreterModelLimits Limits;
  uint64_t Items = 0;
  const auto Charge = [&](uint64_t N) {
    poll(Control);
    if (N > Limits.MaxInputItems || Items > Limits.MaxInputItems - N)
      throw Refusal{Status::BudgetExceeded,
                    "scalar projection input limit exceeded"};
    Items += N;
  };
  Charge(F.arg_size() + F.size());
  Charge(F.getParent()->getDataLayoutStr().size());
  llvm::SmallVector<std::pair<unsigned, llvm::MDNode *>, 4> Metadata;
  F.getAllMetadata(Metadata);
  require(Metadata.size() == 3 + (Lift.Exceptions ? 1U : 0U),
          "unknown lifted function metadata");
  for (auto [Kind, Node] : Metadata) {
    (void)Node;
    const auto Matches = [&](llvm::StringRef Name) {
      return Kind == F.getContext().getMDKindID(Name);
    };
    require(Matches(rewrite_source::FunctionAttachment) ||
                Matches(parameter_sites::FunctionAttachment) ||
                Matches(exception_rewrite::FunctionAttachment) ||
                (Lift.Exceptions && Matches(windows_eh_md::FunctionAttachment)),
            "unknown lifted function metadata");
  }
  for (auto Set : F.getAttributes())
    for (auto A : Set) {
      Charge(1);
      if (A.isConstantRangeListAttribute())
        Charge(A.getValueAsConstantRangeList().size());
      if (A.isStringAttribute())
        Charge(A.getKindAsString().size() + A.getValueAsString().size());
    }
  for (const auto &B : F)
    for (const auto &I : B) {
      Charge(1 + I.getNumOperands());
      require(!I.hasMetadata(), "instruction metadata is outside this profile");
    }
  auto M = std::make_unique<llvm::Module>("scalar-validation", F.getContext());
  M->setDataLayout(F.getParent()->getDataLayout());
  M->setTargetTriple(F.getParent()->getTargetTriple());
  auto *Copy = llvm::Function::Create(
      F.getFunctionType(), llvm::GlobalValue::ExternalLinkage, "subject", *M);
  llvm::ValueToValueMapTy Map;
  for (const auto &A : F.args())
    Map[&A] = Copy->getArg(A.getArgNo());
  llvm::SmallVector<llvm::ReturnInst *, 4> Returns;
  llvm::CloneFunctionInto(
      Copy, &F, Map, llvm::CloneFunctionChangeType::DifferentModule, Returns);
  Copy->setMetadata(rewrite_source::FunctionAttachment, nullptr);
  Copy->setMetadata(parameter_sites::FunctionAttachment, nullptr);
  Copy->setMetadata(exception_rewrite::FunctionAttachment, nullptr);
  Copy->setMetadata(windows_eh_md::FunctionAttachment, nullptr);
  Copy->removeFnAttr(LLVMArgumentBanksAttribute);
  Copy->removeFnAttr(llvm::Attribute::NullPointerIsValid);
  poll(Control);
  return M;
}
} // namespace

ScalarValidationResult validateScalarFunctions(
    const Program &A, uint32_t AI, const ScalarValidationLift &Left,
    const Program &B, uint32_t BI, const ScalarValidationLift &Right,
    const ScalarValidationOptions &Options, const ComparisonControl &Control) {
  if (auto Error = Options.validate())
    throw std::invalid_argument(*Error);
  ScalarValidationResult R;
  R.Proof.InputDomain = analysis::LLVMScalarInputDomain::DefinedBitvectors;
  try {
    poll(Control);
    require(A.Identity.ABI == B.Identity.ABI,
            "scalar validation requires matching x64 native ABI profiles");
    auto AB = binding(A, AI, Left, Options, Control);
    auto BB = binding(B, BI, Right, Options, Control);
    require(AB.size() == BB.size(), "scalar argument counts differ");
    for (size_t I = 0; I < AB.size(); ++I)
      if (AB[I].RegisterOffset != BB[I].RegisterOffset ||
          AB[I].Bits != BB[I].Bits)
        throw Refusal{Status::Unsupported,
                      "argument " + std::to_string(I) + " differs: primary i" +
                          std::to_string(AB[I].Bits) + " at register offset " +
                          std::to_string(AB[I].RegisterOffset) +
                          ", secondary i" + std::to_string(BB[I].Bits) +
                          " at register offset " +
                          std::to_string(BB[I].RegisterOffset)};
    require(Left.Function.getReturnType()->isIntegerTy() &&
                Right.Function.getReturnType()->isIntegerTy() &&
                Left.Function.getReturnType()->getIntegerBitWidth() ==
                    Right.Function.getReturnType()->getIntegerBitWidth(),
            "scalar integer return widths differ or are unsupported");
    R.Arguments = std::move(AB);
    R.ResultBits = Left.Function.getReturnType()->getIntegerBitWidth();
    auto L = project(Left, Control), Q = project(Right, Control);
    analysis::LLVMScalarEquivalenceLimits Limits;
    Limits.MaxWork = Options.MaxWork;
    analysis::LLVMScalarModelOptions Domain;
    Domain.InputDomain = analysis::LLVMScalarInputDomain::DefinedBitvectors;
    Domain.StopRequested = [&] { return Control.stop().has_value(); };
    if (Control.Progress)
      Control.Progress("proof", 0, Options.MaxWork);
    R.Proof = analysis::checkLLVMScalarEquivalence(
        *L->getFunction("subject"), *Q->getFunction("subject"), Limits, Domain);
  } catch (const Refusal &F) {
    R.Proof.Status = F.Code;
    R.Proof.Diagnostic = F.Message;
  }
  if (R.Proof.Status == Status::BudgetExceeded)
    R.State = ComparisonState::ResourceLimited;
  if (auto Stop = Control.stop()) {
    R.State = *Stop;
    R.Proof.Status = Status::Cancelled;
    R.Proof.Diagnostic = "scalar validation stopped";
  }
  return R;
}
} // namespace neverd::diff
