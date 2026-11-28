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
#include "../../../lib/ir/low/X86/RegistrationSEHPrivateCall.h"
#include "../../../lib/ir/low/X86/RegistrationSourceCall.h"
#include "../../../lib/ir/low/X86/RegistrationStateSolver.h"
#include "gtest/gtest.h"

#include "neverd/Limits.h"
#include "neverd/ir/low/CFGBuilder.h"
#include "neverd/ir/med/LowToMed.h"
#include "neverd/ir/med/X86/X86RegistrationSEHCall.h"
#include "neverd/lift/X86/X86Regs.h"
#include "neverd/loader/COFF/COFFLoader.h"
#include "neverd/loader/COFF/COFFRegistrationEH.h"
#include "neverd/loader/COFF/COFFSEH4Entry.h"
#include "neverd/loader/ReadOnlyBytes.h"
#include "neverd/pipeline/Pipeline.h"
#include "neverd/support/BinaryEncoding.h"

#include "llvm/ADT/STLExtras.h"
#include "llvm/BinaryFormat/COFF.h"

#include <cstdint>
#include <initializer_list>
#include <map>
#include <optional>
#include <set>
#include <vector>

using namespace neverd;

TEST(RegistrationConditionalFrame,
     NestedDispatchRequiresOneAuthenticatedKnownRaise) {
  using namespace registration_state;
  for (va_t Base : {0x400000u, 0x18000000u})
    for (unsigned Mutation = 0; Mutation != 10; ++Mutation) {
      SCOPED_TRACE(::testing::Message() << Base << " " << Mutation);
      BinaryImage Image;
      Image.Arch = Arch::X86;
      Image.Bits = Bitness::Bits32;
      Image.Format = BinaryFormat::COFF;
      Image.Base = Base;
      Image.Entry = Base + 0x1000;
      Segment Text;
      Text.VA = Image.Entry;
      Text.Flags = SegmentFlags::Readable | SegmentFlags::Executable;
      Text.Data = {0xe8, 0x0b, 0, 0, 0, 0xc3};
      Text.Data.resize(16, 0xcc);
      Text.Data.insert(Text.Data.end(), {0x55, 0x8b, 0xec});
      if (Mutation != 9)
        for (unsigned Raise = 0; Raise != (Mutation == 3 ? 2u : 1u); ++Raise) {
          if (Raise)
            Text.Data.insert(Text.Data.end(), {0x31, 0xc9, 0x31, 0xd2});
          const std::vector<uint8_t> Body = {
              0x6a, uint8_t(Mutation == 8), // No argument array.
              0x6a, uint8_t(Mutation == 7), // No argument count.
              0x6a, uint8_t(Mutation == 2), // Continuable exception.
              0x8b, 0x45,
              0x08, 0x50,
              0xff, 0x15};
          Text.Data.insert(Text.Data.end(), Body.begin(), Body.end());
          const size_t Slot = Text.Data.size();
          Text.Data.resize(Slot + 4);
          writeLE<uint32_t>(Text.Data.data() + Slot, uint32_t(Base + 0x2000));
        }
      Text.Data.insert(Text.Data.end(), {0x31, 0xc0, 0x5d, 0xc3});
      Text.Size = Text.FileSz = Text.Data.size();
      Section Code;
      Code.VA = Text.VA;
      Code.Size = Code.FileSz = Text.Size;
      Code.Flags = Text.Flags;
      Image.Sections.push_back(Code);
      Image.Segments.push_back(Text);
      Segment Data;
      Data.VA = Base + 0x2000;
      Data.Flags = SegmentFlags::Readable | SegmentFlags::Writable;
      Data.Size = Data.FileSz = 4;
      Data.Data.resize(4);
      Image.Segments.push_back(Data);
      Section Storage;
      Storage.VA = Data.VA;
      Storage.Size = Storage.FileSz = Data.Size;
      Storage.Flags = Data.Flags;
      Image.Sections.push_back(Storage);
      Import Raise;
      Raise.Name = "RaiseException";
      Raise.Module = Mutation == 6 ? "untrusted.dll" : "kernel32.dll";
      Raise.IATAddr = Data.VA;
      Image.Imports.push_back(Raise);
      ASSERT_TRUE(Image.recordImportStorageSlot(
          Raise.IATAddr, Raise.Name, 0,
          ImportStorageEvidence::ImportDirectory));
      LowOp Call;
      Call.Opcode = NdOp::CALL;
      Call.Addr = Image.Entry + (Mutation == 4);
      Call.Seq = Mutation == 5 ? -1 : 0;
      Call.addInput(NdVar::cst(Image.Entry + 16, 4));
      FrameState Parent;
      Parent.Registers[x86reg::RSP / x86reg::GeneralRegStride] =
          FrameValue::frame(-64);
      const std::set<int32_t> Initialized{-64, -63, -62, -61};
      for (uint32_t Code : {0xe0000011u, 0xe0000012u, 0xe0000011u}) {
        SCOPED_TRACE(Code);
        Parent.store(-64, 4,
                     Mutation == 1 ? FrameValue{} : FrameValue::constant(Code));
        size_t Work = 0;
        ConditionalCallResult Result;
        const bool Valid = registration_abi::hasConditionalCallFrame(
            Image, Call, Parent, Work, &Initialized, nullptr, nullptr, nullptr,
            &Result);
        EXPECT_EQ(Valid, Mutation < 6 || Mutation == 9);
        EXPECT_EQ(bool(Result.Dispatch), Mutation == 0 || Mutation == 9);
        if (Result.Dispatch) {
          EXPECT_EQ(Result.StackPopBytes, 0u);
          EXPECT_EQ(Result.ReturnValue, FrameValue::constant(0));
          EXPECT_EQ(Result.Dispatch->ArgumentEnd, Mutation == 0 ? 12 : 8);
          ASSERT_EQ(bool(Result.Dispatch->Raise), Mutation == 0);
          if (Result.Dispatch->Raise) {
            EXPECT_EQ(Result.Dispatch->Raise->Address, Image.Entry + 29);
            EXPECT_EQ(Result.Dispatch->Raise->Code, Code);
          }
        }
        EXPECT_LT(Work, limits::kMaxRegistrationEHStateWork);
      }
    }
}

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

TEST(RegistrationCRTFrame, XCPT4StartupClockHelperKeepsItsPrivateOutputs) {
  auto Image = loadXCPT4(true);
  ASSERT_TRUE(bool(Image)) << llvm::toString(Image.takeError());
  size_t Work = 0;
  RegistrationCallCalleeIndex Callees(*Image, &Work);
  const auto Clock = Callees.stackContract(0x4071c6);
  ASSERT_TRUE(Clock);
  EXPECT_EQ(Clock->StackPopBytes, 0u);
  EXPECT_TRUE(Clock->PreservesImportBindings);
  EXPECT_EQ(Clock->RequiredImports,
            (std::vector<va_t>{0x408024, 0x408028, 0x40802c, 0x408030}));
  EXPECT_LT(Work, limits::kMaxRegistrationEHStateWork);
}

