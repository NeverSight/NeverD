//===- X87WorkloadHarness.c - Native x87 stream replay and timing
//----------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Protocol-v1 finite raw-binary80 input, complete packet comparison and
/// optional process-isolated timing. The host's transcendental bits are the
/// reference; this does not claim processor-independent FSIN accuracy.
//===----------------------------------------------------------------------===//

#include <errno.h>
#include <inttypes.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/resource.h>
#include <time.h>

enum { Samples = 2048, Stride = 32, Header = 16, Guard = 16 };
static const uint32_t Seed = 0x871677;
static unsigned char Input[Header + Samples * Stride + Guard];
static unsigned char Expected[sizeof(Input)], Actual[sizeof(Input)];
#if ORIGINAL_MICROSOFT_ABI
extern unsigned __attribute__((ms_abi)) native_probe(void *);
#else
extern unsigned native_probe(void *);
#endif
extern unsigned batch_probe(void *);

// This host-callable adapter retains the original source calling convention.
// The generated projection exposes its recovered logical argument to the host.
static unsigned original(void *Packet) { return native_probe(Packet); }

static uint32_t next(uint32_t *State) {
  *State = *State * UINT32_C(1664525) + UINT32_C(1013904223);
  return *State;
}

static void input(unsigned Count, uint16_t Control) {
  const uint16_t Exponents[] = {0,      1,      0x3ffd, 0x3ffe, 0x3fff,
                                0x4000, 0x403d, 0x403e, 0x403f, 0x7ffe};
  uint32_t State = Seed;
  memset(Input, 0xa5, sizeof(Input));
  memcpy(Input, &Count, sizeof(Count));
  memcpy(Input + 4, &Control, sizeof(Control));
  for (unsigned I = 0; I != Count; ++I) {
    uint64_t Significand = (uint64_t)next(&State) << 32;
    Significand |= next(&State);
    uint16_t Exponent = Exponents[I % (sizeof(Exponents) / sizeof(*Exponents))];
    if (Exponent)
      Significand |= UINT64_C(0x8000000000000000);
    else
      Significand &= UINT64_C(0x7fffffffffffffff);
    if (I % 31 == 0) {
      Significand = 0;
      Exponent = 0;
    }
    Exponent |= (I & 1) << 15;
    memcpy(Input + Header + I * Stride, &Significand, 8);
    memcpy(Input + Header + I * Stride + 8, &Exponent, 2);
  }
}

static FILE *output(const char *Prefix, const char *Suffix) {
  size_t Size = strlen(Prefix) + strlen(Suffix) + 1;
  char *Path = malloc(Size);
  if (!Path)
    return NULL;
  snprintf(Path, Size, "%s%s", Prefix, Suffix);
  FILE *File = fopen(Path, "wb");
  free(Path);
  return File;
}

static int verify(const char *Prefix) {
  const unsigned Counts[] = {0, 1, 7, 31, 257, Samples};
  FILE *Inputs = output(Prefix, "-inputs.bin");
  FILE *Original = output(Prefix, "-original.bin");
  FILE *Generated = output(Prefix, "-generated.bin");
  if (!Inputs || !Original || !Generated)
    return 2;
  unsigned Cases = 0, Observations = 0;
  for (unsigned PC = 0; PC != 4; ++PC) {
    if (PC == 1)
      continue;
    for (unsigned RC = 0; RC != 4; ++RC)
      for (unsigned I = 0; I != sizeof(Counts) / sizeof(*Counts); ++I) {
        unsigned Count = Counts[I];
        uint16_t Control = 0x7f | (PC << 8) | (RC << 10);
        input(Count, Control);
        memcpy(Expected, Input, sizeof(Input));
        memcpy(Actual, Input, sizeof(Input));
        unsigned A = native_probe(Expected), B = batch_probe(Actual);
        size_t Bytes = Header + Count * Stride + Guard;
        if (fwrite(Input, 1, Bytes, Inputs) != Bytes ||
            fwrite(Expected, 1, Bytes, Original) != Bytes ||
            fwrite(Actual, 1, Bytes, Generated) != Bytes)
          return 2;
        if (A != B || memcmp(Expected, Actual, sizeof(Input))) {
          fprintf(stderr, "count=%u cw=%04x native=%u generated=%u\n", Count,
                  Control, A, B);
          for (size_t Byte = 0; Byte != sizeof(Input); ++Byte)
            if (Expected[Byte] != Actual[Byte]) {
              fprintf(stderr,
                      "first changed byte=%zu native=%02x generated=%02x\n",
                      Byte, Expected[Byte], Actual[Byte]);
              break;
            }
          return 1;
        }
        ++Cases;
        Observations += Count;
      }
  }
  if (fclose(Inputs) || fclose(Original) || fclose(Generated))
    return 2;
  printf("{\"protocol\":1,\"cases\":%u,\"samples\":%u,\"seed\":%u}\n", Cases,
         Observations, Seed);
  return 0;
}

