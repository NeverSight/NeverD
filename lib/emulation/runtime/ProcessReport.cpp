//===- ProcessReport.cpp - Process request and result serialization ------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
#include "neverd/emulation/ProcessReport.h"

#include "ProcessAndroidJSON.h"
#include "ProcessWindowsJSON.h"

#include "neverd/emulation/ProcessReportFields.h"

#include "llvm/ADT/StringExtras.h"
#include "llvm/Support/JSON.h"
#include "llvm/Support/raw_ostream.h"

namespace neverd::emulation {
namespace {
namespace field = process_report;
llvm::Error invalid(llvm::StringRef Reason, llvm::StringRef Detail = {}) {
  return llvm::createStringError(llvm::inconvertibleErrorCode(),
                                 llvm::Twine(Reason) + Detail);
}
std::string bits(uint64_t Value) { return llvm::utohexstr(Value, true); }
llvm::json::Value faultJSON(const std::optional<BackendFault> &Fault) {
  if (!Fault)
    return nullptr;
  llvm::json::Object Object{{field::Kind, backendFaultKindName(Fault->Kind)},
                            {field::PC, bits(Fault->PC)}};
  Object[field::Address] =
      Fault->Address ? llvm::json::Value(bits(*Fault->Address)) : nullptr;
  Object[field::Size] = Fault->Size ? llvm::json::Value(*Fault->Size) : nullptr;
  Object[field::Access] =
      Fault->Access ? llvm::json::Value(backendAccessKindName(*Fault->Access))
                    : nullptr;
  Object[field::Interrupt] =
      Fault->Interrupt ? llvm::json::Value(*Fault->Interrupt) : nullptr;
  return Object;
}
} // namespace

llvm::Expected<ProcessOptions> processOptionsFromJSON(llvm::StringRef Text) {
  if (Text.size() > field::JSONLimit)
    return invalid(field::TooLarge);
  auto Value = llvm::json::parse(Text);
  if (!Value)
    return Value.takeError();
  const auto *Object = Value->getAsObject();
  if (!Object)
    return invalid(field::ObjectRequired);
  ProcessOptions Options;
  for (const auto &[Key, V] : *Object) {
    const llvm::StringRef Name = Key;
    if (Name == field::Windows) {
      auto Windows = windowsOptionsFromJSON(V);
      if (!Windows)
        return Windows.takeError();
      Options.Windows = std::move(*Windows);
      continue;
    }
    if (Name == field::Android) {
      auto Native = androidOptionsFromJSON(V);
      if (!Native)
        return Native.takeError();
      Options.Android = std::move(*Native);
      continue;
    }
    if (Name == field::Backend) {
      auto Text = V.getAsString();
      if (!Text)
        return invalid(field::FieldType, Name);
      auto Backend = parseExecutionBackend(*Text);
      if (!Backend)
        return Backend.takeError();
      Options.Backend = *Backend;
      continue;
    }
#define NEVERD_PROCESS_OPTION_NUMBER(ID, Text, Member)                         \
  if (Name == field::ID) {                                                     \
    auto Number = V.getAsUINT64();                                             \
    if (!Number || !*Number)                                                   \
      return invalid(field::FieldType, Name);                                  \
    Options.Member = *Number;                                                  \
    continue;                                                                  \
  }
#define NEVERD_PROCESS_OPTION_STRINGS(ID, Text, Member)                        \
  if (Name == field::ID) {                                                     \
    const auto *Array = V.getAsArray();                                        \
    if (!Array)                                                                \
      return invalid(field::FieldType, Name);                                  \
    for (const auto &Entry : *Array) {                                         \
      auto String = Entry.getAsString();                                       \
      if (!String || String->contains('\0'))                                   \
        return invalid(field::FieldType, Name);                                \
      Options.Member.push_back(String->str());                                 \
    }                                                                          \
    continue;                                                                  \
  }
#include "neverd/emulation/ProcessReport.def"
#undef NEVERD_PROCESS_OPTION_STRINGS
#undef NEVERD_PROCESS_OPTION_NUMBER
    return invalid(field::UnknownField, Name);
  }
  return Options;
}

std::string processResultJSON(const ProcessResult &Result) {
  llvm::json::Array Services;
  for (const auto &Event : Result.Services) {
    llvm::json::Array Arguments;
    for (uint64_t Value : Event.Arguments)
      Arguments.push_back(bits(Value));
    Services.push_back(llvm::json::Object{
        {field::PC, bits(Event.PC)},
        {field::Number, bits(Event.Number)},
        {field::Arguments, std::move(Arguments)},
        {field::Result,
         Event.Result ? llvm::json::Value(bits(*Event.Result)) : nullptr}});
  }
  llvm::json::Object Object{
      {field::Version, field::SchemaVersion},
      {field::Profile, processProfileName(Result.Profile)},
      {field::Architecture, guestArchitectureName(Result.Architecture)},
      {field::Backend, executionBackendName(Result.SelectedBackend)},
      {field::SelectionReason, Result.BackendSelectionReason},
      {field::Stop, processStopReasonName(Result.Stop)},
      {field::ExitStatus,
       Result.ExitStatus ? llvm::json::Value(*Result.ExitStatus) : nullptr},
      {field::Diagnostic, Result.Diagnostic},
      {field::Entry, bits(Result.Entry)},
      {field::PC, bits(Result.PC)},
      {field::Instructions, Result.Instructions},
      {field::Events, Result.Events},
      {field::Stdout, llvm::toHex(Result.StandardOutput, true)},
      {field::Stderr, llvm::toHex(Result.StandardError, true)},
      {field::Services, std::move(Services)},
      {field::CPUExit, nullptr}};
  if (Result.Profile == ProcessProfile::AndroidNativeAArch64) {
    Object[field::Android] = androidResultJSON(Result);
    Object[field::ReturnValue] =
        Result.ReturnValue ? llvm::json::Value(bits(*Result.ReturnValue))
                           : nullptr;
  }
  if (Result.Profile == ProcessProfile::WindowsPE64) {
    llvm::json::Array Calls;
    for (const auto &Event : Result.NativeCalls) {
      llvm::json::Array Arguments;
      for (unsigned I = 0; I < Event.ArgumentCount; ++I)
        Arguments.push_back(bits(Event.Arguments[I]));
      Calls.push_back(llvm::json::Object{
          {field::PC, bits(Event.PC)},
          {field::Module, Event.Module},
          {field::Name, Event.Name},
          {field::Arguments, std::move(Arguments)},
          {field::Result,
           Event.Result ? llvm::json::Value(bits(*Event.Result)) : nullptr}});
    }
    Object[field::Windows] =
        llvm::json::Object{{field::Initialize, Result.InitializersEnabled},
                           {field::NativeCalls, std::move(Calls)}};
    Object[field::ReturnValue] =
        Result.ReturnValue ? llvm::json::Value(bits(*Result.ReturnValue))
                           : nullptr;
  }
  if (Result.LastCPUExit) {
    const auto &Exit = *Result.LastCPUExit;
    Object[field::CPUExit] =
        llvm::json::Object{{field::Kind, executionExitKindName(Exit.Kind)},
                           {field::Diagnostic, Exit.Diagnostic},
                           {field::Fault, faultJSON(Exit.Fault)},
                           {field::StopRequested, Exit.StopRequested},
                           {field::DeadlineReached, Exit.DeadlineReached}};
  }
  std::string Text;
  llvm::raw_string_ostream OS(Text);
  OS << llvm::json::Value(std::move(Object));
  return Text;
}
} // namespace neverd::emulation
