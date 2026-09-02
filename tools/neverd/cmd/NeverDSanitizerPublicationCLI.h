//===- NeverDSanitizerPublicationCLI.h - sanitizer CLI receipt -*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Pure validation and presentation policy for the strict sanitizer's
/// authenticated publication receipt.  Keeping this independent of command
/// execution gives failure-only outcomes (especially indeterminate publish
/// and published-but-incomplete receipt) a deterministic contract test seam.
///
//===----------------------------------------------------------------------===//

#ifndef NEVERD_TOOLS_NEVERD_SANITIZERPUBLICATIONCLI_H
#define NEVERD_TOOLS_NEVERD_SANITIZERPUBLICATIONCLI_H

#include "neverd/sdk/NeverDCAPIPatch.h"

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <optional>
#include <string>
#include <utility>

namespace neverd::cli::sanitizer_publication {

inline constexpr uint32_t kSupportedPublicationABIVersion =
    NEVERD_SANITIZE_PUBLICATION_ABI_VERSION;

enum class SuccessDisposition : uint8_t {
  CreatedExclusive,
  AuthenticatedNoChange,
};

inline bool containsCaseInsensitive(const std::string &Text,
                                    const std::string &Needle) {
  return std::search(Text.begin(), Text.end(), Needle.begin(), Needle.end(),
                     [](unsigned char Left, unsigned char Right) {
                       return std::tolower(Left) == std::tolower(Right);
                     }) != Text.end();
}

inline void appendAdvisory(std::string &Detail, const char *Advisory) {
  if (!Detail.empty())
    Detail += "; ";
  Detail += Advisory;
}

inline std::string
unknownDestinationStateMessage(std::string Detail,
                               const std::string &RequestedOutputPath) {
  const bool HasUnknownState =
      containsCaseInsensitive(Detail, "destination state is unknown") &&
      containsCaseInsensitive(Detail, "destination may exist") &&
      containsCaseInsensitive(Detail, "inspect");
  const bool HasRequestedPath =
      containsCaseInsensitive(Detail, "requested output path") &&
      Detail.find(RequestedOutputPath) != std::string::npos;
  if (!HasUnknownState) {
    std::string Advisory =
        "destination state is unknown for requested output path '";
    Advisory += RequestedOutputPath;
    Advisory +=
        "' and the destination may exist; inspect it before use, retrying, "
        "or deleting anything";
    appendAdvisory(Detail, Advisory.c_str());
  } else if (!HasRequestedPath) {
    std::string Advisory = "requested output path: '";
    Advisory += RequestedOutputPath;
    Advisory += "'";
    appendAdvisory(Detail, Advisory.c_str());
  }
  return Detail;
}

inline std::optional<std::string>
validateSuccessResultFields(const neverd_sanitize_result_v1 &Result) {
  if (Result.struct_size != sizeof(Result))
    return "native sanitizer changed result struct_size";
  if (Result.ok != 1 || Result.status != NEVERD_SANITIZE_STATUS_OK)
    return "native sanitizer returned a non-success terminal state on its "
           "success path";
  if (Result.plan_version != 1)
    return "unsupported sanitizer plan version " +
           std::to_string(Result.plan_version) + "; expected 1";

  if (Result.publication_receipt_version != kSupportedPublicationABIVersion)
    return "unsupported sanitizer publication receipt version " +
           std::to_string(Result.publication_receipt_version) + "; expected " +
           std::to_string(kSupportedPublicationABIVersion);
  if (Result.publication_receipt_complete != 1)
    return "native sanitizer reported success without a complete publication "
           "receipt; the destination may exist and requires inspection before "
           "use or retry";

  constexpr uint32_t KnownGuarantees =
      NEVERD_SANITIZE_PUBLICATION_GUARANTEE_NAMESPACE_ATOMIC |
      NEVERD_SANITIZE_PUBLICATION_GUARANTEE_DESTINATION_CREATE_EXCLUSIVE |
      NEVERD_SANITIZE_PUBLICATION_GUARANTEE_COMPARE_AND_SWAP |
      NEVERD_SANITIZE_PUBLICATION_GUARANTEE_CRASH_DURABLE;
  if ((Result.publication_guarantee_flags & ~KnownGuarantees) != 0)
    return "native sanitizer returned unknown publication guarantee flags";

  switch (Result.publication_namespace_disposition) {
  case NEVERD_SANITIZE_PUBLICATION_NAMESPACE_CREATE_EXCLUSIVE:
    if (Result.publication_outcome !=
        NEVERD_SANITIZE_PUBLICATION_OUTCOME_PUBLISHED)
      return "create-exclusive receipt does not report a published outcome";
    if ((Result.publication_guarantee_flags &
         NEVERD_SANITIZE_PUBLICATION_GUARANTEE_NAMESPACE_ATOMIC) == 0 ||
        (Result.publication_guarantee_flags &
         NEVERD_SANITIZE_PUBLICATION_GUARANTEE_DESTINATION_CREATE_EXCLUSIVE) ==
            0)
      return "create-exclusive receipt is missing its atomic no-replace "
             "guarantees";
    if ((Result.publication_guarantee_flags &
         NEVERD_SANITIZE_PUBLICATION_GUARANTEE_COMPARE_AND_SWAP) != 0)
      return "binary-sanitizer-v1 create-exclusive publication cannot claim "
             "replacement compare-and-swap";
    if ((Result.publication_guarantee_flags &
         NEVERD_SANITIZE_PUBLICATION_GUARANTEE_CRASH_DURABLE) != 0)
      return "binary-sanitizer-v1 create-exclusive publication cannot claim "
             "crash-durable replacement";
    if (Result.publication_operand_binding !=
            NEVERD_SANITIZE_PUBLICATION_OPERAND_BINDING_ACCESS_CONTROL_CONFINED_DISTINCT_CREDENTIALS &&
        Result.publication_operand_binding !=
            NEVERD_SANITIZE_PUBLICATION_OPERAND_BINDING_KERNEL_HELD_OBJECT)
      return "create-exclusive receipt lacks a supported publication operand "
             "binding";
    break;
  case NEVERD_SANITIZE_PUBLICATION_NAMESPACE_NO_CHANGE:
    if (Result.publication_outcome !=
        NEVERD_SANITIZE_PUBLICATION_OUTCOME_NOT_PUBLISHED)
      return "no-change receipt does not report a not-published outcome";
    if (Result.publication_guarantee_flags != 0 ||
        Result.publication_operand_binding !=
            NEVERD_SANITIZE_PUBLICATION_OPERAND_BINDING_NONE)
      return "no-change receipt claims guarantees for a namespace operation "
             "that did not occur";
    break;
  default:
    return "successful sanitizer receipt has no supported namespace "
           "disposition";
  }

  if (Result.unsupported_sites != 0)
    return "native sanitizer reported success with unsupported sites";
  if (Result.guarded_sites != Result.findings)
    return "native sanitizer guarded-site count does not match findings";
  if (Result.patched_functions < Result.guarded_functions)
    return "native sanitizer patched fewer functions than it guarded";
  if (Result.findings != 0 && Result.guarded_functions == 0)
    return "native sanitizer guarded findings without a guarded function";
  if (Result.findings == 0 &&
      (Result.guarded_functions != 0 || Result.patched_functions != 0 ||
       Result.code_size != 0 || Result.trampoline_count != 0))
    return "native sanitizer returned nonzero patch telemetry for an empty "
           "plan";
  return std::nullopt;
}

/// This function is called only after the native entry point claims success.
/// Any rejected field therefore has to be presented as an unknown destination
/// state even if the malformed receipt appears to say "not published".
inline std::optional<std::string>
validateSuccessResult(const neverd_sanitize_result_v1 &Result,
                      const std::string &RequestedOutputPath) {
  std::optional<std::string> Error = validateSuccessResultFields(Result);
  if (!Error)
    return std::nullopt;
  return unknownDestinationStateMessage(std::move(*Error), RequestedOutputPath);
}

/// The output-path accessor is queried only after the native transaction and
/// receipt have claimed success.  A missing value cannot weaken that claim to a
/// pre-publication failure: the requested destination has to be inspected.
inline std::optional<std::string>
validateSuccessOutputPath(const char *NativeOutputPath,
                          const std::string &RequestedOutputPath) {
  if (NativeOutputPath && NativeOutputPath[0] != '\0')
    return std::nullopt;
  return unknownDestinationStateMessage(
      "successful transaction has no output path", RequestedOutputPath);
}

inline bool terminalTupleRequiresUnknownDestinationAdvisory(
    int NativeReturn, const neverd_sanitize_result_v1 &Result) {
  constexpr uint32_t KnownGuarantees =
      NEVERD_SANITIZE_PUBLICATION_GUARANTEE_NAMESPACE_ATOMIC |
      NEVERD_SANITIZE_PUBLICATION_GUARANTEE_DESTINATION_CREATE_EXCLUSIVE |
      NEVERD_SANITIZE_PUBLICATION_GUARANTEE_COMPARE_AND_SWAP |
      NEVERD_SANITIZE_PUBLICATION_GUARANTEE_CRASH_DURABLE;
  if (Result.struct_size != sizeof(Result) ||
      (NativeReturn != 0 && NativeReturn != 1) ||
      (Result.ok != 0 && Result.ok != 1) ||
      Result.status > NEVERD_SANITIZE_STATUS_PUBLISHED_INCOMPLETE ||
      Result.publication_outcome >
          NEVERD_SANITIZE_PUBLICATION_OUTCOME_INDETERMINATE ||
      Result.publication_receipt_version != kSupportedPublicationABIVersion ||
      Result.publication_receipt_complete > 1 ||
      Result.publication_namespace_disposition >
          NEVERD_SANITIZE_PUBLICATION_NAMESPACE_NO_CHANGE ||
      (Result.publication_guarantee_flags & ~KnownGuarantees) != 0 ||
      Result.publication_operand_binding >
          NEVERD_SANITIZE_PUBLICATION_OPERAND_BINDING_KERNEL_HELD_OBJECT)
    return true;

  if (NativeReturn != Result.ok)
    return true;
  const bool NativeSucceeded = NativeReturn == 1;
  const bool StatusSucceeded = Result.status == NEVERD_SANITIZE_STATUS_OK;
  if (NativeSucceeded != StatusSucceeded)
    return true;
  if (NativeSucceeded)
    return false;
  if (Result.publication_receipt_complete != 0 ||
      Result.publication_guarantee_flags != 0 ||
      Result.publication_operand_binding !=
          NEVERD_SANITIZE_PUBLICATION_OPERAND_BINDING_NONE)
    return true;

  const bool OutcomeIndeterminate =
      Result.publication_outcome ==
      NEVERD_SANITIZE_PUBLICATION_OUTCOME_INDETERMINATE;
  const bool StatusIndeterminate =
      Result.status == NEVERD_SANITIZE_STATUS_PUBLISH_INDETERMINATE;
  if (OutcomeIndeterminate != StatusIndeterminate)
    return true;
  const bool OutcomePublished = Result.publication_outcome ==
                                NEVERD_SANITIZE_PUBLICATION_OUTCOME_PUBLISHED;
  const bool StatusPublishedIncomplete =
      Result.status == NEVERD_SANITIZE_STATUS_PUBLISHED_INCOMPLETE;
  return OutcomePublished != StatusPublishedIncomplete;
}

inline SuccessDisposition
successDisposition(const neverd_sanitize_result_v1 &Result) {
  return Result.publication_namespace_disposition ==
                 NEVERD_SANITIZE_PUBLICATION_NAMESPACE_CREATE_EXCLUSIVE
             ? SuccessDisposition::CreatedExclusive
             : SuccessDisposition::AuthenticatedNoChange;
}

inline const char *successMessage(SuccessDisposition Disposition) {
  switch (Disposition) {
  case SuccessDisposition::CreatedExclusive:
    return "created exclusively";
  case SuccessDisposition::AuthenticatedNoChange:
    return "authenticated existing source / no namespace change";
  }
  return "invalid publication disposition";
}

inline std::string failureMessage(const neverd_sanitize_result_v1 &Result,
                                  std::string NativeDetail,
                                  const std::string &RequestedOutputPath) {
  if (Result.status == NEVERD_SANITIZE_STATUS_PUBLISH_INDETERMINATE ||
      Result.publication_outcome ==
          NEVERD_SANITIZE_PUBLICATION_OUTCOME_INDETERMINATE) {
    if (!containsCaseInsensitive(NativeDetail,
                                 "destination state is unknown") ||
        !containsCaseInsensitive(NativeDetail, "destination may exist") ||
        !containsCaseInsensitive(NativeDetail, "inspect"))
      appendAdvisory(
          NativeDetail,
          "publication outcome is indeterminate: destination state is unknown "
          "and the destination may exist; inspect it before retrying or "
          "deleting anything");
    return unknownDestinationStateMessage(std::move(NativeDetail),
                                          RequestedOutputPath);
  } else if (Result.status == NEVERD_SANITIZE_STATUS_PUBLISHED_INCOMPLETE ||
             (Result.publication_outcome ==
                  NEVERD_SANITIZE_PUBLICATION_OUTCOME_PUBLISHED &&
              Result.publication_receipt_complete != 1)) {
    if (!containsCaseInsensitive(NativeDetail, "destination may exist") ||
        !containsCaseInsensitive(NativeDetail, "receipt is incomplete") ||
        !containsCaseInsensitive(NativeDetail, "inspect"))
      appendAdvisory(
          NativeDetail,
          "the destination may exist, but its authenticated publication "
          "receipt is incomplete; inspect it before use or retry");
    return unknownDestinationStateMessage(std::move(NativeDetail),
                                          RequestedOutputPath);
  } else if (Result.publication_outcome ==
             NEVERD_SANITIZE_PUBLICATION_OUTCOME_PUBLISHED) {
    appendAdvisory(
        NativeDetail,
        "the sanitizer reported failure after publishing the destination; "
        "inspect it before use or retry");
    return unknownDestinationStateMessage(std::move(NativeDetail),
                                          RequestedOutputPath);
  } else {
    switch (Result.status) {
    case NEVERD_SANITIZE_STATUS_PUBLISH_FAILED:
      if ((containsCaseInsensitive(NativeDetail, "destination") &&
           containsCaseInsensitive(NativeDetail, "exist")) ||
          containsCaseInsensitive(NativeDetail, "replacement")) {
        if (containsCaseInsensitive(NativeDetail, "replacement CAS"))
          break;
        appendAdvisory(NativeDetail, "binary-sanitizer-v1 has no authenticated "
                                     "replacement CAS; choose a "
                                     "new output path");
      }
      break;
    default:
      break;
    }
  }
  return NativeDetail;
}

} // namespace neverd::cli::sanitizer_publication

#endif // NEVERD_TOOLS_NEVERD_SANITIZERPUBLICATIONCLI_H
