//===- X86PortSourceTests.cpp - Unsigned port operand transport
//------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Compile and execute the C projections of immediate and DX port accesses
/// against independent recorder functions. These checks observe operand
/// transport without executing privileged port I/O.
///
//===----------------------------------------------------------------------===//

#include "gtest/gtest.h"

#include "neverd/backend/c/CEmitterOptions.h"
#include "neverd/backend/c/LLVMC/LLVMCEmitter.h"

#include "llvm/ADT/ScopeExit.h"
#include "llvm/ADT/SmallString.h"
#include "llvm/AsmParser/Parser.h"
#include "llvm/IR/LLVMContext.h"
#include "llvm/IR/Module.h"
#include "llvm/Support/FileSystem.h"
#include "llvm/Support/MemoryBuffer.h"
#include "llvm/Support/Program.h"
#include "llvm/Support/SourceMgr.h"
#include "llvm/Support/raw_ostream.h"
#include "llvm/TargetParser/Host.h"
#include "llvm/TargetParser/Triple.h"

#include <cstdint>
#include <cstdlib>
#include <optional>
#include <string>
#include <vector>

using namespace neverd;

namespace {

constexpr unsigned kCompilerTimeoutSeconds = 60;
constexpr uint32_t kValue = UINT32_C(0x87654321);

struct PortCase {
  uint16_t Port;
  unsigned Bytes;
  bool Write;
  bool Immediate;
};

std::vector<PortCase> cases() {
  std::vector<PortCase> Result;
  for (unsigned Bytes : {1u, 2u, 4u})
    for (bool Write : {false, true}) {
      for (unsigned Port = 0; Port <= UINT8_MAX; ++Port)
        Result.push_back({uint16_t(Port), Bytes, Write, true});
      for (uint16_t Port : {0, 1, 127, 128, 255, 256, 0xff80, 0xffff})
        Result.push_back({Port, Bytes, Write, false});
    }
  return Result;
}

uint32_t data(unsigned Bytes) {
  return Bytes == 4 ? kValue : kValue & ((UINT32_C(1) << (Bytes * 8)) - 1);
}

std::string ir(const std::vector<PortCase> &Cases) {
  std::string Result;
  for (unsigned Index = 0; Index != Cases.size(); ++Index) {
    const auto &Case = Cases[Index];
    const std::string Type = "i" + std::to_string(Case.Bytes * 8);
    const std::string Port = std::string(Case.Immediate ? "i8 " : "i16 ") +
                             std::to_string(Case.Port);
    const std::string Constraint = Case.Immediate ? "N" : "{dx}";
    const char Suffix = Case.Bytes == 1 ? 'b' : Case.Bytes == 2 ? 'w' : 'l';
    Result += "define " + (Case.Write ? std::string("void") : Type) +
              " @port_case_" + std::to_string(Index) + "() {\n";
    if (Case.Write)
      Result += "call void asm sideeffect \"out" + std::string(1, Suffix) +
                " $0, $1\", \"{ax}," + Constraint + ",~{memory}\"(" + Type +
                " " + std::to_string(data(Case.Bytes)) + ", " + Port +
                ")\nret void\n";
    else
      Result += "%value = call " + Type + " asm sideeffect \"in" +
                std::string(1, Suffix) + " $1, $0\", \"={ax}," + Constraint +
                ",~{memory}\"(" + Port + ")\nret " + Type + " %value\n";
    Result += "}\n";
  }
  return Result;
}

std::string observers(bool Native) {
  std::string Result = R"(
static unsigned expected_port, expected_bytes, expected_write, seen, failed;
static unsigned port_read(unsigned port, unsigned bytes) {
  failed |= port != expected_port || bytes != expected_bytes || expected_write;
  ++seen;
  return bytes == 1 ? 0x21u : bytes == 2 ? 0x4321u : 0x87654321u;
}
static void port_write(unsigned port, unsigned value, unsigned bytes) {
  unsigned wanted = bytes == 1 ? 0x21u : bytes == 2 ? 0x4321u : 0x87654321u;
  failed |= port != expected_port || bytes != expected_bytes || !expected_write
            || value != wanted;
  ++seen;
}
#define __builtin_ia32_inbyte(p) port_read((p), 1)
#define __builtin_ia32_inword(p) port_read((p), 2)
#define __builtin_ia32_indword(p) port_read((p), 4)
#define __builtin_ia32_outbyte(p, v) port_write((p), (v), 1)
#define __builtin_ia32_outword(p, v) port_write((p), (v), 2)
#define __builtin_ia32_outdword(p, v) port_write((p), (v), 4)
#define __inbyte(p) port_read((p), 1)
#define __inword(p) port_read((p), 2)
#define __indword(p) port_read((p), 4)
#define __outbyte(p, v) port_write((p), (v), 1)
#define __outword(p, v) port_write((p), (v), 2)
#define __outdword(p, v) port_write((p), (v), 4)
)";
  Result += Native
                ? "#define NEVERD_X86_SCALAR_SYSTEM(native, msvc, ...) native\n"
                : "#define NEVERD_X86_SCALAR_SYSTEM(native, msvc, ...) msvc\n";
  return Result;
}

