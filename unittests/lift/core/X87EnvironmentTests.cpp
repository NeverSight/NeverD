//===- X87EnvironmentTests.cpp - x87 memory image preservation -----------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Check decoded memory extents through lifting, verification and C/LLVM
/// output.
//===----------------------------------------------------------------------===//

#include "gtest/gtest.h"

#include "neverd/backend/c/HighC/HighCEmitter.h"
#include "neverd/backend/c/render/LLVMC/LLVMCIntrinsicRender.h"
#include "neverd/backend/llvm/MedLLVMEmitter.h"
#include "neverd/decode/Decoder.h"
#include "neverd/ir/X87Context.h"
#include "neverd/ir/high/MedToHigh.h"
#include "neverd/ir/intrinsics/X87Environment.h"
#include "neverd/ir/low/CFGBuilder.h"
#include "neverd/ir/low/LowIR.h"
#include "neverd/ir/med/LowToMed.h"
#include "neverd/loader/BinaryImage.h"

#include "llvm/ADT/SmallString.h"
#include "llvm/IR/InlineAsm.h"
#include "llvm/IR/Instructions.h"
#include "llvm/IR/LLVMContext.h"
#include "llvm/IR/Module.h"
#include "llvm/IR/Verifier.h"
#include "llvm/Object/ObjectFile.h"
#include "llvm/Support/Error.h"
#include "llvm/Support/FileSystem.h"
#include "llvm/Support/FileUtilities.h"
#include "llvm/Support/MemoryBuffer.h"
#include "llvm/Support/Path.h"
#include "llvm/Support/Program.h"
#include "llvm/Support/raw_ostream.h"

#include <algorithm>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

