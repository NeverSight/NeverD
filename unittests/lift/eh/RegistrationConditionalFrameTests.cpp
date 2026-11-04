//===- RegistrationConditionalFrameTests.cpp - Bound SEH inputs -----------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Check invocation-specific SEH frame inputs and bounded effects.

#include "../../../lib/ir/low/X86/RegistrationABIPrivate.h"
#include "../../../lib/ir/low/X86/RegistrationCRTFrame.h"
#include "../../../lib/ir/low/X86/RegistrationFrameEffects.h"
#include "../../../lib/ir/low/X86/RegistrationStateSolver.h"
#include "gtest/gtest.h"

#include "neverd/Limits.h"
#include "neverd/ir/low/CFGBuilder.h"
#include "neverd/ir/med/LowToMed.h"
#include "neverd/ir/med/X86/X86RegistrationSEHCall.h"
#include "neverd/lift/X86/X86Regs.h"
#include "neverd/loader/COFF/COFFLoader.h"
#include "neverd/pipeline/Pipeline.h"
#include "neverd/support/BinaryEncoding.h"

#include "llvm/BinaryFormat/COFF.h"

#include <cstdint>
#include <initializer_list>
#include <map>
#include <optional>
#include <set>
#include <vector>

using namespace neverd;

TEST(RegistrationConditionalFrame, VolatileInputsDoNotRequireStackArguments) {
  using namespace registration_state;
  for (va_t Base : {0x400000u, 0x18000000u})
    for (unsigned Register : {x86reg::RAX, x86reg::RCX, x86reg::RDX})
      for (unsigned Mode = 0; Mode != 14; ++Mode) {
        SCOPED_TRACE(::testing::Message()
                     << Base << " " << Register << " " << Mode);
        const unsigned Index = Register / x86reg::GeneralRegStride;
        BinaryImage Image;
        Image.Arch = Arch::X86;
        Image.Bits = Bitness::Bits32;
        Image.Format = BinaryFormat::COFF;
        Image.Base = Base;
        Image.Entry = Base + 0x1000;
        Segment Text;
        Text.VA = Image.Entry;
        Text.Flags = SegmentFlags::Readable | SegmentFlags::Executable;
        if (Mode >= 10 && Mode <= 12)
          // A zero input bypasses a load through a different, unbound carrier.
          Text.Data = {0x85, uint8_t(0xc0 | (Index << 3) | Index),
                       0x74, 2,
                       0x8b, uint8_t((Index + 1) % 3)};
        if (Mode == 13)
          Text.Data = {0x31, uint8_t(0xc0 | (Index << 3) | Index)};
        Text.Data.insert(Text.Data.end(),
                         {0x8b, uint8_t((Mode == 9 ? 0 : 0xc0) | Index), 0xc3});
        Text.Size = Text.FileSz = Text.Data.size();
        if (Mode == 8)
          Text.Flags = Text.Flags | SegmentFlags::Writable;
        Section Code;
        Code.VA = Text.VA;
        Code.Size = Code.FileSz = Text.Size;
        Code.Flags = Text.Flags;
        Image.Sections.push_back(Code);
        Image.Segments.push_back(Text);
        FrameState Parent;
        for (auto &Value : Parent.Registers)
          Value = FrameValue::taint(true);
        Parent.Registers[x86reg::RSP / x86reg::GeneralRegStride] =
            FrameValue::frame(-64);
        auto Input = Mode == 0 || Mode == 10 ? FrameValue::constant(0)
                     : Mode == 2 ? FrameValue::imageAddress(Base + 0x3000)
                     : Mode == 3 || Mode == 9 || Mode == 12 ? FrameValue{}
                     : Mode == 11 ? FrameValue::constant(1)
                                  : FrameValue::constant(37);
        if (Mode < 4 || Mode >= 8)
          Parent.Registers[Index] = Input;
        if (Mode == 5 || Mode == 6) {
          LowOp Partial;
          Partial.Opcode = NdOp::COPY;
          Partial.Output = NdVar::reg(Register, Mode == 5 ? 1 : 2);
          FrameTransfer(Parent, 0).write(Partial, FrameValue::constant(37));
          EXPECT_TRUE(Parent.Registers[Index].MayBeFrame);
        }
        if (Mode == 7)
          Parent.Registers[x86reg::RBX / x86reg::GeneralRegStride] = Input;
        LowOp Call;
        Call.Opcode = NdOp::CALL;
        Call.addInput(NdVar::cst(Image.Entry, 4));
        const std::set<int32_t> Initialized;
        size_t Work = 0;
        std::optional<FrameValue> Returned = FrameValue::constant(UINT32_MAX);
        const bool Valid = registration_abi::hasConditionalCallFrame(
            Image, Call, Parent, Work, &Initialized, &Returned);
        const bool Expected = Mode < 4 || Mode == 10 || Mode == 13;
        EXPECT_EQ(Valid, Expected);
        if (!Expected) {
          EXPECT_FALSE(Returned);
          continue;
        }
        EXPECT_EQ(Returned, Mode == 13 ? FrameValue::constant(0) : Input);
        ASSERT_LT(Work, limits::kMaxRegistrationEHStateWork);
        size_t Exact = limits::kMaxRegistrationEHStateWork - Work;
        EXPECT_TRUE(registration_abi::hasConditionalCallFrame(
            Image, Call, Parent, Exact, &Initialized, &Returned));
        EXPECT_EQ(Exact, limits::kMaxRegistrationEHStateWork);
        size_t Limited = limits::kMaxRegistrationEHStateWork - Work + 1;
        EXPECT_FALSE(registration_abi::hasConditionalCallFrame(
            Image, Call, Parent, Limited, &Initialized, &Returned));
        EXPECT_FALSE(Returned);
      }
}

