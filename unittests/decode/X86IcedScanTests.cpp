//===- X86IcedScanTests.cpp - iced scan compatibility checks -------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Independent Capstone differential, malformed tails and explicit fallbacks.
//===----------------------------------------------------------------------===//
#include "X86IcedScan.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iterator>
#include <random>
#include <set>
#include <string>
#include <vector>
#if defined(__unix__) || defined(__APPLE__)
#include <sys/mman.h>
#include <unistd.h>
#endif

struct Oracle {
  neverd::x86iced::Scanner Scanner;
  csh Handle = 0;
  csh AdaptedHandle = 0;
  cs_insn *Insn = nullptr;
  cs_insn *AdaptedInsn = nullptr;
  std::set<unsigned> AcceptedIds;
  explicit Oracle(unsigned Bits) : Scanner(Bits) {
    if (cs_open(CS_ARCH_X86, Bits == 64 ? CS_MODE_64 : CS_MODE_32, &Handle) ==
        CS_ERR_OK) {
      cs_option(Handle, CS_OPT_TEXT, CS_OPT_OFF);
      cs_option(Handle, CS_OPT_DETAIL, CS_OPT_OFF);
      Insn = cs_malloc(Handle);
    }
    if (cs_open(CS_ARCH_X86, Bits == 64 ? CS_MODE_64 : CS_MODE_32,
                &AdaptedHandle) == CS_ERR_OK) {
      cs_option(AdaptedHandle, CS_OPT_TEXT, CS_OPT_OFF);
      cs_option(AdaptedHandle, CS_OPT_DETAIL, CS_OPT_OFF);
      if (Scanner.attach(AdaptedHandle))
        AdaptedInsn = cs_malloc(AdaptedHandle);
    }
  }
  ~Oracle() {
    if (AdaptedInsn)
      cs_free(AdaptedInsn, 1);
    if (AdaptedHandle) {
      Scanner.detach(AdaptedHandle);
      cs_close(&AdaptedHandle);
    }
    if (Insn)
      cs_free(Insn, 1);
    if (Handle)
      cs_close(&Handle);
  }
};
static uint64_t Accepted = 0, Checked = 0;
static bool check(Oracle &O, unsigned Bits, const uint8_t *Bytes,
                  size_t Length) {
  ++Checked;
  const uint32_t Result = O.Scanner.scan(Bytes, Length);
  if (Result) {
    ++Accepted;
    O.AcceptedIds.insert(Result >> 8);
  }
  const uint8_t *P = Bytes;
  size_t N = Length;
  uint64_t A = 0xfffffff8ULL;
  const bool OK = cs_disasm_iter(O.Handle, &P, &N, &A, O.Insn);
  const uint8_t *AdaptedP = Bytes;
  size_t AdaptedN = Length;
  uint64_t AdaptedA = 0xfffffff8ULL;
  const bool AdaptedOK = cs_disasm_iter(O.AdaptedHandle, &AdaptedP, &AdaptedN,
                                        &AdaptedA, O.AdaptedInsn);
  const auto &Actual = *O.AdaptedInsn;
  const bool ScanOK = !Result || (OK && (Result >> 8) == O.Insn->id &&
                                  (Result & 0xff) == O.Insn->size);
  const bool CursorOK = P == AdaptedP && N == AdaptedN && A == AdaptedA;
  if (ScanOK && CursorOK && OK == AdaptedOK &&
      (!OK || (Actual.id == O.Insn->id && Actual.size == O.Insn->size &&
               Actual.address == O.Insn->address &&
               !std::memcmp(Actual.bytes, O.Insn->bytes, Actual.size) &&
               !Actual.mnemonic[0] && !Actual.op_str[0])))
    return true;
  std::fprintf(stderr, "bits=%u iced=%u/%u capstone=%u/%u bytes=", Bits,
               Actual.id, Actual.size, OK ? O.Insn->id : 0,
               OK ? O.Insn->size : 0);
  for (size_t I = 0; I < Length; ++I)
    std::fprintf(stderr, "%02x", Bytes[I]);
  std::fprintf(stderr, "\n");
  return false;
}

