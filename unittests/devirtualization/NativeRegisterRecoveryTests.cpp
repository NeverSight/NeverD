//===- NativeRegisterRecoveryTests.cpp - Native register recovery ---------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#include "gtest/gtest.h"

#include "neverd/analysis/BinaryInterpreterSpecialization.h"
#include "neverd/decode/Decoder.h"
#include "neverd/lift/X86/X86Regs.h"
#include "neverd/symbolic/SymExec.h"

using namespace neverd;
using namespace neverd::symbolic;

namespace {
std::vector<LowOp> liftOne(Arch Architecture,
                           std::initializer_list<uint8_t> Bytes) {
  Decoder Decode;
  Decode.init(Architecture);
  Decode.setStrict(true);
  DecodedInsn Insn{};
  std::vector<LowOp> Ops;
  if (!Decode.decodeOneForLift(Bytes.begin(), Bytes.size(), 0x1000, Insn)) {
    ADD_FAILURE() << "cannot decode synthetic register instruction";
    return Ops;
  }
  Decode.liftToLow(Insn, Ops);
  return Ops;
}

void expectRegisterWrite(Arch Architecture, std::initializer_list<uint8_t> Code,
                         uint64_t Register, uint64_t Initial,
                         uint64_t Expected) {
  auto Ops = liftOne(Architecture, Code);
  ASSERT_FALSE(Ops.empty());
  SymContext Context;
  SymState State(Context, llvm::endianness::little);
  State.write(SymSpace::Register, Register, Context.mkConst(64, Initial));
  SymExec Exec(Context, State);
  for (const auto &Op : Ops)
    ASSERT_EQ(Exec.step(Op), StepResult::Continue);
  const auto Result =
      Context.asConst(State.read(SymSpace::Register, Register, 8));
  ASSERT_TRUE(Result);
  EXPECT_EQ(Result->getZExtValue(), Expected);
}
} // namespace

TEST(NativeRegisterRecovery, FrameRegisterDwordWritesClearUpperBits) {
  expectRegisterWrite(Arch::X64, {0xbd, 0x44, 0x33, 0x22, 0x11}, x86reg::RBP,
                      UINT64_C(0xabcdef0100000000), 0x11223344);
  expectRegisterWrite(Arch::X64, {0xbc, 0x44, 0x33, 0x22, 0x11}, x86reg::RSP,
                      UINT64_C(0xabcdef0100000000), 0x11223344);
}

TEST(NativeRegisterRecovery, ByteAndWordWritesPreserveSurroundingBytes) {
  constexpr uint64_t Initial = UINT64_C(0x123456789abcdef0);
  expectRegisterWrite(Arch::X64, {0xb0, 0xaa}, x86reg::RAX, Initial,
                      UINT64_C(0x123456789abcdeaa));
  expectRegisterWrite(Arch::X64, {0xb4, 0xaa}, x86reg::RAX, Initial,
                      UINT64_C(0x123456789abcaaf0));
  expectRegisterWrite(Arch::X64, {0x66, 0xb8, 0x44, 0x33}, x86reg::RAX, Initial,
                      UINT64_C(0x123456789abc3344));
}

TEST(NativeRegisterRecovery, X86DoesNotAcquireAnX64UpperRegisterWrite) {
  for (const auto &[Opcode, Register] :
       {std::pair{uint8_t{0xbd}, x86reg::RBP},
        std::pair{uint8_t{0xbc}, x86reg::RSP}}) {
    auto Ops = liftOne(Arch::X86, {Opcode, 0x44, 0x33, 0x22, 0x11});
    ASSERT_FALSE(Ops.empty());
    for (const auto &Op : Ops)
      EXPECT_FALSE(Op.Output == NdVar::reg(Register, 8));
    expectRegisterWrite(Arch::X86, {Opcode, 0x44, 0x33, 0x22, 0x11}, Register,
                        UINT64_C(0xabcdef0100000000),
                        UINT64_C(0xabcdef0111223344));
  }
}

TEST(NativeRegisterRecovery,
     ProviderFoldsBranchAfterEbpWriteUsingClearedUpper) {
  BinaryImage Image;
  Image.Arch = Arch::X64;
  Image.Format = BinaryFormat::ELF;
  Image.ExceptionMetadata.ParseStatus = ExceptionParseStatus::Complete;
  Segment Code;
  Code.VA = 0x1000;
  Code.Flags = SegmentFlags::Readable | SegmentFlags::Executable;
  // mov ebp,1; cmp rbp,1; jne rejected; mov eax,7; ret; rejected: int3
  Code.Data = {0xbd, 0x01, 0x00, 0x00, 0x00, 0x48, 0x83, 0xfd, 0x01,
               0x75, 0x06, 0xb8, 0x07, 0x00, 0x00, 0x00, 0xc3, 0xcc};
  Code.Size = Code.FileSz = Code.Data.size();
  Image.Segments.push_back(std::move(Code));
  analysis::SpecializationOptions Options;
  Options.EntryConstants.push_back(
      {NdVar::reg(x86reg::RBP, 8), UINT64_C(0x1234567800000000)});
  const auto Result =
      analysis::specializeBinaryInterpreter(Image, 0x1000, Options);
  ASSERT_TRUE(Result.complete()) << Result.Diagnostic;
  for (const auto &Block : Result.Residual.Blocks)
    for (const auto &Boundary : Block.InstructionBoundaries)
      EXPECT_NE(Boundary.Address, 0x1011u);
}