TEST(RegistrationConditionalFrame, PrivateRegisterBindingsKeepTheEntryABI) {
  using namespace registration_state;
  BinaryImage Image;
  Image.Arch = Arch::X86;
  Image.Bits = Bitness::Bits32;
  Image.Format = BinaryFormat::COFF;
  Image.Base = 0x400000;
  Image.Entry = Image.Base + 0x1000;
  Segment Text;
  Text.VA = Image.Entry;
  Text.Flags = SegmentFlags::Readable | SegmentFlags::Executable;
  Text.Data = {0xc3};
  Text.Size = Text.FileSz = Text.Data.size();
  Section Code;
  Code.VA = Text.VA;
  Code.Size = Code.FileSz = Text.Size;
  Code.Flags = Text.Flags;
  Image.Sections.push_back(Code);
  Image.Segments.push_back(Text);
  Decoder Dec;
  ASSERT_TRUE(Dec.init(Image));
  const auto Callee = CFGBuilder().buildIntegerFrameProof(
      Image, Dec, Image.Entry, "bound-registers");
  ASSERT_TRUE(Callee.hasCompleteLiftCoverage());
  const std::map<int32_t, FrameValue> Arguments;
  for (unsigned Index : {0u, 1u, 2u, 3u, 4u, 5u, 6u, 7u, 8u, UINT32_MAX})
    for (unsigned Mode = 0; Mode != 3; ++Mode) {
      SCOPED_TRACE(::testing::Message() << Index << " " << Mode);
      const std::map<unsigned, FrameValue> Registers{
          {Index,
           Mode == 1 ? FrameValue::frame(-16) : FrameValue::constant(37)}};
      std::optional<FrameValue> Returned = FrameValue::constant(99);
      registration_abi::ImageFrameEffects Effects;
      size_t Work = 0;
      const bool Valid = registration_abi::hasPrivateCallerFrame(
          Callee, Image, Work, Effects,
          {.ScalarArguments = Mode == 2 ? nullptr : &Arguments,
           .ScalarRegisters = &Registers,
           .ReturnValue = &Returned});
      const bool Expected = Index < 3 && Mode == 0;
      EXPECT_EQ(Valid, Expected);
      EXPECT_EQ(Returned.has_value(), Expected);
      if (Valid) {
        ASSERT_TRUE(Returned);
        EXPECT_EQ(Returned->Constant,
                  Index == 0 ? std::optional<uint32_t>(37) : std::nullopt);
      }
    }
}

TEST(RegistrationConditionalFrame,
     ReturnsKeepTheirInvocationAndAddressIdentity) {
  using namespace registration_state;
  for (va_t Base : {0x400000u, 0x18000000u})
    for (unsigned Mode = 0; Mode != 11; ++Mode) {
      SCOPED_TRACE(Base);
      SCOPED_TRACE(Mode);
      BinaryImage Image;
      Image.Arch = Arch::X86;
      Image.Bits = Bitness::Bits32;
      Image.Format = BinaryFormat::COFF;
      Image.Base = Base;
      Image.Entry = Base + 0x1000;
      Segment Text;
      Text.VA = Image.Entry;
      Text.Flags = SegmentFlags::Readable | SegmentFlags::Executable;
      Text.Data = {0x55, 0x8b, 0xec};                 // push ebp; mov ebp,esp
      std::vector<uint8_t> Body = {0x8b, 0x45, 0x08}; // return arg0
      if (Mode == 3)
        Body.clear(); // Incoming EAX belongs to another invocation.
      if (Mode == 4)
        Body = {0x8b, 0x45, 0x04}; // This callee's return PC.
      if (Mode == 5)
        Body = {0x8d, 0x45, 0xf8}; // Escaping private-frame address.
      if (Mode == 6 || Mode == 7)
        // The unknown second argument selects arg0 or arg0+4. Neither arm
        // alone may supply the summary, regardless of block visitation order.
        Body = {0x8b, 0x45, 0x08, 0x83, 0x7d, 0x0c,
                0x00, 0x74, 0x03, 0x83, 0xc0, 0x04};
      if (Mode == 10)
        Body = {0xeb, 0xfe}; // A closed loop supplies no ordinary return.
      Text.Data.insert(Text.Data.end(), Body.begin(), Body.end());
      if (Mode != 10)
        Text.Data.insert(Text.Data.end(), {0x5d, 0xc3});
      Text.Size = Text.FileSz = Text.Data.size();
      if (Mode == 9)
        Text.Flags = Text.Flags | SegmentFlags::Writable;
      Section Code;
      Code.VA = Text.VA;
      Code.Size = Code.FileSz = Text.Size;
      Code.Flags = Text.Flags;
      Image.Sections.push_back(Code);
      Image.Segments.push_back(Text);
      LowOp Call;
      Call.Opcode = NdOp::CALL;
      Call.addInput(NdVar::cst(Image.Entry, 4));
      FrameState Parent;
      for (auto &Value : Parent.Registers)
        Value = FrameValue::taint(true);
      Parent.Registers[x86reg::RSP / x86reg::GeneralRegStride] =
          FrameValue::frame(-64);
      const auto Argument = Mode == 1 || Mode == 6
                                ? FrameValue::imageAddress(Base + 0x3000)
                            : Mode == 2 ? FrameValue{}
                                        : FrameValue::constant(37);
      Parent.store(-64, 4, Argument);
      std::set<int32_t> Initialized;
      for (int32_t Byte = -64; Byte != -56; ++Byte)
        Initialized.insert(Byte);
      if (Mode == 8)
        Initialized.erase(-61);
      size_t Work = 0;
      std::optional<FrameValue> Returned = FrameValue::constant(UINT32_MAX);
      const bool Valid = registration_abi::hasConditionalCallFrame(
          Image, Call, Parent, Work, &Initialized, &Returned);
      EXPECT_EQ(Valid, Mode != 5 && Mode != 8 && Mode != 9);
      if (!Valid || Mode == 10) {
        EXPECT_FALSE(Returned);
        continue;
      }
      ASSERT_TRUE(Returned);
      if (Mode == 3 || Mode == 4) {
        auto Opaque = FrameValue::taint(true);
        Opaque.FrameOnlyFromCall = true;
        EXPECT_EQ(*Returned, Opaque);
      } else if (Mode == 6 || Mode == 7)
        EXPECT_EQ(*Returned, FrameValue{});
      else
        EXPECT_EQ(*Returned, Argument);
      size_t Incomplete = limits::kMaxRegistrationEHStateWork - Work + 1;
      EXPECT_FALSE(registration_abi::hasConditionalCallFrame(
          Image, Call, Parent, Incomplete, &Initialized, &Returned));
      EXPECT_FALSE(Returned);
    }
}

