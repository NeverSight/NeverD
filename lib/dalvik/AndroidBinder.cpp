//===- AndroidBinder.cpp - Shared API 28 Binder transaction entry --------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Ordered Java wrapper and native Parcel checks before Binder transport.
//===----------------------------------------------------------------------===//

#include "AndroidFramework.h"

#include <cstdint>
#include <span>

namespace neverd::mobile::dalvik {
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
  ExternalCall call;
  call.kind = ExternalCall::BinderTransaction;
  // Native adapter sees shared identities and the explicit fd/handle only.
  call.binder = BinderTransactionRequest{receiver,
                                         arguments[1],
                                         arguments[2],
                                         uint32_t(arguments[0]),
                                         uint32_t(arguments[3]),
                                         binding->second.fd,
                                         binding->second.handle};
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
      auto object = heap.get(destination);
      if (!heap.resizeBytes(*object, reply.bytes->size()))
        return {ExecutionStop::HeapLimit,
                0,
                {},
                "Android Binder reply storage budget exhausted"};
      if (state.release)
        state.release();
      object->Bytes = std::move(*reply.bytes);
      state.position = 0;
      state.error = 0;
      state.view = std::move(reply.view);
      state.release = std::move(reply.release);
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