static uint64_t nanoseconds(void) {
  struct timespec Time;
  if (clock_gettime(CLOCK_MONOTONIC, &Time))
    exit(2);
  return (uint64_t)Time.tv_sec * UINT64_C(1000000000) + Time.tv_nsec;
}

static unsigned long addressSpacePeakRSS(void) {
  // Linux ru_maxrss can retain the launcher's pre-exec high-water mark.
  // VmHWM belongs to the new executable's mm/address space instead.
  FILE *Status = fopen("/proc/self/status", "r");
  if (!Status)
    exit(2);
  char *Line = NULL;
  size_t Capacity = 0;
  unsigned long Peak = 0;
  while (getline(&Line, &Capacity, Status) >= 0)
    if (sscanf(Line, "VmHWM: %lu kB", &Peak) == 1)
      break;
  free(Line);
  if (fclose(Status) || !Peak)
    exit(2);
  return Peak;
}

static int benchmark(const char *Kind, const char *Repetitions) {
  char *End;
  errno = 0;
  unsigned long Count = strtoul(Repetitions, &End, 10);
  if (errno || !*Repetitions || *End || !Count || Repetitions[0] == '-')
    return 2;
  unsigned (*Probe)(void *) = NULL;
  if (!strcmp(Kind, "original"))
    Probe = original;
  if (!strcmp(Kind, "generated"))
    Probe = batch_probe;
  if (!Probe)
    return 2;
  input(Samples, 0x37f);
  // Each call overwrites only output fields; inputs and CW are unchanged.
  // One full batch warms the code and data before the clock begins.
  if (Probe(Input))
    return 1;
  uint64_t Begin = nanoseconds();
  for (unsigned long I = 0; I != Count; ++I)
    if (Probe(Input))
      return 1;
  uint64_t Elapsed = nanoseconds() - Begin;
  struct rusage Usage;
  if (getrusage(RUSAGE_SELF, &Usage))
    return 2;
  uint64_t Digest = UINT64_C(14695981039346656037);
  for (size_t I = 0; I != sizeof(Input); ++I)
    Digest = (Digest ^ Input[I]) * UINT64_C(1099511628211);
  unsigned long Peak = addressSpacePeakRSS();
  uint16_t Control;
  memcpy(&Control, Input + 4, sizeof(Control));
  printf("{\"protocol\":2,\"kind\":\"%s\",\"repetitions\":%lu,"
         "\"samples_per_batch\":%u,\"nanoseconds\":%" PRIu64 ","
         "\"address_space_peak_rss_kib\":%lu,\"rusage_maxrss_kib\":%ld,"
         "\"control_word\":%u,\"seed\":%u,"
         "\"final_packet_fnv1a64\":\"%016" PRIx64 "\"}\n",
         Kind, Count, Samples, Elapsed, Peak, Usage.ru_maxrss, Control, Seed,
         Digest);
  return 0;
}

int main(int Argc, char **Argv) {
  if (Argc == 2)
    return verify(Argv[1]);
  if (Argc == 4 && !strcmp(Argv[1], "--benchmark"))
    return benchmark(Argv[2], Argv[3]);
  return 2;
}