TEST(RegistrationCRTFrame, XCPT4StartupCookieWritesLeaveTheImportTableIntact) {
  struct Storage {
    va_t OverlappingSlot = 0;
    uint32_t ExtraFlags = 0;
  };
  const Storage Cases[] = {{},
                           {0x40b1b5},
                           {0x40b1b9},
                           {0, llvm::COFF::IMAGE_SCN_MEM_DISCARDABLE},
                           {0, llvm::COFF::IMAGE_SCN_MEM_SHARED}};
  for (bool Selected : {false, true}) {
    auto Original = loadXCPT4(Selected);
    ASSERT_TRUE(bool(Original)) << llvm::toString(Original.takeError());
    for (const auto &Case : Cases) {
      SCOPED_TRACE(::testing::Message()
                   << Selected << ' ' << Case.OverlappingSlot << ' '
                   << Case.ExtraFlags);
      auto Image = *Original;
      if (Case.OverlappingSlot) {
        ASSERT_FALSE(Image.Imports.empty());
        auto Binding = Image.Imports.front();
        Binding.IATAddr = Case.OverlappingSlot;
        Image.Imports.push_back(std::move(Binding));
      }
      if (Case.ExtraFlags) {
        const auto *Owner = Image.getSectionFor(0x40b1b8);
        ASSERT_TRUE(Owner);
        Image.Sections[Owner - Image.Sections.data()].Type |= Case.ExtraFlags;
      }
      size_t Work = 0;
      RegistrationCallCalleeIndex Callees(Image, &Work);
      const auto Cookie = Callees.stackContract(0x407213);
      if (Case.OverlappingSlot || Case.ExtraFlags) {
        EXPECT_FALSE(Cookie && Cookie->PreservesImportBindings);
        continue;
      }
      ASSERT_TRUE(Cookie);
      EXPECT_EQ(Cookie->StackPopBytes, 0u);
      EXPECT_TRUE(Cookie->PreservesImportBindings);
      EXPECT_EQ(Cookie->RequiredImports,
                (std::vector<va_t>{0x408024, 0x408028, 0x40802c, 0x408030}));
      EXPECT_LT(Work, limits::kMaxRegistrationEHStateWork);
    }
  }
}

TEST(RegistrationCRTFrame, XCPT4MainArgumentCallsBindTheirRuntimeSlots) {
  using namespace registration_state;
  auto Image = loadXCPT4(true);
  ASSERT_TRUE(bool(Image)) << llvm::toString(Image.takeError());
  Decoder Dec;
  ASSERT_TRUE(Dec.init(*Image));
  const auto Startup =
      CFGBuilder().buildIntegerFrameProof(*Image, Dec, 0x406c57, "CRT startup");
  const std::map<va_t, const char *> Expected = {
      {0x406d35, "_get_initial_narrow_environment"},
      {0x406d3c, "__p___argv"},
      {0x406d43, "__p___argc"}};
  size_t Work = 0;
  std::set<va_t> Seen;
  for (const auto &Block : Startup.Blocks) {
    for (const auto &Op : Block.Ops) {
      auto It = Expected.find(Op.Addr);
      if (Op.Opcode != NdOp::CALL || It == Expected.end())
        continue;
      SCOPED_TRACE(Op.Addr);
      ASSERT_TRUE(Seen.insert(Op.Addr).second);
      const auto Slot = checkedCRTCallSlot(*Image, Op, Work);
      ASSERT_TRUE(Slot);
      const auto *Import = Image->findImportAt(*Slot);
      ASSERT_TRUE(Import);
      EXPECT_EQ(Import->Name, It->second);
      EXPECT_EQ(
          registration_abi::checkedRegistrationImportStackPop(*Image, *Slot),
          0u);
      EXPECT_EQ(
          registration_abi::preservesRegistrationImportBindings(*Image, *Slot),
          Op.Addr != 0x406d35);
    }
  }
  EXPECT_EQ(Seen.size(), Expected.size());
}

TEST(RegistrationCRTFrame, XCPT4PrintfObtainsTheStandardOutputStream) {
  using namespace registration_state;
  auto Image = loadXCPT4(true);
  ASSERT_TRUE(bool(Image)) << llvm::toString(Image.takeError());
  Decoder Dec;
  ASSERT_TRUE(Dec.init(*Image));
  const auto Wrapper = CFGBuilder().buildIntegerFrameProof(
      *Image, Dec, 0x4045f0, "stdio wrapper");
  size_t Work = 0;
  for (const auto &Block : Wrapper.Blocks) {
    for (const auto &Op : Block.Ops) {
      if (Op.Opcode != NdOp::INDIR_CALL || Op.Addr != 0x404608)
        continue;
      const auto Slot = checkedCRTCallSlot(*Image, Op, Work);
      ASSERT_EQ(Slot, 0x4080fcu);
      EXPECT_TRUE(
          hasBindingPreservingCRTArgument(*Image, Block, Op, *Slot, Work));
      LowBlock EntryAtCall;
      for (const auto &Boundary : Block.InstructionBoundaries)
        if (Boundary.Address >= Op.Addr)
          EntryAtCall.InstructionBoundaries.push_back(Boundary);
      EXPECT_FALSE(hasBindingPreservingCRTArgument(*Image, EntryAtCall, Op,
                                                   *Slot, Work));
      EXPECT_FALSE(
          hasBindingPreservingCRTArgument(*Image, Block, Op, 0x4080f8, Work));
      return;
    }
  }
  FAIL() << "missing original standard stream call";
}

TEST(RegistrationCRTFrame, XCPT4ExitTableInitializationOwnsAllThreeWords) {
  using namespace registration_state;
  for (bool Selected : {false, true}) {
    auto Image = loadXCPT4(Selected);
    ASSERT_TRUE(bool(Image)) << llvm::toString(Image.takeError());
    Decoder Dec;
    ASSERT_TRUE(Dec.init(*Image));
    const auto Caller = CFGBuilder().buildIntegerFrameProof(
        *Image, Dec, 0x407001, "initialize exit tables");
    const std::map<va_t, va_t> Tables = {{0x407030, 0x40b200},
                                         {0x40703f, 0x40b20c}};
    std::set<va_t> Seen;
    size_t Work = 0;
    for (const auto &Block : Caller.Blocks)
      for (const auto &Op : Block.Ops) {
        auto Table = Tables.find(Op.Addr);
        if (Op.Opcode != NdOp::CALL || Table == Tables.end())
          continue;
        SCOPED_TRACE(::testing::Message() << Selected << '/' << Op.Addr);
        ASSERT_TRUE(Seen.insert(Op.Addr).second);
        const auto Slot = checkedCRTCallSlot(*Image, Op, Work);
        ASSERT_EQ(Slot, 0x4080a4u);
        EXPECT_TRUE(
            hasBindingPreservingCRTArgument(*Image, Block, Op, *Slot, Work));
        // An import starting at the last byte overlaps the third write.
        auto Extra = Image->Imports.front();
        Extra.IATAddr = Table->second + 11;
        Image->Imports.push_back(Extra);
        EXPECT_FALSE(
            hasBindingPreservingCRTArgument(*Image, Block, Op, *Slot, Work));
        Image->Imports.pop_back();
      }
    EXPECT_EQ(Seen.size(), Tables.size());
  }
}