// Read byte fields from the pinned upstream fixture files, preserving their
// original license/provenance. Expected results still come from Capstone.
static bool checkIcedFixtures(Oracle &O, unsigned Bits) {
  for (const char *Stem : {"DecoderTest", "DecoderTestMisc", "MemoryTest"}) {
    const std::string Path = std::string(NEVERD_ICED_TEST_DATA) + "/" + Stem +
                             std::to_string(Bits) + ".txt";
    std::ifstream File(Path);
    if (!File)
      return false;
    std::string Line;
    while (std::getline(File, Line)) {
      if (Line.empty() || Line[0] == '#')
        continue;
      std::vector<uint8_t> Bytes;
      int High = -1;
      for (unsigned char C : Line.substr(0, Line.find(','))) {
        if (std::isspace(C))
          continue;
        if (!std::isxdigit(C))
          return false;
        const int Digit = C <= '9' ? C - '0' : std::tolower(C) - 'a' + 10;
        if (High < 0)
          High = Digit;
        else {
          Bytes.push_back(static_cast<uint8_t>((High << 4) | Digit));
          High = -1;
        }
      }
      if (High >= 0 || !check(O, Bits, Bytes.data(), Bytes.size()))
        return false;
      if (!Bytes.empty() && ((Bytes[0] & 0xfe) == 0xc4 || Bytes[0] == 0x62)) {
        for (size_t N = 0; N < Bytes.size(); ++N)
          if (!check(O, Bits, Bytes.data(), N))
            return false;
        Bytes.insert(Bytes.end(), {0x66, 0x48, 0x62, 0xc5, 0xf8, 0x90});
        if (!check(O, Bits, Bytes.data(), Bytes.size()))
          return false;
      }
    }
  }
  return true;
}
int main(int Argc, char **Argv) {
  for (unsigned Bits : {32u, 64u}) {
    Oracle O(Bits);
    if (!O.Insn || !O.AdaptedInsn)
      return 1;
    if (!checkIcedFixtures(O, Bits))
      return 1;
    std::mt19937_64 Random(0xabc123);
    std::array<uint8_t, 15> Bytes{};
    for (unsigned I = 0; I < 1000000; ++I) {
      for (auto &B : Bytes)
        B = static_cast<uint8_t>(Random());
      if (!check(O, Bits, Bytes.data(), 1 + I % Bytes.size()))
        return 1;
    }
    // Systematic ModR/M values, operand/address prefixes and signed tails.
    for (unsigned Prefix : {0u, 0x66u, 0x67u, 0x64u, 0x48u}) {
      if (Bits == 32 && Prefix == 0x48)
        continue;
      for (unsigned Op = 0; Op < 256; ++Op)
        for (unsigned ModRM = 0; ModRM < 256; ++ModRM) {
          Bytes.fill(0xff);
          size_t N = 0;
          if (Prefix)
            Bytes[N++] = Prefix;
          Bytes[N++] = Op;
          Bytes[N] = ModRM;
          if (!check(O, Bits, Bytes.data(), Bytes.size()))
            return 1;
        }
    }
#if defined(__unix__) || defined(__APPLE__)
    const size_t Page = static_cast<size_t>(sysconf(_SC_PAGESIZE));
    auto *Memory =
        static_cast<uint8_t *>(mmap(nullptr, Page * 2, PROT_READ | PROT_WRITE,
                                    MAP_PRIVATE | MAP_ANON, -1, 0));
    if (Memory == MAP_FAILED)
      return 1;
    if (mprotect(Memory + Page, Page, PROT_NONE))
      return 1;
    for (size_t N = 1; N <= 15; ++N) {
      std::memset(Memory + Page - N, 0x66, N);
      if (!check(O, Bits, Memory + Page - N, N))
        return 1;
      for (const auto &Seed :
           {std::vector<uint8_t>{0xc5, 0xf8, 0x28, 0x00},
            std::vector<uint8_t>{0xc4, 0xe2, 0x69, 0xa8, 0x80, 0xff, 0xff, 0xff,
                                 0xff},
            std::vector<uint8_t>{0x62, 0xf1, 0x7c, 0x48, 0x28, 0x00}}) {
        std::memset(Memory + Page - N, 0xff, N);
        std::memcpy(Memory + Page - N, Seed.data(), std::min(N, Seed.size()));
        if (!check(O, Bits, Memory + Page - N, N))
          return 1;
      }
      Memory[Page - 1] = 0x90;
      if (!check(O, Bits, Memory + Page - N, N))
        return 1;
    }
    munmap(Memory, Page * 2);
#endif
    if (Argc == 3 && Bits == static_cast<unsigned>(std::atoi(Argv[1]))) {
      std::ifstream File(Argv[2], std::ios::binary);
      const std::vector<uint8_t> Data{std::istreambuf_iterator<char>(File), {}};
      if (Data.empty())
        return 1;
      for (size_t N = 0; N < Data.size(); ++N)
        if (!check(O, Bits, Data.data() + N, Data.size() - N))
          return 1;
    }
    for (unsigned Id :
         {X86_INS_VMOVAPS, X86_INS_VMOVUPS, X86_INS_VMOVAPD, X86_INS_VMOVUPD,
          X86_INS_VMOVSS, X86_INS_VMOVSD, X86_INS_VXORPS, X86_INS_VXORPD,
          X86_INS_VFMADD132PS, X86_INS_VFMADD213PS, X86_INS_VFMADD231PS,
          X86_INS_VFMADD132PD, X86_INS_VFMADD213PD, X86_INS_VFMADD231PD})
      if (!O.AcceptedIds.count(Id)) {
        std::fprintf(stderr, "missing accepted family: bits=%u id=%u\n", Bits,
                     Id);
        return 1;
      }
  }
  std::printf("checked=%llu accepted=%llu\n", (unsigned long long)Checked,
              (unsigned long long)Accepted);
  return Accepted ? 0 : 1;
}
