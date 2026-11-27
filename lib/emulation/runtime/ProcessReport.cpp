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
#include "ProcessTraceJSON.h"
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
llvm::json::Value bits(std::optional<uint64_t> Value) {
  return Value ? llvm::json::Value(bits(*Value)) : nullptr;
}
llvm::json::Value faultJSON(const std::optional<BackendFault> &Fault) {
  if (!Fault)
    return nullptr;
  llvm::json::Object Object{{field::Kind, backendFaultKindName(Fault->Kind)},
                            {field::PC, bits(Fault->PC)}};
  Object[field::Address] = bits(Fault->Address);
  Object[field::Size] = Fault->Size ? llvm::json::Value(*Fault->Size) : nullptr;
  Object[field::Access] =
      Fault->Access ? llvm::json::Value(backendAccessKindName(*Fault->Access))
                    : nullptr;
  Object[field::Interrupt] =
      Fault->Interrupt ? llvm::json::Value(*Fault->Interrupt) : nullptr;
  Object[field::ErrorCode] = bits(Fault->ErrorCode);
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
  if (Options.Android && Options.Android->TextLogWrites)
    for (const auto &Write : *Options.Android->TextLogWrites)
      EncodedPayloadBytes += Write.Payload.size() * 2;
  if (Options.LinuxFiles)
    for (const auto &[Path, Bytes] : Options.LinuxFiles->Files)
      EncodedPayloadBytes += Bytes.size() * 2;
  if (EncodedPayloadBytes > Text.size() ||
      Text.size() - EncodedPayloadBytes > field::JSONMetadataLimit)
    return invalid(field::TooLarge);
  return Options;
}

void writeHexArray(llvm::json::OStream &J, llvm::StringRef Key,
                   llvm::ArrayRef<uint64_t> Values) {
  J.attributeArray(Key, [&] {
    for (uint64_t Value : Values)
      J.value(bits(Value));
  });
}

void writeNativeCalls(llvm::json::OStream &J, const ProcessResult &Result) {
  const bool Android = Result.Profile == ProcessProfile::AndroidNativeAArch64;
  J.attributeArray(field::NativeCalls, [&] {
    for (const auto &Event : Result.NativeCalls) {
      J.object([&] {
        llvm::ArrayRef<uint64_t> Args(Event.Arguments);
        writeHexArray(J, field::Arguments,
                      Android ? Args : Args.take_front(Event.ArgumentCount));
        if (Result.Profile == ProcessProfile::WindowsPE64 &&
            Event.DirectServiceNumber)
          J.attribute(field::DirectServiceNumber,
                      bits(*Event.DirectServiceNumber));
        if (Android && !Event.Library.empty())
          J.attribute(field::Library, Event.Library);
        if (!Android)
          J.attribute(field::Module, Event.Module);
        J.attribute(field::Name, Event.Name);
        J.attribute(field::PC, bits(Event.PC));
        J.attribute(field::Result, bits(Event.Result));
        if (Android && !Event.Symbol.empty())
          J.attribute(field::Symbol, Event.Symbol);
        if (Android && Event.ThreadID)
          J.attribute(field::ThreadID, *Event.ThreadID);
      });
    }
  });
}