TEST(RegistrationCRTFrame, XCPT4StartupModesRequireTheirActualArgument) {
  using namespace registration_state;
  struct Invocation {
    va_t Entry, Call, ValueInstruction;
    const char *Name;
  };
  const Invocation Cases[] = {{0x406b88, 0x406b8b, 0x406b89, "_set_app_type"},
                              {0x406b88, 0x406b96, 0x407268, "_set_fmode"},
                              {0x406c3b, 0x406c46, 0x40726e, "_set_new_mode"}};
  for (const bool Selected : {false, true}) {
    auto Image = loadXCPT4(Selected);
    ASSERT_TRUE(bool(Image)) << llvm::toString(Image.takeError());
    Decoder Dec;
    ASSERT_TRUE(Dec.init(*Image));
    for (const auto &Case : Cases) {
      SCOPED_TRACE(::testing::Message() << Selected << " " << Case.Name);
      const auto Caller = CFGBuilder().buildIntegerFrameProof(
          *Image, Dec, Case.Entry, "startup initializer");
      unsigned Calls = 0;
      for (const auto &Block : Caller.Blocks)
        for (const auto &Op : Block.Ops) {
          if (Op.Addr != Case.Call || Op.Opcode != NdOp::CALL)
            continue;
          ++Calls;
          size_t Work = 0;
          const auto Slot = checkedCRTCallSlot(*Image, Op, Work);
          ASSERT_TRUE(Slot);
          const auto *Import = Image->findImportAt(*Slot);
          ASSERT_TRUE(Import);
          ASSERT_EQ(Import->Name, Case.Name);
          EXPECT_TRUE(
              hasBindingPreservingCRTArgument(*Image, Block, Op, *Slot, Work));
          LowBlock AfterProducer;
          for (const auto &Boundary : Block.InstructionBoundaries)
            if (Boundary.Address >= Case.Call - 1)
              AfterProducer.InstructionBoundaries.push_back(Boundary);
          EXPECT_FALSE(hasBindingPreservingCRTArgument(*Image, AfterProducer,
                                                       Op, *Slot, Work));
          auto *Byte = const_cast<uint8_t *>(
              Image->readVA(Case.ValueInstruction + 1, 1));
          ASSERT_TRUE(Byte);
          const uint8_t Saved = *Byte;
          *Byte = 0xff;
          EXPECT_FALSE(
              hasBindingPreservingCRTArgument(*Image, Block, Op, *Slot, Work));
          *Byte = Saved;
          EXPECT_LT(Work, limits::kMaxRegistrationEHStateWork);
        }
      EXPECT_EQ(Calls, 1u);
    }
  }
}

TEST(RegistrationCRTFrame, XCPT4QueriesProcessorFeaturesDuringStartup) {
  using namespace registration_state;
  for (const bool Selected : {false, true}) {
    auto Image = loadXCPT4(Selected);
    ASSERT_TRUE(bool(Image)) << llvm::toString(Image.takeError());
    Decoder Decode;
    ASSERT_TRUE(Decode.init(*Image));
    for (const auto &[Entry, Site] :
         {std::pair{0x407769u, 0x40777fu}, std::pair{0x4072f1u, 0x4072fdu}}) {
      SCOPED_TRACE(::testing::Message() << Selected << ' ' << Site);
      const auto Caller = CFGBuilder().buildIntegerFrameProof(
          *Image, Decode, Entry, "startup feature query");
      unsigned Calls = 0;
      for (const auto &Block : Caller.Blocks)
        for (const auto &Op : Block.Ops) {
          if (Op.Addr != Site || Op.Opcode != NdOp::INDIR_CALL)
            continue;
          ++Calls;
          size_t Work = 0;
          const auto Slot = checkedCRTCallSlot(*Image, Op, Work);
          ASSERT_TRUE(Slot);
          const auto *Imported = Image->findImportAt(*Slot);
          ASSERT_TRUE(Imported);
          ASSERT_EQ(Imported->Name, "IsProcessorFeaturePresent");
          FrameState State;
          State.Registers[x86reg::RSP / x86reg::GeneralRegStride] =
              FrameValue::frame(-4);
          EXPECT_TRUE(hasBindingPreservingKernelArgument(*Image, Block, Op,
                                                         *Slot, State, Work));
          State.ImportBindingsIntact = false;
          EXPECT_FALSE(hasBindingPreservingKernelArgument(*Image, Block, Op,
                                                          *Slot, State, Work));
          State.ImportBindingsIntact = true;
          State.Registers[x86reg::RSP / x86reg::GeneralRegStride] =
              FrameValue::frame(0);
          EXPECT_FALSE(hasBindingPreservingKernelArgument(*Image, Block, Op,
                                                          *Slot, State, Work));
        }
      EXPECT_EQ(Calls, 1u);
    }
  }
}

TEST(RegistrationCRTFrame, XCPT4CPUInitializerRestoresTheCallerStack) {
  for (const bool Selected : {false, true}) {
    auto Image = loadXCPT4(Selected);
    ASSERT_TRUE(bool(Image)) << llvm::toString(Image.takeError());
    RegistrationCallCalleeIndex Callees(*Image);
    const auto Init = Callees.stackContract(0x407769);
    ASSERT_TRUE(Init);
    EXPECT_EQ(Init->StackPopBytes, 0u);
    EXPECT_FALSE(Init->PreservesImportBindings);
    EXPECT_FALSE(
        Callees.stackContract(0x4072f1)); // Conditional fast-fail path.
  }
}

TEST(RegistrationCRTFrame, XCPT4StartupFilterSetterPreservesLiveBindings) {
  enum class Change { None, MissingRelocation, DataTarget, Provider, NewMode };
  for (const bool Selected : {false, true}) {
    auto Original = loadXCPT4(Selected);
    ASSERT_TRUE(bool(Original)) << llvm::toString(Original.takeError());
    for (const auto Variant :
         {Change::None, Change::MissingRelocation, Change::DataTarget,
          Change::Provider, Change::NewMode}) {
      SCOPED_TRACE(::testing::Message() << Selected << ' ' << int(Variant));
      BinaryImage Image = *Original;
      if (Variant == Change::MissingRelocation)
        ASSERT_EQ(
            std::erase_if(Image.BaseRelocations,
                          [](const auto &R) { return R.Address == 0x407489; }),
            1u);
      if (Variant == Change::DataTarget) {
        auto *Word = const_cast<uint8_t *>(Image.readVA(0x407489, 4));
        ASSERT_TRUE(Word);
        writeLE<uint32_t>(Word, 0x40b1b8);
      }
      if (Variant == Change::Provider) {
        const auto Import = llvm::find_if(
            Image.Imports, [](const auto &I) { return I.IATAddr == 0x408014; });
        ASSERT_NE(Import, Image.Imports.end());
        Import->Module = "other.dll";
      }
      if (Variant == Change::NewMode) {
        auto *Getter = const_cast<uint8_t *>(Image.readVA(0x40726f, 1));
        ASSERT_TRUE(Getter);
        *Getter = 0xff;
      }
      size_t Work = 0;
      RegistrationCallCalleeIndex Callees(Image, &Work);
      const auto Callback = Callees.stackContract(0x406c3b);
      if (Variant != Change::None) {
        EXPECT_FALSE(Callback && Callback->PreservesImportBindings);
        continue;
      }
      ASSERT_TRUE(Callback);
      EXPECT_EQ(Callback->StackPopBytes, 0u);
      EXPECT_TRUE(Callback->PreservesImportBindings);
      EXPECT_EQ(Callback->RequiredImports,
                (std::vector<va_t>{0x408014, 0x408078}));
      EXPECT_LT(Work, limits::kMaxRegistrationEHStateWork);
    }
  }
}

