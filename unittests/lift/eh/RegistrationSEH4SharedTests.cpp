//===- RegistrationSEH4SharedTests.cpp - Shared EH4 frame ownership -------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Exercise physical SEH4 entry and exit calls through ordinary CFG analysis.
//===----------------------------------------------------------------------===//

#include "../../../lib/ir/low/X86/RegistrationABIPrivate.h"
#include "RegistrationSEH4SharedTestUtils.h"
#include "gtest/gtest.h"

#include "neverd/Limits.h"
#include "neverd/decode/Decoder.h"
#include "neverd/ir/low/CFGBuilder.h"
#include "neverd/ir/low/RegistrationSharedCall.h"
#include "neverd/loader/BinaryImage.h"
#include "neverd/loader/COFF/COFFRegistrationEH.h"
#include "neverd/loader/COFF/COFFSEH4Entry.h"
#include "neverd/loader/ReadOnlyBytes.h"
#include "neverd/support/BinaryEncoding.h"

#include "llvm/ADT/STLExtras.h"

#include <algorithm>
#include <cstdint>
#include <iterator>
#include <string>
#include <utility>
#include <vector>

using namespace neverd;

using namespace neverd::registration_test;

namespace {
std::pair<BinaryImage, ExceptionFunction>
scopeStorageImage(uint32_t Base, bool EH4, bool Filtered) {
  auto Image = sharedSEH4Image(Base, false, 32,
                               Filtered ? SharedSEH4Body::FilteredCall
                                        : SharedSEH4Body::Plain);
  auto Function = *Image.ExceptionMetadata.findFunction(Base + 0x1000);
  auto &Chain = *Function.Registration;
  // This alternate view authenticates the table's fields only. It does not
  // claim the EH4 machine frame implements an EH3 invocation.
  if (!EH4) {
    Function.Personality = ExceptionPersonality::ExceptHandler3;
    Function.Encoding = ExceptionEncoding::X86ScopeTableEH3;
    Chain.ScopeTableVA += 16;
    Chain.SeededTryLevel = -1;
    Chain.Scopes.front().EnclosingLevel = -1;
    writeLE<uint32_t>(Image.Segments.back().Data.data() + 0x110, uint32_t(-1));
  }
  for (va_t Slot : {va_t(Base + 0x2114), va_t(Base + 0x2118)})
    if (Chain.scopePointerTarget(Slot)) {
      Image.CodePtrRelocSlots.insert(Slot);
      Image.BaseRelocations.push_back({Slot, 3});
    }
  Image.Symbols.clear();
  return {std::move(Image), std::move(Function)};
}
} // namespace

TEST(RegistrationSEH4Shared, TypedScopeStorageKeepsCallbackRoles) {
  for (uint32_t Base : {0x400000u, 0x18000000u})
    for (bool EH4 : {false, true})
      for (bool Filtered : {false, true})
        for (bool GS : {false, true}) {
          if (GS && !EH4)
            continue;
          SCOPED_TRACE(::testing::Message()
                       << Base << " " << EH4 << " " << Filtered << " " << GS);
          auto [Image, Function] = scopeStorageImage(Base, EH4, Filtered);
          if (GS) {
            auto &Chain = *Function.Registration;
            Chain.HasSecurityCookies = true;
            Chain.GSCookieOffset = -28;
            writeLE<uint32_t>(Image.Segments.back().Data.data() + 0x100,
                              uint32_t(-28));
          }
          const auto Range =
              coff_loader::getX86RegistrationSEHScopeTableRange(Function);
          ASSERT_TRUE(Range);
          size_t Work = 0;
          const auto Bytes = coff_loader::getCheckedX86SEHScopeTableBytes(
              Image, Function, Work);
          ASSERT_TRUE(Bytes);
          const auto *Raw = Image.readVA(Range->Begin, Range->size());
          ASSERT_NE(Raw, nullptr);
          EXPECT_EQ(*Bytes, std::vector<uint8_t>(Raw, Raw + Range->size()));
          EXPECT_GT(Work, Range->size());
          for (va_t Slot : {va_t(Base + 0x2114), va_t(Base + 0x2118)})
            if (const auto Target =
                    Function.Registration->scopePointerTarget(Slot)) {
              EXPECT_EQ(readImmutablePE32CodePointerStorage(Image, Slot),
                        Target);
              EXPECT_FALSE(readImmutableImageCodePointer(Image, Slot));
              EXPECT_FALSE(Image.hasAuthenticatedFunctionEntryAt(*Target));
            }
        }
}