TEST(RegistrationConditionalFrame, UnknownScalarArgumentsNeedInitializedBytes) {
  using namespace registration_state;
  for (va_t Base : {0x400000u, 0x18000000u})
    for (unsigned Mutation = 0; Mutation != 17; ++Mutation) {
      SCOPED_TRACE(Base);
      SCOPED_TRACE(Mutation);
      BinaryImage Image;
      Image.Arch = Arch::X86;
      Image.Bits = Bitness::Bits32;
      Image.Format = BinaryFormat::COFF;
      Image.Base = Base;
      Image.Entry = Base + 0x1000;
      Segment Text;
      Text.Name = ".text";
      Text.VA = Image.Entry;
      Text.Flags = SegmentFlags::Readable | SegmentFlags::Executable;
      // Search an immutable, count-prefixed table for an unknown scalar.
      // Either comparison outcome is safe; the table count bounds the loop.
      Text.Data = {0x55, 0x8b, 0xec, 0x56, 0x8b, 0x75, 0x08, 0x8b, 0x45,
                   0x0c, 0x8b, 0x0e, 0x8d, 0x56, 0x04, 0x85, 0xc9, 0x74,
                   0x0a, 0x39, 0x02, 0x74, 0x06, 0x83, 0xc2, 0x04, 0x49,
                   0x75, 0xf6, 0x31, 0xc0, 0x5e, 0x5d, 0xc3};
      Segment Data;
      Data.Name = ".rdata";
      Data.VA = Base + 0x3000;
      Data.Flags = SegmentFlags::Readable;
      Data.Data.resize(16);
      writeLE<uint32_t>(Data.Data.data(), 3);
      for (unsigned I = 1; I != 4; ++I)
        writeLE<uint32_t>(Data.Data.data() + I * 4, I * 7);
      for (auto *Segment : {&Text, &Data}) {
        Segment->Size = Segment->FileSz = Segment->Data.size();
        Section Section;
        Section.Name = Segment->Name;
        Section.VA = Segment->VA;
        Section.Size = Section.FileSz = Segment->Size;
        Section.Flags = Segment->Flags;
        Image.Sections.push_back(Section);
        Image.Segments.push_back(*Segment);
      }
      LowOp Call;
      Call.Opcode = NdOp::CALL;
      Call.NumInputs = 1;
      Call.Inputs[0] = NdVar::cst(Image.Entry, 4);
      FrameState Parent;
      Parent.Registers[x86reg::RSP / x86reg::GeneralRegStride] =
          FrameValue::frame(-64);
      Parent.store(-64, 4, FrameValue::imageAddress(Data.VA));
      Parent.store(-60, 4, {});
      ASSERT_FALSE(Parent.Cells.count(-60));
      std::set<int32_t> Initialized;
      for (int32_t Byte = -64; Byte != -56; ++Byte)
        Initialized.insert(Byte);
      if (Mutation == 1 || Mutation == 3 || Mutation == 13)
        Parent.store(-60, 4, FrameValue::constant(14));
      if (Mutation == 2)
        for (int32_t Byte = -60; Byte != -56; ++Byte)
          Parent.store(Byte, 1, {});
      if (Mutation >= 5 && Mutation <= 12)
        Initialized.erase(-64 + int32_t(Mutation - 5));
      if (Mutation == 13)
        Initialized.erase(-58);
      if (Mutation == 14)
        Parent.store(-59, 1, FrameValue::frame(-8));
      if (Mutation == 15)
        Parent.Registers[x86reg::RSP / x86reg::GeneralRegStride] =
            FrameValue::frame(-56);
      if (Mutation == 16)
        Parent.Registers[x86reg::RSP / x86reg::GeneralRegStride] =
            FrameValue::frame(INT32_MAX - 3);
      const auto *Bytes =
          Mutation == 3 || Mutation == 4 ? nullptr : &Initialized;
      size_t Work = 0;
      BoundPlainCalleeEffect Effect;
      EXPECT_EQ(registration_abi::hasConditionalCallFrame(
                    Image, Call, Parent, Work, Bytes, nullptr, &Effect),
                Mutation <= 3);
      size_t Exhausted = limits::kMaxRegistrationEHStateWork;
      EXPECT_FALSE(registration_abi::hasConditionalCallFrame(
          Image, Call, Parent, Exhausted, Bytes, nullptr, &Effect));
      if (Mutation <= 3) {
        ASSERT_GT(Work, 0u);
        size_t Incomplete = limits::kMaxRegistrationEHStateWork - Work + 1;
        EXPECT_FALSE(registration_abi::hasConditionalCallFrame(
            Image, Call, Parent, Incomplete, Bytes, nullptr, &Effect));
        size_t Unretained = 0;
        EXPECT_FALSE(registration_abi::hasConditionalCallFrame(
            Image, Call, Parent, Unretained, Bytes));
      }
    }
}

