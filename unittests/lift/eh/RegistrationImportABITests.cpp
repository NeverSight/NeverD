//===- RegistrationImportABITests.cpp - PE32 import proof boundaries ------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Keep imported stack cleanup separate from dispatch and memory effects.

#include "../../../lib/ir/low/X86/RegistrationABIPrivate.h"
#include "../../../lib/ir/low/X86/RegistrationFrameEffects.h"
#include "gtest/gtest.h"

#include "neverd/Limits.h"
#include "neverd/ir/intrinsics/Intrinsics.h"
#include "neverd/ir/low/CFGBuilder.h"
#include "neverd/ir/low/LowIR.h"
#include "neverd/ir/low/RegistrationABI.h"
#include "neverd/lift/X86Regs.h"
#include "neverd/loader/BinaryImage.h"
#include "neverd/support/BinaryEncoding.h"

#include "llvm/ADT/StringRef.h"

#include <cstddef>
#include <cstdint>
#include <initializer_list>
#include <optional>
#include <vector>

using namespace neverd;

namespace {
struct ImportCase {
  const char *Name;
  unsigned Arguments;
};

constexpr ImportCase StartupImports[] = {{"GetStartupInfoW", 1},
                                         {"HeapSetInformation", 4},
                                         {"InterlockedCompareExchange", 3},
                                         {"InterlockedExchange", 2},
                                         {"DecodePointer", 1},
                                         {"EncodePointer", 1},
                                         {"Sleep", 1}};

struct ImportImage {
  static constexpr va_t TextVA = 0x401000;
  static constexpr va_t StubVA = TextVA + 0x30;
  static constexpr va_t IATVA = 0x403000;
  BinaryImage Image;
  va_t CallVA;

  ImportImage(const ImportCase &C, bool ThroughStub,
              bool CallerCleanup = false) {
    Image.Arch = Arch::X86;
    Image.Bits = Bitness::Bits32;
    Image.Format = BinaryFormat::COFF;
    Image.Base = 0x400000;
    Image.Entry = TextVA;
    Segment Text;
    Text.Name = ".text";
    Text.VA = TextVA;
    Text.Flags = SegmentFlags::Readable | SegmentFlags::Executable;
    for (unsigned I = 0; I != C.Arguments; ++I)
      Text.Data.insert(Text.Data.end(), {0x6a, 0});
    CallVA = TextVA + Text.Data.size();
    if (ThroughStub) {
      Text.Data.push_back(0xe8);
      const size_t Field = Text.Data.size();
      Text.Data.resize(Field + 4);
      writeLE<uint32_t>(Text.Data.data() + Field, StubVA - (CallVA + 5));
    } else {
      Text.Data.insert(Text.Data.end(), {0xff, 0x15, 0, 0x30, 0x40, 0});
    }
    if (CallerCleanup)
      Text.Data.insert(Text.Data.end(), {0x83, 0xc4, uint8_t(C.Arguments * 4)});
    Text.Data.insert(Text.Data.end(), {0x31, 0xc0, 0xc3});
    Text.Data.resize(StubVA - TextVA, 0xcc);
    Text.Data.insert(Text.Data.end(), {0xff, 0x25, 0, 0x30, 0x40, 0});
    Text.Size = Text.FileSz = Text.Data.size();
    Image.Segments.push_back(Text);
    Segment Data;
    Data.Name = ".idata";
    Data.VA = IATVA;
    Data.Flags = SegmentFlags::Readable | SegmentFlags::Writable;
    Data.Data.resize(8);
    Data.Size = Data.FileSz = Data.Data.size();
    Image.Segments.push_back(Data);
    for (const auto &Segment : Image.Segments) {
      Section Section;
      Section.Name = Segment.Name;
      Section.VA = Segment.VA;
      Section.Size = Section.FileSz = Segment.Size;
      Section.Flags = Segment.Flags;
      Image.Sections.push_back(Section);
    }
    Import Imported;
    Imported.Name = C.Name;
    Imported.Module = "KERNEL32.dll";
    Imported.IATAddr = IATVA;
    Image.Imports.push_back(Imported);
    EXPECT_TRUE(Image.recordImportStorageSlot(
        IATVA, C.Name, 0, ImportStorageEvidence::ImportDirectory));
  }

  LowFunc caller() const {
    LowFunc Caller;
    Caller.Entry = 0x402000;
    Caller.Blocks.resize(1);
    LowOp Call;
    Call.Opcode = NdOp::CALL;
    Call.addInput(NdVar::cst(TextVA, 4));
    Caller.Blocks[0].Ops.push_back(Call);
    return Caller;
  }
};
} // namespace

TEST(RegistrationImportABI, StartupCleanupRequiresCurrentProviderAndStorage) {
  for (const auto &C : StartupImports)
    for (bool ThroughStub : {false, true})
      for (unsigned Mutation = 0; Mutation != 23; ++Mutation) {
        SCOPED_TRACE(::testing::Message()
                     << C.Name << ' ' << ThroughStub << ' ' << Mutation);
        ImportImage F(C, ThroughStub);
        auto &Image = F.Image;
        auto &Text = Image.Segments[0];
        auto &Data = Image.Segments[1];
        if (Mutation == 1)
          Image.Imports[0].Module = "kernel32.DLL";
        if (Mutation == 2)
          Image.Imports[0].Module = "custom.dll";
        if (Mutation == 3)
          Image.Imports[0].IsDelayImport = true;
        if (Mutation == 4)
          Image.ConflictingImportStorageSlots.insert(ImportImage::IATVA);
        if (Mutation == 5)
          Image.ImportStorageSlots.at(ImportImage::IATVA).Addend = 4;
        if (Mutation == 6)
          Data.FileSz = 3;
        if (Mutation == 7)
          Image.Sections.pop_back();
        if (Mutation == 8) {
          Image.Imports.push_back(Image.Imports[0]);
          Image.Imports.back().Module = "custom.dll";
        }
        if (Mutation == 9)
          Image.ImportStorageSlots.at(ImportImage::IATVA).Name = "different";
        if (Mutation == 10)
          Image.ImportStorageSlots.at(ImportImage::IATVA).Evidence =
              ImportStorageEvidence::PointerTable;
        if (Mutation == 11)
          Image.Arch = Arch::X64;
        if (Mutation == 12)
          Image.Bits = Bitness::Bits64;
        if (Mutation == 13)
          Image.Format = BinaryFormat::ELF;
        if (Mutation == 14)
          Text.Data[0] = Text.Data[1] = 0x90; // Missing argument push.
        if (Mutation == 15) {
          const size_t Return = F.CallVA - Text.VA + (ThroughStub ? 5 : 6);
          Text.Data[Return] = 0x83;
          Text.Data[Return + 1] = 0xc4;
          Text.Data[Return + 2] = 4; // Caller also pops one argument.
          Text.Data[Return + 3] = 0xc3;
        }
        if (Mutation == 16) {
          Image.Imports[0].Name += "_unchecked";
          Image.ImportStorageSlots.at(ImportImage::IATVA).Name =
              Image.Imports[0].Name;
        }
        if (Mutation == 17)
          Image.Segments.push_back(Data);
        if (Mutation == 18)
          Image.Imports[0].IATAddr += 4;
        if (Mutation == 19)
          Text.Flags = Text.Flags | SegmentFlags::Writable;
        if (Mutation == 20) {
          Image.Imports.push_back(Image.Imports[0]);
          Image.Imports.back().IsDelayImport = true;
        }
        if (Mutation == 21)
          Image.Imports[0].Module = "ntdll.dll";
        if (Mutation == 22) {
          const size_t Field = ThroughStub ? ImportImage::StubVA - Text.VA + 2
                                           : F.CallVA - Text.VA + 2;
          writeLE<uint32_t>(Text.Data.data() + Field, ImportImage::IATVA + 4);
        }
        EXPECT_EQ(getCheckedX86CalleeStackPop(Image, ImportImage::TextVA),
                  Mutation < 2 ? std::optional<uint32_t>{0} : std::nullopt);
        EXPECT_EQ(registration_abi::preservesRegistrationImportBindings(
                      Image, ImportImage::IATVA),
                  (llvm::StringRef(C.Name) == "DecodePointer" ||
                   llvm::StringRef(C.Name) == "EncodePointer") &&
                      (Mutation < 2 || Mutation == 14 || Mutation == 15 ||
                       Mutation == 19 || Mutation == 22));
        // Stack balance grants neither a private object borrow nor memory
        // initialization, even when the imported routine writes its arguments.
        EXPECT_FALSE(getCheckedX86RegistrationObjectCalleeABI(
            Image, ImportImage::TextVA));
        EXPECT_FALSE(getCheckedX86RegistrationInitializerCalleeABI(
            Image, ImportImage::TextVA));
      }
}

