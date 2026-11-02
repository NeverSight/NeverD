//===- RegistrationInitializerTests.cpp - PE32 construction contracts -----===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Check ordinary construction independently of runtime copy callbacks.
//===----------------------------------------------------------------------===//

#include "gtest/gtest.h"

#include "neverd/Limits.h"
#include "neverd/ir/low/CFGBuilder.h"
#include "neverd/ir/low/RegistrationABI.h"
#include "neverd/ir/med/X86/X86RegistrationCall.h"
#include "neverd/loader/BinaryImage.h"
#include "neverd/loader/COFF/COFFLoader.h"
#include "neverd/support/BinaryEncoding.h"

#include <algorithm>
#include <cstdlib>
#include <set>
#include <utility>

namespace {
using namespace neverd;

std::vector<uint8_t> initializeWords() {
  return {0x8b, 0x44, 0x24, 4, 0x89, 1,    0x8b, 0x44, 0x24,
          8,    0x89, 0x41, 4, 0x89, 0xc8, 0xc2, 8,    0};
}

BinaryImage initializerImage(std::vector<uint8_t> Code = initializeWords()) {
  BinaryImage Image;
  Image.Arch = Arch::X86;
  Image.Bits = Bitness::Bits32;
  Image.Format = BinaryFormat::COFF;
  Segment Text;
  Text.VA = 0x1000;
  Text.Flags = SegmentFlags::Readable | SegmentFlags::Executable;
  Text.Data = std::move(Code);
  Text.Size = Text.FileSz = Text.Data.size();
  Section Section;
  Section.Name = ".text";
  Section.VA = Text.VA;
  Section.Size = Section.FileSz = Text.Size;
  Section.Flags = Text.Flags;
  Image.Sections.push_back(Section);
  Image.Segments.push_back(std::move(Text));
  Segment Data;
  Data.VA = 0x3000;
  Data.Flags = SegmentFlags::Readable | SegmentFlags::Writable;
  Data.Data.resize(32);
  Data.Size = Data.Data.size();
  Image.Segments.push_back(std::move(Data));
  return Image;
}

TEST(RegistrationInitializer, BindsScalarArgumentsAndReturnedObject) {
  auto Code = initializeWords();
  const std::vector<uint8_t> Counter = {0x8b, 0x01, 0x01, 0x05, 0, 0x30, 0, 0};
  Code.insert(Code.begin() + 13, Counter.begin(), Counter.end());
  auto Image = initializerImage(Code);
  EXPECT_FALSE(getCheckedX86RegistrationLeafCalleeABI(Image, 0x1000));
  const auto ABI = getCheckedX86RegistrationInitializerCalleeABI(Image, 0x1000);
  ASSERT_TRUE(ABI);
  EXPECT_EQ(ABI->Object.StackPopBytes, 8u);
  EXPECT_FALSE(ABI->Object.HasIndependentScalarReturn);
  EXPECT_EQ(ABI->Initialized, (std::vector<RegistrationObjectExtent>{{0, 8}}));
  EXPECT_EQ(ABI->Object.ECXWrites, ABI->Initialized);
  EXPECT_EQ(ABI->Object.ECXReads,
            (std::vector<RegistrationObjectExtent>{{0, 4}}));
  ASSERT_EQ(ABI->Object.ImageWrites.size(), 1u);
  EXPECT_EQ(ABI->Object.ImageWrites[0].Begin, 0x3000u);
  EXPECT_EQ(ABI->Object.ImageWrites[0].End, 0x3004u);
  Image.Segments[0].Flags = Image.Segments[0].Flags | SegmentFlags::Writable;
  EXPECT_FALSE(getCheckedX86RegistrationInitializerCalleeABI(Image, 0x1000));
  size_t Work = limits::kMaxRegistrationEHStateWork;
  EXPECT_FALSE(getCheckedX86RegistrationInitializerCalleeABI(initializerImage(),
                                                             0x1000, &Work));
}

TEST(RegistrationInitializer, KeepsArgumentsAndThisThroughPrivateSpills) {
  const auto Image = initializerImage(
      {0x55, 0x89, 0xe5, 0x50, 0x8b, 0x45, 12,   0x8b, 0x45, 8,    0x89, 0x4d,
       0xfc, 0x8b, 0x45, 0xfc, 0x8b, 0x4d, 8,    0x89, 8,    0x8b, 0x4d, 12,
       0x89, 0x48, 4,    0x83, 0xc4, 4,    0x5d, 0xc2, 8,    0});
  const auto ABI = getCheckedX86RegistrationInitializerCalleeABI(Image, 0x1000);
  ASSERT_TRUE(ABI);
  EXPECT_EQ(ABI->Initialized, (std::vector<RegistrationObjectExtent>{{0, 8}}));
}

TEST(RegistrationInitializer, IntersectsInitializedBytesAcrossReturns) {
  // Only the first word is initialized on both paths. The second remains a
  // may-write and cannot make an eight-byte throw object safe to dispatch.
  const auto Image =
      initializerImage({0x8b, 0x44, 0x24, 4, 0x89, 1, 0x85, 0xc0, 0x74, 7, 0xc7,
                        0x41, 4,    13,   0, 0,    0, 0x89, 0xc8, 0xc2, 8, 0});
  const auto ABI = getCheckedX86RegistrationInitializerCalleeABI(Image, 0x1000);
  ASSERT_TRUE(ABI);
  EXPECT_EQ(ABI->Initialized, (std::vector<RegistrationObjectExtent>{{0, 4}}));
  EXPECT_EQ(ABI->Object.ECXWrites,
            (std::vector<RegistrationObjectExtent>{{0, 8}}));
}

TEST(RegistrationInitializer, RejectsUnprovedArgumentsEscapesAndReturns) {
  const std::pair<const char *, std::vector<uint8_t>> Cases[] = {
      {"uninitialized object", {0x8b, 0x01}},
      {"argument dereference", {0x8b, 0x44, 0x24, 4, 0x8b, 0}},
      {"argument overwrite", {0x89, 0x44, 0x24, 4}},
      {"argument beyond pop", {0x8b, 0x44, 0x24, 12}},
      {"object escape", {0x89, 0x0d, 0, 0x30, 0, 0}},
      {"caller PC observation", {0x8b, 4, 0x24, 0xa3, 0, 0x30, 0, 0}},
      {"changed nonvolatile", {0x31, 0xdb}},
      {"unchecked call", {0xff, 0xd0}},
      {"FS observation", {0x64, 0xa1, 0, 0, 0, 0}},
      {"code mutation", {0xc7, 0x05, 0, 0x10, 0, 0, 0, 0, 0, 0}}};
  for (const auto &[Name, Prefix] : Cases) {
    SCOPED_TRACE(Name);
    auto Code = initializeWords();
    Code.insert(Code.begin(), Prefix.begin(), Prefix.end());
    EXPECT_FALSE(getCheckedX86RegistrationInitializerCalleeABI(
        initializerImage(Code), 0x1000));
  }
  for (const auto &Return : {std::vector<uint8_t>{0x31, 0xc0},
                             std::vector<uint8_t>{0x8d, 0x41, 4}}) {
    auto Code = initializeWords();
    Code.erase(Code.begin() + 13, Code.begin() + 15);
    Code.insert(Code.begin() + 13, Return.begin(), Return.end());
    EXPECT_FALSE(getCheckedX86RegistrationInitializerCalleeABI(
        initializerImage(Code), 0x1000));
  }
  for (uint8_t Pop : {0, 4, 7}) {
    auto Code = initializeWords();
    Code[Code.size() - 2] = Pop;
    EXPECT_FALSE(getCheckedX86RegistrationInitializerCalleeABI(
        initializerImage(Code), 0x1000));
  }
}

BinaryImage privateConstructionImage(unsigned Mutation) {
  auto Image = initializerImage(std::vector<uint8_t>(0x140, 0xcc));
  Image.Entry = 0x1000;
  auto Initializer = initializeWords();
  if (Mutation == 4)
    std::fill(Initializer.begin() + 10, Initializer.begin() + 13, 0x90);
  std::copy(Initializer.begin(), Initializer.end(),
            Image.Segments[0].Data.begin() + 0x100);
  // Allocate separate object and scalar argument areas. Keep the constructor's
  // EAX return as the actual object argument supplied to the throwing runtime.
  std::vector<uint8_t> Helper = {0x55, 0x89, 0xe5, 0x83, 0xec, 16, 0x8d, 0x4d,
                                 0xf8, 0xc7, 4,    0x24, 7,    0,  0,    0,
                                 0xc7, 0x44, 0x24, 4,    13,   0,  0,    0};
  if (Mutation == 1)
    std::fill(Helper.begin() + 16, Helper.end(), 0x90);
  if (Mutation == 2) {
    const uint8_t Pointer[] = {0x89, 0x0c, 0x24};
    Helper.insert(Helper.end(), std::begin(Pointer), std::end(Pointer));
  }
  if (Mutation == 3)
    Helper[8] = 0xf0;
  auto Call = [&](va_t Target) {
    Helper.push_back(0xe8);
    const size_t Offset = Helper.size();
    Helper.resize(Offset + 4);
    writeLE<uint32_t>(Helper.data() + Offset,
                      uint32_t(Target - (0x1000 + Helper.size())));
  };
  Call(0x1100);
  const uint8_t Arguments[] = {0x68, 0, 0x20, 0, 0, 0x50};
  Helper.insert(Helper.end(), std::begin(Arguments), std::end(Arguments));
  Call(0x1080);
  std::copy(Helper.begin(), Helper.end(), Image.Segments[0].Data.begin());
  Image.Segments[0].Data[0x80] = 0xff;
  Image.Segments[0].Data[0x81] = 0x25;
  writeLE<uint32_t>(Image.Segments[0].Data.data() + 0x82, 0x3010);
  Segment Table;
  Table.VA = 0x2000;
  Table.Flags = SegmentFlags::Readable;
  Table.Data.resize(0x100);
  Table.Size = Table.Data.size();
  for (const auto &[Offset, Value] : {std::pair{12u, 0x2020u},
                                      {0x20u, 1u},
                                      {0x24u, 0x2030u},
                                      {0x34u, 0x2080u},
                                      {0x3cu, UINT32_MAX},
                                      {0x44u, 8u}})
    writeLE<uint32_t>(Table.Data.data() + Offset, Value);
  const std::string Name = ".?AUConstructed@@";
  std::copy(Name.begin(), Name.end(), Table.Data.begin() + 0x88);
  Image.Segments.insert(Image.Segments.begin() + 1, std::move(Table));
  Import Runtime;
  Runtime.Module = "VCRUNTIME140.dll";
  Runtime.Name = "_CxxThrowException";
  Runtime.IATAddr = 0x3010;
  Image.Imports.push_back(Runtime);
  return Image;
}

TEST(RegistrationInitializer, PrivateThrowBindsArgumentsObjectAndCode) {
  for (unsigned Mutation = 0; Mutation != 5; ++Mutation) {
    SCOPED_TRACE(Mutation);
    const auto Image = privateConstructionImage(Mutation);
    const auto Proof = getCheckedX86RegistrationThrowCalleeABI(Image, 0x1000);
    ASSERT_EQ(Proof.has_value(), Mutation == 0);
    if (!Proof)
      continue;
    EXPECT_EQ(Proof->ObjectOffset, -8);
    EXPECT_EQ(Proof->ThrowInfo.ObjectSize, 8u);
    for (va_t Address : {0x1000, 0x1080, 0x1100})
      EXPECT_TRUE(llvm::any_of(Proof->CodeRanges, [&](const auto &Range) {
        return Range.contains(Address);
      }));
  }
}

TEST(RegistrationInitializer, InputPE32BindsSourceConstruction) {
  const auto *Path = std::getenv("NEVERD_REGISTRATION_REALIGNED_NATIVE_PE32");
  if (!Path)
    GTEST_SKIP() << "set NEVERD_REGISTRATION_REALIGNED_NATIVE_PE32";
  auto Image = COFFLoader().load(Path);
  ASSERT_TRUE(bool(Image)) << llvm::toString(Image.takeError());
  Decoder Decode;
  ASSERT_TRUE(Decode.init(*Image));
  std::set<uint32_t> Pops;
  for (const auto &Export : Image->Exports) {
    if (Export.Name != "callback_throw_value" &&
        Export.Name != "callback_throw_reference")
      continue;
    const auto Throw =
        getCheckedX86RegistrationThrowCalleeABI(*Image, Export.Addr);
    ASSERT_TRUE(Throw) << Export.Name;
    const auto Low = CFGBuilder().build(*Image, Decode, Export.Addr);
    for (const auto &Block : Low.Blocks)
      for (const auto &Op : Block.Ops) {
        if (Op.Opcode != NdOp::CALL || Op.NumInputs != 1 ||
            !Op.Inputs[0].isConst())
          continue;
        const auto Init = getCheckedX86RegistrationInitializerCalleeABI(
            *Image, Op.Inputs[0].Offset);
        if (!Init)
          continue;
        Pops.insert(Init->Object.StackPopBytes);
        EXPECT_EQ(Init->Initialized,
                  (std::vector<RegistrationObjectExtent>{
                      {0, int32_t(Throw->ThrowInfo.ObjectSize)}}));
        for (const auto &Range : Init->Object.CodeRanges)
          EXPECT_TRUE(llvm::any_of(Throw->CodeRanges, [&](const auto &Owner) {
            return Owner.Begin <= Range.Begin && Range.End <= Owner.End;
          }));
      }
  }
  EXPECT_EQ(Pops, (std::set<uint32_t>{8, 12}));
  const auto Parent = llvm::find_if(Image->Exports, [](const auto &Export) {
    return Export.Name == "callback_parent";
  });
  ASSERT_NE(Parent, Image->Exports.end());
  const auto Low = CFGBuilder().build(*Image, Decode, Parent->Addr);
  ASSERT_TRUE(Low.RegistrationStates);
  for (const auto &Diagnostic : Low.RegistrationStates->Diagnostics)
    ADD_FAILURE() << Diagnostic;
  EXPECT_TRUE(Low.RegistrationStates->Complete);
  EXPECT_TRUE(Low.RegistrationStates->CallFrameEffectsComplete);
  std::set<uint32_t> DirectPops;
  bool DirectThrow = false;
  for (const auto &Callee : Low.RegistrationStates->CalleeContracts) {
    DirectThrow |= Callee.isRuntimeThrow();
    if (!Callee.isInitializer())
      continue;
    DirectPops.insert(Callee.StackPopBytes);
    const auto Fresh =
        getCheckedX86RegistrationInitializerCalleeABI(*Image, Callee.Target);
    ASSERT_TRUE(Fresh);
    EXPECT_EQ(Callee.ECXInitialized, Fresh->Initialized);
  }
  EXPECT_EQ(DirectPops,
            DirectThrow ? (std::set<uint32_t>{8, 12}) : std::set<uint32_t>{});
  const unsigned FreshThrows = llvm::count_if(
      Low.RegistrationStates->CallFrameEffects, [](const auto &Effect) {
        return Effect.RuntimeThrow && !Effect.RuntimeThrow->isRethrow();
      });
  EXPECT_TRUE(!DirectThrow || FreshThrows == 3 || FreshThrows == 4);
  unsigned DirectCalls = 0, CallbackCalls = 0;
  for (const auto &Block : Low.Blocks)
    for (const auto &Op : Block.Ops) {
      if (Op.Opcode != NdOp::CALL)
        continue;
      const auto *Effect =
          Low.RegistrationStates->callFrameEffect(Op.Addr, Op.Seq);
      if (!Effect)
        continue;
      const auto &Callee =
          Low.RegistrationStates->CalleeContracts[Effect->CalleeIndex];
      if (!Callee.isInitializer())
        continue;
      ++DirectCalls;
      const auto State = llvm::find_if(
          Low.RegistrationStates->Blocks,
          [&](const auto &Candidate) { return Candidate.BlockId == Block.Id; });
      ASSERT_NE(State, Low.RegistrationStates->Blocks.end());
      if (State->CallbackOnly) {
        ++CallbackCalls;
        EXPECT_EQ(Callee.StackPopBytes, 12u);
        ASSERT_EQ(State->CxxCatchStacks.size(), 1u);
        EXPECT_FALSE(State->CxxCatchStacks.front().empty());
      }
      EXPECT_EQ(registrationCallStackPop(Low, Block, Op), Callee.StackPopBytes);
      EXPECT_TRUE(llvm::any_of(
          Low.RegistrationStates->CalleeStacks, [&](const auto &Stack) {
            return !Stack.Indirect && Stack.Target == Op.Inputs[0].Offset &&
                   Stack.StackPopBytes == Callee.StackPopBytes;
          }));
      auto Retargeted = Op;
      ++Retargeted.Inputs[0].Offset;
      EXPECT_FALSE(registrationCallStackPop(Low, Block, Retargeted));
      ++Retargeted.Seq;
      Retargeted.Inputs[0] = Op.Inputs[0];
      EXPECT_FALSE(registrationCallStackPop(Low, Block, Retargeted));
    }
  EXPECT_EQ(DirectCalls, DirectThrow ? (FreshThrows == 4 ? 3u : 2u) : 0u);
  EXPECT_EQ(CallbackCalls, FreshThrows == 4 ? 1u : 0u);
}

TEST(RegistrationCallStackPop, ProvenCalleeCleanupOutlivesAnIncompleteProof) {
  LowFunc Low;
  Low.Entry = 0x1000;
  Low.ExceptionMetadata.emplace();
  Low.RegistrationStates.emplace();
  Low.RegistrationStates->CalleeStacks = {
      {0x2000, 16, false}, {0x2100, 0, false}, {0x3000, 8, true}};
  LowBlock Block;
  Block.StartAddr = Low.Entry;
  for (const auto &[Opcode, Target] :
       {std::pair{NdOp::CALL, 0x2000}, std::pair{NdOp::CALL, 0x2100},
        std::pair{NdOp::CALL, 0x2200}, std::pair{NdOp::INDIR_CALL, 0x3000}}) {
    LowOp Call;
    Call.Opcode = Opcode;
    Call.Addr = Low.Entry + 5 * Block.Ops.size();
    Call.Seq = 0;
    Call.addInput(NdVar::cst(Target, 4));
    Block.Ops.push_back(Call);
  }
  // The state proof around these calls is incomplete, yet a direct callee
  // with a proven return cleanup still pops its bytes at each call to it.
  EXPECT_EQ(registrationCallStackPop(Low, Block, Block.Ops[0]), 16u);
  EXPECT_FALSE(registrationCallStackPop(Low, Block, Block.Ops[1]));
  EXPECT_FALSE(registrationCallStackPop(Low, Block, Block.Ops[2]));
  EXPECT_FALSE(registrationCallStackPop(Low, Block, Block.Ops[3]));
  const LowOp Elsewhere = Block.Ops[0];
  EXPECT_FALSE(registrationCallStackPop(Low, Block, Elsewhere));
  Low.RegistrationStates.reset();
  EXPECT_FALSE(registrationCallStackPop(Low, Block, Block.Ops[0]));
}

} // namespace
