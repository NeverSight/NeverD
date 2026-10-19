//===- Runtime.h - Bounded execution of decoded Dalvik methods --*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Bounded method and object execution; decoding remains owned by parseDex.
//===----------------------------------------------------------------------===//
#pragma once

#include "neverd/dalvik/AndroidApplication.h"
#include "neverd/dalvik/Heap.h"

#include <functional>
#include <memory>
#include <set>
#include <span>

namespace neverd::mobile::dalvik {
struct LibraryMethod;
enum class ExecutionStop {
  Returned,
  Thrown,
  Unsupported,
  Invalid,
  InstructionLimit,
  Timeout,
  StackLimit,
  HeapLimit
};
struct ExecutionResult {
  ExecutionStop stop = ExecutionStop::Returned;
  uint64_t value = 0;
  std::string exception;
  std::string diagnostic;
  uint64_t throwable =
      0; // Shared heap identity, retained across catch/rethrow/JNI.
};
// Return Returned to admit an instruction, or the resource stop to propagate.
// The owner supplies one workload-wide instruction/deadline budget.
using AdmitInstruction = std::function<ExecutionStop()>;
class Program {
public:
  // Input declarations must come from the shared validated DEX reader.
  // Android opts into API 28 Build.VERSION fields and the finite framework
  // hierarchy. Runtime supplies the separate optional application snapshot.
  // This is not framework <clinit> execution or permission to replace platform
  // declarations with DEX.
  explicit Program(std::vector<Class> declarations,
                   std::optional<std::map<std::string, std::string>>
                       android_properties = std::nullopt);
  Program(const Program &) = delete;
  Program &operator=(const Program &) = delete;
  bool containsClass(std::string_view descriptor) const;
  const Class *findClass(std::string_view descriptor) const;
  bool knownReference(std::string_view descriptor) const;
  std::string parent(std::string_view descriptor) const;
  bool assignable(std::string_view source, std::string_view destination) const;
  const Method *virtualMethod(const MethodRef &reference,
                              std::string_view receiver) const;
  const Field *findField(const FieldRef &reference, bool is_static) const;
  /// With no staticness, search only the class/superclass chain. This is used
  /// by native registration, whose declarations may be static or virtual.
  const Method *findMethod(const MethodRef &reference,
                           std::optional<bool> is_static) const;
  const Field *findStaticField(const FieldRef &reference) const;
  std::optional<std::string>
  unsupportedClass(std::string_view descriptor) const;
  const Method *findStatic(const MethodRef &reference) const;
  // Supplied integral/reference code and a validated finite hierarchy.
  // Unknown library bodies, synchronization and floating values are not
  // inferred.
  std::optional<std::string> unsupported(const Method &method) const;
  // One-shot convenience. Use Runtime to retain class state across calls.
  ExecutionResult execute(const Method &method, std::span<const uint64_t> args,
                          const AdmitInstruction &admit,
                          uint64_t stack_bytes) const;

  struct PlatformValue {
    Literal value;
    bool intern_string = false;
  };
  const PlatformValue *platformValue(const Field &field) const;
  // Identity-based ownership, independent of source/debug metadata supplied
  // by a DEX input. These values live in the shared framework projection.
  bool androidFrameworkField(const Field &field) const;

private:
  void addAndroidVersion(const std::map<std::string, std::string> &properties);
  void addAndroidFramework();
  ClassMap classes;
  std::map<FieldRef, PlatformValue> platform_values;
  std::set<const Field *> android_framework_fields;
};

// One workload's mutable class and object state over an immutable Program,
// which must outlive it. Execution is synchronous and single-owner; no Java
// scheduling or monitor/memory-model equivalence is implied. A non-Java stop
// poisons the runtime: partially executed code cannot be retried as a fresh
// invocation.
class Runtime {
public:
  explicit Runtime(const Program &program, std::shared_ptr<Heap> heap = {},
                   std::optional<AndroidApplicationSnapshot> application = {});
  ~Runtime();
  Runtime(const Runtime &) = delete;
  Runtime &operator=(const Runtime &) = delete;
  ExecutionResult allocate(std::string_view descriptor, bool initialized,
                           const AdmitInstruction &admit, uint64_t stack_bytes);
  ExecutionResult field(const Field &field, uint64_t receiver,
                        std::optional<uint64_t> write,
                        const AdmitInstruction &admit, uint64_t stack_bytes);
  ExecutionResult initialize(std::string_view descriptor,
                             const AdmitInstruction &admit,
                             uint64_t stack_bytes);
  ExecutionResult execute(const Method &method, std::span<const uint64_t> args,
                          const AdmitInstruction &admit, uint64_t stack_bytes);
  // JNI and DEX share library semantics and workload-owned framework objects.
  ExecutionResult library(const LibraryMethod &method, uint64_t receiver,
                          std::span<const uint64_t> args,
                          const AdmitInstruction &admit,
                          bool nonvirtual = false);

private:
  struct State;
  std::unique_ptr<State> state;
};
} // namespace neverd::mobile::dalvik
