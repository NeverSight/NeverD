//===- BinaryDiffScalarValidationTests.cpp - Scalar proofs ---------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Synthetic model associations isolate profile refusal; native lift coverage
/// and input rebinding are exercised independently through the SDK fixtures.
//===----------------------------------------------------------------------===//

#include "neverd/backend/ExceptionRewriteContract.h"
#include "neverd/backend/ParameterSites.h"
#include "neverd/backend/RewriteSourceIdentity.h"
#include "neverd/backend/llvm/LLVMCallContract.h"
#include "neverd/backend/llvm/WindowsEHMetadata.h"
#include "neverd/backend/llvm/WindowsEHMetadataEncoder.h"
#include "neverd/diff/ScalarValidation.h"
#include "neverd/lift/X86/X86Regs.h"
#include "neverd/loader/ExceptionFunction.h"

#include "llvm/AsmParser/Parser.h"
#include "llvm/IR/Module.h"
#include "llvm/Support/SourceMgr.h"
#include "llvm/Support/raw_ostream.h"

#include <gtest/gtest.h>

using namespace neverd;
using namespace neverd::diff;
using Status = analysis::LLVMScalarEquivalenceStatus;
namespace {
struct Subject {
  llvm::LLVMContext Context;
  std::unique_ptr<llvm::Module> Module;
  Program Native;
  std::optional<ExceptionFunction> Exceptions;
  BinaryFormat ABI = BinaryFormat::ELF;
  Subject(uint64_t Entry, llvm::StringRef Body) {
    ProgramIdentity Identity;
    Identity.Architecture = "x86-64";
    Identity.Bits = 64;
    Identity.Endian = "little";
    Identity.ABI = "0:x86-64";
    Identity.Normalization = "native-x86-operands-v1";
    Identity.Revision = std::to_string(Entry);
    ProgramBuilder P(Identity);
    P.beginFunction(Entry, 1, "subject", NameSource::Symbol, 0);
    P.addBlock(Entry);
    P.addInstruction(Entry, {0xc3}, "ret", 1, 1, InstructionFlow::Return);
    Native = P.finish();
    llvm::SMDiagnostic Error;
    Module = llvm::parseAssemblyString(
        "define i32 @subject(i32 %x) { " + Body.str() + " }", Error, Context);
    EXPECT_TRUE(Module);
    if (!Module)
      return;
    auto &F = function();
    rewrite_source::setOriginalVA(F, Entry);
    parameter_sites::setParameterSites(F, {{x86reg::RDI, 1}});
    exception_rewrite::setContract(
        F, exception_rewrite::SourceState::Absent,
        exception_rewrite::LoweringState::NotRequired);
    F.addFnAttr(LLVMArgumentBanksAttribute, "1,0");
    F.addFnAttr(llvm::Attribute::NullPointerIsValid);
  }
  llvm::Function &function() { return *Module->getFunction("subject"); }
  ScalarValidationLift lift() {
    return {function(), Exceptions ? &*Exceptions : nullptr, ABI};
  }
  void setUnwindOnly() {
    Exceptions.emplace();
    Exceptions->CodeRange = {Native.Functions[0].Address,
                             Native.Functions[0].Address + 1};
    Exceptions->Encoding = ExceptionEncoding::DwarfFDE;
    Exceptions->Dwarf.emplace();
    exception_rewrite::setContract(
        function(), exception_rewrite::SourceState::Complete,
        exception_rewrite::LoweringState::NotRequired);
    function().setMetadata(windows_eh_md::FunctionAttachment,
                           windows_eh_md::getCanonicalFunctionMetadata(
                               Context, *Exceptions, Arch::X64, ABI));
    function().setUWTableKind(llvm::UWTableKind::Default);
  }
  std::string text() {
    std::string Text;
    llvm::raw_string_ostream OS(Text);
    Module->print(OS, nullptr);
    return Text;
  }
};
ScalarValidationResult check(Subject &A, Subject &B,
                             const ScalarValidationOptions &Options = {},
                             const ComparisonControl &Control = {}) {
  const auto BeforeA = A.text(), BeforeB = B.text();
  auto R = validateScalarFunctions(A.Native, 0, A.lift(), B.Native, 0, B.lift(),
                                   Options, Control);
  EXPECT_EQ(A.text(), BeforeA);
  EXPECT_EQ(B.text(), BeforeB);
  return R;
}
} // namespace