TEST(RegistrationImportABI, StackCleanupCannotAuthorizeDispatchOrObservation) {
  using namespace registration_state;
  for (const auto &C :
       {ImportCase{"RaiseException", 4}, ImportCase{"HeapSetInformation", 4},
        ImportCase{"malloc", 4}, ImportCase{"_initterm", 2},
        ImportCase{"_initterm_e", 2}, ImportCase{"_lock", 1},
        ImportCase{"_unlock", 1}, ImportCase{"_onexit", 1}}) {
    SCOPED_TRACE(C.Name);
    const bool IsRaise = llvm::StringRef(C.Name) == "RaiseException";
    const bool IsCRT = llvm::StringRef(C.Name) == "malloc" ||
                       llvm::StringRef(C.Name) == "_initterm" ||
                       llvm::StringRef(C.Name) == "_initterm_e" ||
                       llvm::StringRef(C.Name) == "_lock" ||
                       llvm::StringRef(C.Name) == "_unlock" ||
                       llvm::StringRef(C.Name) == "_onexit";
    ImportImage F(C, false, IsCRT);
    if (IsCRT)
      F.Image.Imports[0].Module = "msvcrt.dll";
    EXPECT_EQ(getCheckedX86CalleeStackPop(F.Image, ImportImage::TextVA),
              std::optional<uint32_t>{0});
    FrameState State;
    State.Registers[x86reg::RSP / x86reg::GeneralRegStride] =
        FrameValue::frame(-64);
    for (unsigned I = 0; I != C.Arguments; ++I)
      State.store(-64 + I * 4, 4, FrameValue::constant(0));
    LowOp Call;
    Call.Opcode = NdOp::INDIR_CALL;
    Call.Addr = F.CallVA;
    Call.Seq = 0;
    Call.addInput(NdVar::cst(ImportImage::IATVA, 4));
    EXPECT_EQ(
        registration_abi::hasClosedRegistrationRaise(F.Image, Call, State),
        IsRaise);
    EXPECT_EQ(hasCallerCleanupRegistrationABI(F.caller(), F.Image), IsRaise);
  }
}

TEST(RegistrationImportABI, LegacyCRTCallsRetainTheirCallerStackCleanup) {
  for (const auto &C : {ImportCase{"_initterm", 2},
                        ImportCase{"_initterm_e", 2}, ImportCase{"_lock", 1},
                        ImportCase{"_unlock", 1}, ImportCase{"_onexit", 1}})
    for (const char *Module : {"msvcrt.dll", "MSVCR100.DLL"})
      for (bool ThroughStub : {false, true})
        for (unsigned Mutation = 0; Mutation != 13; ++Mutation) {
          SCOPED_TRACE(::testing::Message() << C.Name << ' ' << Module << ' '
                                            << ThroughStub << ' ' << Mutation);
          ImportImage F(C, ThroughStub, true);
          auto &Image = F.Image;
          Image.Imports[0].Module = Module;
          if (Mutation == 1)
            Image.Imports[0].Module = "kernel32.dll";
          if (Mutation == 2)
            Image.Imports[0].IsDelayImport = true;
          if (Mutation == 3)
            Image.Segments[1].FileSz = 3;
          if (Mutation == 4)
            Image.ImportStorageSlots.at(ImportImage::IATVA).Evidence =
                ImportStorageEvidence::PointerTable;
          if (Mutation == 5) {
            Image.Imports.push_back(Image.Imports[0]);
            Image.Imports.back().Module = "unproved.dll";
          }
          if (Mutation == 6)
            Image.Segments[0].Flags =
                Image.Segments[0].Flags | SegmentFlags::Writable;
          if (Mutation == 7) {
            const va_t Address = ThroughStub ? ImportImage::StubVA : F.CallVA;
            writeLE<uint32_t>(Image.Segments[0].Data.data() + Address -
                                  ImportImage::TextVA + 2,
                              ImportImage::IATVA + 4);
          }
          if (Mutation == 8) {
            Image.Imports[0].Name += "@" + std::to_string(C.Arguments * 4);
            Image.ImportStorageSlots.at(ImportImage::IATVA).Name =
                Image.Imports[0].Name;
          }
          if (Mutation == 9 || Mutation == 10) {
            const size_t Cleanup =
                F.CallVA - ImportImage::TextVA + (ThroughStub ? 5 : 6);
            // Missing or partial caller cleanup must not become stdcall.
            Image.Segments[0].Data[Cleanup + 2] =
                Mutation == 9 ? 0 : uint8_t(C.Arguments * 2);
          }
          if (Mutation == 11)
            Image.Arch = Arch::X64;
          if (Mutation == 12) {
            Image.Imports[0].Name += "_m";
            Image.ImportStorageSlots.at(ImportImage::IATVA).Name =
                Image.Imports[0].Name;
          }
          EXPECT_EQ(getCheckedX86CalleeStackPop(Image, ImportImage::TextVA),
                    Mutation == 0 ? std::optional<uint32_t>{0} : std::nullopt);
          // CRT locks and callback registration are not private constructor
          // borrows. Stack balance grants no callback or unwind effect.
          EXPECT_FALSE(getCheckedX86RegistrationObjectCalleeABI(
              Image, ImportImage::TextVA));
          EXPECT_FALSE(getCheckedX86RegistrationInitializerCalleeABI(
              Image, ImportImage::TextVA));
          EXPECT_FALSE(registration_abi::isCheckedRegistrationRaiseSlot(
              Image, ImportImage::IATVA));
          if (Mutation == 0) {
            Decoder Dec;
            ASSERT_TRUE(Dec.init(Image));
            const auto Function = CFGBuilder().buildIntegerFrameProof(
                Image, Dec, ImportImage::TextVA, "legacy-crt-call");
            RegistrationCallCalleeIndex Index(Image);
            const auto Contracts = Index.stackContracts(Function);
            ASSERT_TRUE(Contracts);
            ASSERT_EQ(Contracts->size(), 1u);
            EXPECT_EQ(Contracts->front().StackPopBytes, 0u);
            EXPECT_EQ(Contracts->front().Indirect, !ThroughStub);
          }
        }
}

