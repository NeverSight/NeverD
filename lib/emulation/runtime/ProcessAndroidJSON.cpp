//===- ProcessAndroidJSON.cpp - Android native request and report --------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
#include "ProcessAndroidJSON.h"

#include "../os/linux/android/AndroidDlfcn.h"
#include "../os/linux/android/AndroidJavaLibrary.h"
#include "../os/linux/android/AndroidLog.h"
#include "../os/linux/android/AndroidObservation.h"
#include "ProcessJSONInteger.h"
#include "ProcessNetworkJSON.h"

#include "neverd/dalvik/Budget.h"
#include "neverd/dalvik/Limits.h"
#include "neverd/emulation/Limits.h"
#include "neverd/emulation/ProcessReportFields.h"

#include "llvm/ADT/StringExtras.h"

#include <cstddef>
#include <cstdint>
#include <string>
#include <utility>
#include <vector>

namespace neverd::emulation {
namespace {
namespace field = process_report;
llvm::Error invalid(llvm::StringRef Key) {
  return llvm::createStringError(llvm::inconvertibleErrorCode(),
                                 llvm::Twine(field::AndroidOptions) + Key);
}
llvm::Expected<uint64_t> number(const llvm::json::Value &V,
                                llvm::StringRef Name) {
  if (auto N = V.getAsUINT64())
    return *N;
  if (auto Text = V.getAsString()) {
    unsigned Base = Text->consume_front("0x") ? 16 : 10;
    uint64_t N;
    if (!Text->empty() && !Text->getAsInteger(Base, N))
      return N;
  }
  return invalid(Name);
}
llvm::Expected<std::string> string(const llvm::json::Value &V,
                                   llvm::StringRef Name) {
  auto S = V.getAsString();
  if (!S || S->contains('\0'))
    return invalid(Name);
  return S->str();
}
std::string bits(uint64_t V) { return llvm::utohexstr(V, true); }
llvm::Expected<int32_t> integer32(const llvm::json::Value &Value,
                                  llvm::StringRef Name) {
  auto Integer = process_json::integer<int32_t>(Value);
  if (!Integer)
    return invalid(Name);
  return *Integer;
}
llvm::Expected<mobile::dalvik::AndroidApplicationSnapshot::ServiceManager>
serviceManager(const llvm::json::Value &Value) {
  using State = mobile::dalvik::AndroidApplicationSnapshot::ServiceManager;
  const auto *Object = Value.getAsObject();
  if (!Object)
    return invalid(field::ApplicationServiceManager);
  State Result;
  auto Key = [](const llvm::json::Value &Value)
      -> llvm::Expected<std::optional<uint32_t>> {
    if (Value.kind() == llvm::json::Value::Null)
      return std::optional<uint32_t>{};
    auto Number = process_json::integer<uint32_t>(Value);
    if (!Number)
      return invalid(field::ApplicationServiceManager);
    return std::optional<uint32_t>(*Number);
  };
  for (const auto &[Name, Value] : *Object) {
    if (Name == "binder_proxies") {
      const auto *Array = Value.getAsArray();
      if (!Array ||
          Array->size() > ::neverd::limits::dalvik::kMaxAndroidBinderProxies)
        return invalid(field::ApplicationServiceManager);
      for (const auto &Value : *Array) {
        const auto *Proxy = Value.getAsObject();
        if (!Proxy || !Proxy->get("key"))
          return invalid(field::ApplicationServiceManager);
        auto Number = Key(*Proxy->get("key"));
        if (!Number)
          return Number.takeError();
        if (!*Number)
          return invalid(field::ApplicationServiceManager);
        State::BinderProxy Entry;
        Entry.key = **Number;
        for (const auto &[Name, Value] : *Proxy) {
          if (Name == "key")
            continue;
          if (Name != "warn_on_blocking" || !Value.getAsBoolean())
            return invalid(field::ApplicationServiceManager);
          Entry.warn_on_blocking = *Value.getAsBoolean();
        }
        Result.binder_proxies.push_back(std::move(Entry));
      }
    } else if (Name == "cache") {
      const auto *Array = Value.getAsArray();
      if (!Array ||
          Array->size() >
              ::neverd::limits::dalvik::kMaxAndroidServiceCacheEntries)
        return invalid(field::ApplicationServiceManager);
      auto &Cache = Result.cache.emplace();
      for (const auto &Value : *Array) {
        const auto *Pair = Value.getAsObject();
        if (!Pair || Pair->size() != 2 || !Pair->get("name") ||
            !Pair->get("binder"))
          return invalid(field::ApplicationServiceManager);
        State::CacheEntry Entry;
        const auto &Name = *Pair->get("name");
        if (auto Text = Name.getAsString())
          Entry.name = Text->str();
        else if (Name.kind() != llvm::json::Value::Null)
          return invalid(field::ApplicationServiceManager);
        auto Binder = Key(*Pair->get("binder"));
        if (!Binder)
          return Binder.takeError();
        Entry.binder = *Binder;
        Cache.push_back(std::move(Entry));
      }
    } else if (Name == "lookup_results") {
      const auto *Array = Value.getAsArray();
      if (!Array ||
          Array->size() >
              ::neverd::limits::dalvik::kMaxAndroidServiceLookupResults)
        return invalid(field::ApplicationServiceManager);
      for (const auto &Value : *Array) {
        const auto *Query = Value.getAsObject();
        if (!Query || Query->size() != 4 || !Query->get("operation") ||
            !Query->get("manager") || !Query->get("name") ||
            !Query->get("binder"))
          return invalid(field::ApplicationServiceManager);
        State::LookupResult Entry;
        auto Operation = Query->getString("operation");
        if (!Operation || (*Operation != "get" && *Operation != "check"))
          return invalid(field::ApplicationServiceManager);
        Entry.operation = *Operation == "get"
                              ? State::LookupResult::Operation::Get
                              : State::LookupResult::Operation::Check;
        auto Manager = Key(*Query->get("manager"));
        if (!Manager)
          return Manager.takeError();
        if (!*Manager)
          return invalid(field::ApplicationServiceManager);
        Entry.manager = **Manager;
        const auto &Name = *Query->get("name");
        if (auto Text = Name.getAsString())
          Entry.name = Text->str();
        else if (Name.kind() != llvm::json::Value::Null)
          return invalid(field::ApplicationServiceManager);
        auto Binder = Key(*Query->get("binder"));
        if (!Binder)
          return Binder.takeError();
        Entry.binder = *Binder;
        Result.lookup_results.push_back(std::move(Entry));
      }
    } else if (Name == "manager_proxy" || Name == "context_object") {
      auto Binder = Key(Value);
      if (!Binder)
        return Binder.takeError();
      (Name == "manager_proxy" ? Result.manager_proxy : Result.context_object)
          .emplace(*Binder);
    } else {
      return invalid(field::ApplicationServiceManager);
    }
  }
  return Result;
}
llvm::Expected<mobile::dalvik::AndroidApplicationSnapshot::ApplicationInfo>
applicationInfo(const llvm::json::Value &Value) {
  const auto *Info = Value.getAsObject();
  if (!Info || !Info->get("source_dir"))
    return invalid(field::ApplicationInfo);
  for (const auto &[Key, Entry] : *Info)
    if (Key != "source_dir" && Key != "native_library_dir" &&
        Key != "non_localized_label" && Key != "target_sdk_version" &&
        Key != "split_source_dirs" && Key != "split_public_source_dirs")
      return invalid(field::ApplicationInfo);
  mobile::dalvik::AndroidApplicationSnapshot::ApplicationInfo Result;
  const auto &Entry = *Info->get("source_dir");
  if (auto Text = Entry.getAsString())
    Result.source_dir = Text->str();
  else if (Entry.kind() != llvm::json::Value::Null)
    return invalid(field::ApplicationInfo);
  for (auto [Name, Destination] :
       {std::pair{"native_library_dir", &Result.native_library_dir},
        {"non_localized_label", &Result.non_localized_label}}) {
    if (const auto *Initial = Info->get(Name)) {
      auto &Value = Destination->emplace();
      if (auto Text = Initial->getAsString())
        Value = Text->str();
      else if (Initial->kind() != llvm::json::Value::Null)
        return invalid(field::ApplicationInfo);
    }
  }
  for (auto [Name, Destination] :
       {std::pair{"split_source_dirs", &Result.split_source_dirs},
        {"split_public_source_dirs", &Result.split_public_source_dirs}}) {
    const auto *Initial = Info->get(Name);
    if (!Initial)
      continue;
    auto &Values = Destination->emplace();
    if (Initial->kind() != llvm::json::Value::Null) {
      const auto *Array = Initial->getAsArray();
      if (!Array ||
          Array->size() >
              ::neverd::limits::dalvik::kMaxAndroidApplicationSplitSources)
        return invalid(field::ApplicationInfo);
      Values.emplace();
      for (const auto &Element : *Array) {
        if (auto Text = Element.getAsString())
          Values->emplace_back(Text->str());
        else if (Element.kind() == llvm::json::Value::Null)
          Values->emplace_back();
        else
          return invalid(field::ApplicationInfo);
      }
    }
  }
  if (const auto *Initial = Info->get("target_sdk_version")) {
    auto Value = integer32(*Initial, field::ApplicationInfo);
    if (!Value)
      return Value.takeError();
    Result.target_sdk_version = *Value;
  }
  return Result;
}
llvm::Expected<mobile::dalvik::AndroidApplicationSnapshot::PackageInfo>
packageInfo(const llvm::json::Value &Value) {
  const auto *Info = Value.getAsObject();
  if (!Info)
    return invalid(field::PackageInfoQueries);
  mobile::dalvik::AndroidApplicationSnapshot::PackageInfo Result;
  for (const auto &[Key, Entry] : *Info) {
    if (Key == field::ApplicationInfo) {
      auto &Nested = Result.application_info.emplace();
      if (Entry.kind() != llvm::json::Value::Null) {
        auto Decoded = applicationInfo(Entry);
        if (!Decoded)
          return Decoded.takeError();
        Nested = std::move(*Decoded);
      }
    } else if (Key == "version_code") {
      auto Code = integer32(Entry, field::PackageInfoQueries);
      if (!Code)
        return Code.takeError();
      Result.version_code = *Code;
    } else if (Key == "package_name" || Key == "version_name") {
      auto &Text =
          (Key == "package_name" ? Result.package_name : Result.version_name)
              .emplace();
      if (auto String = Entry.getAsString())
        Text = String->str();
      else if (Entry.kind() != llvm::json::Value::Null)
        return invalid(field::PackageInfoQueries);
    } else
      return invalid(field::PackageInfoQueries);
  }
  return Result;
}
template <class Query, class Decode>
llvm::Error packageQueries(const llvm::json::Value &Value, llvm::StringRef Key,
                           std::vector<Query> &Result, size_t Maximum,
                           Decode decode) {
  const auto *Queries = Value.getAsArray();
  if (!Queries || Queries->size() > Maximum)
    return invalid(Key);
  for (const auto &Entry : *Queries) {
    const auto *Record = Entry.getAsObject();
    if (!Record || Record->size() != 3 || !Record->getString("package_name") ||
        !Record->get("flags") || !Record->get("info"))
      return invalid(Key);
    auto Flags = process_json::integer<int32_t>(*Record->get("flags"));
    if (!Flags)
      return invalid(Key);
    auto Info = decode(*Record->get("info"));
    if (!Info)
      return Info.takeError();
    Result.push_back(
        {Record->getString("package_name")->str(), *Flags, std::move(*Info)});
  }
  return llvm::Error::success();
}

llvm::Expected<std::vector<NativeMemoryRead>>
memoryReads(const llvm::json::Value &Value) {
  const auto *Array = Value.getAsArray();
  if (!Array)
    return invalid(field::ReadMemory);
  std::vector<NativeMemoryRead> Reads;
  for (const auto &Item : *Array) {
    const auto *Object = Item.getAsObject();
    if (!Object || !Object->get(field::Address) || !Object->get(field::Size))
      return invalid(field::ReadMemory);
    NativeMemoryRead Read;
    for (const auto &[Key, V] : *Object) {
      llvm::StringRef Name = Key;
      if (Name == field::Address || Name == field::Size) {
        auto N = number(V, Name);
        if (!N)
          return N.takeError();
        (Name == field::Address ? Read.Address : Read.Size) = *N;
      } else if (Name == field::RequireMappedAtEntry) {
        auto B = V.getAsBoolean();
        if (!B)
          return invalid(Name);
        Read.RequireMappedAtEntry = *B;
      } else
        return invalid(Name);
    }
    Reads.push_back(Read);
  }
  return Reads;
}
llvm::Expected<std::vector<NativeRegisterMemoryRead>>
registerMemoryReads(const llvm::json::Value &Value) {
  const auto *Array = Value.getAsArray();
  if (!Array)
    return invalid(field::ReadRegisterMemory);
  std::vector<NativeRegisterMemoryRead> Reads;
  for (const auto &Item : *Array) {
    const auto *Object = Item.getAsObject();
    if (!Object || !Object->get(field::ObservationRegister) ||
        !Object->get(field::Size))
      return invalid(field::ReadRegisterMemory);
    NativeRegisterMemoryRead Read;
    for (const auto &[Key, V] : *Object) {
      llvm::StringRef Name = Key;
      if (Name == field::ObservationRegister) {
        const auto Text = V.getAsString();
        if (!Text)
          return invalid(Name);
        if (*Text == "sp")
          Read.Register = 31;
        else {
          auto Index = *Text;
          if (!Index.consume_front("x") || Index.empty() ||
              Index.find_first_not_of("0123456789") != llvm::StringRef::npos ||
              (Index.size() > 1 && Index.front() == '0') ||
              Index.getAsInteger(10, Read.Register) || Read.Register > 30)
            return invalid(Name);
        }
      } else if (Name == field::ObservationOffset) {
        // Keep INT64_MAX exact, and reject floating-point 2^63 before the
        // JSON integer accessor can narrow that rounded boundary.
        if (auto Unsigned = V.getAsUINT64()) {
          if (*Unsigned > uint64_t(INT64_MAX))
            return invalid(Name);
          Read.Offset = int64_t(*Unsigned);
          continue;
        }
        auto Numeric = V.getAsNumber();
        if (!Numeric || *Numeric < double(INT64_MIN) ||
            *Numeric >= -double(INT64_MIN))
          return invalid(Name);
        auto Offset = V.getAsInteger();
        if (!Offset)
          return invalid(Name);
        Read.Offset = *Offset;
      } else if (Name == field::Size) {
        auto Size = number(V, Name);
        if (!Size)
          return Size.takeError();
        Read.Size = *Size;
      } else
        return invalid(Name);
    }
    Reads.push_back(Read);
  }
  return Reads;
}
llvm::Expected<NativeExecutionObservationOptions>
observationOptions(const llvm::json::Value &Value) {
  const auto *Object = Value.getAsObject();
  if (!Object)
    return invalid(field::ObserveExecution);
  NativeExecutionObservationOptions Out;
  for (const auto &[Key, V] : *Object) {
    llvm::StringRef Name = Key;
    if (Name == field::ObservationPCs) {
      const auto *PCs = V.getAsArray();
      if (!PCs)
        return invalid(Name);
      for (const auto &PC : *PCs) {
        auto N = number(PC, Name);
        if (!N)
          return N.takeError();
        Out.PCs.push_back(*N);
      }
    } else if (Name == field::ObservationLimit) {
      auto N = number(V, Name);
      if (!N)
        return N.takeError();
      Out.Limit = *N;
    } else if (Name == field::ReadMemory) {
      auto Reads = memoryReads(V);
      if (!Reads)
        return Reads.takeError();
      Out.ReadMemory = std::move(*Reads);
    } else if (Name == field::ReadRegisterMemory) {
      auto Reads = registerMemoryReads(V);
      if (!Reads)
        return Reads.takeError();
      Out.ReadRegisterMemory = std::move(*Reads);
    } else
      return invalid(Name);
  }
  if (auto E = android_model::validateExecutionObservation(Out))
    return std::move(E);
  return Out;
}
} // namespace
llvm::Expected<AndroidNativeOptions>
androidOptionsFromJSON(const llvm::json::Value &Value) {
  const auto *Object = Value.getAsObject();
  if (!Object)
    return invalid(field::Android);
  AndroidNativeOptions Out;
  for (const auto &[Key, V] : *Object) {
    llvm::StringRef Name = Key;
    if (Name == field::ResolverQueries) {
      auto Queries = androidResolverQueriesFromJSON(V);
      if (!Queries)
        return Queries.takeError();
      Out.ResolverQueries = std::move(*Queries);
    } else if (Name == field::EntrySymbol) {
      auto S = string(V, Name);
      if (!S)
        return S.takeError();
      Out.EntrySymbol = std::move(*S);
    } else if (Name == field::EntryAddress || Name == field::LoadBias ||
               Name == field::TraceLimit || Name == field::ThreadLimit) {
      auto N = number(V, Name);
      if (!N)
        return N.takeError();
      if (Name == field::EntryAddress)
        Out.EntryAddress = *N;
      else if (Name == field::LoadBias)
        Out.LoadBias = *N;
      else if (Name == field::ThreadLimit)
        Out.ThreadLimit = *N;
      else
        Out.TraceLimit = *N;
    } else if (Name == field::MallocFillByte) {
      auto N = number(V, Name);
      if (!N)
        return N.takeError();
      if (*N > UINT8_MAX)
        return invalid(Name);
      Out.MallocFillByte = static_cast<uint8_t>(*N);
    } else if (Name == field::Initialize || Name == field::DrainThreads ||
               Name == field::JNI) {
      auto B = V.getAsBoolean();
      if (!B)
        return invalid(Name);
      (Name == field::Initialize ? Out.Initialize
       : Name == field::JNI      ? Out.JNI
                                 : Out.DrainThreads) = *B;
    } else if (Name == field::EventLogWrites) {
      const auto *Writes = V.getAsArray();
      if (!Writes || Writes->size() > android_model::LogRecordLimit)
        return invalid(Name);
      Out.EventLogWrites.emplace();
      uint64_t Remaining = android_model::LogSnapshotLimit;
      for (const auto &Item : *Writes) {
        const auto *Write = Item.getAsObject();
        if (!Write || Write->size() != 2)
          return invalid(Name);
        const auto Payload = Write->getString(field::LogPayload);
        const auto *Result = Write->get(field::Result);
        if (!Payload || !Result || Payload->size() % 2 ||
            Payload->size() / 2 > android_model::LogPayloadLimit ||
            !llvm::all_of(*Payload, llvm::isHexDigit))
          return invalid(Name);
        auto Returned = integer32(*Result, field::Result);
        if (!Returned)
          return Returned.takeError();
        const uint64_t Size = Payload->size() / 2 + sizeof(int32_t);
        if (Size > Remaining)
          return invalid(Name);
        Remaining -= Size;
        const auto Bytes = llvm::fromHex(*Payload);
        Out.EventLogWrites->push_back(
            {{Bytes.begin(), Bytes.end()}, *Returned});
      }
    } else if (Name == field::LogRecords) {
      const auto *Records = V.getAsArray();
      if (!Records || Records->size() > android_model::LogRecordLimit)
        return invalid(Name);
      Out.LogRecords.emplace();
      uint64_t Remaining = android_model::LogSnapshotLimit;
      for (const auto &Item : *Records) {
        const auto *Record = Item.getAsObject();
        if (!Record || Record->size() != 7)
          return invalid(Name);
        NativeLogRecord R;
        for (const auto &[Key, Entry] : *Record) {
          llvm::StringRef K = Key;
          if (K == field::LogPayload) {
            auto Text = Entry.getAsString();
            if (!Text || Text->size() % 2 ||
                Text->size() / 2 > android_model::LogPayloadLimit ||
                !llvm::all_of(*Text, llvm::isHexDigit))
              return invalid(K);
            auto Bytes = llvm::fromHex(*Text);
            R.Payload.assign(Bytes.begin(), Bytes.end());
          } else {
            uint32_t *Destination = K == field::LogID         ? &R.ID
                                    : K == field::LogPID      ? &R.PID
                                    : K == field::LogTID      ? &R.TID
                                    : K == field::Seconds     ? &R.Seconds
                                    : K == field::Nanoseconds ? &R.Nanoseconds
                                    : K == field::FileUID     ? &R.UID
                                                              : nullptr;
            if (!Destination)
              return invalid(K);
            auto N = number(Entry, K);
            if (!N)
              return N.takeError();
            if (*N > UINT32_MAX)
              return invalid(K);
            *Destination = uint32_t(*N);
          }
        }
        const uint64_t Size = 28 + R.Payload.size();
        if (Size > Remaining)
          return invalid(Name);
        Remaining -= Size;
        Out.LogRecords->push_back(std::move(R));
      }
    } else if (Name == field::ApplicationSnapshot) {
      const auto *Input = V.getAsObject();
      if (!Input || Input->empty())
        return invalid(Name);
      mobile::dalvik::AndroidApplicationSnapshot Snapshot;
      bool HasPhase = false;
      for (const auto &[Key, Value] : *Input) {
        if (Key == field::ApplicationServiceManager) {
          auto State = serviceManager(Value);
          if (!State)
            return State.takeError();
          Snapshot.service_manager = std::move(*State);
          continue;
        } else if (Key == field::ApplicationURLFailureHandlers) {
          const auto *Handlers = Value.getAsArray();
          if (!Handlers ||
              Handlers->size() >
                  ::neverd::limits::dalvik::kMaxAndroidURLFailureHandlers)
            return invalid(Key);
          for (const auto &Entry : *Handlers) {
            const auto *Handler = Entry.getAsObject();
            if (!Handler || Handler->size() != 2 ||
                !Handler->getString("scheme") || !Handler->get("message"))
              return invalid(Key);
            mobile::dalvik::AndroidApplicationSnapshot::URLHandlerFailure H;
            H.scheme = Handler->getString("scheme")->str();
            const auto &Message = *Handler->get("message");
            if (auto Text = Message.getAsString())
              H.message = Text->str();
            else if (Message.kind() != llvm::json::Value::Null)
              return invalid(Key);
            Snapshot.url_open_failure_handlers.push_back(std::move(H));
          }
          continue;
        } else if (Key == field::ApplicationInitialMainQueue) {
          const auto *Queue = Value.getAsObject();
          if (!Queue || !Queue->getBoolean("blocked"))
            return invalid(Key);
          for (const auto &Member : *Queue)
            if (Member.first != "blocked" &&
                Member.first != field::ApplicationQueueNativeCallbacksEmpty)
              return invalid(Key);
          auto &State = Snapshot.initial_main_queue.emplace(
              mobile::dalvik::AndroidApplicationSnapshot::EmptyMessageQueue{});
          State.blocked = *Queue->getBoolean("blocked");
          if (const auto *Callbacks =
                  Queue->get(field::ApplicationQueueNativeCallbacksEmpty)) {
            const auto Empty = Callbacks->getAsBoolean();
            if (!Empty)
              return invalid(Key);
            State.native_callbacks_empty = *Empty;
          }
          continue;
        } else if (Key == field::ApplicationMessagePoolEmpty) {
          auto Empty = Value.getAsBoolean();
          if (!Empty)
            return invalid(Key);
          Snapshot.initial_message_pool_empty = *Empty;
          continue;
        } else if (Key == field::ApplicationMainLooperPrepared) {
          auto Prepared = Value.getAsBoolean();
          if (!Prepared)
            return invalid(Key);
          Snapshot.main_looper_prepared = *Prepared;
          continue;
        } else if (Key == field::ApplicationLoaderPaths ||
                   Key == field::SystemLoaderPaths) {
          const auto *Paths = Value.getAsObject();
          if (!Paths || Paths->size() != 3)
            return invalid(Key);
          auto &State = Key == field::ApplicationLoaderPaths
                            ? Snapshot.application_loader_paths.emplace()
                            : Snapshot.system_loader_paths.emplace();
          for (const auto &[Kind, Entry] : *Paths) {
            auto *Destination = Kind == "dex_zip_files" ? &State.dex_zip_files
                                : Kind == "native_library_directories"
                                    ? &State.native_library_directories
                                : Kind == "system_native_library_directories"
                                    ? &State.system_native_library_directories
                                    : nullptr;
            const auto *List = Entry.getAsArray();
            if (!Destination || !List ||
                List->size() >
                    ::neverd::limits::dalvik::kMaxAndroidClassLoaderPaths)
              return invalid(Key);
            for (const auto &Element : *List) {
              auto Text = Element.getAsString();
              if (!Text)
                return invalid(Key);
              Destination->push_back(Text->str());
            }
          }
          continue;
        } else if (Key == field::ApplicationDefaultLocale) {
          const auto *Locale = Value.getAsObject();
          if (!Locale)
            return invalid(Key);
          auto &State = Snapshot.default_locale.emplace();
          for (const auto &[Component, Entry] : *Locale) {
            auto Text = Entry.getAsString();
            if (!Text || (Component != "language" && Component != "country"))
              return invalid(Key);
            (Component == "language" ? State.language : State.country) =
                Text->str();
          }
          continue;
        } else if (Key == field::ApplicationSecureSettingsCache ||
                   Key == field::ApplicationGlobalSettingsCache) {
          const bool Secure = Key == field::ApplicationSecureSettingsCache;
          const char *Setting = Secure ? "android_id" : "adb_enabled";
          const auto *Cache = Value.getAsObject();
          if (!Cache || Cache->size() != 1 || !Cache->get(Setting))
            return invalid(Key);
          const auto &Entry = *Cache->get(Setting);
          auto &CachedValue =
              Secure ? Snapshot.secure_settings_cache.emplace().android_id
                     : Snapshot.global_settings_cache.emplace().adb_enabled;
          if (auto Text = Entry.getAsString())
            CachedValue = Text->str();
          else if (Entry.kind() != llvm::json::Value::Null)
            return invalid(Key);
          continue;
        } else if (Key == field::ApplicationInfo) {
          auto Info = applicationInfo(Value);
          if (!Info)
            return Info.takeError();
          Snapshot.application_info = std::move(*Info);
          continue;
        } else if (Key == field::ApplicationInfoQueries ||
                   Key == field::PackageInfoQueries) {
          auto Error =
              Key == field::ApplicationInfoQueries
                  ? packageQueries(Value, Key,
                                   Snapshot.application_info_queries,
                                   ::neverd::limits::dalvik::
                                       kMaxAndroidApplicationInfoQueries,
                                   applicationInfo)
                  : packageQueries(
                        Value, Key, Snapshot.package_info_queries,
                        ::neverd::limits::dalvik::kMaxAndroidPackageInfoQueries,
                        packageInfo);
          if (Error)
            return std::move(Error);
          continue;
        } else if (Key == field::ApplicationPackageResourcePath) {
          auto &Path = Snapshot.package_resource_path.emplace();
          if (auto Text = Value.getAsString())
            Path = Text->str();
          else if (Value.kind() != llvm::json::Value::Null)
            return invalid(Key);
          continue;
        } else if (Key == field::PackageItemForceSafeLabels) {
          auto Force = Value.getAsBoolean();
          if (!Force)
            return invalid(Key);
          Snapshot.package_item_force_safe_labels = *Force;
          continue;
        } else if (Key == field::ApplicationPackageManagerAvailable) {
          auto Available = Value.getAsBoolean();
          if (!Available)
            return invalid(Key);
          Snapshot.package_manager_available = *Available;
          continue;
        } else if (Key == field::ApplicationLoaderEstablished) {
          auto Established = Value.getAsBoolean();
          if (!Established)
            return invalid(Key);
          Snapshot.application_loader_established = *Established;
          continue;
        } else if (Key == field::ApplicationClassLookupFailures) {
          const auto *Failures = Value.getAsArray();
          if (!Failures ||
              Failures->size() >
                  static_cast<int>(
                      ::neverd::limits::dalvik::
                          kMaxAndroidApplicationClassLookupFailures))
            return invalid(Key);
          for (const auto &Entry : *Failures) {
            const auto *Failure = Entry.getAsObject();
            if (!Failure || Failure->size() != 3 ||
                !Failure->getString("loader") || !Failure->get("message"))
              return invalid(Key);
            auto BinaryName = Failure->getString("binary_name");
            auto ExactQuery = Failure->getString("exact_query");
            if (bool(BinaryName) == bool(ExactQuery))
              return invalid(Key);
            using Outcome =
                mobile::dalvik::AndroidApplicationSnapshot::ClassLookupFailure;
            Outcome Result;
            auto Loader = *Failure->getString("loader");
            if (Loader == "boot")
              Result.loader = Outcome::Loader::Boot;
            else if (Loader == "system")
              Result.loader = Outcome::Loader::System;
            else if (Loader == "application")
              Result.loader = Outcome::Loader::Application;
            else
              return invalid(Key);
            if (ExactQuery)
              Result.exact_query = ExactQuery->str();
            else
              Result.binary_name = BinaryName->str();
            const auto &Message = *Failure->get("message");
            if (auto Text = Message.getAsString())
              Result.message = Text->str();
            else if (Message.kind() != llvm::json::Value::Null)
              return invalid(Key);
            Snapshot.class_lookup_failures.push_back(std::move(Result));
          }
          continue;
        }
        auto Text = Value.getAsString();
        if (!Text)
          return invalid(Key);
        if (Key == field::ApplicationPhase) {
          using Phase = mobile::dalvik::AndroidApplicationSnapshot::Phase;
          if (*Text == "unattached")
            Snapshot.phase = Phase::Unattached;
          else if (*Text == "attached")
            Snapshot.phase = Phase::Attached;
          else if (*Text == "application")
            Snapshot.phase = Phase::Application;
          else
            return invalid(Key);
          HasPhase = true;
        } else if (Key == field::ApplicationPackage) {
          if (Text->empty() ||
              Text->size() >
                  static_cast<int>(
                      ::neverd::limits::dalvik::kMaxAndroidClassNameBytes) ||
              Text->contains('\0'))
            return invalid(Key);
          Snapshot.package_name = Text->str();
        } else if (Key == field::ApplicationFilesDirectory) {
          if (Text->empty() || !Text->starts_with('/') ||
              Text->size() >
                  static_cast<int>(::neverd::limits::dalvik::
                                       kMaxAndroidSharedLibraryPathBytes) ||
              Text->contains('\0'))
            return invalid(Key);
          Snapshot.files_directory = Text->str();
        } else if (Key == field::ApplicationInstrumentation) {
          using State =
              mobile::dalvik::AndroidApplicationSnapshot::Instrumentation;
          if (*Text == "uninitialized")
            Snapshot.instrumentation = State::Uninitialized;
          else if (*Text == "platform")
            Snapshot.instrumentation = State::Platform;
          else
            return invalid(Key);
        } else
          return invalid(Key);
      }
      if (!HasPhase ||
          (Snapshot.instrumentation &&
           Snapshot.phase ==
               mobile::dalvik::AndroidApplicationSnapshot::Phase::Unattached) ||
          ((Snapshot.package_name || Snapshot.files_directory) &&
           Snapshot.phase !=
               mobile::dalvik::AndroidApplicationSnapshot::Phase::Application))
        return invalid(Name);
      try {
        (void)mobile::dalvik::androidApplicationLoaderState(Snapshot);
      } catch (const mobile::Error &) {
        return invalid(field::ApplicationLoaderEstablished);
      }
      try {
        mobile::dalvik::validateAndroidClassLookupFailures(Snapshot);
      } catch (const mobile::Error &) {
        return invalid(field::ApplicationClassLookupFailures);
      }
      try {
        mobile::dalvik::validateAndroidSecureSettingsCache(Snapshot);
      } catch (const mobile::Error &) {
        return invalid(field::ApplicationSecureSettingsCache);
      }
      try {
        mobile::dalvik::validateAndroidGlobalSettingsCache(Snapshot);
      } catch (const mobile::Error &) {
        return invalid(field::ApplicationGlobalSettingsCache);
      }
      try {
        mobile::dalvik::validateAndroidApplicationInfo(Snapshot);
      } catch (const mobile::Error &) {
        return invalid(field::ApplicationInfo);
      }
      try {
        mobile::dalvik::validateAndroidApplicationInfoQueries(Snapshot);
      } catch (const mobile::Error &) {
        return invalid(field::ApplicationInfoQueries);
      }
      try {
        mobile::dalvik::validateAndroidPackageResourcePath(Snapshot);
      } catch (const mobile::Error &) {
        return invalid(field::ApplicationPackageResourcePath);
      }
      try {
        mobile::dalvik::validateAndroidPackageInfoQueries(Snapshot);
      } catch (const mobile::Error &) {
        return invalid(field::PackageInfoQueries);
      }
      try {
        mobile::dalvik::validateAndroidDefaultLocale(Snapshot);
      } catch (const mobile::Error &) {
        return invalid(field::ApplicationDefaultLocale);
      }
      try {
        mobile::dalvik::validateAndroidClassLoaderPaths(Snapshot);
        mobile::dalvik::validateAndroidMessageQueue(Snapshot);
      } catch (const mobile::Error &) {
        return invalid(field::ApplicationSnapshot);
      }
      try {
        mobile::dalvik::validateAndroidServiceManager(Snapshot);
      } catch (const mobile::Error &) {
        return invalid(field::ApplicationServiceManager);
      }
      try {
        mobile::dalvik::validateAndroidURLHandlers(Snapshot);
      } catch (const mobile::Error &) {
        return invalid(field::ApplicationURLFailureHandlers);
      }
      Out.Application = std::move(Snapshot);
    } else if (Name == field::JNIClassLookupFailures) {
      const auto *Failures = V.getAsObject();
      if (!Failures ||
          Failures->size() >
              ::neverd::limits::emulation::kMaxJNIClassLookupFailures)
        return invalid(Name);
      for (const auto &[Class, Exception] : *Failures) {
        auto Type = string(Exception, Name);
        if (!Type)
          return Type.takeError();
        Out.JNIClassLookupFailures.emplace(Class.str(), std::move(*Type));
      }
    } else if (Name == field::DexImages) {
      const auto *Images = V.getAsArray();
      if (!Images || Images->size() > 16)
        return invalid(Name);
      uint64_t Remaining = ::neverd::limits::emulation::
          kProcessAndroidJSONAndroidOptionsFromJSONRemaining;
      for (const auto &Image : *Images) {
        auto Hex = Image.getAsString();
        if (!Hex || Hex->empty() || Hex->size() % 2 ||
            Hex->size() / 2 > Remaining ||
            !llvm::all_of(*Hex, llvm::isHexDigit))
          return invalid(Name);
        Remaining -= Hex->size() / 2;
        std::string Bytes = llvm::fromHex(*Hex);
        Out.DexImages.emplace_back(Bytes.begin(), Bytes.end());
      }
    } else if (Name == field::Arguments) {
      const auto *A = V.getAsArray();
      if (!A)
        return invalid(Name);
      for (const auto &Item : *A) {
        auto N = number(Item, Name);
        if (!N)
          return N.takeError();
        Out.Arguments.push_back(*N);
      }
    } else if (Name == field::Properties) {
      const auto *P = V.getAsObject();
      if (!P)
        return invalid(Name);
      for (const auto &[Key, Item] : *P) {
        auto S = string(Item, Name);
        if (!S)
          return S.takeError();
        if (llvm::StringRef(Key).contains('\0'))
          return invalid(Name);
        Out.Properties.emplace(Key.str(), std::move(*S));
      }
    } else if (Name == field::EnvironmentVector) {
      auto Address = number(V, Name);
      if (!Address)
        return Address.takeError();
      Out.EnvironmentVector = *Address;
    } else if (Name == field::AuxiliaryVector) {
      const auto *Entries = V.getAsArray();
      if (!Entries)
        return invalid(Name);
      Out.AuxiliaryVector.emplace();
      for (const auto &Entry : *Entries) {
        const auto *O = Entry.getAsObject();
        if (!O || O->size() != 2 || !O->get(field::AuxiliaryType) ||
            !O->get(field::AuxiliaryValue))
          return invalid(Name);
        auto Type = number(*O->get(field::AuxiliaryType), field::AuxiliaryType);
        if (!Type)
          return Type.takeError();
        auto Value =
            number(*O->get(field::AuxiliaryValue), field::AuxiliaryValue);
        if (!Value)
          return Value.takeError();
        Out.AuxiliaryVector->push_back({*Type, *Value});
      }
    } else if (Name == field::FixedTimeZone) {
      const auto *Zone = V.getAsObject();
      if (!Zone || Zone->size() != 2)
        return invalid(Name);
      auto Offset = Zone->getInteger(field::ZoneOffsetSeconds);
      const auto *Text = Zone->get(field::ZoneName);
      if (!Offset || !Text || *Offset < INT32_MIN || *Offset > INT32_MAX)
        return invalid(Name);
      auto Label = string(*Text, field::ZoneName);
      if (!Label)
        return Label.takeError();
      Out.FixedTimeZone =
          AndroidFixedTimeZone{int32_t(*Offset), std::move(*Label)};
    } else if (Name == field::DladdrObservations) {
      const auto *Entries = V.getAsArray();
      if (!Entries ||
          Entries->size() > android_model::dladdr_model::MaximumRecords)
        return invalid(Name);
      Out.DladdrObservations.emplace();
      for (const auto &Entry : *Entries) {
        const auto *Record = Entry.getAsObject();
        if (!Record || Record->size() != 5 || !Record->get(field::Address) ||
            !Record->get(field::DladdrImageBase) ||
            !Record->get(field::DladdrImageName) ||
            !Record->get(field::DladdrSymbolName) ||
            !Record->get(field::DladdrSymbolAddress))
          return invalid(Name);
        NativeDladdrObservation O;
        for (auto [Key, Destination] :
             {std::pair{field::Address, &O.Address},
              {field::DladdrImageBase, &O.ImageBase},
              {field::DladdrSymbolAddress, &O.SymbolAddress}}) {
          auto N = number(*Record->get(Key), Name);
          if (!N)
            return N.takeError();
          *Destination = *N;
        }
        auto Text = string(*Record->get(field::DladdrImageName), Name);
        if (!Text)
          return Text.takeError();
        O.ImageName = std::move(*Text);
        if (Record->get(field::DladdrSymbolName)->kind() !=
            llvm::json::Value::Null) {
          auto Symbol = string(*Record->get(field::DladdrSymbolName), Name);
          if (!Symbol)
            return Symbol.takeError();
          O.SymbolName = std::move(*Symbol);
        }
        Out.DladdrObservations->push_back(std::move(O));
      }
    } else if (Name == field::DefaultScope) {
      const auto *Scope = V.getAsArray();
      if (!Scope)
        return invalid(Name);
      Out.DefaultScope.emplace();
      for (const auto &Library : *Scope) {
        auto S = string(Library, Name);
        if (!S)
          return S.takeError();
        Out.DefaultScope->push_back(std::move(*S));
      }
    } else if (Name == field::Libraries) {
      const auto *Libraries = V.getAsObject();
      if (!Libraries)
        return invalid(Name);
      for (const auto &[Library, Entry] : *Libraries) {
        const auto *Symbols = Entry.getAsArray();
        if (!Symbols || llvm::StringRef(Library).empty() ||
            llvm::StringRef(Library).contains('\0'))
          return invalid(Name);
        auto &Names = Out.Libraries[Library.str()];
        for (const auto &Symbol : *Symbols) {
          auto S = string(Symbol, Name);
          if (!S)
            return S.takeError();
          if (S->empty() || llvm::is_contained(Names, *S))
            return invalid(Name);
          Names.push_back(std::move(*S));
        }
      }
    } else if (Name == field::ObserveExecution) {
      auto Observation = observationOptions(V);
      if (!Observation)
        return Observation.takeError();
      Out.ObserveExecution = std::move(*Observation);
    } else if (Name == field::JavaLibrary) {
      auto S = string(V, Name);
      if (!S || !android_model::validJavaLibraryName(*S)) {
        if (!S)
          llvm::consumeError(S.takeError());
        return invalid(Name);
      }
      Out.JavaLibrary = std::move(*S);
    } else if (Name == field::ObserveJavaState ||
               Name == field::ObserveCPUState ||
               Name == field::ObserveThreadCPUStates ||
               Name == field::ObserveMemoryAtStop ||
               Name == field::ObserveMemoryOnFailure) {
      auto B = V.getAsBoolean();
      if (!B)
        return invalid(Name);
      if (Name == field::ObserveJavaState)
        Out.ObserveJavaState = *B;
      else if (Name == field::ObserveCPUState)
        Out.ObserveCPUState = *B;
      else if (Name == field::ObserveThreadCPUStates)
        Out.ObserveThreadCPUStates = *B;
      else if (Name == field::ObserveMemoryAtStop)
        Out.ObserveMemoryAtStop = *B;
      else
        Out.ObserveMemoryOnFailure = *B;
    } else if (Name == field::ReadMemory) {
      auto Reads = memoryReads(V);
      if (!Reads)
        return Reads.takeError();
      Out.ReadMemory = std::move(*Reads);
    } else if (Name == field::Memory) {
      const auto *A = V.getAsArray();
      if (!A)
        return invalid(Name);
      for (const auto &Item : *A) {
        const auto *O = Item.getAsObject();
        if (!O || !O->get(field::Address) || !O->get(field::Size))
          return invalid(Name);
        if (O->get(field::Path) && O->get(field::Bytes))
          return invalid(field::Path);
        NativeMemoryRegion Region;
        for (const auto &[K, Entry] : *O) {
          llvm::StringRef Field = K;
          if (Field == field::Address || Field == field::Size) {
            auto N = number(Entry, Field);
            if (!N)
              return N.takeError();
            (Field == field::Address ? Region.Address : Region.Size) = *N;
          } else if (Field == field::Bytes) {
            auto S = Entry.getAsString();
            if (!S || S->size() % 2 || !llvm::all_of(*S, llvm::isHexDigit))
              return invalid(Field);
            std::string Data = llvm::fromHex(*S);
            Region.Bytes.assign(Data.begin(), Data.end());
          } else if (Field == field::Path) {
            auto S = string(Entry, Field);
            if (!S)
              return S.takeError();
            if (S->empty())
              return invalid(Field);
            Region.File = std::filesystem::path(std::u8string(
                reinterpret_cast<const char8_t *>(S->data()), S->size()));
          } else if (Field == field::Executable) {
            auto B = Entry.getAsBoolean();
            if (!B)
              return invalid(Field);
            Region.Executable = *B;
          } else
            return invalid(Field);
        }
        Out.Memory.push_back(std::move(Region));
      }
    } else
      return invalid(Name);
  }
  if (auto E =
          android_model::validateDladdrObservations(Out.DladdrObservations))
    return std::move(E);
  if (auto E = android_model::validateLogRecords(Out.LogRecords))
    return std::move(E);
  if (auto E = android_model::validateEventLogWrites(Out.EventLogWrites))
    return std::move(E);
  if (Out.ObserveJavaState && !Out.JNI)
    return invalid(field::ObserveJavaState);
  if (Out.JavaLibrary && (!Out.JNI || !Out.Initialize))
    return invalid(field::JavaLibrary);
  return Out;
}
llvm::json::Object androidResultJSON(const ProcessResult &Result) {
  llvm::json::Array Calls, Trace, Memory;
  for (const auto &Event : Result.NativeCalls) {
    llvm::json::Array Args;
    for (uint64_t A : Event.Arguments)
      Args.push_back(bits(A));
    llvm::json::Object Call{
        {field::PC, bits(Event.PC)},
        {field::Name, Event.Name},
        {field::Arguments, std::move(Args)},
        {field::Result,
         Event.Result ? llvm::json::Value(bits(*Event.Result)) : nullptr}};
    if (!Event.Library.empty())
      Call[field::Library] = Event.Library;
    if (!Event.Symbol.empty())
      Call[field::Symbol] = Event.Symbol;
    if (Event.ThreadID)
      Call[field::ThreadID] = *Event.ThreadID;
    Calls.push_back(std::move(Call));
  }
  for (uint64_t PC : Result.Trace)
    Trace.push_back(bits(PC));
  for (const auto &Snapshot : Result.MemorySnapshots) {
    llvm::json::Object Value{{field::Address, bits(Snapshot.Address)},
                             {field::Bytes, llvm::toHex(Snapshot.Bytes, true)}};
    if (!Snapshot.Diagnostic.empty())
      Value[field::Diagnostic] = Snapshot.Diagnostic;
    Memory.push_back(std::move(Value));
  }
  llvm::json::Object Out{{field::Initialize, Result.InitializersEnabled},
                         {field::NativeCalls, std::move(Calls)},
                         {field::Trace, std::move(Trace)},
                         {field::TraceTruncated, Result.TraceTruncated},
                         {field::Memory, std::move(Memory)}};
  if (Result.EventLogWritesConsumed)
    Out[field::EventLogWritesConsumed] = *Result.EventLogWritesConsumed;
  auto CPUStateJSON = [](const NativeCPUState &State) {
    llvm::json::Value Registers = nullptr;
    if (State.Registers) {
      llvm::json::Object Values;
      for (const auto &R : *State.Registers)
        Values[registerName(R.Register)] =
            llvm::json::Array{bits(R.Value[0]), bits(R.Value[1])};
      Registers = std::move(Values);
    }
    return llvm::json::Object{
        {field::ThreadID, State.ThreadID},
        {field::ObservationRegisters, std::move(Registers)},
        {field::Diagnostic, State.Diagnostic}};
  };
  if (Result.CPUState)
    Out[field::CPUState] = CPUStateJSON(*Result.CPUState);
  if (Result.ThreadCPUStates) {
    llvm::json::Array States;
    for (const auto &Thread : *Result.ThreadCPUStates) {
      auto State = CPUStateJSON(Thread.State);
      State[field::CurrentCPU] = Thread.Current;
      States.push_back(std::move(State));
    }
    Out[field::ThreadCPUStates] = std::move(States);
  }
  if (!Result.JNIRegistrations.empty()) {
    llvm::json::Array Bindings;
    for (const auto &Binding : Result.JNIRegistrations)
      Bindings.push_back(
          llvm::json::Object{{field::JNIClass, Binding.Class},
                             {field::Name, Binding.Name},
                             {field::JNISignature, Binding.Signature},
                             {field::Address, bits(Binding.Address)}});
    Out[field::JNIRegistrations] = std::move(Bindings);
  }
  if (Result.JavaState) {
    using namespace mobile::dalvik;
    const auto &State = *Result.JavaState;
    const auto &F = State.framework;
    auto Phase = [](AndroidApplicationSnapshot::Phase V) {
      switch (V) {
      case AndroidApplicationSnapshot::Phase::Unattached:
        return "unattached";
      case AndroidApplicationSnapshot::Phase::Attached:
        return "attached";
      case AndroidApplicationSnapshot::Phase::Application:
        return "application";
      }
      llvm_unreachable("invalid Android phase");
    };
    auto Loader = [](AndroidApplicationSnapshot::ClassLookupFailure::Loader V) {
      using L = AndroidApplicationSnapshot::ClassLookupFailure::Loader;
      switch (V) {
      case L::Boot:
        return "boot";
      case L::System:
        return "system";
      case L::Application:
        return "application";
      case L::Unspecified:
        return "unspecified";
      }
      llvm_unreachable("invalid Android loader");
    };
    auto Initialization = [](JavaClassState::Initialization V) {
      using I = JavaClassState::Initialization;
      switch (V) {
      case I::NotStarted:
        return "not_started";
      case I::Prepared:
        return "prepared";
      case I::Initializing:
        return "initializing";
      case I::Initialized:
        return "initialized";
      case I::Erroneous:
        return "erroneous";
      }
      llvm_unreachable("invalid Java initialization state");
    };
    llvm::json::Object Framework{
        {field::JavaDeclaredPhase,
         F.declared_phase ? llvm::json::Value(Phase(*F.declared_phase))
                          : nullptr},
        {field::JavaActivityThread, bits(F.activity_thread)},
        {field::ApplicationLoaderEstablished,
         F.application_loader_established
             ? llvm::json::Value(*F.application_loader_established)
             : nullptr},
        {field::JavaApplication, bits(F.application)},
        {field::JavaContext, bits(F.context)},
        {field::JavaBootLoader, bits(F.boot_loader)},
        {field::JavaSystemLoader, bits(F.system_loader)},
        {field::JavaApplicationLoader, bits(F.application_loader)},
        {field::ApplicationInstrumentation,
         F.instrumentation ? llvm::json::Value(bits(*F.instrumentation))
                           : nullptr}};
    if (F.main_message_queue) {
      const auto &Queue = *F.main_message_queue;
      llvm::json::Array Messages;
      for (const auto &Message : Queue.messages)
        Messages.push_back(
            llvm::json::Object{{"identity", bits(Message.identity)},
                               {"target", bits(Message.target)},
                               {"callback", bits(Message.callback)},
                               {"next", bits(Message.next)},
                               {"when", Message.when},
                               {"flags", Message.flags}});
      Framework[field::JavaMainMessageQueue] =
          llvm::json::Object{{"identity", bits(Queue.identity)},
                             {"blocked", Queue.blocked},
                             {"complete", Queue.complete},
                             {"messages", std::move(Messages)}};
    }
    if (F.service_manager) {
      const auto &State = *F.service_manager;
      llvm::json::Array Entries, Proxies;
      for (const auto &Entry : State.entries)
        Entries.push_back(llvm::json::Object{{"name", bits(Entry.name)},
                                             {"binder", bits(Entry.binder)}});
      for (const auto &Proxy : State.binder_proxies)
        Proxies.push_back(llvm::json::Object{
            {"key", Proxy.key},
            {"identity", bits(Proxy.identity)},
            {"warn_on_blocking",
             Proxy.warn_on_blocking ? llvm::json::Value(*Proxy.warn_on_blocking)
                                    : nullptr}});
      Framework[field::ApplicationServiceManager] = llvm::json::Object{
          {"initialized", State.initialized},
          {"cache", bits(State.cache)},
          {"cache_known", State.cache_known},
          {"entries", std::move(Entries)},
          {"stat_logger", bits(State.stat_logger)},
          {"manager",
           State.manager ? llvm::json::Value(bits(*State.manager)) : nullptr},
          {"binder_proxies", std::move(Proxies)}};
    }
    llvm::json::Array Classes;
    for (const auto &C : State.classes)
      Classes.push_back(llvm::json::Object{
          {field::JavaDescriptor, C.descriptor},
          {field::JavaSourceID, C.source_id},
          {field::JavaSuperclass,
           C.superclass ? llvm::json::Value(*C.superclass) : nullptr},
          {field::JavaSupplied, C.supplied},
          {field::JavaInitialization, Initialization(C.initialization)},
          {field::JavaClassObject, bits(C.class_object)},
          {field::JavaProjectedLoader,
           C.projected_loader ? llvm::json::Value(Loader(*C.projected_loader))
                              : nullptr}});
    Out[field::JavaState] =
        llvm::json::Object{{field::JavaClasses, std::move(Classes)},
                           {field::JavaFramework, std::move(Framework)},
                           {field::JavaTerminal, State.terminal}};
  }
  if (Result.ExecutionObservationsEnabled) {
    llvm::json::Array Observations;
    for (const auto &Record : Result.ExecutionObservations) {
      llvm::json::Array Registers, Reads;
      for (uint64_t R : Record.Registers)
        Registers.push_back(bits(R));
      for (const auto &Read : Record.Memory)
        Reads.push_back(
            llvm::json::Object{{field::Address, bits(Read.Address)},
                               {field::Bytes, llvm::toHex(Read.Bytes, true)}});
      Observations.push_back(llvm::json::Object{
          {field::PC, bits(Record.PC)},
          {field::ThreadID, Record.ThreadID},
          {field::Instructions, Record.Instructions},
          {field::ObservationSP, bits(Record.SP)},
          {field::ObservationNZCV, bits(Record.NZCV)},
          {field::ObservationRegisters, std::move(Registers)},
          {field::Memory, std::move(Reads)}});
    }
    Out[field::ExecutionObservations] = std::move(Observations);
    Out[field::ExecutionObservationsTruncated] =
        Result.ExecutionObservationsTruncated;
  }
  if (!Result.NativeThreads.empty()) {
    llvm::json::Array Threads, Spans;
    for (const auto &T : Result.NativeThreads) {
      llvm::json::Object Thread{
          {field::ThreadID, T.ID},
          {field::Handle, bits(T.Handle)},
          {field::TLS, bits(T.TLS)},
          {field::StackBase, bits(T.StackBase)},
          {field::StackBytes, T.StackSize},
          {field::GuardSize, T.GuardSize},
          {field::Finished, T.Finished},
          {field::Detached, T.Detached},
          {field::Retired, T.Retired},
          {field::Waiting, T.Waiting},
          {field::ReturnValue,
           T.ReturnValue ? llvm::json::Value(bits(*T.ReturnValue)) : nullptr}};
      if (T.TerminationSignal)
        Thread[field::TerminationSignal] = *T.TerminationSignal;
      Threads.push_back(std::move(Thread));
    }
    for (const auto &Span : Result.TraceThreads)
      Spans.push_back(llvm::json::Object{{field::TraceIndex, Span.Index},
                                         {field::ThreadID, Span.ID}});
    Out[field::Threads] = std::move(Threads);
    Out[field::TraceThreads] = std::move(Spans);
  }
  return Out;
}
} // namespace neverd::emulation
