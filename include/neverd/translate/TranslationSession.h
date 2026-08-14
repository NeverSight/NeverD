//===- TranslationSession.h - Translation execution boundary -*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#ifndef NEVERD_TRANSLATE_TRANSLATIONSESSION_H
#define NEVERD_TRANSLATE_TRANSLATIONSESSION_H

#include "neverd/translate/GuestState.h"
#include "neverd/translate/TranslationOptions.h"
#include "neverd/translate/TranslationResult.h"

#include "llvm/Support/Error.h"

namespace neverd::translate {

/// Validate the modeled state/policy boundary before any code is translated.
llvm::Error validateTranslationRequest(const TranslationOptions &Options,
                                       const GuestState &State);

/// Abstract in-process execution interface.  GuestState remains a logical and
/// debug snapshot at this boundary; generated code may consume only a
/// backend-private, size-and-version-checked fixed-layout runtime state.  AOT
/// translation is a separate compiler-to-artifact operation, not a runnable
/// session.  The interface does not imply that an executable engine factory is
/// available.
class TranslationSession {
public:
  virtual ~TranslationSession() = default;

  virtual const TranslationOptions &options() const = 0;
  virtual const GuestState &state() const = 0;
  virtual llvm::Error restoreState(GuestState State) = 0;
  virtual llvm::Expected<TranslationResult> run() = 0;
  virtual void requestCancellation() = 0;
};

} // namespace neverd::translate

#endif // NEVERD_TRANSLATE_TRANSLATIONSESSION_H
