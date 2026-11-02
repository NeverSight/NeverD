//===- sigprof.cpp - CPU-time stack sampler for NeverD processes ----------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// An LD_PRELOAD sampler for hosts where perf is unavailable. ITIMER_PROF
/// advances with the process's CPU time, so every thread is sampled in
/// proportion to the CPU it consumes. Linux x86-64 and AArch64 only.
///
/// NEVERD_SIGPROF_OUT=PREFIX writes PREFIX.<pid>.bin and PREFIX.<pid>.maps
/// when the process exits. NEVERD_SIGPROF_HZ overrides the sampling rate.
/// report.py symbolizes and summarizes both files.
///
//===----------------------------------------------------------------------===//

#include "../ResourceLimits.h"

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <execinfo.h>
#include <fcntl.h>
#include <iterator>
#include <signal.h>
#include <sys/mman.h>
#include <sys/syscall.h>
#include <sys/time.h>
#include <ucontext.h>
#include <unistd.h>

namespace {

using namespace neverd::bench;

/// File header word; report.py checks it.
constexpr uint64_t kMagic = 0x4e44505246; // "NDPRF"

// A record is: frame count | thread id << 32, monotonic nanoseconds, the
// interrupted instruction, then return addresses outward.
uint64_t *Buffer;
std::atomic<size_t> Used{0}, Dropped{0}, Samples{0};
std::atomic<bool> Active{false};
char Prefix[3500];

uint64_t interruptedPC(const void *Context) {
  const auto *Signal = static_cast<const ucontext_t *>(Context);
#if defined(__x86_64__)
  return static_cast<uint64_t>(Signal->uc_mcontext.gregs[REG_RIP]);
#elif defined(__aarch64__)
  return static_cast<uint64_t>(Signal->uc_mcontext.pc);
#else
#error "sigprof supports Linux x86-64 and AArch64"
#endif
}

void onProf(int, siginfo_t *, void *Context) {
  const int SavedErrno = errno;
  void *Frames[kSigprofMaxFrames + kSigprofHandlerFrames];
  const int Count = backtrace(Frames, static_cast<int>(std::size(Frames)));
  const uint64_t PC = interruptedPC(Context);
  // The unwinder reports the interrupted instruction itself after the
  // handler and the signal trampoline; its callers follow.
  int First = -1;
  for (int I = 0; I < Count && I < static_cast<int>(kSigprofHandlerFrames); ++I)
    if (reinterpret_cast<uint64_t>(Frames[I]) == PC) {
      First = I + 1;
      break;
    }
  if (First < 0)
    First = std::min(Count, 2);
  const int Callers =
      std::min(Count - First, static_cast<int>(kSigprofMaxFrames) - 1);
  const size_t Words = static_cast<size_t>(Callers) + 3;
  const size_t At = Used.fetch_add(Words);
  if (At + Words > kSigprofBufferWords) {
    Dropped.fetch_add(1);
    errno = SavedErrno;
    return;
  }
  timespec Now;
  clock_gettime(CLOCK_MONOTONIC, &Now);
  const auto Thread = static_cast<uint64_t>(syscall(SYS_gettid));
  Buffer[At] = static_cast<uint64_t>(Callers + 1) | Thread << 32;
  Buffer[At + 1] = static_cast<uint64_t>(Now.tv_sec) * 1000000000u +
                   static_cast<uint64_t>(Now.tv_nsec);
  Buffer[At + 2] = PC;
  for (int I = 0; I < Callers; ++I)
    Buffer[At + 3 + I] = reinterpret_cast<uint64_t>(Frames[First + I]);
  Samples.fetch_add(1);
  errno = SavedErrno;
}

void copyFile(const char *From, const char *To) {
  const int In = open(From, O_RDONLY);
  const int Out = open(To, O_WRONLY | O_CREAT | O_TRUNC, 0644);
  char Chunk[65536];
  ssize_t Read;
  while (In >= 0 && Out >= 0 && (Read = read(In, Chunk, sizeof Chunk)) > 0)
    if (write(Out, Chunk, static_cast<size_t>(Read)) != Read)
      break;
  if (In >= 0)
    close(In);
  if (Out >= 0)
    close(Out);
}

void writeAll(int Fd, const void *Data, size_t Bytes) {
  const auto *Cursor = static_cast<const char *>(Data);
  while (Bytes) {
    const ssize_t Written = write(Fd, Cursor, Bytes);
    if (Written <= 0)
      return;
    Cursor += Written;
    Bytes -= static_cast<size_t>(Written);
  }
}

__attribute__((destructor)) void finish() {
  if (!Active.exchange(false))
    return;
  const itimerval Off{};
  setitimer(ITIMER_PROF, &Off, nullptr);
  char Path[4096];
  std::snprintf(Path, sizeof Path, "%s.%d.maps", Prefix,
                static_cast<int>(getpid()));
  copyFile("/proc/self/maps", Path);
  std::snprintf(Path, sizeof Path, "%s.%d.bin", Prefix,
                static_cast<int>(getpid()));
  const int Fd = open(Path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
  if (Fd < 0)
    return;
  const size_t Words = std::min<size_t>(Used.load(), kSigprofBufferWords);
  const uint64_t Header[] = {kMagic, Samples.load(), Dropped.load()};
  writeAll(Fd, Header, sizeof Header);
  writeAll(Fd, Buffer, Words * sizeof(uint64_t));
  close(Fd);
}

__attribute__((constructor)) void start() {
  const char *Out = std::getenv("NEVERD_SIGPROF_OUT");
  if (!Out || !*Out)
    return;
  std::snprintf(Prefix, sizeof Prefix, "%s", Out);
  const char *RateText = std::getenv("NEVERD_SIGPROF_HZ");
  const long Rate = RateText ? std::atol(RateText) : kSigprofDefaultHz;
  void *Reserved = mmap(nullptr, kSigprofBufferWords * sizeof(uint64_t),
                        PROT_READ | PROT_WRITE,
                        MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE, -1, 0);
  if (Reserved == MAP_FAILED || Rate <= 0)
    return;
  Buffer = static_cast<uint64_t *>(Reserved);
  // Load the unwinder now; its first use may allocate.
  void *Warm[4];
  backtrace(Warm, static_cast<int>(std::size(Warm)));
  struct sigaction Action{};
  Action.sa_sigaction = onProf;
  Action.sa_flags = SA_SIGINFO | SA_RESTART;
  sigemptyset(&Action.sa_mask);
  sigaction(SIGPROF, &Action, nullptr);
  const long Interval = std::max(1L, 1000000L / Rate);
  const itimerval Timer{{0, Interval}, {0, Interval}};
  Active = true;
  setitimer(ITIMER_PROF, &Timer, nullptr);
}

} // namespace

// Some tools leave through _exit after flushing their own output; still
// write the samples.
extern "C" void _exit(int Status) {
  finish();
  syscall(SYS_exit_group, Status);
  __builtin_unreachable();
}
