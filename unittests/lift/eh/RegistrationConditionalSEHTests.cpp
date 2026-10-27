//===- RegistrationConditionalSEHTests.cpp - Bound SEH scalar results -----===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Bind initialized inputs and ordinary return values of authored SEH helpers.
//===----------------------------------------------------------------------===//

#include "../../../lib/ir/low/X86/RegistrationABIPrivate.h"
#include "../../../lib/ir/low/X86/RegistrationConditionalFrame.h"
#include "../../../lib/ir/low/X86/RegistrationOriginalImage.h"
#include "RegistrationSEH4SharedTestUtils.h"
#include "gtest/gtest.h"

#include "neverd/Limits.h"
#include "neverd/ir/med/LowToMed.h"
#include "neverd/ir/med/X86RegistrationSEHBoundCall.h"
#include "neverd/lift/X86Regs.h"
#include "neverd/loader/COFF/COFFRegistrationEH.h"
#include "neverd/support/BinaryEncoding.h"

#include "llvm/BinaryFormat/COFF.h"

#include <algorithm>
#include <cstdint>
#include <initializer_list>
#include <set>
#include <vector>

namespace {
using namespace neverd;
using namespace neverd::registration_state;

struct ScalarSEHFixture {
  BinaryImage Image;
  FrameState Parent;
  LowOp Call;
  std::set<int32_t> Initialized;
  size_t ReturnOffset = 0;

  ScalarSEHFixture(uint32_t Base, std::initializer_list<uint8_t> Body)
      : Image(registration_test::sharedSEH4Image(Base, false)) {
    auto &Text = Image.Segments.front().Data;
    std::fill(Text.begin() + 12, Text.begin() + 0x80, 0xcc);
    std::vector<uint8_t> Code(Body);
    // Keep the shared frame protocol; only the authored ordinary body varies.
    Code.insert(Code.end(), {0xc7, 0x45, 0xfc, 0xfe, 0xff, 0xff, 0xff});
    const size_t Exit = 12 + Code.size();
    Code.push_back(0xe8);
    Code.resize(Code.size() + 4);
    writeLE<uint32_t>(Code.data() + Code.size() - 4,
                      uint32_t(0x150 - Exit - 5));
    ReturnOffset = 12 + Code.size();
    Code.push_back(0xc3);
    std::copy(Code.begin(), Code.end(), Text.begin() + 12);
    Text[0x300] = 0xe8;
    writeLE<uint32_t>(Text.data() + 0x301, uint32_t(-0x305));
    Text[0x305] = 0xc3;
    Image.Symbols.push_back(Symbol::makeFunc(Base + 0x1300));
    Image.ExceptionMetadata = {};
    coff_loader::parseX86RegistrationExceptions(Image);
    Parent.Registers[x86reg::RSP / x86reg::GeneralRegStride] =
        FrameValue::frame(-64);
    Parent.store(-64, 4, FrameValue::constant(37));
    for (int32_t Byte = -64; Byte != -60; ++Byte)
      Initialized.insert(Byte);
    Call.Opcode = NdOp::CALL;
    Call.Addr = Base + 0x1300;
    Call.Seq = 0;
    Call.Output = NdVar::reg(x86reg::RAX, 4);
    Call.addInput(NdVar::cst(Base + 0x1000, 4));
  }
};
} // namespace

