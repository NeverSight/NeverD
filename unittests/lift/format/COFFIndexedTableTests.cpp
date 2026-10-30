//===- COFFIndexedTableTests.cpp - Pointer tables among PE code ----------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Pins table storage, callable entries and relocated address arithmetic.
//===----------------------------------------------------------------------===//

#include "../../loader/PEHeaderFixture.h"
#include "gtest/gtest.h"

#include "neverd/backend/llvm/MedLLVMEmitter.h"
#include "neverd/ir/TargetRegInfo.h"
#include "neverd/ir/low/FuncDetector.h"
#include "neverd/loader/COFF/COFFLoader.h"
#include "neverd/loader/COFF/COFFLoaderUtils.h"
#include "neverd/loader/PointerRelocation.h"
#include "neverd/support/BinaryEncoding.h"

#include "llvm/BinaryFormat/MachO.h"
#include "llvm/IR/Constants.h"
#include "llvm/IR/Instructions.h"
#include "llvm/IR/Verifier.h"
#include "llvm/Support/Error.h"

#include <algorithm>
#include <cstdint>
#include <iterator>
#include <set>
#include <string>

using namespace neverd;

namespace {

BinaryImage tableImage(Arch A = Arch::X86,
                       BinaryFormat Format = BinaryFormat::COFF) {
  BinaryImage Img;
  Img.Arch = A;
  Img.Format = Format;
  Img.Bits = A == Arch::X86 ? Bitness::Bits32 : Bitness::Bits64;
  Img.Entry = 0x1000;
  const bool IsMachO = Format == BinaryFormat::MachO;
  for (bool Code : {true, false}) {
    Segment S;
    S.Name = IsMachO ? (Code ? "__TEXT" : "__DATA_CONST")
                     : (Code ? ".text" : ".rdata");
    S.VA = Code ? 0x1000 : 0x2000;
    S.Size = S.FileSz = 0x200;
    S.Flags = Code ? SegmentFlags::Readable | SegmentFlags::Executable
                   : SegmentFlags::Readable;
    S.Data.assign(S.Size, Code ? 0xcc : 0);
    Section Sec;
    Sec.Name = IsMachO ? (Code ? "__text" : "__const") : S.Name;
    Sec.SegmentName = S.Name;
    if (IsMachO && Code)
      Sec.Type = llvm::MachO::S_ATTR_PURE_INSTRUCTIONS;
    Sec.VA = S.VA;
    Sec.Size = Sec.FileSz = S.Size;
    Sec.Flags = S.Flags;
    Img.Sections.push_back(Sec);
    Img.Segments.push_back(S);
  }
  return Img;
}

void pointer(BinaryImage &Img, va_t Slot, va_t Target) {
  auto *Bytes = Img.Segments[Slot >= 0x2000].Data.data() + (Slot & 0xfff);
  if (Img.is64Bit())
    writeLE<uint64_t>(Bytes, Target);
  else
    writeLE<uint32_t>(Bytes, Target);
  recordAbsolutePointerRelocations(Img, {{Slot, Target, InvalidVA}});
}

void access(BinaryImage &Img, uint8_t Opcode = 0xff, uint8_t ModRM = 0x14,
            bool Segmented = false, va_t At = 0x1000, va_t Base = 0x1100) {
  auto *Code = Img.Segments[0].Data.data() + At - 0x1000;
  if (Segmented)
    *Code++ = 0x64; // fs: is not an image-relative table.
  *Code++ = Opcode;
  *Code++ = ModRM;
  *Code++ = Img.is64Bit() ? 0xc5 : 0x85; // [rax/eax * pointer size + disp32]
  writeLE<uint32_t>(Code, Base);
  Code[4] = 0xc3;
}

MedVar temporary(Arch A, unsigned Id) {
  MedVar V;
  V.Kind = MedVar::Temp;
  V.TheArch = A;
  V.Id = Id;
  V.SSAVer = 1;
  V.Size = A == Arch::X86 ? 4 : 8;
  return V;
}

MedFunc addressSource(const BinaryImage &Img, va_t Base, bool Indexed) {
  MedFunc F;
  F.Entry = Img.Entry;
  F.Name = "table_address";
  const unsigned Width = Img.getPointerSize();
  F.ReturnType = NdType::makeInt(Width);
  F.Blocks.emplace_back();
  auto &B = F.Blocks.back();
  B.Id = 0;
  B.StartAddr = F.Entry;
  B.EndAddr = F.Entry + 16;
  MedVar Value =
      MedVar::makeConst(Base, Width,
                        Base < 0x2000 ? ConstantAddressProvenance::CodeAddress
                                      : ConstantAddressProvenance::DataAddress);
  if (Indexed) {
    auto Index = temporary(Img.Arch, 0);
    Index.Kind = MedVar::Param;
    F.Params.push_back(Index);
    MedOp Add;
    Add.Opcode = NdOp::INT_ADD;
    Add.Addr = F.Entry;
    Add.Output = temporary(Img.Arch, 1);
    Add.addInput(Value);
    Add.addInput(Index);
    B.Ops.push_back(Add);
    Value = Add.Output;
  }
  MedOp Copy;
  Copy.Opcode = NdOp::COPY;
  Copy.Addr = F.Entry + 4;
  Copy.Output = temporary(Img.Arch, 2);
  Copy.Output.Kind = MedVar::Reg;
  Copy.Output.RegOff = getTargetRegInfo(Img.Arch).IntReturnReg;
  Copy.addInput(Value);
  B.Ops.push_back(Copy);
  MedOp Return;
  Return.Opcode = NdOp::RETURN;
  Return.Addr = F.Entry + 8;
  Return.addInput(Copy.Output);
  B.Ops.push_back(Return);
  return F;
}

bool references(const llvm::Value *V, const llvm::Value *Target,
                std::set<const llvm::Value *> &Seen) {
  if (V == Target)
    return true;
  if (!Seen.insert(V).second)
    return false;
  if (const auto *U = llvm::dyn_cast<llvm::User>(V))
    for (const auto &Operand : U->operands())
      if (references(Operand, Target, Seen))
        return true;
  return false;
}

} // namespace