TEST(RegistrationImportABI, CachedImportTargetsKeepOnlyTheirReturningStackABI) {
  for (uint8_t Register : {0, 1, 2, 3, 6, 7})
    for (bool CallerCleanup : {false, true})
      for (unsigned Mutation = 0; Mutation != 22; ++Mutation) {
        SCOPED_TRACE(::testing::Message() << unsigned(Register) << ' '
                                          << CallerCleanup << ' ' << Mutation);
        ImportImage F({CallerCleanup ? "_initterm_e" : "DecodePointer",
                       CallerCleanup ? 2u : 1u},
                      false, CallerCleanup);
        auto &Image = F.Image;
        if (CallerCleanup)
          Image.Imports[0].Module = "msvcr100.dll";
        auto &Text = Image.Segments[0];
        auto &Bytes = Text.Data;
        Bytes.clear();
        const auto StoreSlot = [&] {
          Bytes.insert(Bytes.end(), {0xc7, 0x05, 0, 0x30, 0x40, 0, 0, 0, 0, 0});
        };
        const auto OpaqueCall = [&] {
          Bytes.push_back(0xe8);
          const size_t Field = Bytes.size();
          Bytes.resize(Field + 4);
          writeLE<uint32_t>(Bytes.data() + Field,
                            ImportImage::StubVA - (Text.VA + Field + 4));
        };
        const auto ImportCall = [&] {
          for (unsigned I = 0; I != (CallerCleanup ? 2u : 1u); ++I)
            Bytes.insert(Bytes.end(), {0x6a, 0});
          Bytes.insert(Bytes.end(), {0xff, 0x15, 0, 0x30, 0x40, 0});
          if (CallerCleanup)
            Bytes.insert(Bytes.end(), {0x83, 0xc4, 8});
        };
        if (Mutation == 6)
          StoreSlot();
        if (Mutation == 16)
          ImportCall();
        if (Mutation == 8)
          Bytes.insert(Bytes.end(), {0x89, 0x01}); // Unknown memory alias.
        if (Mutation == 9)
          OpaqueCall();
        if (Mutation == 19)
          Bytes.insert(Bytes.end(), {0x64, 0x89, 0x01});
        if (Mutation == 21)
          Bytes.push_back(0x66);
        if (Mutation == 20 || Mutation == 21)
          Bytes.insert(Bytes.end(), {0x64, 0x89, 0x15, 0, 0, 0, 0});
        if (Mutation == 3)
          Bytes.push_back(0x66); // A partial target load is not a pointer.
        Bytes.insert(Bytes.end(),
                     {0x8b, uint8_t(0x05 + Register * 8),
                      uint8_t(Mutation == 5 ? 4 : 0), 0x30, 0x40, 0});
        if (Mutation == 4)
          Bytes.insert(Bytes.end(), {0x66, uint8_t(0xb8 + Register), 0, 0});
        if (Mutation == 7)
          StoreSlot(); // The cached pointer remains the earlier value.
        if (Mutation == 10 || Mutation == 14)
          OpaqueCall();
        if (Mutation == 17)
          StoreSlot();
        if (Mutation == 18)
          ImportCall();
        if (Mutation == 13 || Mutation == 17 || Mutation == 18)
          ImportCall();
        uint8_t CallRegister = Register;
        if (Mutation == 15) {
          CallRegister = Register == 3 ? 6 : 3;
          Bytes.insert(Bytes.end(),
                       {0x89, uint8_t(0xc0 + Register * 8 + CallRegister)});
        }
        if (Mutation == 11 || Mutation == 12) {
          Bytes.insert(Bytes.end(), {0x85, 0xc0, 0x74, 0x02});
          Bytes.insert(Bytes.end(),
                       Mutation == 11
                           ? std::initializer_list<uint8_t>{0x90, 0x90}
                           : std::initializer_list<uint8_t>{
                                 0x31, uint8_t(0xc0 + Register * 9)});
        }
        for (unsigned I = 0; I != (CallerCleanup ? 2u : 1u); ++I)
          Bytes.insert(Bytes.end(), {0x6a, 0});
        Bytes.insert(Bytes.end(), {0xff, uint8_t(0xd0 + CallRegister)});
        if (CallerCleanup)
          Bytes.insert(Bytes.end(), {0x83, 0xc4, 8});
        Bytes.insert(Bytes.end(), {0x31, 0xc0, 0xc3});
        ASSERT_LT(Bytes.size(), ImportImage::StubVA - Text.VA);
        Bytes.resize(ImportImage::StubVA - Text.VA, 0xcc);
        if (Mutation == 14)
          Bytes.insert(Bytes.end(), {0x31, uint8_t(0xc0 + Register * 9)});
        Bytes.push_back(0xc3);
        Text.Size = Text.FileSz = Bytes.size();
        Image.Sections[0].Size = Image.Sections[0].FileSz = Text.Size;
        if (Mutation == 1)
          Image.Imports[0].Module = "unproved.dll";
        if (Mutation == 2)
          Image.Imports[0].IsDelayImport = true;
        const bool Valid =
            Mutation == 0 || Mutation == 7 || Mutation == 11 ||
            Mutation == 15 || Mutation == 20 ||
            (Mutation == 13 && Register >= 3) ||
            (!CallerCleanup &&
             (Mutation == 16 || (Mutation == 18 && Register >= 3)));
        EXPECT_EQ(getCheckedX86CalleeStackPop(Image, ImportImage::TextVA),
                  Valid ? std::optional<uint32_t>{0} : std::nullopt);
        EXPECT_FALSE(getCheckedX86RegistrationObjectCalleeABI(
            Image, ImportImage::TextVA));
        EXPECT_FALSE(getCheckedX86RegistrationInitializerCalleeABI(
            Image, ImportImage::TextVA));
      }
}

