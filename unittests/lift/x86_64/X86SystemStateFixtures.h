//===- X86SystemStateFixtures.h - Independent system encodings -*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Hand-authored x86/x64 system instruction encodings for pipeline tests.
//===----------------------------------------------------------------------===//

#ifndef NEVERD_UNITTESTS_LIFT_X86_64_X86SYSTEMSTATEFIXTURES_H
#define NEVERD_UNITTESTS_LIFT_X86_64_X86SYSTEMSTATEFIXTURES_H

#include "neverd/ir/intrinsics/Intrinsics.h"

#include <cstdint>
#include <vector>

namespace neverd::test {

struct SystemEncoding {
  std::vector<uint8_t> Bytes;
  Intrinsic Id;
  const char *Mnemonic;
};

inline std::vector<SystemEncoding> cacheZeroEncodings() {
  std::vector<SystemEncoding> E;
  for (uint8_t Segment : {0, 0x64, 0x65})
    for (bool AddressOverride : {false, true}) {
      std::vector<uint8_t> Bytes;
      if (Segment)
        Bytes.push_back(Segment);
      if (AddressOverride)
        Bytes.push_back(0x67);
      Bytes.insert(Bytes.end(), {0x0f, 0x01, 0xfc});
      E.push_back({Bytes, Intrinsic::Clzero, "clzero"});
    }
  return E;
}

inline std::vector<SystemEncoding> directStoreEncodings(Arch A) {
  std::vector<SystemEncoding> E;
  for (uint8_t Segment : {0, 0x64, 0x65})
    for (bool AddressOverride : {false, true}) {
      for (bool Wide : {false, true}) {
        if (Wide && A == Arch::X86)
          continue;
        std::vector<uint8_t> Bytes;
        if (Segment)
          Bytes.push_back(Segment);
        if (AddressOverride)
          Bytes.push_back(0x67);
        if (Wide)
          Bytes.push_back(0x48);
        // [eax/rax], ecx/rcx; the i386 address override selects [bx+si].
        Bytes.insert(Bytes.end(), {0x0f, 0x38, 0xf9, 0x08});
        E.push_back({Bytes, Intrinsic::X86DirectStore, "movdiri"});
      }
      if (A == Arch::X64) {
        std::vector<uint8_t> Bytes;
        if (Segment)
          Bytes.push_back(Segment);
        if (AddressOverride)
          Bytes.push_back(0x67);
        Bytes.insert(Bytes.end(), {0x66, 0x0f, 0x38, 0xf8, 0x08});
        E.push_back({Bytes, Intrinsic::X86DirectStore64B, "movdir64b"});
      }
    }
  return E;
}

inline std::vector<SystemEncoding> systemMemoryEncodings(Arch A) {
  std::vector<SystemEncoding> E;
  for (uint8_t Segment : {0, 0x64, 0x65}) {
    for (bool User : {false, true})
      for (bool Wide : {false, true}) {
        if (Wide && A == Arch::X86)
          continue;
        std::vector<uint8_t> Bytes;
        if (Segment)
          Bytes.push_back(Segment);
        if (User)
          Bytes.push_back(0x66);
        if (Wide)
          Bytes.push_back(0x48);
        Bytes.insert(Bytes.end(),
                     {0x0f, 0x38, static_cast<uint8_t>(User ? 0xf5 : 0xf6),
                      0x0c, 0x24}); // [esp/rsp], ecx/rcx
        E.push_back(
            {Bytes, User ? Intrinsic::CetWruss : Intrinsic::CetWrss,
             User ? (Wide ? "wrussq" : "wrussd") : (Wide ? "wrssq" : "wrssd")});
      }
    if (A == Arch::X64)
      for (bool Supervisor : {false, true})
        for (bool Address32 : {false, true}) {
          std::vector<uint8_t> Bytes;
          if (Segment)
            Bytes.push_back(Segment);
          if (Address32)
            Bytes.push_back(0x67);
          Bytes.insert(Bytes.end(),
                       {static_cast<uint8_t>(Supervisor ? 0xf3 : 0xf2), 0x0f,
                        0x38, 0xf8, 0x04, 0x24}); // rax/eax, [rsp/esp]
          E.push_back({Bytes,
                       Supervisor ? Intrinsic::Enqcmds : Intrinsic::Enqcmd,
                       Supervisor ? "enqcmds" : "enqcmd"});
        }
  }
  const auto Direct = directStoreEncodings(A);
  E.insert(E.end(), Direct.begin(), Direct.end());
  return E;
}

inline std::vector<SystemEncoding> systemExtensionEncodings(Arch A) {
  std::vector<SystemEncoding> E = {
      {{0xf3, 0x0f, 0x3a, 0xf0, 0xc0, 0xab},
       Intrinsic::X86HistoryReset,
       "hreset"},
      {{0x0f, 0x01, 0xfe}, Intrinsic::X86InvalidateBroadcast, "invlpgb"},
      {{0x0f, 0x01, 0xff}, Intrinsic::X86TlbSync, "tlbsync"}};
  if (A == Arch::X64) {
    E.push_back({{0x67, 0x0f, 0x01, 0xfe},
                 Intrinsic::X86InvalidateBroadcast,
                 "invlpgb"});
    E.push_back(
        {{0xf3, 0x0f, 0x01, 0xee}, Intrinsic::X86ClearUserInterrupt, "clui"});
    E.push_back(
        {{0xf3, 0x0f, 0x01, 0xef}, Intrinsic::X86SetUserInterrupt, "stui"});
    E.push_back(
        {{0xf3, 0x0f, 0x01, 0xed}, Intrinsic::X86TestUserInterrupt, "testui"});
    E.push_back({{0xf3, 0x0f, 0xc7, 0xf6},
                 Intrinsic::X86SendUserInterrupt,
                 "senduipi"});
    E.push_back({{0xf2, 0x0f, 0x00, 0xf6}, Intrinsic::X86LoadKernelGs, "lkgs"});
    for (uint8_t Segment : {0, 0x64, 0x65})
      for (bool Address32 : {false, true}) {
        std::vector<uint8_t> Bytes;
        if (Segment)
          Bytes.push_back(Segment);
        if (Address32)
          Bytes.push_back(0x67);
        Bytes.insert(Bytes.end(), {0xf2, 0x0f, 0x00, 0x36}); // word [rsi/esi]
        E.push_back({Bytes, Intrinsic::X86LoadKernelGsMemory, "lkgs"});
      }
  }
  return E;
}

inline std::vector<SystemEncoding> systemEncodings(Arch A) {
  std::vector<SystemEncoding> E = {
      {{0x0f, 0x01, 0xca}, Intrinsic::Clac, "clac"},
      {{0x0f, 0x01, 0xcb}, Intrinsic::Stac, "stac"},
      {{0x0f, 0x01, 0xd5}, Intrinsic::Xend, "xend"},
      {{0x0f, 0x01, 0xc6}, Intrinsic::X86WriteMsrNonSerializing, "wrmsrns"},
      {{0xf3, 0x0f, 0xc7, 0xf8}, Intrinsic::X86ReadPid, "rdpid"},
      {{0x0f, 0x06}, Intrinsic::X86ClearTaskSwitched, "clts"},
      {{0xf3, 0x0f, 0xae, 0xf0}, Intrinsic::X86UserMonitor, "umonitor"},
      {{0x67, 0xf3, 0x0f, 0xae, 0xf0}, Intrinsic::X86UserMonitor, "umonitor"},
      {{0x64, 0xf3, 0x0f, 0xae, 0xf0}, Intrinsic::X86UserMonitor, "umonitor"},
      {{0xf2, 0x0f, 0xae, 0xf1}, Intrinsic::X86UserWait, "umwait"},
      {{0x66, 0x0f, 0xae, 0xf1}, Intrinsic::X86TimedPause, "tpause"},
      {{0x0f, 0x02, 0xc1}, Intrinsic::X86LoadAccessRights, "lar"},
      {{0x66, 0x0f, 0x02, 0xc1}, Intrinsic::X86LoadAccessRights, "lar"},
      {{0x0f, 0x02, 0x04, 0x24}, Intrinsic::X86LoadAccessRightsMemory, "lar"},
      {{0x66, 0x0f, 0x02, 0x04, 0x24},
       Intrinsic::X86LoadAccessRightsMemory,
       "lar"},
      {{0x65, 0x0f, 0x02, 0x04, 0x24},
       Intrinsic::X86LoadAccessRightsMemory,
       "lar"},
      {{0x0f, 0x03, 0xc1}, Intrinsic::X86LoadSegmentLimit, "lsl"},
      {{0x66, 0x0f, 0x03, 0xc1}, Intrinsic::X86LoadSegmentLimit, "lsl"},
      {{0x0f, 0x03, 0x04, 0x24}, Intrinsic::X86LoadSegmentLimitMemory, "lsl"},
      {{0x66, 0x0f, 0x03, 0x04, 0x24},
       Intrinsic::X86LoadSegmentLimitMemory,
       "lsl"},
      {{0x65, 0x0f, 0x03, 0x04, 0x24},
       Intrinsic::X86LoadSegmentLimitMemory,
       "lsl"},
  };
  const auto Zero = cacheZeroEncodings();
  E.insert(E.end(), Zero.begin(), Zero.end());
  const auto Extensions = systemExtensionEncodings(A);
  E.insert(E.end(), Extensions.begin(), Extensions.end());
  if (A == Arch::X86) {
    E.push_back({{0x63, 0xc8}, Intrinsic::X86AdjustRpl, "arpl"});
    E.push_back({{0x63, 0x0c, 0x24}, Intrinsic::X86AdjustRplMemory, "arpl"});
    E.push_back(
        {{0x64, 0x63, 0x0c, 0x24}, Intrinsic::X86AdjustRplMemory, "arpl"});
  } else {
    E.push_back(
        {{0xf2, 0x0f, 0x01, 0xc6}, Intrinsic::X86ReadMsrList, "rdmsrlist"});
    E.push_back(
        {{0xf3, 0x0f, 0x01, 0xc6}, Intrinsic::X86WriteMsrList, "wrmsrlist"});
    for (const bool Wide : {false, true})
      for (const unsigned Op : {0u, 1u, 2u, 3u}) {
        std::vector<uint8_t> Bytes{0xf3};
        if (Wide)
          Bytes.push_back(0x48);
        Bytes.insert(Bytes.end(),
                     {0x0f, 0xae, static_cast<uint8_t>(0xc0 + 8 * Op)});
        const Intrinsic Ids[] = {
            Intrinsic::X86ReadFsBase, Intrinsic::X86ReadGsBase,
            Intrinsic::X86WriteFsBase, Intrinsic::X86WriteGsBase};
        const char *Names[] = {"rdfsbase", "rdgsbase", "wrfsbase", "wrgsbase"};
        E.push_back({Bytes, Ids[Op], Names[Op]});
      }
    E.push_back(
        {{0x48, 0x0f, 0x02, 0xc1}, Intrinsic::X86LoadAccessRights, "lar"});
    E.push_back({{0x48, 0x0f, 0x02, 0x04, 0x24},
                 Intrinsic::X86LoadAccessRightsMemory,
                 "lar"});
    E.push_back(
        {{0x48, 0x0f, 0x03, 0xc1}, Intrinsic::X86LoadSegmentLimit, "lsl"});
    E.push_back({{0x48, 0x0f, 0x03, 0x04, 0x24},
                 Intrinsic::X86LoadSegmentLimitMemory,
                 "lsl"});
  }
  return E;
}

} // namespace neverd::test

#endif // NEVERD_UNITTESTS_LIFT_X86_64_X86SYSTEMSTATEFIXTURES_H
