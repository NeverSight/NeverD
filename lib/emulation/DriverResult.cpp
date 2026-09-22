//===- DriverResult.cpp - Driver execution report -------------------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Driver execution report.
///
//===----------------------------------------------------------------------===//

#include "neverd/emulation/DriverReportFields.h"
#include "neverd/emulation/DriverSession.h"

#include "llvm/ADT/STLExtras.h"
#include "llvm/ADT/StringExtras.h"
#include "llvm/Support/ErrorHandling.h"
#include "llvm/Support/JSON.h"
#include "llvm/Support/raw_ostream.h"

namespace neverd::emulation {
namespace {

namespace resourceField {
#define NEVERD_DRIVER_RESOURCE_FIELD(Name, Spelling)                           \
  constexpr llvm::StringLiteral Name = Spelling;
#include "neverd/emulation/DriverResources.def"
#undef NEVERD_DRIVER_RESOURCE_FIELD
} // namespace resourceField

namespace interruptField {
#define NEVERD_DRIVER_INTERRUPT_FIELD(Name, Spelling)                          \
  constexpr llvm::StringLiteral Name = Spelling;
#include "neverd/emulation/DriverInterrupts.def"
#undef NEVERD_DRIVER_INTERRUPT_FIELD
} // namespace interruptField

const char *requestKindName(DriverRequestKind Kind) {
  switch (Kind) {
#define NEVERD_DRIVER_REQUEST_KIND(Name, Spelling, Major)                      \
  case DriverRequestKind::Name:                                                \
    return Spelling;
#include "neverd/emulation/DriverRequestKinds.def"
#undef NEVERD_DRIVER_REQUEST_KIND
  }
  llvm_unreachable("invalid driver request kind");
}

bool scenarioSucceeded(const DriverResult &Result) {
  size_t InterruptCount = 0;
  for (const auto &Request : Result.Configuration.Requests)
    InterruptCount += Request.InterruptEvents.size();
  const size_t ScenarioRequests =
      llvm::count_if(Result.Requests, [](const DriverRequestResult &Request) {
        return Request.Origin == DriverRequestOrigin::Scenario;
      });
  if (Result.Stop != DriverStopReason::Returned || !Result.NTStatus ||
      *Result.NTStatus != 0 ||
      ScenarioRequests != Result.Configuration.Requests.size() ||
      InterruptCount != Result.Interrupts.size() ||
      Result.PnpDevices.size() != Result.Configuration.PnpDevices.size() ||
      (Result.Configuration.Unload && !Result.UnloadCompleted))
    return false;
  if (!llvm::all_of(Result.Interrupts, [](const DriverInterruptResult &Event) {
        return Event.OccurredAt100ns && Event.DeliveredAt100ns &&
               Event.ReturnedAt100ns && Event.ReturnValue &&
               !Event.UndeliveredReason;
      }))
    return false;
  if (!llvm::all_of(Result.PnpDevices, [](const DriverPnpDeviceResult &Device) {
        return Device.AddDeviceStatus &&
               !(*Device.AddDeviceStatus & profile::NTStatusFailureMask);
      }))
    return false;
  return llvm::all_of(Result.Requests, [](const DriverRequestResult &Request) {
    return Request.Completed && Request.DispatchStatus && Request.IOStatus &&
           !(*Request.DispatchStatus & profile::NTStatusFailureMask) &&
           !(*Request.IOStatus & profile::NTStatusFailureMask);
  });
}

llvm::json::Value
registryJSON(const std::optional<std::vector<DriverRegistryKey>> &Registry) {
  if (!Registry)
    return nullptr;
  llvm::json::Array Keys;
  for (const auto &Key : *Registry) {
    llvm::json::Array Values;
    for (const auto &Value : Key.Values)
      Values.push_back(llvm::json::Object{
          {field::Name, Value.Name},
          {field::Type, Value.Type},
          {field::Data,
           llvm::toHex(llvm::ArrayRef<uint8_t>(Value.Data), true)}});
    Keys.push_back(llvm::json::Object{{field::Path, Key.Path},
                                      {field::Values, std::move(Values)}});
  }
  return Keys;
}

const char *busKindName(DriverBusKind Kind) {
  switch (Kind) {
#define NEVERD_DRIVER_BUS_KIND(Name, Spelling)                                 \
  case DriverBusKind::Name:                                                    \
    return Spelling;
#include "neverd/emulation/DriverPnpNames.def"
#undef NEVERD_DRIVER_BUS_KIND
  }
  llvm_unreachable("invalid driver bus kind");
}

const char *pnpRequestName(DevicePnpRequest Minor) {
  switch (Minor) {
#define NEVERD_DRIVER_PNP_REQUEST(Name, Spelling)                              \
  case DevicePnpRequest::Name:                                                 \
    return Spelling;
#include "neverd/emulation/DriverPnpNames.def"
#undef NEVERD_DRIVER_PNP_REQUEST
  default:
    llvm_unreachable("unsupported driver PnP request");
  }
}

const char *pnpStateName(DevicePnpState State) {
  switch (State) {
#define NEVERD_DRIVER_PNP_STATE(Name, Spelling)                                \
  case DevicePnpState::Name:                                                   \
    return Spelling;
#include "neverd/emulation/DriverPnpNames.def"
#undef NEVERD_DRIVER_PNP_STATE
  }
  llvm_unreachable("invalid driver PnP state");
}

const char *devicePowerName(DevicePowerState State) {
  switch (State) {
#define NEVERD_DRIVER_DEVICE_POWER(Name, Spelling)                             \
  case DevicePowerState::Name:                                                 \
    return Spelling;
#include "neverd/emulation/DriverPnpNames.def"
#undef NEVERD_DRIVER_DEVICE_POWER
  }
  llvm_unreachable("invalid driver device power state");
}

const char *systemPowerName(SystemPowerState State) {
  switch (State) {
#define NEVERD_DRIVER_SYSTEM_POWER(Name, Spelling)                             \
  case SystemPowerState::Name:                                                 \
    return Spelling;
#include "neverd/emulation/DriverPnpNames.def"
#undef NEVERD_DRIVER_SYSTEM_POWER
  }
  llvm_unreachable("invalid driver system power state");
}

const char *powerRequestName(DevicePowerRequest Minor) {
  switch (Minor) {
#define NEVERD_DRIVER_POWER_REQUEST(Name, Spelling)                            \
  case DevicePowerRequest::Name:                                               \
    return Spelling;
#include "neverd/emulation/DriverPower.def"
#undef NEVERD_DRIVER_POWER_REQUEST
  }
  llvm_unreachable("invalid driver power request");
}

const char *powerTypeName(DriverPowerType Type) {
  switch (Type) {
#define NEVERD_DRIVER_POWER_TYPE(Name, Value, Spelling)                        \
  case DriverPowerType::Name:                                                  \
    return Spelling;
#include "neverd/emulation/DriverPower.def"
#undef NEVERD_DRIVER_POWER_TYPE
  }
  llvm_unreachable("invalid driver power type");
}

const char *powerActionName(DriverPowerAction Action) {
  switch (Action) {
#define NEVERD_DRIVER_POWER_ACTION(Name, Value, Spelling)                      \
  case DriverPowerAction::Name:                                                \
    return Spelling;
#include "neverd/emulation/DriverPower.def"
#undef NEVERD_DRIVER_POWER_ACTION
  }
  llvm_unreachable("invalid driver power action");
}

const char *requestOriginName(DriverRequestOrigin Origin) {
  switch (Origin) {
#define NEVERD_DRIVER_REQUEST_ORIGIN(Name, Spelling)                           \
  case DriverRequestOrigin::Name:                                              \
    return Spelling;
#include "neverd/emulation/DriverPower.def"
#undef NEVERD_DRIVER_REQUEST_ORIGIN
  }
  llvm_unreachable("invalid driver request origin");
}

const char *powerStateName(DriverPowerType Type, uint32_t State) {
  switch (Type) {
  case DriverPowerType::Device:
    return devicePowerName(static_cast<DevicePowerState>(State));
  case DriverPowerType::System:
    return systemPowerName(static_cast<SystemPowerState>(State));
  }
  llvm_unreachable("invalid driver power type");
}

llvm::json::Object powerOperationJSON(const DriverPowerOperation &Operation) {
  llvm::json::Object Bus{
      {field::Status, nullptr},
      {field::Delay100ns, Operation.BusCompletion.Delay100ns}};
  if (Operation.BusCompletion.Status)
    Bus[field::Status] = *Operation.BusCompletion.Status;
  return llvm::json::Object{
      {field::Minor, powerRequestName(Operation.Minor)},
      {field::PowerType, powerTypeName(Operation.Type)},
      {field::PowerState, powerStateName(Operation.Type, Operation.State)},
      {field::PowerAction, powerActionName(Operation.Action)},
      {field::SystemContext, Operation.SystemContext},
      {field::BusCompletion, std::move(Bus)}};
}

const char *registerAccessName(DriverRegisterAccess Access) {
  switch (Access) {
#define NEVERD_DRIVER_REGISTER_ACCESS(Name, Spelling)                          \
  case DriverRegisterAccess::Name:                                             \
    return Spelling;
#include "neverd/emulation/DriverResources.def"
#undef NEVERD_DRIVER_REGISTER_ACCESS
  }
  llvm_unreachable("invalid driver register access");
}

llvm::json::Array
resourceConfigurationJSON(llvm::ArrayRef<DriverMemoryResource> Resources) {
  llvm::json::Array Result;
  for (const auto &Resource : Resources) {
    llvm::json::Array Registers;
    for (const auto &Register : Resource.Registers)
      Registers.push_back(llvm::json::Object{
          {resourceField::Offset, Register.Offset},
          {resourceField::Width, Register.Width},
          {resourceField::Access, registerAccessName(Register.Access)},
          {resourceField::Value, Register.Value}});
    Result.push_back(llvm::json::Object{
        {resourceField::ID, Resource.ID},
        {resourceField::RawStart,
         "0x" + llvm::utohexstr(Resource.RawStart, true)},
        {resourceField::TranslatedStart,
         "0x" + llvm::utohexstr(Resource.TranslatedStart, true)},
        {resourceField::Length, Resource.Length},
        {resourceField::Registers, std::move(Registers)}});
  }
  return Result;
}

const char *interruptModeName(DriverInterruptMode Mode) {
  switch (Mode) {
#define NEVERD_DRIVER_INTERRUPT_MODE(Name, Value, Spelling)                    \
  case DriverInterruptMode::Name:                                              \
    return Spelling;
#include "neverd/emulation/DriverInterrupts.def"
#undef NEVERD_DRIVER_INTERRUPT_MODE
  }
  llvm_unreachable("invalid driver interrupt mode");
}

const char *interruptShareName(DriverInterruptShare Share) {
  switch (Share) {
#define NEVERD_DRIVER_INTERRUPT_SHARE(Name, Value, Spelling)                   \
  case DriverInterruptShare::Name:                                             \
    return Spelling;
#include "neverd/emulation/DriverInterrupts.def"
#undef NEVERD_DRIVER_INTERRUPT_SHARE
  }
  llvm_unreachable("invalid driver interrupt share");
}

llvm::json::Array
interruptConfigurationJSON(llvm::ArrayRef<DriverInterruptResource> Interrupts) {
  llvm::json::Array Result;
  for (const auto &Interrupt : Interrupts)
    Result.push_back(llvm::json::Object{
        {interruptField::ID, Interrupt.ID},
        {interruptField::RawVector, Interrupt.RawVector},
        {interruptField::RawLevel, Interrupt.RawLevel},
        {interruptField::RawAffinity, Interrupt.RawAffinity},
        {interruptField::TranslatedVector, Interrupt.TranslatedVector},
        {interruptField::TranslatedLevel, Interrupt.TranslatedLevel},
        {interruptField::TranslatedAffinity, Interrupt.TranslatedAffinity},
        {interruptField::Mode, interruptModeName(Interrupt.Mode)},
        {interruptField::Share, interruptShareName(Interrupt.Share)}});
  return Result;
}

llvm::json::Array
interruptEventConfigurationJSON(const DriverOptions &Options) {
  llvm::json::Array Result;
  for (size_t I = 0; I < Options.Requests.size(); ++I) {
    const auto &Events = Options.Requests[I].InterruptEvents;
    for (size_t J = 0; J < Events.size(); ++J)
      Result.push_back(llvm::json::Object{
          {interruptField::SourceRequestIndex, I},
          {interruptField::EventIndex, J},
          {interruptField::After100ns, Events[J].After100ns},
          {interruptField::DeviceID, Events[J].DeviceID},
          {interruptField::InterruptID, Events[J].InterruptID}});
  }
  return Result;
}

llvm::json::Array pnpConfigurationJSON(const DriverOptions &Options) {
  llvm::json::Array Devices;
  for (const auto &Device : Options.PnpDevices) {
    llvm::json::Object Item{{field::ID, Device.ID},
                            {field::Bus, busKindName(Device.Bus)},
                            {field::InitialDevicePower, nullptr},
                            {field::InitialSystemPower, nullptr},
                            {field::InitialReportedDevicePower, nullptr}};
    if (Device.InitialDevicePower)
      Item[field::InitialDevicePower] =
          devicePowerName(*Device.InitialDevicePower);
    if (Device.InitialSystemPower)
      Item[field::InitialSystemPower] =
          systemPowerName(*Device.InitialSystemPower);
    if (Device.InitialReportedDevicePower)
      Item[field::InitialReportedDevicePower] =
          devicePowerName(*Device.InitialReportedDevicePower);
    llvm::json::Array Responses;
    for (const auto &Response : Device.RequestedDevicePower)
      Responses.push_back(powerOperationJSON(Response));
    Item[field::RequestedDevicePower] = std::move(Responses);
    if (Device.Bus == DriverBusKind::RegisterBank) {
      Item[resourceField::Resources] =
          resourceConfigurationJSON(Device.Resources);
      Item[interruptField::Interrupts] =
          interruptConfigurationJSON(Device.Interrupts);
    }
    Devices.push_back(std::move(Item));
  }
  return Devices;
}
} // namespace

const char *driverStopReasonName(DriverStopReason Reason) {
  switch (Reason) {
#define NEVERD_DRIVER_STOP(Name, Spelling)                                     \
  case DriverStopReason::Name:                                                 \
    return stop::Name;
#include "neverd/emulation/DriverStopReasons.def"
#undef NEVERD_DRIVER_STOP
  }
  llvm_unreachable("invalid driver stop reason");
}

std::string driverResultJSON(const DriverResult &Result) {
  auto Address = [](uint64_t V) { return "0x" + llvm::utohexstr(V); };
  llvm::json::Object Root{
      {field::SchemaVersion, 1},
      {field::Profile, profile::ReportProfile},
      {field::StopReason, driverStopReasonName(Result.Stop)},
      {field::PC, Address(Result.PC)},
      {field::Instructions, Result.Instructions},
      {field::ImageBase, Address(Result.ImageBase)},
      {field::PreferredImageBase, Address(Result.PreferredImageBase)},
      {field::SecurityCookie, Address(Result.SecurityCookieAddress)},
      {field::Phase, Result.Phase},
      {field::UnloadCompleted, Result.UnloadCompleted},
      {field::ScenarioSuccess, scenarioSucceeded(Result)},
      {field::Entry, Address(Result.Entry)},
      {field::DriverObject, Address(Result.DriverObject)},
      {field::DriverUnload, Address(Result.DriverUnload)},
      {field::AddDevice, Address(Result.AddDevice)},
      {field::Diagnostic, Result.Diagnostic},
      {field::NTStatus, nullptr},
      {field::NTSuccess, nullptr},
      {field::Fault, nullptr}};
  Root[field::Registry] = registryJSON(Result.Registry);
  llvm::json::Object Exports;
  for (const auto &[Name, Present] : Result.Configuration.KernelExports)
    Exports[Name] = Present;
  Root[field::Configuration] = llvm::json::Object{
      {field::ServiceName, Result.Configuration.ServiceName},
      {field::InstructionLimit, Result.Configuration.InstructionLimit},
      {field::MemoryLimit, Result.Configuration.MemoryLimit},
      {field::EventLimit, Result.Configuration.EventLimit},
      {field::TimeoutMilliseconds, Result.Configuration.TimeoutMilliseconds},
      {field::LoadAddress, Address(Result.Configuration.LoadAddress)},
      {field::Unload, Result.Configuration.Unload},
      {field::Registry, registryJSON(Result.Configuration.Registry)},
      {field::PnpDevices, pnpConfigurationJSON(Result.Configuration)},
      {interruptField::InterruptEvents,
       interruptEventConfigurationJSON(Result.Configuration)},
      {field::KernelExports, std::move(Exports)}};
  if (Result.Fault) {
    const auto &Fault = *Result.Fault;
    llvm::json::Object Item{
        {field::Kind, Fault.Kind}, {field::PC, Address(Fault.PC)},
        {field::Address, nullptr}, {field::Size, nullptr},
        {field::Access, nullptr},  {field::Interrupt, nullptr}};
    if (Fault.Address)
      Item[field::Address] = Address(*Fault.Address);
    if (Fault.Size)
      Item[field::Size] = *Fault.Size;
    if (Fault.Access)
      Item[field::Access] = *Fault.Access;
    if (Fault.Interrupt)
      Item[field::Interrupt] = *Fault.Interrupt;
    Root[field::Fault] = std::move(Item);
  }
  if (Result.NTStatus) {
    Root[field::NTStatus] = *Result.NTStatus;
    Root[field::NTSuccess] =
        (*Result.NTStatus & profile::NTStatusFailureMask) == 0;
  }
  llvm::json::Array Functions;
  for (uint64_t Function : Result.MajorFunctions)
    Functions.push_back(Address(Function));
  Root[field::MajorFunctions] = std::move(Functions);
  llvm::json::Array Devices;
  for (const auto &Device : Result.Devices) {
    llvm::json::Object Item{{field::Address, Address(Device.Address)},
                            {field::Extension, Address(Device.Extension)},
                            {field::Type, Device.Type},
                            {field::Name, Device.Name},
                            {field::ReportedDevicePower, nullptr}};
    if (Device.ReportedDevicePower)
      Item[field::ReportedDevicePower] =
          devicePowerName(*Device.ReportedDevicePower);
    Devices.push_back(std::move(Item));
  }
  Root[field::Devices] = std::move(Devices);
  llvm::json::Array PnpDevices;
  for (const auto &Device : Result.PnpDevices) {
    llvm::json::Object Item{
        {field::ID, Device.ID},
        {field::PDO, Address(Device.PDO)},
        {field::AddDeviceStatus, nullptr},
        {field::Attached, Device.Attached},
        {field::PnpState, pnpStateName(Device.PnpState)},
        {field::ProviderPresent, Device.ProviderPresent},
        {field::DevicePower, devicePowerName(Device.DevicePower)},
        {field::SystemPower, systemPowerName(Device.SystemPower)}};
    if (Device.AddDeviceStatus)
      Item[field::AddDeviceStatus] = *Device.AddDeviceStatus;
    PnpDevices.push_back(std::move(Item));
  }
  Root[field::PnpDevices] = std::move(PnpDevices);
  llvm::json::Array Interrupts;
  for (const auto &Interrupt : Result.Interrupts) {
    llvm::json::Object Item{
        {interruptField::SourceRequestIndex, Interrupt.SourceRequestIndex},
        {interruptField::EventIndex, Interrupt.EventIndex},
        {interruptField::DeviceID, Interrupt.DeviceID},
        {interruptField::InterruptID, Interrupt.InterruptID},
        {interruptField::Epoch, Interrupt.Epoch},
        {interruptField::DueAt100ns, Interrupt.DueAt100ns},
        {interruptField::OccurredAt100ns, nullptr},
        {interruptField::DeliveredAt100ns, nullptr},
        {interruptField::ReturnedAt100ns, nullptr},
        {interruptField::InterruptObject, nullptr},
        {interruptField::ReturnValue, nullptr},
        {interruptField::Claimed, nullptr},
        {interruptField::UndeliveredReason, nullptr}};
    if (Interrupt.OccurredAt100ns)
      Item[interruptField::OccurredAt100ns] = *Interrupt.OccurredAt100ns;
    if (Interrupt.DeliveredAt100ns)
      Item[interruptField::DeliveredAt100ns] = *Interrupt.DeliveredAt100ns;
    if (Interrupt.ReturnedAt100ns)
      Item[interruptField::ReturnedAt100ns] = *Interrupt.ReturnedAt100ns;
    if (Interrupt.InterruptObject)
      Item[interruptField::InterruptObject] =
          Address(*Interrupt.InterruptObject);
    if (Interrupt.ReturnValue) {
      Item[interruptField::ReturnValue] = *Interrupt.ReturnValue;
      Item[interruptField::Claimed] = *Interrupt.ReturnValue != 0;
    }
    if (Interrupt.UndeliveredReason)
      Item[interruptField::UndeliveredReason] = *Interrupt.UndeliveredReason;
    Interrupts.push_back(std::move(Item));
  }
  Root[interruptField::Interrupts] = std::move(Interrupts);
  llvm::json::Array Calls;
  for (const auto &Call : Result.Calls) {
    llvm::json::Array Arguments;
    for (uint64_t Argument : Call.Arguments)
      Arguments.push_back(Address(Argument));
    llvm::json::Object Event{
        {field::PC, Address(Call.PC)}, {field::Phase, Call.Phase},
        {field::Name, Call.Name},      {field::Arguments, std::move(Arguments)},
        {field::Result, nullptr},      {field::Detail, Call.Detail}};
    if (Call.Result)
      Event[field::Result] = Address(*Call.Result);
    Calls.push_back(std::move(Event));
  }
  Root[field::Calls] = std::move(Calls);
  llvm::json::Array Writes;
  for (const auto &Write : Result.Writes) {
    llvm::json::Object Event{{field::PC, Address(Write.PC)},
                             {field::Phase, Write.Phase},
                             {field::Address, Address(Write.Address)},
                             {field::Size, Write.Size},
                             {field::Value, nullptr},
                             {field::Semantics, profile::WriteSemantics}};
    if (Write.Value)
      Event[field::Value] = Address(*Write.Value);
    Writes.push_back(std::move(Event));
  }
  Root[field::Writes] = std::move(Writes);
  llvm::json::Array Messages;
  for (const auto &Message : Result.Messages)
    Messages.push_back(Message);
  Root[field::Messages] = std::move(Messages);
  llvm::json::Array Requests;
  for (const auto &Request : Result.Requests) {
    llvm::json::Object Item{
        {field::Kind, requestKindName(Request.Kind)},
        {field::Device, Request.Device},
        {field::DeviceID, nullptr},
        {field::Pnp, nullptr},
        {field::Power, nullptr},
        {field::Origin, requestOriginName(Request.Origin)},
        {field::ResponseIndex, nullptr},
        {field::File, Request.File},
        {field::ByteOffset, Address(Request.ByteOffset)},
        {field::ControlCode, Request.ControlCode},
        {field::IRP, Address(Request.IRP)},
        {field::Completed, Request.Completed},
        {field::DispatchStatus, nullptr},
        {field::IOStatus, nullptr},
        {field::CancelRequestedAt100ns, nullptr},
        {field::Information, Request.Information},
        {field::InformationHex, Address(Request.Information)},
        {field::Output,
         llvm::toHex(llvm::ArrayRef<uint8_t>(Request.Output), true)}};
    if (!Request.DeviceID.empty())
      Item[field::DeviceID] = Request.DeviceID;
    if (Request.ResponseIndex)
      Item[field::ResponseIndex] = *Request.ResponseIndex;
    if (Request.Kind == DriverRequestKind::Pnp ||
        Request.Kind == DriverRequestKind::Power)
      Item[field::File] = nullptr;
    if (Request.Pnp) {
      const auto &Pnp = *Request.Pnp;
      llvm::json::Object Observation{
          {field::Minor, pnpRequestName(Pnp.Minor)},
          {field::StateBefore, pnpStateName(Pnp.StateBefore)},
          {field::StateAfter, pnpStateName(Pnp.StateAfter)},
          {field::BusStatus, nullptr},
          {field::BusReceivedAt100ns, nullptr},
          {field::BusCompletedAt100ns, nullptr}};
      if (Pnp.BusStatus)
        Observation[field::BusStatus] = *Pnp.BusStatus;
      if (Pnp.BusReceivedAt100ns)
        Observation[field::BusReceivedAt100ns] = *Pnp.BusReceivedAt100ns;
      if (Pnp.BusCompletedAt100ns)
        Observation[field::BusCompletedAt100ns] = *Pnp.BusCompletedAt100ns;
      Item[field::Pnp] = std::move(Observation);
    }
    if (Request.Power) {
      const auto &Power = *Request.Power;
      llvm::json::Object Observation{
          {field::Minor, powerRequestName(Power.Minor)},
          {field::PowerType, powerTypeName(Power.Type)},
          {field::PowerState, powerStateName(Power.Type, Power.State)},
          {field::PowerAction, powerActionName(Power.Action)},
          {field::SystemContext, Power.SystemContext},
          {field::DeviceStateBefore, devicePowerName(Power.DeviceStateBefore)},
          {field::DeviceStateAfter, devicePowerName(Power.DeviceStateAfter)},
          {field::SystemStateBefore, systemPowerName(Power.SystemStateBefore)},
          {field::SystemStateAfter, systemPowerName(Power.SystemStateAfter)},
          {field::RequestedDeviceObject, nullptr},
          {field::BusStatus, nullptr},
          {field::BusReceivedAt100ns, nullptr},
          {field::BusCompletedAt100ns, nullptr}};
      if (Power.RequestedDeviceObject)
        Observation[field::RequestedDeviceObject] =
            Address(*Power.RequestedDeviceObject);
      if (Power.BusStatus)
        Observation[field::BusStatus] = *Power.BusStatus;
      if (Power.BusReceivedAt100ns)
        Observation[field::BusReceivedAt100ns] = *Power.BusReceivedAt100ns;
      if (Power.BusCompletedAt100ns)
        Observation[field::BusCompletedAt100ns] = *Power.BusCompletedAt100ns;
      Item[field::Power] = std::move(Observation);
    }
    if (Request.DispatchStatus)
      Item[field::DispatchStatus] = *Request.DispatchStatus;
    if (Request.IOStatus)
      Item[field::IOStatus] = *Request.IOStatus;
    if (Request.CancelRequestedAt100ns)
      Item[field::CancelRequestedAt100ns] = *Request.CancelRequestedAt100ns;
    Requests.push_back(std::move(Item));
  }
  Root[field::Requests] = std::move(Requests);
  std::string Text;
  llvm::raw_string_ostream OS(Text);
  OS << llvm::json::Value(std::move(Root));
  return Text;
}
} // namespace neverd::emulation