TEST(RegistrationCRTFrame, XCPT4StdioOptionStoresUseTheReturnedDataAddress) {
  struct Change {
    va_t MissingFixup;
    va_t Overlap;
    va_t Instruction = 0;
    uint8_t Byte = 0;
  };
  const Change Cases[] = {{},
                          {0x4045b4, 0},
                          {0x4072b7, 0},
                          {0, 0x40b55f},
                          {0, 0x40b223},
                          {0, 0, 0x4045b8, 0x58},  // POP EAX
                          {0, 0, 0x4072b6, 0xb9}}; // MOV ECX,imm32
  for (bool Selected : {false, true}) {
    auto Original = loadXCPT4(Selected);
    ASSERT_TRUE(bool(Original)) << llvm::toString(Original.takeError());
    // The first getter uses a frame pointer, the second returns directly.
    for (const auto &[MissingFixup, Overlap, Instruction, Byte] : Cases) {
      SCOPED_TRACE(::testing::Message()
                   << Selected << ' ' << MissingFixup << ' ' << Overlap << ' '
                   << Instruction);
      BinaryImage Image = *Original;
      if (Instruction) {
        auto *Code = const_cast<uint8_t *>(Image.readVA(Instruction, 1));
        ASSERT_TRUE(Code);
        *Code = Byte;
      }
      if (MissingFixup)
        ASSERT_EQ(std::erase_if(
                      Image.BaseRelocations,
                      [&](const auto &R) { return R.Address == MissingFixup; }),
                  1u);
      if (Overlap) {
        ASSERT_FALSE(Image.Imports.empty());
        auto Binding = Image.Imports.front();
        Binding.IATAddr = Overlap;
        Image.Imports.push_back(std::move(Binding));
      }
      size_t Work = 0;
      RegistrationCallCalleeIndex Callees(Image, &Work);
      for (va_t Entry : {0x4072bcu, 0x406c33u}) {
        const auto Contract = Callees.stackContract(Entry);
        if (MissingFixup || Overlap || Instruction) {
          EXPECT_FALSE(Contract && Contract->PreservesImportBindings);
          continue;
        }
        ASSERT_TRUE(Contract);
        EXPECT_EQ(Contract->StackPopBytes, 0u);
        EXPECT_TRUE(Contract->PreservesImportBindings);
        EXPECT_TRUE(Contract->RequiredImports.empty());
      }
      EXPECT_LT(Work, limits::kMaxRegistrationEHStateWork);
    }
  }
}

TEST(RegistrationConditionalFrame, DirectCallsRetainTheirPhysicalOwner) {
  auto Image = loadXCPT4(true);
  ASSERT_TRUE(bool(Image)) << llvm::toString(Image.takeError());
  Decoder Dec;
  ASSERT_TRUE(Dec.init(*Image));
  const auto Startup = CFGBuilder().buildIntegerFrameProof(
      *Image, Dec, 0x406c3b, "CRT initializer");
  for (const auto &Block : Startup.Blocks) {
    for (size_t Index = 0; Index < Block.Ops.size(); ++Index) {
      if (Block.Ops[Index].Opcode != NdOp::CALL)
        continue;
      LowFunc Function;
      LowBlock Other;
      Other.StartAddr = 0x401000;
      Other.EndAddr = 0x401010;
      Function.Blocks.push_back(Other);
      Function.Blocks.push_back(Block);
      Function.Blocks.push_back(Block);
      const auto &Owner = Function.Blocks.back();
      const auto &Call = Owner.Ops[Index];
      const auto Boundary =
          llvm::find_if(Owner.InstructionBoundaries,
                        [&](const auto &I) { return I.Address == Call.Addr; });
      ASSERT_NE(Boundary, Owner.InstructionBoundaries.end());
      size_t Work = 0;
      EXPECT_EQ(registration_state::checkedRegistrationDirectCall(
                    *Image, Function, Call, Work),
                &*Boundary);
      // More operations at a different address cannot change ownership.
      Function.Blocks.front().Ops.resize(512);
      size_t More = 0;
      EXPECT_EQ(registration_state::checkedRegistrationDirectCall(
                    *Image, Function, Call, More),
                &*Boundary);
      EXPECT_EQ(More, Work);
      const LowOp Copy = Call;
      EXPECT_FALSE(registration_state::checkedRegistrationDirectCall(
          *Image, Function, Copy, Work));
      return;
    }
  }
  FAIL() << "missing direct initializer call";
}

TEST(RegistrationCRTFrame, XCPT4StdioImportsHaveCallerCleanup) {
  auto Image = loadXCPT4(true);
  ASSERT_TRUE(bool(Image)) << llvm::toString(Image.takeError());
  for (const auto &[Slot, Name] :
       {std::pair{0x4080f8u, "__stdio_common_vfprintf"},
        std::pair{0x4080fcu, "__acrt_iob_func"}}) {
    SCOPED_TRACE(Name);
    const auto *Import = Image->findImportAt(Slot);
    ASSERT_TRUE(Import);
    ASSERT_EQ(Import->Name, Name);
    EXPECT_EQ(registration_abi::checkedRegistrationImportStackPop(*Image, Slot),
              0u);
    EXPECT_FALSE(
        registration_abi::preservesRegistrationImportBindings(*Image, Slot));
  }
}

TEST(RegistrationCRTFrame, XCPT4HeaderGuardReadsItsImageHeaders) {
  for (bool Selected : {false, true}) {
    auto Image = loadXCPT4(Selected);
    ASSERT_TRUE(bool(Image)) << llvm::toString(Image.takeError());
    size_t Work = 0;
    const auto Read = [&](va_t Address, uint16_t Width) {
      const auto Scalar = registration_abi::readImmutableRegistrationScalar(
          *Image, Address, Width, Work);
      return Scalar ? Scalar->Value : std::nullopt;
    };
    ASSERT_EQ(Read(Image->Base, 2), 0x5a4du);
    const auto NT = Read(Image->Base + 0x3c, 4);
    ASSERT_TRUE(NT);
    EXPECT_EQ(Read(Image->Base + *NT, 4), 0x4550u);
    EXPECT_EQ(Read(Image->Base + *NT + 0x18, 2), 0x10bu);
    EXPECT_FALSE(readImmutableImageBytes(*Image, 0x408108, 4));
    EXPECT_FALSE(readImmutableImageBytes(*Image, 0x40b22c, 4));
  }
}

TEST(RegistrationCRTFrame, XCPT4HeaderGuardInstallsItsSharedFrame) {
  auto Image = loadXCPT4();
  ASSERT_TRUE(bool(Image)) << llvm::toString(Image.takeError());
  const auto *Guard = Image->ExceptionMetadata.findFunction(0x407088);
  ASSERT_TRUE(Guard && Guard->Registration);
  const auto Entry = coff_loader::getCheckedX86SEH4PrologueCall(*Image, *Guard);
  ASSERT_TRUE(Entry);
  EXPECT_EQ(Entry->CallVA, 0x40708fu);
  EXPECT_EQ(Guard->Registration->RegistrationOffset, -16);
  EXPECT_EQ(Guard->Registration->TryLevelOffset, -4);
  const auto ABI =
      coff_loader::getCheckedX86EH4PersonalityABI(*Image, Guard->PersonalityVA);
  ASSERT_TRUE(ABI);
  EXPECT_EQ(ABI->CookieCheckVA, 0x407946u);
  ASSERT_EQ(ABI->ImageReads.size(), 1u);
  EXPECT_EQ(ABI->ImageReads.front().Begin, 0x40b230u);
  EXPECT_EQ(ABI->ImageReads.front().End, 0x40b234u);
  EXPECT_FALSE(isFileBackedWritableImageRange(*Image, 0x40b230, 4));
}