TEST(RegistrationConditionalFrame, BoundReadsKeepRuntimeCookieValuesOpaque) {
  using namespace registration_state;
  for (va_t Base : {0x400000u, 0x18000000u})
    for (uint16_t Width : {1, 2, 4})
      for (int32_t Offset : {-4, -1, 0, 1, 3, 4})
        for (bool Cookie : {false, true})
          for (bool Guard : {false, true}) {
            SCOPED_TRACE(::testing::Message()
                         << Base << " " << Width << " " << Offset << " "
                         << Cookie << " " << Guard);
            BinaryImage Image;
            Image.Arch = Arch::X86;
            Image.Bits = Bitness::Bits32;
            Image.Format = BinaryFormat::COFF;
            Image.Base = Base;
            Image.Entry = Base + 0x1000;
            Image.DynInfo.SecurityCookieRVA = Cookie ? 0x3008 : 0;
            Segment Text;
            Text.VA = Image.Entry;
            Text.Flags = SegmentFlags::Readable | SegmentFlags::Executable;
            if (Width == 4)
              Text.Data = {0x8b, 0x01}; // mov eax, [ecx]
            else
              Text.Data = {0x0f, uint8_t(Width == 1 ? 0xb6 : 0xb7), 0x01};
            if (Guard)
              // Initial zero bytes hide an otherwise unproved memory read.
              Text.Data.insert(Text.Data.end(),
                               {0x85, 0xc0, 0x74, 0x02, 0x8b, 0x02});
            Text.Data.push_back(0xc3);
            Segment Data;
            Data.VA = Base + 0x3000;
            Data.Flags = SegmentFlags::Readable;
            Data.Data.resize(32);
            for (auto *Segment : {&Text, &Data}) {
              Segment->Size = Segment->FileSz = Segment->Data.size();
              Section Section;
              Section.VA = Segment->VA;
              Section.Size = Section.FileSz = Segment->Size;
              Section.Flags = Segment->Flags;
              Image.Sections.push_back(Section);
              Image.Segments.push_back(*Segment);
            }
            FrameState Parent;
            Parent.Registers[x86reg::RSP / x86reg::GeneralRegStride] =
                FrameValue::frame(-64);
            Parent.Registers[x86reg::RCX / x86reg::GeneralRegStride] =
                FrameValue::imageAddress(uint32_t(Data.VA + 8 + Offset));
            Parent.Registers[x86reg::RDX / x86reg::GeneralRegStride] =
                FrameValue::taint(true);
            LowOp Call;
            Call.Opcode = NdOp::CALL;
            Call.addInput(NdVar::cst(Image.Entry, 4));
            size_t Work = 0;
            std::optional<FrameValue> Returned;
            BoundPlainCalleeEffect Effect;
            const bool Opaque = Cookie && Offset < 4 && Offset + Width > 0;
            const bool Valid = registration_abi::hasConditionalCallFrame(
                Image, Call, Parent, Work, nullptr, &Returned, &Effect);
            EXPECT_EQ(Valid, !(Opaque && Guard));
            if (!Valid) {
              EXPECT_FALSE(Returned);
              continue;
            }
            ASSERT_TRUE(Returned);
            EXPECT_EQ(Returned->Constant.has_value(), !Opaque);
            EXPECT_EQ(Effect.ReturnValue.Constant.has_value(), !Opaque);
            if (!Opaque)
              EXPECT_EQ(Returned, FrameValue::constant(0));
            ASSERT_EQ(Effect.ImageReads.size(), 1u);
            EXPECT_EQ(Effect.ImageReads.front().Begin, Data.VA + 8 + Offset);
            EXPECT_EQ(Effect.ImageReads.front().End,
                      Data.VA + 8 + Offset + Width);
          }
}

TEST(RegistrationConditionalFrame, BoundImmutableScansKeepTheirMemoryExtent) {
  using namespace registration_state;
  for (va_t Base : {0x400000u, 0x18000000u})
    for (unsigned Count : {0u, 1u, 3u})
      for (unsigned Mutation = 0; Mutation != 8; ++Mutation) {
        SCOPED_TRACE(Base);
        SCOPED_TRACE(Count);
        SCOPED_TRACE(Mutation);
        BinaryImage Image;
        Image.Arch = Arch::X86;
        Image.Bits = Bitness::Bits32;
        Image.Format = BinaryFormat::COFF;
        Image.Base = Base;
        Image.Entry = Base + 0x1000;
        Segment Text;
        Text.Name = ".text";
        Text.VA = Image.Entry;
        Text.Flags = SegmentFlags::Readable | SegmentFlags::Executable;
        // Sum count immutable words. The count, addresses and backedge must
        // be proved together; the mapping's capacity is not a loop bound.
        Text.Data = {0x55, 0x8b, 0xec, 0x56, 0x8b, 0x75, 0x08, 0x8b, 0x0e, 0x8d,
                     0x56, 0x04, 0x31, 0xc0, 0x85, 0xc9, 0x74, 0x08, 0x03, 0x02,
                     0x83, 0xc2, 0x04, 0x49, 0x75, 0xf8, 0x5e, 0x5d, 0xc3};
        Segment Data;
        Data.Name = ".rdata";
        Data.VA = Base + 0x3000;
        Data.Flags = SegmentFlags::Readable;
        Data.Data.resize(16);
        writeLE<uint32_t>(Data.Data.data(), Mutation == 5 ? 4 : Count);
        for (unsigned I = 1; I != 4; ++I)
          writeLE<uint32_t>(Data.Data.data() + I * 4, I * 7);
        for (auto *Segment : {&Text, &Data}) {
          Segment->Size = Segment->FileSz = Segment->Data.size();
          Section Section;
          Section.Name = Segment->Name;
          Section.VA = Segment->VA;
          Section.Size = Section.FileSz = Segment->Size;
          Section.Flags = Segment->Flags;
          Image.Sections.push_back(Section);
          Image.Segments.push_back(*Segment);
        }
        if (Mutation == 1) {
          Image.Segments[1].Flags = Image.Sections[1].Flags =
              SegmentFlags::Readable | SegmentFlags::Writable;
        }
        if (Mutation == 2)
          Image.Sections[1].FileSz = 0;
        if (Mutation == 3)
          Image.BaseRelocations.push_back(
              {Data.VA, llvm::COFF::IMAGE_REL_BASED_HIGHLOW});
        if (Mutation == 4)
          Image.Sections.push_back(Image.Sections[1]);
        if (Mutation == 6) {
          Image.Segments[0].Flags = Image.Sections[0].Flags =
              SegmentFlags::Readable | SegmentFlags::Executable |
              SegmentFlags::Writable;
        }
        LowOp Call;
        Call.Opcode = NdOp::CALL;
        Call.NumInputs = 1;
        Call.Inputs[0] = NdVar::cst(Image.Entry, 4);
        FrameState Parent;
        Parent.Registers[x86reg::RSP / x86reg::GeneralRegStride] =
            FrameValue::frame(-64);
        Parent.store(-64, 4,
                     Mutation == 7 ? FrameValue::frame(-32)
                                   : FrameValue::imageAddress(Data.VA));
        size_t Work = 0;
        std::optional<FrameValue> Returned = FrameValue::constant(UINT32_MAX);
        BoundPlainCalleeEffect Effect;
        EXPECT_EQ(registration_abi::hasConditionalCallFrame(
                      Image, Call, Parent, Work, nullptr, &Returned, &Effect),
                  Mutation == 0);
        if (Mutation == 0) {
          EXPECT_EQ(Returned,
                    FrameValue::constant(7 * Count * (Count + 1) / 2));
          EXPECT_EQ(Effect.ReturnValue, *Returned);
          ASSERT_FALSE(Effect.ImageReads.empty());
          for (const auto &Read : Effect.ImageReads) {
            EXPECT_GE(Read.Begin, Data.VA);
            EXPECT_LE(Read.End, Data.VA + (Count + 1) * 4);
          }
        } else
          EXPECT_FALSE(Returned);
        size_t Exhausted = limits::kMaxRegistrationEHStateWork;
        EXPECT_FALSE(registration_abi::hasConditionalCallFrame(
            Image, Call, Parent, Exhausted, nullptr, &Returned, &Effect));
        EXPECT_FALSE(Returned);
        if (Mutation == 0) {
          ASSERT_GT(Work, 0u);
          size_t Incomplete = limits::kMaxRegistrationEHStateWork - Work + 1;
          EXPECT_FALSE(registration_abi::hasConditionalCallFrame(
              Image, Call, Parent, Incomplete, nullptr, &Returned, &Effect));
          EXPECT_FALSE(Returned);
          size_t Unretained = 0;
          EXPECT_FALSE(registration_abi::hasConditionalCallFrame(
              Image, Call, Parent, Unretained, nullptr, &Returned));
          EXPECT_FALSE(Returned);
        }
      }
}