std::string driver(Arch Architecture, const std::vector<PortCase> &Cases) {
  std::string Result = "static int check(void) {\n";
  for (unsigned Index = 0; Index != Cases.size(); ++Index) {
    const auto &Case = Cases[Index];
    Result += "expected_port=" + std::to_string(Case.Port) +
              "; expected_bytes=" + std::to_string(Case.Bytes) +
              "; expected_write=" + std::to_string(Case.Write) + "; seen=0;\n";
    if (!Case.Write)
      Result += "if (";
    Result += "port_case_" + std::to_string(Index) + "()";
    Result += Case.Write ? ";\n"
                         : " != " + std::to_string(data(Case.Bytes)) +
                               "u) return 1;\n";
    Result += "if (seen != 1 || failed) return 2;\n";
  }
  Result += "return 0;\n}\n";
  if (Architecture == Arch::X86)
    Result += R"(
__attribute__((noreturn)) void _start(void) {
  int status = check();
  __asm__ volatile("int $0x80" : : "a"(1), "b"(status) : "memory");
  __builtin_unreachable();
}
)";
  else
    Result += "int main(void) { return check(); }\n";
  return Result;
}

void writeFile(const std::string &Path, llvm::StringRef Text) {
  std::error_code Error;
  llvm::raw_fd_ostream Stream(Path, Error);
  ASSERT_FALSE(Error);
  Stream << Text;
}

void execute(const std::string &Compiler, const std::string &Directory,
             Arch Architecture, llvm::StringRef Level, llvm::StringRef Source,
             bool NeverC, unsigned &Programs) {
  const auto Input = Directory + "/ports.c";
  const auto Output = Directory + "/ports";
  const auto Errors = Directory + "/errors.txt";
  writeFile(Input, Source);
  std::vector<llvm::StringRef> Args{Compiler, "-std=gnu23", Level,
                                    Input,    "-o",         Output};
  if (Architecture == Arch::X86)
    Args.insert(Args.end(), {"-m32", "-ffreestanding", "-nostdlib", "-static",
                             "-Wl,-e,_start"});
  if (NeverC)
    Args.insert(Args.end(), {"-fno-lto", "-fno-builtin-mimalloc"});
  const std::optional<llvm::StringRef> Redirects[] = {std::nullopt,
                                                      std::nullopt, Errors};
  const int Status = llvm::sys::ExecuteAndWait(
      Compiler, Args, std::nullopt, Redirects, kCompilerTimeoutSeconds);
  const auto Diagnostics = llvm::MemoryBuffer::getFile(Errors);
  ASSERT_EQ(Status, 0) << (Diagnostics ? (*Diagnostics)->getBuffer().str()
                                       : "");
  ASSERT_EQ(llvm::sys::ExecuteAndWait(Output, {Output}, std::nullopt, Redirects,
                                      kCompilerTimeoutSeconds),
            0)
      << "port width or data operand changed in compiled C";
  ++Programs;
}

TEST(X86PortSource, ImmediateAndDxSelectorsKeepTheirUnsignedWidths) {
  const llvm::Triple Host(llvm::sys::getDefaultTargetTriple());
  if (Host.getArch() != llvm::Triple::x86_64 || !Host.isOSLinux())
    GTEST_SKIP() << "native x64 and freestanding i386 probes require Linux x64";
  const auto Clang = llvm::sys::findProgramByName("clang");
  ASSERT_TRUE(Clang) << "clang is required to compile the operand observers";
  std::vector<std::string> Compilers{*Clang};
  if (auto GCC = llvm::sys::findProgramByName("gcc"))
    Compilers.push_back(*GCC);
  llvm::SmallString<128> Temporary;
  ASSERT_FALSE(
      llvm::sys::fs::createUniqueDirectory("neverd-port-source", Temporary));
  const auto Cleanup = llvm::scope_exit(
      [&] { (void)llvm::sys::fs::remove_directories(Temporary); });
  const std::string Directory(Temporary.str());
  const auto Cases = cases();
  unsigned Programs = 0;
  for (Arch Architecture : {Arch::X86, Arch::X64}) {
    llvm::LLVMContext Context;
    llvm::SMDiagnostic Diagnostic;
    auto Module = llvm::parseAssemblyString(ir(Cases), Diagnostic, Context);
    ASSERT_TRUE(Module) << Diagnostic.getMessage().str();
    CEmitterOptions Options;
    Options.TheArch = Architecture;
    Options.Format = BinaryFormat::ELF;
    Options.PreserveLLVMFunctionTypes = true;
    std::string Source;
    llvm::raw_string_ostream OS(Source);
    ASSERT_TRUE(LLVMCEmitter().emit(*Module, OS, Options));
    for (bool Native : {false, true}) {
      const auto Program =
          observers(Native) + Source + driver(Architecture, Cases);
      for (const auto &Compiler : Compilers)
        for (llvm::StringRef Level : {"-O0", "-O2"}) {
          SCOPED_TRACE(Compiler + " " + Level.str() + " " +
                       std::to_string(unsigned(Architecture)) + " " +
                       std::to_string(Native));
          execute(Compiler, Directory, Architecture, Level, Program, false,
                  Programs);
          ASSERT_FALSE(::testing::Test::HasFailure());
        }
      if (const char *NeverC = std::getenv("NEVERD_TEST_NEVERC");
          Architecture == Arch::X64 && NeverC)
        for (llvm::StringRef Level : {"-O0", "-O2"}) {
          execute(NeverC, Directory, Architecture, Level, Program, true,
                  Programs);
          ASSERT_FALSE(::testing::Test::HasFailure());
        }
      if (const char *Exports = std::getenv("NEVERD_PORT_SOURCE_EXPORT_DIR")) {
        ASSERT_FALSE(llvm::sys::fs::create_directories(Exports));
        const std::string Stem = std::string(Exports) +
                                 (Architecture == Arch::X86 ? "/32" : "/64") +
                                 (Native ? "-native" : "-msvc");
        writeFile(Stem + ".c", Program);
      }
    }
  }
  RecordProperty("operand_cases_per_program", Cases.size());
  RecordProperty("compiled_observer_programs", Programs);
}

} // namespace
