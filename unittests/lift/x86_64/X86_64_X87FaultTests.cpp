//===- X86_64_X87FaultTests.cpp - Native x87 signal observations
//-----------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Compare selected Linux x64 fault boundaries without relocating signal PCs
/// or promoting native signal observations to a complete exception model.
//===----------------------------------------------------------------------===//

#include "X86FPStateAccuracyFixture.h"

namespace {
class X87FaultFixture : public X86FPStateFixture {
protected:
  unsigned Kind = 0;

  X87FaultFixture() { X87ContextOverride = true; }

  std::string comparisonDriver(bool, bool DeclareProbe, bool,
                               bool = false) override {
    std::string Source = "#define FAULT_KIND " + std::to_string(Kind) + "\n";
    Source += R"(
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/wait.h>
#include <ucontext.h>
#include <unistd.h>
extern void native_probe(uintptr_t);
)";
    if (DeclareProbe)
      Source += "extern void isa_probe(uintptr_t);\n";
    Source += R"(
struct observation {
  int signal, code, address_matches;
  uintptr_t address_offset;
  uint16_t control, status, tags;
  unsigned char payload[80], packet[32];
};
static int channel;
static void *fault_address;
static unsigned char actual[32];
static void on_fault(int number, siginfo_t *info, void *opaque) {
  const unsigned char *fp = (const unsigned char *)
      ((ucontext_t *)opaque)->uc_mcontext.fpregs;
  struct observation result = {0};
  result.signal = number;
  result.code = info->si_code;
  // A multi-byte fault need not report the operand's first byte. This CPU's
  // FXSAVE reports the last byte; compare its actual offset with the original.
  result.address_offset = number == SIGFPE ? 0 :
      (uintptr_t)info->si_addr - (uintptr_t)fault_address;
  const unsigned extent = FAULT_KIND == 4 ? 512 : FAULT_KIND == 1 ? 2 : 10;
  result.address_matches = number == SIGFPE || result.address_offset < extent;
  result.control = fp[0] | (unsigned)fp[1] << 8;
  result.status = fp[2] | (unsigned)fp[3] << 8;
  result.tags = fp[4];
  for (unsigned slot = 0; slot != 8; ++slot)
    for (unsigned byte = 0; byte != 10; ++byte)
      result.payload[slot * 10 + byte] = fp[32 + slot * 16 + byte];
  for (unsigned byte = 0; byte != sizeof(actual); ++byte)
    result.packet[byte] = actual[byte];
  _exit(write(channel, &result, sizeof(result)) == sizeof(result) ? 0 : 91);
}
static int observe(int generated, struct observation *out) {
  int fd[2];
  if (pipe(fd)) return 1;
  pid_t child = fork();
  if (child < 0) return 2;
  if (!child) {
    close(fd[0]);
    channel = fd[1];
    struct sigaction action = {0};
    action.sa_sigaction = on_fault;
    action.sa_flags = SA_SIGINFO;
    sigemptyset(&action.sa_mask);
    if (sigaction(SIGFPE, &action, 0) || sigaction(SIGSEGV, &action, 0) ||
        sigaction(SIGBUS, &action, 0) || sigaction(SIGILL, &action, 0)) _exit(92);
    if (generated) isa_probe((uintptr_t)actual);
    else native_probe((uintptr_t)actual);
    _exit(93); // Reaching RET is not successful fault delivery.
  }
  close(fd[1]);
  ssize_t bytes = read(fd[0], out, sizeof(*out));
  close(fd[0]);
  int status = 0;
  return waitpid(child, &status, 0) != child || status || bytes != sizeof(*out);
}
int main(void) {
  const long page_size = sysconf(_SC_PAGESIZE);
  if (page_size <= 0) return 1;
  fault_address = mmap(0, page_size, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
  if (fault_address == MAP_FAILED) return 2;
  memset(actual, 0xa5, sizeof(actual));
  const uint16_t control = 0x37e; // Only invalid operation is unmasked.
  memcpy(actual, &control, sizeof(control));
  memcpy(actual + 8, &fault_address, sizeof(fault_address));
  struct observation original, generated;
  if (observe(0, &original) || observe(1, &generated)) return 3;
  const int waiting = FAULT_KIND == 0 || FAULT_KIND == 2;
  if (original.signal != (waiting ? SIGFPE : SIGSEGV) ||
      original.code != (waiting ? FPE_FLTINV : SEGV_ACCERR) ||
      !original.address_matches || original.control != control ||
      original.status != (FAULT_KIND == 3 ? 0x3000 : 0xb081) ||
      original.tags != 0xc0 || memcmp(original.packet, actual, sizeof(actual))) {
    printf("original fault kind=%u signal=%d code=%d cw=%04x sw=%04x tags=%02x\n",
           FAULT_KIND, original.signal, original.code, original.control,
           original.status, original.tags);
    return 4;
  }
  if (generated.signal != original.signal || generated.code != original.code ||
      generated.address_offset != original.address_offset ||
      !generated.address_matches || generated.control != original.control ||
      generated.status != original.status || generated.tags != original.tags ||
      memcmp(generated.payload, original.payload, sizeof(original.payload)) ||
      memcmp(generated.packet, original.packet, sizeof(original.packet))) {
    printf("generated fault kind=%u signal=%d code=%d cw=%04x sw=%04x tags=%02x\n",
           FAULT_KIND, generated.signal, generated.code, generated.control,
           generated.status, generated.tags);
    return 5;
  }
  return munmap(fault_address, page_size) != 0;
}
)";
    return Source;
  }

  void check(unsigned FaultKind) {
#if defined(__linux__) && defined(__x86_64__)
    Kind = FaultKind;
    // FNINIT; FLDCW [rdi]; FLDZ twice; FDIVP; RAX = inaccessible operand.
    // The divide sets a pending invalid exception without popping either slot.
    std::vector<uint8_t> Bytes{0xdb, 0xe3, 0xd9, 0x2f, 0xd9, 0xee, 0xd9,
                               0xee, 0xde, 0xf1, 0x48, 0x8b, 0x47, 8};
    const std::vector<uint8_t> Boundaries[] = {
        {0xdf, 0x20},             // waiting FBLD
        {0xd9, 0x38},             // nonwaiting FNSTCW
        {0x9b},                   // WAIT
        {0xdb, 0xe2, 0xdf, 0x20}, // FNCLEX then waiting FBLD
        {0x48, 0x0f, 0xae, 0}};   // nonwaiting FXSAVE64
    Bytes.insert(Bytes.end(), Boundaries[Kind].begin(), Boundaries[Kind].end());
    Bytes.push_back(0xc3);
    compareFloating(Bytes, false);
#else
    GTEST_SKIP() << "requires native Linux x64 signal contexts";
#endif
  }
};

TEST_F(X87FaultFixture, PendingInvalidPrecedesWaitingOperandFault) { check(0); }
TEST_F(X87FaultFixture, NonwaitingControlStoreReachesTheMemoryFault) {
  check(1);
}
TEST_F(X87FaultFixture, WaitDeliversPendingInvalidWithOriginalState) {
  check(2);
}
TEST_F(X87FaultFixture, ClearRemovesPendingInvalidBeforeTheOperandFault) {
  check(3);
}
TEST_F(X87FaultFixture, ExtendedSaveReachesMemoryBeforePendingInvalid) {
  check(4);
}
} // namespace