TEST(RegistrationCRTFrame, XCPT4CRTStartupHasTwoCallerCleanedWalkers) {
  auto Image = loadXCPT4();
  ASSERT_TRUE(bool(Image)) << llvm::toString(Image.takeError());
  for (const auto &[Slot, Name] : {std::pair{0x4080ccu, "_initterm"},
                                   std::pair{0x4080e4u, "_initterm_e"}}) {
    SCOPED_TRACE(Name);
    const auto *Imported = Image->findImportAt(Slot);
    ASSERT_TRUE(Imported);
    ASSERT_EQ(Imported->Name, Name);
    EXPECT_EQ(registration_abi::checkedRegistrationImportStackPop(*Image, Slot),
              0u);
    EXPECT_FALSE(
        registration_abi::preservesRegistrationImportBindings(*Image, Slot));
  }
  // Each non-null initializer has an immutable HIGHLOW slot. The pointer
  // identifies a callback; its writes require a separate invocation proof.
  for (const auto &[Slot, Target] :
       {std::pair{0x408114u, 0x406c3bu}, std::pair{0x408120u, 0x406b88u},
        std::pair{0x408124u, 0x406c33u}})
    EXPECT_EQ(readImmutablePE32CodePointerStorage(*Image, Slot), Target);
}

TEST(RegistrationCRTFrame, XCPT4BindsOrderedInitializerRangesAtBothCalls) {
  auto Image = loadXCPT4();
  ASSERT_TRUE(bool(Image)) << llvm::toString(Image.takeError());
  Decoder Dec;
  ASSERT_TRUE(Dec.init(*Image));
  const auto Startup =
      CFGBuilder().buildIntegerFrameProof(*Image, Dec, 0x406c57, "CRT startup");
  unsigned Calls = 0;
  for (const auto &Block : Startup.Blocks)
    for (const auto &Op : Block.Ops) {
      if (Op.Opcode != NdOp::CALL ||
          (Op.Addr != 0x406ca8 && Op.Addr != 0x406cce))
        continue;
      const bool Checked = Op.Addr == 0x406ca8;
      const va_t Slot = Checked ? 0x4080e4 : 0x4080cc;
      const std::vector<va_t> Expected =
          Checked ? std::vector<va_t>{0x406b88, 0x406c33}
                  : std::vector<va_t>{0x406c3b};
      size_t Work = 0;
      const auto Targets = registration_state::checkedCRTInitializerTargets(
          *Image, Block, Op, Slot, Work);
      ASSERT_TRUE(Targets) << std::hex << Op.Addr;
      EXPECT_EQ(*Targets, Expected);
      size_t Short = limits::kMaxRegistrationEHStateWork;
      EXPECT_FALSE(registration_state::checkedCRTInitializerTargets(
          *Image, Block, Op, Slot, Short));
      EXPECT_FALSE(registration_state::checkedCRTInitializerTargets(
          *Image, Block, Op, Checked ? 0x4080cc : 0x4080e4, Work));
      auto Split = Block;
      std::erase_if(Split.InstructionBoundaries,
                    [&](const auto &I) { return I.Address < Op.Addr - 5; });
      EXPECT_FALSE(registration_state::checkedCRTInitializerTargets(
          *Image, Split, Op, Slot, Work));
      ++Calls;
    }
  EXPECT_EQ(Calls, 2u);
}

TEST(RegistrationCRTFrame, XCPT4OnExitInitializationUsesItsCallingMode) {
  using namespace registration_state;
  using registration_abi::BoundFrameCalls;
  using Ranges = std::set<std::pair<va_t, va_t>>;
  const Ranges Reads{{0x40b1bc, 0x40b1c0}, {0x40b1fd, 0x40b1fe}};
  Ranges Writes{{0x40b1fd, 0x40b1fe}};
  for (va_t VA = 0x40b200; VA < 0x40b218; VA += 4)
    Writes.emplace(VA, VA + 4);
  for (bool Selected : {false, true}) {
    auto Image = loadXCPT4(Selected);
    ASSERT_TRUE(bool(Image)) << llvm::toString(Image.takeError());
    Decoder Dec;
    ASSERT_TRUE(Dec.init(*Image));
    auto Caller = CFGBuilder().buildIntegerFrameProof(*Image, Dec, 0x406b88,
                                                      "pre-c-init");
    const LowOp *Call = nullptr;
    for (const auto &Block : Caller.Blocks)
      for (const auto &Op : Block.Ops)
        if (Op.Opcode == NdOp::CALL && Op.Addr == 0x406bab)
          Call = &Op;
    ASSERT_TRUE(Call);
    const std::map<unsigned, FrameValue> Registers{
        {x86reg::RAX / x86reg::GeneralRegStride, {}}};
    const std::map<int32_t, FrameValue> Arguments{{8, FrameValue::constant(1)}};
    size_t Work = 0;
    const auto Effect = registration_abi::checkedBoundIntegerCall(
        *Image, Caller, *Call, Arguments, Registers,
        BoundFrameCalls::ReadOnlyLeaves, Work);
    ASSERT_TRUE(Effect) << Selected;
    EXPECT_EQ(Effect->Result.numericLowByte(), 1);
    EXPECT_EQ(Effect->Memory.Reads, Reads);
    EXPECT_EQ(Effect->Memory.Writes, Writes);
    EXPECT_LT(Work, limits::kMaxRegistrationEHStateWork);
    for (uint32_t Mode : {0u, 2u}) {
      size_t Used = 0;
      EXPECT_FALSE(registration_abi::checkedBoundIntegerCall(
          *Image, Caller, *Call, {{8, FrameValue::constant(Mode)}}, Registers,
          BoundFrameCalls::ReadOnlyLeaves, Used));
    }
    size_t Used = 0;
    EXPECT_FALSE(registration_abi::checkedBoundIntegerCall(
        *Image, Caller, *Call, Arguments, {}, BoundFrameCalls::ReadOnlyLeaves,
        Used));
    EXPECT_FALSE(registration_abi::checkedBoundIntegerCall(
        *Image, Caller, *Call, Arguments, Registers, BoundFrameCalls::CallFree,
        Used));
    auto Unowned = *Call;
    EXPECT_FALSE(registration_abi::checkedBoundIntegerCall(
        *Image, Caller, Unowned, Arguments, Registers,
        BoundFrameCalls::ReadOnlyLeaves, Used));
    Used = limits::kMaxRegistrationEHStateWork;
    EXPECT_FALSE(registration_abi::checkedBoundIntegerCall(
        *Image, Caller, *Call, Arguments, Registers,
        BoundFrameCalls::ReadOnlyLeaves, Used));
    for (va_t Field : {0x407006u, 0x40793eu, 0x407052u}) {
      BinaryImage Missing = *Image;
      ASSERT_EQ(
          std::erase_if(Missing.BaseRelocations,
                        [=](const auto &R) { return R.Address == Field; }),
          1u);
      size_t Attempt = 0;
      EXPECT_FALSE(registration_abi::checkedBoundIntegerCall(
          Missing, Caller, *Call, Arguments, Registers,
          BoundFrameCalls::ReadOnlyLeaves, Attempt))
          << std::hex << Field;
    }
    // Change the leaf's CMP [image],EAX to MOV [image],EAX.
    auto *Compare = const_cast<uint8_t *>(Image->readVA(0x40793c, 1));
    ASSERT_TRUE(Compare);
    ASSERT_EQ(*Compare, 0x39);
    *Compare = 0x89;
    Used = 0;
    EXPECT_FALSE(registration_abi::checkedBoundIntegerCall(
        *Image, Caller, *Call, Arguments, Registers,
        BoundFrameCalls::ReadOnlyLeaves, Used));
  }
}