TEST(RegistrationImportABI, CachedImportsNeedEveryInvokedSlotBinding) {
  for (unsigned Mutation = 0; Mutation != 7; ++Mutation) {
    SCOPED_TRACE(Mutation);
    ImportImage F({"DecodePointer", 1}, false);
    auto &Image = F.Image;
    auto Second = Image.Imports[0];
    Second.Name = "Sleep";
    Second.IATAddr += 4;
    Image.Imports.push_back(Second);
    ASSERT_TRUE(
        Image.recordImportStorageSlot(Second.IATAddr, Second.Name, 0,
                                      ImportStorageEvidence::ImportDirectory));
    Image.Segments[1].Data.resize(12);
    Image.Segments[1].Size = Image.Segments[1].FileSz = 12;
    Image.Sections[1].Size = Image.Sections[1].FileSz = 12;
    auto &Text = Image.Segments[0];
    auto &Bytes = Text.Data;
    Bytes.clear();
    const auto ComputedStore = [&] {
      // Build 0x403004 from immutable scalar immediates rather than an
      // absolute relocatable operand; the stack proof retains this address.
      Bytes.insert(Bytes.end(), {0x31, 0xd2, 0x83, 0xc2, 0x40, 0xc1, 0xe2, 8,
                                 0x83, 0xc2, 0x30, 0xc1, 0xe2, 8,    0x83, 0xc2,
                                 4,    0xc7, 2,    0,    0,    0,    0});
    };
    const auto Store = [&] {
      Bytes.insert(Bytes.end(), {0xc7, 0x05, uint8_t(Mutation == 4 ? 8 : 4),
                                 0x30, 0x40, 0, 0, 0, 0, 0});
    };
    if (Mutation == 1)
      Store();
    if (Mutation == 5)
      ComputedStore();
    Bytes.insert(Bytes.end(), {0x8b, 0x35, 0, 0x30, 0x40, 0});
    if (Mutation == 2 || Mutation == 4)
      Store();
    if (Mutation == 6)
      ComputedStore();
    Bytes.insert(Bytes.end(), {0x6a, 0, 0xff, 0x15, 4, 0x30, 0x40, 0});
    if (Mutation == 3)
      Store();
    Bytes.insert(Bytes.end(), {0x6a, 0, 0xff, 0xd6, 0x31, 0xc0, 0xc3});
    Text.Size = Text.FileSz = Bytes.size();
    Image.Sections[0].Size = Image.Sections[0].FileSz = Text.Size;
    EXPECT_EQ(getCheckedX86CalleeStackPop(Image, ImportImage::TextVA),
              Mutation == 0 || Mutation == 3 || Mutation == 4
                  ? std::optional<uint32_t>{0}
                  : std::nullopt);
  }
}

TEST(RegistrationImportABI, RelocatedStoresRetainExactImportAliasBounds) {
  for (uint32_t Slide : {0u, 0x17c00000u})
    for (unsigned Form = 0; Form != 3; ++Form)
      for (unsigned Offset : {0u, 1u, 3u, 4u, 8u})
        for (unsigned Mutation = 0; Mutation != 4; ++Mutation) {
          SCOPED_TRACE(::testing::Message() << Slide << ' ' << Form << ' '
                                            << Offset << ' ' << Mutation);
          ImportImage F({"Sleep", 1}, false);
          auto &Image = F.Image;
          Image.Base += Slide;
          Image.Entry += Slide;
          for (auto &Segment : Image.Segments)
            Segment.VA += Slide;
          for (auto &Section : Image.Sections)
            Section.VA += Slide;
          const va_t Slot = ImportImage::IATVA + Slide;
          Image.Imports[0].IATAddr = Slot;
          Image.ImportStorageSlots.clear();
          ASSERT_TRUE(Image.recordImportStorageSlot(
              Slot, "Sleep", 0, ImportStorageEvidence::ImportDirectory));
          auto &Data = Image.Segments[1];
          Data.Data.resize(16);
          Data.Size = Data.FileSz = Data.Data.size();
          Image.Sections[1].Size = Image.Sections[1].FileSz = Data.Size;
          auto &Text = Image.Segments[0];
          auto &Bytes = Text.Data;
          Bytes.clear();
          if (Form == 0)
            Bytes.insert(Bytes.end(), {0xc7, 0x05}); // mov [address], 0
          else
            Bytes.push_back(0xba); // mov edx, address
          const size_t Field = Bytes.size();
          Bytes.resize(Field + 4);
          const uint32_t Address = Slot + Offset - (Form == 2 ? 4 : 0);
          writeLE<uint32_t>(Bytes.data() + Field, Address);
          if (Form == 2)
            Bytes.insert(Bytes.end(), {0x83, 0xc2, 4});
          if (Form != 0)
            Bytes.insert(Bytes.end(), {0xc7, 2}); // mov [edx], 0
          Bytes.insert(Bytes.end(), {0, 0, 0, 0, 0x6a, 0, 0xff, 0x15});
          const size_t CallField = Bytes.size();
          Bytes.resize(CallField + 4);
          writeLE<uint32_t>(Bytes.data() + CallField, Slot);
          Bytes.insert(Bytes.end(), {0x31, 0xc0, 0xc3});
          Text.Size = Text.FileSz = Bytes.size();
          Image.Sections[0].Size = Image.Sections[0].FileSz = Text.Size;
          Image.BaseRelocations = {{Text.VA + Field, 3}};
          Image.DataAddressRelocOperands[Text.VA + Field] = {Address, Address,
                                                             4, Data.VA};
          if (Mutation == 1)
            Image.BaseRelocations.push_back(Image.BaseRelocations.front());
          if (Mutation == 2)
            Image.BaseRelocations[0].Type = 10;
          if (Mutation == 3)
            Image.DataAddressRelocOperands.at(Text.VA + Field).TargetVA += 4;
          EXPECT_EQ(getCheckedX86CalleeStackPop(Image, Text.VA),
                    Mutation == 0 && Offset >= 4 ? std::optional<uint32_t>{0}
                                                 : std::nullopt);
        }
}

