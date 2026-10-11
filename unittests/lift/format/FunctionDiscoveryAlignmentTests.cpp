//===- FunctionDiscoveryAlignmentTests.cpp - Candidate address checks -----===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#include "gtest/gtest.h"

#include "neverd/decode/Decoder.h"
#include "neverd/ir/low/FuncDetector.h"
#include "neverd/loader/ExceptionInfo.h"
#include "neverd/loader/FunctionDiscovery.h"
#include "neverd/loader/MachO/MachOLoaderUtils.h"
#include "neverd/support/BinaryEncoding.h"

#include "llvm/ADT/ArrayRef.h"

#include <algorithm>
#include <array>
#include <cstdint>
#include <iterator>
#include <utility>
#include <vector>

using namespace neverd;

namespace {

Segment executableSegment(va_t Address, llvm::ArrayRef<uint8_t> Bytes) {
  Segment Seg;
  Seg.VA = Address;
  Seg.Size = Bytes.size();
  Seg.Flags = SegmentFlags::Readable | SegmentFlags::Executable;
  Seg.Data.assign(Bytes.begin(), Bytes.end());
  return Seg;
}

TEST(FunctionDiscoveryAlignment, UsesVirtualAddressForEveryArchitecture) {
  struct Case {
    Arch Architecture;
    uint64_t Alignment;
    std::array<uint8_t, 4> Prologue;
  };
  const Case Cases[] = {
      {Arch::AArch64, 4, {0xff, 0x83, 0x00, 0xd1}}, // sub sp, sp, #32
      {Arch::ARM, 2, {0x10, 0xb5, 0x70, 0x47}},     // push; bx lr (Thumb)
      {Arch::X86, 1, {0x55, 0x89, 0xe5, 0xc3}},
      {Arch::X64, 1, {0x55, 0x48, 0x89, 0xe5}},
  };
  for (const Case &C : Cases) {
    for (va_t BaseOffset = 0; BaseOffset != 4; ++BaseOffset) {
      for (size_t Off = 0; Off != 8; ++Off) {
        SCOPED_TRACE(::testing::Message()
                     << "arch=" << static_cast<unsigned>(C.Architecture)
                     << " base offset=" << BaseOffset << " offset=" << Off);
        std::vector<uint8_t> Bytes(12, 0);
        std::copy(C.Prologue.begin(), C.Prologue.end(), Bytes.begin() + Off);
        const Segment Seg = executableSegment(0x1000 + BaseOffset, Bytes);
        EXPECT_EQ(checkPrologueAtOffset(Seg, Off, C.Architecture),
                  (Seg.VA + Off) % C.Alignment == 0);
      }
    }
  }
}

TEST(FunctionDiscoveryAlignment, StartsIBTImportThunksAtTheirEndbr) {
  constexpr va_t StubVA = 0xCA90;
  constexpr va_t FreeSlot = 0x31C70, MallocSlot = 0x31C78, ExitSlot = 0x31C80;
  // jmp [rip+disp32] whose opcode is at Offset, to Slot.
  const auto Jump = [&](std::vector<uint8_t> &Bytes, size_t Offset, va_t Slot) {
    const auto Disp = static_cast<int32_t>(Slot - (StubVA + Offset + 6));
    Bytes[Offset] = 0xff;
    Bytes[Offset + 1] = 0x25;
    writeLE<int32_t>(Bytes.data() + Offset + 2, Disp);
  };
  std::vector<uint8_t> Bytes(48, 0x90);
  // endbr64; bnd jmp [rip+free]
  const uint8_t Endbr64[] = {0xf3, 0x0f, 0x1e, 0xfa};
  std::copy(std::begin(Endbr64), std::end(Endbr64), Bytes.begin());
  Bytes[4] = 0xf2;
  Jump(Bytes, 5, FreeSlot);
  // endbr64; jmp [rip+malloc]
  std::copy(std::begin(Endbr64), std::end(Endbr64), Bytes.begin() + 16);
  Jump(Bytes, 20, MallocSlot);
  // mov al, 0xf2; jmp [rip+exit]: the F2 ends the previous instruction.
  Bytes[32] = 0xb0;
  Bytes[33] = 0xf2;
  Jump(Bytes, 34, ExitSlot);

  BinaryImage Img;
  Img.Arch = Arch::X64;
  Img.Bits = Bitness::Bits64;
  Img.Format = BinaryFormat::ELF;
  Img.Segments.push_back(executableSegment(StubVA, Bytes));
  Img.Imports.push_back({"extern", "free", 0, FreeSlot});
  Img.Imports.push_back({"extern", "malloc", 0, MallocSlot});
  Img.Imports.push_back({"extern", "exit", 0, ExitSlot});
  scanImportThunks(Img);

  const struct {
    va_t Entry;
    uint64_t Size;
    const char *Import;
  } Thunks[] = {{StubVA, 11, "free"},
                {StubVA + 16, 10, "malloc"},
                {StubVA + 34, 6, "exit"}};
  for (const auto &Thunk : Thunks) {
    SCOPED_TRACE(Thunk.Import);
    const Import *Resolved = Img.findImportAt(Thunk.Entry);
    ASSERT_NE(Resolved, nullptr);
    EXPECT_EQ(Resolved->Name, Thunk.Import);
    const Symbol *Stub = Img.findSymbolAt(Thunk.Entry);
    ASSERT_NE(Stub, nullptr);
    EXPECT_TRUE(Stub->IsFunc);
    EXPECT_EQ(Stub->Size, Thunk.Size);
  }
  // Nothing is registered inside an instruction.
  for (va_t Inside : {StubVA + 4, StubVA + 5, StubVA + 20, StubVA + 33}) {
    EXPECT_FALSE(Img.ImportStubIndices.count(Inside)) << std::hex << Inside;
    EXPECT_EQ(Img.findSymbolAt(Inside), nullptr) << std::hex << Inside;
  }
}

TEST(FunctionDiscoveryAlignment, ImportTailJumpsDoNotSplitKnownBodies) {
  for (Arch Architecture : {Arch::X86, Arch::X64}) {
    for (bool RawUnwind : {false, true}) {
      SCOPED_TRACE(::testing::Message() << int(Architecture) << RawUnwind);
      constexpr va_t Base = 0x1000, Slot = 0x4000;
      std::vector<uint8_t> Bytes(64, 0xcc);
      // A branch skips an import tail jump to the same function's epilogue.
      const uint8_t Head[] = {0x48, 0x83, 0xec, 0x28, 0x74, 0x07, 0x48};
      std::copy(std::begin(Head), std::end(Head), Bytes.begin());
      const auto Jump = [&](size_t Offset) {
        Bytes[Offset] = 0xff;
        Bytes[Offset + 1] = 0x25;
        writeLE<uint32_t>(Bytes.data() + Offset + 2,
                          Architecture == Arch::X64 ? Slot - (Base + Offset + 6)
                                                    : Slot);
      };
      Jump(7);
      const uint8_t Tail[] = {0x90, 0x48, 0x83, 0xc4, 0x28, 0xc3};
      std::copy(std::begin(Tail), std::end(Tail), Bytes.begin() + 13);
      Jump(32); // A real, separate import thunk still gets discovered.
      BinaryImage Img;
      Img.Base = Base;
      Img.Arch = Architecture;
      Img.Bits = Architecture == Arch::X64 ? Bitness::Bits64 : Bitness::Bits32;
      Img.Format = BinaryFormat::COFF;
      Img.Segments.push_back(executableSegment(Base, Bytes));
      Img.Imports.push_back({"crt", "free", 0, Slot});
      if (RawUnwind)
        Img.COFFPDataRecords.push_back({0, 19, 0});
      else
        Img.Symbols.push_back(Symbol::makeFunc(Base, 19));
      scanImportThunks(Img);
      EXPECT_FALSE(Img.ImportStubIndices.count(Base + 7));
      EXPECT_EQ(Img.findSymbolAt(Base + 7), nullptr);
      ASSERT_TRUE(Img.ImportStubIndices.count(Base + 32));
      ASSERT_NE(Img.findSymbolAt(Base + 32), nullptr);
    }
  }
}

TEST(FunctionDiscoveryAlignment, ArmImportPatternsRespectFunctionBodies) {
  for (Arch Architecture : {Arch::ARM, Arch::AArch64}) {
    for (BinaryFormat Format : {BinaryFormat::ELF, BinaryFormat::COFF}) {
      SCOPED_TRACE(::testing::Message() << int(Architecture) << int(Format));
      constexpr va_t Base = 0x1000, Slot = 0x4000;
      std::vector<uint8_t> Bytes(64, 0);
      const auto Veneer = [&](size_t Offset) {
        if (Architecture == Arch::ARM) {
          writeLE<uint32_t>(Bytes.data() + Offset,
                            0xe51ff004); // ldr pc,[pc,#-4]
          writeLE<uint32_t>(Bytes.data() + Offset + 4, Slot);
        } else {
          writeLE<uint32_t>(Bytes.data() + Offset,
                            0xf0000010); // adrp x16,0x4000
          writeLE<uint32_t>(Bytes.data() + Offset + 4,
                            0xf9400210); // ldr x16,[x16]
          writeLE<uint32_t>(Bytes.data() + Offset + 8, 0xd61f0200); // br x16
        }
      };
      Veneer(4);
      Veneer(32);
      BinaryImage Img;
      Img.Arch = Architecture;
      Img.Mode = Architecture == Arch::ARM ? InstructionMode::ARM
                                           : InstructionMode::Default;
      Img.Format = Format;
      Img.Segments.push_back(executableSegment(Base, Bytes));
      Img.Symbols.push_back(Symbol::makeFunc(Base, 24));
      Img.Imports.push_back({"extern", "free", 0, Slot});
      scanImportThunks(Img);
      EXPECT_FALSE(Img.ImportStubIndices.count(Base + 4));
      EXPECT_EQ(Img.findSymbolAt(Base + 4), nullptr);
      EXPECT_TRUE(Img.ImportStubIndices.count(Base + 32));
    }
  }
}

TEST(FunctionDiscoveryAlignment, PLTUnwindRangesDoNotHideIndividualVeneers) {
  for (Arch Architecture : {Arch::X86, Arch::X64, Arch::ARM, Arch::AArch64})
    for (unsigned Mutation = 0; Mutation != 5; ++Mutation) {
      SCOPED_TRACE(testing::Message() << int(Architecture) << ':' << Mutation);
      constexpr va_t Base = 0x1000, Slot = 0x4000, Veneer = Base + 16;
      std::vector<uint8_t> Bytes(64, 0);
      if (Architecture == Arch::X86 || Architecture == Arch::X64) {
        Bytes[16] = 0xff;
        Bytes[17] = 0x25;
        writeLE<uint32_t>(Bytes.data() + 18, Architecture == Arch::X64
                                                 ? Slot - (Veneer + 6)
                                                 : Slot);
      } else if (Architecture == Arch::ARM) {
        writeLE<uint32_t>(Bytes.data() + 16, 0xe51ff004); // ldr pc,[pc,#-4]
        writeLE<uint32_t>(Bytes.data() + 20, Slot);
      } else {
        writeLE<uint32_t>(Bytes.data() + 16, 0xf0000010); // adrp x16,0x4000
        writeLE<uint32_t>(Bytes.data() + 20, 0xf9400210); // ldr x16,[x16]
        writeLE<uint32_t>(Bytes.data() + 24, 0xd61f0200); // br x16
      }
      BinaryImage Img;
      Img.Arch = Architecture;
      Img.Bits = Architecture == Arch::X64 || Architecture == Arch::AArch64
                     ? Bitness::Bits64
                     : Bitness::Bits32;
      Img.Mode = Architecture == Arch::ARM ? InstructionMode::ARM
                                           : InstructionMode::Default;
      Img.Format = BinaryFormat::ELF;
      Img.Segments.push_back(executableSegment(Base, Bytes));
      Img.Imports.push_back({"extern", "printf", 0, Slot});
      Img.KnownCodeRanges.emplace_back(Base, Base + 64);
      if (Mutation != 2)
        Img.recordImportStubRange(Base, Mutation == 3 ? 20 : 64);
      if (Mutation == 1 || Mutation == 4) {
        auto Function = Symbol::makeFunc(Base, 64);
        if (Mutation == 4) {
          Function.Name = "explicit_function";
          Function.Origin = NameOrigin::Stated;
        }
        Img.Symbols.push_back(std::move(Function));
      }
      scanImportThunks(Img);
      EXPECT_EQ(Img.ImportStubIndices.count(Veneer), Mutation <= 1);
      EXPECT_EQ(Img.findSymbolAt(Veneer) != nullptr, Mutation <= 1);
    }
}

TEST(FunctionDiscoveryAlignment, KnownImportEntriesRemainCallable) {
  const uint8_t Code[] = {0xff, 0x25, 0xfa, 0x2f, 0, 0};
  BinaryImage Img;
  Img.Arch = Arch::X64;
  Img.Bits = Bitness::Bits64;
  Img.Segments.push_back(executableSegment(0x1000, Code));
  Img.Symbols.push_back(Symbol::makeFunc(0x1000, 6));
  Img.Imports.push_back({"crt", "free", 0, 0x4000});
  scanImportThunks(Img);
  EXPECT_TRUE(Img.ImportStubIndices.count(0x1000));
  EXPECT_EQ(Img.Symbols.size(), 1u);
}

TEST(FunctionDiscoveryAlignment, RegistersLoaderRunFunctions) {
  // sub rsp, 8; add rsp, 8; ret; ret; ret: initializers nothing in the image
  // calls, and the entry point.
  const uint8_t Code[] = {0x48, 0x83, 0xec, 0x08, 0x48, 0x83,
                          0xc4, 0x08, 0xc3, 0xc3, 0xc3};
  BinaryImage Img;
  Img.Arch = Arch::X64;
  Img.Bits = Bitness::Bits64;
  Img.Format = BinaryFormat::ELF;
  Img.Segments.push_back(executableSegment(0x1000, Code));
  Img.Symbols.push_back(Symbol::makeFunc(0x1008, 1));
  Img.Entry = 0x100A;
  for (va_t Addr : {0x1000, 0x1008, 0x1009, 0x100A})
    ASSERT_TRUE(Img.recordRuntimeFunction(Addr));
  Img.DynInfo.InitAddr = 0x1000;
  Img.DynInfo.FiniArray = {0x1008, 0x1009};
  registerLoaderRunFunctions(Img);

  for (va_t Addr : {0x1000, 0x1009}) {
    const Symbol *Function = Img.findSymbolAt(Addr);
    ASSERT_NE(Function, nullptr) << std::hex << Addr;
    EXPECT_TRUE(Function->IsFunc);
    EXPECT_EQ(Function->Size, 0u);
  }
  // A function symbol already there is kept as the only one.
  EXPECT_EQ(std::count_if(Img.Symbols.begin(), Img.Symbols.end(),
                          [](const Symbol &Sym) { return Sym.Addr == 0x1008; }),
            1);
  // The entry point is the function detector's, which recovers its extent.
  EXPECT_EQ(Img.findSymbolAt(0x100A), nullptr);
}

TEST(FunctionDiscoveryAlignment, RejectsWrappedAndOutOfBufferAddresses) {
  const std::array<uint8_t, 8> Bytes = {0xff, 0x83, 0x00, 0xd1,
                                        0xff, 0x83, 0x00, 0xd1};
  const Segment Seg = executableSegment(InvalidVA - 3, Bytes);
  EXPECT_TRUE(checkPrologueAtOffset(Seg, 0, Arch::AArch64));
  EXPECT_FALSE(checkPrologueAtOffset(Seg, 4, Arch::AArch64));
  EXPECT_FALSE(checkPrologueAtOffset(Seg, Bytes.size(), Arch::AArch64));
}

TEST(FunctionDiscoveryAlignment,
     PaddingDoesNotCreateEntriesInsideAArch64Instructions) {
  // Actual bytes at hello's 0x1000004e0: str wzr; adrp x8; add x8.
  // The zero run at 4e5/4e6 previously made 4e7's 90 08 01 15 look like B.
  constexpr va_t MainVA = 0x1000004c4;
  const std::array<uint8_t, 12> Instructions = {
      0xff, 0x0f, 0x00, 0xb9, 0x08, 0x00, 0x00, 0x90, 0x08, 0x01, 0x15, 0x91};
  for (BinaryFormat Format :
       {BinaryFormat::MachO, BinaryFormat::ELF, BinaryFormat::COFF}) {
    SCOPED_TRACE(static_cast<unsigned>(Format));
    BinaryImage Img;
    Img.Arch = Arch::AArch64;
    Img.Bits = Bitness::Bits64;
    Img.Format = Format;
    std::vector<uint8_t> Bytes(0x40, 0xff);
    std::copy(Instructions.begin(), Instructions.end(), Bytes.begin() + 0x1c);
    Img.Segments.push_back(executableSegment(MainVA, Bytes));
    Img.Symbols.push_back(Symbol::makeFunc(MainVA));

    scanPaddingBoundaries(Img);

    ASSERT_EQ(Img.Symbols.size(), 1u);
    EXPECT_EQ(Img.Symbols.front().Addr, MainVA);
  }
}

TEST(FunctionDiscoveryAlignment, PreservesAlignedEntriesAfterPadding) {
  struct Case {
    Arch Architecture;
    size_t Offset;
    std::array<uint8_t, 4> Prologue;
  };
  const Case Cases[] = {
      {Arch::AArch64, 8, {0xff, 0x83, 0x00, 0xd1}},
      {Arch::ARM, 6, {0x10, 0xb5, 0x70, 0x47}},
      {Arch::X86, 5, {0x55, 0x89, 0xe5, 0xc3}},
      {Arch::X64, 5, {0x55, 0x48, 0x89, 0xe5}},
  };
  for (const Case &C : Cases) {
    SCOPED_TRACE(static_cast<unsigned>(C.Architecture));
    BinaryImage Img;
    Img.Arch = C.Architecture;
    std::vector<uint8_t> Bytes(C.Offset + 4, codePaddingByte(Img.Arch));
    // The scanner deliberately skips a segment's initial padding run.
    Bytes[0] = 0x12;
    std::copy(C.Prologue.begin(), C.Prologue.end(), Bytes.begin() + C.Offset);
    Img.Segments.push_back(executableSegment(0x1000, Bytes));

    scanPaddingBoundaries(Img);

    ASSERT_EQ(Img.Symbols.size(), 1u);
    EXPECT_EQ(Img.Symbols.front().Addr, 0x1000 + C.Offset);
  }
}

TEST(FunctionDiscoveryAlignment, DataPointersRejectMisalignedAArch64Entries) {
  BinaryImage Img;
  Img.Arch = Arch::AArch64;
  Img.Bits = Bitness::Bits64;
  std::vector<uint8_t> Bytes(16, 0xff);
  writeLE<uint32_t>(Bytes.data() + 1, 0xd10083ffu);
  writeLE<uint32_t>(Bytes.data() + 8, 0xd10083ffu);
  Img.Segments.push_back(executableSegment(0x1000, Bytes));
  Segment Data;
  Data.VA = 0x2000;
  Data.Size = 16;
  Data.Flags = SegmentFlags::Readable;
  Data.Data.resize(16);
  writeLE<uint64_t>(Data.Data.data(), 0x1001);
  writeLE<uint64_t>(Data.Data.data() + 8, 0x1008);
  Img.Segments.push_back(std::move(Data));

  scanDataFuncPointers(Img);

  ASSERT_EQ(Img.Symbols.size(), 1u);
  EXPECT_EQ(Img.Symbols.front().Addr, 0x1008u);
}

TEST(FunctionDiscoveryAlignment, DebugCodeReferencesAreNotFunctionEntries) {
  for (BinaryFormat Format :
       {BinaryFormat::COFF, BinaryFormat::ELF, BinaryFormat::MachO}) {
    for (Arch Target : {Arch::X86, Arch::X64, Arch::ARM, Arch::AArch64}) {
      for (const char *Name : {".debug_info", ".debug$S", ".zdebug_info",
                               "__debug_info", ".stab", ".rdata"}) {
        for (bool Sections : {false, true}) {
          SCOPED_TRACE(Name);
          SCOPED_TRACE(static_cast<unsigned>(Target));
          SCOPED_TRACE(Sections);
          SCOPED_TRACE(static_cast<unsigned>(Format));
          BinaryImage Img;
          Img.Arch = Target;
          Img.Bits = Target == Arch::X86 || Target == Arch::ARM
                         ? Bitness::Bits32
                         : Bitness::Bits64;
          Img.Format = Format;
          if (Target == Arch::ARM)
            Img.Mode = InstructionMode::Thumb;
          // MOV at a call-return PC passes the x86 prologue heuristic. AArch64
          // uses a plausible SP adjustment for the same false candidate.
          std::vector<uint8_t> Code(16, 0x90);
          if (Target == Arch::AArch64)
            writeLE<uint32_t>(Code.data(), 0xd10083ffu);
          else if (Target == Arch::ARM)
            Code = {0x10, 0xb5, 0x70, 0x47};
          else {
            Code[0] = 0x8b;
            Code[1] = 0xc1;
            Code[2] = 0xc3;
          }
          Img.Segments.push_back(executableSegment(0x1000, Code));
          Segment Data;
          Data.Name = Name;
          Data.VA = 0x2000;
          Data.Size = 8;
          Data.Flags = SegmentFlags::Readable;
          Data.Data.resize(8);
          writeLE<uint64_t>(Data.Data.data(),
                            Target == Arch::ARM ? 0x1001 : 0x1000);
          Img.Segments.push_back(Data);
          if (Sections) {
            Section Sec;
            Sec.Name = Name;
            Sec.VA = Data.VA;
            Sec.Size = Data.Size;
            Sec.Flags = Data.Flags;
            Img.Sections.push_back(Sec);
          }
          scanDataFuncPointers(Img);
          EXPECT_EQ(Img.Symbols.size(),
                    std::string(Name) == ".rdata" ? 1u : 0u);
        }
      }
    }
  }
}

TEST(FunctionDiscoveryAlignment,
     PreservesTaggedThumbPointersAndOddDataSymbols) {
  BinaryImage Img;
  Img.Arch = Arch::ARM;
  Img.Mode = InstructionMode::Thumb;
  Img.Bits = Bitness::Bits32;
  const std::array<uint8_t, 4> Code = {0x10, 0xb5, 0x70, 0x47};
  Img.Segments.push_back(executableSegment(0x1002, Code));
  Segment Data;
  Data.VA = 0x2000;
  Data.Size = 4;
  Data.Flags = SegmentFlags::Readable;
  Data.Data.resize(4);
  writeLE<uint32_t>(Data.Data.data(), 0x1003);
  Img.Segments.push_back(std::move(Data));
  Symbol Label;
  Label.Name = "odd_data";
  Label.Addr = 0x2001;
  Img.Symbols.push_back(Label);

  scanDataFuncPointers(Img);

  ASSERT_EQ(Img.Symbols.size(), 2u);
  EXPECT_EQ(Img.Symbols[0].Addr, 0x2001u);
  EXPECT_EQ(Img.Symbols[0].Name, "odd_data");
  EXPECT_FALSE(Img.Symbols[0].IsFunc);
  EXPECT_EQ(Img.Symbols[1].Addr, 0x1002u);
  EXPECT_TRUE(Img.Symbols[1].IsFunc);
}

TEST(FunctionDiscoveryAlignment,
     DataPrologueCandidatesKeepExactOwnershipAndExistingSymbols) {
  for (BinaryFormat Format :
       {BinaryFormat::MachO, BinaryFormat::ELF, BinaryFormat::COFF}) {
    SCOPED_TRACE(static_cast<unsigned>(Format));
    BinaryImage Img;
    Img.Arch = Arch::AArch64;
    Img.Bits = Bitness::Bits64;
    Img.Format = Format;
    std::vector<uint8_t> Bytes(0x60, 0);
    for (size_t Offset : {4u, 8u, 12u, 16u, 32u})
      writeLE<uint32_t>(Bytes.data() + Offset, 0xd10083ffu);
    Img.Segments.push_back(executableSegment(0x1000, Bytes));

    Section Code;
    Code.VA = 0x1000;
    Code.Size = 14;
    Code.Flags = SegmentFlags::Readable | SegmentFlags::Executable;
    Code.Type = llvm::MachO::S_ATTR_PURE_INSTRUCTIONS;
    Img.Sections.push_back(Code);
    Section Constants;
    Constants.VA = Code.VA + Code.Size;
    Constants.Size = Bytes.size() - Code.Size;
    Constants.Flags = SegmentFlags::Readable;
    Img.Sections.push_back(Constants);

    Symbol Existing;
    Existing.Name = "existing_data_label";
    Existing.Addr = 0x1004;
    Img.Symbols.push_back(Existing);
    Img.KnownCodeRanges.push_back({0x1010, 0x1014});
    // Includes a duplicate, an instruction crossing a code/data boundary,
    // a claimed body, non-code prologue bytes, and an unmapped target.
    const uint64_t Targets[] = {0x1004, 0x1008, 0x1008, 0x100c,
                                0x1010, 0x1020, 0x2000};
    Segment Data;
    Data.VA = 0x3000;
    Data.Size = sizeof(Targets);
    Data.Flags = SegmentFlags::Readable;
    Data.Data.resize(sizeof(Targets));
    for (size_t I = 0; I < std::size(Targets); ++I)
      writeLE<uint64_t>(Data.Data.data() + I * sizeof(uint64_t), Targets[I]);
    Img.Segments.push_back(std::move(Data));

    scanDataFuncPointers(Img);

    ASSERT_EQ(Img.Symbols.size(), 2u);
    EXPECT_EQ(Img.Symbols[0].Addr, Existing.Addr);
    EXPECT_EQ(Img.Symbols[0].Name, Existing.Name);
    EXPECT_FALSE(Img.Symbols[0].IsFunc);
    EXPECT_EQ(Img.Symbols[1].Addr, 0x1008u);
    EXPECT_TRUE(Img.Symbols[1].IsFunc);
    scanDataFuncPointers(Img);
    EXPECT_EQ(Img.Symbols.size(), 2u);
  }
}

TEST(FunctionDiscoveryAlignment,
     MachOFunctionStartsSuppressRawMetadataPointerGuessing) {
  BinaryImage Img;
  Img.Arch = Arch::AArch64;
  Img.Bits = Bitness::Bits64;
  Img.Format = BinaryFormat::MachO;
  Img.MachOHasFunctionStarts = true;

  std::vector<uint8_t> Bytes(16, 0);
  writeLE<uint32_t>(Bytes.data(), 0xd10083ffu);
  writeLE<uint32_t>(Bytes.data() + 8, 0xd10083ffu);
  Img.Segments.push_back(executableSegment(0x1000, Bytes));
  Img.Symbols.push_back(Symbol::makeFunc(0x1000));

  // Models two adjacent 32-bit relative metadata fields which, when read as
  // one untyped 64-bit word, happen to spell an interior code address.
  Segment Metadata;
  Metadata.VA = 0x2000;
  Metadata.Size = 8;
  Metadata.Flags = SegmentFlags::Readable;
  Metadata.Data.resize(8);
  writeLE<uint64_t>(Metadata.Data.data(), 0x1008);
  Img.Segments.push_back(std::move(Metadata));

  scanDataFuncPointers(Img);

  ASSERT_EQ(Img.Symbols.size(), 1u);
  EXPECT_EQ(Img.Symbols.front().Addr, 0x1000u);
}

TEST(FunctionDiscoveryAlignment,
     MachOFunctionStartsAcceptZeroBasedLinkedImages) {
  BinaryImage Img;
  Img.Arch = Arch::AArch64;
  Img.Bits = Bitness::Bits64;
  Img.Format = BinaryFormat::MachO;
  Img.Segments.push_back(executableSegment(0, std::vector<uint8_t>(32)));

  // LC_FUNCTION_STARTS deltas are relative to the Mach header VA. A linked
  // dylib may map that header at VA zero; zero is an address here, not a
  // missing-base sentinel.
  std::vector<uint8_t> File(16, 0);
  File[8] = 4;
  File[9] = 0;
  macho_loader::FunctionStartsInfo Info;
  Info.DataOff = 8;
  Info.DataSize = 2;

  macho_loader::parseFunctionStarts(File.data(), File.size(), Info, 0, Img);

  ASSERT_TRUE(Img.MachOHasFunctionStarts);
  ASSERT_EQ(Img.Symbols.size(), 1u);
  EXPECT_EQ(Img.Symbols.front().Addr, 4u);
  EXPECT_TRUE(Img.Symbols.front().IsFunc);
}

TEST(FunctionDiscoveryAlignment, CoffFuncLoadSkipsPaddingAndDataScans) {
  std::vector<uint8_t> Text(16, 0xcc);
  Text[8] = 0x55;
  Text[9] = 0x48;
  Text[10] = 0x89;
  Text[11] = 0xe5;
  Text[12] = 0xc3;

  BinaryImage WithPdata;
  WithPdata.Arch = Arch::X64;
  WithPdata.Bits = Bitness::Bits64;
  WithPdata.Format = BinaryFormat::COFF;
  WithPdata.Segments.push_back(executableSegment(0x1000, Text));
  WithPdata.KnownCodeRanges.push_back({0x1000, 0x1008});
  WithPdata.LoadOnlyFunctionEntries.insert(0x1000);
  ExceptionFunction Owned;
  Owned.CodeRange = ExceptionAddressRange{0x1000, 0x1008};
  WithPdata.ExceptionMetadata.Functions.push_back(Owned);
  WithPdata.Symbols.push_back(Symbol::makeFunc(0x1000, 8));
  const size_t Before = WithPdata.Symbols.size();
  runPostLoadDiscovery(WithPdata, "coff-pdata-func");
  EXPECT_EQ(WithPdata.Symbols.size(), Before)
      << "--func pdata load must not walk padding or data pointers";
}

TEST(FunctionDiscoveryAlignment, CoffFullLoadStillScansDataFuncPointers) {
  std::vector<uint8_t> Text(16, 0xcc);
  Text[0] = 0x55;
  Text[1] = 0x48;
  Text[2] = 0x89;
  Text[3] = 0xe5;
  Text[4] = 0xc3;
  Text[8] = 0x55;
  Text[9] = 0x48;
  Text[10] = 0x89;
  Text[11] = 0xe5;
  Text[12] = 0xc3;

  BinaryImage WithPdata;
  WithPdata.Arch = Arch::X64;
  WithPdata.Bits = Bitness::Bits64;
  WithPdata.Format = BinaryFormat::COFF;
  WithPdata.Segments.push_back(executableSegment(0x1000, Text));
  WithPdata.KnownCodeRanges.push_back({0x1000, 0x1008});
  ExceptionFunction Owned;
  Owned.CodeRange = ExceptionAddressRange{0x1000, 0x1008};
  WithPdata.ExceptionMetadata.Functions.push_back(Owned);
  WithPdata.Symbols.push_back(Symbol::makeFunc(0x1000, 8));

  Segment Data;
  Data.VA = 0x2000;
  Data.Size = 8;
  Data.Flags = SegmentFlags::Readable;
  Data.Data.resize(8);
  writeLE<uint64_t>(Data.Data.data(), 0x1008);
  WithPdata.Segments.push_back(std::move(Data));

  runPostLoadDiscovery(WithPdata, "coff-pdata-full");
  EXPECT_NE(std::find_if(WithPdata.Symbols.begin(), WithPdata.Symbols.end(),
                         [](const Symbol &Sym) {
                           return Sym.IsFunc && Sym.Addr == 0x1008;
                         }),
            WithPdata.Symbols.end())
      << "full-image PE still discovers data-pointer callees outside pdata";
}

TEST(FunctionDiscoveryAlignment, FuncLoadNamesImportThunkWithoutScan) {
  // `--func` skips scanImportThunks.  A call to an IAT veneer must still
  // resolve `_CxxThrowException` so HighC can print `throw`.
  constexpr va_t Thunk = 0x140001020;
  constexpr va_t IAT = 0x140003000;
  const int32_t Disp = static_cast<int32_t>(IAT - (Thunk + 6));
  std::vector<uint8_t> Text(6, 0xcc);
  Text[0] = 0xff;
  Text[1] = 0x25;
  Text[2] = static_cast<uint8_t>(Disp);
  Text[3] = static_cast<uint8_t>(Disp >> 8);
  Text[4] = static_cast<uint8_t>(Disp >> 16);
  Text[5] = static_cast<uint8_t>(Disp >> 24);

  BinaryImage Img;
  Img.Arch = Arch::X64;
  Img.Bits = Bitness::Bits64;
  Img.Format = BinaryFormat::COFF;
  Img.LoadOnlyFunctionEntries.insert(0x140001000);
  Img.Segments.push_back(executableSegment(Thunk, Text));
  Import Imp;
  Imp.Name = "_CxxThrowException";
  Imp.IATAddr = IAT;
  Img.Imports.push_back(std::move(Imp));

  runPostLoadDiscovery(Img, "coff-func-thunk");
  EXPECT_TRUE(Img.ImportStubIndices.empty())
      << "--func must not walk every executable byte for IAT veneers";
  const Import *Named = Img.findImportAt(Thunk);
  ASSERT_NE(Named, nullptr);
  EXPECT_EQ(Named->Name, "_CxxThrowException");
}

} // namespace