TEST(RegistrationCRTFrame,
     StartupCallerCarriesItsPrivateWordAcrossTheCRTStore) {
  auto Original = loadXCPT4(true);
  ASSERT_TRUE(bool(Original)) << llvm::toString(Original.takeError());
  enum class Input {
    Current,
    DifferentMode,
    UnknownAddress,
    DifferentProvider
  };
  for (Input Kind : {Input::Current, Input::DifferentMode,
                     Input::UnknownAddress, Input::DifferentProvider}) {
    SCOPED_TRACE(int(Kind));
    BinaryImage Image = *Original;
    // Keep the original accessor, push, store, direct call and complete
    // callees. Two different RET cleanups expose whether the caller proved the
    // returned AL.
    constexpr uint8_t Tail[] = {0x83, 0xc4, 4,    0x84, 0xc0, 0x74,
                                1,    0xc3, 0xc2, 4,    0};
    auto *Code =
        const_cast<uint8_t *>(Image.readVA(0x406ba2, 14 + sizeof(Tail)));
    ASSERT_TRUE(Code);
    std::copy(std::begin(Tail), std::end(Tail), Code + 14);
    if (Kind == Input::DifferentMode)
      Code[6] = 2; // PUSH 2
    if (Kind == Input::UnknownAddress)
      Code[8] = 0x31; // MOV [ECX],ESI
    if (Kind == Input::DifferentProvider) {
      auto Accessor = llvm::find_if(Image.Imports, [](const auto &Entry) {
        return Entry.Name == "__p__commode";
      });
      ASSERT_NE(Accessor, Image.Imports.end());
      Accessor->Module = "api-ms-win-crt-runtime-l1-1-0.dll";
    }
    size_t Work = 0;
    const auto Stack = getCheckedX86CalleeStackPop(Image, 0x406ba2, &Work);
    EXPECT_EQ(Stack, Kind == Input::Current ? std::optional<uint32_t>(0)
                                            : std::nullopt);
    const auto Caller =
        RegistrationCallCalleeIndex(Image, &Work).stackContract(0x406ba2);
    EXPECT_EQ(bool(Caller && Caller->PreservesImportBindings),
              Kind == Input::Current);
    if (Kind == Input::Current) {
      ASSERT_TRUE(Caller);
      EXPECT_EQ(Caller->RequiredImports, (std::vector<va_t>{0x408100}));
    }
    EXPECT_LT(Work, limits::kMaxRegistrationEHStateWork);
  }
}

TEST(RegistrationCRTFrame, XCPT4TypeInfoInitializesAnAlignedPrivateListHead) {
  enum class Input {
    Current,
    Retargeted,
    WrongProvider,
    DelayImport,
    Unaligned,
    MissingOutputFixup,
    MissingSlotFixup,
    OverlappingImport,
    Shared,
    Discardable,
    ReadOnly
  };
  for (bool Selected : {false, true}) {
    auto Original = loadXCPT4(Selected);
    ASSERT_TRUE(bool(Original)) << llvm::toString(Original.takeError());
    for (auto Kind :
         {Input::Current, Input::Retargeted, Input::WrongProvider,
          Input::DelayImport, Input::Unaligned, Input::MissingOutputFixup,
          Input::MissingSlotFixup, Input::OverlappingImport, Input::Shared,
          Input::Discardable, Input::ReadOnly}) {
      SCOPED_TRACE(::testing::Message() << Selected << '/' << int(Kind));
      BinaryImage Image = *Original;
      const auto Slot = llvm::find_if(
          Image.Imports, [](const auto &I) { return I.IATAddr == 0x408020; });
      ASSERT_NE(Slot, Image.Imports.end());
      ASSERT_EQ(Slot->Name, "InitializeSListHead");
      if (Kind == Input::WrongProvider)
        Slot->Module = "application.dll";
      if (Kind == Input::DelayImport)
        Slot->IsDelayImport = true;
      if (Kind == Input::MissingOutputFixup ||
          Kind == Input::MissingSlotFixup) {
        const va_t Field =
            Kind == Input::MissingOutputFixup ? 0x407275 : 0x40727b;
        ASSERT_EQ(
            std::erase_if(Image.BaseRelocations,
                          [=](const auto &R) { return R.Address == Field; }),
            1u);
      }
      if (Kind == Input::Retargeted || Kind == Input::Unaligned) {
        const uint32_t Address =
            Kind == Input::Retargeted ? 0x40b200 : 0x40b201;
        auto *Field = const_cast<uint8_t *>(Image.readVA(0x407275, 4));
        ASSERT_TRUE(Field);
        ASSERT_EQ(readLE<uint32_t>(Field), 0x40b218u);
        writeLE<uint32_t>(Field, Address);
        const auto It = Image.DataAddressRelocOperands.find(0x407275);
        ASSERT_NE(It, Image.DataAddressRelocOperands.end());
        auto &Reloc = It->second;
        ASSERT_EQ(Reloc.TargetVA, 0x40b218u);
        Reloc.EncodedValue = Address;
        Reloc.TargetVA = Address;
        if (Reloc.TargetOwnerVA == 0x40b218)
          Reloc.TargetOwnerVA = Address;
        size_t ExtentWork = 0;
        ASSERT_TRUE(registration_abi::hasSEHCallbackImageMemory(
            Image, Address, 8, true, ExtentWork));
      }
      if (Kind == Input::OverlappingImport) {
        auto Extra = *Slot;
        Extra.IATAddr = 0x40b21f;
        Image.Imports.push_back(std::move(Extra));
      }
      const auto *Owner = Image.getSectionFor(0x40b218);
      ASSERT_TRUE(Owner);
      auto &Storage = Image.Sections[Owner - Image.Sections.data()];
      if (Kind == Input::Shared)
        Storage.Type |= llvm::COFF::IMAGE_SCN_MEM_SHARED;
      if (Kind == Input::Discardable)
        Storage.Type |= llvm::COFF::IMAGE_SCN_MEM_DISCARDABLE;
      if (Kind == Input::ReadOnly)
        Storage.Flags = SegmentFlags::Readable;
      size_t Work = 0;
      const auto Stack =
          RegistrationCallCalleeIndex(Image, &Work).stackContract(0x407274);
      const bool ValidCall =
          Kind == Input::Current || Kind == Input::Retargeted;
      EXPECT_EQ(bool(Stack && Stack->PreservesImportBindings), ValidCall);
      if (ValidCall) {
        ASSERT_TRUE(Stack);
        EXPECT_EQ(Stack->StackPopBytes, 0u);
        EXPECT_EQ(Stack->RequiredImports, (std::vector<va_t>{0x408020}));
      }
      EXPECT_LT(Work, limits::kMaxRegistrationEHStateWork);
    }
  }
}

