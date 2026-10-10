//===- ProcessDarwinSystemJSON.cpp - Explicit system inputs ---------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
#include "ProcessDarwinSystemJSON.h"

#include "../os/darwin/kernel/DarwinSystem.h"
#include "ProcessJSONInteger.h"

#include "neverd/emulation/ProcessReportFields.h"

#include "llvm/ADT/STLExtras.h"
#include "llvm/ADT/StringExtras.h"

#include <type_traits>

namespace neverd::emulation {
namespace {
namespace field = process_report;
llvm::Error invalid(llvm::StringRef Name) {
  return llvm::createStringError(llvm::inconvertibleErrorCode(),
                                 field::DarwinSystemOptions + Name);
}
template <typename T>
bool parse(const llvm::json::Value &Value, std::optional<T> &Out) {
  if constexpr (std::is_same_v<T, std::string>) {
    auto Text = Value.getAsString();
    if (!Text)
      return false;
    Out = Text->str();
  } else if constexpr (std::is_same_v<T, bool>) {
    Out = Value.getAsBoolean();
  } else if constexpr (std::is_same_v<T, std::vector<uint8_t>>) {
    auto Text = Value.getAsString();
    if (!Text || Text->size() != 2 * darwin_model::value::LoginNameSize)
      return false;
    T Bytes(darwin_model::value::LoginNameSize);
    for (size_t I = 0; I != Bytes.size(); ++I) {
      const unsigned High = llvm::hexDigitValue((*Text)[2 * I]);
      const unsigned Low = llvm::hexDigitValue((*Text)[2 * I + 1]);
      if (High >= 16 || Low >= 16)
        return false;
      Bytes[I] = uint8_t(High * 16 + Low);
    }
    Out = std::move(Bytes);
  } else {
    Out = process_json::integer<T>(Value);
  }
  return Out.has_value();
}
std::optional<DarwinResourceUsage> parseUsage(const llvm::json::Value &Value) {
  const auto *Object = Value.getAsObject();
  if (!Object || Object->size() != 5)
    return std::nullopt;
  const auto *US = Object->get(field::ResourceUsageUserSeconds);
  const auto *UM = Object->get(field::ResourceUsageUserMicroseconds);
  const auto *SS = Object->get(field::ResourceUsageSystemSeconds);
  const auto *SM = Object->get(field::ResourceUsageSystemMicroseconds);
  const auto *C = Object->get(field::ResourceUsageCounters);
  if (!US || !UM || !SS || !SM || !C)
    return std::nullopt;
  auto UserSeconds = process_json::integer<int64_t>(*US);
  auto UserMicroseconds = process_json::integer<uint32_t>(*UM);
  auto SystemSeconds = process_json::integer<int64_t>(*SS);
  auto SystemMicroseconds = process_json::integer<uint32_t>(*SM);
  const auto *Counters = C->getAsArray();
  if (!UserSeconds || !UserMicroseconds || !SystemSeconds ||
      !SystemMicroseconds || !Counters ||
      Counters->size() != DarwinResourceUsage::CounterCount)
    return std::nullopt;
  DarwinResourceUsage Out{
      *UserSeconds, *UserMicroseconds, *SystemSeconds, *SystemMicroseconds, {}};
  for (size_t I = 0; I != Out.Counters.size(); ++I) {
    auto Counter = process_json::integer<int64_t>((*Counters)[I]);
    if (!Counter)
      return std::nullopt;
    Out.Counters[I] = *Counter;
  }
  return Out;
}
} // namespace
llvm::Expected<DarwinSystemOptions>
darwinSystemOptionsFromJSON(const llvm::json::Value &Value) {
  const auto *Object = Value.getAsObject();
  if (!Object)
    return invalid(field::DarwinSystem);
  DarwinSystemOptions Out;
  for (const auto &[Key, V] : *Object) {
    const llvm::StringRef Name = Key;
    if (Name == field::SystemCredentials) {
      const auto *C = V.getAsObject();
      if (!C || C->size() < 4 || C->size() > 5)
        return invalid(Name);
      DarwinCredentials Credentials;
      const llvm::StringRef Keys[] = {
          field::CredentialRealUID, field::CredentialEffectiveUID,
          field::CredentialRealGID, field::CredentialEffectiveGID};
      uint32_t *IDs[] = {&Credentials.RealUID, &Credentials.EffectiveUID,
                         &Credentials.RealGID, &Credentials.EffectiveGID};
      for (size_t I = 0; I != 4; ++I) {
        const auto *Input = C->get(Keys[I]);
        if (!Input)
          return invalid(Name);
        auto ID = process_json::integer<uint32_t>(*Input);
        if (!ID)
          return invalid(Name);
        *IDs[I] = *ID;
      }
      for (const auto &[K, Input] : *C) {
        if (K == field::CredentialGroups) {
          const auto *Groups = Input.getAsArray();
          if (!Groups || Groups->size() > darwin_model::value::GroupAccessLimit)
            return invalid(Name);
          auto &OutGroups = Credentials.GroupAccessList.emplace();
          for (const auto &Group : *Groups) {
            auto ID = process_json::integer<uint32_t>(Group);
            if (!ID)
              return invalid(Name);
            OutGroups.push_back(*ID);
          }
        } else if (!llvm::is_contained(Keys, llvm::StringRef(K)))
          return invalid(Name);
      }
      Out.Credentials = std::move(Credentials);
      continue;
    }
    if (Name == field::SystemResourceUsage) {
      const auto *Usages = V.getAsObject();
      if (!Usages || Usages->size() > 2)
        return invalid(Name);
      for (const auto &[K, Snapshot] : *Usages) {
        auto Usage = parseUsage(Snapshot);
        if (!Usage)
          return invalid(Name);
        if (K == field::ResourceUsageSelf)
          Out.ResourceUsageSelf = *Usage;
        else if (K == field::ResourceUsageChildren)
          Out.ResourceUsageChildren = *Usage;
        else
          return invalid(Name);
      }
      continue;
    }
    if (Name == field::SystemResourceLimits) {
      const auto *Limits = V.getAsArray();
      if (!Limits || Limits->size() > darwin_model::value::ResourceLimitCount)
        return invalid(Name);
      for (const auto &Entry : *Limits) {
        const auto *Limit = Entry.getAsObject();
        if (!Limit || Limit->size() != 3)
          return invalid(Name);
        const auto *R = Limit->get(field::ResourceLimitResource);
        const auto *C = Limit->get(field::ResourceLimitCurrent);
        const auto *M = Limit->get(field::ResourceLimitMaximum);
        if (!R || !C || !M)
          return invalid(Name);
        auto Resource = process_json::integer<uint32_t>(*R);
        auto Current = process_json::integer<uint64_t>(*C);
        auto Maximum = process_json::integer<uint64_t>(*M);
        if (!Resource || !Current || !Maximum ||
            !Out.ResourceLimits
                 .emplace(*Resource, DarwinResourceLimit{*Current, *Maximum})
                 .second)
          return invalid(Name);
      }
      continue;
    }
    if (Name == field::SystemEntropyReads) {
      const auto *Reads = V.getAsArray();
      if (!Reads || Reads->size() > darwin_model::value::EntropyReplayLimit)
        return invalid(Name);
      Out.EntropyReads.emplace();
      for (const auto &Read : *Reads) {
        auto Hex = Read.getAsString();
        if (!Hex || Hex->empty() || Hex->size() % 2 ||
            Hex->size() / 2 > darwin_model::value::EntropyReadLimit)
          return invalid(Name);
        std::vector<uint8_t> Bytes(Hex->size() / 2);
        for (size_t I = 0; I != Bytes.size(); ++I) {
          const unsigned High = llvm::hexDigitValue((*Hex)[2 * I]);
          const unsigned Low = llvm::hexDigitValue((*Hex)[2 * I + 1]);
          if (High >= 16 || Low >= 16)
            return invalid(Name);
          Bytes[I] = uint8_t(High * 16 + Low);
        }
        Out.EntropyReads->push_back(std::move(Bytes));
      }
      continue;
    }
#define NEVERD_DARWIN_SYSTEM_FIELD(Member, Field, NativeName, Root, Leaf)      \
  if (Name == field::Field) {                                                  \
    if (!parse(V, Out.Member))                                                 \
      return invalid(Name);                                                    \
    continue;                                                                  \
  }
#define NEVERD_DARWIN_PROCESS_FIELD(Member, Field)                             \
  if (Name == field::Field) {                                                  \
    if (!parse(V, Out.Member))                                                 \
      return invalid(Name);                                                    \
    continue;                                                                  \
  }
#include "../os/darwin/kernel/DarwinSystemFields.def"
#undef NEVERD_DARWIN_SYSTEM_FIELD
#undef NEVERD_DARWIN_PROCESS_FIELD
    return invalid(Name);
  }
  if (auto E = darwin_model::validateSystemOptions(Out))
    return std::move(E);
  return Out;
}
} // namespace neverd::emulation
