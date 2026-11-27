//===- RegistrationCRTDataStackTests.cpp - Typed CRT data bindings --------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Separate CRT data addresses from callable imports and scalar frame facts.
///
//===----------------------------------------------------------------------===//

#include "../../../lib/ir/low/X86/RegistrationImportLoad.h"
#include "gtest/gtest.h"

#include "neverd/Limits.h"
#include "neverd/ir/low/CFGBuilder.h"
#include "neverd/ir/low/RegistrationABI.h"
#include "neverd/lift/X86/X86Regs.h"
#include "neverd/loader/BinaryImage.h"
#include "neverd/support/BinaryEncoding.h"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <utility>
#include <vector>

namespace {
using namespace neverd;

BinaryImage dataImage(uint32_t Base, const char *Module, const char *Name,
                      unsigned Register, bool Moffs, unsigned Mutation) {
  BinaryImage Image;
  Image.Arch = Arch::X86;
  Image.Bits = Bitness::Bits32;
  Image.Format = BinaryFormat::COFF;
  Image.Base = Base;
  Image.Entry = Base + 0x1000;
  const va_t Slot = Base + 0x3000;
  const va_t Other = Image.Entry + 0x70;
  std::vector<uint8_t> Code;
  auto word = [&](uint32_t Value) {
    size_t At = Code.size();
    Code.resize(At + 4);
    writeLE<uint32_t>(Code.data() + At, Value);
  };
  auto direct = [&](va_t Target) {
    Code.push_back(0xe8);
    word(uint32_t(Target - (Image.Entry + Code.size() + 4)));
  };
  auto load = [&](va_t Target, unsigned Destination, bool Short) {
    if (Short)
      Code.push_back(0xa1);
    else
      Code.insert(Code.end(), {0x8b, uint8_t(0x05 + Destination * 8)});
    word(Target);
  };
  if (Mutation == 5)
    Code.push_back(0x66);
  load(Slot, Register, Moffs);
  unsigned Carrier = Register;
  const unsigned Scratch = Register ? 0 : 1;
  if (Mutation == 6)
    Code.insert(Code.end(), {0x66, uint8_t(0xb8 + Carrier), 0, 0});
  if (Mutation == 7) {
    Carrier = Scratch;
    Code.insert(Code.end(), {0x89, uint8_t(0xc0 + Register * 8 + Carrier)});
  }
  if (Mutation == 8)
    Code.insert(Code.end(), {0x83, uint8_t(0xc0 + Carrier), 0});
  if (Mutation == 9)
    load(Slot + 12, Scratch, false);
  if (Mutation == 25 || Mutation == 27)
    load(Slot + (Mutation == 25 ? 16 : 13), Scratch, false);
  if (Mutation == 26)
    Code.insert(Code.end(), {0x8b, uint8_t(Scratch * 9)});
  if (Mutation == 28 || Mutation == 29) {
    if (Mutation == 28)
      Code.push_back(0x66);
    load(Slot + 12, Carrier, false);
  }
  if (Mutation == 30) {
    Code.insert(Code.end(), {0x83, 0xec, 4, 0x8b, uint8_t(4 + Scratch * 8),
                             0x24, 0x83, 0xc4, 4});
  }
  if (Mutation == 31)
    Code.insert(Code.end(), {0x8b, uint8_t(0x44 + Scratch * 8), 0x24, 0xfc});
  if (Mutation == 13) {
    Code.insert(Code.end(), {0xc7, 0x05});
    word(Slot);
    word(0);
  }
  if (Mutation == 14)
    direct(Other);
  if (Mutation == 15)
    Code.insert(Code.end(), {0xeb, 0});
  if (Mutation == 19)
    Code.insert(Code.end(), {0xff, uint8_t(0xd0 + Carrier)});
  if (Mutation == 20)
    Code.insert(Code.end(), {0x83, uint8_t(0xc0 + Scratch), 1});
  if (Mutation == 10)
    Code.push_back(0x64);
  if (Mutation == 11)
    Code.insert(Code.end(), {0x87, uint8_t(Carrier)});
  else if (Mutation == 12)
    Code.insert(Code.end(), {0xc6, uint8_t(Carrier), 7});
  else {
    Code.insert(Code.end(), {0xc7, uint8_t(Carrier)});
    word(7);
  }
  if (Mutation == 22) {
    load(Slot + 8, Register, Moffs);
    Code.insert(Code.end(), {0xc7, uint8_t(Register)});
    word(9);
  }
  Code.insert(Code.end(), {0x6a, 0, 0xff, 0x15});
  word(Slot + 4);
  Code.insert(Code.end(), {0x33, 0xc0, 0xc3});
  EXPECT_LE(Code.size(), size_t(Other - Image.Entry));
  Code.resize(Other - Image.Entry, 0xcc);
  Code.push_back(0xc3);
  Code.resize(0x80, 0xcc);
  if (Mutation == 24) {
    Code.insert(Code.end(), {0xc7, 0x05});
    word(Slot);
    word(0);
  }
  direct(Image.Entry);
  Code.push_back(0xc3);
  Segment Text;
  Text.Name = ".text";
  Text.VA = Image.Entry;
  Text.Flags = SegmentFlags::Readable | SegmentFlags::Executable;
  if (Mutation == 17)
    Text.Flags = Text.Flags | SegmentFlags::Writable;
  Text.Data = std::move(Code);
  Text.Size = Text.FileSz = Text.Data.size();
  Image.Segments.push_back(std::move(Text));
  Segment Data;
  Data.Name = ".idata";
  Data.VA = Slot;
  Data.Flags = SegmentFlags::Readable | SegmentFlags::Writable;
  Data.Data.resize(16);
  Data.Size = Data.FileSz = Data.Data.size();
  if (Mutation == 18)
    Data.FileSz = 3;
  Image.Segments.push_back(std::move(Data));
  for (const auto &Segment : Image.Segments) {
    Section Section;
    Section.Name = Segment.Name;
    Section.VA = Segment.VA;
    Section.Size = Segment.Size;
    Section.FileSz = Segment.FileSz;
    Section.Flags = Segment.Flags;
    Image.Sections.push_back(std::move(Section));
  }
  for (unsigned I = 0; I != 3; ++I) {
    Import Imported;
    Imported.Name = I == 1 ? "Sleep" : I == 2 ? "_commode" : Name;
    Imported.Module = I == 1 ? "KERNEL32.dll" : Module;
    Imported.IATAddr = Slot + I * 4;
    if (!I && Mutation == 1)
      Imported.Module = "application.dll";
    if (!I && Mutation == 2)
      Imported.Name = "unknown_global";
    if (!I && Mutation == 3)
      Imported.IsDelayImport = true;
    Image.Imports.push_back(Imported);
    EXPECT_TRUE(
        Image.recordImportStorageSlot(Imported.IATAddr, Imported.Name, 0,
                                      ImportStorageEvidence::ImportDirectory));
  }
  if (Mutation == 4)
    Image.ImportStorageSlots.erase(Slot);
  if (Mutation == 21)
    Image.Arch = Arch::X64;
  if (Mutation == 23)
    Image.ImportStorageSlots.at(Slot).Name = "_wrong_storage";
  return Image;
}

TEST(RegistrationCRTDataStack, UCRTCommodeAccessorOwnsOneFullIntStore) {
  enum class Input {
    Current,
    OtherProvider,
    Delay,
    ChangedRegister,
    ShortStore,
    SecondStore
  };
  for (const bool Veneer : {false, true})
    for (const auto Kind :
         {Input::Current, Input::OtherProvider, Input::Delay,
          Input::ChangedRegister, Input::ShortStore, Input::SecondStore}) {
      SCOPED_TRACE(::testing::Message() << Veneer << '/' << int(Kind));
      auto Image = dataImage(0x400000, "api-ms-win-crt-stdio-l1-1-0.dll",
                             "__p__commode", 0, false, 0);
      auto &Accessor = Image.Imports.front();
      if (Kind == Input::OtherProvider)
        Accessor.Module = "api-ms-win-crt-runtime-l1-1-0.dll";
      Accessor.IsDelayImport = Kind == Input::Delay;
      const uint32_t Slot = uint32_t(Accessor.IATAddr);
      auto &Text = Image.Segments.front();
      auto &Code = Text.Data;
      Code.clear();
      auto Word = [&](uint32_t Bits) {
        const size_t At = Code.size();
        Code.resize(At + 4);
        writeLE<uint32_t>(Code.data() + At, Bits);
      };
      if (Veneer) {
        Code.push_back(0xe8);
        Word(0x70 - 5);
      } else {
        Code.insert(Code.end(), {0xff, 0x15});
        Word(Slot);
      }
      // PUSH 1 is the next call's private argument, as in MSVC startup.
      Code.insert(Code.end(), {0x6a, 1});
      if (Kind == Input::ChangedRegister)
        Code.insert(Code.end(), {0x31, 0xc0});
      if (Kind == Input::ShortStore)
        Code.push_back(0x66);
      Code.insert(Code.end(), {0x89, 0x30}); // MOV [EAX], ESI
      if (Kind == Input::SecondStore)
        Code.insert(Code.end(), {0x89, 0x30});
      Code.insert(Code.end(), {0xff, 0x15});
      Word(Slot + 4); // Sleep consumes the private argument.
      Code.push_back(0xc3);
      if (Veneer) {
        Code.resize(0x70, 0xcc);
        Code.insert(Code.end(), {0xff, 0x25});
        Word(Slot);
      }
      Text.Size = Text.FileSz = Code.size();
      Image.Sections.front().Size = Image.Sections.front().FileSz = Text.Size;
      const auto Contract =
          RegistrationCallCalleeIndex(Image).stackContract(Image.Entry);
      EXPECT_EQ(bool(Contract), Kind == Input::Current);
      if (Contract) {
        EXPECT_EQ(Contract->StackPopBytes, 0u);
        EXPECT_EQ(Contract->RequiredImports,
                  (std::vector<va_t>{Slot, Slot + 4}));
        EXPECT_FALSE(Contract->PreservesImportBindings);
      }
    }
}

TEST(RegistrationCRTDataStack, InitializerCallsKeepOnlyProvenCallbackEffects) {
  enum class Table {
    NullOnly,
    Returning,
    WritesUnknownMemory,
    Mutable,
    MissingFixup
  };
  for (const auto Kind :
       {Table::NullOnly, Table::Returning, Table::WritesUnknownMemory,
        Table::Mutable, Table::MissingFixup}) {
    SCOPED_TRACE(int(Kind));
    constexpr va_t Base = 0x400000, TableVA = Base + 0x4000;
    auto Image = dataImage(Base, "api-ms-win-crt-runtime-l1-1-0.dll",
                           "_initterm", 0, false, 0);
    const va_t Slot = Image.Imports.front().IATAddr;
    auto &Text = Image.Segments.front();
    auto &Code = Text.Data;
    Code.clear();
    const auto Address = [&](va_t Value, bool TableArgument = false) {
      const size_t At = Code.size();
      Code.resize(At + 4);
      writeLE<uint32_t>(Code.data() + At, uint32_t(Value));
      Image.BaseRelocations.push_back({Image.Entry + At, 3});
      if (TableArgument)
        Image.DataAddressRelocOperands[Image.Entry + At] = {Value, Value, 4,
                                                            TableVA};
    };
    Code.push_back(0x68);
    Address(TableVA + 8, true);
    Code.push_back(0x68);
    Address(TableVA, true);
    Code.insert(Code.end(), {0xff, 0x15});
    Address(Slot);
    Code.insert(Code.end(), {0x83, 0xc4, 8, 0x6a, 1, 0xff, 0x15});
    Address(Slot + 4);
    Code.push_back(0xc3);
    Code.resize(0x40, 0xcc);
    if (Kind == Table::WritesUnknownMemory)
      Code.insert(Code.end(), {0x89, 0x08}); // MOV [EAX], ECX
    Code.push_back(0xc3);
    Text.Size = Text.FileSz = Code.size();
    Image.Sections.front().Size = Image.Sections.front().FileSz = Text.Size;
    Segment Data;
    Data.Name = ".CRT";
    Data.VA = TableVA;
    Data.Flags = SegmentFlags::Readable;
    if (Kind == Table::Mutable)
      Data.Flags = Data.Flags | SegmentFlags::Writable;
    Data.Size = Data.FileSz = 12;
    Data.Data.resize(12);
    if (Kind != Table::NullOnly) {
      writeLE<uint32_t>(Data.Data.data() + 4, uint32_t(Image.Entry + 0x40));
      Image.CodePtrRelocSlots.insert(TableVA + 4);
      if (Kind != Table::MissingFixup)
        Image.BaseRelocations.push_back({TableVA + 4, 3});
    }
    Section Storage;
    Storage.Name = Data.Name;
    Storage.VA = TableVA;
    Storage.Size = Storage.FileSz = Data.Size;
    Storage.Flags = Data.Flags;
    Image.Sections.push_back(Storage);
    Image.Segments.push_back(std::move(Data));
    const auto Contract =
        RegistrationCallCalleeIndex(Image).stackContract(Image.Entry);
    const bool Expected = Kind == Table::NullOnly || Kind == Table::Returning;
    EXPECT_EQ(bool(Contract), Expected);
    if (Contract) {
      EXPECT_EQ(Contract->StackPopBytes, 0u);
      EXPECT_EQ(Contract->RequiredImports, (std::vector<va_t>{Slot, Slot + 4}));
    }
  }
}

TEST(RegistrationCRTDataStack, CompleteCurrentDataStoresPreserveOnlyBindings) {
  for (uint32_t Base : {0x400000u, 0x18000000u, 0x50000000u})
    for (const char *Module : {"msvcrt.dll", "MSVCR100.DLL"})
      for (const char *Name : {"_fmode", "_commode"})
        for (unsigned Register : {0u, 1u, 2u, 3u, 6u, 7u})
          for (bool Moffs : {false, true}) {
            if (Moffs && Register)
              continue;
            for (unsigned Mutation = 0; Mutation != 24; ++Mutation) {
              SCOPED_TRACE(::testing::Message()
                           << Base << ':' << Module << ':' << Name << ':'
                           << Register << ':' << Moffs << ':' << Mutation);
              const auto Image =
                  dataImage(Base, Module, Name, Register, Moffs, Mutation);
              size_t Work =
                  Mutation == 16 ? limits::kMaxRegistrationEHStateWork : 0;
              const auto Pop =
                  getCheckedX86CalleeStackPop(Image, Image.Entry, &Work);
              const bool Valid = Mutation == 0 || Mutation == 7 ||
                                 Mutation == 9 || Mutation == 20 ||
                                 Mutation == 22;
              EXPECT_EQ(Pop, Valid ? std::optional<uint32_t>{0} : std::nullopt);
              EXPECT_LE(Work, limits::kMaxRegistrationEHStateWork);
              if (!Mutation)
                EXPECT_FALSE(
                    getCheckedX86RegistrationLeafCalleeABI(Image, Image.Entry));
            }
          }
}

TEST(RegistrationCRTDataStack, ClosedReadsRetainOnlyUnchangedDataCarriers) {
  for (uint32_t Base : {0x400000u, 0x18000000u, 0x50000000u})
    for (const char *Module : {"msvcrt.dll", "MSVCR100.DLL"})
      for (const char *Name : {"_fmode", "_commode"})
        for (unsigned Register : {0u, 1u, 2u, 3u, 6u, 7u})
          for (bool Moffs : {false, true}) {
            if (Moffs && Register)
              continue;
            for (unsigned Mutation = 25; Mutation != 32; ++Mutation) {
              SCOPED_TRACE(::testing::Message()
                           << Base << ':' << Module << ':' << Name << ':'
                           << Register << ':' << Moffs << ':' << Mutation);
              const auto Image =
                  dataImage(Base, Module, Name, Register, Moffs, Mutation);
              // Only the allocated private read is closed and leaves the
              // carrier untouched. The others read unknown/unmapped storage,
              // overrun a mapping, or overwrite all/part of the carrier.
              EXPECT_EQ(getCheckedX86CalleeStackPop(Image, Image.Entry),
                        Mutation == 30 ? std::optional<uint32_t>{0}
                                       : std::nullopt);
              EXPECT_FALSE(
                  getCheckedX86RegistrationLeafCalleeABI(Image, Image.Entry));
            }
          }
}

TEST(RegistrationCRTDataStack, CallerMustKeepTheTransitiveDataSlotBinding) {
  for (const char *Name : {"_fmode", "_commode"})
    for (bool Rebind : {false, true}) {
      const auto Image =
          dataImage(0x400000, "MSVCR100.dll", Name, 0, true, Rebind ? 24 : 0);
      Decoder Dec;
      ASSERT_TRUE(Dec.init(Image));
      auto Function = CFGBuilder().buildIntegerFrameProof(
          Image, Dec, Image.Entry + 0x80, "crt-data-caller");
      RegistrationCallCalleeIndex Index(Image);
      const auto Contracts = Index.stackContracts(Function);
      ASSERT_TRUE(Contracts);
      ASSERT_EQ(Contracts->size(), 1u);
      EXPECT_EQ(Contracts->front().Target, Image.Entry);
      EXPECT_EQ(Contracts->front().RequiredImports,
                (std::vector<va_t>{Image.Base + 0x3000, Image.Base + 0x3004}));
      EXPECT_FALSE(Contracts->front().PreservesImportBindings);
      EXPECT_EQ(Index.stackPop(Image.Entry + 0x80),
                Rebind ? std::nullopt : std::optional<uint32_t>{0});
    }
}

TEST(RegistrationCRTDataStack, TypedLoadRequiresCurrentBytesLowIRAndStorage) {
  using registration_abi::checkedRegistrationImportLoad;
  using registration_abi::RegistrationImportLoad;
  using registration_abi::RegistrationImportLoadPolicy;
  for (uint32_t Base : {0x400000u, 0x18000000u})
    for (const char *Module : {"MSVCRT.dll", "msvcr100.dll"})
      for (const char *Name : {"_fmode", "_commode"})
        for (unsigned Register : {0u, 1u, 2u, 3u, 6u, 7u})
          for (bool Moffs : {false, true}) {
            if (Moffs && Register)
              continue;
            for (unsigned Mutation = 0; Mutation != 24; ++Mutation) {
              SCOPED_TRACE(::testing::Message()
                           << Base << ':' << Module << ':' << Name << ':'
                           << Register << ':' << Moffs << ':' << Mutation);
              auto Image = dataImage(Base, Module, Name, Register, Moffs, 0);
              Decoder Dec;
              ASSERT_TRUE(Dec.init(Image));
              auto Function = CFGBuilder().buildIntegerFrameProof(
                  Image, Dec, Image.Entry, "crt-data-receipt");
              ASSERT_FALSE(Function.Blocks.empty());
              auto &Block = Function.Blocks.front();
              ASSERT_FALSE(Block.InstructionBoundaries.empty());
              auto &Boundary = Block.InstructionBoundaries.front();
              ASSERT_EQ(Boundary.OpCount, 3u);
              ASSERT_GE(Block.Ops.size(), Boundary.OpCount);
              auto &LoadOp = Block.Ops[1];
              auto &Destination = Block.Ops[2];
              auto &Text = Image.Segments.front();
              if (Mutation == 1)
                Text.Data[0] = Moffs ? 0xb8 : 0x89;
              if (Mutation == 2)
                Text.Data[Moffs ? 1 : 2] += 4;
              if (Mutation == 3)
                Text.Flags = Text.Flags | SegmentFlags::Writable;
              if (Mutation == 4)
                Block.Ops[0].Inputs[0].Offset += 4;
              if (Mutation == 5)
                Destination.Output = NdVar::reg(
                    ((Register + 1) % 8) * x86reg::GeneralRegStride, 4);
              if (Mutation == 6)
                LoadOp.Output.Size = 2;
              if (Mutation == 7)
                Block.Ops[0].Opcode = NdOp::INT_SEXT;
              if (Mutation == 8)
                LoadOp.MemoryAddressSpace = NdMemoryAddressSpace::X86FS;
              if (Mutation == 9)
                LoadOp.MemoryOrdering = NdMemoryOrdering::Acquire;
              if (Mutation == 10)
                --Boundary.OpCount;
              if (Mutation == 11)
                ++Boundary.FirstOp;
              if (Mutation == 12)
                Image.Imports[0].IsDelayImport = true;
              if (Mutation == 13)
                Image.Imports[0].Module = "application.dll";
              if (Mutation == 14)
                Image.ImportStorageSlots.clear();
              if (Mutation == 15)
                Image.ImportStorageSlots.at(Base + 0x3000).Name = "Sleep";
              if (Mutation == 16)
                Destination.Inputs[0] = NdVar::tmp(0x1234, 4);
              if (Mutation == 17)
                ++LoadOp.Seq;
              if (Mutation == 18)
                Boundary.ControlFlags = LowInstructionControlFlag::Conditional;
              if (Mutation == 19)
                Image.Bits = Bitness::Bits64;
              if (Mutation == 20) {
                Import Conflict = Image.Imports.front();
                Conflict.Name = "Sleep";
                Image.Imports.push_back(Conflict);
              }
              if (Mutation == 22)
                ++LoadOp.Addr;
              if (Mutation == 23)
                --Boundary.Size;
              size_t Work = 0;
              EXPECT_FALSE(
                  checkedRegistrationImportLoad(Image, Block, Boundary, Work));
              Work = Mutation == 21 ? limits::kMaxRegistrationEHStateWork : 0;
              const auto Load = checkedRegistrationImportLoad(
                  Image, Block, Boundary, Work,
                  RegistrationImportLoadPolicy::IncludeCRTData);
              ASSERT_EQ(Load.has_value(), Mutation == 0);
              EXPECT_LE(Work, limits::kMaxRegistrationEHStateWork);
              if (Load) {
                EXPECT_EQ(Load->Storage, RegistrationImportLoad::Kind::CRTData);
                EXPECT_EQ(Load->Slot, Base + 0x3000);
                EXPECT_EQ(Load->Register, Register * x86reg::GeneralRegStride);
                EXPECT_EQ(Load->OpSeq, 1);
              }
            }
          }
}
} // namespace
