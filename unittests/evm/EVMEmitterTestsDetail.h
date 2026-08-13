//===- EVMEmitterTestsDetail.h - Shared EVM backend test harness --------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// The programs, the generated-code harness and the external-tool plumbing the
/// per-backend emitter tests are all written against.
///
//===----------------------------------------------------------------------===//

#ifndef NEVERD_UNITTESTS_EVM_EVMEMITTERTESTSDETAIL_H
#define NEVERD_UNITTESTS_EVM_EVMEMITTERTESTSDETAIL_H

#include "gtest/gtest.h"

#include "neverd/evm/analysis/EVMAnalyzer.h"
#include "neverd/evm/emit/EVMCEmitter.h"
#include "neverd/evm/runtime/EVMInterpreter.h"
#include "neverd/evm/emit/EVMLLVMEmitter.h"
#include "neverd/evm/emit/EVMSolidityEmitter.h"

#include "llvm/ADT/ArrayRef.h"
#include "llvm/ADT/SmallString.h"
#include "llvm/Support/Error.h"
#include "llvm/Support/FileSystem.h"
#include "llvm/Support/raw_ostream.h"

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <functional>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace neverd::evm::test {

inline constexpr unsigned kSStoreValueArgumentIndex = 1;
inline constexpr unsigned kAnvilBasePort = 28545;
inline constexpr unsigned kAnvilPortSpan = 10000;
inline constexpr uint8_t kDifferentialMemoryOffset = 1;
inline constexpr llvm::StringLiteral kAnvilPrivateKey =
    "0xac0974bec39a17e36ba4a6b4d238ff944bacb478cbed5efcae784d7bf4f2ff80";
inline constexpr llvm::StringLiteral kAnvilTestAddress =
    "0x1000000000000000000000000000000000000000";

inline std::filesystem::path writeTemporarySource(llvm::StringRef Extension,
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

inline void appendPush(std::vector<uint8_t> &Code, const llvm::APInt &Value) {
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

inline std::vector<uint8_t> differentialALUProgram(bool IncludeFusaka = true) {
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
  if (IncludeFusaka) {
    Unary(Opcode::CLZ, Zero);
    Unary(Opcode::CLZ, One);
    Unary(Opcode::CLZ, SignedMin);
  }

  appendPush(Code, Zero);
  Code.push_back(opcodeByte(Opcode::SSTORE));
  Code.push_back(opcodeByte(Opcode::STOP));
  return Code;
}

inline std::vector<uint8_t> differentialMemoryProgram() {
  static_assert(kWordBytes <= kByteMax);
  return {
      opcodeByte(Opcode::CALLDATASIZE), opcodeByte(Opcode::PUSH0),
      opcodeByte(Opcode::PUSH0),        opcodeByte(Opcode::CALLDATACOPY),
      opcodeByte(Opcode::CALLDATASIZE), opcodeByte(Opcode::PUSH0),
      opcodeByte(Opcode::PUSH1),        kDifferentialMemoryOffset,
      opcodeByte(Opcode::MCOPY),        opcodeByte(Opcode::CALLDATASIZE),
      opcodeByte(Opcode::PUSH1),        kDifferentialMemoryOffset,
      opcodeByte(Opcode::SHA3),         opcodeByte(Opcode::PUSH0),
      opcodeByte(Opcode::MSTORE),       opcodeByte(Opcode::PUSH1),
      static_cast<uint8_t>(kWordBytes), opcodeByte(Opcode::PUSH0),
      opcodeByte(Opcode::RETURN),
  };
}

inline std::vector<uint8_t> amsterdamDifferentialProgram() {
  std::vector<uint8_t> Code{opcodeByte(Opcode::SLOTNUM)};
  for (unsigned Value = 1; Value <= 20; ++Value)
    appendPush(Code, llvm::APInt(kWordBits, Value));

  Code.insert(Code.end(), {
                              opcodeByte(Opcode::DUPN),
                              0x80,
                              opcodeByte(Opcode::SWAPN),
                              0x81,
                              opcodeByte(Opcode::EXCHANGE),
                              0x9d,
                          });
  // Use a non-commutative fold so incorrect SWAPN or EXCHANGE lowering changes
  // the observable storage value rather than merely permuting dead stack data.
  for (unsigned RemainingValues = 22; RemainingValues > 1; --RemainingValues)
    Code.push_back(opcodeByte(Opcode::SUB));
  Code.insert(Code.end(),
              {opcodeByte(Opcode::PUSH0), opcodeByte(Opcode::SSTORE),
               opcodeByte(Opcode::STOP)});
  return Code;
}

inline std::string bytecodeHex(llvm::ArrayRef<uint8_t> Code) {
  std::string Result = "0x";
  Result.reserve(Result.size() + Code.size() * kHexDigitsPerByte);
  for (uint8_t Byte : Code)
    Result += llvm::utohexstr(Byte, /*LowerCase=*/true, kHexDigitsPerByte);
  return Result;
}

inline std::string wordHex(const llvm::APInt &Value) {
  llvm::SmallString<kWordBytes * kHexDigitsPerByte> Digits;
  Value.toStringUnsigned(Digits, kHexRadix);
  const std::string LowerDigits = Digits.str().lower();
  return "0x" +
         std::string(kWordBytes * kHexDigitsPerByte - LowerDigits.size(), '0') +
         LowerDigits;
}

inline unsigned anvilPort(const std::filesystem::path &UniquePath) {
  return kAnvilBasePort +
         static_cast<unsigned>(std::hash<std::string>{}(UniquePath.string()) %
                               kAnvilPortSpan);
}

inline std::string
differentialHarness(const llvm::APInt &ExpectedWord, size_t ExpectedTraces,
                    bool UseLoweredCABI = false,
                    std::optional<llvm::APInt> SlotNumber = std::nullopt) {
  llvm::SmallString<80> Word;
  ExpectedWord.toStringUnsigned(Word, 10);
  llvm::SmallString<80> Slot;
  SlotNumber.value_or(llvm::APInt(kWordBits, 0)).toStringUnsigned(Slot, 10);
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
          "  %is_slot = icmp eq i8 %opcode, "
       << static_cast<unsigned>(opcodeByte(Opcode::SLOTNUM))
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
          "  %host_result = select i1 %is_slot, i"
       << kWordBits << " " << Slot << ", i" << kWordBits
       << " 0\n"
          "  store i"
       << kWordBits
       << " %host_result, ptr %result\n"
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
          "  %is_slot = icmp eq i8 %opcode, "
       << static_cast<unsigned>(opcodeByte(Opcode::SLOTNUM))
       << "\n"
          "  br i1 %is_store, label %store, label %done\n"
          "store:\n"
          "  store i"
       << kWordBits << " %a" << kSStoreValueArgumentIndex
       << ", ptr @captured\n"
          "  br label %done\n"
          "done:\n"
          "  %host_result = select i1 %is_slot, i"
       << kWordBits << " " << Slot << ", i" << kWordBits
       << " 0\n"
          "  ret i"
       << kWordBits
       << " %host_result\n"
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

} // namespace neverd::evm::test

#endif // NEVERD_UNITTESTS_EVM_EVMEMITTERTESTSDETAIL_H
