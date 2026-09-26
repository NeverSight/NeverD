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

#include "llvm/ADT/SmallVector.h"
#include "llvm/ADT/StringExtras.h"
#include "llvm/Support/FormatVariadic.h"
#include "llvm/Support/JSON.h"

#include <algorithm>
#include <map>
#include <set>

namespace neverd::emulation {
namespace {
constexpr uint64_t KibibyteBytes = 1024;

namespace resourceField {
#define NEVERD_DRIVER_RESOURCE_FIELD(Name, Spelling)                           \
  constexpr llvm::StringLiteral Name = Spelling;
#include "neverd/emulation/DriverResources.def"
#undef NEVERD_DRIVER_RESOURCE_FIELD
} // namespace resourceField

namespace dmaField {
#define NEVERD_DRIVER_DMA_FIELD(Name, Spelling)                                \
  constexpr llvm::StringLiteral Name = Spelling;
#include "neverd/emulation/DriverDMA.def"
#undef NEVERD_DRIVER_DMA_FIELD
} // namespace dmaField

namespace interruptField {
#define NEVERD_DRIVER_INTERRUPT_FIELD(Name, Spelling)                          \
  constexpr llvm::StringLiteral Name = Spelling;
#include "neverd/emulation/DriverInterrupts.def"
#undef NEVERD_DRIVER_INTERRUPT_FIELD
} // namespace interruptField

namespace userField {
#define NEVERD_DRIVER_USER_MEMORY_FIELD(Name, Spelling)                        \
  constexpr llvm::StringLiteral Name = Spelling;
#include "neverd/emulation/DriverUserMemory.def"
#undef NEVERD_DRIVER_USER_MEMORY_FIELD
} // namespace userField

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
    interruptField::InterruptEvents,
    dmaField::DmaEvents,
    userField::UserBuffers,
    userField::UserPointers,
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
                     [](unsigned char C) { return C >= ' ' && C <= '~'; });
}

bool validIdentifier(llvm::StringRef ID, size_t Limit) {
  return !ID.empty() && ID.size() <= Limit && llvm::isAlnum(ID.front()) &&
         std::all_of(ID.begin(), ID.end(), [](unsigned char C) {
           return llvm::isAlnum(C) || C == '_' || C == '-' || C == '.';
         });
}

