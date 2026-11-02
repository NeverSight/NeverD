//===- RegistrationEntryRelocationTestUtils.h -----------------*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Mutate the exact PE32 field retired by a checked shared-entry trampoline.
//===----------------------------------------------------------------------===//

#ifndef NEVERD_UNITTESTS_LIFT_EH_REGISTRATIONENTRYRELOCATIONTESTUTILS_H
#define NEVERD_UNITTESTS_LIFT_EH_REGISTRATIONENTRYRELOCATIONTESTUTILS_H

#include "gtest/gtest.h"

#include "neverd/object/PELayout.h"
#include "neverd/support/BinaryEncoding.h"

#include "llvm/ADT/STLFunctionalExtras.h"
#include "llvm/BinaryFormat/COFF.h"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <vector>

namespace neverd::registration_test {

inline void checkRetiredEntryRelocationEdits(
    std::vector<uint8_t> Binary, uint32_t FieldRVA,
    llvm::function_ref<void(const std::vector<uint8_t> &)> Check) {
  auto PE = locatePEHeaders(Binary.data(), Binary.size());
  const auto *Directory =
      getPEDataDirectory(PE, llvm::COFF::BASE_RELOCATION_TABLE);
  ASSERT_TRUE(Directory && Directory->Size);
  auto Raw = [&](uint32_t RVA) -> std::optional<size_t> {
    std::optional<size_t> Result;
    forEachPESection(PE, [&](const PESectionFields &S, uint16_t) {
      if (RVA >= S.VirtualAddress && RVA - S.VirtualAddress < S.SizeOfRawData)
        Result = S.PointerToRawData + RVA - S.VirtualAddress;
    });
    return Result;
  };
  const auto Begin = Raw(Directory->RelativeVirtualAddress);
  ASSERT_TRUE(Begin);
  ASSERT_TRUE(rangeInBounds(*Begin, Directory->Size, Binary.size()));
  std::optional<size_t> Relocation;
  for (size_t Offset = *Begin; Offset < *Begin + Directory->Size;) {
    ASSERT_TRUE(rangeInBounds(Offset, 8, Binary.size()));
    const auto Page = readLE<uint32_t>(Binary.data() + Offset);
    const auto Size = readLE<uint32_t>(Binary.data() + Offset + 4);
    ASSERT_GE(Size, 8u);
    ASSERT_TRUE(rangeInBounds(Offset, Size, Binary.size()));
    for (size_t Entry = Offset + 8; Entry + 2 <= Offset + Size; Entry += 2) {
      const auto Word = readLE<uint16_t>(Binary.data() + Entry);
      if ((Word >> 12) == llvm::COFF::IMAGE_REL_BASED_HIGHLOW &&
          Page + (Word & 0xfff) == FieldRVA) {
        ASSERT_FALSE(Relocation);
        Relocation = Entry;
      }
    }
    Offset += Size;
  }
  ASSERT_TRUE(Relocation);
  const auto Field = Raw(FieldRVA);
  ASSERT_TRUE(Field);
  for (unsigned Mutation = 0; Mutation != 3; ++Mutation) {
    SCOPED_TRACE(Mutation);
    auto Changed = Binary;
    if (Mutation < 2) {
      const auto Word = readLE<uint16_t>(Changed.data() + *Relocation);
      writeLE<uint16_t>(Changed.data() + *Relocation,
                        Mutation ? Word + 1 : Word - 1);
    } else
      Changed[*Field] ^= 1;
    Check(Changed);
  }
}

} // namespace neverd::registration_test

#endif // NEVERD_UNITTESTS_LIFT_EH_REGISTRATIONENTRYRELOCATIONTESTUTILS_H
