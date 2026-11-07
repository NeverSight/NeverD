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
namespace {
ExecutionResult javaCall(MethodRef method, uint64_t receiver,
                         std::span<const uint64_t> arguments) {
  ExecutionResult result{ExecutionStop::Suspended};
  auto &call = result.external.emplace();
  call.kind = ExternalCall::JavaMethod;
  call.java_method = std::move(method);
  call.arguments.push_back(receiver);
  call.arguments.insert(call.arguments.end(), arguments.begin(),
                        arguments.end());
  return result;
}
} // namespace

ExecutionResult AndroidFramework::binderInterface(
    Heap &heap, const Program &program, const LibraryMethod &method,
    uint64_t receiver, std::span<const uint64_t> arguments,
    const AdmitInstruction &admit) {
  if (method.Op == BinderOnTransact) {
    const uint32_t code = arguments[0];
    if (code == 0x5f444d50 || code == 0x5f434d44)
      return {
          ExecutionStop::Unsupported,
          0,
          {},
          "Android Binder dump/shell transactions require file descriptors"};
    if (code != 0x5f4e5446)
      return {};
    auto result = javaCall({"Landroid/os/Binder;",
                            "getInterfaceDescriptor",
                            {},
                            "Ljava/lang/String;"},
                           receiver, {});
    result.external->complete = [this, &heap, &program, admit,
                                 reply = arguments[2]](ExecutionResult value) {
      if (value.stop != ExecutionStop::Returned)
        return value;
      const auto *write = libraryMethod(
          {"Landroid/os/Parcel;", "writeString", {"Ljava/lang/String;"}, "V"},
          false);
      const uint64_t descriptor[] = {value.value};
      auto written = executeLibrary(heap, *write, reply, descriptor, admit,
                                    false, &program, this);
      if (written.stop == ExecutionStop::Returned)
        written.value = 1;
      return written;
    };
    return result;
  }
  const auto object = heap.get(receiver);
  const auto *descriptor = program.findField(
      {"Landroid/os/Binder;", "mDescriptor", "Ljava/lang/String;"}, false);
  const auto *owner = program.findField(
      {"Landroid/os/Binder;", "mOwner", "Landroid/os/IInterface;"}, false);
  if (!object->Fields.contains(descriptor) || !object->Fields.contains(owner))
    return {ExecutionStop::Unsupported,
            0,
            {},
            "Android Binder interface field state is not supplied"};
  const std::string_view name = method.Name;
  if (name == "attachInterface") {
    object->Fields.at(owner) = arguments[0];
    object->Fields.at(descriptor) = arguments[1];
    return {};
  }
  if (name == "getInterfaceDescriptor")
    return {ExecutionStop::Returned, object->Fields.at(descriptor)};
  const auto expected = heap.get(object->Fields.at(descriptor));
  if (!expected)
    return {ExecutionStop::Thrown, 0, "java/lang/NullPointerException"};
  const auto actual = heap.get(arguments[0]);
  return {ExecutionStop::Returned, actual && actual->Bytes == expected->Bytes
                                       ? object->Fields.at(owner)
                                       : 0};
}

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
  if (heap.get(receiver)->ClassName != "android/os/BinderProxy") {
    if (!program.assignable(objectDescriptor(*heap.get(receiver)),
                            "Landroid/os/Binder;"))
      return {ExecutionStop::Unsupported,
              0,
              {},
              "Android custom IBinder transact body is not supplied"};
    const auto *seek = libraryMethod(
        {"Landroid/os/Parcel;", "setDataPosition", {"I"}, "V"}, false);
    const uint64_t zero[] = {0};
    if (arguments[1]) {
      auto result = executeLibrary(heap, *seek, arguments[1], zero, admit,
                                   false, &program, this);
      if (result.stop != ExecutionStop::Returned)
        return result;
    }
    auto result =
        javaCall({"Landroid/os/Binder;",
                  "onTransact",
                  {"I", "Landroid/os/Parcel;", "Landroid/os/Parcel;", "I"},
                  "Z"},
                 receiver, arguments);
    result.external->complete = [this, &heap, &program, admit, seek,
                                 reply = arguments[2]](ExecutionResult value) {
      // The Java wrapper has no finally: an exception retains the reply cursor.
      if (value.stop == ExecutionStop::Returned && reply) {
        const uint64_t zero[] = {0};
        auto result = executeLibrary(heap, *seek, reply, zero, admit, false,
                                     &program, this);
        if (result.stop != ExecutionStop::Returned)
          return result;
      }
      return value;
    };
    return result;
  }
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
  const auto &source = parcelState(arguments[1]);
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
                                         parcelState(arguments[1]).objects};
  if (!binding->second.alive)
    return {ExecutionStop::Thrown, 0, "android/os/DeadObjectException"};
  if (parcelState(arguments[1]).error)
    return completeBinder(heap, call, {parcelState(arguments[1]).error});
  auto read = readParcelBuffer(heap, arguments[1]);
  if (read.stop != ExecutionStop::Returned)
    return read;
  if (parcelState(arguments[1]).position >
      parcelStorage(heap, arguments[1]).Bytes.size())
    return {ExecutionStop::Unsupported,
            0,
            {},
            "Android Binder data requires uninitialized Parcel bytes"};
  call.binder->bytes = parcelStorage(heap, arguments[1]).Bytes;
  ExecutionResult result;
  result.stop = ExecutionStop::Suspended;
  result.external = std::move(call);
  return result;
}
ExecutionResult AndroidFramework::replaceParcel(Heap &heap, ParcelState &state,
                                                Object &object,
                                                BinderReply reply) {
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
    if (!heap.resizeBytes(object, reply.bytes->size()))
      return {ExecutionStop::HeapLimit,
              0,
              {},
              "Android Binder reply storage budget exhausted"};
    auto released = releaseParcelObjects(state);
    if (released.stop != ExecutionStop::Returned)
      return released;
    if (state.release)
      state.release();
    object.Bytes = std::move(*reply.bytes);
    state.position = 0;
    state.capacity = object.Bytes.size();
    state.error = 0;
    state.view = std::move(reply.view);
    state.release = std::move(reply.release);
    state.write = std::move(reply.write);
    state.reallocate_objects = std::move(reply.reallocate_objects);
    state.objects = std::move(reply.objects);
    state.fd = reply.fd;
  }
  if (reply.status)
    state.error = reply.status;
  return {};
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
    const auto status = reply.status;
    auto replaced =
        replaceParcel(heap, parcelState(destination),
                      parcelStorage(heap, destination), std::move(reply));
    if (replaced.stop != ExecutionStop::Returned)
      return replaced;
    reply.status = status;
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
