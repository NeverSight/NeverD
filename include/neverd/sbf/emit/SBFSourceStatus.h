//===- SBFSourceStatus.h - Generated source status mapping -----*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Defines the typed bridge from interpreter faults to the stable status ABI
/// used by the standalone C and Rust source backends.
///
//===----------------------------------------------------------------------===//

#ifndef NEVERD_SBF_EMIT_SBFSOURCESTATUS_H
#define NEVERD_SBF_EMIT_SBFSOURCESTATUS_H

#include "neverd/sbf/SBFConstants.h"

#include "llvm/ADT/StringRef.h"
#include "llvm/Support/ErrorHandling.h"

#include <cstdint>

namespace neverd::sbf {

enum class SourceStatus : uint32_t {
#define SBF_SOURCE_SUCCESS(NAME, FAULT_CODE, C_NAME, VALUE) NAME = (VALUE),
#define SBF_SOURCE_ERROR(NAME, FAULT_CODE, C_NAME, C_VALUE, RUST_VALUE)        \
  NAME = (C_VALUE),
#include "neverd/sbf/emit/SBFSourceStatuses.def"
};

[[nodiscard]] constexpr uint32_t sourceStatusCode(SourceStatus Status) {
  return static_cast<uint32_t>(Status);
}

[[nodiscard]] constexpr bool isKnownSourceStatusCode(uint32_t Value) {
  switch (Value) {
#define SBF_SOURCE_SUCCESS(NAME, FAULT_CODE, C_NAME, VALUE)                    \
  case sourceStatusCode(SourceStatus::NAME):                                   \
    return true;
#define SBF_SOURCE_ERROR(NAME, FAULT_CODE, C_NAME, C_VALUE, RUST_VALUE)        \
  case sourceStatusCode(SourceStatus::NAME):                                   \
    return true;
#include "neverd/sbf/emit/SBFSourceStatuses.def"
  }
  return false;
}

[[nodiscard]] inline SourceStatus sourceStatusForFault(FaultCode Fault) {
  switch (Fault) {
#define SBF_SOURCE_SUCCESS(NAME, FAULT_CODE, C_NAME, VALUE)                    \
  case FaultCode::FAULT_CODE:                                                  \
    return SourceStatus::NAME;
#define SBF_SOURCE_ERROR(NAME, FAULT_CODE, C_NAME, C_VALUE, RUST_VALUE)        \
  case FaultCode::FAULT_CODE:                                                  \
    return SourceStatus::NAME;
#include "neverd/sbf/emit/SBFSourceStatuses.def"
  }
  llvm_unreachable("invalid SBF execution fault");
}

[[nodiscard]] inline llvm::StringLiteral
cSourceStatusName(SourceStatus Status) {
  switch (Status) {
#define SBF_SOURCE_SUCCESS(NAME, FAULT_CODE, C_NAME, VALUE)                    \
  case SourceStatus::NAME:                                                     \
    return #C_NAME;
#define SBF_SOURCE_ERROR(NAME, FAULT_CODE, C_NAME, C_VALUE, RUST_VALUE)        \
  case SourceStatus::NAME:                                                     \
    return #C_NAME;
#include "neverd/sbf/emit/SBFSourceStatuses.def"
  }
  llvm_unreachable("invalid SBF generated-source status");
}

[[nodiscard]] inline llvm::StringLiteral cSourceStatusName(FaultCode Fault) {
  return cSourceStatusName(sourceStatusForFault(Fault));
}

[[nodiscard]] inline llvm::StringLiteral
rustSourceErrorName(SourceStatus Status) {
  switch (Status) {
#define SBF_SOURCE_SUCCESS(NAME, FAULT_CODE, C_NAME, VALUE)                    \
  case SourceStatus::NAME:                                                     \
    break;
#define SBF_SOURCE_ERROR(NAME, FAULT_CODE, C_NAME, C_VALUE, RUST_VALUE)        \
  case SourceStatus::NAME:                                                     \
    return "SbfErrorV2::" #NAME;
#include "neverd/sbf/emit/SBFSourceStatuses.def"
  }
  llvm_unreachable("success status is not a Rust source error");
}

[[nodiscard]] inline llvm::StringLiteral
rustLegacySourceErrorName(SourceStatus Status) {
  switch (Status) {
#define SBF_SOURCE_RUST_V1_ERROR(NAME)                                         \
  case SourceStatus::NAME:                                                     \
    return "SbfError::" #NAME;
#define SBF_SOURCE_RUST_V1_FALLBACK(NAME, LEGACY_NAME)                         \
  case SourceStatus::NAME:                                                     \
    return "SbfError::" #LEGACY_NAME;
#include "neverd/sbf/emit/SBFSourceStatuses.def"
  case SourceStatus::Ok:
    break;
  }
  llvm_unreachable("success status is not a legacy Rust source error");
}

[[nodiscard]] inline llvm::StringLiteral
rustLegacySourceErrorName(FaultCode Fault) {
  return rustLegacySourceErrorName(sourceStatusForFault(Fault));
}

[[nodiscard]] inline llvm::StringLiteral rustSourceErrorName(FaultCode Fault) {
  return rustSourceErrorName(sourceStatusForFault(Fault));
}

[[nodiscard]] inline uint32_t rustSourceErrorCode(SourceStatus Status) {
  switch (Status) {
#define SBF_SOURCE_SUCCESS(NAME, FAULT_CODE, C_NAME, VALUE)                    \
  case SourceStatus::NAME:                                                     \
    break;
#define SBF_SOURCE_ERROR(NAME, FAULT_CODE, C_NAME, C_VALUE, RUST_VALUE)        \
  case SourceStatus::NAME:                                                     \
    return RUST_VALUE;
#include "neverd/sbf/emit/SBFSourceStatuses.def"
  }
  llvm_unreachable("success status is not a Rust source error");
}

[[nodiscard]] inline uint32_t rustSourceErrorCode(FaultCode Fault) {
  return rustSourceErrorCode(sourceStatusForFault(Fault));
}

} // namespace neverd::sbf

#endif // NEVERD_SBF_EMIT_SBFSOURCESTATUS_H
