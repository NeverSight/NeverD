//===- X86LegacyTestState.h - Protected execution fixture -----*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Shared protected state, descriptor tables and ordinary paged RAM for tests.
//===----------------------------------------------------------------------===//

#ifndef NEVERD_UNITTESTS_TRANSLATE_X86_X86LEGACYTESTSTATE_H
#define NEVERD_UNITTESTS_TRANSLATE_X86_X86LEGACYTESTSTATE_H

#include "gtest/gtest.h"

#include "neverd/translate/X86/X86LegacyExecution.h"

#include "llvm/ADT/ArrayRef.h"
#include "llvm/Support/Error.h"

#include <cstdint>
#include <memory>
#include <vector>

namespace neverd::translate {

class X86LegacyTestState {
protected:
  X86LegacyExecutionState S;
  X86LegacyExecutionOptions O{{X86LegacySegmentProfile::WordStackOffsetFirst,
                               X86AlignmentPriority::BeforePaging,
                               {}},
                              X86LegacyEventProfile::OrderedFrame,
                              false,
                              X86LegacyReturnProfile::OrderedFrame};
  std::unique_ptr<X86PagedMemory> M;

  void resetLegacyState(const X86PagingFeatures &Features = {
                            .PhysicalAddressBits = 32}) {
    S = {};
    O.Scalars = X86LegacyScalarProfile::OrderedRAM;
    O.Calls = X86LegacyCallProfile::StoreBeforeTargetLimit;
    auto RAM =
        llvm::cantFail(createZeroedGuestState(GuestArchitecture::X86_64));
    RAM.Memory.push_back({0, MemoryPermission::Read | MemoryPermission::Write,
                          0, std::vector<uint8_t>(0x20000)});
    M = llvm::cantFail(X86PagedMemory::create(
        RAM, Features, {.CR0 = 0x80010001, .CR3 = 0x10000, .RFlags = 2}));
    put(0x10000, 0x11007, GuestScalarWidth::I32);
    for (unsigned I = 0; I != 32; ++I)
      put(0x11000 + I * 4, I * 4096 | 7, GuestScalarWidth::I32);
    S.Core.CPL = 3;
    for (auto &C : S.Core.Segments)
      C = {0, UINT32_MAX, 35, 0xc0f3, false};
    S.Core.Segments[1] = {0, UINT32_MAX, 27, 0xc0fb, false};
    S.Core.GDTRBase = 0x7000;
    S.Core.GDTRLimit = 0xff;
    S.Core.GPR[4] = 0xd000;
    S.Core.GPR[0] = 35;
    S.EIP = 0x1000;
    S.EFlags = 0x10202;
    S.CR2 = 0x12345678;
    S.IDTRBase = 0x6000;
    S.IDTRLimit = 0x7ff;
    S.TR = {0x5800, 0x67, 40, 0x8b, false};
    put(0x7008, 0x00cf9b000000ffffULL);
    put(0x7010, 0x00cf93000000ffffULL);
    put(0x7018, 0x00cffb000000ffffULL);
    put(0x7020, 0x00cff3000000ffffULL);
    put(0x5804, 0xe000, GuestScalarWidth::I32);
    put(0x5808, 16, GuestScalarWidth::I16);
    for (unsigned V = 0; V != 256; ++V)
      put(0x6000 + V * 8,
          0x2000 | (uint64_t{8} << 16) | (uint64_t{0xee} << 40));
  }

  void put(uint64_t Address, uint64_t Value,
           GuestScalarWidth Width = GuestScalarWidth::I64) {
    ASSERT_EQ(M->physicalMemory().storeScalar(Address, Width, Value).Status,
              GuestMemoryAccessStatus::Completed);
  }

  uint64_t get(uint64_t Address,
               GuestScalarWidth Width = GuestScalarWidth::I32) {
    auto R = M->physicalMemory().loadScalar(Address, Width);
    EXPECT_EQ(R.Status, GuestMemoryAccessStatus::Completed);
    return R.Value;
  }

  X86LegacyExecutionResult run(llvm::ArrayRef<uint8_t> Bytes) {
    for (unsigned I = 0; I != Bytes.size(); ++I)
      put(S.EIP + I, Bytes[I], GuestScalarWidth::I8);
    return llvm::cantFail(executeX86LegacyInstruction(S, *M, Bytes, O));
  }
};

} // namespace neverd::translate

#endif // NEVERD_UNITTESTS_TRANSLATE_X86_X86LEGACYTESTSTATE_H
