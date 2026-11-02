//===- ARM32_PredicatedStackTests.cpp - Predicated push and pop ----------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
//
// Capstone spells a predicated push or pop `pushne`/`popne`, and `popne.w`
// inside a Thumb-2 IT block.  Every register it lists is transferred through
// sp, the first one is no base register, and a listed pc makes a pop a
// return.
//
//===----------------------------------------------------------------------===//

#include "gtest/gtest.h"

#include "neverd/backend/c/CEmitterOptions.h"
#include "neverd/backend/c/HighC/HighCEmitter.h"
#include "neverd/decode/Decoder.h"
#include "neverd/lift/ARM/ARMLifter.h"
#include "neverd/lift/ARM/ARMRegs.h"
#include "neverd/pipeline/Pipeline.h"
#include "neverd/support/BinaryLoading.h"

#include "llvm/ADT/ArrayRef.h"
#include "llvm/IR/LLVMContext.h"
#include "llvm/Support/Error.h"
#include "llvm/Support/raw_ostream.h"

#include <filesystem>
#include <string>
#include <utility>
#include <vector>

using namespace neverd;

namespace {

/// The ops the A32 instruction \p Bytes lifts to.
std::vector<LowOp> liftA32(llvm::ArrayRef<uint8_t> Bytes) {
  Decoder Dec;
  EXPECT_TRUE(Dec.init(Arch::ARM, InstructionMode::ARM));
  DecodedInsn Insn{};
  EXPECT_EQ(Dec.decodeOne(Bytes.data(), Bytes.size(), 0x1000, Insn),
            Bytes.size());
  ARMLifter L(Arch::ARM, InstructionMode::ARM);
  std::vector<LowOp> Ops;
  EXPECT_NO_THROW(L.lift(Insn.Raw, Ops));
  return Ops;
}

bool writes(const std::vector<LowOp> &Ops, uint64_t Reg) {
  for (const LowOp &Op : Ops)
    if (Op.Output.isReg() && Op.Output.Offset == Reg)
      return true;
  return false;
}

size_t count(const std::vector<LowOp> &Ops, NdOp Opcode) {
  size_t N = 0;
  for (const LowOp &Op : Ops)
    N += Op.Opcode == Opcode;
  return N;
}

TEST(ARM32PredicatedStack, PredicatedPopTransfersThroughSp) {
  // popne {r4, pc}: a conditional return.
  const uint8_t Return[] = {0x10, 0x80, 0xbd, 0x18};
  std::vector<LowOp> Ops = liftA32(Return);
  EXPECT_EQ(count(Ops, NdOp::RETURN), 1u);
  EXPECT_EQ(count(Ops, NdOp::INDIR_BR), 0u);
  EXPECT_EQ(count(Ops, NdOp::LOAD), 2u);
  EXPECT_TRUE(writes(Ops, armreg::SP));
  EXPECT_TRUE(writes(Ops, armreg::R4));

  // popne {r4, r5}
  const uint8_t Restore[] = {0x30, 0x00, 0xbd, 0x18};
  Ops = liftA32(Restore);
  EXPECT_EQ(count(Ops, NdOp::LOAD), 2u);
  EXPECT_TRUE(writes(Ops, armreg::SP));
  EXPECT_TRUE(writes(Ops, armreg::R4));
  EXPECT_TRUE(writes(Ops, armreg::R5));

  // ldrne r4, [sp], #4, which Capstone spells popne {r4}.
  const uint8_t Single[] = {0x04, 0x40, 0x9d, 0x14};
  Ops = liftA32(Single);
  EXPECT_EQ(count(Ops, NdOp::LOAD), 1u);
  EXPECT_TRUE(writes(Ops, armreg::SP));
  EXPECT_TRUE(writes(Ops, armreg::R4));
}

TEST(ARM32PredicatedStack, PredicatedPushTransfersThroughSp) {
  // pushne {r4, r5} stores both registers below sp and changes no r4.
  const uint8_t Save[] = {0x30, 0x00, 0x2d, 0x19};
  const std::vector<LowOp> Ops = liftA32(Save);
  EXPECT_EQ(count(Ops, NdOp::STORE), 2u);
  EXPECT_TRUE(writes(Ops, armreg::SP));
  EXPECT_FALSE(writes(Ops, armreg::R4));
}

/// The body of \p Name's definition in \p Source, or empty.
std::string definitionBody(const std::string &Source, const std::string &Name) {
  for (size_t At = Source.find(" " + Name + "("); At != std::string::npos;
       At = Source.find(" " + Name + "(", At + 1)) {
    const size_t LineEnd = Source.find('\n', At);
    if (LineEnd == std::string::npos || LineEnd == 0 ||
        Source[LineEnd - 1] != '{')
      continue;
    const size_t End = Source.find("\n}\n", LineEnd);
    return Source.substr(LineEnd,
                         End == std::string::npos ? End : End - LineEnd);
  }
  return {};
}

TEST(ARM32PredicatedStack, PredicatedReturnsDecompileWithoutTraps) {
  auto ImageOrErr = loadBinary(std::filesystem::path(TEST_OBJ_DIR) /
                               "test_predicated_pop_arm.o");
  ASSERT_TRUE(static_cast<bool>(ImageOrErr))
      << llvm::toString(ImageOrErr.takeError());
  const BinaryImage &Img = *ImageOrErr;
  const char *Functions[] = {"pred_return_arm", "pred_single_arm",
                             "pred_return_thumb"};
  llvm::LLVMContext Ctx;
  PipelineOptions Opts;
  Opts.EmitDumpOutput = false;
  for (const char *Name : Functions) {
    const Symbol *Sym = Img.findSymbol(Name);
    ASSERT_NE(Sym, nullptr) << Name;
    Opts.OnlyFunctionEntries.insert(Sym->Addr);
  }
  const PipelineResult Result = Pipeline().run(Img, Ctx, Opts);
  ASSERT_TRUE(Result.Success) << Result.Error;
  std::string Source;
  llvm::raw_string_ostream OS(Source);
  CEmitterOptions Options;
  Options.TheArch = Img.Arch;
  Options.Format = Img.Format;
  Options.Image = &Img;
  ASSERT_TRUE(HighCEmitter().emit(Result.HighFuncs, OS, Options));
  // Each function returns a value on every path: the one each condition
  // selects.
  const std::pair<const char *, std::vector<const char *>> Results[] = {
      {"pred_return_arm", {"7", "+ 1"}},
      {"pred_single_arm", {"5", "9"}},
      {"pred_return_thumb", {"7", "+ 1"}}};
  for (const auto &[Name, Values] : Results) {
    SCOPED_TRACE(Name);
    EXPECT_NE(Source.find(std::string("int32_t ") + Name + "("),
              std::string::npos)
        << Source;
    const std::string Body = definitionBody(Source, Name);
    ASSERT_FALSE(Body.empty()) << Source;
    EXPECT_EQ(Body.find("__builtin_trap"), std::string::npos) << Body;
    for (const char *Value : Values)
      EXPECT_NE(Body.find(Value), std::string::npos) << Value << Body;
  }
}

} // namespace
