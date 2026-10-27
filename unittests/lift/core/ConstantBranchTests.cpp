//===- ConstantBranchTests.cpp - Exact scalar branch recovery ------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Independently authored constant branches with unreachable decode decoys.

#include "../../../lib/ir/low/LowScalarExpression.h"
#include "gtest/gtest.h"

#include "neverd/Limits.h"
#include "neverd/decode/Decoder.h"
#include "neverd/ir/low/CFGBuilder.h"
#include "neverd/loader/BinaryImage.h"
#include "neverd/support/BinaryEncoding.h"

#include "llvm/Support/Error.h"

#include <initializer_list>
#include <set>
#include <vector>

using namespace neverd;

namespace {

std::vector<uint8_t> scalarIdentityBranch(Arch Architecture) {
  // ((x | y) + (x & y)) - x - y + target is target for every word.
  // The inputs remain unknown; no observed argument supplies this identity.
  std::vector<uint8_t> Bytes;
  if (Architecture == Arch::AArch64)
    Bytes = {0x02, 0x00, 0x01, 0xaa,  // orr x2,x0,x1
             0x03, 0x00, 0x01, 0x8a,  // and x3,x0,x1
             0x42, 0x00, 0x03, 0x8b,  // add x2,x2,x3
             0x42, 0x00, 0x00, 0xcb,  // sub x2,x2,x0
             0x42, 0x00, 0x01, 0xcb,  // sub x2,x2,x1
             0x04, 0x10, 0x82, 0xd2,  // mov x4,0x1080
             0x42, 0x00, 0x04, 0x8b,  // add x2,x2,x4
             0x40, 0x00, 0x1f, 0xd6}; // br x2
  else if (Architecture == Arch::X64)
    Bytes = {0x48, 0x89, 0xf8,             // mov rax,rdi
             0x48, 0x09, 0xf0,             // or rax,rsi
             0x48, 0x89, 0xfa,             // mov rdx,rdi
             0x48, 0x21, 0xf2,             // and rdx,rsi
             0x48, 0x01, 0xd0,             // add rax,rdx
             0x48, 0x29, 0xf8,             // sub rax,rdi
             0x48, 0x29, 0xf0,             // sub rax,rsi
             0xba, 0x80, 0x10, 0x00, 0x00, // mov edx,0x1080
             0x48, 0x01, 0xd0,             // add rax,rdx
             0xff, 0xe0};                  // jmp rax
  else
    Bytes = {0x89, 0xf8,                   // mov eax,edi
             0x09, 0xf0,                   // or eax,esi
             0x89, 0xfa,                   // mov edx,edi
             0x21, 0xf2,                   // and edx,esi
             0x01, 0xd0,                   // add eax,edx
             0x29, 0xf8,                   // sub eax,edi
             0x29, 0xf0,                   // sub eax,esi
             0xba, 0x80, 0x10, 0x00, 0x00, // mov edx,0x1080
             0x01, 0xd0,                   // add eax,edx
             0xff, 0xe0};                  // jmp eax
  Bytes.resize(0x80, 0xcc);
  if (Architecture == Arch::AArch64)
    Bytes.insert(Bytes.end(), {0x60, 0x05, 0x80, 0x52, 0xc0, 0x03, 0x5f, 0xd6});
  else
    Bytes.insert(Bytes.end(), {0xb8, 43, 0, 0, 0, 0xc3});
  return Bytes;
}

BinaryImage image(std::vector<uint8_t> Bytes, Arch Architecture,
                  BinaryFormat Format) {
  BinaryImage Image;
  Image.Arch = Architecture;
  Image.Bits = Architecture == Arch::X86 ? Bitness::Bits32 : Bitness::Bits64;
  Image.Format = Format;
  Image.Base = 0x1000;
  Segment Text;
  Text.VA = Image.Base;
  Text.Size = Text.FileSz = Bytes.size();
  Text.Flags = SegmentFlags::Readable | SegmentFlags::Executable;
  Text.Data = std::move(Bytes);
  Image.Segments.push_back(std::move(Text));
  return Image;
}

LowFunc build(const BinaryImage &Image, va_t Entry = 0x1000,
              const std::set<va_t> &Entries = {}) {
  Decoder Dec;
  if (!Dec.init(Image)) {
    ADD_FAILURE() << "decoder initialization failed";
    return {};
  }
  CFGBuilder Builder;
  Builder.setKnownFuncEntries(&Entries);
  auto Function = Builder.build(Image, Dec, Entry, "constant_branch");
  auto Error = validateLowInstructionBoundaries(
      Function, LowInstructionBoundaryRequirement::Required);
  EXPECT_FALSE(bool(Error)) << llvm::toString(std::move(Error));
  return Function;
}

LowFunc build(std::vector<uint8_t> Bytes, Arch Architecture = Arch::X64,
              BinaryFormat Format = BinaryFormat::ELF, va_t Entry = 0x1000,
              const std::set<va_t> &Entries = {}) {
  return build(image(std::move(Bytes), Architecture, Format), Entry, Entries);
}

TEST(ConstantBranch, SymbolicScalarIdentitiesRecoverLocalEdges) {
  for (auto Architecture : {Arch::X86, Arch::X64, Arch::AArch64})
    for (auto Format :
         {BinaryFormat::ELF, BinaryFormat::COFF, BinaryFormat::MachO}) {
      SCOPED_TRACE(::testing::Message()
                   << unsigned(Architecture) << ':' << unsigned(Format));
      const auto Function =
          build(scalarIdentityBranch(Architecture), Architecture, Format);
      ASSERT_FALSE(Function.Blocks.empty());
      EXPECT_EQ(Function.Blocks.front().Succs.size(), 1u);
      bool Local = false;
      for (const auto &Block : Function.Blocks)
        for (const auto &Op : Block.Ops)
          Local |= Op.Opcode == NdOp::BRANCH && Op.NumInputs == 1 &&
                   Op.Inputs[0].isConst() && Op.Inputs[0].Offset == 0x1080;
      EXPECT_TRUE(Local);
    }
}

TEST(ConstantBranch, SymbolicProofIsWithdrawnAfterNewBlockEntries) {
  for (auto Architecture : {Arch::X86, Arch::X64, Arch::AArch64}) {
    SCOPED_TRACE(unsigned(Architecture));
    auto Bytes = scalarIdentityBranch(Architecture);
    // The initially recovered destination branches into the identity after
    // its first definition. The new block no longer has that definition on
    // every incoming edge, so its indirect transfer must be restored.
    const va_t Interior = Architecture == Arch::AArch64 ? 0x1004
                          : Architecture == Arch::X64   ? 0x1003
                                                        : 0x1002;
    if (Architecture == Arch::AArch64)
      writeLE<uint32_t>(Bytes.data() + 0x80,
                        0x14000000 | ((Interior - 0x1080) / 4 & 0x03ffffff));
    else {
      Bytes[0x80] = 0xe9;
      writeLE<uint32_t>(Bytes.data() + 0x81, Interior - 0x1085);
    }
    const auto Function = build(std::move(Bytes), Architecture);
    bool Indirect = false;
    for (const auto &Block : Function.Blocks)
      for (const auto &Op : Block.Ops) {
        Indirect |=
            Op.Opcode == NdOp::INDIR_BR || Op.Opcode == NdOp::INDIR_CALL;
        EXPECT_FALSE(Op.Opcode == NdOp::BRANCH && Op.NumInputs == 1 &&
                     Op.Inputs[0].isConst() && Op.Inputs[0].Offset == 0x1080);
      }
    EXPECT_TRUE(Indirect);
  }
}

TEST(ConstantBranch, SimilarScalarExpressionsKeepTheirInputDependence) {
  for (auto Architecture : {Arch::X86, Arch::X64, Arch::AArch64}) {
    auto Bytes = scalarIdentityBranch(Architecture);
    // Replace AND by XOR. Zero-filled or sample inputs could still suggest
    // the same target, but the complete expression no longer is constant.
    if (Architecture == Arch::AArch64)
      Bytes[7] = 0xca;
    else
      Bytes[Architecture == Arch::X64 ? 10 : 6] = 0x31;
    const auto Function = build(std::move(Bytes), Architecture);
    for (const auto &Block : Function.Blocks)
      for (const auto &Op : Block.Ops)
        EXPECT_FALSE(Op.Opcode == NdOp::BRANCH && Op.NumInputs == 1 &&
                     Op.Inputs[0].isConst() && Op.Inputs[0].Offset == 0x1080);
  }
}

std::vector<LowOp> scalarIdentity(unsigned Bytes, unsigned Form) {
  std::vector<LowOp> Ops;
  const auto X = NdVar::reg(0, Bytes), Y = NdVar::reg(8, Bytes);
  const auto A = NdVar::reg(16, Bytes), B = NdVar::reg(24, Bytes);
  const auto Put = [&](NdOp Opcode, NdVar Output,
                       std::initializer_list<NdVar> Inputs) {
    LowOp Op;
    Op.Opcode = Opcode;
    Op.Output = Output;
    Op.Addr = 0x1000 + Ops.size();
    for (auto Input : Inputs)
      Op.addInput(Input);
    Ops.push_back(Op);
  };
  if (Form == 2) {
    Put(NdOp::INT_NOT, B, {X});
    Put(NdOp::INT_AND, B, {B, Y});
    Put(NdOp::INT_OR, A, {X, Y});
    Put(NdOp::INT_SUB, A, {A, B});
  } else if (Form == 3) {
    Put(NdOp::INT_AND, A, {X, NdVar::scalar(0x5555555555555555, Bytes)});
    Put(NdOp::INT_AND, B, {X, NdVar::scalar(0xaaaaaaaaaaaaaaaa, Bytes)});
    Put(NdOp::INT_ADD, A, {A, B});
  } else {
    Put(Form == 0 ? NdOp::INT_OR : NdOp::INT_XOR, A, {X, Y});
    Put(NdOp::INT_AND, B, {X, Y});
    if (Form == 1)
      Put(NdOp::INT_MULT, B, {B, NdVar::scalar(2, Bytes)});
    Put(NdOp::INT_ADD, A, {A, B});
    Put(NdOp::INT_SUB, A, {A, Y});
  }
  Put(NdOp::INT_SUB, A, {A, X});
  Put(NdOp::INT_ADD, A, {A, NdVar::scalar(0x80, Bytes)});
  return Ops;
}

TEST(ScalarControlExpression, IdentitiesUseEveryBitOfTheirOwnWidth) {
  for (unsigned Bytes : {1, 2, 4, 8})
    for (unsigned Form = 0; Form != 4; ++Form) {
      SCOPED_TRACE(::testing::Message() << Bytes << ':' << Form);
      const auto Ops = scalarIdentity(Bytes, Form);
      size_t Work = limits::kMaxScalarBranchEvidenceWork;
      EXPECT_EQ(
          constantScalarExpression(Ops, NdVar::reg(16, Bytes), 0x2000, Work),
          0x80u);
      EXPECT_LT(Work, limits::kMaxScalarBranchEvidenceWork);
    }
}

TEST(ScalarControlExpression, WritesCallsAndMemoryReadsRetainTheirIdentities) {
  for (unsigned Change = 0; Change != 5; ++Change) {
    SCOPED_TRACE(Change);
    auto Ops = scalarIdentity(8, 0);
    LowOp Barrier;
    Barrier.Addr = 0x1800;
    if (Change == 0) {
      Barrier.Opcode = NdOp::COPY;
      Barrier.Output = NdVar::reg(0, 1);
      Barrier.addInput(NdVar::scalar(7, 1));
    } else if (Change == 1 || Change == 2) {
      Barrier.Opcode = Change == 1 ? NdOp::CALL : NdOp::INTRINSIC;
      Barrier.Output = NdVar::reg(32, 8);
      Barrier.addInput(NdVar::scalar(0x3000, 8));
    } else {
      Barrier.Opcode = NdOp::LOAD;
      Barrier.Output = NdVar::reg(0, 8);
      Barrier.addInput(NdVar::scalar(0x3000, 8));
      if (Change == 4)
        Barrier.MemoryOrdering = NdMemoryOrdering::Acquire;
      Ops.insert(Ops.begin(), Barrier);
    }
    Ops.insert(Ops.end() - 2, Barrier);
    size_t Work = limits::kMaxScalarBranchEvidenceWork;
    EXPECT_FALSE(
        constantScalarExpression(Ops, NdVar::reg(16, 8), 0x2000, Work));
  }
}

TEST(ScalarControlExpression, TemporariesCannotCrossInstructionBoundaries) {
  LowOp Set;
  Set.Opcode = NdOp::COPY;
  Set.Addr = 0x1000;
  Set.Output = NdVar::tmp(1, 8);
  Set.addInput(NdVar::scalar(0x1080, 8));
  for (va_t Query : {0x1000, 0x1001}) {
    size_t Work = limits::kMaxScalarBranchEvidenceWork;
    EXPECT_EQ(constantScalarExpression({Set}, Set.Output, Query, Work),
              Query == Set.Addr ? std::optional<uint64_t>{0x1080}
                                : std::nullopt);
  }
}

TEST(ScalarControlExpression, MalformedAndUnaffordableQueriesPublishNothing) {
  for (unsigned Change = 0; Change != 4; ++Change) {
    auto Ops = scalarIdentity(8, 0);
    size_t Work = limits::kMaxScalarBranchEvidenceWork;
    if (Change == 0)
      Work = 0;
    else if (Change == 1)
      Ops.resize(limits::kMaxScalarBranchSymbolicOperations + 1);
    else if (Change == 2)
      Ops.front().NumInputs = std::size(Ops.front().Inputs) + 1;
    else
      Ops.front().Inputs[0].Size = 4;
    EXPECT_FALSE(
        constantScalarExpression(Ops, NdVar::reg(16, 8), 0x2000, Work));
  }
}

BinaryImage scalarLoadImage(Arch Architecture, BinaryFormat Format) {
  // A full readonly scalar supplies the delta; an unreachable opcode before
  // the destination must not hide the actual continuation's first byte.
  auto Image = image(
      Architecture == Arch::AArch64
          ? std::vector<uint8_t>{0x00, 0x00, 0x82, 0xd2, // mov x0,0x1000
                                 0xe1, 0x7f, 0x00, 0x18, // ldr w1,0x2000
                                 0x00, 0x00, 0x01, 0x8b, // add x0,x0,x1
                                 0x00, 0x00, 0x1f, 0xd6, // br x0
                                 0x00, 0x00, 0x20, 0xd4, // unreachable brk
                                 0xc0, 0x03, 0x5f, 0xd6} // ret
          : std::vector<uint8_t>{0xb8, 0x00, 0x10, 0x00, 0x00, // mov eax,0x1000
                                 0x8b, 0x0c, 0x25, 0x00, 0x20, 0x00, 0x00, 0x01,
                                 0xc8,       // add eax,ecx
                                 0xff, 0xe0, // jmp eax/rax
                                 0xe9, 0xc3},
      Architecture, Format);
  Section Text;
  Text.VA = Image.Base;
  Text.Size = Text.FileSz = Image.Segments.front().Size;
  Text.Flags = Image.Segments.front().Flags;
  if (Format == BinaryFormat::MachO)
    Text.Type = llvm::MachO::S_ATTR_PURE_INSTRUCTIONS;
  Image.Sections.push_back(Text);
  Segment Data;
  Data.VA = 0x2000;
  Data.Flags = SegmentFlags::Readable;
  Data.Data.resize(8);
  writeLE<uint32_t>(Data.Data.data(), Architecture == Arch::AArch64 ? 20 : 17);
  Data.Size = Data.FileSz = Data.Data.size();
  Section ReadOnly;
  ReadOnly.VA = Data.VA;
  ReadOnly.Size = ReadOnly.FileSz = Data.Size;
  ReadOnly.Flags = Data.Flags;
  Image.Segments.push_back(std::move(Data));
  Image.Sections.push_back(ReadOnly);
  return Image;
}

std::set<va_t> addresses(const LowFunc &Function) {
  std::set<va_t> Result;
  for (const auto &Block : Function.Blocks)
    for (const auto &Boundary : Block.InstructionBoundaries)
      Result.insert(Boundary.Address);
  return Result;
}

TEST(ConstantBranch, ArithmeticTargetSkipsUnreachableOpcode) {
  // mov eax,0x1000; add eax,11; jmp eax/rax; db E9; ret.
  for (auto Architecture : {Arch::X86, Arch::X64})
    for (auto Format :
         {BinaryFormat::ELF, BinaryFormat::COFF, BinaryFormat::MachO}) {
      const auto Function = build({0xb8, 0x00, 0x10, 0x00, 0x00, 0x83, 0xc0,
                                   0x0b, 0xff, 0xe0, 0xe9, 0xc3},
                                  Architecture, Format);
      EXPECT_EQ(addresses(Function),
                (std::set<va_t>{0x1000, 0x1005, 0x1008, 0x100b}));
      ASSERT_EQ(Function.Blocks.size(), 2u);
      EXPECT_EQ(Function.Blocks.front().Succs.size(), 1u);
    }
}

TEST(ConstantBranch, SelfDifferenceRecoversTheActualInstructionBoundary) {
  for (auto Architecture : {Arch::X86, Arch::X64, Arch::AArch64})
    for (auto Format :
         {BinaryFormat::ELF, BinaryFormat::COFF, BinaryFormat::MachO}) {
      SCOPED_TRACE(::testing::Message()
                   << unsigned(Architecture) << ':' << unsigned(Format));
      const bool ARM64 = Architecture == Arch::AArch64;
      const auto Function = build(
          ARM64
              ? std::vector<uint8_t>{0x20, 0x00, 0x01, 0xcb, // sub x0,x1,x1
                                     0x90, 0x02, 0x82, 0xd2, // mov x16,0x1014
                                     0x00, 0x00, 0x10, 0x8b, // add x0,x0,x16
                                     0x00, 0x00, 0x1f, 0xd6, // br x0
                                     0x00, 0x00, 0x20, 0xd4, // unreachable brk
                                     0xc0, 0x03, 0x5f, 0xd6} // ret
              : std::vector<uint8_t>{0x29, 0xc0,             // sub eax,eax
                                     0x05, 0x0a, 0x10, 0x00,
                                     0x00, // add eax,0x100a
                                     0xff, 0xe0, 0xe9,
                                     0xc3}, // jmp eax/rax; db E9; ret
          Architecture, Format);
      EXPECT_TRUE(addresses(Function).count(ARM64 ? 0x1014 : 0x100a));
      EXPECT_FALSE(addresses(Function).count(ARM64 ? 0x1010 : 0x1009));
      ASSERT_EQ(Function.Blocks.size(), 2u);
      EXPECT_EQ(Function.Blocks.front().Succs.size(), 1u);
    }
}

TEST(ConstantBranch, UnaryArithmeticReachesTheActualInstructionBoundary) {
  for (auto Architecture : {Arch::X86, Arch::X64})
    for (auto Format :
         {BinaryFormat::ELF, BinaryFormat::COFF, BinaryFormat::MachO})
      for (bool Negate : {false, true}) {
        // mov eax, encoded target; not/neg eax; jmp eax/rax; db E9; ret.
        const auto Function =
            build({0xb8, uint8_t(Negate ? 0xf6 : 0xf5), 0xef, 0xff, 0xff, 0xf7,
                   uint8_t(Negate ? 0xd8 : 0xd0), 0xff, 0xe0, 0xe9, 0xc3},
                  Architecture, Format);
        EXPECT_TRUE(addresses(Function).count(0x100a));
        EXPECT_FALSE(addresses(Function).count(0x1009));
      }
  const auto Signed =
      build({0xb8, 0x14, 0x10, 0,    0,    0xb9, 0xff, 0xff, 0xff, 0xff,
             0x48, 0x63, 0xc9, 0x48, 0x01, 0xc8, 0xff, 0xe0, 0xe9, 0xc3});
  EXPECT_TRUE(addresses(Signed).count(0x1013));
  EXPECT_FALSE(addresses(Signed).count(0x1012));
}

TEST(ConstantBranch,
     ImmutableScalarsRecoverTargetsAcrossArchitecturesAndFormats) {
  for (auto Architecture : {Arch::X86, Arch::X64, Arch::AArch64})
    for (auto Format :
         {BinaryFormat::ELF, BinaryFormat::COFF, BinaryFormat::MachO}) {
      SCOPED_TRACE(::testing::Message()
                   << unsigned(Architecture) << ':' << unsigned(Format));
      const auto Function = build(scalarLoadImage(Architecture, Format));
      EXPECT_TRUE(addresses(Function).count(
          Architecture == Arch::AArch64 ? 0x1014 : 0x1011));
      EXPECT_FALSE(addresses(Function).count(0x1010));
      ASSERT_EQ(Function.Blocks.size(), 2u);
    }
}

TEST(ConstantBranch, UnprovedLoadsDoNotRecoverATarget) {
  for (auto Architecture : {Arch::X86, Arch::X64, Arch::AArch64})
    for (auto Format :
         {BinaryFormat::ELF, BinaryFormat::COFF, BinaryFormat::MachO})
      for (unsigned Mutation = 0; Mutation != 8; ++Mutation) {
        SCOPED_TRACE(::testing::Message()
                     << unsigned(Architecture) << ':' << unsigned(Format) << ':'
                     << Mutation);
        auto Image = scalarLoadImage(Architecture, Format);
        switch (Mutation) {
        case 0:
          Image.Segments.back().Flags =
              SegmentFlags::Readable | SegmentFlags::Writable;
          break;
        case 1:
          Image.Sections.back().Flags =
              SegmentFlags::Readable | SegmentFlags::Writable;
          break;
        case 2:
          Image.BaseRelocations.push_back({0x2000, 3});
          break;
        case 3:
          Image.Segments.back().FileSz = 3;
          break;
        case 4:
          Image.Segments.push_back(Image.Segments.back());
          break;
        case 5:
          Image.Sections.push_back(Image.Sections.back());
          break;
        case 6:
          Image.IsRelocatable = true;
          break;
        case 7:
          Image.Sections.back().FileOff = 1;
          break;
        }
        EXPECT_FALSE(
            addresses(build(Image))
                .count(Architecture == Arch::AArch64 ? 0x1014 : 0x1011));
      }
}

TEST(ConstantBranch, ImmutableLoadStillRequiresAnExactIndexAndFunctionOwner) {
  auto Image = scalarLoadImage(Arch::X64, BinaryFormat::COFF);
  // mov ecx,[rdi*4+0x2000] cannot assume that the unknown index is zero.
  Image.Segments.front().Data[7] = 0xbd;
  EXPECT_FALSE(addresses(build(Image)).count(0x1011));
  Image.Segments.front().Data[7] = 0x25;
  const auto Function = build(Image, 0x1000, {0x1011});
  EXPECT_FALSE(addresses(Function).count(0x1011));
  bool Call = false;
  for (const auto &Block : Function.Blocks)
    for (const auto &Op : Block.Ops)
      Call |= Op.Opcode == NdOp::CALL && Op.NumInputs &&
              Op.Inputs[0].isConst() && Op.Inputs[0].Offset == 0x1011;
  EXPECT_TRUE(Call);
}

TEST(ConstantBranch, UnaffordableImageReadsKeepIndependentArithmeticProofs) {
  auto Image = scalarLoadImage(Arch::X64, BinaryFormat::COFF);
  Image.BaseRelocations.resize(limits::kMaxScalarBranchEvidenceWork + 1);
  EXPECT_FALSE(addresses(build(Image)).count(0x1011));
  auto &Text = Image.Segments.front();
  // Overwrite the unproved load result before the transfer. A cold image
  // query must not consume the remaining budget of this independent proof.
  Text.Data.insert(Text.Data.begin() + 12, {0xb9, 0x16, 0, 0, 0});
  Text.Size = Text.FileSz = Text.Data.size();
  Image.Sections.front().Size = Image.Sections.front().FileSz = Text.Size;
  EXPECT_TRUE(addresses(build(Image)).count(0x1016));
}

TEST(ConstantBranch, ImageReadCacheSurvivesTheCompletedGraphRevalidation) {
  auto Image = scalarLoadImage(Arch::X64, BinaryFormat::COFF);
  Image.BaseRelocations.resize(limits::kMaxScalarBranchEvidenceWork / 2,
                               {0x3000, 3});
  // The budget admits one cold lookup, but not a second. Final revalidation
  // must reuse the exact immutable bytes instead of withdrawing the proof.
  EXPECT_TRUE(addresses(build(Image)).count(0x1011));
}

TEST(ConstantBranch, OrderedAndSegmentRelativeReadsRemainRuntimeInputs) {
  auto Image = scalarLoadImage(Arch::AArch64, BinaryFormat::COFF);
  auto &Text = Image.Segments.front();
  Text.Data = {0x00, 0x00, 0x82, 0xd2,  // mov x0,0x1000
               0x02, 0x00, 0x84, 0xd2,  // mov x2,0x2000
               0x41, 0xfc, 0xdf, 0x88,  // ldar w1,[x2]
               0x00, 0x00, 0x01, 0x8b,  // add x0,x0,x1
               0x00, 0x00, 0x1f, 0xd6,  // br x0
               0x00, 0x00, 0x20, 0xd4,  // unreachable brk
               0xc0, 0x03, 0x5f, 0xd6}; // ret
  Text.Size = Text.FileSz = Text.Data.size();
  Image.Sections.front().Size = Image.Sections.front().FileSz = Text.Size;
  writeLE<uint32_t>(Image.Segments.back().Data.data(), 24);
  EXPECT_FALSE(addresses(build(Image)).count(0x1018));

  Image = scalarLoadImage(Arch::X64, BinaryFormat::COFF);
  auto &X86Text = Image.Segments.front();
  // A GS override cannot read the image's identically numbered default-space
  // address without an independently proved segment base.
  X86Text.Data.insert(X86Text.Data.begin() + 5, 0x65);
  X86Text.Size = X86Text.FileSz = X86Text.Data.size();
  Image.Sections.front().Size = Image.Sections.front().FileSz = X86Text.Size;
  writeLE<uint32_t>(Image.Segments.back().Data.data(), 18);
  EXPECT_FALSE(addresses(build(Image)).count(0x1012));
}

TEST(ConstantBranch, UnknownLiveInAndPartialRegisterStayUnknown) {
  const auto Unknown = build({0x48, 0x83, 0xc0, 0x07, 0xff, 0xe0, 0xe9, 0xc3});
  EXPECT_FALSE(addresses(Unknown).count(0x1007));
  // Writing AX cannot manufacture the remaining 48 bits of RAX.
  const auto Partial =
      build({0x66, 0xb8, 0x08, 0x10, 0xff, 0xe0, 0xe9, 0x90, 0xc3});
  EXPECT_FALSE(addresses(Partial).count(0x1008));
}

TEST(ConstantBranch, NoncontiguousTargetsRetainTheirFrame) {
  // An unrelated function at 0x1008 bounds the entry's initial range, but
  // does not own the distant continuation. That continuation branches back
  // below the selected entry. Neither transfer is a new ABI invocation.
  for (auto Architecture : {Arch::X86, Arch::X64})
    for (auto Format :
         {BinaryFormat::ELF, BinaryFormat::COFF, BinaryFormat::MachO}) {
      const auto Function =
          build({0xc3, 0xb8, 0x10, 0x10, 0,    0,    0xff, 0xe0,
                 0xc3, 0xcc, 0xcc, 0xcc, 0xcc, 0xcc, 0xcc, 0xe9,
                 0xb8, 0x00, 0x10, 0,    0,    0xff, 0xe0},
                Architecture, Format, 0x1001, {0x1008});
      EXPECT_EQ(addresses(Function),
                (std::set<va_t>{0x1000, 0x1001, 0x1006, 0x1010, 0x1015}));
      for (const auto &Block : Function.Blocks)
        for (const auto &Op : Block.Ops)
          EXPECT_NE(Op.Opcode, NdOp::INDIR_CALL);
    }
}

TEST(ConstantBranch, AnExactIndependentEntryRemainsATailCall) {
  const auto Function = build(
      {0xb8, 0x0a, 0x10, 0, 0, 0xff, 0xe0, 0xe9, 0x90, 0x90, 0x31, 0xc0, 0xc3},
      Arch::X64, BinaryFormat::COFF, 0x1000, {0x100a});
  EXPECT_FALSE(addresses(Function).count(0x100c));
  EXPECT_EQ(addresses(Function), (std::set<va_t>{0x1000, 0x1005}));
  bool FoundCall = false;
  for (const auto &Block : Function.Blocks)
    for (const auto &Op : Block.Ops)
      if (Op.Opcode == NdOp::CALL && Op.NumInputs && Op.Inputs[0].isConst()) {
        FoundCall |= Op.Inputs[0].Offset == 0x100a;
        EXPECT_EQ(Op.Addr, 0x1005u);
      }
  EXPECT_TRUE(FoundCall);
}

TEST(ConstantBranch, CallsDoNotPreserveAnInventedRegisterValue) {
  const auto Function = build({0xb8, 0x0d, 0x10, 0x00, 0x00, 0xe8, 0xf6, 0x0f,
                               0x00, 0x00, 0xff, 0xe0, 0xe9, 0xc3});
  EXPECT_FALSE(addresses(Function).count(0x100d));
}

TEST(ConstantBranch, MemoryWithAnUnknownIndexCannotSupplyAConstantTarget) {
  // mov rax,[rdi*8+0x1018]; jmp rax. Slot zero happens to point at 0x1010,
  // but no input fact selects it and there is no finite switch domain.
  std::vector<uint8_t> Bytes = {0x48, 0x8b, 0x04, 0xfd, 0x18,
                                0x10, 0x00, 0x00, 0xff, 0xe0};
  Bytes.resize(0x18, 0x90);
  Bytes[0x10] = 0xc3;
  Bytes.insert(Bytes.end(), {0x10, 0x10, 0, 0, 0, 0, 0, 0});
  const auto Function = build(std::move(Bytes));
  EXPECT_FALSE(addresses(Function).count(0x1010));
  EXPECT_TRUE(Function.JumpTables.empty());
}

TEST(ConstantBranch, AResumableTrapCannotSupplyARegisterFact) {
  const auto Function = build(
      {0xb8, 0x0b, 0x10, 0x00, 0x00, 0xcc, 0x90, 0x90, 0xff, 0xe0, 0xe9, 0xc3});
  EXPECT_FALSE(addresses(Function).count(0x100b));
}

TEST(ConstantBranch, RepeatedBranchesKeepFlagsAndReachBothBodies) {
  // mov eax,0x1000; pushfq; add rax,15; popfq; jmp rax;
  // db E9; mov ecx,0x1000; add rcx,30; jmp rcx; db E9; nop; ret.
  const auto Function =
      build({0xb8, 0x00, 0x10, 0x00, 0x00, 0x9c, 0x48, 0x83, 0xc0, 0x0f, 0x9d,
             0xff, 0xe0, 0xe9, 0x90, 0xb9, 0x00, 0x10, 0x00, 0x00, 0x48, 0x83,
             0xc1, 0x1e, 0xff, 0xe1, 0xe9, 0x90, 0x90, 0x90, 0xc3});
  const auto Starts = addresses(Function);
  EXPECT_TRUE(Starts.count(0x100f));
  EXPECT_TRUE(Starts.count(0x101e));
  EXPECT_FALSE(Starts.count(0x100d));
  EXPECT_FALSE(Starts.count(0x101a));
  bool SavedFlags = false, RestoredFlags = false;
  for (const auto &Block : Function.Blocks)
    for (const auto &Boundary : Block.InstructionBoundaries) {
      SavedFlags |= Boundary.Address == 0x1005;
      RestoredFlags |= Boundary.Address == 0x100a;
    }
  EXPECT_TRUE(SavedFlags);
  EXPECT_TRUE(RestoredFlags);
}

TEST(ConstantBranch, ANewIncomingEdgeWithdrawsAnEarlierConstantProof) {
  const auto Function =
      build({0x85, 0xff, 0x74, 0x0d,       // test edi,edi; jz 0x1011
             0xb8, 0x1b, 0x10, 0x00, 0x00, // mov eax,0x101b
             0x83, 0xc0, 0x00, 0xff, 0xe0, // add eax,0; jmp rax
             0xe9, 0xc3, 0x90, 0xb9, 0x09,
             0x10, 0x00, 0x00, 0xff, 0xe1, // mov ecx,0x1009; jmp rcx
             0xe9, 0x90, 0x90, 0xc3});
  EXPECT_TRUE(addresses(Function).count(0x1009));
  EXPECT_FALSE(addresses(Function).count(0x101b));
  bool Found = false;
  for (const auto &Block : Function.Blocks)
    for (const auto &Boundary : Block.InstructionBoundaries)
      if (Boundary.Address == 0x100c) {
        Found = true;
        EXPECT_FALSE(Boundary.Immediate);
        EXPECT_TRUE(hasLowInstructionControlFlag(
            Boundary.ControlFlags, LowInstructionControlFlag::Indirect));
      }
  EXPECT_TRUE(Found);
  EXPECT_TRUE(Function.UnsafeIndirectBranchAddresses.count(0x100c));
}

TEST(ConstantBranch, CachedBytesDoNotSurviveALostAddressProof) {
  auto Image = scalarLoadImage(Arch::X64, BinaryFormat::COFF);
  auto &Text = Image.Segments.front();
  Text.Data = {0x85, 0xff, 0x74, 0x0c,    // test edi,edi; jz 0x1010
               0xb8, 0x00, 0x20, 0,    0, // mov eax,0x2000
               0x8b, 0x00,                // mov eax,[rax]
               0xff, 0xe0,                // jmp rax
               0xe9, 0xc3, 0x90, 0xb9, 0x09, 0x10, 0, 0, // mov ecx,0x1009
               0xff, 0xe1,                               // jmp rcx
               0xe9, 0x90, 0xc3};
  Text.Size = Text.FileSz = Text.Data.size();
  Image.Sections.front().Size = Image.Sections.front().FileSz = Text.Size;
  writeLE<uint32_t>(Image.Segments.back().Data.data(), 0x1019);
  const auto Function = build(Image);
  EXPECT_TRUE(addresses(Function).count(0x1009));
  EXPECT_FALSE(addresses(Function).count(0x1019));
  EXPECT_TRUE(Function.UnsafeIndirectBranchAddresses.count(0x100b));
}

TEST(ConstantBranch, AnOpcodeReachedByAnotherPathRemainsCode) {
  const auto Function =
      build({0x74, 0x0a, 0xb8, 0x0d, 0x10, 0x00, 0x00, 0x90, 0x90, 0x90, 0xff,
             0xe0, 0xe9, 0xc3, 0x90, 0x90, 0x90});
  EXPECT_TRUE(addresses(Function).count(0x100c));
  EXPECT_TRUE(addresses(Function).count(0x100d));
}

TEST(ConstantBranch, AArch64UsesTheSameExactScalarAuthority) {
  const auto Function = build({0x90, 0x01, 0x82, 0xd2, // mov x16,0x100c
                               0x00, 0x02, 0x1f, 0xd6, // br x16
                               0x00, 0x00, 0x20, 0xd4, // unreachable brk
                               0xc0, 0x03, 0x5f, 0xd6},
                              Arch::AArch64);
  EXPECT_EQ(addresses(Function), (std::set<va_t>{0x1000, 0x1004, 0x100c}));
}

} // namespace