TEST(COFFIndexedTables, ShortCallTableKeepsAdjacentInstructionRelocations) {
  for (Arch A : {Arch::X86, Arch::X64}) {
    auto Img = tableImage(A);
    access(Img);
    pointer(Img, 0x1100, 0x1080);
    pointer(Img, 0x1100 + Img.getPointerSize(), 0x1090);
    pointer(Img, 0x1112, 0x1090); // An unrelated MOV immediate after a gap.
    coff_loader::recordIndexedTableBasesInCode(Img);
    EXPECT_TRUE(Img.CodePtrRelocSlots.count(0x1100));
    EXPECT_TRUE(Img.CodePtrRelocSlots.count(0x1100 + Img.getPointerSize()));
    EXPECT_TRUE(Img.CodeAddressRelocOperands.count(0x1112));
    EXPECT_FALSE(Img.CodePtrRelocSlots.count(0x1112));
  }
}

TEST(COFFIndexedTables, LongCallTableKeepsEveryContiguousSlot) {
  auto Img = tableImage();
  access(Img);
  for (unsigned I = 0; I != 12; ++I)
    pointer(Img, 0x1100 + 4 * I, 0x1080);
  coff_loader::recordIndexedTableBasesInCode(Img);
  EXPECT_EQ(Img.CodePtrRelocSlots.size(), 12u);
}

TEST(COFFIndexedTables, ConstructorSentinelDoesNotBecomeACallableSlot) {
  for (uint32_t Sentinel : {0u, 0xffffffffu}) {
    auto Img = tableImage();
    access(Img);
    writeLE<uint32_t>(Img.Segments[0].Data.data() + 0x100, Sentinel);
    pointer(Img, 0x1104, 0x1080);
    pointer(Img, 0x1108, 0x1090);
    coff_loader::recordIndexedTableBasesInCode(Img);
    EXPECT_EQ(Img.CodePtrRelocSlots, (std::set<va_t>{0x1104, 0x1108}));
    EXPECT_TRUE(Img.DataInCode.count(0x1100));
  }
}

TEST(COFFIndexedTables, LinkedPELoaderAndDiscoveryRetainBothCallbacks) {
  using namespace llvm::COFF;
  auto Fixture = test::peImageBaseWalker(IMAGE_FILE_MACHINE_I386, false);
  auto *Bytes = reinterpret_cast<uint8_t *>(Fixture.Bytes.data());
  auto PE = locatePEHeaders(Bytes, Fixture.Bytes.size());
  auto *Text = reinterpret_cast<llvm::object::coff_section *>(PE.SectionTable);
  Text->VirtualSize = 0x200;
  uint8_t *Code = Bytes + Text->PointerToRawData;
  std::fill_n(Code, 0x200, 0xcc);
  // and eax,1; call [eax*4 + table]; ret
  const uint8_t Entry[] = {0x83, 0xe0, 0x01, 0xff, 0x14, 0x85};
  std::copy(std::begin(Entry), std::end(Entry), Code);
  writeLE<uint32_t>(Code + 6, Fixture.Entry + 0x100);
  Code[10] = 0xc3;
  Code[0x80] = Code[0x90] = 0xc3;
  writeLE<uint32_t>(Code + 0x100, Fixture.Entry + 0x80);
  writeLE<uint32_t>(Code + 0x104, Fixture.Entry + 0x90);
  const uint16_t Offsets[] = {6, 0x100, 0x104};
  for (unsigned I = 0; I != std::size(Offsets); ++I)
    writeLE<uint16_t>(Bytes + 0x408 + 2 * I,
                      (IMAGE_REL_BASED_HIGHLOW << 12) | Offsets[I]);
  auto Img = COFFLoader().loadBuffer(
      llvm::MemoryBufferRef(Fixture.Bytes, "indexed-callbacks.exe"));
  ASSERT_TRUE(bool(Img)) << llvm::toString(Img.takeError());
  for (unsigned Offset : {0x100, 0x104})
    ASSERT_TRUE(Img->CodePtrRelocSlots.count(Fixture.Entry + Offset));
  Decoder Dec;
  ASSERT_TRUE(Dec.init(*Img));
  const auto Entries = FuncDetector().detect(*Img, Dec);
  for (unsigned Offset : {0x80, 0x90}) {
    const va_t Callback = Fixture.Entry + Offset;
    EXPECT_TRUE(std::any_of(Entries.begin(), Entries.end(), [&](const auto &E) {
      return E.first == Callback;
    }));
    EXPECT_TRUE(Img->VerifiedFunctionEntries.count(Callback));
  }
}