TEST(RegistrationConditionalSEH, ScalarInputsNeedNoObjectBorrow) {
  for (uint32_t Base : {0x400000u, 0x18000000u})
    for (unsigned Mode = 0; Mode != 14; ++Mode) {
      SCOPED_TRACE(Base);
      SCOPED_TRACE(Mode);
      ScalarSEHFixture F(Base, {0x8b, 0x45, 8});
      const auto Argument = Mode == 1 ? FrameValue::imageAddress(Base + 0x2200)
                            : Mode == 2 || Mode == 3 ? FrameValue{}
                                                     : FrameValue::constant(37);
      F.Parent.store(-64, 4, Argument);
      if (Mode == 4)
        F.Initialized.erase(-61);
      if (Mode == 5)
        F.Parent.store(-61, 1, FrameValue::frame(-32));
      if (Mode == 6)
        ++F.Call.Addr;
      if (Mode == 7)
        F.Image.Segments.front().Data[F.ReturnOffset] = 0xcb;
      if (Mode == 8)
        F.Image.Segments.front().Flags =
            F.Image.Segments.front().Flags | SegmentFlags::Writable;
      if (Mode == 9)
        F.Call.Seq = -1;
      if (Mode == 10)
        F.Call.MemoryAddressSpace = NdMemoryAddressSpace::X86FS;
      if (Mode == 11)
        F.Call.Inputs[0].Size = 8;
      if (Mode == 12)
        F.Parent.Registers[x86reg::RSP / x86reg::GeneralRegStride] = {};
      size_t Work = Mode == 13 ? limits::kMaxRegistrationEHStateWork : 0;
      const auto *Initialized = Mode == 3 ? nullptr : &F.Initialized;
      const auto Effect = proveBoundSEHCallee(F.Image, F.Call, F.Parent, Work,
                                              nullptr, Initialized);
      ASSERT_EQ(bool(Effect), Mode < 3);
      if (!Effect)
        continue;
      EXPECT_TRUE(Effect->HasOrdinaryReturn);
      EXPECT_EQ(Effect->ReturnValue, Argument);
      EXPECT_TRUE(Effect->Reads.empty());
      EXPECT_TRUE(Effect->Writes.empty());
      ASSERT_TRUE(Effect->NativeCall);
      ASSERT_EQ(Effect->NativeCall->Arguments.size(), 1u);
      EXPECT_EQ(Effect->NativeCall->Arguments[0].ArgumentKind,
                RegistrationSEHBoundArgument::Kind::Scalar);
      EXPECT_EQ(Effect->NativeCall->Arguments[0].Offset, -64);
      EXPECT_EQ(Effect->NativeCall->ConstantReturn, Argument.Constant);
      ASSERT_GT(Work, 0u);
      ASSERT_LT(Work, limits::kMaxRegistrationEHStateWork);
      size_t Exact = limits::kMaxRegistrationEHStateWork - Work;
      EXPECT_TRUE(proveBoundSEHCallee(F.Image, F.Call, F.Parent, Exact, nullptr,
                                      Initialized));
      EXPECT_EQ(Exact, limits::kMaxRegistrationEHStateWork);
      size_t Short = limits::kMaxRegistrationEHStateWork - Work + 1;
      EXPECT_FALSE(proveBoundSEHCallee(F.Image, F.Call, F.Parent, Short,
                                       nullptr, Initialized));
    }
}

TEST(RegistrationConditionalSEH, ReturnsJoinWithoutLeakingCalleeIdentities) {
  for (unsigned Mode = 0; Mode != 6; ++Mode) {
    SCOPED_TRACE(Mode);
    ScalarSEHFixture F(
        0x400000,
        Mode == 0   ? std::initializer_list<uint8_t>{0x8b, 0xc2}
        : Mode == 1 ? std::initializer_list<uint8_t>{0x8b, 0xc3}
        : Mode == 2 ? std::initializer_list<uint8_t>{0x8b, 0x45, 4}
        : Mode == 3 ? std::initializer_list<uint8_t>{0x8d, 0x45, 0xd0}
        : Mode == 4 ? std::initializer_list<uint8_t>{0x8b, 0x45, 8, 0x85, 0xc0,
                                                     0x74, 7, 0xb8, 37, 0, 0, 0,
                                                     0xeb, 5, 0xb8, 38, 0, 0, 0}
                    : std::initializer_list<uint8_t>{0x8b, 0x45, 8, 0x8b, 0});
    F.Parent.store(-64, 4, {});
    size_t Work = 0;
    const auto Effect = proveBoundSEHCallee(F.Image, F.Call, F.Parent, Work,
                                            nullptr, &F.Initialized);
    ASSERT_EQ(bool(Effect), Mode != 5);
    if (!Effect)
      continue;
    ASSERT_TRUE(Effect->ReturnValue);
    const auto &Value = *Effect->ReturnValue;
    EXPECT_FALSE(Value.Offset);
    EXPECT_FALSE(Value.ReturnPC);
    EXPECT_EQ(Value.SavedRegister, 0u);
    if (Effect->NativeCall)
      EXPECT_FALSE(Effect->NativeCall->ConstantReturn);
    if (Mode == 4)
      EXPECT_EQ(Value, FrameValue{});
    else {
      EXPECT_TRUE(Value.MayBeFrame);
      EXPECT_TRUE(Value.FrameOnlyFromCall);
    }
  }
}