TEST(RegistrationCRTFrame, XCPT4ExitRegistrarsUseTheirEagerRuntimeSlots) {
  for (bool Selected : {false, true}) {
    auto Image = loadXCPT4(Selected);
    ASSERT_TRUE(bool(Image)) << llvm::toString(Image.takeError());
    size_t Work = 0;
    RegistrationCallCalleeIndex Index(*Image, &Work);
    for (const auto &[Entry, Slot] :
         {std::pair<va_t, va_t>{0x407d5c, 0x4080b0}, {0x407d62, 0x4080b4}}) {
      SCOPED_TRACE(::testing::Message() << Selected << '/' << Entry);
      const auto Callee = Index.stackContract(Entry);
      ASSERT_TRUE(Callee);
      EXPECT_TRUE(Callee->ImportVeneer);
      EXPECT_EQ(Callee->RequiredImports, (std::vector<va_t>{Slot}));
      EXPECT_EQ(Callee->StackPopBytes, 0u);
      EXPECT_FALSE(Callee->PreservesImportBindings);
    }
    EXPECT_LT(Work, limits::kMaxRegistrationEHStateWork);
  }
}

TEST(RegistrationCRTFrame, XCPT4ExitRegistrarReadsThePushedCallerWord) {
  for (bool Selected : {false, true}) {
    auto Original = loadXCPT4(Selected);
    ASSERT_TRUE(bool(Original)) << llvm::toString(Original.takeError());
    for (bool Pushed : {true, false}) {
      SCOPED_TRACE(::testing::Message() << Selected << '/' << Pushed);
      BinaryImage Image = *Original;
      // Retain both entire callees and the original PUSH imm32 / CALL prefix.
      // End this isolated caller after the call with its matching cleanup.
      auto *CallPrefix = const_cast<uint8_t *>(Image.readVA(0x406bbf, 12));
      ASSERT_TRUE(CallPrefix);
      ASSERT_EQ(CallPrefix[0], 0x68);
      ASSERT_EQ(CallPrefix[5], 0xe8);
      if (!Pushed)
        CallPrefix[0] = 0xb8; // MOV EAX,imm32 retains the same relocation.
      CallPrefix[10] = Pushed ? 0x59 : 0xc3;
      CallPrefix[11] = 0xc3;
      size_t Work = 0;
      RegistrationCallCalleeIndex Index(Image, &Work);
      EXPECT_FALSE(Index.stackContract(0x407161));
      const auto Caller = Index.stackContract(0x406bbf);
      EXPECT_EQ(bool(Caller), Pushed);
      if (Caller) {
        EXPECT_EQ(Caller->StackPopBytes, 0u);
        EXPECT_EQ(Caller->RequiredImports,
                  (std::vector<va_t>{0x4080b0, 0x4080dc}));
        EXPECT_FALSE(Caller->PreservesImportBindings);
      }
      EXPECT_FALSE(Index.stackContract(0x4071b1));
      EXPECT_FALSE(Index.stackContract(0x407161));
      EXPECT_LT(Work, limits::kMaxRegistrationEHStateWork);
    }
  }
}

TEST(RegistrationCRTFrame, XCPT4StartupCallsTheUCRTCommodeAccessor) {
  auto Image = loadXCPT4();
  ASSERT_TRUE(bool(Image)) << llvm::toString(Image.takeError());
  Decoder Dec;
  ASSERT_TRUE(Dec.init(*Image));
  CFGBuilder Builder;
  const auto Function =
      Builder.buildIntegerFrameProof(*Image, Dec, 0x406b88, "pre-c-init");
  size_t Work = 0;
  unsigned Calls = 0;
  for (const auto &Block : Function.Blocks)
    for (const auto &Op : Block.Ops) {
      if (Op.Addr != 0x406ba2 || Op.Opcode != NdOp::CALL)
        continue;
      const auto Slot =
          registration_state::checkedCRTCallSlot(*Image, Op, Work);
      ASSERT_TRUE(Slot);
      const auto *Imported = Image->findImportAt(*Slot);
      ASSERT_TRUE(Imported);
      EXPECT_EQ(Imported->Name, "__p__commode");
      EXPECT_EQ(Imported->Module, "api-ms-win-crt-stdio-l1-1-0.dll");
      EXPECT_EQ(
          registration_abi::checkedRegistrationImportStackPop(*Image, *Slot),
          0u);
      ++Calls;
    }
  EXPECT_EQ(Calls, 1u);
}

