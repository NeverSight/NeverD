//===- CalleeSignatureMemoTests.cpp - The callee memo's bookkeeping ------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// The callee signature memo's own rules: which kept conversion serves a run,
/// and which forwarder probes and recoveries a conversion keeps.  They read
/// the pipeline's internals, so they link its library rather than the shared
/// engine, whose Windows DLL exports only the C API.
///
//===----------------------------------------------------------------------===//

#include "PipelineCallAbiDetail.h"
#include "gtest/gtest.h"

#include "neverd/pipeline/Pipeline.h"

#include <iterator>
#include <memory>
#include <utility>

namespace {

TEST(CalleeSignatureMemo, AKeptBodyServesWhileTheSummariesItReadHold) {
  // A callee body kept from one run serves another whose call summaries read
  // the same under every key the conversion read them under; summaries under
  // other keys do not bear on it.
  auto State = [](neverd::PipelineResult &R) {
    R.CallMayWriteGPRs[0x1000] = 0x3;
    R.CallEntryReadGPRs[0x1000][0] = 8;
    R.CallEntryStackArgs[0x2000] = 2;
    R.CallEntryStackSlots.insert(0x2000);
    R.CallEntryArgumentCounts[0x2000] = 3;
    R.CallEntryVariadicArguments[0x2000] = {8, 4};
  };
  neverd::PipelineResult Read;
  State(Read);
  neverd::OmittedCalleeMemo::Conversion Callee;
  for (const neverd::va_t Key : {0x1000, 0x2000})
    Callee.Reads.push_back(neverd::OmittedCalleeMemo::readAt(Read, Key));
  EXPECT_TRUE(Callee.readsHold(Read));
  neverd::PipelineResult Elsewhere;
  State(Elsewhere);
  Elsewhere.CallMayWriteGPRs[0x3000] = 0x1;
  Elsewhere.CallEntryReadGPRs[0x3000][2] = 4;
  Elsewhere.CallEntryArgumentCounts[0x3000] = 4;
  Elsewhere.CallEntryVariadicArguments[0x3000] = {4};
  EXPECT_TRUE(Callee.readsHold(Elsewhere));
  // A summary under a key it read that changes, appears or goes away does.
  void (*const Changes[])(neverd::PipelineResult &) = {
      [](neverd::PipelineResult &R) { R.CallMayWriteGPRs[0x1000] = 0x7; },
      [](neverd::PipelineResult &R) { R.CallMayWriteGPRs[0x2000] = 0x1; },
      [](neverd::PipelineResult &R) { R.CallEntryReadGPRs[0x1000][1] = 4; },
      [](neverd::PipelineResult &R) { R.CallEntryReadGPRs.erase(0x1000); },
      [](neverd::PipelineResult &R) { R.CallEntryStackArgs[0x2000] = 1; },
      [](neverd::PipelineResult &R) { R.CallEntryStackArgs[0x1000] = 1; },
      [](neverd::PipelineResult &R) { R.CallEntryStackSlots.erase(0x2000); },
      [](neverd::PipelineResult &R) { R.CallEntryStackSlots.insert(0x1000); },
      [](neverd::PipelineResult &R) { R.CallEntryArgumentCounts[0x2000] = 2; },
      [](neverd::PipelineResult &R) {
        R.CallEntryVariadicArguments[0x2000][1] = 8;
      },
      [](neverd::PipelineResult &R) {
        R.CallEntryVariadicArguments.erase(0x2000);
      },
      [](neverd::PipelineResult &R) {
        R.CallEntryArgumentCounts.erase(0x2000);
      },
  };
  for (size_t I = 0; I < std::size(Changes); ++I) {
    SCOPED_TRACE(I);
    neverd::PipelineResult Changed;
    State(Changed);
    Changes[I](Changed);
    EXPECT_FALSE(Callee.readsHold(Changed));
  }
}

TEST(CalleeSignatureMemo, ACalleeKeepsTheConversionOfEachSummaryItReads) {
  // A callee read around selections that call different imports keeps a
  // conversion for each, and a run takes the one its summaries hold for.
  using Memo = neverd::OmittedCalleeMemo;
  auto Counted = [](int Count) {
    auto R = std::make_unique<neverd::PipelineResult>();
    if (Count)
      R->CallEntryArgumentCounts[0x4000] = Count;
    return R;
  };
  auto Converted = [](const neverd::PipelineResult &R) {
    Memo::Conversion C;
    C.Body = std::make_shared<const neverd::MedFunc>();
    C.Reads.push_back(Memo::readAt(R, 0x4000));
    return C;
  };
  Memo::Callee Callee;
  const auto None = Counted(0), Two = Counted(2), Three = Counted(3);
  EXPECT_EQ(Callee.conversionFor(*None), nullptr);
  Callee.Conversions.push_back(Converted(*None));
  Callee.Conversions.insert(Callee.Conversions.begin(), Converted(*Two));
  const auto TwoBody = Callee.Conversions.front().Body;
  const auto NoneBody = Callee.Conversions.back().Body;
  // Each run finds its own conversion, which it then uses first.
  const Memo::Conversion *Found = Callee.conversionFor(*None);
  ASSERT_NE(Found, nullptr);
  EXPECT_EQ(Found->Body, NoneBody);
  EXPECT_EQ(&Callee.Conversions.front(), Found);
  Found = Callee.conversionFor(*Two);
  ASSERT_NE(Found, nullptr);
  EXPECT_EQ(Found->Body, TwoBody);
  EXPECT_EQ(&Callee.Conversions.front(), Found);
  EXPECT_EQ(Callee.conversionFor(*Three), nullptr);
  EXPECT_EQ(Callee.Conversions.size(), 2u);
}

TEST(CalleeSignatureMemo, ASourceProjectionKeepsTheCompleteReadSet) {
  using namespace neverd;
  PipelineResult Source;
  Source.CallEntryReadGPRs[0x1000][0] = 8;
  Source.CallUnreadGPRs[0x1000] = 0x10;
  Source.CallEntryIntegerArguments[0x1000].Parameters = {
      {SourceABICarrierKind::IntegerRegister, 8, 0, 8}};
  Source.OmittedCalleeIntegerArguments =
      std::make_shared<const decltype(Source.CallEntryIntegerArguments)>(
          Source.CallEntryIntegerArguments);
  Source.CallEntryIntegerArguments.clear();
  Source.CallEntryFloatingArguments[0x2000].ParameterBytes = {8, 4};
  Source.CallEntryFloatingArguments[0x2000].ReturnBytes = 8;
  Source.OmittedCalleeFloatingArguments =
      std::make_shared<const decltype(Source.CallEntryFloatingArguments)>(
          Source.CallEntryFloatingArguments);
  Source.CallEntryFloatingArguments.clear();
  Source.CallEntryVariadicArguments[0x2000] = {8, 4};
  Source.CallEntryStackArgs[0x2000] = 2;
  Source.CallEntryStackSlots.insert(0x2000);

  OmittedCalleeMemo::Callee Callee;
  auto &Conversion = Callee.Conversions.emplace_back();
  Conversion.Body = std::make_shared<const MedFunc>();
  for (va_t Entry : {0x1000, 0x2000})
    Conversion.Reads.push_back(OmittedCalleeMemo::readAt(Source, Entry));
  const auto Body = Conversion.Body;
  PipelineResult Projection;
  copyCallSignatureFacts(Source, Projection);
  const auto *Hit = Callee.conversionFor(Projection);
  ASSERT_TRUE(Hit);
  EXPECT_EQ(Hit->Body, Body);
  // Losing either the unread fact or the image-wide typed declarations must
  // invalidate the conversion. The SDK used to omit all three from its copy.
  Projection.CallUnreadGPRs.clear();
  EXPECT_EQ(Callee.conversionFor(Projection), nullptr);
  copyCallSignatureFacts(Source, Projection);
  Projection.OmittedCalleeIntegerArguments.reset();
  EXPECT_EQ(Callee.conversionFor(Projection), nullptr);
  copyCallSignatureFacts(Source, Projection);
  Projection.OmittedCalleeFloatingArguments.reset();
  EXPECT_EQ(Callee.conversionFor(Projection), nullptr);
}

TEST(CalleeSignatureMemo, WideOperandsCountAgainstTheBodyBudget) {
  using Memo = neverd::OmittedCalleeMemo;
  const auto HeldBytes = [](unsigned Count, bool Shrink = false) {
    neverd::MedFunc Body;
    Body.Blocks.resize(1);
    auto &Call = Body.Blocks.front().Ops.emplace_back();
    Call.Opcode = neverd::NdOp::CALL;
    for (unsigned I = 0; I < Count; ++I)
      Call.addInput(neverd::MedVar::makeConst(I, 8));
    if (Shrink) {
      Call.Inputs.resize(2);
      Call.NumInputs = 2;
    }
    Memo Kept;
    Memo::Conversion Converted;
    Converted.Body = std::make_shared<const neverd::MedFunc>(std::move(Body));
    const auto &Held = Kept.keep(*Kept.find(0x4000), std::move(Converted));
    EXPECT_EQ(Kept.bytes(), Held.Bytes);
    return Held.Bytes;
  };
  const size_t Inline = HeldBytes(2);
  EXPECT_GE(HeldBytes(3), Inline + 3 * sizeof(neverd::MedVar));
  EXPECT_GE(HeldBytes(65), Inline + 65 * sizeof(neverd::MedVar));
  // Shrinking the active operands retains the external allocation.
  EXPECT_GE(HeldBytes(65, true), Inline + 65 * sizeof(neverd::MedVar));
}

TEST(CalleeSignatureMemo, AConversionKeepsTheProbesUsedLast) {
  // A conversion keeps the forwarder probes of its body under the summaries
  // of the last runs, the latest first, and the memo counts them against its
  // bound.  A body the memo holds names its conversion; another, none.
  using Memo = neverd::OmittedCalleeMemo;
  Memo Kept;
  neverd::MedFunc Body;
  Body.Entry = 0x4000;
  Memo::Conversion &Held = Kept.find(0x4000)->Conversions.emplace_back();
  Held.Body = std::make_shared<const neverd::MedFunc>(Body);
  for (int Arity = 0; Arity <= static_cast<int>(Memo::MaxProbes); ++Arity) {
    Memo::Probe Probe;
    Probe.Reads.emplace_back().Key = 0x4000;
    Probe.Reads.front().RegArity = Arity;
    Probe.Reads.emplace_back().Key = 0x5000;
    Probe.Reads.back().Name = "callee";
    Probe.Gives.RegisterArity = Arity;
    Kept.keepProbe(Held, std::move(Probe));
  }
  ASSERT_EQ(Held.Probes.size(), Memo::MaxProbes);
  EXPECT_EQ(Held.Probes.front().Gives.RegisterArity,
            static_cast<int>(Memo::MaxProbes));
  EXPECT_EQ(Held.Probes.back().Gives.RegisterArity, 1);
  size_t Bytes = 0;
  for (const Memo::Probe &Probe : Held.Probes) {
    EXPECT_GT(Probe.Bytes, 0u);
    Bytes += Probe.Bytes;
  }
  EXPECT_EQ(Held.Bytes, Bytes);
  EXPECT_EQ(Kept.bytes(), Bytes);
  EXPECT_EQ(Kept.conversionOf(*Held.Body), &Held);
  EXPECT_EQ(Kept.conversionOf(Body), nullptr);
}

TEST(CalleeSignatureMemo, AConversionKeepsTheRecoveriesUsedLast) {
  // A conversion keeps the recoveries of its body under the summaries of the
  // last runs, the latest first, and each recovery the second passes over
  // the body it gave; the memo counts both against its bound.
  using Memo = neverd::OmittedCalleeMemo;
  Memo Kept;
  Memo::Conversion &Held = Kept.find(0x4000)->Conversions.emplace_back();
  Held.Body = std::make_shared<const neverd::MedFunc>();
  auto Read = [](int Arity) {
    std::vector<Memo::ProbeRead> Reads(2);
    Reads.front().Key = 0x4000;
    Reads.front().RegArity = Arity;
    Reads.back().Key = 0x5000;
    Reads.back().Name = "callee";
    return Reads;
  };
  Memo::Recovery *Last = nullptr;
  for (int Arity = 0; Arity <= static_cast<int>(Memo::MaxProbes); ++Arity) {
    Memo::Recovery Recovery;
    Recovery.Reads = Read(Arity);
    Recovery.RegisterArity = Arity;
    Recovery.IntegerArgumentRegisters = {1, 2};
    Last = &Kept.keepRecovery(Held, std::move(Recovery));
  }
  ASSERT_EQ(Held.Recoveries.size(), Memo::MaxProbes);
  EXPECT_EQ(Last, &Held.Recoveries.front());
  EXPECT_EQ(Held.Recoveries.front().RegisterArity,
            static_cast<int>(Memo::MaxProbes));
  EXPECT_EQ(Held.Recoveries.back().RegisterArity, 1);
  for (int Arity = 0; Arity <= static_cast<int>(Memo::MaxProbes); ++Arity) {
    Memo::SecondPass Pass;
    Pass.Reads = Read(Arity);
    Pass.Parameters = static_cast<size_t>(Arity);
    Kept.keepSecondPass(Held, *Last, std::move(Pass));
  }
  ASSERT_EQ(Last->Seconds.size(), Memo::MaxProbes);
  EXPECT_EQ(Last->Seconds.front().Parameters, Memo::MaxProbes);
  EXPECT_EQ(Last->Seconds.back().Parameters, 1u);
  size_t Bytes = 0;
  for (const Memo::Recovery &Recovery : Held.Recoveries) {
    size_t Passes = 0;
    for (const Memo::SecondPass &Pass : Recovery.Seconds) {
      EXPECT_GT(Pass.Bytes, 0u);
      Passes += Pass.Bytes;
    }
    EXPECT_GT(Recovery.Bytes, Passes);
    Bytes += Recovery.Bytes;
  }
  EXPECT_EQ(Held.Bytes, Bytes);
  EXPECT_EQ(Kept.bytes(), Bytes);
}

} // namespace