using namespace neverd;
namespace {
constexpr va_t kEntry = 0x1000;
testing::AssertionResult validLLVMModule(llvm::Module &Module) {
  std::string Error;
  llvm::raw_string_ostream OS(Error);
  if (llvm::verifyModule(Module, &OS)) {
    OS.flush();
    return testing::AssertionFailure() << Error;
  }
  return testing::AssertionSuccess();
}

testing::AssertionResult validHighC(llvm::StringRef Source,
                                    unsigned ExpectedImageBytes) {
#ifdef NEVERD_TEST_CLANG
  std::string CompilerPath = NEVERD_TEST_CLANG;
#else
  std::string CompilerPath;
#endif
  if (CompilerPath.empty()) {
    auto Compiler = llvm::sys::findProgramByName("clang");
    if (!Compiler)
      return testing::AssertionFailure() << Compiler.getError().message();
    CompilerPath = *Compiler;
  }
  llvm::SmallString<128> IncludeDir;
  std::error_code EC = llvm::sys::fs::createUniqueDirectory(
      "neverd-segment-include", IncludeDir);
  if (EC)
    return testing::AssertionFailure() << EC.message();
  llvm::FileRemover RemoveIncludeDir(IncludeDir);
  llvm::SmallString<128> StringHeader = IncludeDir;
  llvm::sys::path::append(StringHeader, "string.h");
  {
    llvm::raw_fd_ostream Header(StringHeader, EC);
    if (EC)
      return testing::AssertionFailure() << EC.message();
    Header << "void *memcpy(void *, const void *, __SIZE_TYPE__);\n"
              "void *memset(void *, int, __SIZE_TYPE__);\n"
              "int memcmp(const void *, const void *, __SIZE_TYPE__);\n";
  }
  llvm::FileRemover RemoveStringHeader(StringHeader);
  llvm::SmallString<128> SourcePath;
  llvm::SmallString<128> StdoutPath;
  llvm::SmallString<128> StderrPath;
  llvm::SmallString<128> ObjectPath;
  EC = llvm::sys::fs::createTemporaryFile("neverd-segment-memory", "c",
                                          SourcePath);
  if (EC)
    return testing::AssertionFailure() << EC.message();
  llvm::FileRemover RemoveSource(SourcePath);
  EC = llvm::sys::fs::createTemporaryFile("neverd-segment-memory", "out",
                                          StdoutPath);
  if (EC)
    return testing::AssertionFailure() << EC.message();
  llvm::FileRemover RemoveStdout(StdoutPath);
  EC = llvm::sys::fs::createTemporaryFile("neverd-segment-memory", "err",
                                          StderrPath);
  if (EC)
    return testing::AssertionFailure() << EC.message();
  llvm::FileRemover RemoveStderr(StderrPath);
  EC = llvm::sys::fs::createTemporaryFile("neverd-segment-memory", "o",
                                          ObjectPath);
  if (EC)
    return testing::AssertionFailure() << EC.message();
  llvm::FileRemover RemoveObject(ObjectPath);
  {
    llvm::raw_fd_ostream Out(SourcePath, EC);
    if (EC)
      return testing::AssertionFailure() << EC.message();
    Out << Source;
  }
  llvm::SmallVector<llvm::StringRef, 14> Args{CompilerPath,
                                              "-target",
                                              "x86_64-none-elf",
                                              "-ffreestanding",
                                              "-std=gnu11",
                                              "-mavx2",
                                              "-I",
                                              IncludeDir,
                                              "-c",
                                              SourcePath,
                                              "-o",
                                              ObjectPath};
  std::optional<llvm::StringRef> Redirects[] = {std::nullopt, StdoutPath.str(),
                                                StderrPath.str()};
  std::string ExecuteError;
  const int RC = llvm::sys::ExecuteAndWait(
      CompilerPath, Args, std::nullopt, Redirects,
      /*SecondsToWait=*/30, /*MemoryLimit=*/0, &ExecuteError);
  if (RC == 0) {
    auto Object = llvm::object::ObjectFile::createObjectFile(ObjectPath);
    if (!Object)
      return testing::AssertionFailure() << llvm::toString(Object.takeError());
    csh Handle;
    if (cs_open(CS_ARCH_X86, CS_MODE_64, &Handle) != CS_ERR_OK)
      return testing::AssertionFailure() << "cannot decode emitted object";
    cs_option(Handle, CS_OPT_DETAIL, CS_OPT_ON);
    unsigned Images = 0;
    bool Matches = true;
    for (const auto &Section : Object->getBinary()->sections()) {
      if (!Section.isText())
        continue;
      auto Contents = Section.getContents();
      if (!Contents) {
        cs_close(&Handle);
        return testing::AssertionFailure()
               << llvm::toString(Contents.takeError());
      }
      cs_insn *Instructions = nullptr;
      const size_t Count =
          cs_disasm(Handle, reinterpret_cast<const uint8_t *>(Contents->data()),
                    Contents->size(), 0, 0, &Instructions);
      for (size_t I = 0; I < Count; ++I) {
        const auto &Insn = Instructions[I];
        if (Insn.id != X86_INS_FLDENV && Insn.id != X86_INS_FNSTENV &&
            Insn.id != X86_INS_FRSTOR && Insn.id != X86_INS_FNSAVE)
          continue;
        ++Images;
        Matches &= Insn.detail && Insn.detail->x86.op_count == 1 &&
                   Insn.detail->x86.operands[0].size == ExpectedImageBytes;
      }
      cs_free(Instructions, Count);
    }
    cs_close(&Handle);
    if (Images == 1 && Matches)
      return testing::AssertionSuccess();
    return testing::AssertionFailure()
           << "emitted object lost the x87 image extent: images=" << Images
           << " expected bytes=" << ExpectedImageBytes;
  }
  auto ErrorBuffer = llvm::MemoryBuffer::getFile(StderrPath);
  return testing::AssertionFailure()
         << ExecuteError
         << (ErrorBuffer ? (*ErrorBuffer)->getBuffer().str() : std::string{});
}

LowFunc buildFunction(Arch Architecture, InstructionMode Mode,
                      std::vector<uint8_t> Bytes) {
  BinaryImage Image;
  Image.Arch = Architecture;
  Image.Mode = Mode;
  Image.Bits = Architecture == Arch::X64 ? Bitness::Bits64 : Bitness::Bits32;
  Image.Format = BinaryFormat::ELF;
  Image.Base = kEntry;
  Segment Text;
  Text.VA = kEntry;
  Text.Size = Bytes.size();
  Text.Flags = SegmentFlags::Readable | SegmentFlags::Executable;
  Text.Data = std::move(Bytes);
  Image.Segments.push_back(std::move(Text));
  Decoder Dec;
  if (!Dec.init(Image)) {
    ADD_FAILURE() << "decoder initialization failed";
    return {};
  }
  return CFGBuilder().build(Image, Dec, kEntry, "x87_environment");
}

TEST(X87Environment, ImageExtentSurvivesScanAndLifting) {
  struct PrefixCase {
    std::vector<uint8_t> Bytes;
    unsigned EnvironmentBytes;
    bool Only64;
  };
  const PrefixCase Prefixes[] = {{{}, 28, false},
                                 {{0x66}, 14, false},
                                 {{0x67}, 28, false},
                                 {{0x66, 0x67}, 14, false},
                                 {{0x66, 0x48}, 28, true},
                                 {{0x48, 0x66}, 14, true},
                                 {{0x66, 0x48, 0x40}, 14, true},
                                 {{0x66, 0x40, 0x48}, 28, true},
                                 {{0x48, 0x66, 0x48}, 28, true},
                                 {{0x66, 0x48, 0x67}, 14, true},
                                 {{0x66, 0x67, 0x48}, 28, true}};
  const Intrinsic Ids[] = {Intrinsic::X87Fnstenv, Intrinsic::X87Fldenv,
                           Intrinsic::X87Fnsave, Intrinsic::X87Frstor};
  for (Arch A : {Arch::X86, Arch::X64})
    for (bool Owned : {false, true}) {
      if (Owned && A != Arch::X64)
        continue;
      Decoder D;
      ASSERT_TRUE(D.init(A));
      D.setX87ContextOwnership(Owned);
      for (const auto &Prefix : Prefixes) {
        if (Prefix.Only64 && A != Arch::X64)
          continue;
        for (unsigned Family = 0; Family < 4; ++Family) {
          auto Bytes = Prefix.Bytes;
          Bytes.push_back(Family < 2 ? 0xd9 : 0xdd);
          Bytes.push_back(Family % 2 ? 0x20 : 0x30);
          const unsigned Expected =
              Prefix.EnvironmentBytes + (Family < 2 ? 0 : 80);
          SCOPED_TRACE(testing::Message()
                       << "arch=" << unsigned(A) << " owned=" << Owned
                       << " family=" << Family
                       << " bytes=" << testing::PrintToString(Bytes));
          D.setText(false);
          D.setDetail(false);
          DecodedInsn Insn{};
          ASSERT_EQ(D.decodeOneLight(Bytes.data(), Bytes.size(), 0x1000, Insn),
                    Bytes.size());
          D.setText(true);
          D.setDetail(true);
          ASSERT_EQ(
              D.decodeOneForLift(Bytes.data(), Bytes.size(), 0x1000, Insn),
              Bytes.size());
          ASSERT_NE(Insn.Raw->detail, nullptr);
          ASSERT_EQ(Insn.Raw->detail->x86.op_count, 1);
          EXPECT_EQ(Insn.Raw->detail->x86.operands[0].size, Expected);
          EXPECT_EQ(Insn.Raw->detail->x86.operands[0].access,
                    Family % 2 ? CS_AC_READ : CS_AC_WRITE);
          std::vector<LowOp> Ops;
          ASSERT_NO_THROW(D.liftToLow(Insn, Ops));
          const Intrinsic Id = Owned ? Intrinsic::X87ContextStep : Ids[Family];
          auto It = std::find_if(Ops.begin(), Ops.end(), [&](const LowOp &Op) {
            return Op.Opcode == NdOp::INTRINSIC && Op.NumInputs &&
                   Op.Inputs[0].isConst() &&
                   Op.Inputs[0].Offset == static_cast<unsigned>(Id);
          });
          ASSERT_NE(It, Ops.end());
          ASSERT_GE(It->NumInputs, 3u);
          ASSERT_TRUE(It->Inputs[2].isConst());
          if (Owned) {
            const auto Spec = x87ContextInstruction(It->Inputs[2].Offset);
            ASSERT_TRUE(Spec);
            EXPECT_EQ(Spec->MemoryBytes, Expected);
          } else {
            EXPECT_EQ(It->Inputs[2].Offset, Expected);
          }
        }
      }
    }
}

TEST(X87Environment, ExtentReachesEveryEmitter) {
  struct PrefixCase {
    std::vector<uint8_t> Bytes;
    bool Short;
    bool Only64;
  };
  const PrefixCase Prefixes[] = {{{}, false, false},
                                 {{0x66}, true, false},
                                 {{0x66, 0x48}, false, true},
                                 {{0x48, 0x66}, true, true}};
  const Intrinsic Ids[] = {Intrinsic::X87Fnstenv, Intrinsic::X87Fldenv,
                           Intrinsic::X87Fnsave, Intrinsic::X87Frstor};
  for (Arch A : {Arch::X86, Arch::X64})
    for (const auto &Prefix : Prefixes) {
      if (Prefix.Only64 && A != Arch::X64)
        continue;
      for (uint8_t Segment : {0x00, 0x64, 0x65})
        for (unsigned Family = 0; Family < 4; ++Family) {
          SCOPED_TRACE(testing::Message()
                       << "arch=" << unsigned(A) << " short=" << Prefix.Short
                       << " segment=" << unsigned(Segment)
                       << " family=" << Family
                       << " prefix=" << testing::PrintToString(Prefix.Bytes));
          std::vector<uint8_t> Bytes;
          if (Segment)
            Bytes.push_back(Segment);
          Bytes.insert(Bytes.end(), Prefix.Bytes.begin(), Prefix.Bytes.end());
          Bytes.push_back(Family < 2 ? 0xd9 : 0xdd);
          Bytes.push_back(Family % 2 ? 0x20 : 0x30);
          Bytes.push_back(0xc3);
          LowFunc Low = buildFunction(A, InstructionMode::Default, Bytes);
          ASSERT_FALSE(Low.Blocks.empty());
          auto Error = validateLowInstructionBoundaries(Low);
          ASSERT_FALSE(bool(Error)) << llvm::toString(std::move(Error));
          MedFunc Med = LowToMedConverter().convert(Low, A, BinaryFormat::ELF);
          ASSERT_TRUE(verifyMedFunc(Med, "x87-environment-extent"));

          llvm::LLVMContext Context;
          auto Module = MedLLVMEmitter().emit({Med}, Context, "x87-env", A);
          ASSERT_NE(Module, nullptr);
          ASSERT_TRUE(validLLVMModule(*Module));
          const std::string Mnemonic =
              std::string(Prefix.Short ? "data16 " : "") +
              intrinsicAsmMnemonic(Ids[Family]);
          const unsigned ExpectedImageBytes =
              (Prefix.Short ? 14 : 28) + (Family < 2 ? 0 : 80);
          const std::string LLVMAddress = Segment == 0x64   ? "%fs:($0)"
                                          : Segment == 0x65 ? "%gs:($0)"
                                                            : "($0)";
          bool SawInstruction = false;
          for (const auto &Function : *Module)
            for (const auto &Block : Function)
              for (const auto &Instruction : Block) {
                const auto *Call = llvm::dyn_cast<llvm::CallBase>(&Instruction);
                const auto *Asm = Call ? llvm::dyn_cast<llvm::InlineAsm>(
                                             Call->getCalledOperand())
                                       : nullptr;
                if (!Asm || Asm->getAsmString() != Mnemonic + " " + LLVMAddress)
                  continue;
                SawInstruction = true;
                const auto Rendered =
                    renderInlineAsm(A, Asm->getAsmString().str(), false, "",
                                    false, {"address"});
                EXPECT_NE(Rendered.Code.find(Mnemonic + " "),
                          std::string::npos);
                EXPECT_NE(Rendered.Code.find("(%[address])"),
                          std::string::npos);
                if (Segment)
                  EXPECT_NE(
                      Rendered.Code.find(Segment == 0x64 ? "%%fs:" : "%%gs:"),
                      std::string::npos);
                if (A == Arch::X64 && Segment == 0 && Prefix.Bytes.size() <= 1)
                  EXPECT_TRUE(validHighC(
                      "#include <stdint.h>\nvoid env(uintptr_t address) {\n" +
                          Rendered.Code + "}\n",
                      ExpectedImageBytes));
              }
          EXPECT_TRUE(SawInstruction);

          const HighFunc High = MedToHighConverter().convert(Med, A);
          CEmitterOptions Options;
          Options.TheArch = A;
          std::string Source;
          llvm::raw_string_ostream OS(Source);
          ASSERT_TRUE(HighCEmitter().emit({High}, OS, Options));
          OS.flush();
          EXPECT_NE(Source.find(Mnemonic + " "), std::string::npos);
          EXPECT_NE(Source.find("(%[address])"), std::string::npos);
          if (A == Arch::X64 && Segment == 0 && Prefix.Bytes.size() <= 1)
            EXPECT_TRUE(validHighC(Source, ExpectedImageBytes));

          // Removing or corrupting the extent must be rejected before output.
          for (auto &Block : Med.Blocks)
            for (auto &Op : Block.Ops)
              if (Op.Opcode == NdOp::INTRINSIC && Op.NumInputs &&
                  Op.Inputs[0].isConst() &&
                  Op.Inputs[0].ConstVal == unsigned(Ids[Family])) {
                ASSERT_EQ(Op.NumInputs, 3u);
                const auto Extent = Op.Inputs[2];
                Op.Inputs[2] = MedVar::makeConst(15, 1);
                EXPECT_FALSE(verifyMedFunc(Med, "invalid-x87-image-size"));
                Op.Inputs[2] = Extent;
                Op.NumInputs = 2;
                EXPECT_FALSE(verifyMedFunc(Med, "missing-x87-image-size"));
                Op.NumInputs = 3;
              }
        }
    }
}

} // namespace
