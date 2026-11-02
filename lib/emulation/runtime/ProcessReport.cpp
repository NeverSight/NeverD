//===- ProcessReport.cpp - Process request and result serialization ------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
#include "neverd/emulation/ProcessReport.h"

#include "ProcessAndroidJSON.h"
#include "ProcessDarwinFilesJSON.h"
#include "ProcessDarwinJSON.h"
#include "ProcessDarwinSystemJSON.h"
#include "ProcessDarwinTimeJSON.h"
#include "ProcessLinuxFilesJSON.h"
#include "ProcessLinuxKernelJSON.h"
#include "ProcessLinuxPriorityJSON.h"
#include "ProcessLinuxSignalsJSON.h"
#include "ProcessLinuxTimeJSON.h"
#include "ProcessNetworkJSON.h"
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
  Object[field::ErrorCode] =
      Fault->ErrorCode ? llvm::json::Value(bits(*Fault->ErrorCode)) : nullptr;
  Object[field::Cause] =
      Fault->Cause ? llvm::json::Value(backendFaultCauseName(*Fault->Cause))
                   : nullptr;
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
    if (Name == field::Darwin) {
      auto Darwin = darwinOptionsFromJSON(V);
      if (!Darwin)
        return Darwin.takeError();
      Options.Darwin = std::move(*Darwin);
      continue;
    }
    if (Name == field::DarwinSystem) {
      auto System = darwinSystemOptionsFromJSON(V);
      if (!System)
        return System.takeError();
      Options.DarwinSystem = std::move(*System);
      continue;
    }
    if (Name == field::DarwinTime) {
      auto Time = darwinTimeOptionsFromJSON(V);
      if (!Time)
        return Time.takeError();
      Options.DarwinTime = std::move(*Time);
      continue;
    }
    if (Name == field::DarwinFiles) {
      auto Files = darwinFileOptionsFromJSON(V);
      if (!Files)
        return Files.takeError();
      Options.DarwinFiles = std::move(*Files);
      continue;
    }
    if (Name == field::LinuxKernel) {
      auto Kernel = linuxKernelOptionsFromJSON(V);
      if (!Kernel)
        return Kernel.takeError();
      Options.LinuxKernel = std::move(*Kernel);
      continue;
    }
    if (Name == field::LinuxNetwork) {
      auto Network = linuxNetworkOptionsFromJSON(V);
      if (!Network)
        return Network.takeError();
      Options.LinuxNetwork = std::move(*Network);
      continue;
    }
    if (Name == field::LinuxPriority) {
      auto Priority = linuxPriorityOptionsFromJSON(V);
      if (!Priority)
        return Priority.takeError();
      Options.LinuxPriority = std::move(*Priority);
      continue;
    }
    if (Name == field::LinuxSignals) {
      auto Signals = linuxSignalOptionsFromJSON(V);
      if (!Signals)
        return Signals.takeError();
      Options.LinuxSignals = std::move(*Signals);
      continue;
    }
    if (Name == field::LinuxFiles) {
      auto Files = linuxFileOptionsFromJSON(V);
      if (!Files)
        return Files.takeError();
      Options.LinuxFiles = std::move(*Files);
      continue;
    }
    if (Name == field::LinuxTime) {
      auto Time = linuxTimeOptionsFromJSON(V);
      if (!Time)
        return Time.takeError();
      Options.LinuxTime = std::move(*Time);
      continue;
    }
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
    if (Name == field::Contract) {
      auto Text = V.getAsString();
      if (!Text)
        return invalid(field::FieldType, Name);
      auto Contract = parseExecutionContract(*Text);
      if (!Contract)
        return Contract.takeError();
      Options.Contract = *Contract;
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
  // Only payloads already admitted by their decoded-byte owners earn wire
  // capacity. Paths, other binary fields, whitespace and JSON escape
  // overhead remain metadata; no field name alone grants an exemption.
  uint64_t EncodedPayloadBytes = 0;
  if (Options.Android)
    for (const auto &Image : Options.Android->DexImages)
      EncodedPayloadBytes += Image.size() * 2;
  if (Options.Android && Options.Android->EventLogWrites)
    for (const auto &Write : *Options.Android->EventLogWrites)
      EncodedPayloadBytes += Write.Payload.size() * 2;
  if (Options.LinuxFiles)
    for (const auto &[Path, Bytes] : Options.LinuxFiles->Files)
      EncodedPayloadBytes += Bytes.size() * 2;
  if (EncodedPayloadBytes > Text.size() ||
      Text.size() - EncodedPayloadBytes > field::JSONMetadataLimit)
    return invalid(field::TooLarge);
  return Options;
}