#ifdef NEVERD_BINARY_CORPUS_ROOT
namespace {
auto loadXCPT4(bool Selected = false) {
  COFFLoader Loader;
  if (Selected)
    Loader.restrictFunctions({0x401580});
  return Loader.load(
      std::string(NEVERD_BINARY_CORPUS_ROOT) +
      "/corpus/windows-eh/msvc/vs2019/x86/native/no-gs/o0/windows-seh-tests/"
      "xcpt4-msvc-x86-native-no-gs-o0.exe");
}
} // namespace

TEST(RegistrationConditionalFrame, BoundRaiseWrapperRetainsOnlyLocalProofs) {
  using namespace registration_state;
  auto Image = loadXCPT4();
  ASSERT_TRUE(bool(Image)) << llvm::toString(Image.takeError());
  LowOp Call;
  Call.Opcode = NdOp::CALL;
  Call.Addr = 0x401288;
  Call.Seq = 0;
  Call.addInput(NdVar::cst(0x401030, 4));
  FrameState Parent;
  Parent.Registers[x86reg::RSP / x86reg::GeneralRegStride] =
      FrameValue::frame(-64);
  Parent.store(-64, 4, FrameValue::constant(0xc0000005));
  const std::set<int32_t> Initialized{-64, -63, -62, -61};
  // The independent query cannot assume this cdecl argument is accessible.
  EXPECT_FALSE(getCheckedX86CalleeStackPop(*Image, 0x401030));
  size_t Work = 0;
  std::optional<FrameValue> Returned;
  EXPECT_TRUE(registration_abi::hasConditionalCallFrame(
      *Image, Call, Parent, Work, &Initialized, &Returned));
  ASSERT_TRUE(Returned);
  EXPECT_FALSE(Returned->Constant);
  EXPECT_TRUE(Returned->FrameOnlyFromCall);
  // Frame transport grants no closed call-free source effect: the outer SEH
  // solver still owns RaiseException's callback and runtime binding proofs.
  BoundPlainCalleeEffect Effect;
  EXPECT_FALSE(registration_abi::hasConditionalCallFrame(
      *Image, Call, Parent, Work, &Initialized, &Returned, &Effect));
  EXPECT_FALSE(Returned);
  EXPECT_FALSE(getCheckedX86CalleeStackPop(*Image, 0x401030));
}

TEST(RegistrationConditionalFrame,
     XCPT4CompletesWithinTheDefaultClosureBudget) {
  auto Image = loadXCPT4();
  ASSERT_TRUE(bool(Image)) << llvm::toString(Image.takeError());
  size_t Work = 0;
  RegistrationCallCalleeIndex Index(*Image, &Work);
  CFGBuilder Builder;
  Builder.setRegistrationCalleeIndex(&Index);
  Decoder Dec;
  ASSERT_TRUE(Dec.init(*Image));
  const auto Function = Builder.build(*Image, Dec, 0x401580, "main");
  ASSERT_TRUE(Function.RegistrationStates);
  const auto &States = *Function.RegistrationStates;
  for (const auto &Diagnostic : States.Diagnostics)
    ADD_FAILURE() << Diagnostic;
  EXPECT_TRUE(States.Complete);
  EXPECT_TRUE(States.CallbackStatesComplete);
  EXPECT_TRUE(States.RegistrationLifetimeComplete);
  EXPECT_TRUE(States.SEHInvocationExitsComplete);
  EXPECT_TRUE(States.HasSEHJumpCaptures);
  EXPECT_TRUE(States.SEHJumpResumptionsComplete);
  EXPECT_FALSE(States.SEHInvocationExits.empty());
  EXPECT_FALSE(States.SEHJumpResumptions.empty());
  EXPECT_LE(Work, limits::kMaxRegistrationEHStateWork);
}

TEST(RegistrationConditionalFrame,
     XCPT4SelectedFunctionRetainsItsScalarFilterRange) {
  auto Image = loadXCPT4(true);
  ASSERT_TRUE(bool(Image)) << llvm::toString(Image.takeError());
  size_t Work = 0;
  RegistrationCallCalleeIndex Index(*Image, &Work);
  CFGBuilder Builder;
  Builder.setRegistrationCalleeIndex(&Index);
  Decoder Dec;
  ASSERT_TRUE(Dec.init(*Image));
  const auto Function = Builder.build(*Image, Dec, 0x401580, "main");
  ASSERT_TRUE(Function.RegistrationStates);
  const auto &States = *Function.RegistrationStates;
  for (const auto &Diagnostic : States.Diagnostics)
    ADD_FAILURE() << Diagnostic;
  ASSERT_TRUE(States.Complete);
  const auto Ranges =
      registrationRangesWhere(States, [](int32_t Level) { return Level == 1; });
  ASSERT_TRUE(Ranges);
  ASSERT_EQ(Ranges->size(), 1u);
  EXPECT_LE(Work, limits::kMaxRegistrationEHStateWork);
  const auto Med =
      LowToMedConverter().convert(Function, Arch::X86, BinaryFormat::COFF);
  EXPECT_FALSE(Med.SkippedSSA);
  EXPECT_TRUE(getRegistrationSEHReturns(Med))
      << "native parent chain: " << States.SEHParentChainComplete;
}

