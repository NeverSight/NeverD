//===- X64RtmCapture.h - Captured XBEGIN evidence ----------------*- C++
//-*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Bind a captured instruction to its vCPU, code bytes and image address.
//===----------------------------------------------------------------------===//

#ifndef NEVERD_LIB_PIPELINE_X86_X64RTMCAPTURE_H
#define NEVERD_LIB_PIPELINE_X86_X64RTMCAPTURE_H

#include "neverd/analysis/arch/x86_64/X64ContinuationSource.h"

#include "llvm/Support/JSON.h"

#include <limits>
#include <optional>
#include <string>

namespace neverd {

template <typename T>
std::optional<T> rtmCaptureWord(const llvm::json::Object &Record,
                                llvm::StringRef Key) {
  const auto *Value = Record.get(Key);
  if (!Value)
    return std::nullopt;
  if (const auto Number = Value->getAsUINT64();
      Number && *Number <= std::numeric_limits<T>::max())
    return static_cast<T>(*Number);
  const auto Text = Value->getAsString();
  T Number;
  if (Text && !Text->empty() && !Text->starts_with('-') &&
      !Text->getAsInteger(0, Number))
    return Number;
  return std::nullopt;
}

llvm::Expected<va_t> rtmCaptureImagePC(va_t Rip, std::optional<va_t> LoadBase,
                                       va_t ImageBase);

/// Emit the V6 owner's scalar step directly. Unsupported future operations
/// fall back to the requested general backend.
std::optional<std::string> renderX64RtmFaultStep(const LowFunc &F);

struct X64RtmFaultCapture {
  analysis::X64XbeginFaultSite Site;
  llvm::json::Object Receipt;

  static llvm::Expected<X64RtmFaultCapture>
  read(const llvm::json::Value &Value, va_t ImageBase, unsigned AddressBits);
};

} // namespace neverd

#endif // NEVERD_LIB_PIPELINE_X86_X64RTMCAPTURE_H