TEST(RegistrationSEH4Shared, TypedScopeStorageRejectsStaleFieldsAndOwners) {
  for (uint32_t Base : {0x400000u, 0x18000000u})
    for (bool EH4 : {false, true})
      for (bool Filtered : {false, true})
        for (unsigned Mutation = 0; Mutation != 26; ++Mutation) {
          SCOPED_TRACE(::testing::Message() << Base << " " << EH4 << " "
                                            << Filtered << " " << Mutation);
          auto [Image, Function] = scopeStorageImage(Base, EH4, Filtered);
          auto &Chain = *Function.Registration;
          switch (Mutation) {
          case 0:
            Image.Segments.back().Data[0x110] ^= 1;
            break;
          case 1:
            Image.Segments.back().Data[0x118] ^= 1;
            break;
          case 2:
            if (EH4)
              ++Chain.EHCookieOffset;
            else
              Chain.HasSecurityCookies = true;
            break;
          case 3:
            Chain.Scopes.front().IsFinally = !Chain.Scopes.front().IsFinally;
            break;
          case 4:
            ++Chain.Scopes.front().HandlerVA;
            break;
          case 5:
            Image.Segments.back().Flags =
                SegmentFlags::Readable | SegmentFlags::Writable;
            break;
          case 6:
            Image.Sections.back().Flags =
                SegmentFlags::Readable | SegmentFlags::Writable;
            break;
          case 7:
            Image.Segments.push_back(Image.Segments.back());
            break;
          case 8:
            Image.Sections.push_back(Image.Sections.back());
            break;
          case 9:
            Image.Segments.back().FileSz = 0x11b;
            break;
          case 10:
            Image.BaseRelocations.clear();
            break;
          case 11:
            Image.BaseRelocations.push_back(Image.BaseRelocations.back());
            break;
          case 12:
            Image.BaseRelocations.back().Type = 10;
            break;
          case 13:
            Image.CodePtrRelocSlots.erase(Base + 0x2118);
            break;
          case 14:
            Image.BaseRelocations.push_back({Base + 0x2110, 3});
            break;
          case 15:
            Image.Segments.front().Flags =
                Image.Segments.front().Flags | SegmentFlags::Writable;
            break;
          case 16:
            Chain.Scopes.clear();
            break;
          case 17:
            Function.Encoding = EH4 ? ExceptionEncoding::X86ScopeTableEH3
                                    : ExceptionEncoding::X86ScopeTableEH4;
            break;
          case 18:
            Chain.Scopes.front().EnclosingLevel = 0;
            break;
          case 19:
            Chain.Scopes.front().HandlerVA = uint64_t(UINT32_MAX) + 1;
            break;
          case 20:
            Chain.SeededTryLevel = EH4 ? -1 : -2;
            break;
          case 21:
            Chain.SeededTryLevel = 0;
            break;
          case 22:
            Image.IsRelocatable = true;
            break;
          case 23:
            Image.Segments.back().Data[0x114] ^= 1;
            break;
          case 24:
            Image.RuntimeCallablePointerSlots.push_back({Base + 0x2118});
            break;
          case 25:
            Chain.ScopeTableVA = UINT32_MAX;
            break;
          }
          size_t Work = 0;
          EXPECT_FALSE(coff_loader::getCheckedX86SEHScopeTableBytes(
              Image, Function, Work));
        }
}

TEST(RegistrationSEH4Shared, TypedScopeStorageChargesEveryCurrentQuery) {
  for (bool EH4 : {false, true}) {
    auto [Image, Function] = scopeStorageImage(0x400000, EH4, true);
    size_t Required = 0;
    ASSERT_TRUE(coff_loader::getCheckedX86SEHScopeTableBytes(Image, Function,
                                                             Required));
    for (size_t Available : {size_t{0}, Required - 1, Required}) {
      size_t Work = limits::kMaxRegistrationEHStateWork - Available;
      EXPECT_EQ(
          coff_loader::getCheckedX86SEHScopeTableBytes(Image, Function, Work)
              .has_value(),
          Available == Required);
      EXPECT_EQ(Work, limits::kMaxRegistrationEHStateWork);
      EXPECT_FALSE(
          coff_loader::getCheckedX86SEHScopeTableBytes(Image, Function, Work));
    }
  }
}

