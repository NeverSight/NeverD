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
  if (Result.Stop != DriverStopReason::Returned || !Result.NTStatus ||
      *Result.NTStatus != 0 ||
      Result.Requests.size() != Result.Configuration.Requests.size() ||
      (Result.Configuration.Unload && !Result.UnloadCompleted))
    return false;
  return llvm::all_of(Result.Requests, [](const DriverRequestResult &Request) {
    return Request.Completed && Request.DispatchStatus && Request.IOStatus &&
           !(*Request.DispatchStatus & profile::NTStatusFailureMask) &&
           !(*Request.IOStatus & profile::NTStatusFailureMask);
  });
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
      {field::NTSuccess, nullptr}};
  Root[field::Configuration] = llvm::json::Object{
      {field::ServiceName, Result.Configuration.ServiceName},
      {field::InstructionLimit, Result.Configuration.InstructionLimit},
      {field::MemoryLimit, Result.Configuration.MemoryLimit},
      {field::EventLimit, Result.Configuration.EventLimit},
      {field::TimeoutMilliseconds, Result.Configuration.TimeoutMilliseconds},
      {field::LoadAddress, Address(Result.Configuration.LoadAddress)},
      {field::Unload, Result.Configuration.Unload}};
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
  for (const auto &Device : Result.Devices)
    Devices.push_back(
        llvm::json::Object{{field::Address, Address(Device.Address)},
                           {field::Extension, Address(Device.Extension)},
                           {field::Type, Device.Type},
                           {field::Name, Device.Name}});
  Root[field::Devices] = std::move(Devices);
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
        {field::ControlCode, Request.ControlCode},
        {field::IRP, Address(Request.IRP)},
        {field::Completed, Request.Completed},
        {field::DispatchStatus, nullptr},
        {field::IOStatus, nullptr},
        {field::Information, Request.Information},
        {field::Output,
         llvm::toHex(llvm::ArrayRef<uint8_t>(Request.Output), true)}};
    if (Request.DispatchStatus)
      Item[field::DispatchStatus] = *Request.DispatchStatus;
    if (Request.IOStatus)
      Item[field::IOStatus] = *Request.IOStatus;
    Requests.push_back(std::move(Item));
  }
  Root[field::Requests] = std::move(Requests);
  std::string Text;
  llvm::raw_string_ostream OS(Text);
  OS << llvm::json::Value(std::move(Root));
  return Text;
}
} // namespace neverd::emulation