TEST(RegistrationConditionalSEH, FilterResultsDoNotBecomeOrdinaryReturns) {
  for (unsigned Mode = 0; Mode != 3; ++Mode) {
    SCOPED_TRACE(Mode);
    ScalarSEHFixture F(0x400000, {0x8b, 0x45, 8});
    auto Image = registration_test::sharedSEH4Image(
        0x400000, false, 32, registration_test::SharedSEH4Body::FilteredCall);
    auto &Text = Image.Segments.front().Data;
    std::copy(F.Image.Segments.front().Data.begin() + 0x300,
              F.Image.Segments.front().Data.begin() + 0x306,
              Text.begin() + 0x300);
    if (Mode) {
      Text[60] = Text[61] = 0x90; // Retain the ordinary/except-body EAX.
      if (Mode == 2) {
        writeLE<uint32_t>(Text.data() + 32, 37);
        writeLE<uint32_t>(Text.data() + 49, 37);
      }
    }
    size_t Work = 0;
    const auto Effect = proveBoundSEHCallee(Image, F.Call, F.Parent, Work,
                                            nullptr, &F.Initialized);
    ASSERT_TRUE(Effect);
    EXPECT_TRUE(Effect->HasOrdinaryReturn);
    // The filter returns one. The two parent paths return zero, different
    // scalars, or the same scalar; only those parent paths own the result.
    EXPECT_EQ(Effect->ReturnValue, Mode == 0   ? FrameValue::constant(0)
                                   : Mode == 1 ? FrameValue{}
                                               : FrameValue::constant(37));
  }
}