TEST(RegistrationSEH4Shared, FrameCallsLeaveBudgetForOrdinaryCallees) {
  for (uint32_t Base : {0x400000u, 0x18000000u})
    for (bool Wide : {false, true}) {
      auto Image =
          sharedSEH4Image(Base, Wide, 32, SharedSEH4Body::OrdinaryCall);
      auto Source = liftSharedSEH4(Image);
      ASSERT_TRUE(Source.RegistrationStates);
      ASSERT_TRUE(Source.RegistrationStates->Complete);
      // Classification precedes state solving and cannot borrow its result.
      Source.RegistrationStates.reset();
      for (bool Stack : {false, true}) {
        SCOPED_TRACE(::testing::Message()
                     << Base << " " << Wide << " " << Stack);
        size_t Work = limits::kMaxRegistrationEHStateWork - 2048;
        RegistrationCallCalleeIndex Index(Image, &Work);
        if (Stack) {
          const auto Calls = Index.stackContracts(Source);
          ASSERT_TRUE(Calls);
          ASSERT_EQ(Calls->size(), 1u);
          EXPECT_EQ(Calls->front().Target, Base + 0x1180);
          EXPECT_EQ(Calls->front().StackPopBytes, 0u);
        } else {
          const auto Calls = Index.contracts(Source);
          ASSERT_TRUE(Calls);
          ASSERT_EQ(Calls->size(), 1u);
          EXPECT_EQ(Calls->front().Target, Base + 0x1180);
          EXPECT_TRUE(Calls->front().HasIndependentScalarReturn);
        }
        EXPECT_LT(Work, limits::kMaxRegistrationEHStateWork);
      }
      size_t Exhausted = limits::kMaxRegistrationEHStateWork;
      RegistrationCallCalleeIndex Limited(Image, &Exhausted);
      EXPECT_FALSE(Limited.stackContracts(Source));
      EXPECT_FALSE(Limited.contracts(Source));
    }
}

TEST(RegistrationSEH4Shared, FrameClassificationRequiresTheCurrentOccurrence) {
  auto Image = sharedSEH4Image(0x400000, false);
  const auto Source = liftSharedSEH4(Image);
  size_t Work = 0;
  const auto Calls =
      registration_abi::checkedRegistrationFrameCalls(Image, Source, Work);
  ASSERT_TRUE(Calls);
  ASSERT_EQ(Calls->size(), 2u);
  for (unsigned Mutation = 0; Mutation != 7; ++Mutation) {
    SCOPED_TRACE(Mutation);
    auto Changed = Source;
    auto Bytes = Image;
    auto Exit = llvm::find_if(Changed.Blocks, [](const auto &Block) {
      return !Block.Ops.empty() && Block.Ops.back().Addr == 0x40101d;
    });
    ASSERT_NE(Exit, Changed.Blocks.end());
    if (Mutation == 0)
      Changed.Blocks.front().Ops.back().Inputs[0].Offset += 1;
    else if (Mutation == 1)
      Bytes.Segments.front().Data[0x100] ^= 1;
    else if (Mutation == 2)
      Exit->Ops.back().Inputs[0].Offset += 1;
    else if (Mutation == 3)
      Exit->InstructionBoundaries.back().Immediate.reset();
    else if (Mutation == 4)
      Bytes.Segments.front().Data[0x150] ^= 1;
    else if (Mutation == 5)
      Changed.OrdinaryModuleAnalysisRoots.insert(0x40101e);
    else
      Changed.ExceptionMetadata.reset();
    Work = 0;
    const auto Current =
        registration_abi::checkedRegistrationFrameCalls(Bytes, Changed, Work);
    ASSERT_TRUE(Current);
    EXPECT_EQ(Current->size(), Mutation < 2 || Mutation == 6 ? 0u : 1u);
  }
  auto Changed = Source;
  LowBlock Extra;
  Extra.Id = 99;
  Extra.StartAddr = 0x401300;
  Extra.Ops.push_back(**Calls->begin());
  Changed.Blocks.push_back(std::move(Extra));
  Work = 0;
  const auto Current =
      registration_abi::checkedRegistrationFrameCalls(Image, Changed, Work);
  ASSERT_TRUE(Current);
  EXPECT_EQ(Current->size(), 2u);
  EXPECT_FALSE(Current->count(&Changed.Blocks.back().Ops.back()));
}

