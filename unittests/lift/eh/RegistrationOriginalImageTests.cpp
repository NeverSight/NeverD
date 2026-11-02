//===- RegistrationOriginalImageTests.cpp - Scoped PE32 facts -----------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Keep original-layout evidence separate from generic and native facts.
//===----------------------------------------------------------------------===//

#include "../../../lib/ir/low/X86/RegistrationOriginalImage.h"
#include "gtest/gtest.h"

#include "neverd/Limits.h"
#include "neverd/ir/low/LowIR.h"
#include "neverd/lift/X86/X86Regs.h"
#include "neverd/loader/BinaryImage.h"
#include "neverd/support/BinaryEncoding.h"

#include "llvm/BinaryFormat/COFF.h"

#include <cstddef>
#include <cstdint>
#include <utility>

namespace {
using namespace neverd;
using namespace neverd::registration_state;

BinaryImage scalarImage(uint32_t Base) {
  BinaryImage Image;
  Image.Base = Base;
  Image.Arch = Arch::X86;
  Image.Bits = Bitness::Bits32;
  Image.Format = BinaryFormat::COFF;
  Segment Data;
  Data.VA = Base + 0x2000;
  Data.Size = Data.FileSz = 64;
  Data.Flags = SegmentFlags::Readable;
  Data.Data.resize(Data.Size);
  writeLE<uint32_t>(Data.Data.data() + 8, 0x87654321u);
  Section Storage;
  Storage.VA = Data.VA;
  Storage.Size = Storage.FileSz = Data.Size;
  Storage.Flags = Data.Flags;
  Image.Segments.push_back(std::move(Data));
  Image.Sections.push_back(Storage);
  return Image;
}

LowOp difference() {
  LowOp Op;
  Op.Opcode = NdOp::INT_SUB;
  Op.Output = NdVar::reg(x86reg::RAX, 4);
  Op.addInput(NdVar::reg(x86reg::RCX, 4));
  Op.addInput(NdVar::reg(x86reg::RDX, 4));
  return Op;
}
} // namespace

TEST(RegistrationOriginalImage, DifferenceRequiresExactMappedIdentities) {
  for (uint32_t Base : {0x400000u, 0x18000000u})
    for (unsigned Mode = 0; Mode != 17; ++Mode) {
      SCOPED_TRACE(Base);
      SCOPED_TRACE(Mode);
      auto Image = scalarImage(Base);
      FrameState State;
      auto &Left = State.Registers[x86reg::RCX / x86reg::GeneralRegStride];
      auto &Right = State.Registers[x86reg::RDX / x86reg::GeneralRegStride];
      Left = FrameValue::imageAddress(Base + 0x2030);
      Right = FrameValue::imageAddress(Base + 0x2008);
      auto Op = difference();
      if (Mode == 1)
        std::swap(Left, Right);
      if (Mode == 2)
        Left = FrameValue::constant(*Left.ImageAddress);
      if (Mode == 3)
        Right.MayBeFrame = true;
      if (Mode == 4)
        Right.SecurityCookie = true;
      if (Mode == 5)
        Right = FrameValue::imageAddress(Base + 0x2040);
      if (Mode == 6)
        Image.IsRelocatable = true;
      if (Mode == 7)
        Image.Format = BinaryFormat::ELF;
      if (Mode == 8)
        Image.Arch = Arch::X64;
      if (Mode == 9)
        Image.Sections.push_back(Image.Sections.front());
      if (Mode == 10)
        Image.Segments.push_back(Image.Segments.front());
      if (Mode == 11)
        Op.Output.Size = 2;
      if (Mode == 12)
        Op.Inputs[1].Size = 8;
      if (Mode == 13)
        Op.MemoryAddressSpace = NdMemoryAddressSpace::X86FS;
      if (Mode == 14)
        Op.Opcode = NdOp::INT_ADD;
      if (Mode == 15)
        Op.Output = NdVar::cst(0, 4);
      FrameTransfer Transfer(State, -16);
      const auto Generic = Transfer.evaluate(Op, false);
      size_t Work = Mode == 16 ? limits::kMaxRegistrationEHStateWork : 0;
      OriginalImageFrameProof Scope(Image);
      const auto Value = Scope.evaluate(Op, Transfer, Work);
      ASSERT_EQ(bool(Value), Mode < 2);
      if (Value) {
        EXPECT_EQ(Value->Constant, Mode ? uint32_t(-40) : 40u);
        EXPECT_FALSE(Generic.Constant);
        EXPECT_EQ(Scope.addresses().size(), 2u);
        EXPECT_TRUE(Scope.reads().empty());
      } else
        EXPECT_TRUE(Scope.addresses().empty());
      EXPECT_EQ(Transfer.evaluate(Op, false), Generic);
      EXPECT_LE(Work, limits::kMaxRegistrationEHStateWork);
    }
}