TEST(RegistrationConditionalSEH, RootBindsAConstantNativeReturn) {
  for (uint32_t Argument : {37u, 38u}) {
    SCOPED_TRACE(Argument);
    ScalarSEHFixture F(0x400000, {0x8b, 0x45, 8});
    auto &Text = F.Image.Segments.front().Data;
    // Relocate the authored scalar helper and reuse the authenticated shared
    // entry/exit instructions. Each physical relative CALL is adjusted.
    const size_t CalleeSize = F.ReturnOffset + 1;
    std::copy_n(Text.begin(), CalleeSize, Text.begin() + 0x300);
    writeLE<uint32_t>(Text.data() + 0x308, uint32_t(0x100 - 0x30c));
    const size_t CalleeExit = 0x300 + F.ReturnOffset - 5;
    writeLE<uint32_t>(Text.data() + CalleeExit + 1,
                      uint32_t(0x150 - CalleeExit - 5));
    std::fill(Text.begin() + 12, Text.begin() + 0x80, 0xcc);
    std::vector<uint8_t> Parent = {
        0x6a, uint8_t(Argument), // PUSH an initialized scalar argument.
        0xe8, 0,
        0,    0,
        0, // CALL the scalar SEH helper.
        0x83, 0xc4,
        4, // Retire the incoming argument.
        0x83, 0xf8,
        37,   0x74,
        2,    0x89,
        0x28, // A mismatching result exposes EBP.
        0xe8, 0,
        0,    0,
        0,    0xc3};
    writeLE<uint32_t>(Parent.data() + 3, uint32_t(0x300 - 19));
    writeLE<uint32_t>(Parent.data() + 18, uint32_t(0x150 - 34));
    std::copy(Parent.begin(), Parent.end(), Text.begin() + 12);
    F.Image.ExceptionMetadata = {};
    coff_loader::parseX86RegistrationExceptions(F.Image);
    const auto Low = registration_test::liftSharedSEH4(F.Image);
    ASSERT_TRUE(Low.RegistrationStates);
    const auto &States = *Low.RegistrationStates;
    EXPECT_EQ(States.Complete, Argument == 37);
    EXPECT_TRUE(States.HasBoundSEHCallees);
    if (Argument == 37) {
      EXPECT_TRUE(States.SEHBoundCallsComplete);
      const auto Med =
          LowToMedConverter().convert(Low, Arch::X86, BinaryFormat::COFF);
      const auto Bound = getX86RegistrationSEHBoundCalls(Med);
      ASSERT_TRUE(Bound);
      ASSERT_EQ(Bound->size(), 1u);
      EXPECT_EQ(Bound->begin()->second.Source->ConstantReturn, 37u);
      EXPECT_TRUE(Bound->begin()->second.ObservesScalarReturn);
      for (unsigned Mutation = 0; Mutation != 6; ++Mutation) {
        SCOPED_TRACE(Mutation);
        auto Changed = Med;
        auto &Call = Changed.RegistrationStates->SEHBoundCalls.front();
        if (Mutation == 0)
          Call.ConstantReturn.reset();
        if (Mutation == 1)
          Call.HasOrdinaryReturn = false;
        if (Mutation == 2)
          Call.CalleeKind = RegistrationSEHBoundCall::Kind::ControlFP;
        if (Mutation == 3)
          Changed.SkippedSSA = true;
        for (auto &Block : Changed.Blocks)
          for (auto &Op : Block.Ops)
            if (Op.Addr == Call.Address && Op.Opcode == NdOp::CALL) {
              if (Mutation == 4)
                Op.Output.Size = 2;
              if (Mutation == 5) {
                Op.Output.Kind = MedVar::Reg;
                Op.Output.RegOff = x86reg::RDX;
              }
            }
        EXPECT_FALSE(getX86RegistrationSEHBoundCalls(Changed));
      }
    }
  }
}