TEST(RegistrationSEH4Shared, OrdinaryAnalysisOwnsTheInstalledFrame) {
  for (uint32_t Base : {0x400000u, 0x18000000u})
    for (bool Wide : {false, true}) {
      SCOPED_TRACE(::testing::Message() << Base << " " << Wide);
      auto Image = sharedSEH4Image(Base, Wide);
      const auto *EH = Image.ExceptionMetadata.findFunction(Base + 0x1000);
      ASSERT_NE(EH, nullptr);
      ASSERT_TRUE(EH->Registration);
      ASSERT_EQ(EH->ParseStatus, ExceptionParseStatus::Complete);
      EXPECT_EQ(EH->Registration->RegistrationOffset, -16);
      EXPECT_EQ(EH->Registration->TryLevelOffset, -4);
      auto Low = liftSharedSEH4(Image);
      ASSERT_TRUE(Low.hasCompleteLiftCoverage());
      ASSERT_TRUE(Low.RegistrationStates);
      const auto &States = *Low.RegistrationStates;
      std::string Diagnostics;
      for (const auto &Diagnostic : States.Diagnostics)
        Diagnostics += Diagnostic + "\n";
      SCOPED_TRACE(Diagnostics);
      EXPECT_TRUE(States.Complete);
      EXPECT_TRUE(States.CallbackStatesComplete);
      EXPECT_TRUE(States.RegistrationLifetimeComplete);
      EXPECT_TRUE(States.ChainOperationsComplete);
      EXPECT_TRUE(States.SecurityCookiesComplete);
      ASSERT_TRUE(States.SEH4Prologue);
      EXPECT_EQ(States.SEH4Prologue->HelperVA, Base + 0x1100);
      EXPECT_EQ(States.SEH4Prologue->StackOffset, -64);
      EXPECT_FALSE(States.CxxPrologue);
    }
}

TEST(RegistrationSEH4Shared, RequiresEveryCurrentHelperInstruction) {
  const auto Original = sharedSEH4Image(0x400000, false);
  const auto *EH = Original.ExceptionMetadata.findFunction(0x401000);
  ASSERT_NE(EH, nullptr);
  ASSERT_TRUE(coff_loader::getCheckedX86SEH4PrologueCall(Original, *EH));
  ASSERT_TRUE(
      coff_loader::getCheckedX86SEH4EpilogueCall(Original, *EH, 0x40101d));
  for (size_t Byte = 0; Byte != 69; ++Byte) {
    auto Changed = Original;
    Changed.Segments.front().Data[0x100 + Byte] ^= 1;
    EXPECT_FALSE(coff_loader::getCheckedX86SEH4PrologueCall(Changed, *EH))
        << Byte;
  }
  for (size_t Byte = 0; Byte != 20; ++Byte) {
    auto Changed = Original;
    Changed.Segments.front().Data[0x150 + Byte] ^= 1;
    EXPECT_FALSE(
        coff_loader::getCheckedX86SEH4EpilogueCall(Changed, *EH, 0x40101d))
        << Byte;
  }
  for (size_t Byte = 0; Byte != 12; ++Byte) {
    if (Byte == 1)
      continue; // A changed allocation is a different valid protocol input.
    auto Changed = Original;
    Changed.Segments.front().Data[Byte] ^= 1;
    EXPECT_FALSE(coff_loader::getCheckedX86SEH4PrologueCall(Changed, *EH))
        << Byte;
  }
}

TEST(RegistrationSEH4Shared, PrefixEvidenceCannotSurviveChangedLowIR) {
  auto Image = sharedSEH4Image(0x400000, false);
  const auto Original = liftSharedSEH4(Image);
  ASSERT_TRUE(getCheckedX86RegistrationSEH4ProloguePrefix(Image, Original));
  for (unsigned Mutation = 0; Mutation != 5; ++Mutation) {
    auto Changed = Original;
    auto &Entry = Changed.Blocks.front();
    if (Mutation == 0)
      Entry.Ops.front().Opcode = NdOp::INT_XOR;
    if (Mutation == 1)
      Entry.Ops.back().Inputs[0].Offset += 1;
    if (Mutation == 2)
      Entry.InstructionBoundaries.back().Immediate.reset();
    if (Mutation == 3)
      Changed.OrdinaryModuleAnalysisRoots.insert(Image.Base + 0x1002);
    if (Mutation == 4)
      Entry.Preds.push_back(Entry.Id);
    EXPECT_FALSE(getCheckedX86RegistrationSEH4ProloguePrefix(Image, Changed))
        << Mutation;
  }
}

