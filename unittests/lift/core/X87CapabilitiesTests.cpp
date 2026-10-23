//===- X87CapabilitiesTests.cpp - x87 inventory and admission tests ------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Prevent structural recognition and native observations from widening
/// portable, state/exception or formal qualification.
//===----------------------------------------------------------------------===//

#include "gtest/gtest.h"

#include "neverd/ir/X87Capabilities.h"
#include "neverd/ir/X87Context.h"

#include "llvm/Support/JSON.h"

#include <array>
#include <set>
#include <string>

using namespace neverd;

namespace {
X87CapabilityQuery query(uint32_t Selector) {
  return {Selector,
          X87SourceMode::X64,
          X87ABI::Microsoft,
          X87StatePath::OwnedContext,
          X87Consumer::Concrete,
          X87Backend::NativeX86,
          X87Destination::X64};
}
uint32_t memory(unsigned Escape, unsigned ModRM) {
  return Escape | ((ModRM & 0x38) << 8) | X87ContextMemory |
         (ModRM << X87ContextOriginalModRMShift);
}

TEST(X87Capabilities, InventoryCoversEveryAdmittedEncodingExactlyOnce) {
  std::set<std::string> IDs;
  for (const auto &Form : x87InstructionForms()) {
    EXPECT_TRUE(IDs.insert(Form.ID).second) << Form.ID;
    for (unsigned ModRM = Form.FirstModRM; ModRM <= Form.LastModRM; ++ModRM) {
      const uint32_t Selector =
          Form.Memory ? memory(Form.Escape, ModRM) : Form.Escape | (ModRM << 8);
      ASSERT_TRUE(x87ContextInstruction(Selector)) << Form.ID;
      EXPECT_EQ(x87InstructionForm(Selector), &Form) << Form.ID;
    }
  }
  // Exercise reserved opcode/group bits, all addressing forms, and both
  // operand-size and wait-prefix flags, not only rows present in the table.
  for (unsigned Escape = 0; Escape <= 0xff; ++Escape)
    for (unsigned ModRM = 0; ModRM <= 0xff; ++ModRM)
      for (bool Memory : {false, true})
        for (uint32_t Flags : {0U, X87ContextOperand16, X87ContextWaitPrefix,
                               X87ContextOperand16 | X87ContextWaitPrefix}) {
          const auto Selector =
              (Memory ? memory(Escape, ModRM) : Escape | (ModRM << 8)) | Flags;
          EXPECT_EQ(bool(x87InstructionForm(Selector)),
                    bool(x87ContextInstruction(Selector)))
              << Selector;
        }
}

TEST(X87Capabilities, IndependentMemoryAndFlagForms) {
  struct Expected {
    uint32_t Selector;
    const char *Name;
    unsigned Bytes;
    bool Read, Write, Flags, AX, Waiting;
  };
  const Expected Cases[] = {
      {memory(0xdb, 0x28), "FLD", 10, true, false, false, false, true},
      {memory(0xdf, 0x28), "FILD", 8, true, false, false, false, true},
      {memory(0xdf, 0x30), "FBSTP", 10, false, true, false, false, true},
      {memory(0xd9, 0x30), "FNSTENV", 28, false, true, false, false, false},
      {memory(0xd9, 0x30) | X87ContextOperand16, "FNSTENV", 14, false, true,
       false, false, false},
      {memory(0xdd, 0x30), "FNSAVE", 108, false, true, false, false, false},
      {memory(0xdd, 0x30) | X87ContextOperand16, "FNSAVE", 94, false, true,
       false, false, false},
      {0xe0df, "FNSTSW", 0, false, false, false, true, false},
      {0xe0df | X87ContextWaitPrefix, "FNSTSW", 0, false, false, false, true,
       true},
      {0xf1df, "FCOMIP", 0, false, false, true, false, true},
      {0xe9da, "FUCOMPP", 0, false, false, false, false, true},
      {0x9b, "WAIT", 0, false, false, false, false, true}};
  for (const auto &Case : Cases) {
    SCOPED_TRACE(Case.Name);
    auto Spec = x87ContextInstruction(Case.Selector);
    auto *Form = x87InstructionForm(Case.Selector);
    ASSERT_TRUE(Spec);
    ASSERT_NE(Form, nullptr);
    EXPECT_STREQ(Form->Mnemonic, Case.Name);
    EXPECT_EQ(Spec->MemoryBytes, Case.Bytes);
    EXPECT_EQ(Spec->ReadsMemory, Case.Read);
    EXPECT_EQ(Spec->WritesMemory, Case.Write);
    EXPECT_EQ(Spec->WritesFlags, Case.Flags);
    EXPECT_EQ(Spec->WritesAX, Case.AX);
    EXPECT_EQ(Spec->Waiting, Case.Waiting);
  }
}

TEST(X87Capabilities, NativeTypedAndExplicitPortableExecutionAreSeparate) {
  for (auto ABI : {X87ABI::SystemV, X87ABI::Microsoft, X87ABI::MinGW}) {
    auto Q = query(0xc1d8); // FADD ST0, ST1.
    Q.ABI = ABI;
    auto Native = queryX87Capability(Q);
    EXPECT_TRUE(Native.Implemented);
    EXPECT_EQ(Native.Numerical, X87Support::Exact);
    EXPECT_EQ(Native.ArchitecturalState, X87Support::Unknown);
    Q.Destination = X87Destination::ARM64;
    EXPECT_EQ(queryX87Capability(Q).Numerical, X87Support::Unsupported);
    Q.Backend = X87Backend::Portable;
    auto Portable = queryX87Capability(Q);
    EXPECT_TRUE(Portable.Implemented);
    EXPECT_EQ(Portable.Numerical, X87Support::Exact);
    EXPECT_EQ(Portable.ArchitecturalState, X87Support::Unknown);
    EXPECT_STREQ(Portable.Profile, "intel64-architectural-masked-v1");
    Q.StatePath = X87StatePath::TypedStack;
    Q.Consumer = X87Consumer::Lift;
    Q.Backend = X87Backend::None;
    EXPECT_TRUE(queryX87Capability(Q).Implemented);
    EXPECT_EQ(queryX87Capability(Q).Numerical, X87Support::Unknown);
    Q.Consumer = X87Consumer::LLVM;
    Q.Destination = X87Destination::X64;
    Q.Backend = X87Backend::NativeX86;
    auto Typed = queryX87Capability(Q);
    EXPECT_TRUE(Typed.Implemented);
    EXPECT_EQ(Typed.Numerical, X87Support::Unknown);
    EXPECT_EQ(Typed.ArchitecturalState, X87Support::Unknown);
    Q.SourceMode = X87SourceMode::I386;
    Q.StatePath = X87StatePath::OwnedContext;
    EXPECT_FALSE(queryX87Capability(Q).Implemented);
  }
}

TEST(X87Capabilities, PortableCoverageCannotAdmitAnUnimplementedInstruction) {
  for (const auto &Form : x87InstructionForms()) {
    auto Q = query(Form.Memory ? memory(Form.Escape, Form.FirstModRM)
                               : Form.Escape | (Form.FirstModRM << 8));
    Q.Backend = X87Backend::Portable;
    Q.Destination = X87Destination::ARM64;
    EXPECT_EQ(queryX87Capability(Q).Implemented,
              Form.PortableOperation != X87PortableOperation::None)
        << Form.ID;
    Q.Destination = X87Destination::I386;
    EXPECT_FALSE(queryX87Capability(Q).Implemented) << Form.ID;
  }
}

TEST(X87Capabilities, TypedKnownRefusalsAndPartialValuesAreDistinct) {
  for (uint32_t Selector :
       {0xc0dfU, 0xf6d9U, memory(0xdf, 0x20), memory(0xdf, 0x30)}) {
    auto Q = query(Selector);
    EXPECT_TRUE(queryX87Capability(Q).Implemented);
    Q.StatePath = X87StatePath::TypedStack;
    Q.Consumer = X87Consumer::Lift;
    Q.Backend = X87Backend::None;
    EXPECT_FALSE(queryX87Capability(Q).Implemented);
  }
  for (uint32_t Selector : {0xe0d9U, 0xe1d9U, 0xebd9U, memory(0xdf, 0x28)}) {
    auto Q = query(Selector);
    Q.StatePath = X87StatePath::TypedStack;
    Q.Consumer = X87Consumer::HighC;
    auto Result = queryX87Capability(Q);
    EXPECT_EQ(Result.Numerical, X87Support::Exact);
    EXPECT_EQ(Result.ArchitecturalState, X87Support::Unknown);
  }
  auto Q = query(0xd1d8); // FCOM changes only a projection of SW in typed IR.
  Q.StatePath = X87StatePath::TypedStack;
  Q.Consumer = X87Consumer::LLVM;
  EXPECT_EQ(queryX87Capability(Q).ArchitecturalState, X87Support::Unknown);
}

TEST(X87Capabilities, StateTransportCannotClaimNumericalQualification) {
  for (uint32_t Selector :
       {0xe3dbU, 0xe2dbU, 0x9bU, 0x770fU, 0xf6d9U, memory(0xd9, 0x30)}) {
    auto Result = queryX87Capability(query(Selector));
    EXPECT_TRUE(Result.Implemented);
    EXPECT_EQ(Result.Numerical, X87Support::Unknown);
    EXPECT_EQ(Result.ArchitecturalState, X87Support::Unknown);
  }
}

TEST(X87Capabilities, ProcessorDependentOperationsRemainExplicitlyOptIn) {
  const std::set<unsigned> Dependent = {0xf0, 0xf1, 0xf2, 0xf3, 0xf5,
                                        0xf8, 0xf9, 0xfb, 0xfe, 0xff};
  for (unsigned ModRM = 0xc0; ModRM <= 0xff; ++ModRM) {
    auto Q = query(0xd9 | (ModRM << 8));
    EXPECT_EQ(x87InstructionIsProcessorDependent(Q.Selector),
              Dependent.contains(ModRM));
    if (!Dependent.contains(ModRM))
      continue;
    EXPECT_EQ(queryX87Capability(Q).Numerical, X87Support::Unsupported);
    Q.Backend = X87Backend::NativeReference;
    EXPECT_EQ(queryX87Capability(Q).Numerical, X87Support::Approximate);
    EXPECT_EQ(queryX87Capability(Q).ArchitecturalState, X87Support::Unknown);
    Q.Consumer = X87Consumer::CheckedRefinement;
    Q.Backend = X87Backend::None;
    EXPECT_EQ(queryX87Capability(Q).Numerical, X87Support::Unknown);
  }
}

TEST(X87Capabilities, MalformedQueriesAndAllTranslationCellsFailClosed) {
  auto Valid = query(0xc1d8);
  for (unsigned Dimension = 0; Dimension != 7; ++Dimension) {
    auto Q = Valid;
    switch (Dimension) {
    case 0:
      Q.SourceMode = static_cast<X87SourceMode>(-1);
      break;
    case 1:
      Q.ABI = static_cast<X87ABI>(-1);
      break;
    case 2:
      Q.StatePath = static_cast<X87StatePath>(-1);
      break;
    case 3:
      Q.Consumer = static_cast<X87Consumer>(-1);
      break;
    case 4:
      Q.Backend = static_cast<X87Backend>(-1);
      break;
    case 5:
      Q.Destination = static_cast<X87Destination>(-1);
      break;
    case 6:
      Q.Selector |= 0x80000000;
      break;
    }
    EXPECT_EQ(queryX87Capability(Q).Numerical, X87Support::Unsupported);
    EXPECT_FALSE(queryX87Capability(Q).Implemented);
  }
  for (const auto &Form : x87InstructionForms()) {
    auto Q = query(Form.Memory ? memory(Form.Escape, Form.FirstModRM)
                               : Form.Escape | (Form.FirstModRM << 8));
    Q.Consumer = X87Consumer::Translation;
    for (auto Mode : {X87SourceMode::I386, X87SourceMode::X64})
      for (auto Host :
           {X87Destination::I386, X87Destination::X64, X87Destination::ARM64}) {
        Q.SourceMode = Mode;
        Q.Destination = Host;
        EXPECT_EQ(queryX87Capability(Q).Numerical, X87Support::Unsupported);
      }
  }
}

TEST(X87Capabilities, FactoredReportNeverWidensAnIndividualInstruction) {
  for (const auto &Form : x87InstructionForms()) {
    uint32_t Representative = memory(0xd8, 0);
    if (!Form.TypedLift)
      Representative = Form.ValueClass == X87ValueClass::StateOnly
                           ? 0xf6d9
                           : memory(0xdf, 0x20);
    else if (Form.ValueClass == X87ValueClass::ExactValue)
      Representative = 0xe8d9;
    else if (Form.ValueClass == X87ValueClass::ProcessorDependent)
      Representative = 0xfed9;
    else if (Form.ValueClass == X87ValueClass::StateOnly)
      Representative = 0xd0d9;
    if (Form.PortableOperation != X87PortableOperation::None)
      Representative = Form.ValueClass == X87ValueClass::StateOnly    ? 0xe3db
                       : Form.ValueClass == X87ValueClass::ExactValue ? 0xe0d9
                                                                      : 0xc1d8;
    auto Q = query(Form.Memory ? memory(Form.Escape, Form.FirstModRM)
                               : Form.Escape | (Form.FirstModRM << 8));
    for (unsigned Mode = 0; Mode != 2; ++Mode)
      for (unsigned Path = 0; Path != 2; ++Path)
        for (unsigned Consumer = 0; Consumer != 9; ++Consumer)
          for (unsigned Backend = 0; Backend != 4; ++Backend)
            for (unsigned Destination = 0; Destination != 3; ++Destination)
              for (unsigned ABI = 0; ABI != 3; ++ABI) {
                Q.SourceMode = static_cast<X87SourceMode>(Mode);
                Q.StatePath = static_cast<X87StatePath>(Path);
                Q.Consumer = static_cast<X87Consumer>(Consumer);
                Q.Backend = static_cast<X87Backend>(Backend);
                Q.Destination = static_cast<X87Destination>(Destination);
                Q.ABI = static_cast<X87ABI>(ABI);
                auto Group = Q;
                Group.Selector = Representative;
                auto Actual = queryX87Capability(Q);
                auto Reported = queryX87Capability(Group);
                ASSERT_EQ(Actual.Implemented, Reported.Implemented) << Form.ID;
                ASSERT_EQ(Actual.Numerical, Reported.Numerical) << Form.ID;
                ASSERT_EQ(Actual.ArchitecturalState,
                          Reported.ArchitecturalState)
                    << Form.ID;
                ASSERT_STREQ(Actual.Reason, Reported.Reason) << Form.ID;
              }
  }
}

TEST(X87Capabilities, ReportPreservesEveryDimensionAndLabelsSourceEvidence) {
  auto Parsed = llvm::json::parse(x87CapabilitiesJSON());
  ASSERT_TRUE(bool(Parsed)) << llvm::toString(Parsed.takeError());
  const auto *Root = Parsed->getAsObject();
  ASSERT_NE(Root, nullptr);
  EXPECT_EQ(Root->getInteger("schema_version"), kX87CapabilitiesSchemaVersion);
  ASSERT_NE(Root->getArray("forms"), nullptr);
  EXPECT_EQ(Root->getArray("forms")->size(), x87InstructionForms().size());
  ASSERT_NE(Root->getArray("source_paths"), nullptr);
  EXPECT_EQ(Root->getArray("source_paths")->size(), 12U);
  ASSERT_NE(Root->getArray("coverage"), nullptr);
  EXPECT_EQ(Root->getArray("coverage")->size(), 3888U);
  EXPECT_TRUE(Root->getString("evidence")->contains("not passing"));
  for (const auto &Row : *Root->getArray("coverage")) {
    const auto *Cell = Row.getAsObject();
    ASSERT_NE(Cell, nullptr);
    EXPECT_TRUE(Cell->getString("source_mode"));
    EXPECT_TRUE(Cell->getString("state_path"));
    EXPECT_TRUE(Cell->getString("consumer"));
    EXPECT_TRUE(Cell->getString("backend"));
    EXPECT_TRUE(Cell->getString("destination"));
    EXPECT_NE(Cell->getString("architectural_state"), "supported-exact");
  }
}
} // namespace
