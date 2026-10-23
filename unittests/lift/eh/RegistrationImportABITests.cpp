//===- RegistrationImportABITests.cpp - PE32 import proof boundaries ------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Keep imported stack cleanup separate from dispatch and memory effects.

#include "../../../lib/ir/low/X86/RegistrationABIPrivate.h"
#include "gtest/gtest.h"

#include "neverd/ir/low/CFGBuilder.h"
#include "neverd/ir/low/RegistrationABI.h"
#include "neverd/lift/X86Regs.h"
#include "neverd/loader/BinaryImage.h"
#include "neverd/support/BinaryEncoding.h"

#include "llvm/ADT/StringRef.h"

#include <cstddef>
#include <cstdint>
#include <initializer_list>
#include <optional>

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
        ImportCase{"malloc", 4}}) {
    SCOPED_TRACE(C.Name);
    const bool IsRaise = llvm::StringRef(C.Name) == "RaiseException";
    const bool IsCRT = llvm::StringRef(C.Name) == "malloc";
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