bool validDeviceID(llvm::StringRef ID) {
  return validIdentifier(ID, DriverScenarioDeviceIDLimit);
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

bool supportedDevicePower(uint32_t State) {
  switch (static_cast<DevicePowerState>(State)) {
#define NEVERD_DRIVER_POWER_DEVICE_STATE(Name) case DevicePowerState::Name:
#include "neverd/emulation/DriverPower.def"
#undef NEVERD_DRIVER_POWER_DEVICE_STATE
    return true;
  default:
    return false;
  }
}

bool supportedSystemPower(uint32_t State) {
  switch (static_cast<SystemPowerState>(State)) {
#define NEVERD_DRIVER_POWER_SYSTEM_STATE(Name) case SystemPowerState::Name:
#include "neverd/emulation/DriverPower.def"
#undef NEVERD_DRIVER_POWER_SYSTEM_STATE
    return true;
  default:
    return false;
  }
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

llvm::Expected<DriverUserPageAccess>
userPageAccess(const llvm::json::Value &Value, llvm::StringRef Field) {
  auto Text = Value.getAsString();
  if (!Text)
    return invalid(Field + " must be a user-page access string");
#define NEVERD_DRIVER_USER_PAGE_ACCESS(Name, Spelling)                         \
  if (*Text == Spelling)                                                       \
    return DriverUserPageAccess::Name;
#include "neverd/emulation/DriverUserPageAccess.def"
#undef NEVERD_DRIVER_USER_PAGE_ACCESS
  return invalid(Field + " has an unsupported user-page access value");
}

bool validUserPageAccess(DriverUserPageAccess Access) {
  switch (Access) {
#define NEVERD_DRIVER_USER_PAGE_ACCESS(Name, Spelling)                         \
  case DriverUserPageAccess::Name:
#include "neverd/emulation/DriverUserPageAccess.def"
#undef NEVERD_DRIVER_USER_PAGE_ACCESS
    return true;
  }
  return false;
}

llvm::Expected<std::vector<uint8_t>> hexBytes(const llvm::json::Value &Value,
                                              llvm::StringRef Field) {
  auto Text = Value.getAsString();
  if (!Text || Text->size() > DriverScenarioBufferLimit * 2 ||
      Text->size() % 2 ||
      !std::all_of(Text->begin(), Text->end(), llvm::isHexDigit))
    return invalid(Field +
                   llvm::formatv(" must be an even-length hexadecimal byte "
                                 "string of at most {0} bytes",
                                 DriverScenarioBufferLimit)
                       .str());
  std::vector<uint8_t> Result;
  Result.reserve(Text->size() / 2);
  for (size_t I = 0; I < Text->size(); I += 2)
    Result.push_back((llvm::hexDigitValue((*Text)[I]) << 4) |
                     llvm::hexDigitValue((*Text)[I + 1]));
  return Result;
}

llvm::Expected<std::vector<DriverUserBuffer>>
userBuffers(const llvm::json::Value &Value) {
  const auto *Array = Value.getAsArray();
  if (!Array || Array->size() > DriverScenarioUserBufferLimit)
    return invalid("user_buffers must be a bounded array");
  std::vector<DriverUserBuffer> Result;
  for (const auto &Entry : *Array) {
    const auto *Object = Entry.getAsObject();
    if (!Object)
      return invalid("each user_buffers entry must be an object");
    if (auto E = fields(
            *Object, {field::ID, field::Size, userField::Input, field::Access}))
      return E;
    auto ID = Object->getString(field::ID);
    const auto *SizeValue = Object->get(field::Size);
    auto Size = SizeValue ? SizeValue->getAsUINT64() : std::nullopt;
    if (!ID || !validIdentifier(*ID, DriverScenarioUserBufferIDLimit))
      return invalid("user buffer id must be a bounded ASCII identifier");
    if (!Size || !*Size || *Size > DriverScenarioBufferLimit)
      return invalid("user buffer size must be a nonzero bounded integer");
    DriverUserBuffer Buffer;
    Buffer.ID = ID->str();
    Buffer.Size = static_cast<uint32_t>(*Size);
    if (const auto *Input = Object->get(userField::Input)) {
      auto Bytes = hexBytes(*Input, userField::Input);
      if (!Bytes)
        return Bytes.takeError();
      Buffer.Input = std::move(*Bytes);
    }
    if (const auto *Access = Object->get(field::Access)) {
      auto Parsed = userPageAccess(*Access, field::Access);
      if (!Parsed)
        return Parsed.takeError();
      Buffer.Access = *Parsed;
    }
    Result.push_back(std::move(Buffer));
  }
  return Result;
}

llvm::Expected<DriverUserBufferRef>
userBufferRef(const llvm::json::Value &Value) {
  const auto *Object = Value.getAsObject();
  if (!Object)
    return invalid("user pointer reference must be an object");
  if (auto E =
          fields(*Object, {userField::Buffer, field::ID, userField::Offset}))
    return E;
  auto Kind = Object->getString(userField::Buffer);
  const auto *OffsetValue = Object->get(userField::Offset);
  auto Offset = OffsetValue ? OffsetValue->getAsUINT64() : std::nullopt;
  if (!Kind || !Offset || *Offset > UINT32_MAX)
    return invalid("user pointer requires a buffer kind and unsigned offset");
  std::optional<DriverUserBufferKind> ParsedKind;
#define NEVERD_DRIVER_USER_BUFFER_KIND(Name, Spelling)                         \
  if (*Kind == Spelling)                                                       \
    ParsedKind = DriverUserBufferKind::Name;
#include "neverd/emulation/DriverUserMemory.def"
#undef NEVERD_DRIVER_USER_BUFFER_KIND
  if (!ParsedKind)
    return invalid("unsupported user pointer buffer kind");
  DriverUserBufferRef Result;
  Result.Kind = *ParsedKind;
  Result.Offset = static_cast<uint32_t>(*Offset);
  if (Result.Kind == DriverUserBufferKind::Memory) {
    auto ID = Object->getString(field::ID);
    if (!ID || !validIdentifier(*ID, DriverScenarioUserBufferIDLimit))
      return invalid("memory pointer reference requires a user buffer id");
    Result.ID = ID->str();
  } else if (Object->get(field::ID)) {
    return invalid("input/output pointer references do not accept id");
  }
  return Result;
}

llvm::Expected<std::vector<DriverUserPointer>>
userPointers(const llvm::json::Value &Value) {
  const auto *Array = Value.getAsArray();
  if (!Array || Array->size() > DriverScenarioUserPointerLimit)
    return invalid("user_pointers must be a bounded array");
  std::vector<DriverUserPointer> Result;
  for (const auto &Entry : *Array) {
    const auto *Object = Entry.getAsObject();
    if (!Object)
      return invalid("each user_pointers entry must be an object");
    if (auto E = fields(*Object, {userField::Source, userField::Target}))
      return E;
    const auto *Source = Object->get(userField::Source);
    const auto *Target = Object->get(userField::Target);
    if (!Source || !Target)
      return invalid("user pointer requires source and target references");
    auto ParsedSource = userBufferRef(*Source);
    if (!ParsedSource)
      return ParsedSource.takeError();
    auto ParsedTarget = userBufferRef(*Target);
    if (!ParsedTarget)
      return ParsedTarget.takeError();
    Result.push_back({std::move(*ParsedSource), std::move(*ParsedTarget)});
  }
  return Result;
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

llvm::Expected<uint64_t> unsigned64(const llvm::json::Value &Value,
                                    llvm::StringRef Field) {
  if (auto Text = Value.getAsString()) {
    return hexNumber(*Text, Field);
  }
  if (auto Integer = Value.getAsUINT64())
    return *Integer;
  return invalid(Field +
                 " must be an unsigned integer or a 0x hexadecimal string");
}

llvm::Expected<uint32_t> unsigned32(const llvm::json::Value &Value,
                                    llvm::StringRef Field) {
  auto Number = unsigned64(Value, Field);
  if (!Number)
    return Number.takeError();
  if (*Number > UINT32_MAX)
    return invalid(Field + " exceeds 32 bits");
  return static_cast<uint32_t>(*Number);
}

llvm::Expected<DriverBusCompletion>
busCompletion(const llvm::json::Object &Object) {
  const auto *Completion = Object.getObject(BusCompletionField);
  if (!Completion)
    return invalid("bus_completion must be an explicit object");
  if (auto E = fields(*Completion, {field::Status, field::Delay100ns}))
    return std::move(E);
  const auto *Status = Completion->get(field::Status);
  if (!Status)
    return invalid("bus_completion requires an explicit status");
  auto ParsedStatus = unsigned32(*Status, field::Status);
  if (!ParsedStatus)
    return ParsedStatus.takeError();
  DriverBusCompletion Result;
  Result.Status = *ParsedStatus;
  if (const auto *Delay = Completion->get(field::Delay100ns)) {
    auto Number = Delay->getAsUINT64();
    if (!Number)
      return invalid("delay_100ns must be a nonnegative integer");
    Result.Delay100ns = *Number;
  }
  return Result;
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
  auto Bus = busCompletion(Object);
  if (!Bus)
    return Bus.takeError();
  Result.BusCompletion = *Bus;
  return Result;
}

llvm::Expected<DriverPowerOperation>
powerOperation(const llvm::json::Object &Object, bool ResponseTemplate) {
  llvm::SmallVector<llvm::StringRef, 8> Allowed{
      MinorField,       PowerTypeField,     PowerStateField,
      PowerActionField, SystemContextField, BusCompletionField};
  if (!ResponseTemplate) {
    Allowed.push_back(KindField);
    Allowed.push_back(DeviceIDField);
  }
  if (auto E = fields(Object, Allowed))
    return std::move(E);
  auto Minor = Object.getString(MinorField);
  auto Type = Object.getString(PowerTypeField);
  auto State = Object.getString(PowerStateField);
  auto Action = Object.getString(PowerActionField);
  const auto *Context = Object.get(SystemContextField);
  if (!Minor || !Type || !State || !Action || !Context)
    return invalid("power requires explicit minor, power_type, power_state, "
                   "power_action and system_context");
  DriverPowerOperation Result;
  bool FoundMinor = false, FoundType = false, FoundAction = false;
#define NEVERD_DRIVER_POWER_REQUEST(Name, Spelling)                            \
  if (*Minor == Spelling) {                                                    \
    Result.Minor = DevicePowerRequest::Name;                                   \
    FoundMinor = true;                                                         \
  }
#define NEVERD_DRIVER_POWER_TYPE(Name, Value, Spelling)                        \
  if (*Type == Spelling) {                                                     \
    Result.Type = DriverPowerType::Name;                                       \
    FoundType = true;                                                          \
  }
#define NEVERD_DRIVER_POWER_ACTION(Name, Value, Spelling)                      \
  if (*Action == Spelling) {                                                   \
    Result.Action = DriverPowerAction::Name;                                   \
    FoundAction = true;                                                        \
  }
#include "neverd/emulation/DriverPower.def"
#undef NEVERD_DRIVER_POWER_REQUEST
#undef NEVERD_DRIVER_POWER_TYPE
#undef NEVERD_DRIVER_POWER_ACTION
  if (!FoundMinor || !FoundType || !FoundAction)
    return invalid("unsupported power minor, type or action");
  if (Result.Type == DriverPowerType::Device) {
#define NEVERD_DRIVER_DEVICE_POWER(Name, Spelling)                             \
  if (*State == Spelling)                                                      \
    Result.State = static_cast<uint32_t>(DevicePowerState::Name);
#include "neverd/emulation/DriverPnpNames.def"
#undef NEVERD_DRIVER_DEVICE_POWER
  } else {
#define NEVERD_DRIVER_SYSTEM_POWER(Name, Spelling)                             \
  if (*State == Spelling)                                                      \
    Result.State = static_cast<uint32_t>(SystemPowerState::Name);
#include "neverd/emulation/DriverPnpNames.def"
#undef NEVERD_DRIVER_SYSTEM_POWER
  }
  auto RawContext = unsigned32(*Context, SystemContextField);
  if (!RawContext)
    return RawContext.takeError();
  Result.SystemContext = *RawContext;
  auto Bus = busCompletion(Object);
  if (!Bus)
    return Bus.takeError();
  Result.BusCompletion = *Bus;
  return Result;
}

llvm::Expected<DriverRegister>
registerDescription(const llvm::json::Value &Value) {
  const auto *Object = Value.getAsObject();
  if (!Object)
    return invalid("each register must be an object");
  if (auto E = fields(*Object, {resourceField::Offset, resourceField::Width,
                                resourceField::Access, resourceField::Value}))
    return std::move(E);
  const auto *Offset = Object->get(resourceField::Offset);
  const auto *Width = Object->get(resourceField::Width);
  const auto *Initial = Object->get(resourceField::Value);
  auto Access = Object->getString(resourceField::Access);
  if (!Offset || !Width || !Initial || !Access)
    return invalid(
        "registers require explicit offset, width, access and value");
  DriverRegister Result;
  auto ParsedOffset = unsigned32(*Offset, resourceField::Offset);
  if (!ParsedOffset)
    return ParsedOffset.takeError();
  Result.Offset = *ParsedOffset;
  auto ParsedWidth = unsigned32(*Width, resourceField::Width);
  if (!ParsedWidth)
    return ParsedWidth.takeError();
  if (*ParsedWidth > UINT8_MAX)
    return invalid("register width exceeds 8 bits");
  Result.Width = static_cast<uint8_t>(*ParsedWidth);
  auto ParsedInitial = unsigned32(*Initial, resourceField::Value);
  if (!ParsedInitial)
    return ParsedInitial.takeError();
  Result.Value = *ParsedInitial;
  bool Found = false;
#define NEVERD_DRIVER_REGISTER_ACCESS(Name, Spelling)                          \
  if (*Access == Spelling) {                                                   \
    Result.Access = DriverRegisterAccess::Name;                                \
    Found = true;                                                              \
  }
#include "neverd/emulation/DriverResources.def"
#undef NEVERD_DRIVER_REGISTER_ACCESS
  if (!Found)
    return invalid("unsupported register access '" + *Access + "'");
  return Result;
}

llvm::Expected<std::vector<DriverMemoryResource>>
memoryResources(const llvm::json::Value &Value) {
  const auto *Array = Value.getAsArray();
  if (!Array || Array->size() > DriverScenarioResourcesPerDeviceLimit)
    return invalid("resources must be a bounded array");
  std::vector<DriverMemoryResource> Result;
  for (const auto &Item : *Array) {
    const auto *Object = Item.getAsObject();
    if (!Object)
      return invalid("each resource must be an object");
    if (auto E =
            fields(*Object, {resourceField::ID, resourceField::RawStart,
                             resourceField::TranslatedStart,
                             resourceField::Length, resourceField::Registers}))
      return std::move(E);
    auto ID = Object->getString(resourceField::ID);
    const auto *Raw = Object->get(resourceField::RawStart);
    const auto *Translated = Object->get(resourceField::TranslatedStart);
    const auto *Length = Object->get(resourceField::Length);
    const auto *Registers = Object->getArray(resourceField::Registers);
    if (!ID || !Raw || !Translated || !Length || !Registers)
      return invalid("resources require explicit id, raw_start, "
                     "translated_start, length and registers");
    if (Registers->size() > DriverScenarioRegistersPerResourceLimit)
      return invalid("registers exceeds the per-resource count limit");
    DriverMemoryResource Resource;
    Resource.ID = ID->str();
    auto ParsedRaw = unsigned64(*Raw, resourceField::RawStart);
    if (!ParsedRaw)
      return ParsedRaw.takeError();
    Resource.RawStart = *ParsedRaw;
    auto ParsedTranslated =
        unsigned64(*Translated, resourceField::TranslatedStart);
    if (!ParsedTranslated)
      return ParsedTranslated.takeError();
    Resource.TranslatedStart = *ParsedTranslated;
    auto ParsedLength = unsigned32(*Length, resourceField::Length);
    if (!ParsedLength)
      return ParsedLength.takeError();
    Resource.Length = *ParsedLength;
    for (const auto &Entry : *Registers) {
      auto Register = registerDescription(Entry);
      if (!Register)
        return Register.takeError();
      Resource.Registers.push_back(*Register);
    }
    Result.push_back(std::move(Resource));
  }
  return Result;
}

llvm::Expected<std::vector<DriverInterruptResource>>
interruptResources(const llvm::json::Value &Value) {
  const auto *Array = Value.getAsArray();
  if (!Array || Array->size() > DriverScenarioInterruptsPerDeviceLimit)
    return invalid("interrupts must be a bounded array");
  std::vector<DriverInterruptResource> Result;
  for (const auto &Item : *Array) {
    const auto *Object = Item.getAsObject();
    if (!Object)
      return invalid("each interrupt must be an object");
    if (auto E = fields(
            *Object,
            {interruptField::ID, interruptField::RawVector,
             interruptField::RawLevel, interruptField::RawAffinity,
             interruptField::TranslatedVector, interruptField::TranslatedLevel,
             interruptField::TranslatedAffinity, interruptField::Mode,
             interruptField::Share, interruptField::RetriggerAfter100ns}))
      return std::move(E);
    auto ID = Object->getString(interruptField::ID);
    auto Mode = Object->getString(interruptField::Mode);
    auto Share = Object->getString(interruptField::Share);
    if (!ID || !Mode || !Share)
      return invalid("interrupts require explicit id, mode and share strings");
    DriverInterruptResource Resource;
    Resource.ID = ID->str();
    bool ModeFound = false, ShareFound = false;
#define NEVERD_DRIVER_INTERRUPT_MODE(Name, Value, Spelling)                    \
  if (*Mode == Spelling) {                                                     \
    Resource.Mode = DriverInterruptMode::Name;                                 \
    ModeFound = true;                                                          \
  }
#define NEVERD_DRIVER_INTERRUPT_SHARE(Name, Value, Spelling)                   \
  if (*Share == Spelling) {                                                    \
    Resource.Share = DriverInterruptShare::Name;                               \
    ShareFound = true;                                                         \
  }
#include "neverd/emulation/DriverInterrupts.def"
#undef NEVERD_DRIVER_INTERRUPT_MODE
#undef NEVERD_DRIVER_INTERRUPT_SHARE
    if (!ModeFound || !ShareFound)
      return invalid("unsupported interrupt mode or share");
    if (const auto *Period = Object->get(interruptField::RetriggerAfter100ns)) {
      auto Parsed = unsigned64(*Period, interruptField::RetriggerAfter100ns);
      if (!Parsed)
        return Parsed.takeError();
      Resource.RetriggerAfter100ns = *Parsed;
    }
    const std::pair<llvm::StringRef, uint32_t DriverInterruptResource::*>
        Words[] = {
            {interruptField::RawVector, &DriverInterruptResource::RawVector},
            {interruptField::RawLevel, &DriverInterruptResource::RawLevel},
            {interruptField::TranslatedVector,
             &DriverInterruptResource::TranslatedVector},
            {interruptField::TranslatedLevel,
             &DriverInterruptResource::TranslatedLevel}};
    for (const auto &[Name, Member] : Words) {
      const auto *Fact = Object->get(Name);
      if (!Fact)
        return invalid("interrupts require explicit " + Name);
      auto Parsed = unsigned32(*Fact, Name);
      if (!Parsed)
        return Parsed.takeError();
      Resource.*Member = *Parsed;
    }
    const std::pair<llvm::StringRef, uint64_t DriverInterruptResource::*>
        Masks[] = {{interruptField::RawAffinity,
                    &DriverInterruptResource::RawAffinity},
                   {interruptField::TranslatedAffinity,
                    &DriverInterruptResource::TranslatedAffinity}};
    for (const auto &[Name, Member] : Masks) {
      const auto *Fact = Object->get(Name);
      if (!Fact)
        return invalid("interrupts require explicit " + Name);
      auto Parsed = unsigned64(*Fact, Name);
      if (!Parsed)
        return Parsed.takeError();
      Resource.*Member = *Parsed;
    }
    Result.push_back(std::move(Resource));
  }
  return Result;
}

llvm::Expected<std::vector<DriverInterruptEvent>>
interruptEvents(const llvm::json::Value &Value) {
  const auto *Array = Value.getAsArray();
  if (!Array || Array->size() > DriverScenarioInterruptEventsPerRequestLimit)
    return invalid("interrupt_events must be a bounded array");
  std::vector<DriverInterruptEvent> Result;
  for (const auto &Item : *Array) {
    const auto *Object = Item.getAsObject();
    if (!Object)
      return invalid("each interrupt event must be an object");
    if (auto E = fields(*Object,
                        {interruptField::After100ns, interruptField::DeviceID,
                         interruptField::InterruptID, interruptField::Action}))
      return std::move(E);
    const auto *After = Object->get(interruptField::After100ns);
    auto DeviceID = Object->getString(interruptField::DeviceID);
    auto InterruptID = Object->getString(interruptField::InterruptID);
    if (!After || !DeviceID || !InterruptID)
      return invalid("interrupt events require explicit after_100ns, "
                     "device_id and interrupt_id");
    auto ParsedAfter = unsigned64(*After, interruptField::After100ns);
    if (!ParsedAfter)
      return ParsedAfter.takeError();
    DriverInterruptEvent Event{*ParsedAfter, DeviceID->str(),
                               InterruptID->str()};
    if (const auto *Field = Object->get(interruptField::Action)) {
      auto Action = Field->getAsString();
      bool Found = false;
#define NEVERD_DRIVER_INTERRUPT_ACTION(Name, Spelling)                         \
  if (Action && *Action == Spelling) {                                         \
    Event.Action = DriverInterruptAction::Name;                                \
    Found = true;                                                              \
  }
#include "neverd/emulation/DriverInterrupts.def"
#undef NEVERD_DRIVER_INTERRUPT_ACTION
      if (!Found)
        return invalid("interrupt action must be pulse, assert or deassert");
    }
    Result.push_back(std::move(Event));
  }
  return Result;
}

llvm::Expected<DriverDmaConfig>
dmaConfiguration(const llvm::json::Value &Value) {
  const auto *Object = Value.getAsObject();
  if (!Object)
    return invalid("dma must be an explicit object");
  if (auto E = fields(*Object, {dmaField::AddressBits, dmaField::MaximumLength,
                                dmaField::MapRegisters, dmaField::Alignment,
                                dmaField::LogicalBase, dmaField::LogicalLength,
                                dmaField::ScatterGather}))
    return std::move(E);
  DriverDmaConfig Result;
  const std::pair<llvm::StringRef, uint32_t DriverDmaConfig::*> Words[] = {
      {dmaField::AddressBits, &DriverDmaConfig::AddressBits},
      {dmaField::MaximumLength, &DriverDmaConfig::MaximumLength},
      {dmaField::MapRegisters, &DriverDmaConfig::MapRegisters},
      {dmaField::Alignment, &DriverDmaConfig::Alignment}};
  for (const auto &[Name, Member] : Words) {
    const auto *Fact = Object->get(Name);
    if (!Fact)
      return invalid("dma requires explicit " + Name);
    auto Parsed = unsigned32(*Fact, Name);
    if (!Parsed)
      return Parsed.takeError();
    Result.*Member = *Parsed;
  }
  const std::pair<llvm::StringRef, uint64_t DriverDmaConfig::*> Addresses[] = {
      {dmaField::LogicalBase, &DriverDmaConfig::LogicalBase},
      {dmaField::LogicalLength, &DriverDmaConfig::LogicalLength}};
  for (const auto &[Name, Member] : Addresses) {
    const auto *Fact = Object->get(Name);
    if (!Fact)
      return invalid("dma requires explicit " + Name);
    auto Parsed = unsigned64(*Fact, Name);
    if (!Parsed)
      return Parsed.takeError();
    Result.*Member = *Parsed;
  }
  auto ScatterGather = Object->getBoolean(dmaField::ScatterGather);
  if (!ScatterGather)
    return invalid("dma requires explicit scatter_gather boolean");
  Result.ScatterGather = *ScatterGather;
  return Result;
}

llvm::Expected<std::vector<DriverDmaEvent>>
dmaEvents(const llvm::json::Value &Value) {
  const auto *Array = Value.getAsArray();
  if (!Array || Array->size() > DriverScenarioDmaEventsPerRequestLimit)
    return invalid("dma_events must be a bounded array");
  std::vector<DriverDmaEvent> Result;
  for (const auto &Item : *Array) {
    const auto *Object = Item.getAsObject();
    if (!Object)
      return invalid("each DMA event must be an object");
    if (auto E = fields(*Object, {dmaField::After100ns, dmaField::DeviceID,
                                  dmaField::LogicalAddress, dmaField::Direction,
                                  dmaField::Length, dmaField::Data}))
      return std::move(E);
    const auto *After = Object->get(dmaField::After100ns);
    const auto *Address = Object->get(dmaField::LogicalAddress);
    const auto *Length = Object->get(dmaField::Length);
    auto DeviceID = Object->getString(dmaField::DeviceID);
    auto Direction = Object->getString(dmaField::Direction);
    if (!After || !Address || !Length || !DeviceID || !Direction)
      return invalid("DMA events require explicit after_100ns, device_id, "
                     "logical_address, direction and length");
    DriverDmaEvent Event;
    Event.DeviceID = DeviceID->str();
    bool Found = false;
#define NEVERD_DRIVER_DMA_DIRECTION(Name, Spelling)                            \
  if (*Direction == Spelling) {                                                \
    Event.Direction = DriverDmaDirection::Name;                                \
    Found = true;                                                              \
  }
#include "neverd/emulation/DriverDMA.def"
#undef NEVERD_DRIVER_DMA_DIRECTION
    if (!Found)
      return invalid("unsupported DMA direction");
    auto ParsedAfter = unsigned64(*After, dmaField::After100ns);
    if (!ParsedAfter)
      return ParsedAfter.takeError();
    Event.After100ns = *ParsedAfter;
    auto ParsedAddress = unsigned64(*Address, dmaField::LogicalAddress);
    if (!ParsedAddress)
      return ParsedAddress.takeError();
    Event.LogicalAddress = *ParsedAddress;
    auto ParsedLength = unsigned32(*Length, dmaField::Length);
    if (!ParsedLength)
      return ParsedLength.takeError();
    Event.Length = *ParsedLength;
    const auto *Data = Object->get(dmaField::Data);
    if (Event.Direction == DriverDmaDirection::ReadMemory && Data)
      return invalid("read_memory DMA events cannot specify data_hex");
    if (Event.Direction == DriverDmaDirection::WriteMemory) {
      auto Bytes = Data ? Data->getAsString() : std::nullopt;
      if (!Bytes || Bytes->size() > DriverDmaMaximumLengthLimit * 2 ||
          Bytes->size() % 2 || Bytes->size() / 2 != Event.Length ||
          !std::all_of(Bytes->begin(), Bytes->end(), llvm::isHexDigit))
        return invalid("write_memory DMA events require data_hex with exactly "
                       "length bytes");
      Event.Data.reserve(Event.Length);
      for (size_t I = 0; I < Bytes->size(); I += 2)
        Event.Data.push_back((llvm::hexDigitValue((*Bytes)[I]) << 4) |
                             llvm::hexDigitValue((*Bytes)[I + 1]));
    }
    Result.push_back(std::move(Event));
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
    if (auto E = fields(*Object,
                        {field::ID, field::Bus, field::InitialDevicePower,
                         field::InitialSystemPower,
                         field::InitialReportedDevicePower,
                         field::RequestedDevicePower, resourceField::Resources,
                         interruptField::Interrupts, dmaField::Dma}))
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
    if (const auto *Resources = Object->get(resourceField::Resources)) {
      if (Device.Bus == DriverBusKind::ResourceFree)
        return invalid("resource_free devices cannot specify resources");
      auto ParsedResources = memoryResources(*Resources);
      if (!ParsedResources)
        return ParsedResources.takeError();
      Device.Resources = std::move(*ParsedResources);
    }
    if (const auto *Interrupts = Object->get(interruptField::Interrupts)) {
      if (Device.Bus == DriverBusKind::ResourceFree)
        return invalid("resource_free devices cannot specify interrupts");
      auto ParsedInterrupts = interruptResources(*Interrupts);
      if (!ParsedInterrupts)
        return ParsedInterrupts.takeError();
      Device.Interrupts = std::move(*ParsedInterrupts);
    }
    if (const auto *Dma = Object->get(dmaField::Dma)) {
      if (Device.Bus == DriverBusKind::ResourceFree)
        return invalid("resource_free devices cannot specify dma");
      auto ParsedDma = dmaConfiguration(*Dma);
      if (!ParsedDma)
        return ParsedDma.takeError();
      Device.Dma = *ParsedDma;
    }
    if (Device.Bus == DriverBusKind::RegisterBank &&
        !Object->get(resourceField::Resources) &&
        !Object->get(interruptField::Interrupts))
      return invalid(
          "register_bank devices require explicit resources or interrupts");
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
    if (const auto *Reported = Object->get(field::InitialReportedDevicePower)) {
      auto ReportedName = Reported->getAsString();
      if (!ReportedName)
        return invalid("initial_reported_device_power must be a state string");
#define NEVERD_DRIVER_DEVICE_POWER(Name, Spelling)                             \
  if (*ReportedName == Spelling)                                               \
    Device.InitialReportedDevicePower = DevicePowerState::Name;
#include "neverd/emulation/DriverPnpNames.def"
#undef NEVERD_DRIVER_DEVICE_POWER
      if (!Device.InitialReportedDevicePower)
        return invalid("unknown initial_reported_device_power");
    }
    if (const auto *Responses = Object->get(field::RequestedDevicePower)) {
      const auto *Queue = Responses->getAsArray();
      if (!Queue || Queue->size() > DriverScenarioPowerResponseLimit)
        return invalid("requested_device_power must be a bounded array");
      for (const auto &Entry : *Queue) {
        const auto *Template = Entry.getAsObject();
        if (!Template)
          return invalid("each requested_device_power entry must be an object");
        auto Operation = powerOperation(*Template, true);
        if (!Operation)
          return Operation.takeError();
        Device.RequestedDevicePower.push_back(*Operation);
      }
    }
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
  if ((Object->get(userField::UserBuffers) ||
       Object->get(userField::UserPointers)) &&
      Result.Kind != DriverRequestKind::DeviceControl &&
      Result.Kind != DriverRequestKind::Read &&
      Result.Kind != DriverRequestKind::Write)
    return invalid("user memory requires a neither-I/O transfer request");
  if (const auto *Buffers = Object->get(userField::UserBuffers)) {
    auto Parsed = userBuffers(*Buffers);
    if (!Parsed)
      return Parsed.takeError();
    Result.UserBuffers = std::move(*Parsed);
  }
  if (const auto *Pointers = Object->get(userField::UserPointers)) {
    auto Parsed = userPointers(*Pointers);
    if (!Parsed)
      return Parsed.takeError();
    Result.UserPointers = std::move(*Parsed);
  }
  if (Object->get(AsynchronousFileField) &&
      Result.Kind != DriverRequestKind::Create)
    return invalid("asynchronous_file requires a create request");
  if ((Result.Kind == DriverRequestKind::Pnp ||
       Result.Kind == DriverRequestKind::Power) &&
      Object->get(RequestorProcessIDField))
    return invalid("requestor_process_id requires a file request");
  if (const auto *Events = Object->get(dmaField::DmaEvents)) {
    if (Result.Kind != DriverRequestKind::Read &&
        Result.Kind != DriverRequestKind::Write &&
        Result.Kind != DriverRequestKind::DeviceControl)
      return invalid(
          "dma_events is valid only for read, write and ioctl requests");
    auto ParsedEvents = dmaEvents(*Events);
    if (!ParsedEvents)
      return ParsedEvents.takeError();
    Result.DmaEvents = std::move(*ParsedEvents);
  }
  if (const auto *Events = Object->get(interruptField::InterruptEvents)) {
    if (Result.Kind != DriverRequestKind::Read &&
        Result.Kind != DriverRequestKind::Write &&
        Result.Kind != DriverRequestKind::DeviceControl)
      return invalid(
          "interrupt_events is valid only for read, write and ioctl requests");
    auto ParsedEvents = interruptEvents(*Events);
    if (!ParsedEvents)
      return ParsedEvents.takeError();
    Result.InterruptEvents = std::move(*ParsedEvents);
  }
  if (const auto *Device = Object->get(DeviceField)) {
    auto Name = Device->getAsString();
    if (!Name || Name->empty() || !validDeviceName(*Name))
      return invalid(
          llvm::formatv("device must contain 1..{0} printable ASCII bytes",
                        profile::MaxDeviceNameSize)
              .str());
    Result.Device = Name->str();
  }
  if (const auto *DeviceID = Object->get(DeviceIDField)) {
    auto ID = DeviceID->getAsString();
    if (!ID || !validDeviceID(*ID))
      return invalid("device_id must be a bounded ASCII identifier");
    Result.DeviceID = ID->str();
  }
  auto UserAccess =
      [&](llvm::StringRef Field,
          std::optional<DriverUserPageAccess> &Output) -> llvm::Error {
    const auto *Value = Object->get(Field);
    if (!Value)
      return llvm::Error::success();
    auto Parsed = userPageAccess(*Value, Field);
    if (!Parsed)
      return Parsed.takeError();
    Output = *Parsed;
    return llvm::Error::success();
  };
  if (auto E = UserAccess(UserInputAccessField, Result.UserInputAccess))
    return std::move(E);
  if (auto E = UserAccess(UserOutputAccessField, Result.UserOutputAccess))
    return std::move(E);
  if (const auto *Unmap = Object->get(UserUnmapAfterDispatchField)) {
    auto Value = Unmap->getAsBoolean();
    if (!Value)
      return invalid("user_unmap_after_dispatch must be a boolean");
    Result.UserUnmapAfterDispatch = *Value;
  }
  if (const auto *Exit = Object->get(RequestorExitAfterDispatchField)) {
    auto Value = Exit->getAsBoolean();
    if (!Value)
      return invalid("requestor_exit_after_dispatch must be a boolean");
    Result.RequestorExitAfterDispatch = *Value;
  }
  if (const auto *PID = Object->get(RequestorProcessIDField)) {
    auto Number = PID->getAsUINT64();
    if (!Number || *Number <= 4 || *Number > UINT32_MAX)
      return invalid("requestor_process_id must be a user process identity "
                     "from 5 through UINT32_MAX");
    Result.RequestorProcessID = static_cast<uint32_t>(*Number);
  }
  if (Result.Kind == DriverRequestKind::Pnp) {
    auto Pnp = pnpOperation(*Object);
    if (!Pnp)
      return Pnp.takeError();
    Result.Pnp = *Pnp;
    return Result;
  }
  if (Result.Kind == DriverRequestKind::Power) {
    auto Power = powerOperation(*Object, false);
    if (!Power)
      return Power.takeError();
    Result.Power = *Power;
    return Result;
  }
  const bool FileLifecycle = Result.Kind == DriverRequestKind::Create ||
                             Result.Kind == DriverRequestKind::Cleanup ||
                             Result.Kind == DriverRequestKind::Close;
  if (FileLifecycle && Object->get(BusCompletionField)) {
    auto Bus = busCompletion(*Object);
    if (!Bus)
      return Bus.takeError();
    Result.FileBusCompletion = *Bus;
  }
  if (Object->get(MinorField) ||
      (!FileLifecycle && Object->get(BusCompletionField)) ||
      Object->get(PowerTypeField) || Object->get(PowerStateField) ||
      Object->get(PowerActionField) || Object->get(SystemContextField))
    return invalid("power or pnp fields require the matching request kind");
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
  if (const auto *Async = Object->get(AsynchronousFileField)) {
    auto Value = Async->getAsBoolean();
    if (!Value)
      return invalid("asynchronous_file must be a boolean");
    Result.AsynchronousFile = *Value;
  }
  if (const auto *Cancel = Object->get(CancelAfter100nsField)) {
    auto Number = Cancel->getAsUINT64();
    if (!Number)
      return invalid("cancel_after_100ns must be a nonnegative integer");
    Result.CancelAfter100ns = *Number;
  }
  if (const auto *Defer = Object->get(DeferCallbackDrainField)) {
    auto Value = Defer->getAsBoolean();
    if (!Value)
      return invalid("defer_callback_drain must be a boolean");
    Result.DeferCallbackDrain = *Value;
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
    if ((Object->get(userField::UserBuffers) ||
         Object->get(userField::UserPointers)) &&
        (Result.ControlCode & windows::IoControlMethodMask) !=
            windows::MethodNeither)
      return invalid("user memory requires METHOD_NEITHER ioctl");
  }
  auto Bytes = [&](llvm::StringRef Field,
                   std::vector<uint8_t> &Output) -> llvm::Error {
    const auto *Input = Object->get(Field);
    if (!Input)
      return llvm::Error::success();
    auto Parsed = hexBytes(*Input, Field);
    if (!Parsed)
      return Parsed.takeError();
    Output = std::move(*Parsed);
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
          llvm::formatv("output_size must be an unsigned integer of at most "
                        "{0}",
                        DriverScenarioBufferLimit)
              .str());
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

llvm::Error validateDriverPowerOperation(const DriverPowerOperation &Operation,
                                         bool RequireDeviceType) {
  switch (Operation.Minor) {
#define NEVERD_DRIVER_POWER_REQUEST(Name, Spelling)                            \
  case DevicePowerRequest::Name:
#include "neverd/emulation/DriverPower.def"
#undef NEVERD_DRIVER_POWER_REQUEST
    break;
  default:
    return invalid("unsupported power minor");
  }
  if (RequireDeviceType && Operation.Type != DriverPowerType::Device)
    return invalid("requested_device_power requires device power type");
  switch (Operation.Type) {
  case DriverPowerType::Device:
    if (!supportedDevicePower(Operation.State))
      return invalid("device power target must be D0 or D3");
    break;
  case DriverPowerType::System:
    if (!supportedSystemPower(Operation.State))
      return invalid("system power target must be working or sleeping3");
    if (Operation.Minor == DevicePowerRequest::Query &&
        Operation.State == static_cast<uint32_t>(SystemPowerState::Working))
      return invalid("system query-power to working is unsupported");
    break;
  default:
    return invalid("unsupported power type");
  }
  switch (Operation.Action) {
#define NEVERD_DRIVER_POWER_ACTION(Name, Value, Spelling)                      \
  case DriverPowerAction::Name:
#include "neverd/emulation/DriverPower.def"
#undef NEVERD_DRIVER_POWER_ACTION
    break;
  default:
    return invalid("unsupported power action");
  }
  const auto &Bus = Operation.BusCompletion;
  if (!Bus.Status)
    return invalid("bus_completion requires an explicit status");
  if (*Bus.Status == windows::StatusPending)
    return invalid("STATUS_PENDING is not a final power completion");
  if (Bus.Delay100ns > INT64_MAX)
    return invalid("delay_100ns exceeds the signed 64-bit time limit");
  return llvm::Error::success();
}

llvm::Error validateDriverInterrupts(llvm::ArrayRef<DriverPnpDevice> Devices) {
  size_t Count = 0;
  std::map<uint32_t, const DriverInterruptResource *> Vectors;
  for (const auto &Device : Devices) {
    if (Device.Bus == DriverBusKind::ResourceFree && !Device.Interrupts.empty())
      return invalid("resource_free devices cannot have interrupts");
    if (Device.Interrupts.size() > DriverScenarioInterruptsPerDeviceLimit)
      return invalid("interrupts exceeds the per-device count limit");
    if (Device.Interrupts.size() > DriverScenarioInterruptLimit - Count)
      return invalid("interrupts exceeds the combined count limit");
    Count += Device.Interrupts.size();
    std::set<std::string> IDs;
    for (const auto &Interrupt : Device.Interrupts) {
      if (!validIdentifier(Interrupt.ID, DriverScenarioInterruptIDLimit))
        return invalid("interrupt id must be a bounded ASCII identifier");
      if (!IDs.insert(Interrupt.ID).second)
        return invalid("duplicate interrupt id '" + Interrupt.ID + "'");
      auto [Vector, Inserted] =
          Vectors.emplace(Interrupt.TranslatedVector, &Interrupt);
      if (!Inserted) {
        const auto &Peer = *Vector->second;
        if (Peer.Share != DriverInterruptShare::Shared ||
            Interrupt.Share != DriverInterruptShare::Shared ||
            Peer.Mode != Interrupt.Mode ||
            Peer.RetriggerAfter100ns != Interrupt.RetriggerAfter100ns ||
            Peer.TranslatedLevel != Interrupt.TranslatedLevel ||
            Peer.TranslatedAffinity != Interrupt.TranslatedAffinity)
          return invalid(
              "translated interrupt vectors must be globally exclusive "
              "or describe the same shared line");
      }
      switch (Interrupt.Mode) {
#define NEVERD_DRIVER_INTERRUPT_MODE(Name, Value, Spelling)                    \
  case DriverInterruptMode::Name:                                              \
    break;
#include "neverd/emulation/DriverInterrupts.def"
#undef NEVERD_DRIVER_INTERRUPT_MODE
      default:
        return invalid("unsupported interrupt mode");
      }
      if (Interrupt.Mode == DriverInterruptMode::LevelSensitive) {
        if (!Interrupt.RetriggerAfter100ns || !*Interrupt.RetriggerAfter100ns ||
            *Interrupt.RetriggerAfter100ns > INT64_MAX)
          return invalid("level-sensitive interrupts require positive "
                         "retrigger_after_100ns within signed 64-bit time");
      } else if (Interrupt.RetriggerAfter100ns) {
        return invalid(
            "latched interrupts cannot specify retrigger_after_100ns");
      }
      switch (Interrupt.Share) {
#define NEVERD_DRIVER_INTERRUPT_SHARE(Name, Value, Spelling)                   \
  case DriverInterruptShare::Name:                                             \
    break;
#include "neverd/emulation/DriverInterrupts.def"
#undef NEVERD_DRIVER_INTERRUPT_SHARE
      default:
        return invalid("unsupported interrupt share");
      }
      if (Interrupt.RawLevel > DriverInterruptRawLevelLimit)
        return invalid(
            "raw interrupt level must fit the group-zero descriptor");
      if (Interrupt.TranslatedLevel < DriverInterruptMinimumLevel ||
          Interrupt.TranslatedLevel > DriverInterruptMaximumLevel)
        return invalid(
            "translated interrupt level must be a supported device DIRQL");
      if (Interrupt.RawAffinity != DriverInterruptAffinity ||
          Interrupt.TranslatedAffinity != DriverInterruptAffinity)
        return invalid("interrupt affinity must name only CPU zero");
    }
  }
  return llvm::Error::success();
}

llvm::Error validateDriverDma(llvm::ArrayRef<DriverPnpDevice> Devices) {
  for (const auto &Device : Devices) {
    if (!Device.Dma)
      continue;
    if (Device.Bus != DriverBusKind::RegisterBank)
      return invalid("dma requires a register_bank device");
    const auto &Dma = *Device.Dma;
    if (Dma.AddressBits != 32 && Dma.AddressBits != 64)
      return invalid("DMA address_bits must be 32 or 64");
    if (!Dma.MaximumLength || Dma.MaximumLength > DriverDmaMaximumLengthLimit)
      return invalid(
          llvm::formatv("DMA maximum_length must be between 1 and {0} bytes",
                        DriverDmaMaximumLengthLimit)
              .str());
    if (!Dma.MapRegisters || Dma.MapRegisters > DriverDmaMapRegisterLimit)
      return invalid(
          llvm::formatv("DMA map_registers must be between 1 and {0}",
                        DriverDmaMapRegisterLimit)
              .str());
    if (!Dma.Alignment || Dma.Alignment > DriverDmaAlignmentLimit ||
        (Dma.Alignment & (Dma.Alignment - 1)))
      return invalid(llvm::formatv("DMA alignment must be a power of two "
                                   "between 1 and {0}",
                                   DriverDmaAlignmentLimit)
                         .str());
    if (!Dma.LogicalBase || Dma.LogicalBase % DriverDmaPageSize)
      return invalid("DMA logical_base must be nonzero and page aligned");
    if (!Dma.LogicalLength || Dma.LogicalLength > DriverDmaLogicalLengthLimit ||
        Dma.LogicalLength % DriverDmaPageSize)
      return invalid(
          llvm::formatv("DMA logical_length must be page aligned from {0} "
                        "to {1} bytes",
                        DriverDmaPageSize, DriverDmaLogicalLengthLimit)
              .str());
    if (Dma.LogicalBase > UINT64_MAX - Dma.LogicalLength)
      return invalid("DMA logical aperture overflows 64 bits");
    if (Dma.AddressBits == 32 &&
        Dma.LogicalBase + Dma.LogicalLength - 1 > UINT32_MAX)
      return invalid("DMA logical aperture exceeds address_bits");
  }
  // MDL page identities use this RAM reservation even without a DMA adapter.
  // It is independent of every device logical domain and MMIO assignment.
  for (const auto &Device : Devices)
    for (const auto &Resource : Device.Resources)
      if (Resource.Length &&
          Resource.TranslatedStart <
              DriverDmaPhysicalBase + DriverDmaPhysicalSize &&
          (Resource.TranslatedStart >= DriverDmaPhysicalBase ||
           Resource.Length > DriverDmaPhysicalBase - Resource.TranslatedStart))
        return invalid(
            "translated resource overlaps reserved DMA physical RAM");
  return llvm::Error::success();
}

namespace {
llvm::Error validateDmaEvents(const DriverOptions &Options) {
  size_t Count = 0;
  uint64_t Bytes = 0;
  for (const auto &Request : Options.Requests) {
    if (Request.AsynchronousFile && Request.Kind != DriverRequestKind::Create)
      return invalid("asynchronous_file requires a create request");
    if (!Request.DmaEvents.empty() && Request.Kind != DriverRequestKind::Read &&
        Request.Kind != DriverRequestKind::Write &&
        Request.Kind != DriverRequestKind::DeviceControl)
      return invalid(
          "dma_events is valid only for read, write and ioctl requests");
    if (Request.DmaEvents.size() > DriverScenarioDmaEventsPerRequestLimit)
      return invalid("dma_events exceeds the per-request count limit");
    if (Request.DmaEvents.size() > DriverScenarioDmaEventLimit - Count)
      return invalid("dma_events exceeds the combined count limit");
    Count += Request.DmaEvents.size();
    for (const auto &Event : Request.DmaEvents) {
      if (Event.After100ns > INT64_MAX)
        return invalid("DMA after_100ns exceeds the signed 64-bit time limit");
      if (!validDeviceID(Event.DeviceID))
        return invalid(
            "DMA event device_id must be a bounded ASCII identifier");
      const auto Device = std::find_if(
          Options.PnpDevices.begin(), Options.PnpDevices.end(),
          [&](const DriverPnpDevice &D) { return D.ID == Event.DeviceID; });
      if (Device == Options.PnpDevices.end() || !Device->Dma)
        return invalid("DMA event device_id must name a configured DMA device");
      if (!Event.Length || Event.Length > DriverDmaMaximumLengthLimit)
        return invalid("DMA event length must be between 1 and 1 MiB");
      if (Event.LogicalAddress > UINT64_MAX - Event.Length)
        return invalid("DMA event logical address interval overflows 64 bits");
      if (Event.Length > DriverScenarioDmaBytesLimit - Bytes)
        return invalid("dma_events exceeds the combined byte limit");
      Bytes += Event.Length;
      switch (Event.Direction) {
      case DriverDmaDirection::ReadMemory:
        if (!Event.Data.empty())
          return invalid("read_memory DMA events cannot contain data");
        break;
      case DriverDmaDirection::WriteMemory:
        if (Event.Data.size() != Event.Length)
          return invalid(
              "write_memory DMA events require exactly length bytes");
        break;
      default:
        return invalid("unsupported DMA direction");
      }
    }
  }
  return llvm::Error::success();
}
} // namespace

llvm::Error validateDriverResources(llvm::ArrayRef<DriverPnpDevice> Devices) {
  if (auto E = validateDriverInterrupts(Devices))
    return E;
  if (auto E = validateDriverDma(Devices))
    return E;
  using Interval = std::pair<uint64_t, uint64_t>;
  const auto HasOverlap = [](std::vector<Interval> Ranges) {
    std::sort(Ranges.begin(), Ranges.end());
    for (size_t I = 1; I < Ranges.size(); ++I)
      if (Ranges[I].first < Ranges[I - 1].second)
        return true;
    return false;
  };
  std::vector<Interval> TranslatedRanges;
  size_t ResourceCount = 0, RegisterCount = 0;
  for (const auto &Device : Devices) {
    switch (Device.Bus) {
    case DriverBusKind::ResourceFree:
      if (!Device.Resources.empty())
        return invalid("resource_free devices cannot have resources");
      continue;
    case DriverBusKind::RegisterBank:
      if (Device.Resources.empty() && Device.Interrupts.empty())
        return invalid("register_bank devices require resources or interrupts");
      break;
    default:
      return invalid("unsupported PnP bus");
    }
    if (Device.Resources.size() > DriverScenarioResourcesPerDeviceLimit)
      return invalid("resources exceeds the per-device count limit");
    if (Device.Resources.size() > DriverScenarioResourceLimit - ResourceCount)
      return invalid("resources exceeds the combined count limit");
    ResourceCount += Device.Resources.size();
    std::set<std::string> IDs;
    std::vector<Interval> RawRanges;
    for (const auto &Resource : Device.Resources) {
      if (!validIdentifier(Resource.ID, DriverScenarioResourceIDLimit))
        return invalid("resource id must be a bounded ASCII identifier");
      if (!IDs.insert(Resource.ID).second)
        return invalid("duplicate resource id '" + Resource.ID + "'");
      if (!Resource.Length ||
          Resource.Length > DriverScenarioResourceLengthLimit)
        return invalid("resource length must be between 1 and 1 MiB");
      if (Resource.RawStart > UINT64_MAX - Resource.Length ||
          Resource.TranslatedStart > UINT64_MAX - Resource.Length)
        return invalid("resource physical address interval overflows 64 bits");
      RawRanges.emplace_back(Resource.RawStart,
                             Resource.RawStart + Resource.Length);
      TranslatedRanges.emplace_back(Resource.TranslatedStart,
                                    Resource.TranslatedStart + Resource.Length);
      if (Resource.Registers.size() > DriverScenarioRegistersPerResourceLimit)
        return invalid("registers exceeds the per-resource count limit");
      if (Resource.Registers.size() >
          DriverScenarioRegisterLimit - RegisterCount)
        return invalid("registers exceeds the combined count limit");
      RegisterCount += Resource.Registers.size();
      std::vector<Interval> RegisterRanges;
      for (const auto &Register : Resource.Registers) {
        switch (Register.Width) {
#define NEVERD_DRIVER_REGISTER_WIDTH(Value) case Value:
#include "neverd/emulation/DriverResources.def"
#undef NEVERD_DRIVER_REGISTER_WIDTH
          break;
        default:
          return invalid("register width must be 1, 2 or 4 bytes");
        }
        switch (Register.Access) {
#define NEVERD_DRIVER_REGISTER_ACCESS(Name, Spelling)                          \
  case DriverRegisterAccess::Name:
#include "neverd/emulation/DriverResources.def"
#undef NEVERD_DRIVER_REGISTER_ACCESS
          break;
        default:
          return invalid("unsupported register access");
        }
        if (Register.Offset >= Resource.Length ||
            Register.Width > Resource.Length - Register.Offset)
          return invalid("register extends beyond its resource length");
        if (Register.Offset % Register.Width ||
            (Resource.TranslatedStart + Register.Offset) % Register.Width)
          return invalid("register offset and translated address must be "
                         "naturally aligned to its width");
        if (static_cast<uint64_t>(Register.Value) >=
            (uint64_t{1} << (Register.Width * 8)))
          return invalid("register value exceeds its width");
        RegisterRanges.emplace_back(Register.Offset,
                                    static_cast<uint64_t>(Register.Offset) +
                                        Register.Width);
      }
      if (HasOverlap(std::move(RegisterRanges)))
        return invalid("register intervals overlap");
    }
    if (HasOverlap(std::move(RawRanges)))
      return invalid("raw resource intervals overlap within a device");
  }
  if (HasOverlap(std::move(TranslatedRanges)))
    return invalid("translated resource intervals overlap across devices");
  return llvm::Error::success();
}

llvm::Error validateDriverUserMemory(const DriverRequest &Request) {
  if (Request.UserInputAccess && !validUserPageAccess(*Request.UserInputAccess))
    return invalid(UserInputAccessField + " has an unsupported value");
  if (Request.UserOutputAccess &&
      !validUserPageAccess(*Request.UserOutputAccess))
    return invalid(UserOutputAccessField + " has an unsupported value");
  if (Request.UserBuffers.empty() && Request.UserPointers.empty())
    return llvm::Error::success();
  const bool IOCTL = Request.Kind == DriverRequestKind::DeviceControl;
  const bool Read = Request.Kind == DriverRequestKind::Read;
  const bool Write = Request.Kind == DriverRequestKind::Write;
  if ((!IOCTL && !Read && !Write) ||
      (IOCTL && (Request.ControlCode & windows::IoControlMethodMask) !=
                    windows::MethodNeither))
    return invalid("user memory requires a neither-I/O transfer request");
  if (Request.UserBuffers.size() > DriverScenarioUserBufferLimit ||
      Request.UserPointers.size() > DriverScenarioUserPointerLimit)
    return invalid("user memory exceeds the buffer or pointer count limit");
  std::map<std::string, uint32_t> Buffers;
  for (const auto &Buffer : Request.UserBuffers) {
    if (!validIdentifier(Buffer.ID, DriverScenarioUserBufferIDLimit))
      return invalid("user buffer id must be a bounded ASCII identifier");
    if (!Buffers.emplace(Buffer.ID, Buffer.Size).second)
      return invalid("duplicate user buffer id '" + Buffer.ID + "'");
    if (!Buffer.Size || Buffer.Size > DriverScenarioBufferLimit ||
        Buffer.Input.size() > Buffer.Size)
      return invalid("user buffer size or initial bytes exceed its extent");
    if (!validUserPageAccess(Buffer.Access))
      return invalid("user buffer access has an unsupported value");
  }
  auto Size = [&](const DriverUserBufferRef &Ref) -> llvm::Expected<uint64_t> {
    if (Ref.Kind != DriverUserBufferKind::Memory && !Ref.ID.empty())
      return invalid("input/output pointer references do not accept id");
    switch (Ref.Kind) {
    case DriverUserBufferKind::Input:
      return IOCTL || Write ? Request.Input.size() : 0;
    case DriverUserBufferKind::Output:
      return IOCTL || Read ? Request.OutputSize : 0;
    case DriverUserBufferKind::Memory: {
      auto It = Buffers.find(Ref.ID);
      if (It == Buffers.end())
        return invalid("user pointer references an unknown buffer id");
      return It->second;
    }
    }
    return invalid("unsupported user pointer buffer kind");
  };
  using BufferKey = std::pair<DriverUserBufferKind, std::string>;
  using Range = std::pair<uint64_t, uint64_t>;
  std::map<BufferKey, std::vector<Range>> Slots;
  for (const auto &Pointer : Request.UserPointers) {
    auto SourceSize = Size(Pointer.Source);
    if (!SourceSize)
      return SourceSize.takeError();
    auto TargetSize = Size(Pointer.Target);
    if (!TargetSize)
      return TargetSize.takeError();
    if (Pointer.Source.Offset > *SourceSize ||
        profile::PointerSize > *SourceSize - Pointer.Source.Offset)
      return invalid("user pointer source slot exceeds its buffer");
    // A one-past pointer is valid data; guest access still needs real storage.
    if (!*TargetSize || Pointer.Target.Offset > *TargetSize)
      return invalid("user pointer target exceeds a nonempty buffer");
    Slots[{Pointer.Source.Kind, Pointer.Source.ID}].emplace_back(
        Pointer.Source.Offset, Pointer.Source.Offset + profile::PointerSize);
  }
  for (auto &[Key, Ranges] : Slots) {
    std::sort(Ranges.begin(), Ranges.end());
    for (size_t I = 1; I < Ranges.size(); ++I)
      if (Ranges[I].first < Ranges[I - 1].second)
        return invalid("user pointer source slots overlap");
  }
  return llvm::Error::success();
}

llvm::Error validateDriverScenario(const DriverOptions &Options) {
  if (auto E = validateDriverRegistry(Options.Registry))
    return invalid(llvm::toString(std::move(E)));
  if (Options.PnpDevices.size() > DriverScenarioPnpDeviceLimit)
    return invalid("pnp_devices exceeds the device count limit");
  if (auto E = validateDriverResources(Options.PnpDevices))
    return E;
  std::set<std::string> DeviceIDs;
  size_t PowerResponses = 0;
  for (const auto &Device : Options.PnpDevices) {
    if (!validDeviceID(Device.ID))
      return invalid("pnp device id must be a bounded ASCII identifier");
    if (!DeviceIDs.insert(Device.ID).second)
      return invalid("duplicate pnp device id '" + Device.ID + "'");
    if (!Device.InitialDevicePower || !Device.InitialSystemPower)
      return invalid("pnp devices require explicit initial power states");
    if (*Device.InitialDevicePower != DevicePowerState::D0 ||
        *Device.InitialSystemPower != SystemPowerState::Working)
      return invalid("pnp devices require D0 and working initial "
                     "power states");
    if (Device.InitialReportedDevicePower &&
        !supportedDevicePower(
            static_cast<uint32_t>(*Device.InitialReportedDevicePower)))
      return invalid("initial_reported_device_power must be D0 or D3");
    if (Device.RequestedDevicePower.size() >
        DriverScenarioPowerResponseLimit - PowerResponses)
      return invalid("requested_device_power exceeds the combined response "
                     "limit");
    PowerResponses += Device.RequestedDevicePower.size();
    for (const auto &Operation : Device.RequestedDevicePower)
      if (auto E = validateDriverPowerOperation(Operation, true))
        return E;
  }
  if (Options.Requests.size() > DriverScenarioRequestLimit)
    return invalid("at most 64 requests are permitted");
  if (auto E = validateDmaEvents(Options))
    return E;
  uint64_t Total = 0;
  size_t UserBufferCount = 0, UserPointerCount = 0;
  size_t InterruptEventCount = 0;
  for (const auto &Request : Options.Requests) {
    if (auto E = validateDriverUserMemory(Request))
      return E;
    if (Request.UserBuffers.size() >
            DriverScenarioUserBufferLimit - UserBufferCount ||
        Request.UserPointers.size() >
            DriverScenarioUserPointerLimit - UserPointerCount)
      return invalid("user memory exceeds the combined buffer or pointer "
                     "count limit");
    UserBufferCount += Request.UserBuffers.size();
    UserPointerCount += Request.UserPointers.size();
    if (Request.DeferCallbackDrain && Request.Kind != DriverRequestKind::Read &&
        Request.Kind != DriverRequestKind::Write &&
        Request.Kind != DriverRequestKind::DeviceControl)
      return invalid("defer_callback_drain requires a file transfer request");
    if ((Request.Kind == DriverRequestKind::Pnp ||
         Request.Kind == DriverRequestKind::Power) &&
        Request.RequestorProcessID != DriverRequest::DefaultRequestorProcessID)
      return invalid("requestor_process_id requires a file request");
    if (Request.RequestorProcessID <= 4)
      return invalid("requestor_process_id must identify a user process");
    if (Request.RequestorExitAfterDispatch && Request.UserUnmapAfterDispatch)
      return invalid("requestor_exit_after_dispatch and "
                     "user_unmap_after_dispatch are mutually exclusive");
    if (Request.RequestorExitAfterDispatch) {
      const bool NeitherIOCTL =
          Request.Kind == DriverRequestKind::DeviceControl &&
          (Request.ControlCode & windows::IoControlMethodMask) ==
              windows::MethodNeither;
      if ((!NeitherIOCTL && Request.Kind != DriverRequestKind::Read &&
           Request.Kind != DriverRequestKind::Write) ||
          (Request.Input.empty() && !Request.OutputSize &&
           Request.UserBuffers.empty()))
        return invalid("requestor_exit_after_dispatch requires a nonempty "
                       "neither-I/O transfer buffer");
    }
    if (Request.UserUnmapAfterDispatch) {
      const bool NeitherIOCTL =
          Request.Kind == DriverRequestKind::DeviceControl &&
          (Request.ControlCode & windows::IoControlMethodMask) ==
              windows::MethodNeither;
      if ((!NeitherIOCTL && Request.Kind != DriverRequestKind::Read &&
           Request.Kind != DriverRequestKind::Write) ||
          (Request.Input.empty() && !Request.OutputSize &&
           Request.UserBuffers.empty()))
        return invalid("user_unmap_after_dispatch requires a nonempty "
                       "neither-I/O transfer buffer");
    }
    if (Request.UserInputAccess || Request.UserOutputAccess) {
      const bool NeitherIOCTL =
          Request.Kind == DriverRequestKind::DeviceControl &&
          (Request.ControlCode & windows::IoControlMethodMask) ==
              windows::MethodNeither;
      if ((!NeitherIOCTL && Request.Kind != DriverRequestKind::Read &&
           Request.Kind != DriverRequestKind::Write) ||
          (Request.Kind == DriverRequestKind::Read &&
           Request.UserInputAccess) ||
          (Request.Kind == DriverRequestKind::Write &&
           Request.UserOutputAccess))
        return invalid("user page access fields require the matching "
                       "METHOD_NEITHER ioctl or neither READ/WRITE buffer");
      if ((Request.UserInputAccess && Request.Input.empty()) ||
          (Request.UserOutputAccess && !Request.OutputSize))
        return invalid("user page access requires a nonempty corresponding "
                       "neither-I/O buffer");
    }
    if (!Request.InterruptEvents.empty() &&
        Request.Kind != DriverRequestKind::Read &&
        Request.Kind != DriverRequestKind::Write &&
        Request.Kind != DriverRequestKind::DeviceControl)
      return invalid(
          "interrupt_events is valid only for read, write and ioctl requests");
    if (Request.InterruptEvents.size() >
        DriverScenarioInterruptEventsPerRequestLimit)
      return invalid("interrupt_events exceeds the per-request count limit");
    if (Request.InterruptEvents.size() >
        DriverScenarioInterruptEventLimit - InterruptEventCount)
      return invalid("interrupt_events exceeds the combined count limit");
    InterruptEventCount += Request.InterruptEvents.size();
    for (const auto &Event : Request.InterruptEvents) {
      if (Event.After100ns > INT64_MAX)
        return invalid("after_100ns exceeds the signed 64-bit time limit");
      if (!validDeviceID(Event.DeviceID) ||
          !validIdentifier(Event.InterruptID, DriverScenarioInterruptIDLimit))
        return invalid("interrupt event device_id and interrupt_id must be "
                       "bounded ASCII identifiers");
      const auto Device = std::find_if(
          Options.PnpDevices.begin(), Options.PnpDevices.end(),
          [&](const DriverPnpDevice &D) { return D.ID == Event.DeviceID; });
      if (Device == Options.PnpDevices.end())
        return invalid(
            "interrupt event device_id must name a configured pnp device");
      const auto Interrupt =
          std::find_if(Device->Interrupts.begin(), Device->Interrupts.end(),
                       [&](const DriverInterruptResource &I) {
                         return I.ID == Event.InterruptID;
                       });
      if (Interrupt == Device->Interrupts.end())
        return invalid(
            "interrupt_id must name an interrupt on the event device");
      switch (Event.Action) {
      case DriverInterruptAction::Pulse:
        if (Interrupt->Mode != DriverInterruptMode::Latched)
          return invalid(
              "level-sensitive interrupts require assert/deassert actions");
        break;
      case DriverInterruptAction::Assert:
      case DriverInterruptAction::Deassert:
        if (Interrupt->Mode != DriverInterruptMode::LevelSensitive)
          return invalid("latched interrupts require pulse actions");
        break;
      default:
        return invalid("unsupported interrupt action");
      }
    }
    if (!Request.Device.empty() && !Request.DeviceID.empty())
      return invalid("device and device_id are mutually exclusive");
    if (!Request.DeviceID.empty() && (!validDeviceID(Request.DeviceID) ||
                                      !DeviceIDs.contains(Request.DeviceID)))
      return invalid("device_id must name a configured pnp device");
    if (Request.Kind == DriverRequestKind::Pnp) {
      if (Request.DeviceID.empty() || !Request.Pnp)
        return invalid("pnp requests require device_id and a PnP operation");
      if (Request.Power || !Request.Device.empty() || Request.File ||
          Request.ControlCode || !Request.Input.empty() || Request.OutputSize ||
          !Request.DirectInput.empty() || Request.ByteOffset ||
          Request.CancelAfter100ns)
        return invalid("pnp requests cannot contain file or transfer fields");
      if (auto E = validateDriverPnpOperation(*Request.Pnp))
        return E;
      continue;
    }
    if (Request.Kind == DriverRequestKind::Power) {
      if (Request.DeviceID.empty() || !Request.Power)
        return invalid(
            "power requests require device_id and a power operation");
      if (Request.Pnp || !Request.Device.empty() || Request.File ||
          Request.ControlCode || !Request.Input.empty() || Request.OutputSize ||
          !Request.DirectInput.empty() || Request.ByteOffset ||
          Request.CancelAfter100ns)
        return invalid("power requests cannot contain file, transfer or PnP "
                       "fields");
      if (auto E = validateDriverPowerOperation(*Request.Power))
        return E;
      continue;
    }
    if (Request.Pnp)
      return invalid("PnP operation requires request kind pnp");
    if (Request.Power)
      return invalid("power operation requires request kind power");
    if (Request.FileBusCompletion) {
      if (Request.Kind != DriverRequestKind::Create &&
          Request.Kind != DriverRequestKind::Cleanup &&
          Request.Kind != DriverRequestKind::Close)
        return invalid("bus_completion requires a file lifecycle request");
      if (Request.DeviceID.empty() || !Request.FileBusCompletion->Status ||
          *Request.FileBusCompletion->Status == windows::StatusPending)
        return invalid("file bus_completion requires device_id and a "
                       "nonpending explicit final status");
      if (Request.FileBusCompletion->Delay100ns > INT64_MAX)
        return invalid("file bus_completion delay exceeds signed time range");
    }
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
      return invalid(
          llvm::formatv("device must contain at most {0} printable ASCII "
                        "bytes",
                        profile::MaxDeviceNameSize)
              .str());
    Total +=
        Request.Input.size() + Request.OutputSize + Request.DirectInput.size();
    for (const auto &Buffer : Request.UserBuffers)
      Total += Buffer.Size;
    if (Total > DriverScenarioTotalBufferLimit)
      return invalid(
          llvm::formatv("combined request buffers exceed {0} KiB",
                        DriverScenarioTotalBufferLimit / KibibyteBytes)
              .str());
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
                       [](unsigned char C) { return C >= '!' && C <= '~'; }))
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
