//===- AndroidEnvironment.cpp - Live Bionic environment queries -----------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// API 28 getenv over an explicit guest vector, without host state or copies
/// of returned values. Imported, dynamic and threaded calls share this owner.
///
//===----------------------------------------------------------------------===//

#include "AndroidInternal.h"

#include "neverd/emulation/CPU.h"

#include "llvm/Support/Endian.h"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <utility>

namespace neverd::emulation::android_model {
BionicResult Bionic::queryEnvironment(const NativeCallEvent &Call) {
  const auto &Vector = Options.Android->EnvironmentVector;
  if (!Vector) {
    Result.Stop = ProcessStopReason::UnsupportedService;
    Result.Diagnostic = diagnostic::EnvironmentUnspecified;
    return std::optional<BionicValue>();
  }
  // Bound total work, including repeated entries and comparison prefixes.
  // Exhaustion is a model limit, never a fabricated missing variable.
  uint64_t Remaining = Options.MemoryLimit;
  auto ReadByte = [&](uint64_t Base,
                      uint64_t Offset) -> llvm::Expected<uint8_t> {
    if (!Remaining)
      return failure(diagnostic::EnvironmentScanLimit);
    if (Base > UINT64_MAX - Offset)
      return failure(diagnostic::GuestPointer);
    --Remaining;
    return byte(Base + Offset);
  };
  std::string Name;
  for (;;) {
    auto C = ReadByte(Call.Arguments[0], Name.size());
    if (!C)
      return C.takeError();
    // API 28 stops at '=' as well as NUL. Even a null environment first
    // reads the name, and an empty name can match an entry beginning '='.
    if (!*C || *C == '=')
      break;
    Name.push_back(*C);
  }
  if (!*Vector)
    return value(0);
  uint64_t Slot = *Vector;
  for (;;) {
    uint8_t Bytes[sizeof(uint64_t)];
    if (Remaining < sizeof(Bytes))
      return failure(diagnostic::EnvironmentScanLimit);
    Remaining -= sizeof(Bytes);
    if (auto E = access(Slot, sizeof(Bytes), Read))
      return std::move(E);
    if (auto E = CPU.read(Slot, Bytes))
      return std::move(E);
    const uint64_t Entry = llvm::support::endian::read64le(Bytes);
    if (!Entry)
      return value(0);
    size_t Matched = 0;
    while (Matched < Name.size()) {
      auto C = ReadByte(Entry, Matched);
      if (!C)
        return C.takeError();
      if (*C != static_cast<uint8_t>(Name[Matched]))
        break;
      ++Matched;
    }
    if (Matched == Name.size()) {
      auto Separator = ReadByte(Entry, Matched);
      if (!Separator)
        return Separator.takeError();
      if (*Separator == '=') {
        if (Entry > UINT64_MAX - Matched - 1)
          return failure(diagnostic::GuestPointer);
        // Do not read the value, later entries, or an assumed string tail.
        // The caller receives the original guest pointer and its lifetime.
        return value(Entry + Matched + 1);
      }
    }
    if (Slot > UINT64_MAX - sizeof(Bytes))
      return failure(diagnostic::GuestPointer);
    Slot += sizeof(Bytes);
  }
}
} // namespace neverd::emulation::android_model