TEST(RegistrationImportABI, DirectCleanupNeedsTheInvokedImportBinding) {
  for (unsigned Form = 0; Form != 3; ++Form)
    for (bool Overwrite : {false, true}) {
      SCOPED_TRACE(Form);
      SCOPED_TRACE(Overwrite);
      ImportImage F({"Sleep", 1}, Form != 0);
      auto &Image = F.Image;
      auto &Text = Image.Segments[0];
      auto &Bytes = Text.Data;
      Bytes.clear();
      if (Overwrite)
        Bytes.insert(Bytes.end(),
                     {0xc7, 0x05, 0, 0x30, 0x40, 0, 0x36, 0x10, 0x40, 0});
      // A replacement RET leaves the argument in place. Copying the caller
      // PC here makes the wrapper return with the wrong ESP, not just fault.
      Bytes.insert(Bytes.end(), {0xff, 0x34, 0x24});
      if (Form) {
        const auto CallVA = Text.VA + Bytes.size();
        Bytes.push_back(0xe8);
        const auto Field = Bytes.size();
        Bytes.resize(Field + 4);
        writeLE<uint32_t>(Bytes.data() + Field, ImportImage::StubVA +
                                                    (Form == 2 ? 16 : 0) -
                                                    (CallVA + 5));
      } else
        Bytes.insert(Bytes.end(), {0xff, 0x15, 0, 0x30, 0x40, 0});
      Bytes.insert(Bytes.end(), {0x31, 0xc0, 0xc3});
      Bytes.resize(ImportImage::StubVA - Text.VA, 0xcc);
      Bytes.insert(Bytes.end(), {0xff, 0x25, 0, 0x30, 0x40, 0, 0xc3});
      if (Form == 2) {
        Bytes.resize(ImportImage::StubVA + 16 - Text.VA, 0xcc);
        Bytes.insert(Bytes.end(), {0xff, 0x74, 0x24, 4, 0xe8});
        const auto Field = Bytes.size();
        Bytes.resize(Field + 4);
        writeLE<uint32_t>(Bytes.data() + Field,
                          ImportImage::StubVA - (Text.VA + Field + 4));
        Bytes.insert(Bytes.end(), {0xc2, 4, 0});
      }
      Text.Size = Text.FileSz = Bytes.size();
      Image.Sections[0].Size = Image.Sections[0].FileSz = Text.Size;
      EXPECT_EQ(getCheckedX86CalleeStackPop(Image, ImportImage::TextVA),
                Overwrite ? std::nullopt : std::optional<uint32_t>{0});
      RegistrationCallCalleeIndex Index(Image);
      const auto Contracts = Index.stackContracts(F.caller());
      ASSERT_TRUE(Contracts);
      if (Overwrite)
        EXPECT_TRUE(Contracts->empty());
      else {
        ASSERT_EQ(Contracts->size(), 1u);
        EXPECT_EQ(Contracts->front().RequiredImports,
                  (std::vector<va_t>{ImportImage::IATVA}));
      }
    }
}

TEST(RegistrationImportABI, PointerTransformsRetainOnlyLiveImportBindings) {
  for (const auto *Name : {"DecodePointer", "EncodePointer", "Sleep"})
    for (unsigned Form = 0; Form != 3; ++Form)
      for (unsigned Mutation = 0; Mutation != 6; ++Mutation) {
        SCOPED_TRACE(::testing::Message()
                     << Name << ' ' << Form << ' ' << Mutation);
        ImportImage F({Name, 1}, false);
        auto &Image = F.Image;
        auto &Text = Image.Segments[0];
        auto &Bytes = Text.Data;
        Bytes.clear();
        const auto Store = [&] {
          Bytes.insert(Bytes.end(), {0xc7, 0x05, 0, 0x30, 0x40, 0, 0, 0, 0, 0});
        };
        const auto Invoke = [&] {
          Bytes.insert(Bytes.end(), {0x6a, 0});
          if (Form == 2) {
            const auto CallVA = Text.VA + Bytes.size();
            Bytes.push_back(0xe8);
            const auto Field = Bytes.size();
            Bytes.resize(Field + 4);
            writeLE<uint32_t>(Bytes.data() + Field,
                              ImportImage::StubVA - (CallVA + 5));
          } else
            Bytes.insert(Bytes.end(), {0xff, 0x15, 0, 0x30, 0x40, 0});
        };
        if (Mutation == 1)
          Store();
        Invoke();
        if (Mutation == 2)
          Store();
        if (Mutation == 3) {
          const auto CallVA = Text.VA + Bytes.size();
          Bytes.push_back(0xe8);
          const auto Field = Bytes.size();
          Bytes.resize(Field + 4);
          writeLE<uint32_t>(Bytes.data() + Field,
                            ImportImage::StubVA + 6 - (CallVA + 5));
        }
        if (Form == 1)
          Bytes.insert(Bytes.end(),
                       {0x8b, 0x35, 0, 0x30, 0x40, 0, 0x6a, 0, 0xff, 0xd6});
        else
          Invoke();
        Bytes.insert(Bytes.end(), {0x31, 0xc0, 0xc3});
        ASSERT_LE(Bytes.size(), ImportImage::StubVA - Text.VA);
        Bytes.resize(ImportImage::StubVA - Text.VA, 0xcc);
        Bytes.insert(Bytes.end(), {0xff, 0x25, 0, 0x30, 0x40, 0, 0xc3});
        Text.Size = Text.FileSz = Bytes.size();
        Image.Sections[0].Size = Image.Sections[0].FileSz = Text.Size;
        if (Mutation == 4)
          Image.Imports[0].Module = "unproved.dll";
        if (Mutation == 5)
          Image.Imports[0].IsDelayImport = true;
        const bool Valid = Mutation == 0 && llvm::StringRef(Name) != "Sleep";
        EXPECT_EQ(getCheckedX86CalleeStackPop(Image, ImportImage::TextVA),
                  Valid ? std::optional<uint32_t>{0} : std::nullopt);
        Decoder Dec;
        ASSERT_TRUE(Dec.init(Image));
        const auto Function = CFGBuilder().buildIntegerFrameProof(
            Image, Dec, ImportImage::TextVA, "import-effects");
        RegistrationCallCalleeIndex Index(Image);
        const auto Contracts = Index.stackContracts(Function);
        ASSERT_TRUE(Contracts);
        for (const auto &Contract : *Contracts) {
          if (Contract.Target !=
              (Form == 2 ? ImportImage::StubVA : ImportImage::IATVA))
            continue;
          EXPECT_EQ(Contract.ImportVeneer, Form == 2);
          EXPECT_EQ(Contract.PreservesImportBindings,
                    llvm::StringRef(Name) != "Sleep");
        }
        EXPECT_FALSE(getCheckedX86RegistrationObjectCalleeABI(
            Image, ImportImage::TextVA));
        EXPECT_FALSE(getCheckedX86RegistrationInitializerCalleeABI(
            Image, ImportImage::TextVA));
      }
}