std::string processResultJSON(const ProcessResult &Result) {
  llvm::json::Array Services;
  for (const auto &Event : Result.Services) {
    llvm::json::Array Arguments;
    for (uint64_t Value : Event.Arguments)
      Arguments.push_back(bits(Value));
    llvm::json::Object Service{
        {field::PC, bits(Event.PC)},
        {field::Number, bits(Event.Number)},
        {field::Arguments, std::move(Arguments)},
        {field::Result,
         Event.Result ? llvm::json::Value(bits(*Event.Result)) : nullptr}};
    if (Event.Error)
      Service[field::Error] = *Event.Error;
    if (Event.ThreadID)
      Service[field::ThreadID] = *Event.ThreadID;
    Services.push_back(std::move(Service));
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
  if (Result.IoctlTraceEnabled) {
    llvm::json::Array Calls;
    for (const auto &Event : Result.IoctlTrace) {
      llvm::json::Array Memory;
      for (const auto &M : Event.Memory)
        Memory.push_back(llvm::json::Object{
            {field::Address,
             M.Address ? llvm::json::Value(bits(*M.Address)) : nullptr},
            {field::Size, M.Size},
            {field::IoctlTraceBefore, llvm::toHex(M.Before, true)},
            {field::IoctlTraceAfter, llvm::toHex(M.After, true)},
            {field::IoctlTraceBeforeStatus, M.BeforeStatus},
            {field::IoctlTraceAfterStatus, M.AfterStatus}});
      llvm::json::Object Call{
          {field::PC, bits(Event.PC)},
          {field::DeviceRequest, bits(Event.Request)},
          {field::DeviceArgument, bits(Event.Argument)},
          {field::ThreadID, Event.ThreadID},
          {field::IoctlTraceFD, Event.FD},
          {field::Result,
           Event.Result ? llvm::json::Value(bits(*Event.Result)) : nullptr},
          {field::Memory, std::move(Memory)}};
      if (Event.ReturnAddress)
        Call[field::ReturnAddress] = bits(*Event.ReturnAddress);
      Calls.push_back(std::move(Call));
    }
    Object[field::IoctlTrace] = std::move(Calls);
    Object[field::IoctlTraceTruncated] = Result.IoctlTraceTruncated;
  }
  if (Result.TerminationSignal)
    Object[field::TerminationSignal] = *Result.TerminationSignal;
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
      llvm::json::Object Call{
          {field::PC, bits(Event.PC)},
          {field::Module, Event.Module},
          {field::Name, Event.Name},
          {field::Arguments, std::move(Arguments)},
          {field::Result,
           Event.Result ? llvm::json::Value(bits(*Event.Result)) : nullptr}};
      if (Event.DirectServiceNumber)
        Call[field::DirectServiceNumber] = bits(*Event.DirectServiceNumber);
      Calls.push_back(std::move(Call));
    }
    Object[field::Windows] =
        llvm::json::Object{{field::Initialize, Result.InitializersEnabled},
                           {field::NativeCalls, std::move(Calls)}};
    Object[field::ReturnValue] =
        Result.ReturnValue ? llvm::json::Value(bits(*Result.ReturnValue))
                           : nullptr;
  }
  if (!Result.NativeCalls.empty() &&
      (Result.Profile == ProcessProfile::MacOSMachO64 ||
       Result.Profile == ProcessProfile::IOSMachO64 ||
       Result.Profile == ProcessProfile::IOSSimulatorMachO64)) {
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
    Object[field::Darwin] =
        llvm::json::Object{{field::NativeCalls, std::move(Calls)}};
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
