//===- AndroidBionic.cpp - Bionic import dispatch ------------------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
#include "AndroidInternal.h"
#include "AndroidThreads.h"

#include "llvm/ADT/StringMap.h"

namespace neverd::emulation::android_model {
uint64_t Bionic::tlsAddress() const {
  return Threads ? Threads->tls() : TLSAddress;
}
uint64_t Bionic::threadID() const {
  return Threads ? Threads->id() : linux_model::ThreadID;
}
BinderThreadState &Bionic::binderThread() {
  return Threads ? Threads->binder() : SingleThreadBinder;
}
BionicResult Bionic::unsupportedImport(const NativeCallEvent &Call) {
  Result.Stop = ProcessStopReason::UnsupportedService;
  Result.Diagnostic = diagnostic::ImportPrefix + Call.Name;
  return std::optional<BionicValue>();
}

BionicResult Bionic::failCall(const NativeCallEvent &Call) {
  return failure(diagnostic::GuestCalledPrefix + Call.Name);
}

BionicResult Bionic::invoke(NativeCallEvent &Call) {
  if (Threads && Threads->enabled())
    if (auto E = Threads->validateTLS())
      return std::move(E);
  if (!Call.Library.empty()) {
    auto I = OpenLibraries.find(Call.Library);
    if (!isResident(Call.Library) &&
        (I == OpenLibraries.end() || !I->second.References)) {
      Result.Stop = ProcessStopReason::UnsupportedService;
      Result.Diagnostic = diagnostic::InactiveProviderPrefix + Call.Library;
      return std::optional<BionicValue>();
    }
  }

  using Handler = BionicResult (*)(Bionic &, NativeCallEvent &);
  static const llvm::StringMap<Handler> Handlers{
#define NEVERD_ANDROID_CALL(Symbol, Text, Method)                              \
  {Text, [](Bionic &Model, NativeCallEvent &Event) -> BionicResult {           \
     return Model.Method(Event);                                               \
   }},
#include "AndroidSymbols.def"
#undef NEVERD_ANDROID_CALL
#define NEVERD_ANDROID_KERNEL_SERVICE(Text, Kind)                              \
  {Text, [](Bionic &Model, NativeCallEvent &Event) -> BionicResult {           \
     return Model.kernelCall(linux_model::ServiceKind::Kind, Event);           \
   }},
#include "AndroidKernelServices.def"
#undef NEVERD_ANDROID_KERNEL_SERVICE
#define NEVERD_ANDROID_PATH_STATUS(Text, Flags)                                \
  {Text, [](Bionic &Model, NativeCallEvent &Event) -> BionicResult {           \
     auto Adapted = Event;                                                     \
     Adapted.Arguments = {linux_model::AtCurrentDirectory, Event.Arguments[0], \
                          Event.Arguments[1], Flags};                          \
     return Model.kernelCall(linux_model::ServiceKind::FstatAt, Adapted);      \
   }},
#include "AndroidKernelServices.def"
#undef NEVERD_ANDROID_PATH_STATUS
  };
  auto I = Handlers.find(Call.Name);
  if (I != Handlers.end())
    return I->second(*this, Call);

  struct FamilyHandler {
    llvm::StringLiteral Prefix;
    Handler Invoke;
  };
  static constexpr FamilyHandler Families[] = {
#define NEVERD_ANDROID_PREFIX(Symbol, Text, Method)                            \
  {Text, [](Bionic &Model, NativeCallEvent &Event) -> BionicResult {           \
     return Model.Method(Event);                                               \
   }},
#include "AndroidSymbols.def"
#undef NEVERD_ANDROID_PREFIX
  };
  for (const auto &Family : Families)
    if (llvm::StringRef(Call.Name).starts_with(Family.Prefix))
      return Family.Invoke(*this, Call);
  return unsupportedImport(Call);
}

} // namespace neverd::emulation::android_model