TEST(COFFIndexedTables, AddressCalculationAndSegmentedAccessDoNotProveStorage) {
  for (unsigned Kind = 0; Kind != 4; ++Kind) {
    SCOPED_TRACE(Kind);
    auto Img = tableImage();
    if (Kind == 0)
      access(Img, 0x8d, 0x04); // lea eax, [eax*4 + base]
    else if (Kind == 1)
      access(Img, 0xff, 0x14, true);
    else if (Kind == 2)
      access(Img, 0x8a, 0x04); // A byte read proves no pointer-wide table.
    else
      access(Img, 0x89, 0x04); // A write is not a pointer table read.
    pointer(Img, 0x1100, 0x1080);
    coff_loader::recordIndexedTableBasesInCode(Img);
    EXPECT_TRUE(Img.CodeAddressRelocOperands.count(0x1100));
    EXPECT_FALSE(Img.DataInCode.count(0x1100));
  }
}

TEST(COFFIndexedTables, UnreachableInstructionShapedBytesDoNotProveStorage) {
  auto Img = tableImage();
  Img.Segments[0].Data[0] = 0xc3;
  access(Img, 0xff, 0x14, false, 0x1020);
  pointer(Img, 0x1100, 0x1080);
  coff_loader::recordIndexedTableBasesInCode(Img);
  EXPECT_TRUE(Img.CodeAddressRelocOperands.count(0x1100));
  EXPECT_FALSE(Img.DataInCode.count(0x1100));
}

TEST(COFFIndexedTables, ReachedInstructionOperandCannotBecomeATableSlot) {
  auto Img = tableImage();
  access(Img);
  pointer(Img, 0x1100, 0x1080);
  Img.Segments[0].Data[0xff] = 0xb8; // mov eax, pointer; ret
  Img.Segments[0].Data[0x104] = 0xc3;
  Symbol Owner;
  Owner.Name = "operand_owner";
  Owner.Addr = 0x10ff;
  Owner.Size = 6;
  Owner.IsFunc = true;
  Img.Symbols.push_back(Owner);
  coff_loader::recordIndexedTableBasesInCode(Img);
  EXPECT_TRUE(Img.CodeAddressRelocOperands.count(0x1100));
  EXPECT_FALSE(Img.CodePtrRelocSlots.count(0x1100));
  EXPECT_FALSE(Img.DataInCode.count(0x1100));
}

TEST(COFFIndexedTables, ExistingPointerRunStillSuppliesCallableEvidence) {
  auto Img = tableImage();
  access(Img);
  for (unsigned I = 0; I != 12; ++I) {
    pointer(Img, 0x1100 + 4 * I, 0x1080);
  }
  Img.Segments[0].Data[0x80] = 0xc3;
  coff_loader::recordRelocatedPointerRunsInCode(Img);
  ASSERT_TRUE(Img.CodeAddressRelocOperands.empty());
  coff_loader::recordIndexedTableBasesInCode(Img);
  Decoder Dec;
  ASSERT_TRUE(Dec.init(Img));
  const auto Entries = FuncDetector().detect(Img, Dec);
  EXPECT_TRUE(std::any_of(Entries.begin(), Entries.end(),
                          [](const auto &E) { return E.first == 0x1080; }));
  EXPECT_TRUE(Img.VerifiedFunctionEntries.count(0x1080));
}

TEST(COFFIndexedTables, UnreadDataCallbackIsNotAnEntryFunctionsLabel) {
  auto Img = tableImage();
  Img.Segments[0].Data[0] = 0xc3;
  Img.Segments[0].Data[0x80] = 0xc3;
  pointer(Img, 0x2000, 0x1080);
  Decoder Dec;
  ASSERT_TRUE(Dec.init(Img));
  const auto Entries = FuncDetector().detect(Img, Dec);
  EXPECT_TRUE(std::any_of(Entries.begin(), Entries.end(),
                          [](const auto &E) { return E.first == 0x1080; }));
  EXPECT_TRUE(Img.VerifiedFunctionEntries.count(0x1080));
}

