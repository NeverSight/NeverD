//===- LinuxSyscallTests.cpp - Linux supervisor call contracts ------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
//
// A Linux `svc #0` reads its number and argument registers and writes its
// result register.  HighC, LLVM IR and LLVM-C keep each value in its register,
// and a syscall whose arity is verified reads only its own arguments.
//
//===----------------------------------------------------------------------===//

#include "NeverDLiftFixture.h"

#include "neverd/ir/intrinsics/X86/X64Syscall.h"

#include <fstream>
#include <sstream>

namespace {

fs::path a64Obj() { return fs::path(TEST_OBJ_DIR) / "test_linux_svc_a64.o"; }
fs::path armObj() { return fs::path(TEST_OBJ_DIR) / "test_linux_svc_arm.o"; }
fs::path exitAfterCallObj() {
  return fs::path(TEST_OBJ_DIR) / "test_call_arg_phi_a64_elf.o";
}

std::string readText(const fs::path &Path) {
  std::ifstream In(Path);
  std::ostringstream Text;
  Text << In.rdbuf();
  return Text.str();
}

/// The body of function \p Name in C source \p C.
std::string cFunction(const std::string &C, const std::string &Name) {
  const size_t Start = C.find(" " + Name + "(");
  if (Start == std::string::npos)
    return "";
  return C.substr(Start, C.find("\n}", Start) - Start);
}

} // namespace

// The arities the kernel's handlers take: one register per argument, and on
// ARM an even-aligned pair per 64-bit one.
TEST(LinuxSyscallArguments, CountTheKernelHandlersRegisters) {
  using neverd::Arch;
  using neverd::linuxSyscallArgumentCount;
  EXPECT_EQ(linuxSyscallArgumentCount(Arch::X64, 0), 3u);       // read
  EXPECT_EQ(linuxSyscallArgumentCount(Arch::X64, 39), 0u);      // getpid
  EXPECT_EQ(linuxSyscallArgumentCount(Arch::X64, 9), 6u);       // mmap
  EXPECT_EQ(linuxSyscallArgumentCount(Arch::AArch64, 56), 4u);  // openat
  EXPECT_EQ(linuxSyscallArgumentCount(Arch::AArch64, 172), 0u); // getpid
  EXPECT_EQ(linuxSyscallArgumentCount(Arch::ARM, 180), 6u);     // pread64
  EXPECT_EQ(linuxSyscallArgumentCount(Arch::ARM, 194), 4u);     // ftruncate64
  EXPECT_EQ(linuxSyscallArgumentCount(Arch::ARM, 192), 6u);     // mmap2
  EXPECT_EQ(linuxSyscallArgumentCount(Arch::X64, 100000), std::nullopt);
  // RDX:RSI and R8:R10 are read whole once either half is.
  EXPECT_EQ(neverd::x64SyscallRegistersRead(2), 3u);
  EXPECT_EQ(neverd::x64SyscallRegistersRead(4), 5u);
}

// Linux 7.2 at 8d3ae59288f1e7d58d76558a6ee96d533bc5019f.
// These calls have no by-value 64-bit scalar on ARM EABI: u64 pointers
// (listmount, lsm_list_modules, listns) still occupy one register.
TEST(LinuxSyscallArguments, Linux72IncrementalMetadata) {
  using neverd::Arch;
  using neverd::linuxSyscallArgumentCount;
  constexpr unsigned Counts[] = {4, 4, 3, 4, 6, 4, 4, 4, 4, 4, 3,
                                 3, 6, 6, 5, 4, 5, 5, 5, 4, 0};
  for (const Arch Target : {Arch::X64, Arch::AArch64, Arch::ARM}) {
    SCOPED_TRACE(static_cast<unsigned>(Target));
    for (unsigned I = 0; I < sizeof(Counts) / sizeof(Counts[0]); ++I) {
      SCOPED_TRACE(451 + I);
      EXPECT_EQ(linuxSyscallArgumentCount(Target, 451 + I), Counts[I]);
    }
    EXPECT_EQ(linuxSyscallArgumentCount(Target, 473), std::nullopt);
    EXPECT_EQ(linuxSyscallArgumentCount(Target, 100000), std::nullopt);
  }
  // The syscall is SYSCALL_DEFINE5(bpf), not the three-argument internal
  // BPF helper. Keeping five inputs is conservative for older kernels.
  EXPECT_EQ(linuxSyscallArgumentCount(Arch::X64, 321), 5u);
  EXPECT_EQ(linuxSyscallArgumentCount(Arch::AArch64, 280), 5u);
  EXPECT_EQ(linuxSyscallArgumentCount(Arch::ARM, 386), 5u);
  // Architecture-specific holes must not inherit another ABI's definition.
  EXPECT_EQ(linuxSyscallArgumentCount(Arch::ARM, 447), std::nullopt);
  // Native x86-64 metadata must not silently adopt x32 entries.
  EXPECT_EQ(linuxSyscallArgumentCount(Arch::X64, 512), std::nullopt);
}

