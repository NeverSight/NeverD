//===- ProcessCall.h - Named guest process service evidence -----*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
#ifndef NEVERD_EMULATION_PROCESSCALL_H
#define NEVERD_EMULATION_PROCESSCALL_H
#include <array>
#include <cstdint>
#include <optional>
#include <string>
namespace neverd::emulation {
namespace process_call {
#define NEVERD_PROCESS_CALL_VALUE(Name, Value)                                 \
  inline constexpr unsigned Name = Value;
#include "neverd/emulation/ProcessProfile.def"
#undef NEVERD_PROCESS_CALL_VALUE
} // namespace process_call
struct NativeCallEvent {
  uint64_t PC;
  std::string Name;
  std::array<uint64_t, process_call::MaxArguments> Arguments{};
  std::optional<uint64_t> Result;
  /// Explicit provider/argument count for Windows named imports. Android's
  /// existing Bionic report keeps its compatible register-bank representation.
  std::string Module;
  unsigned ArgumentCount = 0;
  /// Android resolver request, or the explicit provider of a dynamically
  /// obtained call.
  std::string Library;
  std::string Symbol;
};
} // namespace neverd::emulation
#endif