TEST(RegistrationConditionalFrame,
     XCPT4SourcePipelineRetainsItsScalarFilterRange) {
  auto Image = loadXCPT4(true);
  ASSERT_TRUE(bool(Image)) << llvm::toString(Image.takeError());
  llvm::LLVMContext Context;
  PipelineOptions Options;
  Options.EmitDumpOutput = false;
  Options.OnlyFunctionEntries.insert(0x401580);
  const auto Result = Pipeline().run(*Image, Context, Options);
  ASSERT_TRUE(Result.Success) << Result.Error;
  ASSERT_EQ(Result.LowFuncs.size(), 1u);
  ASSERT_EQ(Result.MedFuncs.size(), 1u);
  // Pipeline function discovery exposes additional call targets to the
  // shared proof budget. A standalone CFGBuilder run missed this regression.
  for (const auto *States : {&Result.LowFuncs.front().RegistrationStates,
                             &Result.MedFuncs.front().RegistrationStates}) {
    ASSERT_TRUE(*States);
    for (const auto &Diagnostic : (*States)->Diagnostics)
      ADD_FAILURE() << Diagnostic;
    EXPECT_TRUE((*States)->Complete);
    const auto Ranges = registrationRangesWhere(
        **States, [](int32_t Level) { return Level == 1; });
    ASSERT_TRUE(Ranges);
    EXPECT_EQ(Ranges->size(), 1u);
  }
}

TEST(RegistrationConditionalFrame, XCPT4HelperChecksItsBoundIncomingCounter) {
  auto Image = loadXCPT4();
  ASSERT_TRUE(bool(Image)) << llvm::toString(Image.takeError());
  Decoder Dec;
  ASSERT_TRUE(Dec.init(*Image));
  CFGBuilder Builder;
  const auto Function =
      Builder.build(*Image, Dec, 0x401250, "conditional-counter");
  ASSERT_TRUE(Function.ExceptionMetadata &&
              Function.ExceptionMetadata->Registration);
  RegistrationCallCalleeIndex Index(*Image);
  const auto Stacks = Index.stackContracts(Function);
  const auto Unwinds = Index.localUnwindContracts(Function);
  const auto Controls = Index.sehControlContracts(Function);
  ASSERT_TRUE(Stacks && Unwinds && Controls);
  using namespace registration_state;
  for (unsigned Mutation = 0; Mutation != 7; ++Mutation) {
    SCOPED_TRACE(Mutation);
    auto Current = Function;
    ConditionalFrame Inputs;
    Inputs.Arguments = {{8, FrameValue::constant(0xc0000005)},
                        {12, FrameValue::frame(256)}};
    Inputs.Objects = {{256, 260}};
    Inputs.ObjectCells = {{256, FrameValue::constant(1)}};
    Inputs.ProveCall = [&](const LowOp &Op, const FrameState &State,
                           const std::set<int32_t> &Initialized,
                           std::optional<FrameValue> &Returned) {
      return Mutation != 4 &&
             registration_abi::hasConditionalCallFrame(
                 *Image, Op, State, Inputs.Work, &Initialized, &Returned);
    };
    if (Mutation == 1)
      Inputs.Objects[0].End = 259;
    if (Mutation == 2)
      Inputs.Arguments[12] = FrameValue::frame(4);
    if (Mutation == 3)
      Inputs.ObjectCells[256] = FrameValue::frame(300);
    if (Mutation == 5)
      Inputs.Arguments.erase(8);
    if (Mutation == 6) {
      // Leave the decoded slice intact, but replace the exception-code store
      // with a different local. The subsequent -32 read is uninitialized.
      bool Changed = false;
      for (auto &Block : Current.Blocks)
        for (auto &Op : Block.Ops)
          if (Op.Addr == 0x4012a0 && Op.Opcode == NdOp::INT_ADD &&
              Op.NumInputs == 2 && Op.Inputs[1].isConst()) {
            Op.Inputs[1].Offset = uint32_t(-36);
            Changed = true;
          }
      ASSERT_TRUE(Changed);
    }
    RegistrationStateSolver Solver(Current, 0, 0, nullptr, nullptr, &*Stacks,
                                   &*Unwinds, &*Controls, &Inputs);
    const auto States = Solver.run();
    if (Mutation == 0) {
      for (const auto &Diagnostic : States.Diagnostics)
        ADD_FAILURE() << Diagnostic;
      EXPECT_TRUE(States.Complete);
      EXPECT_TRUE(States.CallbackStatesComplete);
      EXPECT_TRUE(States.RegistrationLifetimeComplete);
      EXPECT_TRUE(Inputs.Complete);
      EXPECT_TRUE(Inputs.NativeFramePrivate);
      EXPECT_EQ(Inputs.Reads,
                (std::set<std::pair<int32_t, int32_t>>{{256, 260}}));
      EXPECT_EQ(Inputs.Writes,
                (std::set<std::pair<int32_t, int32_t>>{{256, 260}}));
    } else {
      EXPECT_FALSE(States.Complete && States.CallbackStatesComplete &&
                   States.RegistrationLifetimeComplete && Inputs.Complete);
    }
  }
}