// fchroot(int fd, unsigned int flags) has two word-sized arguments on all
// three ABIs. Linux cbbddb6b08cc9900a87cccaa9b22bbe06aa4f50a assigns 472.
TEST(LinuxSyscallArguments, FchrootMetadata) {
  for (const neverd::Arch Target :
       {neverd::Arch::X64, neverd::Arch::AArch64, neverd::Arch::ARM}) {
    EXPECT_EQ(neverd::linuxSyscallArgumentCount(Target, 472), 2u);
    EXPECT_EQ(neverd::linuxSyscallArgumentCount(Target, 473), std::nullopt);
  }
}

TEST(LinuxSyscallArguments, AArch64NativeNotCompat) {
  using neverd::Arch;
  using neverd::linuxSyscallArgumentCount;
  constexpr unsigned Numbers[] = {43,  44,  45,  46,  84,  86,  87,  99, 100,
                                  115, 122, 123, 127, 137, 138, 183, 240};
  constexpr unsigned Counts[] = {2, 2, 2, 2, 4, 4, 2, 2, 3,
                                 4, 3, 3, 2, 4, 3, 5, 4};
  for (unsigned I = 0; I < sizeof(Numbers) / sizeof(Numbers[0]); ++I) {
    SCOPED_TRACE(Numbers[I]);
    EXPECT_EQ(linuxSyscallArgumentCount(Arch::AArch64, Numbers[I]), Counts[I]);
  }
  for (unsigned Number = 403; Number <= 423; ++Number) {
    SCOPED_TRACE(Number);
    EXPECT_EQ(linuxSyscallArgumentCount(Arch::AArch64, Number), std::nullopt);
  }
  // These really are assigned in the 32-bit EABI and must remain intact.
  EXPECT_EQ(linuxSyscallArgumentCount(Arch::ARM, 403), 2u);
  EXPECT_EQ(linuxSyscallArgumentCount(Arch::ARM, 422), 6u);
  // A removed kernel implementation does not erase the historical ABI.
  EXPECT_EQ(linuxSyscallArgumentCount(Arch::AArch64, 18), 3u);
  EXPECT_EQ(linuxSyscallArgumentCount(Arch::ARM, 249), 4u);
}

class LinuxSyscall : public NeverDLiftTest {
protected:
  std::string highC(const fs::path &Object) {
    const auto R = decompileToHighC(Object);
    EXPECT_EQ(R.exitCode, 0) << R.err;
    return readText(tmpFile("decompiled_high.c"));
  }
};

TEST_F(LinuxSyscall, AArch64CallsPassTheirRegisters) {
  const std::string C = highC(a64Obj());
  EXPECT_NE(cFunction(C, "a64_linux_exit_group")
                .find("neverd_a64_linux_syscall(0x5E, 5, 0, 0, 0, 0, 0)"),
            std::string::npos)
      << C;
  EXPECT_NE(cFunction(C, "a64_linux_six")
                .find("neverd_a64_linux_syscall(0x5AB, 0x14, 0x15, 0x16, 0x17, "
                      "0x18, 0x19)"),
            std::string::npos)
      << C;
  EXPECT_EQ(cFunction(C, "a64_other_immediate").find("linux_syscall"),
            std::string::npos)
      << C;
  EXPECT_NE(C.find("register uint64_t in_number __asm__(\"x8\") = number;"),
            std::string::npos)
      << C;
  EXPECT_NE(C.find("__asm__ volatile(\"svc #0\""), std::string::npos) << C;
}