TEST(RegistrationConditionalSEH, RelocatedArithmeticKeepsItsAddressIdentity) {
  for (uint32_t Base : {0x400000u, 0x18000000u})
    for (uint8_t Opcode : {uint8_t(0x05), uint8_t(0x2d)})
      for (unsigned Mode = 0; Mode != 13; ++Mode) {
        SCOPED_TRACE(Base);
        SCOPED_TRACE(Opcode);
        SCOPED_TRACE(Mode);
        ScalarSEHFixture F(Base,
                           {0x8b, 0x45, 8, Opcode, 0, 0, 0, 0, 0x90, 0x90});
        const va_t Field = Base + 0x1010;
        const uint32_t Target = Base + 0x2200;
        auto &Text = F.Image.Segments.front().Data;
        writeLE<uint32_t>(Text.data() + 16, Target);
        F.Image.BaseRelocations.push_back(
            {Field, llvm::COFF::IMAGE_REL_BASED_HIGHLOW});
        F.Image.DataAddressRelocOperands[Field] = {Target, Target, 4,
                                                   Base + 0x2000};
        F.Parent.store(-64, 4, FrameValue::imageAddress(Target + 64));
        if (Mode == 1)
          F.Image.DataAddressRelocOperands.clear();
        if (Mode == 2)
          ++F.Image.DataAddressRelocOperands[Field].EncodedValue;
        if (Mode == 3)
          F.Image.DataAddressRelocOperands[Field].TargetVA = 0x7fff0000;
        if (Mode == 4)
          F.Image.BaseRelocations.push_back(F.Image.BaseRelocations.front());
        if (Mode == 5)
          F.Image.BaseRelocations.front().Type =
              llvm::COFF::IMAGE_REL_BASED_DIR64;
        if (Mode == 6)
          --F.Image.BaseRelocations.front().Address;
        if (Mode == 7)
          F.Image.Segments.front().Flags =
              F.Image.Segments.front().Flags | SegmentFlags::Writable;
        if (Mode == 8)
          F.Image.Sections.front().Flags =
              F.Image.Sections.front().Flags | SegmentFlags::Writable;
        if (Mode == 9)
          F.Image.DataAddressRelocOperands[Field].PCRelativeFromInstructionEnd =
              true;
        if (Mode == 10)
          F.Image.CodeAddressRelocOperands[Field] =
              F.Image.DataAddressRelocOperands[Field];
        if (Mode == 11)
          Text[15] = 0xe8; // A control displacement is not an absolute operand.
        if (Mode == 12) {
          Text[20] = 0x8b;
          Text[21] = 0x00; // No storage proof follows from the arithmetic.
        }
        Decoder Dec;
        ASSERT_TRUE(Dec.init(F.Image));
        LowInstructionBoundary Boundary;
        Boundary.Address = Field - 1;
        Boundary.Size = 5;
        EXPECT_EQ(registration_abi::hasImmutableCalleeInstruction(
                      F.Image, Boundary, Dec),
                  Mode == 0 || Mode == 12);
        size_t Work = 0;
        const auto Effect = proveBoundSEHCallee(F.Image, F.Call, F.Parent, Work,
                                                nullptr, &F.Initialized);
        ASSERT_EQ(bool(Effect), Mode == 0);
        if (!Effect)
          continue;
        // Even subtraction of addresses in this one source data segment
        // retains an unknown result; independently emitted objects may move.
        EXPECT_EQ(Effect->ReturnValue, FrameValue{});
        EXPECT_TRUE(Effect->Reads.empty());
        EXPECT_TRUE(Effect->Writes.empty());
        ASSERT_LT(Work, limits::kMaxRegistrationEHStateWork);
        size_t Exact = limits::kMaxRegistrationEHStateWork - Work;
        EXPECT_TRUE(proveBoundSEHCallee(F.Image, F.Call, F.Parent, Exact,
                                        nullptr, &F.Initialized));
        EXPECT_EQ(Exact, limits::kMaxRegistrationEHStateWork);
        size_t Short = limits::kMaxRegistrationEHStateWork - Work + 1;
        EXPECT_FALSE(proveBoundSEHCallee(F.Image, F.Call, F.Parent, Short,
                                         nullptr, &F.Initialized));
      }
}