TEST(RegistrationSEH4Shared, SavedStackWordCannotReplaceANonvolatileSave) {
  for (uint32_t Allocation : {0, 1, 4, 32, 36, 200}) {
    auto Image = sharedSEH4Image(0x400000, true, Allocation);
    auto Low = liftSharedSEH4(Image);
    ASSERT_TRUE(Low.RegistrationStates);
    // Small allocations overlap the helper's mandatory [EBP-24] ESP store
    // with the saved ESI/EBX bytes. Exact instructions alone cannot restore
    // those nonvolatile registers at the physical epilogue.
    EXPECT_EQ(Low.RegistrationStates->Complete, Allocation >= 32) << Allocation;
    EXPECT_EQ(Low.RegistrationStates->ChainOperationsComplete, Allocation >= 32)
        << Allocation;
  }
}

TEST(RegistrationSEH4Shared, WrapperBindsBothCallFormsAndHotpatchEntry) {
  for (uint32_t Base : {0x400000u, 0x18000000u})
    for (bool Hotpatch : {false, true})
      for (bool Direct : {false, true}) {
        auto Image = sharedSEH4Image(Base, false);
        auto &Text = Image.Segments.front().Data;
        std::vector<uint8_t> Body;
        if (Hotpatch)
          Body = {0x8b, 0xff};
        Body.insert(Body.end(), Text.begin() + 0x200, Text.begin() + 0x219);
        const size_t Operand = Body.size() + (Direct ? 1 : 2);
        Body.push_back(Direct ? 0xe8 : 0xff);
        if (!Direct)
          Body.push_back(0x15);
        Body.resize(Body.size() + 4);
        writeLE<uint32_t>(Body.data() + Operand,
                          Direct ? uint32_t(0x90 - Body.size())
                                 : Base + 0x2008);
        Body.insert(Body.end(), {0x83, 0xc4, 0x18, 0x5d, 0xc3});
        std::fill(Text.begin() + 0x200, Text.begin() + 0x240, 0xcc);
        std::copy(Body.begin(), Body.end(), Text.begin() + 0x200);
        for (const char *Module : {"msvcrt.dll", "msvcr100.dll", "ucrtbase.dll",
                                   "vcruntime140.dll", "vcruntime140d.dll"}) {
          Image.Imports.front().Module = Module;
          EXPECT_EQ(
              coff_loader::getCheckedX86EH4CookieCheck(Image, Base + 0x1200),
              Base + 0x1280)
              << Hotpatch << Direct << Module;
        }
      }
}

TEST(RegistrationSEH4Shared, WrapperRequiresExactArgumentsAndRuntimeBinding) {
  for (unsigned Mutation = 0; Mutation != 10; ++Mutation) {
    auto Image = sharedSEH4Image(0x400000, false);
    auto &Text = Image.Segments.front().Data;
    if (Mutation == 0)
      Image.Imports.front().Module = "other.dll";
    if (Mutation == 1)
      Image.Imports.front().Name = "different_handler";
    if (Mutation == 2)
      Image.Imports.front().IsDelayImport = true;
    if (Mutation == 3)
      Image.Imports.push_back(Image.Imports.front());
    if (Mutation == 4)
      Image.Segments.front().Flags = Image.Sections.front().Flags =
          SegmentFlags::Readable | SegmentFlags::Executable |
          SegmentFlags::Writable;
    if (Mutation == 5)
      Text[0x205] = 0x10;
    if (Mutation == 6)
      Text[0x215] ^= 4;
    if (Mutation == 7)
      Text[0x220] = 20;
    if (Mutation == 8) {
      Text[0x290] = 0xe9;
      writeLE<uint32_t>(Text.data() + 0x291, uint32_t(-5));
    }
    if (Mutation == 9)
      Image.Arch = Arch::X64;
    EXPECT_FALSE(coff_loader::getCheckedX86EH4CookieCheck(Image, 0x401200))
        << Mutation;
  }
}