TEST(BinaryDiffScalarValidation,
     CopiesValidatedPackagingAndProvesCompleteDomain) {
  Subject A(0x1000, "%r = add i32 %x, 3\nret i32 %r");
  Subject B(0x8000, "%a = add i32 %x, 1\n%r = add i32 %a, 2\nret i32 %r");
  const auto R = check(A, B);
  EXPECT_EQ(R.Proof.Status, Status::Proved) << R.Proof.Diagnostic;
  EXPECT_EQ(R.Proof.InputDomain,
            analysis::LLVMScalarInputDomain::DefinedBitvectors);
  ASSERT_EQ(R.Arguments.size(), 1U);
  EXPECT_EQ(R.Arguments[0].RegisterOffset, x86reg::RDI);
  EXPECT_EQ(R.ResultBits, 32U);
  EXPECT_GT(R.Proof.CompletedPartitions, 0U);
}

TEST(BinaryDiffScalarValidation, EntryLocationsAndEHMustBindExactly) {
  Subject A(0x1000, "ret i32 %x"), B(0x8000, "ret i32 %x");
  rewrite_source::setOriginalVA(B.function(), 0x8001);
  EXPECT_EQ(check(A, B).Proof.Status, Status::Unsupported);
  rewrite_source::setOriginalVA(B.function(), 0x8000);
  parameter_sites::setParameterSites(B.function(), {{x86reg::RSI, 1}});
  EXPECT_EQ(check(A, B).Proof.Status, Status::Unsupported);
  parameter_sites::setParameterSites(B.function(), {{~uint64_t(0), 1}});
  EXPECT_EQ(check(A, B).Proof.Status, Status::Unsupported);
  parameter_sites::setParameterSites(B.function(), {{x86reg::RDI, 1}});
  B.function().removeFnAttr(LLVMArgumentBanksAttribute);
  EXPECT_EQ(check(A, B).Proof.Status, Status::Unsupported);
  B.function().addFnAttr(LLVMArgumentBanksAttribute, "1,0");
  exception_rewrite::setContract(B.function(),
                                 exception_rewrite::SourceState::Partial,
                                 exception_rewrite::LoweringState::Incomplete);
  EXPECT_EQ(check(A, B).Proof.Status, Status::Unsupported);
}

TEST(BinaryDiffScalarValidation, CanonicalUnwindOnlySourceCanBeProjected) {
  Subject A(0x1000, "%r = add i32 %x, 3\nret i32 %r");
  Subject B(0x8000, "%r = add i32 %x, 3\nret i32 %r");
  B.setUnwindOnly();
  EXPECT_EQ(check(A, B).Proof.Status, Status::Proved);
  A.setUnwindOnly();
  A.Exceptions->establishDecodeProvenance();
  const auto R = check(A, B);
  EXPECT_EQ(R.Proof.Status, Status::Proved) << R.Proof.Diagnostic;
  EXPECT_TRUE(A.function().hasUWTable());
  EXPECT_TRUE(B.function().hasUWTable());
}