TEST(RegistrationConditionalSEH,
     InvocationJoinsRetainOnlyACommonConstantReturn) {
  for (bool Same : {false, true})
    for (bool Observed : {false, true}) {
      SCOPED_TRACE(Same);
      SCOPED_TRACE(Observed);
      ScalarSEHFixture F(0x400000, {0x8b, 0x45, 8});
      auto &Text = F.Image.Segments.front().Data;
      std::copy_n(Text.begin(), F.ReturnOffset + 1, Text.begin() + 0x300);
      writeLE<uint32_t>(Text.data() + 0x308, uint32_t(0x100 - 0x30c));
      const size_t CalleeExit = 0x300 + F.ReturnOffset - 5;
      writeLE<uint32_t>(Text.data() + CalleeExit + 1,
                        uint32_t(0x150 - CalleeExit - 5));
      std::fill(Text.begin() + 12, Text.begin() + 0x80, 0xcc);
      // Different live scope levels keep both actual argument states at the
      // same call. The native signature is shared; its returned value need not
      // be.
      std::vector<uint8_t> Body = {
          0x85, 0xc9, 0x74, 11,   0xc7, 0x45, 0xfc, 0,
          0,    0,    0,    0x6a, 37,   0xeb, 9,    0xc7,
          0x45, 0xfc, 0xfe, 0xff, 0xff, 0xff, 0x6a, uint8_t(Same ? 37 : 38)};
      const auto Call = [&](size_t Target) {
        const size_t At = 12 + Body.size();
        Body.push_back(0xe8);
        Body.resize(Body.size() + 4);
        writeLE<uint32_t>(Body.data() + Body.size() - 4,
                          uint32_t(Target - At - 5));
      };
      Call(0x300);
      Body.insert(Body.end(), {0x83, 0xc4, 4});
      if (Observed)
        Body.insert(Body.end(), {0x83, 0xc0, 1});
      else
        Body.insert(Body.end(), {0x31, 0xc0});
      Body.insert(Body.end(), {0xc7, 0x45, 0xfc, 0xfe, 0xff, 0xff, 0xff});
      Call(0x150);
      Body.push_back(0xc3);
      std::copy(Body.begin(), Body.end(), Text.begin() + 12);
      F.Image.ExceptionMetadata = {};
      coff_loader::parseX86RegistrationExceptions(F.Image);
      const auto Low = registration_test::liftSharedSEH4(F.Image);
      ASSERT_TRUE(Low.RegistrationStates);
      const auto &States = *Low.RegistrationStates;
      ASSERT_TRUE(States.Complete);
      ASSERT_TRUE(States.SEHBoundCallsComplete);
      ASSERT_EQ(States.SEHBoundCalls.size(), 1u);
      EXPECT_EQ(States.SEHBoundCalls.front().ConstantReturn,
                Same ? std::optional<uint32_t>{37} : std::nullopt);
      const auto Med =
          LowToMedConverter().convert(Low, Arch::X86, BinaryFormat::COFF);
      const auto Bound = getX86RegistrationSEHBoundCalls(Med);
      EXPECT_EQ(bool(Bound), Same || !Observed);
      if (Bound)
        EXPECT_EQ(Bound->begin()->second.ObservesScalarReturn, Observed);
    }
}

TEST(RegistrationConditionalSEH, OriginalImageDifferenceStaysScoped) {
  for (uint32_t Base : {0x400000u, 0x18000000u}) {
    SCOPED_TRACE(Base);
    ScalarSEHFixture F(Base, {0x8b, 0x45, 8, 0x2d, 0, 0, 0, 0});
    const va_t Field = Base + 0x1010;
    const uint32_t Target = Base + 0x2200;
    writeLE<uint32_t>(F.Image.Segments.front().Data.data() + 16, Target);
    F.Image.BaseRelocations.push_back(
        {Field, llvm::COFF::IMAGE_REL_BASED_HIGHLOW});
    F.Image.DataAddressRelocOperands[Field] = {Target, Target, 4,
                                               Base + 0x2000};
    F.Parent.store(-64, 4, FrameValue::imageAddress(Target + 64));
    size_t Work = 0;
    const auto Scoped = proveBoundOriginalImageSEHCallee(
        F.Image, F.Call, F.Parent, Work, &F.Initialized);
    ASSERT_TRUE(Scoped);
    EXPECT_EQ(Scoped->ReturnValue, 64u);
    EXPECT_FALSE(Scoped->Call.ConstantReturn);
    EXPECT_EQ(Scoped->Arguments.at(-64), FrameValue::imageAddress(Target + 64));
    EXPECT_EQ(Scoped->Addresses, (std::set<uint32_t>{Target, Target + 64}));
    EXPECT_FALSE(Scoped->Code.empty());
    EXPECT_TRUE(Scoped->Reads.empty());
    ASSERT_LT(Work, limits::kMaxRegistrationEHStateWork);
    size_t Exact = limits::kMaxRegistrationEHStateWork - Work;
    EXPECT_TRUE(proveBoundOriginalImageSEHCallee(F.Image, F.Call, F.Parent,
                                                 Exact, &F.Initialized));
    EXPECT_EQ(Exact, limits::kMaxRegistrationEHStateWork);
    size_t Short = limits::kMaxRegistrationEHStateWork - Work + 1;
    EXPECT_FALSE(proveBoundOriginalImageSEHCallee(F.Image, F.Call, F.Parent,
                                                  Short, &F.Initialized));
    size_t OrdinaryWork = 0;
    const auto Ordinary = proveBoundSEHCallee(
        F.Image, F.Call, F.Parent, OrdinaryWork, nullptr, &F.Initialized);
    ASSERT_TRUE(Ordinary);
    EXPECT_EQ(Ordinary->ReturnValue, FrameValue{});
    ASSERT_TRUE(Ordinary->NativeCall);
    EXPECT_FALSE(Ordinary->NativeCall->ConstantReturn);
  }
}