TEST_F(LinuxSyscall, ARMCallsPassTheirRegisters) {
  const std::string C = highC(armObj());
  EXPECT_NE(cFunction(C, "arm_linux_exit")
                .find("neverd_arm_linux_syscall(1, 3, 0, 0, 0, 0, 0, 0)"),
            std::string::npos)
      << C;
  EXPECT_NE(
      cFunction(C, "arm_linux_seven")
          .find("neverd_arm_linux_syscall(0x5AB, 10, 0xB, 0xC, 0xD, 0xE, 0xF, "
                "0x10)"),
      std::string::npos)
      << C;
  EXPECT_EQ(cFunction(C, "arm_oabi_call").find("linux_syscall"),
            std::string::npos)
      << C;
  EXPECT_NE(C.find("register uint32_t in_number __asm__(\"r7\") = number;"),
            std::string::npos)
      << C;
}

// A syscall that reads a verified number of arguments takes no parameter from
// the registers it ignores.
TEST_F(LinuxSyscall, AnExitReadsOnlyItsStatus) {
  const std::string C = highC(a64Obj());
  EXPECT_NE(C.find("void a64_linux_exit_group(void)"), std::string::npos) << C;
}

// exit after a call takes the call's result: it is a value HighC renders,
// not an SVC result it must refuse.
TEST_F(LinuxSyscall, AnExitAfterACallDecompiles) {
  const std::string C = highC(exitAfterCallObj());
  EXPECT_NE(C.find("neverd_a64_linux_syscall(0x5D, "), std::string::npos) << C;
}

TEST_F(LinuxSyscall, LLVMIRBindsEachRegister) {
  const auto A64 = liftToLLVMIR(a64Obj());
  ASSERT_EQ(A64.exitCode, 0) << A64.err;
  EXPECT_TRUE(
      A64.contains("asm sideeffect \"svc #0\", "
                   "\"={x0},{x8},0,{x1},{x2},{x3},{x4},{x5},~{memory}\""))
      << A64.out;
  const auto ARM = liftToLLVMIR(armObj());
  ASSERT_EQ(ARM.exitCode, 0) << ARM.err;
  EXPECT_TRUE(
      ARM.contains("asm sideeffect \"svc #0\", "
                   "\"={r0},{r7},0,{r1},{r2},{r3},{r4},{r5},{r6},~{memory}\""))
      << ARM.out;
}

TEST_F(LinuxSyscall, LLVMCCallsTheHelper) {
  const auto R = decompileToC(a64Obj());
  ASSERT_EQ(R.exitCode, 0) << R.err;
  const std::string C = readText(tmpFile("decompiled.c"));
  EXPECT_NE(C.find("static inline uint64_t neverd_a64_linux_syscall("),
            std::string::npos)
      << C;
  EXPECT_NE(C.find("neverd_a64_linux_syscall(0x5AB, "), std::string::npos) << C;
}

// The helpers build for the system the code was lifted from.
TEST_F(LinuxSyscall, TheCBuildsForItsTarget) {
  if (!hasCrossTargetClang())
    GTEST_SKIP() << "cross-target compilation requires Clang";
  const std::pair<fs::path, const char *> Cases[] = {
      {a64Obj(), "aarch64-linux-gnu"}, {armObj(), "armv7-linux-gnueabi"}};
  for (const auto &[Object, Target] : Cases) {
    SCOPED_TRACE(Target);
    const auto Source = tmpFile(std::string("syscall-") + Target + ".c");
    std::ofstream(Source) << highC(Object);
    const auto Built =
        exec(NEVERD_TEST_CLANG, {"-target", Target, "-fsyntax-only",
                                 "-fno-strict-aliasing", Source.string()});
    EXPECT_EQ(Built.exitCode, 0) << Built.err;
  }
}

// An image for another operating system gives `svc #0` no Linux contract.
TEST_F(LinuxSyscall, AnotherSystemKeepsTheBareCall) {
  const auto Object = tmpFile("freebsd-svc.o");
  fs::copy_file(armObj(), Object, fs::copy_options::overwrite_existing);
  {
    std::fstream File(Object, std::ios::in | std::ios::out | std::ios::binary);
    File.seekp(7);                  // e_ident[EI_OSABI]
    File.put(static_cast<char>(9)); // ELFOSABI_FREEBSD
  }
  const std::string C = highC(Object);
  EXPECT_EQ(C.find("neverd_arm_linux_syscall"), std::string::npos) << C;
}
