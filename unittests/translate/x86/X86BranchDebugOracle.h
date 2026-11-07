//===- X86BranchDebugOracle.h - Original branch stepping ------*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Linux ptrace owns #DB entry and TF normalization. This oracle compares only
/// original branch selection and completed GPR/stack changes, not raw frames,
/// DR6/DEBUGCTL, kernel execution or task/transactional debug.
//===----------------------------------------------------------------------===//
#ifndef NEVERD_UNITTESTS_TRANSLATE_X86_X86BRANCHDEBUGORACLE_H
#define NEVERD_UNITTESTS_TRANSLATE_X86_X86BRANCHDEBUGORACLE_H

#include "gtest/gtest.h"

#include <algorithm>
#include <cstdint>
#include <vector>

#if defined(__linux__) && defined(__x86_64__)
#include <asm/ldt.h>
#include <cerrno>
#include <chrono>
#include <csignal>
#include <cstring>
#include <sys/mman.h>
#include <sys/ptrace.h>
#include <sys/syscall.h>
#include <sys/user.h>
#include <sys/wait.h>
#include <unistd.h>

namespace neverd::test {

struct X86BranchDebugCase {
  std::vector<uint8_t> Code;
  /// Authenticated instruction extents up to the first taken branch.
  std::vector<unsigned> Lengths;
  uint64_t Flags = 0x202, Count = 0;
};

inline std::vector<X86BranchDebugCase> x86BranchDebugCases(bool Code16) {
  return {
      {Code16 ? std::vector<uint8_t>{0x90, 0xb8, 0x34, 0x12, 0xeb, 2, 0x0f,
                                     0x0b, 0xcc}
              : std::vector<uint8_t>{0x90, 0xb8, 0x34, 0x12, 0, 0, 0xeb, 2,
                                     0x0f, 0x0b, 0xcc},
       {1, Code16 ? 3u : 5u, 2}},
      {{0xeb, 0, 0x90, 0xcc}, {2}},
      {{0x74, 2, 0x0f, 0x0b, 0x90, 0xcc}, {2}, 0x242},
      {{0x74, 2, 0x90, 0xeb, 2, 0x0f, 0x0b, 0xcc}, {2, 1, 2}},
      {{0x74, 0, 0x90, 0xcc}, {2}, 0x242},
      {{0x74, 0, 0x90, 0xeb, 0, 0xcc}, {2, 1, 2}},
      {{0xe2, 2, 0x0f, 0x0b, 0x90, 0xcc}, {2}, 0x202, 2},
      {{0xe2, 2, 0x90, 0xeb, 0, 0xcc}, {2, 1, 2}, 0x202, 1},
      {{0xe1, 0, 0x90, 0xcc}, {2}, 0x242, 2},
      {{0xe1, 0, 0x90, 0xeb, 0, 0xcc}, {2, 1, 2}, 0x202, 2},
      {{0xe0, 0, 0x90, 0xcc}, {2}, 0x202, 2},
      {{0xe0, 0, 0x90, 0xeb, 0, 0xcc}, {2, 1, 2}, 0x242, 2},
      {Code16 ? std::vector<uint8_t>{0xe8, 3, 0, 0x0f, 0x0b, 0xcc, 0x90, 0xcc}
              : std::vector<uint8_t>{0xe8, 3, 0, 0, 0, 0x0f, 0x0b, 0xcc, 0x90,
                                     0xcc},
       {Code16 ? 3u : 5u}},
      {{0xc3, 0x0f, 0x0b, 0xcc, 0x90, 0xcc}, {1}},
      {{0xe3, 0, 0x90, 0xcc}, {2}},
      {{0xe3, 0, 0x90, 0xeb, 0, 0xcc}, {2, 1, 2}, 0x202, 1},
  };
}

class X86BranchDebugOracle {
public:
  enum class Mode { Long64, Code32, Code16 };
  struct Result {
    uint64_t PC = 0, Accumulator = 0, Count = 0, Flags = 0;
    int64_t StackDelta = 0;
  };

  ~X86BranchDebugOracle() {
    if (Child > 0) {
      kill(Child, SIGKILL);
      while (waitpid(Child, nullptr, 0) < 0 && errno == EINTR) {
      }
    }
    if (RAM != MAP_FAILED)
      munmap(RAM, MemoryBytes);
  }

