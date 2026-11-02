//===- X86MpxAddressFaultTests.cpp - Original address fault identity -----===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Compare MPX memory-request provenance with ordinary native MOV faults.
/// This checks address rules, not execution on enabled MPX hardware.
//===----------------------------------------------------------------------===//

#include "gtest/gtest.h"

#include "neverd/lift/X86/X86MpxExecution.h"

#include <array>
#include <cstdint>
#include <cstring>
#include <vector>

#if defined(__linux__) && defined(__x86_64__)
#include <signal.h>
#include <sys/mman.h>
#include <sys/wait.h>
#include <ucontext.h>
#include <unistd.h>
#endif

using namespace neverd;

namespace {

#if defined(__linux__) && defined(__x86_64__)
constexpr uint64_t Noncanonical = 0x8000000000000000;

void nativeFault(int, siginfo_t *, void *Opaque) {
  const auto *Context = static_cast<const ucontext_t *>(Opaque);
  _exit(int(Context->uc_mcontext.gregs[REG_TRAPNO]));
}

int originalMovFault(unsigned Base, uint8_t Prefix) {
  const auto Child = fork();
  if (Child < 0)
    return -1;
  if (Child == 0) {
    stack_t Stack{};
    Stack.ss_size = SIGSTKSZ;
    Stack.ss_sp = mmap(nullptr, Stack.ss_size, PROT_READ | PROT_WRITE,
                       MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (Stack.ss_sp == MAP_FAILED || sigaltstack(&Stack, nullptr))
      _exit(99);
    struct sigaction Handler{};
    Handler.sa_sigaction = nativeFault;
    Handler.sa_flags = SA_SIGINFO | SA_ONSTACK;
    if (sigemptyset(&Handler.sa_mask) ||
        sigaction(SIGSEGV, &Handler, nullptr) ||
        sigaction(SIGBUS, &Handler, nullptr))
      _exit(99);

    // MOVABS base, Noncanonical; [prefix] MOV RAX, [base+0]; RET.
    // An alternate signal stack also handles an invalid RSP base. The child
    // exits from the handler; it never returns through the modified GPRs.
    std::array<uint8_t, 32> Code{};
    unsigned N = 0;
    Code[N++] = Base >= 8 ? 0x49 : 0x48;
    Code[N++] = 0xb8 | (Base & 7);
    std::memcpy(Code.data() + N, &Noncanonical, sizeof(Noncanonical));
    N += sizeof(Noncanonical);
    if (Prefix)
      Code[N++] = Prefix;
    Code[N++] = Base >= 8 ? 0x49 : 0x48;
    Code[N++] = 0x8b;
    Code[N++] = 0x40 | (Base & 7);
    if ((Base & 7) == 4)
      Code[N++] = 0x24;
    Code[N++] = 0;
    Code[N++] = 0xc3;
    const auto Size = size_t(sysconf(_SC_PAGESIZE));
    void *Page = mmap(nullptr, Size, PROT_READ | PROT_WRITE,
                      MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (Page == MAP_FAILED)
      _exit(99);
    std::memcpy(Page, Code.data(), N);
    if (mprotect(Page, Size, PROT_READ | PROT_EXEC))
      _exit(99);
    reinterpret_cast<void (*)()>(Page)();
    _exit(98);
  }
  int Status = 0;
  if (waitpid(Child, &Status, 0) != Child || !WIFEXITED(Status))
    return -1;
  return WEXITSTATUS(Status);
}
#endif

TEST(X86MpxAddressFault,
     OriginalStackFaultClassSurvivesIgnoredSegmentPrefixes) {
#if defined(__linux__) && defined(__x86_64__)
  class FaultMemory : public X86MpxMemoryProvider {
  public:
    int Trap = 0;
    unsigned Requests = 0;
    X86MpxMemoryCompletion access(const X86MpxMemoryAccess &A) override {
      ++Requests;
      EXPECT_EQ(A.Operation, X86MpxMemoryOperation::LoadBounds);
      EXPECT_EQ(A.Address, Noncanonical);
      EXPECT_EQ(A.usesStackSegment() ? 12 : 13, Trap);
      return {X86MpxMemoryOutcome::Fault, {0x1111, 0x2222, 0}};
    }
  };
  for (uint8_t Prefix : {0, 0x26, 0x2e, 0x36, 0x3e, 0x64, 0x65})
    for (unsigned Base : {3u, 4u, 5u, 11u, 12u, 13u}) {
      SCOPED_TRACE(::testing::Message()
                   << "prefix=" << unsigned(Prefix) << " base=" << Base);
      const int Trap = originalMovFault(Base, Prefix);
      ASSERT_TRUE(Trap == 12 || Trap == 13) << "native probe failed: " << Trap;
      EXPECT_EQ(Trap,
                (Base == 4 || Base == 5) && Prefix != 0x64 && Prefix != 0x65
                    ? 12
                    : 13);
      X86MpxConfiguration Config;
      Config.Supported = true;
      Config.OSXSAVE = true;
      Config.XCR0 = X86MpxXcr0Mask;
      Config.PrivilegeLevel = 3;
      Config.UserConfig = X86MpxConfigEnable;
      Config.InTransaction = false;
      X86MpxContext Context;
      ASSERT_TRUE(Context.configure(Config));
      const X86MpxBounds Before{{0x1234, UINT64_MAX}, {0x5678, UINT64_MAX}};
      ASSERT_TRUE(Context.setBounds(0, Before));
      X86MpxExecutionInputs Inputs;
      Inputs.GPR[Base] = Noncanonical;
      std::vector<uint8_t> Bytes;
      if (Prefix)
        Bytes.push_back(Prefix);
      Bytes.push_back(0x66);
      if (Base >= 8)
        Bytes.push_back(0x41);
      Bytes.insert(Bytes.end(), {0x0f, 0x1a, uint8_t(0x40 | (Base & 7))});
      if ((Base & 7) == 4)
        Bytes.push_back(0x24);
      Bytes.push_back(0);
      FaultMemory Memory;
      Memory.Trap = Trap;
      const auto Result = executeX86MpxInstruction(Bytes, Arch::X64, 0x1000,
                                                   Context, Inputs, Memory);
      EXPECT_EQ(Result.Result, X86MpxResult::MemoryFault);
      EXPECT_EQ(Result.Size, Bytes.size());
      EXPECT_EQ(Memory.Requests, 1u);
      EXPECT_EQ(*Context.bounds(0), Before);
    }
  RecordProperty("native_address_fault_cases", 42);
#else
  GTEST_SKIP() << "original MOV fault oracle requires Linux x86-64";
#endif
}

} // namespace
