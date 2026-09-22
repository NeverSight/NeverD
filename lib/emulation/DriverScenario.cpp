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

llvm::Expected<uint32_t> controlCode(const llvm::json::Value &Value) {
  uint64_t Number;
  if (auto Text = Value.getAsString()) {
    auto Parsed = hexNumber(*Text, "code");
    if (!Parsed)
      return Parsed.takeError();
    Number = *Parsed;
  } else if (auto Integer = Value.getAsUINT64()) {
    Number = *Integer;
  } else {
    return invalid(
        "code must be an unsigned integer or a 0x hexadecimal string");
  }
  if (Number > UINT32_MAX)
    return invalid("code exceeds 32 bits");
  return static_cast<uint32_t>(Number);
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
    auto ParsedCode = controlCode(*Code);
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

} // namespace

llvm::Error validateDriverScenario(const DriverOptions &Options) {
  if (Options.Requests.size() > DriverScenarioRequestLimit)
    return invalid("at most 64 requests are permitted");
  uint64_t Total = 0;
  for (const auto &Request : Options.Requests) {
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