namespace {
void writeIoctls(llvm::json::OStream &J, const ProcessResult &Result) {
  J.attributeArray(field::IoctlTrace, [&] {
    for (const auto &Event : Result.IoctlTrace) {
      J.object([&] {
        J.attribute(field::DeviceArgument, bits(Event.Argument));
        J.attribute(field::IoctlTraceFD, Event.FD);
        J.attributeArray(field::Memory, [&] {
          for (const auto &M : Event.Memory)
            J.value(llvm::json::Object{
                {field::Address, bits(M.Address)},
                {field::Size, M.Size},
                {field::IoctlTraceBefore, llvm::toHex(M.Before, true)},
                {field::IoctlTraceAfter, llvm::toHex(M.After, true)},
                {field::IoctlTraceBeforeStatus, M.BeforeStatus},
                {field::IoctlTraceAfterStatus, M.AfterStatus}});
        });
        J.attribute(field::PC, bits(Event.PC));
        J.attribute(field::DeviceRequest, bits(Event.Request));
        J.attribute(field::Result, bits(Event.Result));
        if (Event.ReturnAddress)
          J.attribute(field::ReturnAddress, bits(*Event.ReturnAddress));
        J.attribute(field::ThreadID, Event.ThreadID);
      });
    }
  });
  J.attribute(field::IoctlTraceTruncated, Result.IoctlTraceTruncated);
}

void writeServices(llvm::json::OStream &J, const ProcessResult &Result) {
  J.attributeArray(field::Services, [&] {
    for (const auto &Event : Result.Services) {
      J.object([&] {
        writeHexArray(J, field::Arguments, Event.Arguments);
        if (Event.Error)
          J.attribute(field::Error, *Event.Error);
        J.attribute(field::Number, bits(Event.Number));
        J.attribute(field::PC, bits(Event.PC));
        J.attribute(field::Result, bits(Event.Result));
        if (Event.ThreadID)
          J.attribute(field::ThreadID, *Event.ThreadID);
      });
    }
  });
}
} // namespace

void writeProcessResultJSON(llvm::raw_ostream &OS,
                            const ProcessResult &Result) {
  llvm::json::OStream J(OS);
  // Keep the lexical key order of the original llvm::json::Object reports.
  J.object([&] {
    const bool Android = Result.Profile == ProcessProfile::AndroidNativeAArch64;
    const bool Windows = Result.Profile == ProcessProfile::WindowsPE64;
    if (Android)
      J.attributeObject(field::Android,
                        [&] { writeAndroidResultFields(J, Result); });
    J.attribute(field::Architecture,
                guestArchitectureName(Result.Architecture));
    J.attribute(field::Backend, executionBackendName(Result.SelectedBackend));
    J.attribute(field::SelectionReason, Result.BackendSelectionReason);
    if (Result.LastCPUExit) {
      const auto &Exit = *Result.LastCPUExit;
      J.attribute(
          field::CPUExit,
          llvm::json::Object{{field::Kind, executionExitKindName(Exit.Kind)},
                             {field::Diagnostic, Exit.Diagnostic},
                             {field::Fault, faultJSON(Exit.Fault)},
                             {field::StopRequested, Exit.StopRequested},
                             {field::DeadlineReached, Exit.DeadlineReached}});
    } else {
      J.attribute(field::CPUExit, nullptr);
    }
    if (!Result.NativeCalls.empty() &&
        (Result.Profile == ProcessProfile::MacOSMachO64 ||
         Result.Profile == ProcessProfile::IOSMachO64 ||
         Result.Profile == ProcessProfile::IOSSimulatorMachO64))
      J.attributeObject(field::Darwin, [&] { writeNativeCalls(J, Result); });
    J.attribute(field::Diagnostic, Result.Diagnostic);
    J.attribute(field::Entry, bits(Result.Entry));
    J.attribute(field::Events, Result.Events);
    J.attribute(field::ExitStatus, Result.ExitStatus);
    J.attribute(field::Instructions, Result.Instructions);
    if (Result.IoctlTraceEnabled)
      writeIoctls(J, Result);
    J.attribute(field::PC, bits(Result.PC));
    J.attribute(field::Profile, processProfileName(Result.Profile));
    if (Android || Windows)
      J.attribute(field::ReturnValue, bits(Result.ReturnValue));
    J.attribute(field::Version, field::SchemaVersion);
    writeServices(J, Result);
    J.attribute(field::Stderr, llvm::toHex(Result.StandardError, true));
    J.attribute(field::Stdout, llvm::toHex(Result.StandardOutput, true));
    J.attribute(field::Stop, processStopReasonName(Result.Stop));
    if (Result.TerminationSignal)
      J.attribute(field::TerminationSignal, *Result.TerminationSignal);
    if (Windows)
      J.attributeObject(field::Windows, [&] {
        J.attribute(field::Initialize, Result.InitializersEnabled);
        writeNativeCalls(J, Result);
      });
  });
}

std::string processResultJSON(const ProcessResult &Result) {
  std::string Text;
  llvm::raw_string_ostream OS(Text);
  writeProcessResultJSON(OS, Result);
  return Text;
}
} // namespace neverd::emulation
