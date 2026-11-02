//===- AndroidServiceManager.cpp - Explicit Java service cache state
//-------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// API 28 initialization, cached Binder identities and explicit lookup replies.
//===----------------------------------------------------------------------===//
#include "AndroidEventLog.h"
#include "AndroidFramework.h"

#include "neverd/dalvik/Limits.h"

#include <algorithm>
#include <bit>
#include <initializer_list>
#include <optional>
#include <set>
#include <string>
#include <tuple>
#include <utility>
#include <vector>

namespace neverd::mobile::dalvik {
namespace {
constexpr auto Manager = "Landroid/os/ServiceManager;";
constexpr auto Proxy = "Landroid/os/ServiceManagerProxy;";
struct Stop {
  ExecutionResult result;
};
uint64_t take(ExecutionResult result) {
  if (result.stop != ExecutionStop::Returned)
    throw Stop{std::move(result)};
  return result.value;
}
void tick(const AdmitInstruction &admit) {
  take({admit(), 0, {}, "Android service state work budget exhausted"});
}
uint32_t objectIdentity(const std::shared_ptr<Object> &object) {
  if (!object)
    throw Stop{{ExecutionStop::HeapLimit,
                0,
                {},
                "Android service state storage budget exhausted"}};
  return object->Identity;
}
ExecutionResult unsupported(std::string text) {
  return {ExecutionStop::Unsupported, 0, {}, std::move(text)};
}
} // namespace

void validateAndroidServiceManager(const AndroidApplicationSnapshot &input) {
  if (!input.service_manager)
    return;
  const auto &state = *input.service_manager;
  if (state.binder_proxies.size() >
      ::neverd::limits::dalvik::kMaxAndroidBinderProxies)
    throw Error("Android Binder proxy snapshot exceeds its budget");
  std::set<uint32_t> keys;
  for (const auto &proxy : state.binder_proxies)
    if (!proxy.key || !keys.insert(proxy.key).second)
      throw Error("Android Binder proxy keys must be positive and unique");
  auto binder = [&](std::optional<uint32_t> key) {
    if (key && !keys.contains(*key))
      throw Error(
          "Android service state references an undeclared Binder proxy");
  };
  if (state.manager_proxy)
    binder(*state.manager_proxy);
  if (state.context_object)
    binder(*state.context_object);
  if (state.cache) {
    if (state.cache->size() >
        ::neverd::limits::dalvik::kMaxAndroidServiceCacheEntries)
      throw Error("Android service cache snapshot exceeds its budget");
    std::set<std::optional<std::vector<uint8_t>>> names;
    for (const auto &entry : *state.cache) {
      std::optional<std::vector<uint8_t>> name;
      if (entry.name) {
        if (entry.name->size() >
            ::neverd::limits::dalvik::kMaxAndroidServiceNameBytes)
          throw Error("Android service cache name exceeds its budget");
        name = javaStringBytes(*entry.name);
      }
      if (!names.insert(std::move(name)).second)
        throw Error("Android service cache contains a duplicate Java key");
      binder(entry.binder);
    }
  }
  using Query = AndroidApplicationSnapshot::ServiceManager::LookupResult;
  if (state.lookup_results.size() >
      ::neverd::limits::dalvik::kMaxAndroidServiceLookupResults)
    throw Error("Android service lookup results exceed their budget");
  std::set<std::tuple<uint32_t, Query::Operation,
                      std::optional<std::vector<uint8_t>>>>
      queries;
  for (const auto &query : state.lookup_results) {
    if (query.operation != Query::Operation::Get &&
        query.operation != Query::Operation::Check)
      throw Error("invalid Android service lookup operation");
    binder(query.manager);
    binder(query.binder);
    std::optional<std::vector<uint8_t>> name;
    if (query.name) {
      if (query.name->size() >
          ::neverd::limits::dalvik::kMaxAndroidServiceNameBytes)
        throw Error("Android service lookup name exceeds its budget");
      name = javaStringBytes(*query.name);
    }
    if (!queries.emplace(query.manager, query.operation, std::move(name))
             .second)
      throw Error("Android service lookup results contain a duplicate query");
  }
}

ExecutionResult AndroidFramework::binderProxy(Heap &heap, uint32_t key,
                                              const AdmitInstruction &admit) {
  if (auto entry = binder_identities.find(key);
      entry != binder_identities.end())
    return {ExecutionStop::Returned, entry->second};
  // All references were checked against this immutable input on admission.
  const auto &proxies = snapshot->service_manager->binder_proxies;
  const auto entry =
      std::find_if(proxies.begin(), proxies.end(),
                   [&](const auto &proxy) { return proxy.key == key; });
  try {
    tick(admit);
    auto object = heap.allocate(
        Object::Instance, "android/os/BinderProxy", 0,
        ::neverd::limits::dalvik::kFrameworkFieldStateOverheadBytes);
    const auto id = objectIdentity(object);
    binder_identities.emplace(key, id);
    binder_warnings.emplace(id, entry->warn_on_blocking);
    return {ExecutionStop::Returned, id};
  } catch (Stop &stop) {
    return std::move(stop.result);
  }
}

ExecutionResult AndroidFramework::serviceProxy(Heap &heap,
                                               const Program &program,
                                               uint64_t binder,
                                               const AdmitInstruction &admit) {
  try {
    tick(admit);
    auto object =
        heap.allocate(Object::Instance, "android/os/ServiceManagerProxy", 0,
                      ::neverd::limits::dalvik::kInstanceFieldStorageBytes);
    objectIdentity(object);
    object->Fields.emplace(
        program.findField({Proxy, "mRemote", "Landroid/os/IBinder;"}, false),
        binder);
    return {ExecutionStop::Returned, object->Identity};
  } catch (Stop &stop) {
    return std::move(stop.result);
  }
}

ExecutionResult AndroidFramework::initializeServiceManager(
    Heap &heap, const Program &program, const AdmitInstruction &admit,
    const std::function<ExecutionResult(std::string_view)> &initialize) {
  auto field = [&](const char *name,
                   const char *type) -> std::optional<uint64_t> & {
    return service_fields.at(program.findField({Manager, name, type}, true));
  };
  auto call = [&](const MethodRef &method, bool is_static, uint64_t receiver,
                  std::initializer_list<uint64_t> args = {}) {
    return take(executeLibrary(heap, *libraryMethod(method, is_static),
                               receiver, args, admit, false, &program, this));
  };
  try {
    for (const auto &entry : program.findClass(Manager)->fields) {
      tick(admit);
      uint64_t value = 0;
      if (auto text = std::get_if<std::string>(&entry.value))
        value = objectIdentity(heap.literal(*text));
      else if (auto number = std::get_if<int64_t>(&entry.value))
        value = *number;
      service_fields.emplace(&entry, value);
    }
    const auto *input = snapshot && snapshot->service_manager
                            ? &*snapshot->service_manager
                            : nullptr;
    // Existing process state is independent of running this modeled class
    // initializer. A missing observation must not become null or an empty map.
    auto &manager = field("sServiceManager", "Landroid/os/IServiceManager;");
    manager.reset();
    tick(admit);
    field("sLock", "Ljava/lang/Object;") =
        objectIdentity(heap.allocate(Object::Instance, "java/lang/Object", 0));
    take(initialize("Ljava/util/HashMap;"));
    const auto cache =
        call({"Ljava/util/HashMap;", "<init>", {}, "V"}, false, 0);
    field("sCache", "Ljava/util/HashMap;") = cache;
    if (!input || !input->cache) {
      heap.get(cache)->MapEntries.reset();
    } else {
      for (const auto &entry : *input->cache) {
        const auto name =
            entry.name ? take(stringValue(heap, *entry.name, admit)) : 0;
        const auto binder =
            entry.binder ? take(binderProxy(heap, *entry.binder, admit)) : 0;
        call({"Ljava/util/HashMap;",
              "put",
              {"Ljava/lang/Object;", "Ljava/lang/Object;"},
              "Ljava/lang/Object;"},
             false, cache, {name, binder});
      }
    }
    const char *properties[] = {"debug.servicemanager.slow_call_core_ms",
                                "debug.servicemanager.slow_call_ms",
                                "debug.servicemanager.log_calls_core",
                                "debug.servicemanager.log_calls"};
    const char *fields[] = {"GET_SERVICE_SLOW_THRESHOLD_US_CORE",
                            "GET_SERVICE_SLOW_THRESHOLD_US_NON_CORE",
                            "GET_SERVICE_LOG_EVERY_CALLS_CORE",
                            "GET_SERVICE_LOG_EVERY_CALLS_NON_CORE"};
    const uint32_t defaults[] = {10, 50, 100, 200};
    take(initialize("Landroid/os/SystemProperties;"));
    for (unsigned i = 0; i < 4; ++i) {
      tick(admit);
      const auto key = objectIdentity(heap.literal(properties[i]));
      const uint32_t value = call({"Landroid/os/SystemProperties;",
                                   "getInt",
                                   {"Ljava/lang/String;", "I"},
                                   "I"},
                                  true, 0, {key, defaults[i]});
      // Java multiplies two ints before widening to a signed long.
      field(fields[i], i < 2 ? "J" : "I") =
          i < 2 ? uint64_t(
                      int64_t(std::bit_cast<int32_t>(value * uint32_t(1000))))
                : uint64_t(value);
    }
    take(initialize("Lcom/android/internal/util/StatLogger;"));
    tick(admit);
    auto labels = heap.allocateArray("[Ljava/lang/String;", 1);
    objectIdentity(labels);
    tick(admit);
    labels->Elements[0] =
        heap.get(objectIdentity(heap.literal("getService()")));
    field("sStatLogger", "Lcom/android/internal/util/StatLogger;") =
        call({"Lcom/android/internal/util/StatLogger;",
              "<init>",
              {"[Ljava/lang/String;"},
              "V"},
             false, 0, {labels->Identity});
    if (input && input->manager_proxy) {
      manager = 0;
      if (*input->manager_proxy)
        manager = take(serviceProxy(
            heap, program,
            take(binderProxy(heap, **input->manager_proxy, admit)), admit));
    }
    service_manager_initialized = true;
    return {ExecutionStop::Returned};
  } catch (Stop &stop) {
    return std::move(stop.result);
  }
}

ExecutionResult
AndroidFramework::serviceManager(Heap &heap, const Program &program,
                                 const LibraryMethod &method, uint64_t receiver,
                                 std::span<const uint64_t> arguments,
                                 const AdmitInstruction &admit) {
  auto call = [&](const MethodRef &reference, uint64_t object,
                  std::initializer_list<uint64_t> args = {}) {
    return take(executeLibrary(heap, *libraryMethod(reference, false), object,
                               args, admit, false, &program, this));
  };
  auto field = [&](const char *name,
                   const char *type) -> std::optional<uint64_t> & {
    return service_fields.at(program.findField({Manager, name, type}, true));
  };
  try {
    if (method.Op == BinderAllowBlocking) {
      if (!arguments[0])
        return {ExecutionStop::Returned};
      auto entry = binder_warnings.find(arguments[0]);
      if (entry == binder_warnings.end())
        return unsupported(
            "Android allowBlocking requires a supplied BinderProxy identity");
      entry->second = false;
      return {ExecutionStop::Returned, arguments[0]};
    }
    if (method.Op == BinderQueryLocalInterface) {
      if (heap.get(receiver)->ClassName != "android/os/BinderProxy")
        return unsupported(
            "Android local Binder interface lookup is not modeled");
      // The Java BinderProxy method returns null without consulting native
      // state.
      return {ExecutionStop::Returned};
    }
    if (method.Op == ServiceAsBinder) {
      const auto object = heap.get(receiver);
      const auto *remote =
          program.findField({Proxy, "mRemote", "Landroid/os/IBinder;"}, false);
      if (object->ClassName != "android/os/ServiceManagerProxy" ||
          !object->Fields.contains(remote))
        return unsupported(
            "Android asBinder requires ServiceManagerProxy field state");
      return {ExecutionStop::Returned, object->Fields.at(remote)};
    }
    if (!service_manager_initialized)
      return unsupported(
          "Android ServiceManager initialization has not completed");
    const auto cache = *field("sCache", "Ljava/util/HashMap;");
    if (cache && !heap.get(cache)->MapEntries)
      return unsupported(
          "Android ServiceManager cache contents are not supplied");
    if (method.Op == ServiceInitCache) {
      if (call({"Ljava/util/HashMap;", "size", {}, "I"}, cache)) {
        tick(admit);
        auto exception = heap.allocate(Object::Throwable,
                                       "java/lang/IllegalStateException", 0);
        objectIdentity(exception);
        exception->Message = take(stringValue(
            heap, "setServiceCache may only be called once", admit));
        ExecutionResult result{ExecutionStop::Thrown, 0,
                               "java/lang/IllegalStateException",
                               "ServiceManager cache is already populated"};
        result.throwable = exception->Identity;
        return result;
      }
      call({"Ljava/util/HashMap;", "putAll", {"Ljava/util/Map;"}, "V"}, cache,
           {arguments[0]});
      return {ExecutionStop::Returned};
    }
    const auto cached = call({"Ljava/util/HashMap;",
                              "get",
                              {"Ljava/lang/Object;"},
                              "Ljava/lang/Object;"},
                             cache, {arguments[0]});
    if (cached) {
      if (!program.assignable(objectDescriptor(*heap.get(cached)),
                              "Landroid/os/IBinder;"))
        return {ExecutionStop::Thrown, 0, "java/lang/ClassCastException",
                "ServiceManager cache value is not an IBinder"};
      return {ExecutionStop::Returned, cached};
    }
    uint64_t start = 0;
    const auto logger =
        *field("sStatLogger", "Lcom/android/internal/util/StatLogger;");
    if (method.Op == ServiceGet)
      start =
          call({"Lcom/android/internal/util/StatLogger;", "getTime", {}, "J"},
               logger);
    auto &manager = field("sServiceManager", "Landroid/os/IServiceManager;");
    if (!manager)
      return unsupported("Android ServiceManager proxy state is not supplied");
    if (!*manager) {
      const auto *input = snapshot && snapshot->service_manager
                              ? &*snapshot->service_manager
                              : nullptr;
      if (!input || !input->context_object)
        return unsupported("Android Binder context object is not supplied");
      if (!*input->context_object)
        return {ExecutionStop::Thrown, 0, "java/lang/NullPointerException",
                "ServiceManager has a null context interface"};
      const auto binder =
          take(binderProxy(heap, **input->context_object, admit));
      const uint64_t args[] = {binder};
      take(executeLibrary(heap,
                          *libraryMethod({"Landroid/os/Binder;",
                                          "allowBlocking",
                                          {"Landroid/os/IBinder;"},
                                          "Landroid/os/IBinder;"},
                                         true),
                          0, args, admit, false, &program, this));
      manager = take(serviceProxy(heap, program, binder, admit));
    }
    const auto proxy = heap.get(*manager);
    const auto *remoteField =
        program.findField({Proxy, "mRemote", "Landroid/os/IBinder;"}, false);
    if (proxy->ClassName != "android/os/ServiceManagerProxy" ||
        !proxy->Fields.contains(remoteField))
      return unsupported(
          "Android service lookup requires ServiceManagerProxy state");
    const uint64_t remote = proxy->Fields.at(remoteField);
    const auto identity =
        std::find_if(binder_identities.begin(), binder_identities.end(),
                     [&](const auto &entry) { return entry.second == remote; });
    if (identity == binder_identities.end())
      return unsupported(
          "Android service lookup Binder identity is not supplied");
    using Query = AndroidApplicationSnapshot::ServiceManager::LookupResult;
    const Query *matched = nullptr;
    if (snapshot && snapshot->service_manager) {
      const auto name = heap.get(arguments[0]);
      for (const auto &query : snapshot->service_manager->lookup_results) {
        tick(admit);
        if (query.manager == identity->first &&
            query.operation == (method.Op == ServiceGet
                                    ? Query::Operation::Get
                                    : Query::Operation::Check) &&
            (name ? query.name && javaStringBytes(*query.name) == name->Bytes
                  : !query.name)) {
          matched = &query;
          break;
        }
      }
    }
    if (!matched)
      return unsupported(
          "Android ServiceManager Binder transaction result is not supplied");
    const uint64_t binder =
        matched->binder ? take(binderProxy(heap, *matched->binder, admit)) : 0;
    if (method.Op == ServiceGet) {
      const uint32_t duration = call({"Lcom/android/internal/util/StatLogger;",
                                      "logDurationStat",
                                      {"I", "J"},
                                      "J"},
                                     logger, {0, start});
      if (!services.process_uid)
        return unsupported("Android process UID service is not supplied");
      tick(admit);
      const auto uid = take(services.process_uid());
      if (uid > UINT32_MAX)
        return unsupported("Android process UID exceeds its Java int carrier");
      const bool core =
          std::bit_cast<int32_t>(uint32_t(uid)) >= 0 && uid % 100000 < 10000;
      const auto signedLong = [](uint64_t value) {
        return std::bit_cast<int64_t>(value);
      };
      const int64_t time = std::bit_cast<int32_t>(duration);
      const int64_t threshold =
          signedLong(*field(core ? "GET_SERVICE_SLOW_THRESHOLD_US_CORE"
                                 : "GET_SERVICE_SLOW_THRESHOLD_US_NON_CORE",
                            "J"));
      if (!*field("sLock", "Ljava/lang/Object;"))
        return {ExecutionStop::Thrown, 0, "java/lang/NullPointerException",
                "null Android service statistics lock"};
      tick(admit);
      auto &total = *field("sGetServiceAccumulatedUs", "I");
      total = uint32_t(uint32_t(total) + duration);
      auto &count = *field("sGetServiceAccumulatedCallCount", "I");
      count = uint32_t(uint32_t(count) + 1);
      const auto now = take(readClock(UptimeMillis));
      const auto interval = std::bit_cast<int32_t>(
          uint32_t(*field(core ? "GET_SERVICE_LOG_EVERY_CALLS_CORE"
                               : "GET_SERVICE_LOG_EVERY_CALLS_NON_CORE",
                          "I")));
      auto writeLog = [&](uint32_t tag, std::initializer_list<int32_t> values,
                          std::optional<uint64_t> name = {}) {
        AndroidEventLog log(tag);
        for (int32_t value : values) {
          tick(admit);
          log.scalar(0, uint32_t(value));
        }
        if (name)
          take(log.string(heap.get(*name).get(), admit));
        take(log.write(services, admit));
      };
      if (time >= threshold &&
          (signedLong(now) >
               signedLong(*field("sLastSlowLogUptime", "J") + 5000) ||
           signedLong(*field("sLastSlowLogActualTime", "J")) < time)) {
        writeLog(230001, {int32_t(time / 1000)}, arguments[0]);
        *field("sLastSlowLogUptime", "J") = now;
        *field("sLastSlowLogActualTime", "J") = uint64_t(time);
      }
      if (std::bit_cast<int32_t>(uint32_t(count)) >= interval &&
          signedLong(now) >=
              signedLong(*field("sLastStatsLogUptime", "J") + 5000)) {
        writeLog(230000, {std::bit_cast<int32_t>(uint32_t(count)),
                          std::bit_cast<int32_t>(uint32_t(total)) / 1000,
                          std::bit_cast<int32_t>(uint32_t(
                              now - *field("sLastStatsLogUptime", "J")))});
        count = 0;
        total = 0;
        *field("sLastStatsLogUptime", "J") = now;
      }
    }
    const uint64_t args[] = {binder};
    return executeLibrary(heap,
                          *libraryMethod({"Landroid/os/Binder;",
                                          "allowBlocking",
                                          {"Landroid/os/IBinder;"},
                                          "Landroid/os/IBinder;"},
                                         true),
                          0, args, admit, false, &program, this);
  } catch (Stop &stop) {
    return std::move(stop.result);
  }
}
} // namespace neverd::mobile::dalvik