TEST(RegistrationConditionalSEH, OriginalImageReadRecordsExactInputsAndBytes) {
  for (uint32_t Base : {0x400000u, 0x18000000u})
    for (unsigned Mode = 0; Mode != 7; ++Mode) {
      SCOPED_TRACE(Base);
      SCOPED_TRACE(Mode);
      ScalarSEHFixture F(Base, {0x8b, 0x45, 8, 0x8b, 0});
      const uint32_t Address = Base + 0x2300;
      F.Parent.store(-64, 4, FrameValue::imageAddress(Address));
      writeLE<uint32_t>(F.Image.Segments.back().Data.data() + 0x300, 37u);
      if (Mode == 1)
        F.Image.Segments.back().Flags =
            SegmentFlags::Readable | SegmentFlags::Writable;
      if (Mode == 2)
        writeLE<uint32_t>(F.Image.Segments.back().Data.data() + 0x300, Address);
      if (Mode == 3)
        F.Image.BaseRelocations.push_back(
            {Address, llvm::COFF::IMAGE_REL_BASED_HIGHLOW});
      if (Mode == 4)
        F.Parent.store(-64, 4, FrameValue{});
      if (Mode == 5)
        F.Initialized.erase(-61);
      if (Mode == 6)
        ++F.Call.Addr;
      size_t Work = 0;
      const auto Scoped = proveBoundOriginalImageSEHCallee(
          F.Image, F.Call, F.Parent, Work, &F.Initialized);
      ASSERT_EQ(bool(Scoped), Mode == 0);
      if (!Scoped)
        continue;
      EXPECT_EQ(Scoped->ReturnValue, 37u);
      EXPECT_FALSE(Scoped->Call.ConstantReturn);
      EXPECT_EQ(Scoped->Arguments.at(-64), FrameValue::imageAddress(Address));
      ASSERT_EQ(Scoped->Reads.size(), 1u);
      const auto &Read = Scoped->Reads.at({Address, 4});
      EXPECT_EQ(Read.Value, 37u);
      ASSERT_EQ(Read.Bytes.size(), 4u);
      EXPECT_EQ(readLE<uint32_t>(Read.Bytes.data()), 37u);
      size_t OrdinaryWork = 0;
      EXPECT_FALSE(proveBoundSEHCallee(F.Image, F.Call, F.Parent, OrdinaryWork,
                                       nullptr, &F.Initialized));
    }
}

