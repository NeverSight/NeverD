//===- EVMEmitterTests.cpp - EVM backend tests --------------------------===//

#include "gtest/gtest.h"

#include "neverd/evm/Analyzer.h"
#include "neverd/evm/CEmitter.h"
#include "neverd/evm/Interpreter.h"
#include "neverd/evm/LLVMEmitter.h"
#include "neverd/evm/SolidityEmitter.h"

#include "llvm/ADT/SmallString.h"
#include "llvm/IR/LLVMContext.h"
#include "llvm/IR/Verifier.h"
#include "llvm/Support/Error.h"
#include "llvm/Support/FileSystem.h"
#include "llvm/Support/raw_ostream.h"

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <functional>

namespace neverd::evm {
namespace {

inline constexpr unsigned kSStoreValueArgumentIndex = 1;

TEST(EVMLLVMEmitter, ProducesVerifiedI256StateMachine) {
  const std::vector<uint8_t> Code = {
      0x60, 0x01, 0x60, 0x02, 0x60, 0x03, 0x08, // ADDMOD
      0x60, 0x0b, 0x56, 0x00, 0x5b, 0x00};      // validated JUMP
  auto Program = analyze(Code);
  ASSERT_TRUE(static_cast<bool>(Program))
      << llvm::toString(Program.takeError());

  llvm::LLVMContext Context;
  auto Module = emitLLVM(*Program, Context);
  ASSERT_TRUE(static_cast<bool>(Module)) << llvm::toString(Module.takeError());
  EXPECT_FALSE(llvm::verifyModule(**Module, &llvm::errs()));

  const std::string IR = emitLLVMText(**Module);
  EXPECT_NE(IR.find("define i32 @evm_execute(ptr"), std::string::npos);
  EXPECT_NE(IR.find("[1024 x i256]"), std::string::npos);
  EXPECT_NE(IR.find("i512"), std::string::npos);
  EXPECT_NE(IR.find("switch i256"), std::string::npos);
  EXPECT_NE(IR.find("neverd_evm_host_op"), std::string::npos);
  EXPECT_NE(IR.find("stack.overflow"), std::string::npos);
}

TEST(EVMLLVMEmitter, EmitsGuardedSignedDivisionAndEnvironmentHooks) {
  const std::vector<uint8_t> Code = {0x60, 0x07, 0x60, 0x00, 0x05, 0x60, 0x80,
                                     0x5f, 0x0b, 0x60, 0x01, 0x60, 0xff, 0x1a,
                                     0x34, 0x01, 0x60, 0x00, 0x55, 0x00};
  auto Program = analyze(Code);
  ASSERT_TRUE(static_cast<bool>(Program))
      << llvm::toString(Program.takeError());
  llvm::LLVMContext Context;
  auto Module = emitLLVM(*Program, Context);
  ASSERT_TRUE(static_cast<bool>(Module)) << llvm::toString(Module.takeError());
  EXPECT_FALSE(llvm::verifyModule(**Module, &llvm::errs()));
  const std::string IR = emitLLVMText(**Module);
  EXPECT_NE(IR.find("sdiv"), std::string::npos);
  EXPECT_NE(IR.find("select i1"), std::string::npos);
  EXPECT_NE(IR.find("i8 52"), std::string::npos);  // CALLVALUE host opcode.
  EXPECT_NE(IR.find("i8 85"), std::string::npos);  // SSTORE host opcode.
  EXPECT_EQ(IR.find("i8 11,"), std::string::npos); // SIGNEXTEND is inline.
  EXPECT_EQ(IR.find("i8 26,"), std::string::npos); // BYTE is inline.
}

std::filesystem::path writeTemporarySource(llvm::StringRef Extension,
                                           llvm::StringRef Source) {
  llvm::SmallString<128> UniquePath;
  const std::error_code EC = llvm::sys::fs::createTemporaryFile(
      "neverd-evm-emitter", Extension.ltrim(".-"), UniquePath);
  if (EC) {
    ADD_FAILURE() << "cannot create temporary source: " << EC.message();
    return {};
  }
  const std::filesystem::path Path(UniquePath.str().str());
  std::ofstream Output(Path, std::ios::binary);
  if (!Output) {
    ADD_FAILURE() << "cannot open temporary source: " << Path;
    return {};
  }
  Output.write(Source.data(), static_cast<std::streamsize>(Source.size()));
  return Path;
}

void appendPush(std::vector<uint8_t> &Code, const llvm::APInt &Value) {
  if (Value.isZero()) {
    Code.push_back(opcodeByte(Opcode::PUSH0));
    return;
  }
  const unsigned DataBytes =
      (Value.getActiveBits() + kBitsPerByte - 1) / kBitsPerByte;
  Code.push_back(
      static_cast<uint8_t>(opcodeByte(Opcode::PUSH1) + DataBytes - 1));
  for (unsigned I = DataBytes; I != 0; --I)
    Code.push_back(static_cast<uint8_t>(
        Value.extractBitsAsZExtValue(kBitsPerByte, (I - 1) * kBitsPerByte)));
}

std::vector<uint8_t> differentialALUProgram() {
  // Fold every scalar ALU family into one observable storage word. Inputs hit
  // non-commutative, signed, overflow, zero-divisor, saturated-shift, byte,
  // wide modular, exponent, and Fusaka CLZ behavior in all three backends.
  std::vector<uint8_t> Code;
  appendPush(Code, llvm::APInt(kWordBits, 0));
  const auto Fold = [&] { Code.push_back(opcodeByte(Opcode::XOR)); };
  const auto Unary = [&](Opcode Op, const llvm::APInt &A) {
    appendPush(Code, A);
    Code.push_back(opcodeByte(Op));
    Fold();
  };
  const auto Binary = [&](Opcode Op, const llvm::APInt &A,
                          const llvm::APInt &B) {
    appendPush(Code, B);
    appendPush(Code, A);
    Code.push_back(opcodeByte(Op));
    Fold();
  };
  const auto Ternary = [&](Opcode Op, const llvm::APInt &A,
                           const llvm::APInt &B, const llvm::APInt &Modulus) {
    appendPush(Code, Modulus);
    appendPush(Code, B);
    appendPush(Code, A);
    Code.push_back(opcodeByte(Op));
    Fold();
  };

  const llvm::APInt Zero(kWordBits, 0);
  const llvm::APInt One(kWordBits, 1);
  const llvm::APInt Two(kWordBits, 2);
  const llvm::APInt Max = llvm::APInt::getMaxValue(kWordBits);
  const llvm::APInt SignedMin = llvm::APInt::getSignedMinValue(kWordBits);
  const llvm::APInt NegativeOne = llvm::APInt::getAllOnes(kWordBits);
  const llvm::APInt Negative123 = -llvm::APInt(kWordBits, 123);

  Binary(Opcode::ADD, Max, Two);
  Binary(Opcode::MUL, llvm::APInt(kWordBits, 0x123456789ULL),
         llvm::APInt(kWordBits, 0xfedcba987ULL));
  Binary(Opcode::SUB, llvm::APInt(kWordBits, 10), llvm::APInt(kWordBits, 3));
  Binary(Opcode::DIV, Max, llvm::APInt(kWordBits, 17));
  Binary(Opcode::DIV, llvm::APInt(kWordBits, 5), Zero);
  Binary(Opcode::SDIV, Negative123, llvm::APInt(kWordBits, 7));
  Binary(Opcode::SDIV, SignedMin, NegativeOne);
  Binary(Opcode::SDIV, Negative123, Zero);
  Binary(Opcode::MOD, Max, llvm::APInt(kWordBits, 97));
  Binary(Opcode::SMOD, Negative123, llvm::APInt(kWordBits, 10));
  Binary(Opcode::SMOD, Negative123, Zero);
  Ternary(Opcode::ADDMOD, Max, Max, llvm::APInt(kWordBits, 97));
  Ternary(Opcode::ADDMOD, Max, One, Zero);
  Ternary(Opcode::MULMOD, Max, Max, llvm::APInt(kWordBits, 101));
  Ternary(Opcode::MULMOD, Max, One, Zero);
  Binary(Opcode::EXP, llvm::APInt(kWordBits, 3), llvm::APInt(kWordBits, 19));
  Binary(Opcode::EXP, Max, Two);
  Binary(Opcode::SIGNEXTEND, Zero, llvm::APInt(kWordBits, 0x80));
  Binary(Opcode::SIGNEXTEND, llvm::APInt(kWordBits, kWordBytes), Max);

  Binary(Opcode::LT, Two, llvm::APInt(kWordBits, 3));
  Binary(Opcode::GT, llvm::APInt(kWordBits, 4), llvm::APInt(kWordBits, 3));
  Binary(Opcode::SLT, NegativeOne, One);
  Binary(Opcode::SGT, One, NegativeOne);
  Binary(Opcode::EQ, Max, Max);
  Unary(Opcode::ISZERO, Zero);
  Unary(Opcode::ISZERO, One);

  Binary(Opcode::AND, llvm::APInt(kWordBits, 0xf0f0),
         llvm::APInt(kWordBits, 0x0ff0));
  Binary(Opcode::OR, llvm::APInt(kWordBits, 0xf000),
         llvm::APInt(kWordBits, 0x0f0f));
  Binary(Opcode::XOR, llvm::APInt(kWordBits, 0xaaaa),
         llvm::APInt(kWordBits, 0x5555));
  Unary(Opcode::NOT, llvm::APInt(kWordBits, 0x1234));
  Binary(Opcode::BYTE, llvm::APInt(kWordBits, kWordBytes - 2),
         llvm::APInt(kWordBits, 0x1234));
  Binary(Opcode::BYTE, llvm::APInt(kWordBits, kWordBytes), Max);
  Binary(Opcode::SHL, llvm::APInt(kWordBits, 5),
         llvm::APInt(kWordBits, 0x1234));
  Binary(Opcode::SHR, llvm::APInt(kWordBits, 9), Max);
  Binary(Opcode::SAR, llvm::APInt(kWordBits, 9), Negative123);
  Binary(Opcode::SHL, llvm::APInt(kWordBits, kWordBits), One);
  Binary(Opcode::SHR, llvm::APInt(kWordBits, kWordBits), Max);
  Binary(Opcode::SAR, llvm::APInt(kWordBits, kWordBits), One);
  Binary(Opcode::SAR, llvm::APInt(kWordBits, kWordBits), NegativeOne);
  Unary(Opcode::CLZ, Zero);
  Unary(Opcode::CLZ, One);
  Unary(Opcode::CLZ, SignedMin);

  appendPush(Code, Zero);
  Code.push_back(opcodeByte(Opcode::SSTORE));
  Code.push_back(opcodeByte(Opcode::STOP));
  return Code;
}

TEST(EVMEmitters, ProduceValidEmptyPrograms) {
  EVMProgram Program;

  llvm::LLVMContext Context;
  auto Module = emitLLVM(Program, Context);
  ASSERT_TRUE(static_cast<bool>(Module)) << llvm::toString(Module.takeError());
  EXPECT_FALSE(llvm::verifyModule(**Module, &llvm::errs()));

  auto CSource = emitC(Program);
  ASSERT_TRUE(static_cast<bool>(CSource))
      << llvm::toString(CSource.takeError());
  auto SoliditySource = emitSolidity(Program);
  ASSERT_TRUE(static_cast<bool>(SoliditySource))
      << llvm::toString(SoliditySource.takeError());

  const char *Clang =
      std::filesystem::exists("/opt/homebrew/opt/llvm/bin/clang")
          ? "/opt/homebrew/opt/llvm/bin/clang"
          : "clang";
  if (std::system(
          (std::string("command -v ") + Clang + " >/dev/null 2>&1").c_str()) !=
          0 ||
      std::system("command -v solc >/dev/null 2>&1") != 0)
    GTEST_SKIP() << "clang or solc is unavailable";

  const auto CPath = writeTemporarySource("-empty.c", *CSource);
  const auto SolidityPath = writeTemporarySource("-empty.sol", *SoliditySource);
  const std::string CompileC = std::string(Clang) +
                               " -std=c2x -Werror -fsyntax-only -ffreestanding "
                               "-target x86_64-unknown-linux-gnu '" +
                               CPath.string() + "'";
  const std::string CompileSolidity =
      "solc --bin '" + SolidityPath.string() + "' >/dev/null 2>&1";
  EXPECT_EQ(std::system(CompileC.c_str()), 0);
  EXPECT_EQ(std::system(CompileSolidity.c_str()), 0);

  std::error_code EC;
  std::filesystem::remove(CPath, EC);
  std::filesystem::remove(SolidityPath, EC);
}

std::string differentialHarness(const llvm::APInt &ExpectedWord,
                                size_t ExpectedTraces,
                                bool UseLoweredCABI = false) {
  llvm::SmallString<80> Word;
  ExpectedWord.toStringUnsigned(Word, 10);
  std::string Harness;
  llvm::raw_string_ostream OS(Harness);
  OS << "@captured = internal global i" << kWordBits
     << " 0\n"
        "@trace_count = internal global i64 0\n\n"
        "declare i32 @"
     << kDefaultExecutionFunctionName << "(ptr)\n\n";
  if (UseLoweredCABI) {
    OS << "define void @" << kHostFunctionName << "(ptr sret(i" << kWordBits
       << ") %result, ptr %env, i8 %opcode";
    for (unsigned I = 0; I < maxHostOpcodeArguments(); ++I)
      OS << ", ptr byval(i" << kWordBits << ") %a" << I;
    OS << ") {\n"
          "entry:\n"
          "  %is_store = icmp eq i8 %opcode, "
       << static_cast<unsigned>(opcodeByte(Opcode::SSTORE))
       << "\n"
          "  br i1 %is_store, label %store, label %done\n"
          "store:\n"
          "  %value = load i"
       << kWordBits << ", ptr %a" << kSStoreValueArgumentIndex
       << "\n"
          "  store i"
       << kWordBits
       << " %value, ptr @captured\n"
          "  br label %done\n"
          "done:\n"
          "  store i"
       << kWordBits
       << " 0, ptr %result\n"
          "  ret void\n"
          "}\n\n";
  } else {
    OS << "define i" << kWordBits << " @" << kHostFunctionName
       << "(ptr %env, i8 %opcode";
    for (unsigned I = 0; I < maxHostOpcodeArguments(); ++I)
      OS << ", i" << kWordBits << " %a" << I;
    OS << ") {\n"
          "entry:\n"
          "  %is_store = icmp eq i8 %opcode, "
       << static_cast<unsigned>(opcodeByte(Opcode::SSTORE))
       << "\n"
          "  br i1 %is_store, label %store, label %done\n"
          "store:\n"
          "  store i"
       << kWordBits << " %a" << kSStoreValueArgumentIndex
       << ", ptr @captured\n"
          "  br label %done\n"
          "done:\n"
          "  ret i"
       << kWordBits
       << " 0\n"
          "}\n\n";
  }
  OS << "define void @" << kTraceFunctionName
     << "(ptr %env, i64 %pc, i8 %opcode) {\n"
        "entry:\n"
        "  %old = load i64, ptr @trace_count\n"
        "  %next = add i64 %old, 1\n"
        "  store i64 %next, ptr @trace_count\n"
        "  ret void\n"
        "}\n\n"
        "define i32 @main() {\n"
        "entry:\n"
        "  %status = call i32 @"
     << kDefaultExecutionFunctionName
     << "(ptr null)\n"
        "  %captured = load i"
     << kWordBits
     << ", ptr @captured\n"
        "  %traces = load i64, ptr @trace_count\n"
        "  %status_ok = icmp eq i32 %status, "
     << static_cast<unsigned>(exitStatusCode(ExitStatus::Stopped))
     << "\n"
        "  %value_ok = icmp eq i"
     << kWordBits << " %captured, " << Word
     << "\n"
        "  %trace_ok = icmp eq i64 %traces, "
     << ExpectedTraces
     << "\n"
        "  %first = and i1 %status_ok, %value_ok\n"
        "  %all = and i1 %first, %trace_ok\n"
        "  %exit = select i1 %all, i32 "
     << EXIT_SUCCESS << ", i32 " << EXIT_FAILURE
     << "\n"
        "  ret i32 %exit\n"
        "}\n";
  OS.flush();
  return Harness;
}

TEST(EVMLLVMEmitter, ExecutesDifferentiallyAgainstInterpreter) {
  const std::vector<uint8_t> Code = differentialALUProgram();
  auto Program = analyze(Code);
  ASSERT_TRUE(static_cast<bool>(Program))
      << llvm::toString(Program.takeError());
  auto Oracle = execute(Program->Low);
  ASSERT_TRUE(static_cast<bool>(Oracle)) << llvm::toString(Oracle.takeError());
  ASSERT_EQ(Oracle->Status, ExecutionStatus::Stopped);
  const auto Stored = Oracle->Storage.find(llvm::APInt(kWordBits, 0));
  ASSERT_NE(Stored, Oracle->Storage.end());

  llvm::LLVMContext Context;
  auto Module = emitLLVM(*Program, Context);
  ASSERT_TRUE(static_cast<bool>(Module)) << llvm::toString(Module.takeError());
  const auto ModulePath = writeTemporarySource(".ll", emitLLVMText(**Module));

  const std::string Harness =
      differentialHarness(Stored->second, Oracle->Trace.size());
  const auto HarnessPath = writeTemporarySource("-harness.ll", Harness);
  const auto ExecutablePath = writeTemporarySource("-runner", "");

  const char *Clang =
      std::filesystem::exists("/opt/homebrew/opt/llvm/bin/clang")
          ? "/opt/homebrew/opt/llvm/bin/clang"
          : "clang";
  if (std::system(
          (std::string("command -v ") + Clang + " >/dev/null 2>&1").c_str()) !=
      0)
    GTEST_SKIP() << "clang is unavailable";
  const std::string Compile = std::string(Clang) + " '" + ModulePath.string() +
                              "' '" + HarnessPath.string() + "' -o '" +
                              ExecutablePath.string() + "'";
  ASSERT_EQ(std::system(Compile.c_str()), 0);
  EXPECT_EQ(std::system(("'" + ExecutablePath.string() + "'").c_str()), 0);

  std::error_code EC;
  std::filesystem::remove(ModulePath, EC);
  std::filesystem::remove(HarnessPath, EC);
  std::filesystem::remove(ExecutablePath, EC);
}

TEST(EVMCEmitter, ProducesStandaloneCompilableC23) {
  const std::vector<uint8_t> Code = {0x60, 0x01, 0x60, 0x02, 0x60, 0x03, 0x09,
                                     0x34, 0x01, 0x60, 0x00, 0x55, 0x00};
  auto Program = analyze(Code);
  ASSERT_TRUE(static_cast<bool>(Program))
      << llvm::toString(Program.takeError());
  auto Source = emitC(*Program);
  ASSERT_TRUE(static_cast<bool>(Source)) << llvm::toString(Source.takeError());
  EXPECT_NE(Source->find("#define NEVERD_EVM_WORD_BITS 256u"),
            std::string::npos);
  EXPECT_NE(Source->find("#define NEVERD_EVM_WIDE_WORD_BITS 512u"),
            std::string::npos);
  EXPECT_NE(Source->find("evm_stack[NEVERD_EVM_STACK_LIMIT]"),
            std::string::npos);
  EXPECT_NE(Source->find("neverd_evm_host_op"), std::string::npos);
  EXPECT_NE(Source->find("pc_0:"), std::string::npos);
  EXPECT_NE(Source->find("evm_word negative = v >> NEVERD_EVM_WORD_MSB"),
            std::string::npos);
  EXPECT_EQ(Source->find("(evm_sword)v >>"), std::string::npos);

  const char *Clang =
      std::filesystem::exists("/opt/homebrew/opt/llvm/bin/clang")
          ? "/opt/homebrew/opt/llvm/bin/clang"
          : "clang";
  if (std::system(
          (std::string("command -v ") + Clang + " >/dev/null 2>&1").c_str()) !=
      0)
    GTEST_SKIP() << "clang is unavailable";
  const auto Path = writeTemporarySource(".c", *Source);
  const std::string Command = std::string(Clang) +
                              " -std=c2x -Werror -fsyntax-only -ffreestanding "
                              "-target x86_64-unknown-linux-gnu '" +
                              Path.string() + "'";
  EXPECT_EQ(std::system(Command.c_str()), 0);
  std::error_code EC;
  std::filesystem::remove(Path, EC);
}

TEST(EVMCEmitter, ExecutesDifferentiallyAgainstInterpreter) {
  const std::vector<uint8_t> Code = differentialALUProgram();
  auto Program = analyze(Code);
  ASSERT_TRUE(static_cast<bool>(Program))
      << llvm::toString(Program.takeError());
  auto Oracle = execute(Program->Low);
  ASSERT_TRUE(static_cast<bool>(Oracle)) << llvm::toString(Oracle.takeError());
  const auto Stored = Oracle->Storage.find(llvm::APInt(kWordBits, 0));
  ASSERT_NE(Stored, Oracle->Storage.end());

  auto Source = emitC(*Program);
  ASSERT_TRUE(static_cast<bool>(Source)) << llvm::toString(Source.takeError());
  const auto SourcePath = writeTemporarySource(".c", *Source);
  const auto ModulePath = writeTemporarySource("-from-c.ll", "");
  const auto HarnessPath = writeTemporarySource(
      "-c-harness.ll",
      differentialHarness(Stored->second, Oracle->Trace.size(), true));
  const auto ExecutablePath = writeTemporarySource("-c-runner", "");

  const char *Clang =
      std::filesystem::exists("/opt/homebrew/opt/llvm/bin/clang")
          ? "/opt/homebrew/opt/llvm/bin/clang"
          : "clang";
  if (std::system(
          (std::string("command -v ") + Clang + " >/dev/null 2>&1").c_str()) !=
      0)
    GTEST_SKIP() << "clang is unavailable";

  // Darwin currently caps frontend _BitInt at 128 bits. Parse the source
  // using a freestanding Linux target, then retarget the produced LLVM IR to
  // the native host for execution.
  const std::string Lower = std::string(Clang) +
                            " -std=c2x -Werror -ffreestanding -target "
                            "x86_64-unknown-linux-gnu -S -emit-llvm '" +
                            SourcePath.string() + "' -o '" +
                            ModulePath.string() + "'";
  ASSERT_EQ(std::system(Lower.c_str()), 0);
  const std::string Link = std::string(Clang) + " '" + ModulePath.string() +
                           "' '" + HarnessPath.string() + "' -o '" +
                           ExecutablePath.string() + "'";
  ASSERT_EQ(std::system(Link.c_str()), 0);
  EXPECT_EQ(std::system(("'" + ExecutablePath.string() + "'").c_str()), 0);

  std::error_code EC;
  std::filesystem::remove(SourcePath, EC);
  std::filesystem::remove(ModulePath, EC);
  std::filesystem::remove(HarnessPath, EC);
  std::filesystem::remove(ExecutablePath, EC);
}

TEST(EVMSolidityEmitter, ProducesCompilableRecoveredContractAndStateMachine) {
  const std::vector<uint8_t> Code = {
      0x60, 0x00, 0x35, 0x60, 0xe0, 0x1c, 0x80, 0x63, 0x12, 0x34,
      0x56, 0x78, 0x14, 0x60, 0x15, 0x57, 0x5b, 0x60, 0x00, 0x80,
      0xfd, 0x5b, 0x60, 0x2a, 0x60, 0x00, 0x52, 0x60, 0x20, 0x60,
      0x00, 0xf3, 0x60, 0x03, 0x54, 0x50, 0x00};
  auto Program = analyze(Code);
  ASSERT_TRUE(static_cast<bool>(Program))
      << llvm::toString(Program.takeError());
  auto Source = emitSolidity(*Program);
  ASSERT_TRUE(static_cast<bool>(Source)) << llvm::toString(Source.takeError());
  EXPECT_NE(Source->find("pragma solidity"), std::string::npos);
  EXPECT_NE(Source->find("abstract contract NeverDRecovered"),
            std::string::npos);
  EXPECT_NE(Source->find("selector 0x12345678"), std::string::npos);
  EXPECT_NE(Source->find("function func_12345678"), std::string::npos);
  EXPECT_NE(Source->find("uint256[1024] memory evmStack"), std::string::npos);
  EXPECT_NE(Source->find("constant recovered_storage_slot_3"),
            std::string::npos);
  EXPECT_NE(Source->find("error EVMInvalidJump"), std::string::npos);

  if (std::system("command -v solc >/dev/null 2>&1") != 0)
    GTEST_SKIP() << "solc is unavailable";
  const auto Path = writeTemporarySource(".sol", *Source);
  const std::string Command =
      "solc --bin '" + Path.string() + "' >/dev/null 2>&1";
  EXPECT_EQ(std::system(Command.c_str()), 0);
  std::error_code EC;
  std::filesystem::remove(Path, EC);
}

TEST(EVMSolidityEmitter, StopsOnFalseTerminalJumpWithoutJumpDestinations) {
  const std::vector<uint8_t> Code = {opcodeByte(Opcode::PUSH0),
                                     opcodeByte(Opcode::PUSH0),
                                     opcodeByte(Opcode::JUMPI)};
  auto Program = analyze(Code);
  ASSERT_TRUE(static_cast<bool>(Program))
      << llvm::toString(Program.takeError());
  auto Source = emitSolidity(*Program);
  ASSERT_TRUE(static_cast<bool>(Source)) << llvm::toString(Source.takeError());
  EXPECT_NE(Source->find("if (condition == 0) { return "
                         "NEVERD_EVM_STOPPED; }"),
            std::string::npos);

  if (std::system("command -v solc >/dev/null 2>&1") != 0)
    GTEST_SKIP() << "solc is unavailable";
  const auto Path = writeTemporarySource("-terminal-jump.sol", *Source);
  const std::string Command =
      "solc --bin '" + Path.string() + "' >/dev/null 2>&1";
  EXPECT_EQ(std::system(Command.c_str()), 0);
  std::error_code EC;
  std::filesystem::remove(Path, EC);
}

TEST(EVMSolidityEmitter, ExecutesDifferentiallyOnAnvil) {
#if defined(_WIN32)
  GTEST_SKIP() << "Anvil differential test requires a POSIX shell";
#else
  if (std::system("command -v solc >/dev/null 2>&1 && "
                  "command -v anvil >/dev/null 2>&1 && "
                  "command -v cast >/dev/null 2>&1 && "
                  "command -v jq >/dev/null 2>&1") != 0)
    GTEST_SKIP() << "solc, anvil, cast, or jq is unavailable";

  const std::vector<uint8_t> Code = differentialALUProgram();
  auto Program = analyze(Code);
  ASSERT_TRUE(static_cast<bool>(Program))
      << llvm::toString(Program.takeError());
  auto Oracle = execute(Program->Low);
  ASSERT_TRUE(static_cast<bool>(Oracle)) << llvm::toString(Oracle.takeError());
  const auto Stored = Oracle->Storage.find(llvm::APInt(kWordBits, 0));
  ASSERT_NE(Stored, Oracle->Storage.end());

  auto Source = emitSolidity(*Program);
  ASSERT_TRUE(static_cast<bool>(Source)) << llvm::toString(Source.takeError());
  std::string Harness;
  llvm::raw_string_ostream HarnessOS(Harness);
  HarnessOS << "\ncontract NeverDDifferentialHarness is "
            << kDefaultContractName
            << " {\n"
               "    uint256 public captured;\n\n"
               "    function _evmHost(uint8 opcode, uint256["
            << static_cast<unsigned>(maxHostOpcodeArguments())
            << "] memory args_, bytes memory)\n"
               "        internal override returns (uint256)\n"
               "    {\n"
               "        if (opcode == 0x"
            << llvm::utohexstr(opcodeByte(Opcode::SSTORE))
            << ") captured = args_[" << kSStoreValueArgumentIndex
            << "];\n"
               "        return 0;\n"
               "    }\n"
               "}\n";
  HarnessOS.flush();
  *Source += Harness;

  const auto SourcePath = writeTemporarySource("-anvil.sol", *Source);
  const auto ArtifactDirectory =
      SourcePath.parent_path() / (SourcePath.stem().string() + "-artifacts");
  std::filesystem::create_directories(ArtifactDirectory);
  const auto BinaryPath = ArtifactDirectory / "NeverDDifferentialHarness.bin";
  const unsigned Port =
      28545u + static_cast<unsigned>(
                   std::hash<std::string>{}(SourcePath.string()) % 10000u);
  const std::string URL = "http://127.0.0.1:" + std::to_string(Port);
  constexpr llvm::StringLiteral PrivateKey(
      "0xac0974bec39a17e36ba4a6b4d238ff944bacb478cbed5efcae784d7bf4f2ff80");
  llvm::SmallString<80> StoredDecimal;
  Stored->second.toStringUnsigned(StoredDecimal, 10);

  std::string Command =
      "set -eu; anvil --silent --port " + std::to_string(Port) +
      " >/dev/null 2>&1 & anvil_pid=$!; "
      "trap 'kill $anvil_pid >/dev/null 2>&1 || true' EXIT; "
      "ready=0; for attempt in 1 2 3 4 5 6 7 8 9 10 11 12 13 14 15 16 "
      "17 18 19 20; do if cast block-number --rpc-url '" +
      URL +
      "' >/dev/null 2>&1; then ready=1; break; fi; sleep 0.1; done; "
      "test $ready -eq 1; "
      "solc --bin '" +
      SourcePath.string() + "' -o '" + ArtifactDirectory.string() +
      "' --overwrite >/dev/null 2>&1; "
      "deploy_bin=$(tr -d '\\n' < '" +
      BinaryPath.string() +
      "'); "
      "address=$(cast send --rpc-url '" +
      URL + "' --private-key '" + PrivateKey.str() +
      "' --create \"$deploy_bin\" --json | jq -r .contractAddress); "
      "test \"$address\" != null; "
      "receipt=$(cast send \"$address\" 'execute(bytes)' 0x --rpc-url '" +
      URL + "' --private-key '" + PrivateKey.str() +
      "' --json); "
      "status=$(printf '%s' \"$receipt\" | jq -r .status); "
      "if test \"$status\" != 0x1; then printf 'transaction status: %s\\n' "
      "\"$status\" >&2; exit 1; fi; "
      "actual_logs=$(printf '%s' \"$receipt\" | jq '.logs | length'); "
      "if test \"$actual_logs\" -ne " +
      std::to_string(Oracle->Trace.size()) +
      "; then printf 'trace count: expected %s, got %s\\n' '" +
      std::to_string(Oracle->Trace.size()) +
      "' \"$actual_logs\" >&2; exit 1; fi; "
      "value=$(cast call \"$address\" 'captured()(uint256)' --rpc-url '" +
      URL + "' | awk '{print $1}'); if test \"$value\" != " +
      StoredDecimal.str().str() +
      "; then printf 'captured value: expected %s, got %s\\n' '" +
      StoredDecimal.str().str() + "' \"$value\" >&2; exit 1; fi;";
  EXPECT_EQ(std::system(Command.c_str()), 0);

  std::error_code EC;
  std::filesystem::remove(SourcePath, EC);
  std::filesystem::remove_all(ArtifactDirectory, EC);
#endif
}

} // namespace
} // namespace neverd::evm