TEST(RegistrationCRTFrame, XCPT4StdioWrappersSeparateExplicitStackRestoration) {
  using namespace registration_state;
  auto Image = loadXCPT4(true);
  ASSERT_TRUE(bool(Image)) << llvm::toString(Image.takeError());
  Decoder Dec;
  ASSERT_TRUE(Dec.init(*Image));
  CFGBuilder Builder;
  for (va_t Entry : {0x401580u, 0x4045f0u}) {
    const auto Function =
        Builder.buildIntegerFrameProof(*Image, Dec, Entry, "stdio-caller");
    SEHPrivateCallProof Calls(*Image, Function);
    size_t Work = 0;
    std::set<va_t> Seen;
    for (const auto &Block : Function.Blocks)
      for (const auto &Op : Block.Ops) {
        if ((Op.Opcode != NdOp::CALL && Op.Opcode != NdOp::INDIR_CALL) ||
            (Op.Addr != 0x4015b5 && Op.Addr != 0x4015e1 &&
             Op.Addr != 0x404608 && Op.Addr != 0x404612))
          continue;
        SCOPED_TRACE(Op.Addr);
        ASSERT_TRUE(Seen.insert(Op.Addr).second);
        // The outer wrapper executes MOV ESP, EBP before its return. The
        // inner wrapper needs the UCRT stack contract for its ADD ESP, 24.
        const auto Pop =
            Entry == 0x401580 ? std::optional<uint32_t>{0} : std::nullopt;
        EXPECT_EQ(Calls.opaqueStackPop(Op, Work), Pop);
        const size_t Before = Work;
        EXPECT_EQ(Calls.opaqueStackPop(Op, Work), Pop);
        if (Pop)
          EXPECT_EQ(Work - Before, 1u);
        const auto Detached = Op;
        EXPECT_FALSE(Calls.opaqueStackPop(Detached, Work));
      }
    EXPECT_EQ(Seen.size(), 2u);
    EXPECT_LT(Work, limits::kMaxRegistrationEHStateWork);
  }
}

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
  ConditionalCallResult Invocation;
  EXPECT_TRUE(registration_abi::hasConditionalCallFrame(
      *Image, Call, Parent, Work, &Initialized, &Returned, nullptr, nullptr,
      &Invocation));
  ASSERT_TRUE(Invocation.Dispatch);
  ASSERT_TRUE(Invocation.Dispatch->Raise);
  EXPECT_EQ(Invocation.Dispatch->Raise->Code, 0xc0000005u);
  EXPECT_EQ(Invocation.Dispatch->ArgumentEnd, 12);
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
     XCPT4FinallyOnlyLocalUnwindUsesItsCurrentStack) {
  auto Image = loadXCPT4();
  ASSERT_TRUE(bool(Image)) << llvm::toString(Image.takeError());
  size_t Work = 0;
  RegistrationCallCalleeIndex Index(*Image, &Work);
  CFGBuilder Builder;
  Builder.setRegistrationCalleeIndex(&Index);
  Decoder Dec;
  ASSERT_TRUE(Dec.init(*Image));
  // The original MSVC EH3 parent has finally scopes but no SavedESP store.
  const auto Function = Builder.build(*Image, Dec, 0x4059a0, "finally-loop");
  ASSERT_TRUE(Function.RegistrationStates);
  const auto &States = *Function.RegistrationStates;
  for (const auto &Diagnostic : States.Diagnostics)
    ADD_FAILURE() << Diagnostic;
  EXPECT_TRUE(States.Complete);
  ASSERT_TRUE(States.HasSEHLocalUnwinds);
  ASSERT_TRUE(States.SEHLocalUnwindsComplete);
  ASSERT_EQ(States.SEHLocalUnwinds.size(), 2u);
  for (const auto &Call : States.SEHLocalUnwinds) {
    EXPECT_TRUE(Call.Address == 0x4059fa || Call.Address == 0x405a16);
    EXPECT_EQ(Call.StackOffset, -48);
    EXPECT_EQ(Call.Runtime.Protocol,
              RegistrationLocalUnwindContract::Kind::EH3);
  }
  EXPECT_LT(Work, limits::kMaxRegistrationEHStateWork);
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

TEST(RegistrationConditionalFrame, XCPT4DisplayKeepsWaitInItsTryLevel) {
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
  // The final FWAIT before main resets the try level can deliver a pending
  // x87 exception. Both source views must keep it under the scalar filter.
  constexpr va_t Wait = 0x401679;
  const auto &Low = Result.LowFuncs.front();
  ASSERT_TRUE(llvm::any_of(Low.Blocks, [&](const LowBlock &Block) {
    return llvm::any_of(Block.Ops, [&](const LowOp &Op) {
      return Op.Addr == Wait && Op.Opcode == NdOp::INTRINSIC &&
             Op.NumInputs == 1 &&
             Op.Inputs[0] == NdVar::cst(uint64_t(Intrinsic::X87Wait), 2);
    });
  }));
  for (const auto *States :
       {&Low.RegistrationStates, &Result.MedFuncs.front().RegistrationStates}) {
    ASSERT_TRUE(*States && (*States)->Display);
    const auto &Stretches = (*States)->Display->Stretches;
    const auto It = llvm::find_if(Stretches, [&](const auto &Stretch) {
      return Stretch.Range.contains(Wait);
    });
    ASSERT_NE(It, Stretches.end());
    EXPECT_EQ(It->Levels, (std::vector<int32_t>{1}));
    EXPECT_FALSE(It->Quiet);
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
  for (unsigned Mutation = 0; Mutation != 10; ++Mutation) {
    SCOPED_TRACE(Mutation);
    auto *Flags = const_cast<uint8_t *>(Image->readVA(0x401038, 1));
    auto *Code = const_cast<uint8_t *>(Image->readVA(0x401039, 3));
    ASSERT_TRUE(Flags && Code);
    *Flags = Mutation == 8 ? 1 : 0;
    // An authentic wrapper may raise a different code than its incoming
    // scalar. That search must not borrow the caller argument as its origin.
    Code[0] = Mutation == 9 ? 0x31 : 0x8b;
    Code[1] = Mutation == 9 ? 0xc0 : 0x45;
    Code[2] = Mutation == 9 ? 0x90 : 0x08;
    auto Current = Function;
    ConditionalFrame Inputs;
    Inputs.Arguments = {{8, FrameValue::constant(0xc0000005)},
                        {12, FrameValue::frame(256)}};
    Inputs.Objects = {{256, 260}};
    Inputs.ObjectCells = {{256, FrameValue::constant(1)}};
    Inputs.ProveCall = [&](const LowOp &Op, const FrameState &State,
                           const std::set<int32_t> &Initialized)
        -> std::optional<ConditionalCallResult> {
      ConditionalCallResult Invocation;
      if (Mutation == 4 || !registration_abi::hasConditionalCallFrame(
                               *Image, Op, State, Inputs.Work, &Initialized,
                               nullptr, nullptr, nullptr, &Invocation))
        return std::nullopt;
      // The returning frame remains valid, but cannot stand in for the
      // missing proof of this wrapper's exceptional dispatch effects.
      if (Mutation == 7)
        Invocation.Dispatch.reset();
      return Invocation;
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

TEST(RegistrationCRTFrame, XCPT4ClearsX87StatusWithinItsIntegerFrame) {
  using namespace registration_state;
  for (bool Selected : {false, true}) {
    auto Image = loadXCPT4(Selected);
    ASSERT_TRUE(bool(Image)) << llvm::toString(Image.takeError());
    Decoder Dec;
    ASSERT_TRUE(Dec.init(*Image));
    const auto Function = CFGBuilder().buildIntegerFrameProof(
        *Image, Dec, 0x406b88, "pre-c-initializer");
    unsigned Matches = 0;
    for (const auto &Block : Function.Blocks)
      for (const auto &Op : Block.Ops) {
        if (Op.Addr != 0x406bb8)
          continue;
        ++Matches;
        ASSERT_EQ(Op.Opcode, NdOp::INTRINSIC);
        ASSERT_EQ(Op.NumInputs, 1u);
        ASSERT_EQ(Op.Inputs[0], NdVar::cst(uint64_t(Intrinsic::X87Fnclex), 2));
        EXPECT_TRUE(isFramePreservingIntrinsic(Op));
        EXPECT_FALSE(isX86FrameReadIntrinsic(Op, Arch::X86));

        FrameState State;
        FrameTransfer Transfer(State, 0);
        const auto Status = NdVar::reg(x86reg::FPU_SW, 2);
        State.OtherRegisterBytes[x86reg::FPU_SW] = FrameValue::constant(0x41);
        State.OtherRegisterBytes[x86reg::FPU_SW + 1] =
            FrameValue::constant(0x38);
        State.Registers[x86reg::RSP / x86reg::GeneralRegStride] =
            FrameValue::frame(-8);
        State.Registers[x86reg::RAX / x86reg::GeneralRegStride] =
            FrameValue::constant(1);
        State.store(-8, 4, FrameValue::constant(42));
        EXPECT_EQ(Transfer.read(Status).Constant, 0x3841u);
        Transfer.beginInstruction(Op.Addr);
        Transfer.write(Op, Transfer.evaluate(Op, false));
        EXPECT_FALSE(Transfer.read(Status).Constant);
        EXPECT_EQ(Transfer.read(NdVar::reg(x86reg::RSP, 4)),
                  FrameValue::frame(-8));
        EXPECT_EQ(Transfer.read(NdVar::reg(x86reg::RAX, 4)).Constant, 1u);
        EXPECT_EQ(State.load(-8, 4).Constant, 42u);

        auto Malformed = Op;
        Malformed.Output = NdVar::reg(x86reg::RSP, 4);
        EXPECT_FALSE(isFramePreservingIntrinsic(Malformed));
        Malformed = Op;
        Malformed.addInput(NdVar::reg(x86reg::RAX, 4));
        EXPECT_FALSE(isFramePreservingIntrinsic(Malformed));
        Malformed = Op;
        Malformed.MemoryAddressSpace = NdMemoryAddressSpace::X86FS;
        EXPECT_FALSE(isFramePreservingIntrinsic(Malformed));
      }
    EXPECT_EQ(Matches, 1u);
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
