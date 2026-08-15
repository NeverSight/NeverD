//===- NativeTranslationSession.h - Native translated execution -*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Defines the fail-closed x86-64 guest to native AArch64 execution session.
/// The version-1 engine publishes one audited block at a time and commits its
/// fixed runtime state back into GuestState only after invocation, exit, and
/// memory snapshots have all passed validation.
///
//===----------------------------------------------------------------------===//

#ifndef NEVERD_TRANSLATE_NATIVETRANSLATIONSESSION_H
#define NEVERD_TRANSLATE_NATIVETRANSLATIONSESSION_H

#include "neverd/translate/TranslationObjectCompiler.h"
#include "neverd/translate/TranslationSession.h"

#include "llvm/ADT/StringRef.h"
#include "llvm/Support/Error.h"

#include <cstdint>
#include <memory>
#include <string>
#include <system_error>

namespace llvm {
class raw_ostream;
} // namespace llvm

namespace neverd::translate {

/// Stable failures at the native execution-session boundary.  Guest-visible
/// stops such as returns and memory faults are TranslationResult values rather
/// than errors.  Append values without renumbering existing entries.
enum class NativeTranslationSessionErrorCode : uint8_t {
  InvalidRequest = 0,
  UnsupportedProcessTarget = 1,
  RuntimeCreationFailed = 2,
  TranslationFailed = 3,
  RuntimeRegistryFailed = 4,
  LinkFailed = 5,
  StaleCode = 6,
  RuntimeStateRejected = 7,
  RuntimeFrameRejected = 8,
  InvocationFailed = 9,
  RuntimeExitRejected = 10,
  StateCommitFailed = 11,
  UnloadFailed = 12,
  AlreadyRunning = 13,
  RestoreWhileRunning = 14,
  IdentityExhausted = 15,
  RunCounterOverflow = 16,
};

static_assert(
    static_cast<uint8_t>(NativeTranslationSessionErrorCode::InvalidRequest) ==
        0 &&
    static_cast<uint8_t>(
        NativeTranslationSessionErrorCode::UnsupportedProcessTarget) == 1 &&
    static_cast<uint8_t>(
        NativeTranslationSessionErrorCode::RuntimeCreationFailed) == 2 &&
    static_cast<uint8_t>(
        NativeTranslationSessionErrorCode::TranslationFailed) == 3 &&
    static_cast<uint8_t>(
        NativeTranslationSessionErrorCode::RuntimeRegistryFailed) == 4 &&
    static_cast<uint8_t>(NativeTranslationSessionErrorCode::LinkFailed) == 5 &&
    static_cast<uint8_t>(NativeTranslationSessionErrorCode::StaleCode) == 6 &&
    static_cast<uint8_t>(
        NativeTranslationSessionErrorCode::RuntimeStateRejected) == 7 &&
    static_cast<uint8_t>(
        NativeTranslationSessionErrorCode::RuntimeFrameRejected) == 8 &&
    static_cast<uint8_t>(NativeTranslationSessionErrorCode::InvocationFailed) ==
        9 &&
    static_cast<uint8_t>(
        NativeTranslationSessionErrorCode::RuntimeExitRejected) == 10 &&
    static_cast<uint8_t>(
        NativeTranslationSessionErrorCode::StateCommitFailed) == 11 &&
    static_cast<uint8_t>(NativeTranslationSessionErrorCode::UnloadFailed) ==
        12 &&
    static_cast<uint8_t>(NativeTranslationSessionErrorCode::AlreadyRunning) ==
        13 &&
    static_cast<uint8_t>(
        NativeTranslationSessionErrorCode::RestoreWhileRunning) == 14 &&
    static_cast<uint8_t>(
        NativeTranslationSessionErrorCode::IdentityExhausted) == 15 &&
    static_cast<uint8_t>(
        NativeTranslationSessionErrorCode::RunCounterOverflow) == 16);

class NativeTranslationSessionError final
    : public llvm::ErrorInfo<NativeTranslationSessionError> {
public:
  static char ID;

  NativeTranslationSessionError(NativeTranslationSessionErrorCode Code,
                                std::string Detail = {});

  NativeTranslationSessionErrorCode code() const { return Code; }
  llvm::StringRef detail() const { return Detail; }

  void log(llvm::raw_ostream &OS) const override;
  std::error_code convertToErrorCode() const override;

private:
  NativeTranslationSessionErrorCode Code;
  std::string Detail;
};

/// Native x86-64 to AArch64 execution session, version 1.
///
/// run(), restoreState(), state(), and options() are single-owner operations.
/// requestCancellation() is the sole operation permitted concurrently with
/// run().  A successful run commits register and authoritative guest-memory
/// snapshots together; infrastructure failures leave the logical state
/// unchanged.  A cancellation request accepted before the final commit mutex
/// is acquired wins the reported result while preserving already executed
/// guest side effects; requests made after the run commits are ignored.
class NativeTranslationSessionV1 final : public TranslationSession {
public:
  static llvm::Expected<std::unique_ptr<NativeTranslationSessionV1>>
  create(TranslationOptions Options, GuestState State,
         TranslationSemanticPolicyV1 Semantic =
             TranslationSemanticPolicyV1::unlimited());

  NativeTranslationSessionV1(const NativeTranslationSessionV1 &) = delete;
  NativeTranslationSessionV1 &
  operator=(const NativeTranslationSessionV1 &) = delete;
  NativeTranslationSessionV1(NativeTranslationSessionV1 &&) = delete;
  NativeTranslationSessionV1 &operator=(NativeTranslationSessionV1 &&) = delete;
  ~NativeTranslationSessionV1() override;

  const TranslationOptions &options() const override;
  const GuestState &state() const override;
  llvm::Error restoreState(GuestState State) override;
  llvm::Expected<TranslationResult> run() override;
  void requestCancellation() override;

private:
  struct Impl;

  explicit NativeTranslationSessionV1(std::unique_ptr<Impl> State);

  std::unique_ptr<Impl> State;
};

} // namespace neverd::translate

#endif // NEVERD_TRANSLATE_NATIVETRANSLATIONSESSION_H