TEST(RegistrationImportABI, ImportVeneersPreserveCachedNonvolatileTargets) {
  for (const auto *Name : {"DecodePointer", "Sleep", "_lock"})
    for (unsigned Mutation = 0; Mutation != 4; ++Mutation) {
      SCOPED_TRACE(::testing::Message() << Name << ' ' << Mutation);
      const bool CallerCleanup = llvm::StringRef(Name) == "_lock";
      ImportImage F({Name, 1}, false, CallerCleanup);
      if (CallerCleanup)
        F.Image.Imports[0].Module = "msvcr100.dll";
      auto &Text = F.Image.Segments[0];
      auto &Bytes = Text.Data;
      Bytes = {0x8b, 0x35, 0, 0x30, 0x40, 0};
      if (Mutation == 1)
        Bytes.insert(Bytes.end(), {0xc7, 0x05, 0, 0x30, 0x40, 0, 0, 0, 0, 0});
      Bytes.insert(Bytes.end(), {0x6a, 0, 0xe8});
      const auto Field = Bytes.size();
      Bytes.resize(Field + 4);
      writeLE<uint32_t>(Bytes.data() + Field,
                        ImportImage::StubVA - (Text.VA + Field + 4));
      if (CallerCleanup)
        Bytes.insert(Bytes.end(), {0x83, 0xc4, 4});
      Bytes.insert(Bytes.end(), {0x6a, 0, 0xff, 0xd6});
      if (CallerCleanup)
        Bytes.insert(Bytes.end(), {0x83, 0xc4, 4});
      Bytes.insert(Bytes.end(), {0x31, 0xc0, 0xc3});
      Bytes.resize(ImportImage::StubVA - Text.VA, 0xcc);
      if (Mutation == 2)
        Bytes.insert(Bytes.end(), {0x31, 0xf6, 0xc2, 4, 0});
      else
        Bytes.insert(Bytes.end(), {0xff, 0x25, 0, 0x30, 0x40, 0});
      Text.Size = Text.FileSz = Bytes.size();
      F.Image.Sections[0].Size = F.Image.Sections[0].FileSz = Text.Size;
      if (Mutation == 3)
        Text.Flags = Text.Flags | SegmentFlags::Writable;
      EXPECT_EQ(getCheckedX86CalleeStackPop(F.Image, ImportImage::TextVA),
                Mutation == 0 ? std::optional<uint32_t>{0} : std::nullopt);
    }
}

TEST(RegistrationImportABI, CachedLoadReceiptsBindCurrentBytesAndLowIR) {
  for (bool AccumulatorForm : {false, true})
    for (unsigned Mutation = 0; Mutation != 20; ++Mutation) {
      SCOPED_TRACE(::testing::Message() << AccumulatorForm << ' ' << Mutation);
      ImportImage F({"DecodePointer", 1}, false);
      auto &Image = F.Image;
      auto &Text = Image.Segments[0];
      Text.Data = AccumulatorForm
                      ? std::vector<uint8_t>{0xa1, 0, 0x30, 0x40, 0}
                      : std::vector<uint8_t>{0x8b, 0x35, 0, 0x30, 0x40, 0};
      Text.Data.insert(Text.Data.end(),
                       {0x6a, 0, 0xff, uint8_t(AccumulatorForm ? 0xd0 : 0xd6),
                        0x31, 0xc0, 0xc3});
      Text.Size = Text.FileSz = Text.Data.size();
      Image.Sections[0].Size = Image.Sections[0].FileSz = Text.Size;
      Decoder Dec;
      ASSERT_TRUE(Dec.init(Image));
      auto Function = CFGBuilder().buildIntegerFrameProof(
          Image, Dec, ImportImage::TextVA, "cached-import");
      ASSERT_FALSE(Function.Blocks.empty());
      auto &Block = Function.Blocks.front();
      ASSERT_GE(Block.Ops.size(), 4u);
      auto &Boundary = Block.InstructionBoundaries.front();
      if (Mutation == 1)
        Text.Data[0] = 0x90;
      if (Mutation == 2)
        Text.Data[AccumulatorForm ? 1 : 2] = 4;
      if (Mutation == 3)
        Text.Flags = Text.Flags | SegmentFlags::Writable;
      if (Mutation == 4)
        Block.Ops[0].Inputs[0].Offset += 4;
      if (Mutation == 5)
        Block.Ops[3].Output = NdVar::reg(x86reg::RDI, 4);
      if (Mutation == 6)
        Block.Ops[2].Output.Size = 2;
      if (Mutation == 7)
        Block.Ops[2].Opcode = NdOp::COPY;
      if (Mutation == 8)
        Block.Ops[2].MemoryAddressSpace = NdMemoryAddressSpace::X86FS;
      if (Mutation == 9)
        Block.Ops[2].MemoryOrdering = NdMemoryOrdering::Acquire;
      if (Mutation == 10)
        Boundary.OpCount = 3;
      if (Mutation == 11)
        Block.InstructionBoundaries.push_back(Boundary);
      if (Mutation == 12)
        Image.Imports[0].IsDelayImport = true;
      if (Mutation == 13)
        Image.Imports[0].Module = "unproved.dll";
      if (Mutation == 14)
        Image.ImportStorageSlots.clear();
      if (Mutation == 15)
        Image.ImportStorageSlots.at(ImportImage::IATVA).Name = "Sleep";
      if (Mutation == 16)
        Block.Ops[3].Inputs[0] = NdVar::tmp(0x1234, 4);
      if (Mutation == 17)
        ++Block.Ops[2].Seq;
      if (Mutation == 18)
        Boundary.ControlFlags = LowInstructionControlFlag::Conditional;
      if (Mutation == 19)
        Image.Bits = Bitness::Bits64;
      RegistrationCallCalleeIndex Index(Image);
      const auto Contracts = Index.stackContracts(Function);
      ASSERT_TRUE(Contracts);
      ASSERT_EQ(Contracts->size(), Mutation == 0 ? 1u : 0u);
      if (Mutation == 0) {
        EXPECT_EQ(Contracts->front().Target, ImportImage::IATVA);
        EXPECT_EQ(Contracts->front().StackPopBytes, 4u);
        EXPECT_TRUE(Contracts->front().Indirect);
        EXPECT_EQ(Contracts->front().Loads, (std::vector<std::pair<va_t, int>>{
                                                {ImportImage::TextVA, 2}}));
      }
    }
}