TEST(COFFIndexedTables, OutOfLineJumpTableCasesRemainInTheirDispatcher) {
  auto Img = tableImage();
  access(Img, 0xff, 0x24, false, 0x1000, 0x2000); // jmp [eax*4 + .rdata]
  for (unsigned Offset : {0x20, 0x40, 0x80})
    Img.Segments[0].Data[Offset] = 0xc3;
  Symbol Following;
  Following.Addr = 0x1080;
  Following.Size = 1;
  Following.IsFunc = true;
  Following.Name = "following";
  Img.Symbols.push_back(Following);
  pointer(Img, 0x2000, 0x1020);
  pointer(Img, 0x2004, 0x1040);
  coff_loader::recordIndexedTableBasesInCode(Img);
  Decoder Dec;
  ASSERT_TRUE(Dec.init(Img));
  const auto Entries = FuncDetector().detect(Img, Dec);
  for (va_t Label : {0x1020, 0x1040})
    EXPECT_FALSE(std::any_of(Entries.begin(), Entries.end(),
                             [&](const auto &E) { return E.first == Label; }));
}

TEST(COFFIndexedTables, UnresolvedCodePointersNeverBecomeRawAddressConstants) {
  for (Arch A : {Arch::X86, Arch::X64, Arch::AArch64})
    for (BinaryFormat Format :
         {BinaryFormat::COFF, BinaryFormat::ELF, BinaryFormat::MachO}) {
      auto Img = tableImage(A, Format);
      pointer(Img, 0x2000, 0x1080);
      ASSERT_TRUE(Img.CodePtrRelocSlots.count(0x2000));
      // An unemitted continuation has neither callable nor block identity.
      Img.ContinuationCodeStarts.insert(0x1080);
      llvm::LLVMContext Context;
      auto M =
          MedLLVMEmitter().emit({addressSource(Img, 0x2000, false)}, Context,
                                "unresolved", A, {}, &Img, Format);
      EXPECT_EQ(M.get(), nullptr) << static_cast<int>(A) << ':' << int(Format);
    }
}

TEST(COFFIndexedTables, UnclaimedCodeCannotBorrowAnUnrelatedTableMirror) {
  auto Img = tableImage();
  pointer(Img, 0x1100, 0x2000);
  coff_loader::recordIndexedTableBasesInCode(Img);
  // The table has independently proven data storage, the other address does
  // not.
  Img.DataPtrRelocSlots.insert(0x1100);
  Img.DataInCode.insert(0x1100);
  Img.DataAddressRelocOperands.erase(0x1100);
  llvm::LLVMContext Context;
  auto M =
      MedLLVMEmitter().emit({addressSource(Img, 0x1080, false)}, Context,
                            "unclaimed-code", Img.Arch, {}, &Img, Img.Format);
  EXPECT_EQ(M.get(), nullptr);
}

TEST(COFFIndexedTables, EscapedTableAddressPreservesRuntimeIndex) {
  for (Arch A : {Arch::X86, Arch::X64})
    for (bool Large : {false, true}) {
      SCOPED_TRACE(::testing::Message() << int(A) << ':' << Large);
      auto Img = tableImage(A);
      if (Large) {
        Img.Segments[0].Size = Img.Segments[0].FileSz = 0x110000;
        Img.Segments[0].Data.resize(0x110000, 0xcc);
        Img.Sections[0].Size = Img.Sections[0].FileSz = 0x110000;
        Img.Segments[1].VA = Img.Sections[1].VA = 0x200000;
      }
      pointer(Img, 0x1100, Img.Segments[1].VA);
      Img.DataPtrRelocSlots.insert(0x1100);
      Img.DataAddressRelocOperands.erase(0x1100);
      Img.DataInCode.insert(0x1100);
      llvm::LLVMContext Context;
      auto M =
          MedLLVMEmitter().emit({addressSource(Img, 0x1100, true)}, Context,
                                "indexed-address", A, {}, &Img, Img.Format);
      ASSERT_NE(M, nullptr);
      ASSERT_FALSE(llvm::verifyModule(*M, &llvm::errs()));
      auto *F = M->getFunction("table_address");
      ASSERT_NE(F, nullptr);
      ASSERT_EQ(F->arg_size(), 1u);
      for (const auto &B : *F)
        if (const auto *Return =
                llvm::dyn_cast<llvm::ReturnInst>(B.getTerminator())) {
          std::set<const llvm::Value *> Seen;
          EXPECT_TRUE(references(Return->getReturnValue(), F->getArg(0), Seen));
        }
    }
}