TEST(RegistrationOriginalImage, ScalarStorageKeepsPointerBytesUnknown) {
  for (unsigned Mode = 0; Mode != 11; ++Mode) {
    SCOPED_TRACE(Mode);
    auto Image = scalarImage(0x400000);
    uint16_t Width = 4;
    if (Mode == 1)
      Width = 1;
    if (Mode == 2)
      Width = 2;
    if (Mode == 3)
      writeLE<uint32_t>(Image.Segments.front().Data.data() + 8, 0x402010u);
    if (Mode == 4)
      Image.Segments.front().Flags =
          SegmentFlags::Readable | SegmentFlags::Writable;
    if (Mode == 5)
      Image.Sections.front().Flags =
          SegmentFlags::Readable | SegmentFlags::Writable;
    if (Mode == 6)
      Image.BaseRelocations.push_back(
          {0x402008, llvm::COFF::IMAGE_REL_BASED_HIGHLOW});
    if (Mode == 7)
      Image.Segments.front().FileSz = 10;
    if (Mode == 8)
      Width = 0;
    if (Mode == 9)
      Width = 8;
    if (Mode == 10)
      Image.DataPtrRelocSlots.insert(0x402007);
    OriginalImageFrameProof Scope(Image);
    size_t Work = 0;
    EXPECT_EQ(Scope.permitsRead(0x402008, Width, Work), Mode <= 3);
    const auto Value = Scope.readScalar(0x402008, Width, Work);
    ASSERT_EQ(bool(Value), Mode < 3);
    const auto &Read = Scope.reads().at({0x402008, Width});
    if (Value) {
      EXPECT_EQ(Value->Constant, Mode == 0   ? 0x87654321u
                                 : Mode == 1 ? 0x21u
                                             : 0x4321u);
      ASSERT_TRUE(Read);
      EXPECT_EQ(Read->Bytes.size(), Width);
    }
    if (Mode == 3) {
      ASSERT_TRUE(Read);
      EXPECT_FALSE(Read->Value);
      EXPECT_EQ(readLE<uint32_t>(Read->Bytes.data()), 0x402010u);
    }
  }
}

TEST(RegistrationOriginalImage, OrdinaryStorageExcludesLiveCookieOverlaps) {
  auto Image = scalarImage(0x400000);
  Image.DynInfo.SecurityCookieRVA = 0x2008;
  for (uint16_t Width : {1, 2, 4})
    for (int32_t Offset : {-4, -1, 0, 1, 3, 4}) {
      SCOPED_TRACE(::testing::Message() << Width << " " << Offset);
      const bool Overlap = Offset < 4 && Offset + Width > 0;
      OriginalImageFrameProof Scope(Image);
      size_t Work = 0;
      const va_t Address = uint32_t(0x402008 + Offset);
      size_t ScalarWork = 0;
      EXPECT_EQ(bool(registration_abi::readImmutableRegistrationScalar(
                    Image, Address, Width, ScalarWork)),
                !Overlap);
      EXPECT_EQ(Scope.permitsRead(Address, Width, Work), !Overlap);
      if (Overlap) {
        EXPECT_FALSE(Scope.readScalar(Address, Width, Work));
        EXPECT_FALSE(Scope.permitsRead(Address, Width, Work));
      }
    }
}

TEST(RegistrationOriginalImage, ReadDependsOnTheCurrentImageAndWorkBudget) {
  auto First = scalarImage(0x400000);
  auto Second = First;
  writeLE<uint32_t>(Second.Segments.front().Data.data() + 8, 17u);
  FrameState State;
  State.Registers[x86reg::RCX / x86reg::GeneralRegStride] =
      FrameValue::imageAddress(0x402008);
  FrameTransfer Transfer(State, -16);
  LowOp Load;
  Load.Opcode = NdOp::LOAD;
  Load.Output = NdVar::reg(x86reg::RAX, 4);
  Load.addInput(NdVar::reg(x86reg::RCX, 4));
  OriginalImageFrameProof FirstScope(First), SecondScope(Second);
  size_t FirstWork = 0, SecondWork = 0;
  EXPECT_EQ(FirstScope.evaluate(Load, Transfer, FirstWork),
            FrameValue::constant(0x87654321u));
  EXPECT_EQ(SecondScope.evaluate(Load, Transfer, SecondWork),
            FrameValue::constant(17));
  EXPECT_FALSE(Transfer.evaluate(Load, false).Constant);
  ASSERT_GT(FirstWork, 0u);
  ASSERT_LT(FirstWork, limits::kMaxRegistrationEHStateWork);
  OriginalImageFrameProof ExactScope(First), ShortScope(First);
  size_t Exact = limits::kMaxRegistrationEHStateWork - FirstWork;
  size_t Short = Exact + 1;
  EXPECT_EQ(ExactScope.evaluate(Load, Transfer, Exact),
            FrameValue::constant(0x87654321u));
  EXPECT_EQ(Exact, limits::kMaxRegistrationEHStateWork);
  EXPECT_FALSE(ShortScope.evaluate(Load, Transfer, Short));
  EXPECT_EQ(Short, limits::kMaxRegistrationEHStateWork);
  // A cache hit is still charged to its caller. Exhaustion cannot retrieve
  // previously proved values without paying for the current operation.
  EXPECT_FALSE(FirstScope.evaluate(Load, Transfer, Exact));
  EXPECT_FALSE(FirstScope.permitsRead(0x402008, 4, Exact));

  LowOp Extend;
  Extend.Opcode = NdOp::INT_ZEXT;
  Extend.Output = NdVar::tmp(0x10000, 8);
  Extend.addInput(Load.Inputs[0]);
  Transfer.write(Extend, Transfer.evaluate(Extend, false));
  Load.Inputs[0] = Extend.Output;
  size_t ExtendedWork = 0;
  EXPECT_EQ(FirstScope.evaluate(Load, Transfer, ExtendedWork),
            FrameValue::constant(0x87654321u));
  for (const auto Address : {NdVar::reg(x86reg::RCX, 8), NdVar::tmp(0x10000, 2),
                             NdVar::tmp(0x10000, 16)}) {
    Load.Inputs[0] = Address;
    EXPECT_FALSE(FirstScope.evaluate(Load, Transfer, ExtendedWork));
  }
}