TEST(RegistrationImportABI, TransitiveImportDependenciesRespectCumulativeWork) {
  ImportImage F({"Sleep", 1}, true);
  size_t Used = 0;
  ASSERT_EQ(getCheckedX86CalleeStackPop(F.Image, ImportImage::TextVA, &Used),
            std::optional<uint32_t>{0});
  ASSERT_GT(Used, 0u);
  ASSERT_LT(Used, limits::kMaxRegistrationEHStateWork);
  for (bool Fits : {false, true}) {
    SCOPED_TRACE(Fits);
    size_t Work = limits::kMaxRegistrationEHStateWork - Used + !Fits;
    EXPECT_EQ(getCheckedX86CalleeStackPop(F.Image, ImportImage::TextVA, &Work),
              Fits ? std::optional<uint32_t>{0} : std::nullopt);
    EXPECT_EQ(Work, limits::kMaxRegistrationEHStateWork);
  }
}

TEST(RegistrationImportABI, CachedImportsRespectCumulativeWork) {
  ImportImage F({"DecodePointer", 1}, false);
  auto &Text = F.Image.Segments[0];
  Text.Data = {0x8b, 0x35, 0,    0x30, 0x40, 0,   0x6a,
               0,    0xff, 0xd6, 0x31, 0xc0, 0xc3};
  Text.Size = Text.FileSz = Text.Data.size();
  F.Image.Sections[0].Size = F.Image.Sections[0].FileSz = Text.Size;
  size_t Used = 0;
  ASSERT_EQ(getCheckedX86CalleeStackPop(F.Image, ImportImage::TextVA, &Used),
            std::optional<uint32_t>{0});
  ASSERT_GT(Used, 0u);
  ASSERT_LT(Used, limits::kMaxRegistrationEHStateWork);
  for (bool Exhausted : {false, true}) {
    size_t Work = limits::kMaxRegistrationEHStateWork - Used + Exhausted;
    EXPECT_EQ(getCheckedX86CalleeStackPop(F.Image, ImportImage::TextVA, &Work),
              Exhausted ? std::nullopt : std::optional<uint32_t>{0});
    EXPECT_EQ(Work, limits::kMaxRegistrationEHStateWork);
  }
  Decoder Dec;
  ASSERT_TRUE(Dec.init(F.Image));
  const auto Function = CFGBuilder().buildIntegerFrameProof(
      F.Image, Dec, ImportImage::TextVA, "cached-import-budget");
  size_t Work = limits::kMaxRegistrationEHStateWork - 1;
  RegistrationCallCalleeIndex Index(F.Image, &Work);
  EXPECT_FALSE(Index.stackContracts(Function));
  EXPECT_EQ(Work, limits::kMaxRegistrationEHStateWork);
}

TEST(RegistrationImportABI, RaiseCleanupRequiresRealImportDirectoryStorage) {
  for (unsigned Mutation = 0; Mutation != 8; ++Mutation) {
    SCOPED_TRACE(Mutation);
    ImportImage F({"RaiseException", 4}, false);
    if (Mutation == 1)
      F.Image.Imports[0].Module = "KernelBase.DLL";
    if (Mutation == 2)
      F.Image.Imports[0].IsDelayImport = true;
    if (Mutation == 3)
      F.Image.Segments[1].FileSz = 3;
    if (Mutation == 4)
      F.Image.ImportStorageSlots.at(ImportImage::IATVA).Evidence =
          ImportStorageEvidence::PointerTable;
    if (Mutation == 5)
      F.Image.Sections.pop_back();
    if (Mutation == 6) {
      F.Image.Imports.push_back(F.Image.Imports[0]);
      F.Image.Imports.back().IsDelayImport = true;
    }
    if (Mutation == 7)
      F.Image.Imports[0].Module = "custom.dll";
    EXPECT_EQ(getCheckedX86CalleeStackPop(F.Image, ImportImage::TextVA),
              Mutation < 2 ? std::optional<uint32_t>{0} : std::nullopt);
  }
}

TEST(RegistrationImportABI, ImportAliasesCannotReplaceCurrentVeneerBytes) {
  for (const auto &C :
       {ImportCase{"RaiseException", 4}, ImportCase{"HeapSetInformation", 4},
        ImportCase{"GetStartupInfoW", 1}}) {
    SCOPED_TRACE(C.Name);
    ImportImage F(C, true);
    ASSERT_TRUE(F.Image.recordImportStub(ImportImage::StubVA, 0));
    EXPECT_EQ(registration_abi::checkedRegistrationImportStackPop(
                  F.Image, ImportImage::IATVA),
              C.Arguments * 4);
    EXPECT_FALSE(registration_abi::checkedRegistrationImportStackPop(
        F.Image, ImportImage::StubVA));
    EXPECT_FALSE(registration_abi::isCheckedRegistrationRaiseSlot(
        F.Image, ImportImage::StubVA));
    EXPECT_EQ(getCheckedX86CalleeStackPop(F.Image, ImportImage::StubVA),
              C.Arguments * 4);
    Decoder Dec;
    ASSERT_TRUE(Dec.init(F.Image));
    const auto Function = CFGBuilder().buildIntegerFrameProof(
        F.Image, Dec, ImportImage::TextVA, "import-wrapper");
    RegistrationCallCalleeIndex Index(F.Image);
    const auto Contracts = Index.stackContracts(Function);
    ASSERT_TRUE(Contracts);
    ASSERT_EQ(Contracts->size(), 1u);
    EXPECT_EQ(Contracts->front().Target, ImportImage::StubVA);
    EXPECT_EQ(Contracts->front().StackPopBytes, C.Arguments * 4);
    EXPECT_FALSE(Contracts->front().Indirect);
    // A stale registered alias cannot restore the old slot's contract after
    // the current veneer was redirected to an unrelated address.
    writeLE<uint32_t>(F.Image.Segments[0].Data.data() + ImportImage::StubVA -
                          ImportImage::TextVA + 2,
                      ImportImage::IATVA + 4);
    EXPECT_FALSE(getCheckedX86CalleeStackPop(F.Image, ImportImage::StubVA));
    EXPECT_FALSE(getCheckedX86CalleeStackPop(F.Image, ImportImage::TextVA));
  }
}