  void initialize(Mode Selected) {
    Target = Selected;
    RAM = mmap(nullptr, MemoryBytes, PROT_READ | PROT_WRITE | PROT_EXEC,
               MAP_SHARED | MAP_ANONYMOUS | MAP_32BIT, -1, 0);
    ASSERT_NE(RAM, MAP_FAILED) << std::strerror(errno);
    Child = fork();
    ASSERT_GE(Child, 0) << std::strerror(errno);
    if (!Child) {
      if (Target == Mode::Code16) {
        user_desc Descriptor{};
        Descriptor.base_addr = reinterpret_cast<uintptr_t>(RAM);
        Descriptor.limit = 0xffff;
        Descriptor.contents = 2;
        Descriptor.useable = 1;
        if (syscall(SYS_modify_ldt, 1, &Descriptor, sizeof(Descriptor)))
          _exit(97);
      }
      if (ptrace(PTRACE_TRACEME, 0, nullptr, nullptr))
        _exit(98);
      kill(getpid(), SIGSTOP);
      _exit(99);
    }
    int Signal = 0;
    ASSERT_NO_FATAL_FAILURE(wait(Signal));
    ASSERT_EQ(Signal, SIGSTOP);
    ASSERT_EQ(ptrace(PTRACE_GETREGS, Child, nullptr, &Initial), 0);
  }

  void run(const X86BranchDebugCase &Case, bool Branch, Result &Output) {
    ASSERT_GT(Child, 0);
    ASSERT_LT(Case.Code.size(), StackOffset);
    auto *Bytes = static_cast<uint8_t *>(RAM);
    std::fill_n(Bytes, MemoryBytes, 0xcc); // Every unexpected tail stops.
    std::copy(Case.Code.begin(), Case.Code.end(), Bytes);
    const auto Base = reinterpret_cast<uintptr_t>(RAM);
    const uint64_t Return = (Target == Mode::Code16 ? 0 : Base) + 4;
    std::memcpy(Bytes + StackOffset, &Return, sizeof(Return));
    auto Registers = Initial;
    if (Target != Mode::Long64) {
      Registers.cs = Target == Mode::Code16 ? 7 : 0x23;
      Registers.ss = 0x2b; // Linux x86-64 user data/stack selector.
    }
    Registers.rip = Target == Mode::Code16 ? 0 : Base;
    Registers.rsp = Base + StackOffset;
    Registers.eflags = Case.Flags;
    Registers.rax = 0;
    Registers.rcx = Case.Count;
    Registers.orig_rax = UINT64_MAX;
    ASSERT_EQ(ptrace(PTRACE_SETREGS, Child, nullptr, &Registers), 0);
    ASSERT_EQ(ptrace(Branch ? PTRACE_SINGLEBLOCK : PTRACE_SINGLESTEP, Child,
                     nullptr, nullptr),
              0);
    int Signal = 0;
    ASSERT_NO_FATAL_FAILURE(wait(Signal));
    ASSERT_EQ(Signal, SIGTRAP);
    siginfo_t Info{};
    ASSERT_EQ(ptrace(PTRACE_GETSIGINFO, Child, nullptr, &Info), 0);
    ASSERT_EQ(Info.si_code, TRAP_TRACE); // INT3 is a failure, not evidence.
    ASSERT_EQ(ptrace(PTRACE_GETREGS, Child, nullptr, &Registers), 0);
    Output = {Registers.rip - (Target == Mode::Code16 ? 0 : Base),
              Registers.rax, Registers.rcx, Registers.eflags,
              int64_t(Registers.rsp) - int64_t(Base + StackOffset)};
  }

private:
  static constexpr unsigned MemoryBytes = 0x4000, StackOffset = 0x2000;
  void *RAM = MAP_FAILED;
  pid_t Child = -1;
  Mode Target = Mode::Long64;
  user_regs_struct Initial{};

  void wait(int &Signal) {
    const auto Deadline =
        std::chrono::steady_clock::now() + std::chrono::seconds(5);
    do {
      int Status = 0;
      const auto Ready = waitpid(Child, &Status, WNOHANG);
      ASSERT_TRUE(Ready >= 0 || errno == EINTR) << std::strerror(errno);
      if (Ready == Child) {
        if (WIFEXITED(Status) || WIFSIGNALED(Status))
          Child = -1; // Already reaped; never signal a recycled process ID.
        ASSERT_TRUE(WIFSTOPPED(Status)) << Status;
        Signal = WSTOPSIG(Status);
        return;
      }
      usleep(1000);
    } while (std::chrono::steady_clock::now() < Deadline);
    FAIL() << "Original branch-step tracee did not stop";
  }
};

} // namespace neverd::test
#endif
#endif // NEVERD_UNITTESTS_TRANSLATE_X86_X86BRANCHDEBUGORACLE_H