TEST(RegistrationConditionalSEH, CandidateReplayBindsTheOriginalCall) {
  for (uint32_t Base : {0x400000u, 0x18000000u})
    for (unsigned Mode = 0; Mode != 11; ++Mode) {
      SCOPED_TRACE(::testing::Message() << Base << " " << Mode);
      ScalarSEHFixture F(Base, {0x8b, 0x45, 8, 0x8b, 0, 0x25, 1, 0, 0, 0});
      const uint32_t Address = Base + 0x2300;
      F.Parent.store(-64, 4, FrameValue::imageAddress(Address));
      writeLE<uint32_t>(F.Image.Segments.back().Data.data() + 0x300, 37u);
      auto Candidate = F.Image;
      if (Mode)
        Candidate.Segments.front().Data[0x300] = 0xe9;
      if (Mode == 2 || Mode == 3)
        writeLE<uint32_t>(Candidate.Segments.back().Data.data() + 0x300,
                          Mode == 2 ? 39u : 38u);
      if (Mode == 4)
        Candidate.Segments.front().Data[18] = 3; // Same scalar, different code.
      if (Mode == 5) {
        F.Image.Segments.front().Data[0x300] = 0xe9;
        Candidate.Segments.front().Data[0x300] = 0xe8;
      }
      if (Mode == 6)
        Candidate.Base += 0x1000;
      if (Mode == 7)
        F.Initialized.erase(-61);
      if (Mode == 8)
        Candidate.Segments.back().Flags =
            SegmentFlags::Readable | SegmentFlags::Writable;
      if (Mode == 9)
        Candidate.BaseRelocations.push_back(
            {Address, llvm::COFF::IMAGE_REL_BASED_HIGHLOW});
      if (Mode == 10) {
        auto Tainted = FrameValue::imageAddress(Address);
        Tainted.MayBeFrame = true;
        F.Parent.store(-64, 4, Tainted);
      }
      size_t Work = 0;
      const auto Replay = replayBoundOriginalImageSEHCallee(
          F.Image, Candidate, F.Call, F.Parent, Work, &F.Initialized);
      ASSERT_EQ(bool(Replay), Mode < 3);
      if (!Replay)
        continue;
      EXPECT_EQ(Replay->Original.ReturnValue, 1u);
      EXPECT_EQ(Replay->Candidate.ReturnValue, 1u);
      EXPECT_FALSE(Replay->Original.Call.ConstantReturn);
      EXPECT_FALSE(Replay->Candidate.Call.ConstantReturn);
      EXPECT_EQ(Replay->Original.Code, Replay->Candidate.Code);
      EXPECT_EQ(Replay->Original.Arguments, Replay->Candidate.Arguments);
      EXPECT_EQ(Replay->Original.Reads.at({Address, 4}).Value, 37u);
      EXPECT_EQ(Replay->Candidate.Reads.at({Address, 4}).Value,
                Mode == 2 ? 39u : 37u);
      if (Mode) {
        size_t CandidateWork = 0;
        EXPECT_FALSE(proveBoundOriginalImageSEHCallee(
            Candidate, F.Call, F.Parent, CandidateWork, &F.Initialized));
      }
    }
}

TEST(RegistrationConditionalSEH, CandidateReplaySharesOneWorkBudget) {
  ScalarSEHFixture F(0x400000, {0x8b, 0x45, 8, 0x8b, 0});
  F.Parent.store(-64, 4, FrameValue::imageAddress(0x402300));
  writeLE<uint32_t>(F.Image.Segments.back().Data.data() + 0x300, 37u);
  auto Candidate = F.Image;
  Candidate.Segments.front().Data[0x300] = 0xe9;
  size_t Required = 0;
  ASSERT_TRUE(replayBoundOriginalImageSEHCallee(
      F.Image, Candidate, F.Call, F.Parent, Required, &F.Initialized));
  ASSERT_GT(Required, 0u);
  for (size_t Available : {size_t{0}, Required - 1, Required}) {
    size_t Work = limits::kMaxRegistrationEHStateWork - Available;
    EXPECT_EQ(replayBoundOriginalImageSEHCallee(F.Image, Candidate, F.Call,
                                                F.Parent, Work, &F.Initialized)
                  .has_value(),
              Available == Required);
    EXPECT_EQ(Work, limits::kMaxRegistrationEHStateWork);
    EXPECT_FALSE(replayBoundOriginalImageSEHCallee(
        F.Image, Candidate, F.Call, F.Parent, Work, &F.Initialized));
  }
}
