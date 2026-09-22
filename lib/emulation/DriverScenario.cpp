//===- DriverScenario.cpp - Strict bounded driver scenarios ---------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Parses bounded, explicitly requested driver lifecycle scenarios.
///
//===----------------------------------------------------------------------===//

#include "DriverScenario.h"

#include "windows/WindowsKernelLayout.h"

#include "neverd/emulation/DriverReportFields.h"
#include "neverd/emulation/DriverSession.h"

#include "llvm/ADT/StringExtras.h"
#include "llvm/Support/JSON.h"

#include <algorithm>
#include <set>

namespace neverd::emulation {
namespace {

#define NEVERD_DRIVER_SCENARIO_ROOT_FIELD(Name, Spelling)                      \
  constexpr llvm::StringLiteral Name##Field = Spelling;
#define NEVERD_DRIVER_SCENARIO_REQUEST_FIELD(Name, Spelling)                   \
  constexpr llvm::StringLiteral Name##Field = Spelling;
#include "DriverScenarioFields.def"
#undef NEVERD_DRIVER_SCENARIO_ROOT_FIELD
#undef NEVERD_DRIVER_SCENARIO_REQUEST_FIELD

constexpr llvm::StringRef RootFields[] = {
#define NEVERD_DRIVER_SCENARIO_ROOT_FIELD(Name, Spelling) Spelling,
#define NEVERD_DRIVER_SCENARIO_REQUEST_FIELD(Name, Spelling)
#include "DriverScenarioFields.def"
#undef NEVERD_DRIVER_SCENARIO_ROOT_FIELD
#undef NEVERD_DRIVER_SCENARIO_REQUEST_FIELD
};
constexpr llvm::StringRef RequestFields[] = {
#define NEVERD_DRIVER_SCENARIO_ROOT_FIELD(Name, Spelling)
#define NEVERD_DRIVER_SCENARIO_REQUEST_FIELD(Name, Spelling) Spelling,
#include "DriverScenarioFields.def"
#undef NEVERD_DRIVER_SCENARIO_ROOT_FIELD
#undef NEVERD_DRIVER_SCENARIO_REQUEST_FIELD
};

llvm::Error invalid(const llvm::Twine &Message) {
  return llvm::createStringError(llvm::inconvertibleErrorCode(),
                                 "driver scenario: " + Message);
}

bool validDeviceName(llvm::StringRef Name) {
  return Name.size() <= profile::MaxDeviceNameSize &&
         std::all_of(Name.begin(), Name.end(),
                     [](unsigned char C) { return C >= 0x20 && C <= 0x7e; });
}

bool validDeviceID(llvm::StringRef ID) {
  return !ID.empty() && ID.size() <= DriverScenarioDeviceIDLimit &&
         llvm::isAlnum(ID.front()) &&
         std::all_of(ID.begin(), ID.end(), [](unsigned char C) {
           return llvm::isAlnum(C) || C == '_' || C == '-' || C == '.';
         });
}

bool supportedPnpRequest(DevicePnpRequest Minor) {
  switch (Minor) {
#define NEVERD_DRIVER_PNP_REQUEST(Name, Spelling)                              \
  case DevicePnpRequest::Name:                                                 \
    return true;
#include "neverd/emulation/DriverPnpNames.def"
#undef NEVERD_DRIVER_PNP_REQUEST
  default:
    return false;
  }
}

bool pnpRequestMayFail(DevicePnpRequest Minor) {
  switch (Minor) {
#define NEVERD_DEVICE_PNP_REQUEST(Name, Value, MayFail)                        \
  case DevicePnpRequest::Name:                                                 \
    return MayFail;
#include "neverd/emulation/DeviceLifecycle.def"
#undef NEVERD_DEVICE_PNP_REQUEST
  }
  return false;
}

// LLVM's object parser retains the final value of a duplicate key. Scenarios
// reject ambiguity instead. Run this scanner only after JSON syntax validation;
// decode each key using that same parser so escaped key aliases also collide.
llvm::Error uniqueFields(llvm::StringRef JSON) {
  std::vector<std::set<std::string>> Objects;
  for (size_t I = 0; I < JSON.size(); ++I) {
    if (JSON[I] == '{') {
      Objects.emplace_back();
    } else if (JSON[I] == '}') {
      Objects.pop_back();
    } else if (JSON[I] == '"') {
      const size_t Start = I++;
      while (I < JSON.size() && JSON[I] != '"') {
        if (JSON[I] == '\\')
          ++I;
        ++I;
      }
      size_t Next = I + 1;
      while (Next < JSON.size() && llvm::isSpace(JSON[Next]))
        ++Next;
      if (Next < JSON.size() && JSON[Next] == ':') {
        auto Key = llvm::json::parse(JSON.slice(Start, I + 1));
        if (!Key)
          return invalid(llvm::toString(Key.takeError()));
        const std::string Name = Key->getAsString()->str();
        if (!Objects.back().insert(Name).second)
          return invalid("duplicate field '" + Name + "'");
      }
    }
  }
  return llvm::Error::success();
}

llvm::Error fields(const llvm::json::Object &Object,
                   llvm::ArrayRef<llvm::StringRef> Allowed) {
  for (const auto &Field : Object)
    if (std::find(Allowed.begin(), Allowed.end(),
                  llvm::StringRef(Field.first)) == Allowed.end())
      return invalid("unknown field '" + Field.first.str() + "'");
  return llvm::Error::success();
}

llvm::Expected<uint64_t> hexNumber(llvm::StringRef Text,
                                   llvm::StringRef Field) {
  if (!(Text.consume_front("0x") || Text.consume_front("0X")) || Text.empty() ||
      !std::all_of(Text.begin(), Text.end(), llvm::isHexDigit))
    return invalid(Field + " must be a hexadecimal string beginning with 0x");
  uint64_t Value = 0;
  if (Text.getAsInteger(16, Value))
    return invalid(Field + " exceeds 64 bits");
  return Value;
}

llvm::Expected<uint32_t> unsigned32(const llvm::json::Value &Value,
                                    llvm::StringRef Field) {
  uint64_t Number;
  if (auto Text = Value.getAsString()) {
    auto Parsed = hexNumber(*Text, Field);
    if (!Parsed)
      return Parsed.takeError();
    Number = *Parsed;
  } else if (auto Integer = Value.getAsUINT64()) {
    Number = *Integer;
  } else {
    return invalid(Field +
                   " must be an unsigned integer or a 0x hexadecimal string");
  }
  if (Number > UINT32_MAX)
    return invalid(Field + " exceeds 32 bits");
  return static_cast<uint32_t>(Number);
}

llvm::Expected<DriverPnpOperation>
pnpOperation(const llvm::json::Object &Object) {
  if (auto E = fields(
          Object, {KindField, DeviceIDField, MinorField, BusCompletionField}))
    return std::move(E);
  auto Minor = Object.getString(MinorField);
  if (!Minor)
    return invalid("pnp minor must be a string");
  DriverPnpOperation Result;
  bool Found = false;
#define NEVERD_DRIVER_PNP_REQUEST(Name, Spelling)                              \
  if (*Minor == Spelling) {                                                    \
    Result.Minor = DevicePnpRequest::Name;                                     \
    Found = true;                                                              \
  }
#include "neverd/emulation/DriverPnpNames.def"
#undef NEVERD_DRIVER_PNP_REQUEST
  if (!Found)
    return invalid("unsupported pnp minor '" + *Minor + "'");
  const auto *Completion = Object.getObject(BusCompletionField);
  if (!Completion)
    return invalid("pnp bus_completion must be an explicit object");
  if (auto E = fields(*Completion, {field::Status, field::Delay100ns}))
    return std::move(E);
  const auto *Status = Completion->get(field::Status);
  if (!Status)
    return invalid("bus_completion requires an explicit status");
  auto ParsedStatus = unsigned32(*Status, field::Status);
  if (!ParsedStatus)
    return ParsedStatus.takeError();
  Result.BusCompletion.Status = *ParsedStatus;
  if (const auto *Delay = Completion->get(field::Delay100ns)) {
    auto Number = Delay->getAsUINT64();
    if (!Number)
      return invalid("delay_100ns must be a nonnegative integer");
    Result.BusCompletion.Delay100ns = *Number;
  }
  return Result;
}

llvm::Expected<std::vector<DriverPnpDevice>>
pnpDevices(const llvm::json::Value &Value) {
  const auto *Array = Value.getAsArray();
  if (!Array || Array->size() > DriverScenarioPnpDeviceLimit)
    return invalid("pnp_devices must be a bounded array of devices");
  std::vector<DriverPnpDevice> Result;
  for (const auto &Item : *Array) {
    const auto *Object = Item.getAsObject();
    if (!Object)
      return invalid("each pnp_devices entry must be an object");
    if (auto E =
            fields(*Object, {field::ID, field::Bus, field::InitialDevicePower,
                             field::InitialSystemPower}))
      return std::move(E);
    auto ID = Object->getString(field::ID);
    auto Bus = Object->getString(field::Bus);
    auto DevicePower = Object->getString(field::InitialDevicePower);
    auto SystemPower = Object->getString(field::InitialSystemPower);
    if (!ID || !Bus || !DevicePower || !SystemPower)
      return invalid(
          "pnp_devices entries require id, bus, initial_device_power "
          "and initial_system_power strings");
    DriverPnpDevice Device;
    Device.ID = ID->str();
    bool Found = false;
#define NEVERD_DRIVER_BUS_KIND(Name, Spelling)                                 \
  if (*Bus == Spelling) {                                                      \
    Device.Bus = DriverBusKind::Name;                                          \
    Found = true;                                                              \
  }
#include "neverd/emulation/DriverPnpNames.def"
#undef NEVERD_DRIVER_BUS_KIND
    if (!Found)
      return invalid("unsupported pnp bus '" + *Bus + "'");
#define NEVERD_DRIVER_DEVICE_POWER(Name, Spelling)                             \
  if (*DevicePower == Spelling)                                                \
    Device.InitialDevicePower = DevicePowerState::Name;
#define NEVERD_DRIVER_SYSTEM_POWER(Name, Spelling)                             \
  if (*SystemPower == Spelling)                                                \
    Device.InitialSystemPower = SystemPowerState::Name;
#include "neverd/emulation/DriverPnpNames.def"
#undef NEVERD_DRIVER_DEVICE_POWER
#undef NEVERD_DRIVER_SYSTEM_POWER
    if (!Device.InitialDevicePower || !Device.InitialSystemPower)
      return invalid("unknown initial PnP power state");
    Result.push_back(std::move(Device));
  }
  return Result;
}

llvm::Expected<DriverRequest> request(const llvm::json::Value &Value) {
  const auto *Object = Value.getAsObject();
  if (!Object)
    return invalid("each request must be an object");
  if (auto E = fields(*Object, RequestFields))
    return std::move(E);
  auto Kind = Object->getString(KindField);
  if (!Kind)
    return invalid("request kind must be a string");
  DriverRequest Result;
  bool KnownKind = false;
#define NEVERD_DRIVER_REQUEST_KIND(Name, Spelling, Major)                      \
  if (*Kind == Spelling) {                                                     \
    Result.Kind = DriverRequestKind::Name;                                     \
    KnownKind = true;                                                          \
  }
#include "neverd/emulation/DriverRequestKinds.def"
#undef NEVERD_DRIVER_REQUEST_KIND
  if (!KnownKind)
    return invalid("unsupported request kind '" + *Kind + "'");
  if (const auto *Device = Object->get(DeviceField)) {
    auto Name = Device->getAsString();
    if (!Name || Name->empty() || !validDeviceName(*Name))
      return invalid("device must contain 1..512 printable ASCII bytes");
    Result.Device = Name->str();
  }
  if (const auto *DeviceID = Object->get(DeviceIDField)) {
    auto ID = DeviceID->getAsString();
    if (!ID || !validDeviceID(*ID))
      return invalid("device_id must be a bounded ASCII identifier");
    Result.DeviceID = ID->str();
  }
  if (Result.Kind == DriverRequestKind::Pnp) {
    auto Pnp = pnpOperation(*Object);
    if (!Pnp)
      return Pnp.takeError();
    Result.Pnp = *Pnp;
    return Result;
  }
  if (Object->get(MinorField) || Object->get(BusCompletionField))
    return invalid("minor and bus_completion require request kind pnp");
  const bool IOCTL = Result.Kind == DriverRequestKind::DeviceControl;
  const bool Read = Result.Kind == DriverRequestKind::Read;
  const bool Write = Result.Kind == DriverRequestKind::Write;
  if ((!IOCTL && (Object->get(CodeField) || Object->get(DirectInputField))) ||
      (!IOCTL && !Write && Object->get(InputField)) ||
      (!IOCTL && !Read && Object->get(OutputSizeField)) ||
      (!Read && !Write && Object->get(ByteOffsetField)))
    return invalid("request fields do not match the request kind");
  if (const auto *File = Object->get(FileField)) {
    auto Number = File->getAsUINT64();
    if (!Number || *Number > UINT32_MAX)
      return invalid("file must be an unsigned 32-bit scenario identity");
    Result.File = static_cast<uint32_t>(*Number);
  }
  if (const auto *Cancel = Object->get(CancelAfter100nsField)) {
    auto Number = Cancel->getAsUINT64();
    if (!Number)
      return invalid("cancel_after_100ns must be a nonnegative integer");
    Result.CancelAfter100ns = *Number;
  }
  if (const auto *Offset = Object->get(ByteOffsetField)) {
    if (auto Text = Offset->getAsString()) {
      auto Number = hexNumber(*Text, ByteOffsetField);
      if (!Number)
        return Number.takeError();
      Result.ByteOffset = *Number;
    } else if (auto Number = Offset->getAsUINT64()) {
      Result.ByteOffset = *Number;
    } else {
      return invalid("byte_offset must be an unsigned integer or hex string");
    }
  }
  if (IOCTL) {
    const auto *Code = Object->get(CodeField);
    if (!Code)
      return invalid("ioctl requests require code");
    auto ParsedCode = unsigned32(*Code, CodeField);
    if (!ParsedCode)
      return ParsedCode.takeError();
    Result.ControlCode = *ParsedCode;
  }
  auto Bytes = [&](llvm::StringRef Field,
                   std::vector<uint8_t> &Output) -> llvm::Error {
    const auto *Input = Object->get(Field);
    if (!Input)
      return llvm::Error::success();
    auto Text = Input->getAsString();
    if (!Text || Text->size() > DriverScenarioBufferLimit * 2 ||
        Text->size() % 2 ||
        !std::all_of(Text->begin(), Text->end(), llvm::isHexDigit))
      return invalid(Field + " must be an even-length hexadecimal byte string "
                             "of at most 65536 bytes");
    Output.reserve(Text->size() / 2);
    for (size_t I = 0; I < Text->size(); I += 2)
      Output.push_back((llvm::hexDigitValue((*Text)[I]) << 4) |
                       llvm::hexDigitValue((*Text)[I + 1]));
    return llvm::Error::success();
  };
  if (auto E = Bytes(InputField, Result.Input))
    return std::move(E);
  if (auto E = Bytes(DirectInputField, Result.DirectInput))
    return std::move(E);
  if (const auto *Output = Object->get(OutputSizeField)) {
    auto Size = Output->getAsUINT64();
    if (!Size || *Size > DriverScenarioBufferLimit)
      return invalid(
          "output_size must be an unsigned integer of at most 65536");
    Result.OutputSize = static_cast<uint32_t>(*Size);
  }
  return Result;
}

llvm::Expected<std::vector<DriverRegistryKey>>
registry(const llvm::json::Value &Value) {
  const auto *Keys = Value.getAsArray();
  if (!Keys || Keys->size() > MaxRegistryKeys)
    return invalid("registry must be a bounded array of keys");
  std::vector<DriverRegistryKey> Result;
  for (const auto &KeyValue : *Keys) {
    const auto *Key = KeyValue.getAsObject();
    if (!Key)
      return invalid("each registry key must be an object");
    if (auto E = fields(*Key, {field::Path, field::Values}))
      return std::move(E);
    auto Path = Key->getString(field::Path);
    if (!Path)
      return invalid("registry key path must be a string");
    DriverRegistryKey Entry;
    Entry.Path = Path->str();
    if (const auto *ValuesValue = Key->get(field::Values)) {
      const auto *Values = ValuesValue->getAsArray();
      if (!Values || Values->size() > MaxRegistryValues)
        return invalid("registry values must be a bounded array");
      for (const auto &Item : *Values) {
        const auto *Object = Item.getAsObject();
        if (!Object)
          return invalid("each registry value must be an object");
        if (auto E = fields(*Object, {field::Name, field::Type, field::Data}))
          return std::move(E);
        auto Name = Object->getString(field::Name);
        const auto *TypeValue = Object->get(field::Type);
        auto Type = TypeValue ? TypeValue->getAsUINT64() : std::nullopt;
        auto Data = Object->getString(field::Data);
        if (!Name || !Type || *Type > UINT32_MAX || !Data ||
            Data->size() > MaxRegistryValueBytes * 2 || Data->size() % 2 ||
            !std::all_of(Data->begin(), Data->end(), llvm::isHexDigit))
          return invalid("registry values require name, unsigned type and "
                         "bounded hexadecimal data");
        DriverRegistryValue Value;
        Value.Name = Name->str();
        Value.Type = static_cast<uint32_t>(*Type);
        Value.Data.reserve(Data->size() / 2);
        for (size_t I = 0; I < Data->size(); I += 2)
          Value.Data.push_back((llvm::hexDigitValue((*Data)[I]) << 4) |
                               llvm::hexDigitValue((*Data)[I + 1]));
        Entry.Values.push_back(std::move(Value));
      }
    }
    Result.push_back(std::move(Entry));
  }
  return Result;
}

} // namespace

const char *devicePnpFinalStatusError(DevicePnpRequest Request,
                                      uint32_t Status) {
  if (!supportedPnpRequest(Request))
    return "unsupported pnp minor";
  if (Status == windows::StatusPending)
    return "STATUS_PENDING is not a final PnP completion";
  if (!pnpRequestMayFail(Request) && (Status & profile::NTStatusFailureMask))
    return "this PnP request must not fail";
  if (devicePnpRequiresSuccess(Request) && Status != windows::StatusSuccess)
    return "this PnP request requires STATUS_SUCCESS";
  // This informational result asks the PnP manager to requery resources; it
  // cannot be treated as an ordinary success in the resource-free profile.
  // https://learn.microsoft.com/windows-hardware/drivers/kernel/irp-mn-query-stop-device
  if (Request == DevicePnpRequest::QueryStop &&
      Status == windows::StatusResourceRequirementsChanged)
    return "STATUS_RESOURCE_REQUIREMENTS_CHANGED requires unsupported resource "
           "requery";
  return nullptr;
}

llvm::Error validateDriverPnpOperation(const DriverPnpOperation &Operation) {
  if (!supportedPnpRequest(Operation.Minor))
    return invalid("unsupported pnp minor");
  const auto &Bus = Operation.BusCompletion;
  if (!Bus.Status)
    return invalid("bus_completion requires an explicit status");
  if (const char *Error =
          devicePnpFinalStatusError(Operation.Minor, *Bus.Status))
    return invalid(Error);
  if (Bus.Delay100ns > INT64_MAX)
    return invalid("delay_100ns exceeds the signed 64-bit time limit");
  return llvm::Error::success();
}

llvm::Error validateDriverScenario(const DriverOptions &Options) {
  if (auto E = validateDriverRegistry(Options.Registry))
    return invalid(llvm::toString(std::move(E)));
  if (Options.PnpDevices.size() > DriverScenarioPnpDeviceLimit)
    return invalid("pnp_devices exceeds the device count limit");
  std::set<std::string> DeviceIDs;
  for (const auto &Device : Options.PnpDevices) {
    if (!validDeviceID(Device.ID))
      return invalid("pnp device id must be a bounded ASCII identifier");
    if (!DeviceIDs.insert(Device.ID).second)
      return invalid("duplicate pnp device id '" + Device.ID + "'");
    if (Device.Bus != DriverBusKind::ResourceFree)
      return invalid("only the resource_free PnP bus is supported");
    if (!Device.InitialDevicePower || !Device.InitialSystemPower)
      return invalid("pnp devices require explicit initial power states");
    if (*Device.InitialDevicePower != DevicePowerState::D0 ||
        *Device.InitialSystemPower != SystemPowerState::Working)
      return invalid("resource_free devices require D0 and working initial "
                     "power states");
  }
  if (Options.Requests.size() > DriverScenarioRequestLimit)
    return invalid("at most 64 requests are permitted");
  uint64_t Total = 0;
  for (const auto &Request : Options.Requests) {
    if (!Request.Device.empty() && !Request.DeviceID.empty())
      return invalid("device and device_id are mutually exclusive");
    if (!Request.DeviceID.empty() && (!validDeviceID(Request.DeviceID) ||
                                      !DeviceIDs.contains(Request.DeviceID)))
      return invalid("device_id must name a configured pnp device");
    if (Request.Kind == DriverRequestKind::Pnp) {
      if (Request.DeviceID.empty() || !Request.Pnp)
        return invalid("pnp requests require device_id and a PnP operation");
      if (!Request.Device.empty() || Request.File || Request.ControlCode ||
          !Request.Input.empty() || Request.OutputSize ||
          !Request.DirectInput.empty() || Request.ByteOffset ||
          Request.CancelAfter100ns)
        return invalid("pnp requests cannot contain file or transfer fields");
      if (auto E = validateDriverPnpOperation(*Request.Pnp))
        return E;
      continue;
    }
    if (Request.Pnp)
      return invalid("PnP operation requires request kind pnp");
    if (Request.CancelAfter100ns) {
      if (*Request.CancelAfter100ns > INT64_MAX)
        return invalid(
            "cancel_after_100ns exceeds the signed 64-bit time limit");
      if (Request.Kind != DriverRequestKind::Read &&
          Request.Kind != DriverRequestKind::Write &&
          Request.Kind != DriverRequestKind::DeviceControl)
        return invalid("cancel_after_100ns is valid only for read, write and "
                       "ioctl requests");
    }
    if (Request.Input.size() > DriverScenarioBufferLimit ||
        Request.OutputSize > DriverScenarioBufferLimit ||
        Request.DirectInput.size() > Request.OutputSize ||
        Request.ByteOffset > INT64_MAX)
      return invalid("request exceeds the buffer or signed offset limit");
    if (!validDeviceName(Request.Device))
      return invalid("device must contain at most 512 printable ASCII bytes");
    Total +=
        Request.Input.size() + Request.OutputSize + Request.DirectInput.size();
    if (Total > DriverScenarioTotalBufferLimit)
      return invalid("combined request buffers exceed 512 KiB");
    switch (Request.Kind) {
    case DriverRequestKind::Create:
    case DriverRequestKind::Cleanup:
    case DriverRequestKind::Close:
      if (Request.ControlCode || !Request.Input.empty() || Request.OutputSize ||
          !Request.DirectInput.empty() || Request.ByteOffset)
        return invalid("lifecycle requests cannot contain transfer parameters");
      break;
    case DriverRequestKind::Read:
      if (Request.ControlCode || !Request.Input.empty() ||
          !Request.DirectInput.empty())
        return invalid("read requests accept output_size and byte_offset");
      if (Request.OutputSize > INT64_MAX - Request.ByteOffset)
        return invalid("READ byte range exceeds a nonnegative file offset");
      break;
    case DriverRequestKind::Write:
      if (Request.ControlCode || Request.OutputSize ||
          !Request.DirectInput.empty())
        return invalid("write requests accept input and byte_offset");
      if (Request.Input.size() > INT64_MAX - Request.ByteOffset)
        return invalid("WRITE byte range exceeds a nonnegative file offset");
      break;
    case DriverRequestKind::DeviceControl: {
      if (Request.ByteOffset)
        return invalid("ioctl requests cannot contain byte_offset");
      const uint32_t Method =
          Request.ControlCode & windows::IoControlMethodMask;
      if (!Request.DirectInput.empty() && Method != windows::MethodInDirect &&
          Method != windows::MethodOutDirect)
        return invalid("direct_input is valid only for direct IOCTLs");
      break;
    }
    default:
      return invalid("invalid driver request kind");
    }
  }
  return llvm::Error::success();
}

llvm::Expected<DriverOptions>
driverOptionsFromScenarioJSON(llvm::StringRef JSON, DriverOptions Base) {
  if (JSON.size() > DriverScenarioJSONLimit)
    return invalid("JSON exceeds 2 MiB");
  auto Parsed = llvm::json::parse(JSON);
  if (!Parsed)
    return invalid(llvm::toString(Parsed.takeError()));
  if (auto E = uniqueFields(JSON))
    return std::move(E);
  const auto *Object = Parsed->getAsObject();
  if (!Object)
    return invalid("root must be an object");
  if (auto E = fields(*Object, RootFields))
    return std::move(E);
  if (const auto *Devices = Object->get(PnpDevicesField)) {
    auto ParsedDevices = pnpDevices(*Devices);
    if (!ParsedDevices)
      return ParsedDevices.takeError();
    Base.PnpDevices = std::move(*ParsedDevices);
  }
  if (const auto *Registry = Object->get(RegistryField)) {
    auto ParsedRegistry = registry(*Registry);
    if (!ParsedRegistry)
      return ParsedRegistry.takeError();
    Base.Registry = std::move(*ParsedRegistry);
  }
  if (const auto *Exports = Object->get(KernelExportsField)) {
    const auto *Inventory = Exports->getAsObject();
    if (!Inventory || Inventory->size() > profile::MaxImports)
      return invalid("kernel_exports must be a bounded object of booleans");
    Base.KernelExports.clear();
    for (const auto &[Name, Value] : *Inventory) {
      auto Present = Value.getAsBoolean();
      const std::string ExportName = Name.str();
      if (!Present || ExportName.empty() ||
          ExportName.size() > profile::MaxKernelExportNameSize ||
          !std::all_of(ExportName.begin(), ExportName.end(),
                       [](unsigned char C) { return C >= 0x21 && C <= 0x7e; }))
        return invalid(
            "kernel_exports requires printable names and boolean values");
      Base.KernelExports.emplace(ExportName, *Present);
    }
  }
  if (const auto *Address = Object->get(LoadAddressField)) {
    auto Text = Address->getAsString();
    if (!Text)
      return invalid("load_address must be a hexadecimal string");
    auto Number = hexNumber(*Text, "load_address");
    if (!Number)
      return Number.takeError();
    Base.LoadAddress = *Number;
  }
  if (const auto *Unload = Object->get(UnloadField)) {
    auto Boolean = Unload->getAsBoolean();
    if (!Boolean)
      return invalid("unload must be boolean");
    Base.Unload = *Boolean;
  }
  if (const auto *Requests = Object->get(RequestsField)) {
    const auto *Array = Requests->getAsArray();
    if (!Array || Array->size() > DriverScenarioRequestLimit)
      return invalid("requests must be an array of at most 64 objects");
    Base.Requests.clear();
    for (const auto &Value : *Array) {
      auto Request = request(Value);
      if (!Request)
        return Request.takeError();
      Base.Requests.push_back(std::move(*Request));
    }
  }
  if (auto E = validateDriverScenario(Base))
    return std::move(E);
  return Base;
}

} // namespace neverd::emulation