TEST(FunctionDiscoveryAlignment, FDEExtentOwnsInteriorRelocationLabels) {
  for (BinaryFormat Format :
       {BinaryFormat::COFF, BinaryFormat::ELF, BinaryFormat::MachO}) {
    for (Arch Architecture : {Arch::X86, Arch::X64, Arch::ARM, Arch::AArch64}) {
      for (unsigned Mutation = 0; Mutation < 4; ++Mutation) {
        SCOPED_TRACE(static_cast<unsigned>(Format));
        SCOPED_TRACE(static_cast<unsigned>(Architecture));
        SCOPED_TRACE(Mutation);
        constexpr va_t Entry = 0x401000, Interior = Entry + 0x20;
        BinaryImage Img;
        Img.Arch = Architecture;
        const unsigned Width =
            Architecture == Arch::X86 || Architecture == Arch::ARM ? 4 : 8;
        Img.Bits = Width == 4 ? Bitness::Bits32 : Bitness::Bits64;
        Img.Format = Format;
        if (Architecture == Arch::ARM)
          Img.Mode = InstructionMode::Thumb;
        Img.Base = 0x400000;
        Img.Entry = Entry;
        std::vector<uint8_t> Bytes(0x50, 0x90);
        Bytes[0] = 0x55;
        Bytes[1] = 0x89;
        Bytes[2] = 0xe5;
        Bytes[0x20] = 0xc3;
        Bytes[0x40] = 0xc3;
        if (Architecture == Arch::AArch64) {
          for (unsigned I = 0; I < Bytes.size(); I += 4)
            writeLE<uint32_t>(Bytes.data() + I, 0xd503201fu); // nop
          writeLE<uint32_t>(Bytes.data(), 0xd10083ffu);       // sub sp, sp, #32
          for (unsigned I : {0x20u, 0x40u})
            writeLE<uint32_t>(Bytes.data() + I, 0xd65f03c0u); // ret
        } else if (Architecture == Arch::ARM) {
          for (unsigned I = 0; I < Bytes.size(); I += 2)
            writeLE<uint16_t>(Bytes.data() + I, 0xbf00u); // nop
          writeLE<uint16_t>(Bytes.data(), 0xb510u);       // push {r4, lr}
          for (unsigned I : {0x20u, 0x40u})
            writeLE<uint16_t>(Bytes.data() + I, 0x4770u); // bx lr
        } else if (Architecture == Arch::X64) {
          Bytes[1] = 0x48;
          Bytes[2] = 0x89;
          Bytes[3] = 0xe5;
        }
        Img.Segments.push_back(executableSegment(Entry, Bytes));
        Segment Table;
        Table.Name = ".rdata";
        Table.VA = 0x402000;
        Table.Size = Width * 2;
        Table.Flags = SegmentFlags::Readable;
        Table.Data.resize(Table.Size);
        const unsigned Tag = Architecture == Arch::ARM ? 1 : 0;
        if (Width == 4) {
          writeLE<uint32_t>(Table.Data.data(), Interior | Tag);
          writeLE<uint32_t>(Table.Data.data() + Width, (Entry + 0x40) | Tag);
        } else {
          writeLE<uint64_t>(Table.Data.data(), Interior);
          writeLE<uint64_t>(Table.Data.data() + Width, Entry + 0x40);
        }
        Img.CodePtrRelocSlots = {Table.VA, Table.VA + Width};
        Img.Segments.push_back(Table);
        Img.Symbols.push_back(Symbol::makeFunc(Entry)); // COFF has no size.
        ExceptionFunction EH;
        EH.Encoding = ExceptionEncoding::DwarfFDE;
        EH.CodeRange = {Entry, Entry + 0x30};
        EH.Dwarf.emplace();
        if (Mutation == 1)
          EH.ParseStatus = ExceptionParseStatus::Malformed;
        if (Mutation == 2)
          EH.Dwarf.reset();
        if (Mutation == 3)
          Img.Symbols.push_back(Symbol::makeFunc(Interior));
        Img.ExceptionMetadata.Functions.push_back(EH);
        // Linked i386 PE deliberately probes pointers inside coarse CRT
        // ranges. Other targets already reject those weaker candidates, so
        // leave coarse coverage absent there to test the FDE's own authority.
        if (Format == BinaryFormat::COFF && Architecture == Arch::X86)
          Img.KnownCodeRanges = {{Entry, Entry + 0x30}};
        Decoder Dec;
        ASSERT_TRUE(Dec.init(Img));
        auto Found = FuncDetector().detect(Img, Dec);
        auto Has = [&](va_t A) {
          return std::any_of(Found.begin(), Found.end(),
                             [&](const auto &F) { return F.first == A; });
        };
        EXPECT_EQ(Has(Interior), Mutation != 0);
        EXPECT_TRUE(Has(Entry));
        EXPECT_TRUE(Has(Entry + 0x40));
      }
    }
  }
}
