//===- AndroidBinder.cpp - Shared API 28 Binder transaction entry --------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Ordered Java wrapper and native Parcel checks before Binder transport.
//===----------------------------------------------------------------------===//

#include "AndroidFramework.h"

#include "neverd/dalvik/Limits.h"

#include <cstdint>
#include <span>

namespace neverd::mobile::dalvik {
ExecutionResult AndroidFramework::binderHandle(Heap &heap, uint32_t fd,
                                               uint32_t handle,
                                               const AdmitInstruction &admit) {
  const auto endpoint = std::make_pair(fd, handle);
  if (auto found = binder_handles.find(endpoint); found != binder_handles.end())
    return {ExecutionStop::Returned, found->second};
  // An explicitly supplied proxy already denotes a live ProcessState cache
  // entry, including handle zero. Do not replace its declared warning state.
  if (auto found = binder_input_keys.find(endpoint);
      found != binder_input_keys.end())
    return binderProxy(heap, found->second, admit);
  if (!handle)
    return {ExecutionStop::Unsupported,
            0,
            {},
            "Android uncached Binder handle zero requires a native ping"};
  if (!services.retain_binder_handle)
    return {ExecutionStop::Unsupported,
            0,
            {},
            "Android Binder handle ownership service is not supplied"};
  const auto supplied = snapshot && snapshot->service_manager
                            ? snapshot->service_manager->binder_proxies.size()
                            : 0;
  if (binder_handles.size() + supplied >=
      ::neverd::limits::dalvik::kMaxAndroidBinderProxies)
    return {ExecutionStop::HeapLimit,
            0,
            {},
            "Android Binder proxy budget exhausted"};
  auto stop = admit();
  if (stop != ExecutionStop::Returned)
    return {stop};
  auto object = heap.allocate(
      Object::Instance, "android/os/BinderProxy", 0,
      ::neverd::limits::dalvik::kFrameworkFieldStateOverheadBytes);
  if (!object)
    return {ExecutionStop::HeapLimit,
            0,
            {},
            "Android Binder proxy storage budget exhausted"};
  auto retained = retainBinderHandle(fd, handle);
  if (retained.stop != ExecutionStop::Returned)
    return retained;
  binder_handles.emplace(endpoint, object->Identity);
  binder_warnings.emplace(object->Identity, false); // API 28 sWarnOnBlocking.
  binder_bindings.emplace(
      object->Identity,
      AndroidApplicationSnapshot::ServiceManager::BinderProxy::NativeBinding{
          fd, handle, true});
  return {ExecutionStop::Returned, object->Identity};
}

ExecutionResult
AndroidFramework::binderTransact(Heap &heap, const Program &program,
                                 const LibraryMethod &method, uint64_t receiver,
                                 std::span<const uint64_t> arguments,
                                 const AdmitInstruction &admit) {
  // Local Binder.transact rewinds Parcels and dispatches onTransact. It must
  // not acquire BinderProxy semantics merely through the IBinder interface.
  if (heap.get(receiver)->ClassName != "android/os/BinderProxy")
    return {ExecutionStop::Unsupported,
            0,
            {},
            "Android local Binder onTransact dispatch is not modeled"};
  if (method.Op == BinderTransact) {
    // API 28 CHECK_PARCEL_SIZE is false. There is no size read or null check
    // before the warning/tracing wrapper. FLAG_ONEWAY suppresses the warning
    // independently of the otherwise unknown mWarnOnBlocking value.
    if (!(uint32_t(arguments[3]) & 1)) {
      const auto *warning = program.findField(
          {"Landroid/os/BinderProxy;", "mWarnOnBlocking", "Z"}, false);
      auto enabled = field(heap, *warning, receiver, {}, admit);
      if (enabled.stop != ExecutionStop::Returned)
        return enabled;
      if (enabled.value) {
        field(heap, *warning, receiver, 0, admit);
        return {ExecutionStop::Unsupported,
                0,
                {},
                "Android Binder blocking warning requires Log.w and a Java "
                "stack trace"};
      }
    }
    if (binder_tracing_enabled)
      return {ExecutionStop::Unsupported,
              0,
              {},
              "Android Binder transaction tracing requires a Java stack trace"};
  }
  if (!arguments[1])
    return {ExecutionStop::Thrown, 0, "java/lang/NullPointerException"};
  auto data = parcelStatus(arguments[1]);
  if (data.stop != ExecutionStop::Returned)
    return data;
  if (arguments[2]) {
    auto reply = parcelStatus(arguments[2]);
    if (reply.stop != ExecutionStop::Returned)
      return reply;
  }
  if (!binder_warnings.contains(receiver))
    return {ExecutionStop::Unsupported,
            0,
            {},
            "Android BinderProxy native object is not supplied"};
  const auto binding = binder_bindings.find(receiver);
  if (binding == binder_bindings.end())
    return {ExecutionStop::Unsupported,
            0,
            {},
            "Android BinderProxy.transactNative requires an explicit native "
            "Binder binding"};
  const auto &source = parcels.at(arguments[1]);
  if (!source.objects.empty() && source.fd && source.fd != binding->second.fd)
    return {
        ExecutionStop::Unsupported,
        0,
        {},
        "Android Binder data objects belong to a different native connection"};
  ExternalCall call;
  call.kind = ExternalCall::BinderTransaction;
  // Native adapter sees shared identities and the explicit fd/handle only.
  call.binder = BinderTransactionRequest{receiver,
                                         arguments[1],
                                         arguments[2],
                                         uint32_t(arguments[0]),
                                         uint32_t(arguments[3]),
                                         binding->second.fd,
                                         binding->second.handle,
                                         parcels.at(arguments[1]).objects};
  if (!binding->second.alive)
    return {ExecutionStop::Thrown, 0, "android/os/DeadObjectException"};
  if (parcels.at(arguments[1]).error)
    return completeBinder(heap, call, {parcels.at(arguments[1]).error});
  auto read = readParcelBuffer(heap, arguments[1]);
  if (read.stop != ExecutionStop::Returned)
    return read;
  if (parcels.at(arguments[1]).position > heap.get(arguments[1])->Bytes.size())
    return {ExecutionStop::Unsupported,
            0,
            {},
            "Android Binder data requires uninitialized Parcel bytes"};
  ExecutionResult result;
  result.stop = ExecutionStop::Suspended;
  result.external = std::move(call);
  return result;
}
ExecutionResult AndroidFramework::completeBinder(Heap &heap,
                                                 const ExternalCall &call,
                                                 BinderReply reply) {
  const auto receiver = call.binder->receiver, destination = call.binder->reply;
  if (reply.status == -32)
    binder_bindings.at(receiver).alive = false;
  if (destination) {
    auto &state = parcels.at(destination);
    if (state.recycled)
      return {ExecutionStop::Unsupported,
              0,
              {},
              "Android Binder reply Parcel was recycled during transaction"};
    if (reply.bytes) {
      uint64_t minimum = 0;
      for (auto offset : reply.objects) {
        if (offset < minimum || offset % 4 || offset > reply.bytes->size() ||
            reply.bytes->size() - offset < 24)
          return {ExecutionStop::Invalid,
                  0,
                  {},
                  "Android Binder reply object table is malformed"};
        minimum = offset + 24;
      }
      auto object = heap.get(destination);
      if (!heap.resizeBytes(*object, reply.bytes->size()))
        return {ExecutionStop::HeapLimit,
                0,
                {},
                "Android Binder reply storage budget exhausted"};
      auto released = releaseParcelObjects(state);
      if (released.stop != ExecutionStop::Returned)
        return released;
      if (state.release)
        state.release();
      object->Bytes = std::move(*reply.bytes);
      state.position = 0;
      state.capacity = object->Bytes.size();
      state.error = 0;
      state.view = std::move(reply.view);
      state.release = std::move(reply.release);
      state.write = std::move(reply.write);
      state.objects = std::move(reply.objects);
      state.fd = reply.fd;
    }
    if (reply.status)
      state.error = reply.status;
  } else if (reply.release)
    reply.release(); // IPCThreadState's temporary reply queues BC_FREE_BUFFER.
  switch (reply.status) {
  case 0:
    return {ExecutionStop::Returned, 1};
  case -74: // UNKNOWN_TRANSACTION is the JNI boolean-false return.
    return {};
  case -32:
    return {ExecutionStop::Thrown, 0, "android/os/DeadObjectException"};
  case -22:
  case INT32_MIN + 1: // BAD_TYPE
    return {ExecutionStop::Thrown, 0, "java/lang/IllegalArgumentException"};
  case -12:
    return {ExecutionStop::Thrown, 0, "java/lang/OutOfMemoryError"};
  default:
    return {ExecutionStop::Unsupported,
            0,
            {},
            "Android Binder native status exception/logging is not modeled: " +
                std::to_string(reply.status)};
  }
}
} // namespace neverd::mobile::dalvik