TEST(BinaryDiffScalarValidation, UnwindSourceAndCanonicalAttachmentMustAgree) {
  const auto Refuses = [](auto Mutate) {
    Subject A(0x1000, "ret i32 %x"), B(0x8000, "ret i32 %x");
    B.setUnwindOnly();
    Mutate(B);
    const auto R = check(A, B);
    EXPECT_EQ(R.Proof.Status, Status::Unsupported) << R.Proof.Diagnostic;
  };
  Refuses([](Subject &B) { B.Exceptions.reset(); });
  Refuses([](Subject &B) {
    B.function().setMetadata(windows_eh_md::FunctionAttachment, nullptr);
  });
  Refuses([](Subject &B) {
    exception_rewrite::setContract(
        B.function(), exception_rewrite::SourceState::Absent,
        exception_rewrite::LoweringState::NotRequired);
  });
  Refuses([](Subject &B) {
    exception_rewrite::setContract(B.function(),
                                   exception_rewrite::SourceState::Complete,
                                   exception_rewrite::LoweringState::Complete);
  });
  Refuses([](Subject &B) { B.ABI = BinaryFormat::COFF; });
  Refuses([](Subject &B) { B.Exceptions->FunctionEntry = 0x8001; });
  Refuses([](Subject &B) { B.Exceptions->CodeRange.End = 0x8000; });
  Refuses([](Subject &B) { B.Exceptions->UnwindInfoVA = 0x1234; });
  Refuses([](Subject &B) {
    B.Exceptions->establishDecodeProvenance().Structural.ParseStatus =
        ExceptionParseStatus::Partial;
  });
  Refuses([](Subject &B) {
    B.Exceptions->establishDecodeProvenance().Language.Diagnostics.push_back(
        "unresolved language contribution");
  });
  Refuses([](Subject &B) {
    auto *MD = B.function().getMetadata(windows_eh_md::FunctionAttachment);
    llvm::SmallVector<llvm::Metadata *, windows_eh_md::OperandCount> Fields;
    for (const auto &Operand : MD->operands())
      Fields.push_back(Operand.get());
    B.function().setMetadata(windows_eh_md::FunctionAttachment,
                             llvm::MDNode::getDistinct(B.Context, Fields));
  });
  Refuses([](Subject &B) {
    B.function().setMetadata(
        "unknown.unwind.contract",
        B.function().getMetadata(windows_eh_md::FunctionAttachment));
  });
}

TEST(BinaryDiffScalarValidation,
     LanguageAndIncompleteUnwindSourcesStayUnsupported) {
  const auto Refuses = [](auto Mutate) {
    Subject A(0x1000, "ret i32 %x"), B(0x8000, "ret i32 %x");
    B.setUnwindOnly();
    Mutate(*B.Exceptions);
    // Even matching canonical metadata cannot authorize a language handler or
    // incomplete source under this scalar profile.
    B.function().setMetadata(windows_eh_md::FunctionAttachment,
                             windows_eh_md::getCanonicalFunctionMetadata(
                                 B.Context, *B.Exceptions, Arch::X64, B.ABI));
    const auto R = check(A, B);
    EXPECT_EQ(R.Proof.Status, Status::Unsupported) << R.Proof.Diagnostic;
  };
  Refuses([](auto &EH) { EH.ParseStatus = ExceptionParseStatus::Partial; });
  Refuses([](auto &EH) { EH.ParseStatus = ExceptionParseStatus::Malformed; });
  Refuses([](auto &EH) { EH.PersonalityVA = 0x1234; });
  Refuses([](auto &EH) { EH.PersonalityName = "unresolved_handler"; });
  Refuses([](auto &EH) { EH.HandlerDataVA = 0x1234; });
  Refuses([](auto &EH) { EH.Dwarf->LSDAVA = 0x1234; });
  Refuses([](auto &EH) { EH.Itanium.emplace(); });
  Refuses([](auto &EH) { EH.SEH.emplace(); });
  Refuses([](auto &EH) { EH.GSCookie.emplace(); });
  Refuses([](auto &EH) { EH.Rust.emplace(); });
  Refuses([](auto &EH) { EH.ObjC.emplace(); });
}