TEST(RegistrationConditionalFrame,
     CallerCounterExtentAndOriginalCodeAreRequired) {
  using namespace registration_state;
  for (unsigned Mutation = 0; Mutation != 12; ++Mutation) {
    SCOPED_TRACE(Mutation);
    auto Image = loadXCPT4();
    ASSERT_TRUE(bool(Image)) << llvm::toString(Image.takeError());
    LowOp Call;
    Call.Opcode = NdOp::CALL;
    Call.Addr = 0x40215f;
    Call.Seq = 0;
    Call.NumInputs = 1;
    Call.Inputs[0] = NdVar::cst(0x401250, 4);
    FrameState Parent;
    Parent.Registers[x86reg::RSP / x86reg::GeneralRegStride] =
        FrameValue::frame(-308);
    Parent.Cells.set(-308, FrameValue::constant(0xc0000005));
    Parent.Cells.set(-304, FrameValue::frame(-28));
    Parent.Cells.set(-28, FrameValue::constant(1));
    size_t Work = 0;
    if (Mutation == 1)
      Parent.Cells.erase(-28);
    if (Mutation == 2)
      Parent.Cells.set(-304, FrameValue::frame(-20));
    if (Mutation == 3)
      Parent.Cells.set(-28, FrameValue::frame(-100));
    if (Mutation == 4)
      Parent.Cells.set(-304, FrameValue::frame(-304));
    if (Mutation == 5)
      Parent.Registers[x86reg::RSP / x86reg::GeneralRegStride] = {};
    if (Mutation == 6)
      Call.Inputs[0].Size = 8;
    if (Mutation == 7)
      Work = limits::kMaxRegistrationEHStateWork;
    if (Mutation == 8) {
      Image->Segments[0].Flags =
          Image->Segments[0].Flags | SegmentFlags::Writable;
      for (auto &Section : Image->Sections)
        if (Section.isExecutable())
          Section.Flags = Section.Flags | SegmentFlags::Writable;
    }
    if (Mutation == 9 || Mutation == 10) {
      auto *Bytes = const_cast<uint8_t *>(Image->readVA(0x401312, 1));
      ASSERT_TRUE(Bytes);
      *Bytes = Mutation == 9 ? 0xcb : 0x90; // far RET / no exact near RET
    }
    if (Mutation == 11)
      ++Call.Addr;
    const auto Effect = proveBoundSEHCallee(*Image, Call, Parent, Work);
    if (Mutation == 0) {
      ASSERT_TRUE(Effect);
      ASSERT_TRUE(Effect->NativeCall);
      EXPECT_EQ(Effect->NativeCall->Arguments.size(), 2u);
      EXPECT_EQ(Effect->StackPopBytes, 0u);
      ASSERT_EQ(Effect->Reads.size(), 1u);
      EXPECT_EQ(Effect->Reads[0].Begin, -28);
      EXPECT_EQ(Effect->Reads[0].End, -24);
      ASSERT_EQ(Effect->Writes.size(), 1u);
      EXPECT_EQ(Effect->Writes[0].Begin, -28);
      EXPECT_EQ(Effect->Writes[0].End, -24);
      EXPECT_GT(Work, 0u);
      // Repeat the same authenticated invocation with one work unit less
      // than its complete nested proof needs. No partial effect may escape.
      ASSERT_LT(Work, limits::kMaxRegistrationEHStateWork);
      size_t Limited = limits::kMaxRegistrationEHStateWork - Work + 1;
      EXPECT_FALSE(proveBoundSEHCallee(*Image, Call, Parent, Limited));
      EXPECT_EQ(Limited, limits::kMaxRegistrationEHStateWork);
    } else
      EXPECT_FALSE(Effect);
  }
}
TEST(RegistrationConditionalFrame, OriginalHelperResumesItsBorrowedCapture) {
  using namespace registration_state;
  for (unsigned Mutation = 0; Mutation != 8; ++Mutation) {
    SCOPED_TRACE(Mutation);
    auto Image = loadXCPT4();
    ASSERT_TRUE(bool(Image)) << llvm::toString(Image.takeError());
    LowOp Call;
    Call.Opcode = NdOp::CALL;
    Call.Addr = 0x40263d;
    Call.Seq = 0;
    Call.addInput(NdVar::cst(0x4011a0, 4));
    FrameState Parent;
    Parent.Registers[x86reg::RSP / x86reg::GeneralRegStride] =
        FrameValue::frame(-308);
    Parent.store(-308, 4, FrameValue::frame(-288));
    Parent.store(-304, 4, FrameValue::frame(-28));
    Parent.store(-28, 4, FrameValue::constant(1));
    SEHJumpCapture Capture;
    Capture.Call = 0x4025fe;
    Capture.ReturnPC = 0x402603;
    Capture.UnwindTarget = 0x407c78;
    Capture.BufferOffset = -288;
    Capture.StackOffset = -316;
    Capture.Nonvolatile[0] = FrameValue::frame(0);
    storeSEHJumpCapture(Parent, Capture);
    std::map<int32_t, SEHJumpCapture> Captures{{-288, Capture}};
    size_t Work = 0;
    if (Mutation == 1)
      Captures.clear();
    if (Mutation == 2)
      Parent.store(-268, 1, FrameValue::constant(0));
    if (Mutation == 3)
      Parent.store(-304, 4, FrameValue::frame(-268));
    if (Mutation == 4)
      for (auto &Import : Image->Imports)
        if (Import.Name == "_longjmpex")
          Import.Module = "application.dll";
    if (Mutation == 5) {
      auto *Byte = const_cast<uint8_t *>(Image->readVA(0x40122c, 1));
      ASSERT_TRUE(Byte);
      *Byte = 0x90;
    }
    if (Mutation == 6)
      Work = limits::kMaxRegistrationEHStateWork;
    if (Mutation == 7)
      ++Call.Addr;
    const auto Effect =
        proveBoundSEHCallee(*Image, Call, Parent, Work, &Captures);
    if (!Mutation) {
      ASSERT_TRUE(Effect);
      EXPECT_FALSE(Effect->HasOrdinaryReturn);
      ASSERT_TRUE(Effect->NativeCall);
      EXPECT_EQ(Effect->NativeCall->TargetVA, 0x4011a0u);
      EXPECT_FALSE(Effect->NativeCall->HasOrdinaryReturn);
      using Kind = RegistrationSEHBoundArgument::Kind;
      EXPECT_EQ(
          Effect->NativeCall->Arguments,
          (std::vector<RegistrationSEHBoundArgument>{
              {Kind::JumpBuffer, 0, Capture.Call}, {Kind::Object, -28, 0}}));
      ASSERT_EQ(Effect->JumpResumes.size(), 1u);
      EXPECT_EQ(Effect->JumpResumes.front().Address, 0x40122cu);
      EXPECT_EQ(Effect->JumpResumes.front().CaptureCall, Capture.Call);
      EXPECT_EQ(Effect->JumpResumes.front().Value.Constant, 1u);
      ASSERT_EQ(Effect->Writes.size(), 1u);
      EXPECT_EQ(Effect->Writes.front().Begin, -28);
      EXPECT_EQ(Effect->Writes.front().End, -24);
    } else
      EXPECT_FALSE(Effect);
  }
}
TEST(RegistrationCRTFrame, ControlFPRequiresItsImportAndPhysicalCall) {
  using namespace registration_state;
  for (unsigned Mutation = 0; Mutation != 11; ++Mutation) {
    SCOPED_TRACE(Mutation);
    auto Image = loadXCPT4();
    ASSERT_TRUE(bool(Image)) << llvm::toString(Image.takeError());
    LowOp Call;
    Call.Opcode = NdOp::INDIR_CALL;
    Call.Addr = 0x404014;
    Call.Seq = 0;
    Call.addInput(NdVar::cst(0x4080e0, 4));
    auto Import = llvm::find_if(
        Image->Imports, [](const auto &I) { return I.Name == "_controlfp_s"; });
    ASSERT_NE(Import, Image->Imports.end());
    if (Mutation == 1)
      Import->Module = "application.dll";
    if (Mutation == 2)
      Import->IsDelayImport = true;
    if (Mutation == 3)
      Image->ImportStorageSlots.clear();
    if (Mutation == 4)
      Call.MemoryAddressSpace = NdMemoryAddressSpace::X86FS;
    if (Mutation == 5)
      Call.Seq = -1;
    if (Mutation == 6) {
      auto *Byte = const_cast<uint8_t *>(Image->readVA(Call.Addr, 1));
      ASSERT_TRUE(Byte);
      *Byte = 0x90;
    }
    if (Mutation == 7)
      Call.Inputs[0].Size = 8;
    if (Mutation == 8) {
      for (auto &Segment : Image->Segments)
        if (Segment.isExecutable())
          Segment.Flags = Segment.Flags | SegmentFlags::Writable;
      for (auto &Section : Image->Sections)
        if (Section.isExecutable())
          Section.Flags = Section.Flags | SegmentFlags::Writable;
    }
    if (Mutation == 9)
      Image->Imports.push_back(
          {"application.dll", "_controlfp_s", 0, 0x4080e0});
    if (Mutation == 10)
      Import->Module = "MSVCR100.DLL";
    size_t Work = 0;
    EXPECT_EQ(isCheckedControlFPCall(*Image, Call, Work),
              Mutation == 0 || Mutation == 10);
  }
}
TEST(RegistrationCRTFrame, MathHelperStackFactsDoNotGrantMemoryEffects) {
  for (unsigned Mutation = 0; Mutation != 5; ++Mutation) {
    SCOPED_TRACE(Mutation);
    auto Image = loadXCPT4();
    ASSERT_TRUE(bool(Image)) << llvm::toString(Image.takeError());
    if (Mutation == 1 || Mutation == 2)
      for (auto &Import : Image->Imports)
        if (Import.Name == "log") {
          if (Mutation == 1)
            Import.Module = "application.dll";
          else
            Import.IsDelayImport = true;
        }
    if (Mutation == 3) {
      auto *Byte = const_cast<uint8_t *>(Image->readVA(0x407ccc, 1));
      ASSERT_TRUE(Byte);
      *Byte = 0x90;
    }
    size_t Work = Mutation == 4 ? limits::kMaxRegistrationEHStateWork : 0;
    const auto Pop = getCheckedX86CalleeStackPop(*Image, 0x407ccc, &Work);
    if (!Mutation) {
      ASSERT_TRUE(Pop);
      EXPECT_EQ(*Pop, 0u);
      EXPECT_EQ(getCheckedX86CalleeStackPop(*Image, 0x401540, &Work), 0u);
    } else
      EXPECT_FALSE(Pop);
  }
}

