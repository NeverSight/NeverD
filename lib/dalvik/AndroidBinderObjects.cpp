//===- AndroidBinderObjects.cpp - JavaBBinder identity and references -----===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// API 28 holder weak caches, owned Parcel references and driver references.
//===----------------------------------------------------------------------===//
#include "AndroidFramework.h"

#include "neverd/dalvik/Limits.h"

namespace neverd::mobile::dalvik {
namespace {
ExecutionResult unsupported(const char *message) {
  return {ExecutionStop::Unsupported, 0, {}, message};
}
} // namespace

ExecutionResult
AndroidFramework::constructBinder(Heap &heap, const Program &program,
                                  uint64_t receiver,
                                  const AdmitInstruction &admit) {
  const auto *field =
      program.findField({"Landroid/os/Binder;", "mObject", "J"}, false);
  auto object = heap.get(receiver);
  if (object &&
      (object->Initialized ||
       !program.assignable(objectDescriptor(*object), "Landroid/os/Binder;") ||
       !object->Fields.contains(field)))
    return {ExecutionStop::Invalid,
            0,
            {},
            "Android Binder constructor requires uninitialized field storage"};
  if (!services.binder_identity)
    return unsupported(
        "Android Binder native identity service is not supplied");
  const auto stop = admit();
  if (stop != ExecutionStop::Returned)
    return {stop};
  if (!object) {
    object =
        heap.allocate(Object::Instance, "android/os/Binder", 0,
                      ::neverd::limits::dalvik::kInstanceFieldStorageBytes);
    if (!object)
      return {ExecutionStop::HeapLimit,
              0,
              {},
              "Android Binder holder storage budget exhausted"};
    object->Fields.emplace(field, 0);
  }
  auto identity =
      services.binder_identity(BinderIdentityOperation::Allocate, 0);
  if (identity.stop != ExecutionStop::Returned)
    return identity;
  object->Fields.at(field) = identity.value;
  binder_holders.try_emplace(object->Identity, BinderHolder{identity.value});
  return {ExecutionStop::Returned, object->Identity};
}

ExecutionResult AndroidFramework::localBinder(Heap &heap,
                                              const Program &program,
                                              uint64_t receiver) {
  auto holder = binder_holders.find(receiver);
  const auto *field =
      program.findField({"Landroid/os/Binder;", "mObject", "J"}, false);
  const auto object = heap.get(receiver);
  if (holder == binder_holders.end() || !object->Fields.contains(field) ||
      object->Fields.at(field) != holder->second.address)
    return unsupported(
        "Android local Binder requires its original native holder");
  auto valid = services.binder_identity(BinderIdentityOperation::Validate,
                                        holder->second.address);
  if (valid.stop != ExecutionStop::Returned)
    return valid;
  const auto old = holder->second.cookie;
  if (old) {
    const auto &node = local_binders.find(old)->second;
    if (node.alive()) {
      valid = services.binder_identity(BinderIdentityOperation::Validate,
                                       node.pointer);
      return valid.stop == ExecutionStop::Returned
                 ? ExecutionResult{ExecutionStop::Returned, old}
                 : valid;
    }
  }
  auto identity =
      services.binder_identity(BinderIdentityOperation::Allocate, 0);
  if (identity.stop != ExecutionStop::Returned)
    return identity;
  // Placement is deliberately opaque. These two identities are weakref_type
  // and JavaBBinder respectively, not a guest C++ object layout.
  const uint64_t cookie = identity.value + sizeof(uint64_t);
  local_binders.try_emplace(cookie,
                            LocalBinder{uint32_t(receiver), identity.value});
  holder->second.cookie = cookie;
  if (old) {
    auto retired = retireLocalBinder(old);
    if (retired.stop != ExecutionStop::Returned)
      return retired;
  }
  return {ExecutionStop::Returned, cookie};
}

ExecutionResult AndroidFramework::retireLocalBinder(uint64_t cookie) {
  auto node = local_binders.find(cookie);
  const auto &value = node->second;
  // A JavaBBinderHolder retains its last weak cache even after the native
  // strong object dies. A later get() replaces that cache with a new object.
  if (value.alive() || value.weak ||
      binder_holders.find(value.object)->second.cookie == cookie)
    return {};
  auto released =
      services.binder_identity(BinderIdentityOperation::Release, value.pointer);
  if (released.stop == ExecutionStop::Returned)
    local_binders.erase(node);
  return released;
}

ExecutionResult AndroidFramework::releaseParcelObjects(ParcelState &state) {
  while (!state.local_objects.empty()) {
    const auto cookie = state.local_objects.back();
    --local_binders.find(cookie)->second.references;
    state.local_objects.pop_back();
    auto released = retireLocalBinder(cookie);
    if (released.stop != ExecutionStop::Returned)
      return released;
  }
  return {};
}

ExecutionResult
AndroidFramework::binderReference(BinderReferenceOperation operation,
                                  uint64_t pointer, uint64_t cookie) {
  auto node = local_binders.find(cookie);
  if (node == local_binders.end() || node->second.pointer != pointer)
    return unsupported(
        "Android Binder reference notification has no local Java association");
  auto valid =
      services.binder_identity(BinderIdentityOperation::Validate, pointer);
  if (valid.stop != ExecutionStop::Returned)
    return valid;
  auto &value = node->second;
  switch (operation) {
  case BinderReferenceOperation::AcquireWeak:
    ++value.weak;
    break;
  case BinderReferenceOperation::AcquireStrong:
    if (!value.alive())
      return unsupported(
          "Android Binder strong acquisition requires a live local object");
    ++value.strong;
    break;
  case BinderReferenceOperation::DeferStrongRelease:
    if (value.strong == value.pending_strong)
      return unsupported("Android Binder strong reference is not owned");
    ++value.pending_strong;
    break;
  case BinderReferenceOperation::DeferWeakRelease:
    if (value.weak == value.pending_weak)
      return unsupported("Android Binder weak reference is not owned");
    ++value.pending_weak;
    break;
  }
  return retireLocalBinder(cookie);
}
} // namespace neverd::mobile::dalvik