TEST(BinaryDiffScalarValidation, UnwindSourceProjectionHasFiniteAllowance) {
  Subject A(0x1000, "ret i32 %x"), B(0x8000, "ret i32 %x");
  B.setUnwindOnly();
  B.Exceptions->NativeUnwindBytes.resize(65537);
  const auto R = check(A, B);
  EXPECT_EQ(R.Proof.Status, Status::BudgetExceeded);
  EXPECT_EQ(R.State, ComparisonState::ResourceLimited);
  EXPECT_NE(R.Proof.Diagnostic.find("unwind projection input limit"),
            std::string::npos);
}

TEST(BinaryDiffScalarValidation, UnknownContractsAndEffectsAreNeverStripped) {
  Subject A(0x1000, "ret i32 %x"), B(0x8000, "ret i32 %x");
  B.function().setMetadata("unknown.contract",
                           llvm::MDNode::get(B.Context, {}));
  EXPECT_EQ(check(A, B).Proof.Status, Status::Unsupported);
  B.function().setMetadata("unknown.contract", nullptr);
  B.function().addFnAttr("unknown.contract", "true");
  EXPECT_EQ(check(A, B).Proof.Status, Status::Unsupported);
  B.function().removeFnAttr("unknown.contract");
  B.function().setGC("unknown-runtime");
  EXPECT_EQ(check(A, B).Proof.Status, Status::Unsupported);
  B.function().clearGC();
  B.Native.Instructions[0].Flow = InstructionFlow::IndirectCall;
  EXPECT_EQ(check(A, B).Proof.Status, Status::Unsupported);
  for (InstructionFlow Flow :
       {InstructionFlow::Jump, InstructionFlow::Branch}) {
    B.Native.Instructions[0].Flow = Flow;
    B.Native.Instructions[0].UnresolvedCallee = true;
    EXPECT_EQ(check(A, B).Proof.Status, Status::Unsupported);
  }
  B.Native.Instructions[0].UnresolvedCallee = false;
  B.Native.Instructions[0].Flow = InstructionFlow::Return;
  B.Native.Functions[0].Traits |= TraitIncomplete;
  EXPECT_EQ(check(A, B).Proof.Status, Status::Unsupported);
  B.Native.Functions[0].Traits = 0;
  B.Native.Identity.ABI = "other-abi";
  EXPECT_EQ(check(A, B).Proof.Status, Status::Unsupported);
}

TEST(BinaryDiffScalarValidation, DefinedInputsDoNotErasePoisonOrDisagreement) {
  Subject A(0x1000, "%r = add i32 %x, 3\nret i32 %r");
  Subject B(0x8000, "%r = add nsw i32 %x, 3\nret i32 %r");
  EXPECT_NE(check(A, B).Proof.Status, Status::Proved);
  Subject C(0x8000, "%r = add i32 %x, 5\nret i32 %r");
  EXPECT_EQ(check(A, C).Proof.Status, Status::Unproved);
  Subject D(0x8000, "ret i32 poison");
  EXPECT_EQ(check(A, D).Proof.Status, Status::Unsupported);
}

TEST(BinaryDiffScalarValidation, LimitsAndStopsCannotGrantEquivalence) {
  Subject A(0x1000, "ret i32 %x"), B(0x8000, "ret i32 %x");
  ScalarValidationOptions Options;
  Options.MaxWork = 1;
  const auto Limited = check(A, B, Options);
  EXPECT_EQ(Limited.State, ComparisonState::ResourceLimited);
  EXPECT_EQ(Limited.Proof.Status, Status::BudgetExceeded);
  std::atomic<bool> Cancel{true};
  ComparisonControl Control;
  Control.Cancel = &Cancel;
  EXPECT_EQ(check(A, B, {}, Control).State, ComparisonState::Cancelled);
  Cancel = false;
  Control.Deadline = std::chrono::steady_clock::now();
  EXPECT_EQ(check(A, B, {}, Control).State, ComparisonState::TimedOut);
  Options.MaxWork = 0;
  EXPECT_TRUE(Options.validate());
}
