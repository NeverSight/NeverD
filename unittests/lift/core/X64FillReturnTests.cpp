//===- X64FillReturnTests.cpp - Physical scratch results ----------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Static projection tests; no original or generated machine code is executed.
//===----------------------------------------------------------------------===//

#include "../../../lib/pipeline/X86/X64FillReturn.h"
#include "gtest/gtest.h"

#include "neverd/backend/c/HighC/HighCEmitter.h"
#include "neverd/backend/c/LLVMC/LLVMCEmitter.h"
#include "neverd/ir/med/MedIR.h"
#include "neverd/lift/X86/X86Regs.h"
#include "neverd/loader/BinaryImage.h"
#include "neverd/pipeline/Pipeline.h"

#include "llvm/IR/Verifier.h"
#include "llvm/Support/raw_ostream.h"

#include <cstdint>
#include <iterator>
#include <map>
#include <string>
#include <tuple>
#include <vector>

namespace {
using namespace neverd;

// Relocatable scalar/SSE fill instruction fixture. The checker also changes
// each byte independently, including otherwise unreachable padding.
const uint8_t Code[] = {
    0x48, 0x8b, 0xc1, 0x49, 0x83, 0xf8, 0x08, 0x72, 0x47, 0x0f, 0xb6, 0xd2,
    0x49, 0xb9, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x49, 0x0f,
    0xaf, 0xd1, 0x49, 0x83, 0xf8, 0x4f, 0x73, 0x50, 0x4d, 0x8b, 0xc8, 0x49,
    0x83, 0xe1, 0xf8, 0x49, 0x03, 0xc9, 0x66, 0x0f, 0x1f, 0x44, 0x00, 0x00,
    0x4a, 0x89, 0x54, 0x08, 0xf8, 0x49, 0x83, 0xe9, 0x08, 0x75, 0xf5, 0x49,
    0x83, 0xe0, 0x07, 0x74, 0x05, 0x4a, 0x89, 0x54, 0x01, 0xf8, 0xc3, 0x66,
    0x0f, 0x1f, 0x84, 0x00, 0x00, 0x00, 0x00, 0x00, 0x49, 0x83, 0xe0, 0x07,
    0x74, 0x0a, 0x42, 0x88, 0x54, 0x00, 0xff, 0x49, 0xff, 0xc8, 0x75, 0xf6,
    0xc3, 0x66, 0x66, 0x66, 0x66, 0x66, 0x66, 0x66, 0x0f, 0x1f, 0x84, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x66, 0x48, 0x0f, 0x6e, 0xc2, 0x0f, 0x16, 0xc0,
    0x0f, 0x11, 0x01, 0x4c, 0x03, 0xc1, 0x48, 0x83, 0xc1, 0x10, 0x48, 0x83,
    0xe1, 0xf0, 0x4c, 0x2b, 0xc1, 0x4d, 0x8b, 0xc8, 0x49, 0xc1, 0xe9, 0x07,
    0x74, 0x2f, 0x0f, 0x29, 0x01, 0x0f, 0x29, 0x41, 0x10, 0x48, 0x81, 0xc1,
    0x80, 0x00, 0x00, 0x00, 0x0f, 0x29, 0x41, 0xa0, 0x0f, 0x29, 0x41, 0xb0,
    0x49, 0xff, 0xc9, 0x0f, 0x29, 0x41, 0xc0, 0x0f, 0x29, 0x41, 0xd0, 0x0f,
    0x29, 0x41, 0xe0, 0x0f, 0x29, 0x41, 0xf0, 0x75, 0xd5, 0x49, 0x83, 0xe0,
    0x7f, 0x4d, 0x8b, 0xc8, 0x49, 0xc1, 0xe9, 0x04, 0x74, 0x12, 0x66, 0x0f,
    0x1f, 0x44, 0x00, 0x00, 0x0f, 0x11, 0x01, 0x48, 0x83, 0xc1, 0x10, 0x49,
    0xff, 0xc9, 0x75, 0xf4, 0x49, 0x83, 0xe0, 0x0f, 0x74, 0x06, 0x42, 0x0f,
    0x11, 0x44, 0x01, 0xf0, 0xc3,
};

BinaryImage image() {
  BinaryImage Image;
  Image.Arch = Arch::X64;
  Image.Bits = Bitness::Bits64;
  Image.Format = BinaryFormat::COFF;
  Image.Base = Image.Entry = 0x1000;
  Segment S;
  S.VA = 0x2000;
  S.Size = sizeof(Code);
  S.Data.assign(std::begin(Code), std::end(Code));
  S.Flags = SegmentFlags::Readable | SegmentFlags::Executable;
  Image.Segments.push_back(std::move(S));
  return Image;
}

MedVar reg(int Id, uint64_t Offset, int Version = 0) {
  MedVar V;
  V.Kind = MedVar::Reg;
  V.Id = Id;
  V.RegOff = Offset;
  V.SSAVer = Version;
  V.Size = 8;
  V.TheArch = Arch::X64;
  return V;
}

MedFunc caller() {
  MedFunc F;
  F.Entry = 0x1000;
  F.CC = CallingConv::Win64;
  F.Blocks.resize(1);
  auto &B = F.Blocks.front();
  B.Id = 0;
  MedOp Call;
  Call.Opcode = NdOp::CALL;
  Call.CallSiteId = 1;
  Call.Output = reg(1, x86reg::RAX, 1);
  Call.NumInputs = 4;
  Call.Inputs.resize(Call.NumInputs);
  Call.Inputs[0] = MedVar::makeConst(0x2000, 8);
  Call.Inputs[1] = reg(2, x86reg::RCX);
  Call.Inputs[2] = MedVar::makeConst(0x5c, 4);
  Call.Inputs[3] = reg(3, x86reg::R8);
  B.Ops.push_back(Call);
  const auto Result = reg(2, x86reg::RCX, 1);
  F.CallClobbers.push_back({Result, 1});
  F.CallClobbers.push_back({reg(4, x86reg::RDX, 1), 1});
  MedOp Return;
  Return.Opcode = NdOp::RETURN;
  Return.NumInputs = 1;
  Return.Inputs[0] = Result;
  B.Ops.push_back(Return);
  MedCallInfo CI;
  CI.BlockId = 0;
  CI.OpIdx = 0;
  CI.TargetAddr = 0x2000;
  CI.Args.assign(Call.Inputs.begin() + 1, Call.Inputs.begin() + 4);
  F.CallInfos.push_back(CI);
  return F;
}

uint64_t evaluate(const MedFunc &F, uint64_t Destination, uint64_t Count) {
  using Key = std::tuple<MedVar::VarKind, int, int>;
  const auto KeyOf = [](const MedVar &V) {
    return Key{V.Kind, V.Id, V.SSAVer};
  };
  std::map<Key, uint64_t> Values;
  Values[KeyOf(reg(2, x86reg::RCX))] = Destination;
  Values[KeyOf(reg(3, x86reg::R8))] = Count;
  const auto Read = [&](const MedVar &V) {
    return V.isConst() ? V.ConstVal : Values.at(KeyOf(V));
  };
  for (const auto &Op : F.Blocks.front().Ops) {
    if (Op.Opcode == NdOp::CALL)
      continue; // Only evaluate the added pure instructions.
    if (Op.Opcode == NdOp::RETURN)
      return Read(Op.Inputs[0]);
    uint64_t Value = 0;
    switch (Op.Opcode) {
    case NdOp::COPY:
      Value = Read(Op.Inputs[0]);
      break;
    case NdOp::INT_ADD:
      Value = Read(Op.Inputs[0]) + Read(Op.Inputs[1]);
      break;
    case NdOp::INT_AND:
      Value = Read(Op.Inputs[0]) & Read(Op.Inputs[1]);
      break;
    case NdOp::INT_LESS:
      Value = Read(Op.Inputs[0]) < Read(Op.Inputs[1]);
      break;
    case NdOp::SELECT:
      Value = Read(Op.Inputs[Read(Op.Inputs[0]) ? 1 : 2]);
      break;
    default:
      ADD_FAILURE() << "unexpected effect in result expression";
      return 0;
    }
    Values[KeyOf(Op.Output)] = Value;
  }
  ADD_FAILURE() << "missing return";
  return 0;
}

TEST(X64FillReturn, KeepsCallAndOtherClobbersAndBindsThisOccurrencesArguments) {
  auto Image = image();
  auto F = caller();
  recoverX64FillReturns(F, Image);
  ASSERT_EQ(F.CallClobbers.size(), 1u);
  EXPECT_EQ(F.CallClobbers.front().Value.RegOff, x86reg::RDX);
  ASSERT_EQ(F.CallInfos.size(), 1u);
  EXPECT_EQ(F.CallInfos.front().OpIdx, 0);
  EXPECT_EQ(F.Blocks.front().Ops.front().Opcode, NdOp::CALL);
  for (uint64_t N = 0; N != 513; ++N)
    for (uint64_t A = 0; A != 16; ++A) {
      const uint64_t Start = 0x10000 + A;
      uint64_t Cursor = Start;
      // Independently follow the checked loops' cursor increments.
      if (N >= 79) {
        Cursor = (Cursor + 16) & ~uint64_t{15};
        uint64_t Left = Start + N - Cursor;
        while (Left >= 128) {
          Cursor += 128;
          Left -= 128;
        }
        while (Left >= 16) {
          Cursor += 16;
          Left -= 16;
        }
      } else if (N >= 8) {
        for (uint64_t I = N / 8; I; --I)
          Cursor += 8;
      }
      EXPECT_EQ(evaluate(F, Start, N), Cursor) << N << " / " << A;
    }
  const auto Size = F.Blocks.front().Ops.size();
  recoverX64FillReturns(F, Image);
  EXPECT_EQ(F.Blocks.front().Ops.size(), Size);
}

TEST(X64FillReturn, ChangedBytesAndUntrustedCarriersRetainUnknowns) {
  for (size_t I = 0; I < sizeof(Code); ++I) {
    auto Image = image();
    Image.Segments.front().Data[I] ^= 1;
    auto F = caller();
    recoverX64FillReturns(F, Image);
    EXPECT_EQ(F.CallClobbers.size(), 2u) << I;
    EXPECT_EQ(F.Blocks.front().Ops.size(), 2u) << I;
  }
  for (unsigned Case = 0; Case != 13; ++Case) {
    auto Image = image();
    auto F = caller();
    if (Case == 0)
      Image.Format = BinaryFormat::ELF;
    if (Case == 1)
      Image.Arch = Arch::AArch64;
    if (Case == 2)
      Image.Segments.front().Flags =
          Image.Segments.front().Flags | SegmentFlags::Writable;
    if (Case == 3)
      Image.Segments.front().Data.pop_back();
    if (Case == 4)
      F.SourceParametersBound = true;
    if (Case == 5)
      F.Blocks.front().Ops.front().Inputs[3].Size = 4;
    if (Case == 6)
      F.Blocks.front().Ops.front().IsTailCall = true;
    if (Case == 7)
      F.CallClobbers.front().PreservedPrefixSize = 1;
    if (Case == 8)
      F.Blocks.front().Ops.front().Inputs[0].ConstVal += 1;
    if (Case == 9)
      F.CallInfos.front().TargetAddr += 1;
    if (Case == 10)
      F.CallInfos.front().Args[2].Size = 4;
    if (Case == 11)
      F.Blocks.front().ExceptionalSuccs.push_back({-1, 0});
    if (Case == 12)
      F.Blocks.front().Ops.front().Dead = true;
    recoverX64FillReturns(F, Image);
    EXPECT_EQ(F.CallClobbers.size(), 2u) << Case;
    EXPECT_EQ(F.Blocks.front().Ops.size(), 2u) << Case;
  }
}

TEST(X64FillReturn, FixedCountsNeedNoConditionalExpression) {
  auto Image = image();
  for (uint64_t Count : {0u, 1u, 7u, 8u, 15u, 16u, 78u, 79u, 160u, 4095u}) {
    auto F = caller();
    F.CallInfos.front().Args[2] = MedVar::makeConst(Count, 8);
    recoverX64FillReturns(F, Image);
    ASSERT_EQ(F.CallClobbers.size(), 1u);
    EXPECT_EQ(F.Blocks.front().Ops.size(), Count < 8    ? 3u
                                           : Count < 79 ? 4u
                                                        : 5u);
    for (uint64_t Destination : {uint64_t{0x10003}, ~uint64_t{3}}) {
      const uint64_t Expected = Count < 8 ? Destination
                                : Count < 79
                                    ? Destination + (Count & ~uint64_t{7})
                                    : (Destination + Count) & ~uint64_t{15};
      EXPECT_EQ(evaluate(F, Destination, 999), Expected);
    }
  }
}

TEST(X64FillReturn, SourceBindingsAndFollowingCallsKeepTheirOwnPositions) {
  auto Image = image();
  auto F = caller();
  // The source ABI owner has propagated the count. The native operand is
  // deliberately still the old register, so recomputing from it is wrong.
  F.CallInfos.front().Args[2] = MedVar::makeConst(8, 8);
  auto Following = F.Blocks.front().Ops.front();
  Following.CallSiteId = 2;
  Following.Inputs[0] = MedVar::makeConst(0x3000, 8);
  F.Blocks.front().Ops.insert(F.Blocks.front().Ops.begin() + 1, Following);
  auto Info = F.CallInfos.front();
  Info.TargetAddr = 0x3000;
  Info.OpIdx = 1;
  F.CallInfos.push_back(Info);
  recoverX64FillReturns(F, Image);
  ASSERT_EQ(F.CallClobbers.size(), 1u);
  EXPECT_EQ(evaluate(F, 0x10003, 256), 0x1000bu);
  ASSERT_EQ(F.CallInfos.size(), 2u);
  const auto Index = F.CallInfos.back().OpIdx;
  ASSERT_LT(unsigned(Index), F.Blocks.front().Ops.size());
  EXPECT_EQ(F.Blocks.front().Ops[Index].CallSiteId, 2u);
  EXPECT_EQ(F.Blocks.front().Ops[Index].Inputs[0].ConstVal, 0x3000u);

  auto Unused = caller();
  Unused.Blocks.front().Ops.back().NumInputs = 0;
  recoverX64FillReturns(Unused, Image);
  EXPECT_EQ(Unused.Blocks.front().Ops.size(), 2u);
  EXPECT_EQ(Unused.CallClobbers.size(), 2u);
}

TEST(X64FillReturn, NamedScalarCountFoldsWithoutFreezingAddresses) {
  for (bool Address : {false, true}) {
    auto Image = image();
    auto F = caller();
    MedOp Count;
    Count.Opcode = NdOp::INT_ZEXT;
    Count.Output = F.CallInfos.front().Args[2];
    Count.NumInputs = 1;
    Count.Inputs[0] =
        MedVar::makeConst(160, 4,
                          Address ? ConstantAddressProvenance::AddressFragment
                                  : ConstantAddressProvenance::Scalar);
    F.Blocks.front().Ops.insert(F.Blocks.front().Ops.begin(), Count);
    F.CallInfos.front().OpIdx = 1;
    recoverX64FillReturns(F, Image);
    ASSERT_EQ(F.CallClobbers.size(), 1u);
    EXPECT_EQ(F.Blocks.front().Ops.size(), Address ? 12u : 6u);
    EXPECT_EQ(F.Blocks.front().Ops[1].Opcode, NdOp::CALL);
  }
}

TEST(X64FillReturn, BothSourceRoutesKeepThePhysicalReturnValue) {
  for (unsigned Route = 0; Route != 3; ++Route) {
    const bool Llvm = Route != 0;
    const bool NativeAnalysis = Route == 2;
    auto Image = image();
    Segment Caller;
    Caller.VA = Image.Entry;
    Caller.Flags = SegmentFlags::Readable | SegmentFlags::Executable;
    Caller.Data = {0x48, 0x83, 0xec, 0x28,         // sub rsp,40
                   0x41, 0xb8, 0xa0, 0,    0,   0, // mov r8d,160
                   0x31, 0xd2,                     // xor edx,edx
                   0xe8, 0xef, 0x0f, 0,    0,      // call 0x2000
                   0x48, 0x8b, 0xc1,               // mov rax,rcx
                   0x48, 0x83, 0xc4, 0x28, 0xc3};
    Caller.Size = Caller.Data.size();
    Image.Segments.push_back(std::move(Caller));
    for (const auto &S : Image.Segments) {
      auto Sym = Symbol::makeFunc(S.VA);
      Sym.Size = S.Size;
      Sym.Name = S.VA == Image.Entry ? "caller" : "fill_body";
      Image.Symbols.push_back(std::move(Sym));
    }
    llvm::LLVMContext Context;
    PipelineOptions Options;
    Options.EmitDumpOutput = false;
    Options.LiftMode = Llvm && !NativeAnalysis;
    Options.SourceProjection = !NativeAnalysis;
    Options.MedAnalysisOnly = NativeAnalysis;
    Options.OnlyFunctionEntries = {Image.Entry};
    auto Result = Pipeline().run(Image, Context, Options);
    ASSERT_TRUE(Result.Success) << Result.Error;
    if (NativeAnalysis) {
      ASSERT_EQ(Result.MedFuncs.size(), 1u);
      bool HasNativeClobber = false;
      for (const auto &Clobber : Result.MedFuncs.front().CallClobbers)
        HasNativeClobber |= Clobber.Value.RegOff == x86reg::RCX;
      EXPECT_TRUE(HasNativeClobber);
      continue;
    }
    std::string Text;
    llvm::raw_string_ostream OS(Text);
    if (Llvm) {
      ASSERT_TRUE(Result.LlvmModule);
      ASSERT_FALSE(llvm::verifyModule(*Result.LlvmModule, &llvm::errs()));
      ASSERT_TRUE(LLVMCEmitter().emit(*Result.LlvmModule, OS));
    } else {
      CEmitterOptions Emit;
      Emit.Image = &Image;
      Emit.TheArch = Image.Arch;
      Emit.Format = Image.Format;
      HighCEmitter().emit(Result.HighFuncs, OS, Emit);
    }
    EXPECT_EQ(Text.find("__builtin_trap"), std::string::npos) << Text;
    EXPECT_NE(Text.find(Llvm ? "sub_2000(" : "fill_body("), std::string::npos)
        << Text;
    EXPECT_NE(Text.find("0xA0"), std::string::npos) << Text;
    EXPECT_NE(Text.find("& -16"), std::string::npos) << Text;
  }
}
} // namespace