TEST(RegistrationImportABI,
     PrivateRuntimeEffectsRequireTheExactPhysicalInvocation) {
  for (bool Heap : {false, true})
    for (unsigned Mutation = 0; Mutation != 13; ++Mutation) {
      SCOPED_TRACE(Mutation);
      SCOPED_TRACE(Heap);
      ImportImage F(Heap ? ImportCase{"HeapSetInformation", 4}
                         : ImportCase{"GetStartupInfoW", 1},
                    false);
      LowOp Call;
      Call.Opcode = NdOp::INDIR_CALL;
      Call.Addr = F.CallVA;
      Call.Seq = 0;
      Call.addInput(NdVar::cst(ImportImage::IATVA, 4));
      size_t Work = 0;
      if (Mutation == 1)
        Call.Opcode = NdOp::CALL;
      if (Mutation == 2)
        Call.Seq = -1;
      if (Mutation == 3)
        Call.MemoryAddressSpace = NdMemoryAddressSpace::X86FS;
      if (Mutation == 4)
        Call.Inputs[0].Size = 8;
      if (Mutation == 5)
        Call.NumInputs = 0;
      if (Mutation == 6)
        F.Image.Segments[0].Data[Call.Addr - ImportImage::TextVA] = 0x90;
      if (Mutation == 7)
        writeLE<uint32_t>(F.Image.Segments[0].Data.data() + Call.Addr -
                              ImportImage::TextVA + 2,
                          ImportImage::IATVA + 4);
      if (Mutation == 8)
        F.Image.Segments[0].Flags =
            F.Image.Segments[0].Flags | SegmentFlags::Writable;
      if (Mutation == 9)
        F.Image.Imports[0].Module = "custom.dll";
      if (Mutation == 10)
        F.Image.Imports[0].IsDelayImport = true;
      if (Mutation == 11) {
        F.Image.Imports[0].Name = "DecodePointer";
        F.Image.ImportStorageSlots.at(ImportImage::IATVA).Name =
            "DecodePointer";
      }
      if (Mutation == 12)
        Work = limits::kMaxRegistrationEHStateWork;
      EXPECT_EQ(
          (Heap ? registration_abi::isCheckedRegistrationHeapTerminationCall(
                      F.Image, Call, Work)
                : registration_abi::isCheckedRegistrationStartupInfoCall(
                      F.Image, Call, Work)),
          Mutation == 0);
      EXPECT_LE(Work, limits::kMaxRegistrationEHStateWork);
    }
}

TEST(RegistrationImportABI, ReturningHelperMayReplaceRuntimeSlot) {
  ImportImage F({"GetStartupInfoW", 1}, false);
  auto &Text = F.Image.Segments[0];
  Text.Data = {0xc7, 0x05, 0x00, 0x30, 0x40, 0x00, 0x30,
               0x10, 0x40, 0x00, 0x31, 0xc0, 0xc3};
  Text.Data.resize(0x30, 0xcc);
  Text.Data.push_back(0xc3);
  Text.Size = Text.FileSz = Text.Data.size();
  F.Image.Sections[0].Size = F.Image.Sections[0].FileSz = Text.Size;
  EXPECT_EQ(getCheckedX86CalleeStackPop(F.Image, ImportImage::TextVA),
            std::optional<uint32_t>{0});
  RegistrationCallCalleeIndex Index(F.Image);
  const auto Stacks = Index.stackContracts(F.caller());
  ASSERT_TRUE(Stacks);
  ASSERT_EQ(Stacks->size(), 1u);
  EXPECT_FALSE(Stacks->front().PreservesImportBindings);
  const auto Contracts = Index.contracts(F.caller());
  ASSERT_TRUE(Contracts);
  ASSERT_EQ(Contracts->size(), 1u);
  ASSERT_EQ(Contracts->front().ImageWrites.size(), 1u);
  EXPECT_EQ(Contracts->front().ImageWrites.front().Begin, ImportImage::IATVA);
  EXPECT_EQ(Contracts->front().ImageWrites.front().End, ImportImage::IATVA + 4);
}

TEST(RegistrationImportABI, DivideGuardKeepsOnlyTheNormalReturnStack) {
  for (bool Signed : {false, true})
    for (bool Memory : {false, true})
      for (unsigned Width : {1u, 2u, 4u})
        for (unsigned Mutation = 0; Mutation != 4; ++Mutation) {
          SCOPED_TRACE(::testing::Message() << Signed << "/" << Memory << "/"
                                            << Width << "/" << Mutation);
          ImportImage F({"Sleep", 1}, false);
          auto &Text = F.Image.Segments[0];
          Text.Data.clear();
          if (Mutation == 2)
            Text.Data = {0xc6, 0x05, 0, 0x30, 0x40, 0, 0};
          if (Mutation == 3)
            Text.Data.insert(Text.Data.end(), {0x0f, 0xa2}); // CPUID.
          else {
            if (Width == 2)
              Text.Data.push_back(0x66);
            Text.Data.push_back(Width == 1 ? 0xf6 : 0xf7);
            Text.Data.push_back((Memory ? 0x74 : 0xf1) + (Signed ? 8 : 0));
            if (Memory)
              Text.Data.insert(Text.Data.end(), {0x24, 4}); // [ESP + 4].
          }
          Text.Data.insert(Text.Data.end(),
                           {0x6a, 0, 0xff, 0x15, 0, 0x30, 0x40, 0});
          if (Mutation == 1)
            Text.Data.push_back(0x50); // An unmatched push still rejects.
          Text.Data.insert(Text.Data.end(), {0xc2, 4, 0});
          Text.Size = Text.FileSz = Text.Data.size();
          F.Image.Sections[0].Size = F.Image.Sections[0].FileSz = Text.Size;
          Decoder Decode;
          ASSERT_TRUE(Decode.init(F.Image));
          const auto Low = CFGBuilder().buildIntegerFrameProof(
              F.Image, Decode, Text.VA, "divide-stack");
          ASSERT_TRUE(Low.hasCompleteLiftCoverage());
          unsigned Guards = 0;
          for (const auto &Block : Low.Blocks)
            for (const auto &Op : Block.Ops)
              if (Op.Opcode == NdOp::INTRINSIC && Op.NumInputs &&
                  Op.Inputs[0] ==
                      NdVar::cst(static_cast<uint64_t>(
                                     Intrinsic::X86RequireDivPrecondition),
                                 2)) {
                ++Guards;
                EXPECT_TRUE(intrinsicX86DivPreconditionShapeIsValid(
                    Intrinsic::X86RequireDivPrecondition,
                    x86DivPreconditionLowShape(Op, Arch::X86)));
                EXPECT_FALSE(
                    registration_state::isFramePreservingIntrinsic(Op));
              }
          EXPECT_EQ(Guards, Mutation == 3 ? 0u : 1u);
          EXPECT_EQ(getCheckedX86CalleeStackPop(F.Image, Text.VA),
                    Mutation ? std::nullopt : std::optional<uint32_t>{4});
          RegistrationCallCalleeIndex Index(F.Image);
          const auto Contracts = Index.stackContracts(F.caller());
          ASSERT_TRUE(Contracts);
          ASSERT_EQ(Contracts->size(), Mutation ? 0u : 1u);
          if (!Mutation) {
            EXPECT_EQ(Contracts->front().StackPopBytes, 4u);
            EXPECT_EQ(Contracts->front().RequiredImports,
                      std::vector<va_t>{ImportImage::IATVA});
          }
        }
}
