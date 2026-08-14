//===- TranslationSession.cpp - Translation execution boundary -----------===//

#include "neverd/translate/TranslationSession.h"

#include "llvm/Support/Errc.h"

namespace neverd::translate {
namespace {

llvm::Error invalid(llvm::StringRef Message) {
  return llvm::createStringError(llvm::errc::invalid_argument, "%s",
                                 Message.str().c_str());
}

} // namespace

llvm::Error validateTranslationRequest(const TranslationOptions &Options,
                                       const GuestState &State) {
  if (llvm::Error Error = validateTranslationOptions(Options))
    return Error;
  if (State.Architecture != Options.Guest)
    return invalid("state architecture does not match translation options");
  if (llvm::Error Error = validateGuestState(State))
    return Error;

  if (Options.CodeInvalidation ==
      CodeInvalidationPolicy::RejectExecutableWrites) {
    for (const GuestMemoryRegion &Region : State.Memory) {
      if (hasPermission(Region.Permissions, MemoryPermission::Write) &&
          hasPermission(Region.Permissions, MemoryPermission::Execute))
        return invalid("writable executable guest memory is rejected by the "
                       "code-invalidation policy");
    }
  }
  return llvm::Error::success();
}

} // namespace neverd::translate