TEST(RegistrationCRTFrame, FloatingStateEffectsRequireTheirExactShape) {
  auto Image = loadXCPT4();
  ASSERT_TRUE(bool(Image)) << llvm::toString(Image.takeError());
  Decoder Dec;
  ASSERT_TRUE(Dec.init(*Image));
  const auto Function = CFGBuilder().build(*Image, Dec, 0x401540, "math");
  unsigned Checked = 0;
  for (const auto &Block : Function.Blocks)
    for (const auto &Original : Block.Ops) {
      if (Original.Opcode != NdOp::INTRINSIC || !Original.NumInputs ||
          !Original.Inputs[0].isConst())
        continue;
      const auto Id = static_cast<Intrinsic>(Original.Inputs[0].Offset);
      if (Id != Intrinsic::X86ReadMXCSR && Id != Intrinsic::X86WriteMXCSR &&
          Id != Intrinsic::X86FPArithState &&
          Id != Intrinsic::X86FPArithMemoryState)
        continue;
      ++Checked;
      for (unsigned Mutation = 0; Mutation != 7; ++Mutation) {
        SCOPED_TRACE(Mutation);
        auto Op = Original;
        if (Mutation == 1)
          Op.Inputs[0] = NdVar::reg(0, 2);
        if (Mutation == 2)
          Op.Inputs[0].Size = 4;
        if (Mutation == 3)
          Op.NumInputs = 0;
        if (Mutation == 4)
          Op.Output = NdVar::reg(x86reg::RSP, 4);
        if (Mutation == 5)
          Op.MemoryAddressSpace = NdMemoryAddressSpace::X86FS;
        if (Mutation == 6)
          Op.Inputs[0].Offset = static_cast<unsigned>(Intrinsic::X87Wait);
        EXPECT_EQ(registration_state::isFramePreservingIntrinsic(Op),
                  Mutation == 0);
      }
    }
  EXPECT_GT(Checked, 0u);
}
#endif
