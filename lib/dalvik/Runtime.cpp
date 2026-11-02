//===- Runtime.cpp - Integral Dalvik method execution ---------------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Explicit, bounded value semantics over the shared DEX decoder's model.
//===----------------------------------------------------------------------===//
#include "neverd/dalvik/Runtime.h"

#include "AndroidFramework.h"
#include "ExecutionTask.h"

#include "neverd/dalvik/Library.h"
#include "neverd/dalvik/Limits.h"

#include <bit>
#include <limits>
#include <type_traits>

namespace neverd::mobile::dalvik {
namespace {
bool integral(std::string_view type) {
  return type.size() == 1 &&
         std::string_view("ZBCSIJ").find(type[0]) != std::string_view::npos;
}
uint64_t narrow(uint64_t value, std::string_view type) {
  if (type == "J" || referenceType(type))
    return value;
  if (type == "B")
    return uint32_t(int32_t(std::bit_cast<int8_t>(uint8_t(value))));
  if (type == "S")
    return uint32_t(int32_t(std::bit_cast<int16_t>(uint16_t(value))));
  if (type == "C")
    return uint16_t(value);
  if (type == "Z")
    return uint8_t(value);
  return uint32_t(value);
}
struct Halt {
  ExecutionResult result;
};
[[noreturn]] void halt(ExecutionStop stop, const Method &method, uint32_t pc,
                       std::string message, std::string exception = {}) {
  throw Halt{{stop, 0, std::move(exception),
              method.reference.identity() + " at code-unit " +
                  std::to_string(pc) + ": " + message}};
}
struct Register {
  enum Kind { Empty, Zero, Int, Ref, WideLow, WideHigh } kind = Empty;
  uint32_t bits = 0;
};
struct Frame {
  const Method &method;
  uint32_t pc = 0, constructing = 0;
  bool base_initialized = false;
  std::vector<Register> registers;
  explicit Frame(const Method &method)
      : method(method), registers(method.registers) {}
  [[noreturn]] void invalid(const char *message) const {
    halt(ExecutionStop::Invalid, method, pc, message);
  }
  void range(unsigned index, bool wide) const {
    if (index >= registers.size() || (wide && index + 1 >= registers.size()))
      invalid("register outside frame");
  }
  uint64_t get(unsigned index, bool wide = false) const {
    range(index, wide);
    if ((registers[index].kind != (wide ? Register::WideLow : Register::Int) &&
         (wide || registers[index].kind != Register::Zero)) ||
        (wide && registers[index + 1].kind != Register::WideHigh))
      invalid("uninitialized or incompatible register value");
    return registers[index].bits |
           (wide ? uint64_t(registers[index + 1].bits) << 32 : 0);
  }
  uint32_t ref(unsigned index) const {
    range(index, false);
    if (registers[index].kind != Register::Ref &&
        registers[index].kind != Register::Zero)
      invalid("reference operand is not an object or null constant");
    return registers[index].bits;
  }
  void putRef(unsigned index, uint64_t bits) {
    if (bits > UINT32_MAX)
      invalid("object identity exceeds register width");
    put(index, bits);
    registers[index].kind = Register::Ref;
  }
  void erase(unsigned index) {
    if (registers[index].kind == Register::WideLow)
      registers[index + 1].kind = Register::Empty;
    if (registers[index].kind == Register::WideHigh)
      registers[index - 1].kind = Register::Empty;
    registers[index].kind = Register::Empty;
  }
  void put(unsigned index, uint64_t bits, bool wide = false) {
    range(index, wide);
    erase(index);
    if (wide)
      erase(index + 1);
    registers[index] = {wide ? Register::WideLow : Register::Int,
                        uint32_t(bits)};
    if (wide)
      registers[index + 1] = {Register::WideHigh, uint32_t(bits >> 32)};
  }
};
template <class U> U arithmeticShift(U value, unsigned shift) {
  shift &= sizeof(U) * 8 - 1;
  if (!shift)
    return value;
  U high =
      value >> (sizeof(U) * 8 - 1) ? U(~U(0)) << (sizeof(U) * 8 - shift) : 0;
  return (value >> shift) | high;
}
template <class U>
U binary(std::string_view op, U left, U right, const Frame &frame) {
  using S = std::make_signed_t<U>;
  unsigned shift = unsigned(right) & (sizeof(U) * 8 - 1);
  if (op == "add")
    return left + right;
  if (op == "sub")
    return left - right;
  if (op == "rsub")
    return right - left;
  if (op == "mul")
    return left * right;
  if (op == "and")
    return left & right;
  if (op == "or")
    return left | right;
  if (op == "xor")
    return left ^ right;
  if (op == "shl")
    return left << shift;
  if (op == "ushr")
    return left >> shift;
  if (op == "shr")
    return arithmeticShift(left, shift);
  if (op == "div" || op == "rem") {
    if (!right)
      halt(ExecutionStop::Thrown, frame.method, frame.pc, "division by zero",
           "java/lang/ArithmeticException");
    S a = std::bit_cast<S>(left), b = std::bit_cast<S>(right);
    if (a == std::numeric_limits<S>::min() && b == -1)
      return op == "div" ? left : 0;
    return op == "div" ? U(a / b) : U(a % b);
  }
  halt(ExecutionStop::Unsupported, frame.method, frame.pc,
       "unsupported integer operation " + std::string(op));
}
} // namespace

struct Runtime::State {
  const Program &program;
  std::shared_ptr<Heap> heap;
  AndroidFramework framework;
  struct ClassState {
    enum Status {
      Prepared,
      Initializing,
      Initialized,
      Erroneous
    } status = Prepared;
    uint64_t owner = 0;
    std::map<const Field *, uint64_t> fields;
  };
  std::map<const Class *, ClassState> classes;
  std::optional<ExecutionResult> stopped;
  struct Engine;
  std::map<uint64_t, std::unique_ptr<Engine>> executions;
  uint64_t next_execution = 1;
  ~State();
  ExecutionResult start(std::function<ExecutionTask(Engine &)> entry,
                        const AdmitInstruction &admit, uint64_t stack,
                        uint64_t owner);
  ExecutionResult advance(uint64_t id);
  void throwable(ExecutionResult &result) {
    if (result.stop != ExecutionStop::Thrown || result.throwable)
      return;
    auto value = heap->allocate(Object::Throwable, result.exception, 0);
    if (!value) {
      result.stop = ExecutionStop::HeapLimit;
      result.diagnostic = "Java throwable storage budget exhausted";
      result.exception.clear();
    } else
      result.throwable = value->Identity;
  }
  ExecutionResult invoke(const std::function<uint64_t()> &body) {
    if (stopped)
      return *stopped;
    try {
      return {ExecutionStop::Returned, body(), {}, {}};
    } catch (Halt &halt) {
      throwable(halt.result);
      if (halt.result.stop != ExecutionStop::Thrown)
        stopped = halt.result;
      return halt.result;
    }
  }
};

struct Runtime::State::Engine {
  State &state;
  const Program &program;
  AdmitInstruction admit;
  uint64_t remaining_stack, owner;
  std::function<ExecutionTask(Engine &)> entry;
  std::optional<ExecutionTask> task;
  std::coroutine_handle<> suspended;
  std::optional<ExternalCall> external;
  ExecutionResult completion;
  struct ExternalAwaiter {
    Engine &engine;
    bool await_ready() const { return false; }
    void await_suspend(std::coroutine_handle<> handle) {
      engine.suspended = handle;
    }
    uint64_t await_resume() {
      engine.external.reset();
      if (engine.completion.stop != ExecutionStop::Returned)
        throw Halt{std::move(engine.completion)};
      return engine.completion.value;
    }
  };
  ExternalAwaiter request(ExternalCall call) {
    if (!owner)
      fail(ExecutionStop::Unsupported,
           "external Java invocation requires a resumable owner");
    external = std::move(call);
    return {*this};
  }
  Engine(State &state, const AdmitInstruction &admit, uint64_t stack,
         uint64_t owner)
      : state(state), program(state.program), admit(admit),
        remaining_stack(stack), owner(owner) {
    if (!admit)
      throw Error("DEX execution requires an instruction budget");
  }
  [[noreturn]] void fail(ExecutionStop stop, std::string message,
                         std::string exception = {}) const {
    throw Halt{{stop, 0, std::move(exception), std::move(message)}};
  }
  void known(std::string_view type) const {
    if (referenceType(type) && !program.knownReference(type))
      fail(ExecutionStop::Unsupported,
           "unmodeled reference type " + std::string(type));
  }
  std::shared_ptr<Object> object(uint64_t id, const Frame *frame = nullptr,
                                 bool nullable = false) const {
    if (!id && nullable)
      return {};
    if (!id)
      fail(ExecutionStop::Thrown, "null object dereference",
           "java/lang/NullPointerException");
    auto value = state.heap->get(id);
    if (!value)
      fail(ExecutionStop::Invalid,
           "object identity does not belong to this workload");
    if (!value->Initialized &&
        !(frame && frame->constructing == id && frame->base_initialized))
      fail(ExecutionStop::Invalid,
           "uninitialized object use or constructor escape");
    return value;
  }
  void compatible(uint64_t id, std::string_view type,
                  const Frame *frame = nullptr) const {
    known(type);
    auto value = object(id, frame, true);
    if (value && !program.assignable(objectDescriptor(*value), type))
      fail(ExecutionStop::Invalid,
           "object value is incompatible with declared type");
  }
  ExecutionTask allocate(std::string_view name, bool initialized,
                         unsigned depth) {
    if (name == "Ljava/lang/String;" || name == "Ljava/lang/Object;" ||
        (!program.containsClass(name) && program.knownReference(name) &&
         program.assignable(name, "Ljava/lang/Throwable;"))) {
      auto value = state.heap->allocate(
          name == "Ljava/lang/String;"   ? Object::String
          : name == "Ljava/lang/Object;" ? Object::Instance
                                         : Object::Throwable,
          name.substr(1, name.size() - 2), 0);
      if (!value)
        fail(ExecutionStop::HeapLimit, "Java object storage budget exhausted");
      value->Initialized = initialized;
      co_return value->Identity;
    }
    co_await initialize(name, depth);
    const auto *klass = program.findClass(name);
    if (has(klass->access, "abstract") || has(klass->access, "interface"))
      fail(ExecutionStop::Unsupported, "abstract DEX class allocation");
    std::vector<const Field *> fields;
    for (auto base = klass; base;
         base = program.findClass(program.parent(base->name)))
      for (const auto &field : base->fields)
        if (!has(field.access, "static"))
          fields.push_back(&field);
    auto value = state.heap->allocate(
        Object::Instance, name.substr(1, name.size() - 2), 0,
        fields.size() * ::neverd::limits::dalvik::kInstanceFieldStorageBytes);
    if (!value)
      fail(ExecutionStop::HeapLimit, "Java object storage budget exhausted");
    value->Initialized = initialized;
    for (auto field : fields)
      value->Fields.emplace(field, 0);
    co_return value->Identity;
  }
  std::shared_ptr<Object> array(std::string_view type, int32_t length) {
    known(type);
    if (!type.starts_with('['))
      fail(ExecutionStop::Invalid, "array allocation requires an array type");
    if (length < 0)
      fail(ExecutionStop::Thrown, "negative DEX array length",
           "java/lang/NegativeArraySizeException");
    const auto component = type.substr(1);
    bool refs = referenceType(component);
    if (!refs && !integral(component))
      fail(ExecutionStop::Unsupported,
           "floating array execution is unsupported");
    auto value = state.heap->allocateArray(type, length);
    if (!value)
      fail(ExecutionStop::HeapLimit, "Java array storage budget exhausted");
    return value;
  }
  uint64_t arrayValue(Object &array, uint32_t index,
                      std::optional<uint64_t> write) {
    if (array.Type != Object::PrimitiveArray &&
        array.Type != Object::ObjectArray)
      fail(ExecutionStop::Invalid, "array operand has non-array type");
    if (index >= array.Length)
      fail(ExecutionStop::Thrown, "DEX array index out of bounds",
           "java/lang/ArrayIndexOutOfBoundsException");
    const auto type = std::string_view(array.ClassName).substr(1);
    if (array.Type == Object::ObjectArray) {
      if (write) {
        auto value = object(*write, nullptr, true);
        if (value && !program.assignable(objectDescriptor(*value), type))
          fail(ExecutionStop::Thrown, "incompatible array element",
               "java/lang/ArrayStoreException");
        array.Elements[index] = value;
      }
      auto value = array.Elements[index].lock();
      return value ? value->Identity : 0;
    }
    if (!integral(type))
      fail(ExecutionStop::Unsupported,
           "floating array execution is unsupported");
    if (write && type == "Z" && *write > 1)
      fail(ExecutionStop::Invalid, "noncanonical boolean array store");
    uint64_t bits = 0;
    for (unsigned i = 0; i < array.ElementSize; ++i) {
      auto &byte = array.Bytes[uint64_t(index) * array.ElementSize + i];
      if (write)
        byte = uint8_t(*write >> (i * 8));
      bits |= uint64_t(byte) << (i * 8);
    }
    if (type == "Z" && bits > 1)
      fail(ExecutionStop::Invalid, "noncanonical boolean array value");
    return narrow(bits, type);
  }
  ExecutionTask fieldValue(const Field &field, uint64_t receiver,
                           std::optional<uint64_t> write, unsigned depth,
                           const Frame *frame = nullptr) {
    const auto &ref = field.reference;
    const bool is_static = has(field.access, "static");
    if (program.findField(ref, is_static) != &field)
      fail(ExecutionStop::Invalid,
           "field identity is not owned by supplied DEX");
    if (auto reason = program.unsupportedClass(ref.owner))
      fail(ExecutionStop::Unsupported, *reason);
    if (write && has(field.access, "final") &&
        !(frame && frame->method.reference.owner == ref.owner &&
          frame->method.reference.name == (is_static ? "<clinit>" : "<init>") &&
          (is_static || frame->constructing == receiver)))
      fail(ExecutionStop::Invalid, "final field write outside its initializer");
    if (write && referenceType(ref.type))
      compatible(*write,
                 ref.type); // Constructor escape is outside this profile.
    if (write && ref.type == "Z" && *write > 1)
      fail(ExecutionStop::Invalid, "noncanonical boolean field store");
    if (write)
      write = narrow(*write, ref.type);
    uint64_t *value;
    if (is_static) {
      if (receiver)
        fail(ExecutionStop::Invalid, "static field has an instance receiver");
      co_await initialize(ref.owner, depth);
      if (program.androidFrameworkField(field)) {
        auto result =
            state.framework.field(*state.heap, field, 0, write, admit);
        if (result.stop != ExecutionStop::Returned)
          throw Halt{std::move(result)};
        co_return narrow(result.value, ref.type);
      }
      value = &state.classes.at(program.findClass(ref.owner)).fields.at(&field);
    } else {
      auto instance = object(receiver, frame);
      if (instance->Type != Object::Instance ||
          !program.assignable(objectDescriptor(*instance), ref.owner))
        fail(ExecutionStop::Invalid, "field receiver type mismatch");
      if (program.androidFrameworkField(field)) {
        auto result =
            state.framework.field(*state.heap, field, receiver, write, admit);
        if (result.stop != ExecutionStop::Returned)
          throw Halt{std::move(result)};
        co_return narrow(result.value, ref.type);
      }
      value = &instance->Fields.at(&field);
    }
    if (write)
      *value = *write;
    co_return *value;
  }
  bool accessible(const std::string &owner, const Access &access,
                  const std::string &caller) const {
    if (owner == caller)
      return true;
    const auto *klass = program.findClass(owner);
    if (!klass)
      return program.knownReference(owner);
    auto package = [](std::string_view name) {
      const auto slash = name.rfind('/');
      return slash == name.npos ? std::string_view() : name.substr(0, slash);
    };
    const bool same_package = package(owner) == package(caller);
    // Supplied classes occupy one Program namespace. Admit package-visible
    // static members without changing virtual/protected resolution or JNI's
    // separate access rules. A public member still cannot expose an
    // inaccessible declaring class.
    const bool member = has(access, "public") ||
                        (same_package && has(access, "static") &&
                         !has(access, "private") && !has(access, "protected"));
    return member && (has(klass->access, "public") || same_package);
  }
  ExecutionTask initialize(std::string_view name, unsigned depth) {
    auto fail = [&](ExecutionStop stop, std::string message,
                    std::string exception = {}) {
      throw Halt{
          {stop, 0, std::move(exception), std::string(name) + ": " + message}};
    };
    if (auto reason = program.unsupportedClass(name))
      fail(ExecutionStop::Unsupported, *reason);
    const auto *klass = program.findClass(name);
    auto [entry, inserted] = state.classes.try_emplace(klass);
    auto &storage = entry->second;
    if (inserted) {
      // DEX encoded values are installed before <clinit>, including when a
      // recursive initialization reads this class's partially initialized
      // state.
      for (const auto &field : klass->fields) {
        if (!has(field.access, "static") ||
            program.androidFrameworkField(field))
          continue;
        uint64_t value = 0;
        const auto &type = field.reference.type;
        if (const auto *platform = program.platformValue(field)) {
          if (const auto *text = std::get_if<std::string>(&platform->value)) {
            auto bytes = javaStringBytes(*text);
            auto string =
                platform->intern_string
                    ? state.heap->literal(*text)
                    : state.heap->allocate(Object::String, "java/lang/String",
                                           bytes.size());
            if (!string)
              fail(ExecutionStop::HeapLimit,
                   "Java platform string storage budget exhausted");
            if (!platform->intern_string) {
              string->Bytes = std::move(bytes);
              string->Length = string->Bytes.size() / 2;
            }
            value = string->Identity;
          } else {
            value = narrow(uint64_t(std::get<int64_t>(platform->value)), type);
          }
          storage.fields.emplace(&field, value);
          continue;
        }
        if (referenceType(type) &&
            !std::holds_alternative<std::monostate>(field.value) &&
            !(type == "Ljava/lang/String;" &&
              std::holds_alternative<std::string>(field.value)))
          fail(ExecutionStop::Unsupported,
               "encoded reference field value is unsupported");
        if (auto text = std::get_if<std::string>(&field.value);
            text && type == "Ljava/lang/String;") {
          auto literal = state.heap->literal(*text);
          if (!literal)
            fail(ExecutionStop::HeapLimit,
                 "Java string storage budget exhausted");
          value = literal->Identity;
        } else if (auto integer = std::get_if<int64_t>(&field.value)) {
          value = narrow(uint64_t(*integer), type);
          int64_t canonical =
              type == "J" ? std::bit_cast<int64_t>(value)
              : type == "C" || type == "Z"
                  ? int64_t(value)
                  : int64_t(std::bit_cast<int32_t>(uint32_t(value)));
          if (canonical != *integer || (type == "Z" && value > 1))
            fail(ExecutionStop::Invalid, "noncanonical static field value");
        } else if (auto boolean = std::get_if<bool>(&field.value)) {
          if (type != "Z")
            fail(ExecutionStop::Invalid, "boolean value in nonboolean field");
          value = *boolean;
        } else if (!std::holds_alternative<std::monostate>(field.value))
          fail(ExecutionStop::Invalid, "incompatible static field value");
        storage.fields.emplace(&field, value);
      }
    }
    if (storage.status == ClassState::Initialized)
      co_return 0;
    if (storage.status == ClassState::Initializing) {
      if (storage.owner != owner)
        fail(ExecutionStop::Unsupported,
             "class initialization is owned by another execution thread");
      co_return 0;
    }
    if (storage.status == ClassState::Erroneous)
      fail(ExecutionStop::Thrown, "class initialization previously failed",
           "java/lang/NoClassDefFoundError");
    storage.status = ClassState::Initializing;
    storage.owner = owner;
    try {
      if (!has(klass->access, "interface") && klass->superclass &&
          program.containsClass(*klass->superclass))
        co_await initialize(*klass->superclass, depth + 1);
      if (program.android_framework_initializers.contains(klass)) {
        auto result = state.framework.initializeServiceManager(
            *state.heap, program, admit, [&](std::string_view dependency) {
              // Reserved framework dependencies have no suspended DEX bodies.
              // Reuse class preparation, owner and failure propagation.
              return state.start(
                  [dependency = std::string(dependency), depth](auto &engine) {
                    return engine.initialize(dependency, depth + 1);
                  },
                  admit, remaining_stack, owner);
            });
        if (result.stop != ExecutionStop::Returned)
          throw Halt{std::move(result)};
      }
      for (const auto &method : klass->methods)
        if (method.reference.name == "<clinit>") {
          if (!method.reference.parameters.empty() ||
              method.reference.returns != "V")
            fail(ExecutionStop::Invalid, "malformed class initializer");
          co_await run(method, {}, depth, true);
        }
      storage.status = ClassState::Initialized;
    } catch (Halt &halt) {
      if (halt.result.stop == ExecutionStop::Thrown) {
        storage.status = ClassState::Erroneous;
        // Nested initialization Errors propagate; Java exceptions are wrapped.
        if (!program.assignable("L" + halt.result.exception + ";",
                                "Ljava/lang/Error;")) {
          halt.result.exception = "java/lang/ExceptionInInitializerError";
          halt.result.throwable = 0;
        }
      }
      throw;
    }
    co_return 0;
  }
  ExecutionTask run(const Method &method, std::span<const uint64_t> args,
                    unsigned depth, bool initializer = false,
                    bool chained = false) {
    if (auto reason = program.unsupported(method, owner != 0))
      halt(ExecutionStop::Unsupported, method, 0, *reason);
    const bool constructor = method.reference.name == "<init>";
    const bool is_static = has(method.access, "static");
    if (method.reference.name.starts_with('<') && !initializer && !constructor)
      halt(ExecutionStop::Invalid, method, 0,
           "explicit initializer invocation");
    co_await initialize(method.reference.owner, depth);
    if (has(method.access, "native")) {
      if (depth >= ::neverd::limits::dalvik::kRuntimeRunNativeDepthLimit ||
          remaining_stack <
              ::neverd::limits::dalvik::kRuntimeRunNativeRemainingStackLimit)
        halt(ExecutionStop::StackLimit, method, 0,
             "DEX native frame budget exhausted");
      if (args.size() != method.reference.parameters.size() + !is_static)
        halt(ExecutionStop::Invalid, method, 0,
             "native argument count mismatch");
      if (!is_static) {
        if (!args[0])
          fail(ExecutionStop::Thrown, "null native receiver",
               "java/lang/NullPointerException");
        compatible(args[0], method.reference.owner);
      }
      for (size_t i = !is_static; i < args.size(); ++i) {
        const auto &type = method.reference.parameters[i - !is_static];
        if (referenceType(type))
          compatible(args[i], type);
        else if (args[i] != narrow(args[i], type) ||
                 (type == "Z" && args[i] > 1))
          halt(ExecutionStop::Invalid, method, 0,
               "noncanonical native argument");
      }
      remaining_stack -= 256;
      struct RestoreNativeStack {
        uint64_t &remaining;
        ~RestoreNativeStack() { remaining += 256; }
      } restore_native{remaining_stack};
      ExternalCall call;
      call.method = &method;
      call.arguments.assign(args.begin(), args.end());
      auto result = co_await request(std::move(call));
      const auto &type = method.reference.returns;
      if (type == "V")
        co_return 0;
      if (referenceType(type))
        compatible(result, type);
      else if (result != narrow(result, type) || (type == "Z" && result > 1))
        halt(ExecutionStop::Invalid, method, 0, "noncanonical native return");
      co_return result;
    }
    const uint64_t bytes = uint64_t(method.registers) * sizeof(Register) +
                           method.instructions.size() * 64 + 256;
    if (depth >= ::neverd::limits::dalvik::kRuntimeRunDepthLimit ||
        bytes > remaining_stack)
      halt(ExecutionStop::StackLimit, method, 0, "DEX frame budget exhausted");
    remaining_stack -= bytes;
    struct RestoreStack {
      uint64_t &remaining;
      uint64_t bytes;
      ~RestoreStack() { remaining += bytes; }
    } restore{remaining_stack, bytes};
    Frame frame(method);
    if (args.size() != method.reference.parameters.size() + !is_static ||
        method.incomingWords() > method.registers)
      frame.invalid("argument/frame shape mismatch");
    unsigned input = method.registers - method.incomingWords();
    if (!is_static) {
      auto receiver = state.heap->get(args[0]);
      if (!args[0])
        halt(ExecutionStop::Thrown, method, 0, "null method receiver",
             "java/lang/NullPointerException");
      if (!receiver || receiver->Type != Object::Instance ||
          !program.assignable(objectDescriptor(*receiver),
                              method.reference.owner))
        frame.invalid("method receiver type mismatch");
      if (constructor) {
        if (receiver->Initialized || method.reference.returns != "V")
          frame.invalid("constructor receiver is already initialized");
        frame.constructing = receiver->Identity;
      } else
        object(args[0]);
      frame.putRef(input++, args[0]);
    }
    for (size_t i = !is_static; i < args.size(); ++i) {
      const auto &type = method.reference.parameters[i - !is_static];
      if (referenceType(type)) {
        compatible(args[i], type);
        frame.putRef(input++, args[i]);
        continue;
      }
      auto bits = narrow(args[i], type);
      if (type == "Z" && bits > 1)
        frame.invalid("noncanonical boolean argument");
      frame.put(input, bits, type == "J");
      input += width(type);
    }
    std::map<uint32_t, size_t> positions;
    for (size_t i = 0; i < method.instructions.size(); ++i)
      if (!positions.emplace(method.instructions[i].pc, i).second)
        frame.invalid("duplicate instruction address");
    for (const auto &region : method.tries) {
      if (region.start >= region.end || region.handlers.empty() ||
          !positions.contains(region.start))
        frame.invalid("invalid try region");
      for (const auto &handler : region.handlers) {
        if (!positions.contains(handler.target))
          frame.invalid("invalid handler target");
        if (handler.type &&
            (!program.knownReference(*handler.type) ||
             !program.assignable(*handler.type, "Ljava/lang/Throwable;")))
          halt(ExecutionStop::Unsupported, method, 0, "unmodeled catch type");
      }
    }
    std::optional<uint64_t> pending, caught;
    std::string pending_type;
    for (size_t index = 0; index < method.instructions.size();) {
      const auto &insn = method.instructions[index++];
      frame.pc = insn.pc;
      auto budget = admit();
      if (budget != ExecutionStop::Returned)
        halt(budget, method, frame.pc, "DEX execution budget exhausted");
      const auto &r = insn.registers;
      const std::string_view op = insn.opcode;
      auto count = [&](size_t n) {
        if (r.size() != n)
          frame.invalid("operand shape mismatch");
      };
      auto branch = [&](std::optional<uint32_t> target) {
        if (!target || !positions.contains(*target))
          frame.invalid("branch does not target an instruction");
        index = positions.at(*target);
      };
      try {
        if (op == "move-exception") {
          count(1);
          if (!caught)
            frame.invalid("move-exception outside handler entry");
          frame.putRef(r[0], *caught);
          caught.reset();
          continue;
        }
        caught.reset(); // A handler may intentionally ignore its throwable.
        if (op == "move-result" || op == "move-result-wide" ||
            op == "move-result-object") {
          count(1);
          bool wide = op == "move-result-wide";
          const bool ref = op == "move-result-object";
          if (!pending || wide != (pending_type == "J") ||
              ref != referenceType(pending_type))
            frame.invalid("move-result without compatible invocation");
          if (ref)
            frame.putRef(r[0], *pending);
          else
            frame.put(r[0], *pending, wide);
          pending.reset();
          continue;
        }
        pending.reset();
        if (op == "nop")
          continue;
        if (op == "const-string" || op == "const-string/jumbo") {
          count(1);
          auto text = std::get_if<std::string>(&insn.literal);
          if (!text)
            frame.invalid("missing string literal");
          auto value = state.heap->literal(*text);
          if (!value)
            fail(ExecutionStop::HeapLimit,
                 "Java string storage budget exhausted");
          frame.putRef(r[0], value->Identity);
        } else if (op == "const-class") {
          count(1);
          auto type = std::get_if<std::string>(&insn.reference);
          if (!type || !referenceType(*type) || !program.knownReference(*type))
            fail(ExecutionStop::Unsupported, "unmodeled class literal");
          auto name = type->starts_with('L') ? type->substr(1, type->size() - 2)
                                             : *type;
          auto value = state.heap->classObject(name);
          if (!value)
            fail(ExecutionStop::HeapLimit,
                 "Java class storage budget exhausted");
          frame.putRef(r[0], value->Identity);
        } else if (op == "throw") {
          count(1);
          auto value = object(frame.ref(r[0]));
          if (!program.assignable(objectDescriptor(*value),
                                  "Ljava/lang/Throwable;"))
            frame.invalid("throw operand is not a throwable");
          throw Halt{{ExecutionStop::Thrown, 0, value->ClassName,
                      "explicit throw", value->Identity}};
        } else if (op == "const" || op == "const/4" || op == "const/16" ||
                   op == "const/high16" || op == "const-wide" ||
                   op == "const-wide/16" || op == "const-wide/32" ||
                   op == "const-wide/high16") {
          count(1);
          auto literal = std::get_if<int64_t>(&insn.literal);
          if (!literal)
            frame.invalid("missing integer constant");
          frame.put(r[0], uint64_t(*literal), op.starts_with("const-wide"));
          if (!*literal && !op.starts_with("const-wide"))
            frame.registers[r[0]].kind = Register::Zero;
        } else if (op == "move-object" || op == "move-object/from16" ||
                   op == "move-object/16") {
          count(2);
          frame.putRef(r[0], frame.ref(r[1]));
        } else if (op == "move" || op == "move/from16" || op == "move/16" ||
                   op == "move-wide" || op == "move-wide/from16" ||
                   op == "move-wide/16") {
          count(2);
          bool wide = op.starts_with("move-wide");
          frame.range(r[1], wide);
          const bool zero =
              !wide && frame.registers[r[1]].kind == Register::Zero;
          frame.put(r[0], frame.get(r[1], wide), wide);
          if (zero)
            frame.registers[r[0]].kind = Register::Zero;
        } else if (op == "return" || op == "return-wide" ||
                   op == "return-void" || op == "return-object") {
          bool empty = op == "return-void", wide = op == "return-wide",
               ref = op == "return-object";
          count(empty ? 0 : 1);
          if (empty != (method.reference.returns == "V") ||
              wide != (method.reference.returns == "J") ||
              ref != referenceType(method.reference.returns))
            frame.invalid("return type mismatch");
          auto value = empty ? 0
                       : ref ? frame.ref(r[0])
                             : frame.get(r[0], wide);
          if (ref)
            compatible(value, method.reference.returns);
          if (constructor) {
            if (!frame.base_initialized)
              frame.invalid("constructor returned without base initialization");
            if (!chained)
              state.heap->get(frame.constructing)->Initialized = true;
          }
          if (!empty && value != narrow(value, method.reference.returns))
            frame.invalid(
                "return value is not canonical for its declared type");
          if (method.reference.returns == "Z" && value > 1)
            frame.invalid("noncanonical boolean return");
          co_return value;
        } else if (op == "goto" || op == "goto/16" || op == "goto/32") {
          count(0);
          branch(insn.target);
        } else if (op.starts_with("if-")) {
          bool zero = op.ends_with('z');
          count(zero ? 1 : 2);
          frame.range(r[0], false);
          if (!zero)
            frame.range(r[1], false);
          auto relation = op.substr(3, 2);
          const bool refs =
              frame.registers[r[0]].kind == Register::Ref ||
              (!zero && frame.registers[r[1]].kind == Register::Ref);
          if (refs) {
            if (relation != "eq" && relation != "ne")
              frame.invalid("ordered reference comparison");
            object(frame.ref(r[0]), nullptr, true);
            if (!zero)
              object(frame.ref(r[1]), nullptr, true);
            bool equal = frame.ref(r[0]) == (zero ? 0 : frame.ref(r[1]));
            if (equal == (relation == "eq"))
              branch(insn.target);
            continue;
          }
          int32_t a = std::bit_cast<int32_t>(uint32_t(frame.get(r[0])));
          int32_t b =
              zero ? 0 : std::bit_cast<int32_t>(uint32_t(frame.get(r[1])));
          bool take;
          if (relation == "eq")
            take = a == b;
          else if (relation == "ne")
            take = a != b;
          else if (relation == "lt")
            take = a < b;
          else if (relation == "ge")
            take = a >= b;
          else if (relation == "gt")
            take = a > b;
          else if (relation == "le")
            take = a <= b;
          else
            frame.invalid("unknown branch relation");
          if (take)
            branch(insn.target);
        } else if (op == "packed-switch" || op == "sparse-switch") {
          count(1);
          if (insn.keys.size() != insn.targets.size())
            frame.invalid("switch shape mismatch");
          int32_t key = std::bit_cast<int32_t>(uint32_t(frame.get(r[0])));
          for (size_t i = 0; i < insn.keys.size(); ++i)
            if (key == insn.keys[i]) {
              branch(insn.targets[i]);
              break;
            }
        } else if (op == "new-instance") {
          count(1);
          auto type = std::get_if<std::string>(&insn.reference);
          if (!type)
            frame.invalid("missing allocation type");
          if (!accessible(*type, Access{"public"}, method.reference.owner))
            halt(ExecutionStop::Unsupported, method, frame.pc,
                 "inaccessible allocation class");
          frame.putRef(r[0], co_await allocate(*type, false, depth + 1));
        } else if (op == "new-array" || op == "filled-new-array" ||
                   op == "filled-new-array/range") {
          auto type = std::get_if<std::string>(&insn.reference);
          if (!type)
            frame.invalid("missing array type");
          bool filled = op != "new-array";
          if (!filled)
            count(2);
          const int32_t length =
              filled ? int32_t(r.size())
                     : std::bit_cast<int32_t>(uint32_t(frame.get(r[1])));
          auto value = array(*type, length);
          if (filled) {
            auto component = std::string_view(*type).substr(1);
            if (component == "J")
              frame.invalid("wide filled-new-array component");
            for (unsigned i = 0; i < r.size(); ++i)
              arrayValue(*value, i,
                         referenceType(component) ? frame.ref(r[i])
                                                  : frame.get(r[i]));
            pending = value->Identity;
            pending_type = *type;
          } else
            frame.putRef(r[0], value->Identity);
        } else if (op == "array-length") {
          count(2);
          auto value = object(frame.ref(r[1]));
          if (value->Type != Object::PrimitiveArray &&
              value->Type != Object::ObjectArray)
            frame.invalid("array-length operand is not an array");
          frame.put(r[0], value->Length);
        } else if (op == "fill-array-data") {
          count(1);
          auto value = object(frame.ref(r[0]));
          if (value->Type != Object::PrimitiveArray ||
              value->ElementSize != insn.element_width)
            frame.invalid("array payload element width/type mismatch");
          if (insn.data.size() > value->Length)
            halt(ExecutionStop::Thrown, method, frame.pc,
                 "array payload exceeds length",
                 "java/lang/ArrayIndexOutOfBoundsException");
          for (unsigned i = 0; i < insn.data.size(); ++i) {
            auto status = admit();
            if (status != ExecutionStop::Returned)
              halt(status, method, frame.pc, "array payload budget exhausted");
            arrayValue(*value, i, insn.data[i]);
          }
        } else if (op.starts_with("aget") || op.starts_with("aput")) {
          count(3);
          auto value = object(frame.ref(r[1]));
          auto suffix = op.substr(4);
          std::string_view type = suffix.empty()         ? "I"
                                  : suffix == "-wide"    ? "J"
                                  : suffix == "-object"  ? "L"
                                  : suffix == "-boolean" ? "Z"
                                  : suffix == "-byte"    ? "B"
                                  : suffix == "-char"    ? "C"
                                  : suffix == "-short"   ? "S"
                                                         : "";
          auto component = std::string_view(value->ClassName).substr(1);
          if (type.empty() ||
              (type == "L" ? !referenceType(component) : type != component))
            frame.invalid("array opcode/component mismatch");
          bool write = op.starts_with("aput");
          auto bits =
              arrayValue(*value, uint32_t(frame.get(r[2])),
                         write ? std::optional<uint64_t>(
                                     type == "L" ? frame.ref(r[0])
                                                 : frame.get(r[0], type == "J"))
                               : std::nullopt);
          if (!write) {
            if (type == "L")
              frame.putRef(r[0], bits);
            else
              frame.put(r[0], bits, type == "J");
          }
        } else if (op == "check-cast" || op == "instance-of") {
          count(op == "check-cast" ? 1 : 2);
          auto type = std::get_if<std::string>(&insn.reference);
          if (!type || !referenceType(*type))
            frame.invalid("invalid cast type");
          known(*type);
          auto value =
              object(frame.ref(r[op == "check-cast" ? 0 : 1]), nullptr, true);
          bool matches =
              value && program.assignable(objectDescriptor(*value), *type);
          if (op == "instance-of")
            frame.put(r[0], matches);
          else if (value && !matches)
            halt(ExecutionStop::Thrown, method, frame.pc, "incompatible cast",
                 "java/lang/ClassCastException");
        } else if (op == "invoke-static" || op == "invoke-static/range" ||
                   op == "invoke-direct" || op == "invoke-direct/range" ||
                   op == "invoke-super" || op == "invoke-super/range" ||
                   op == "invoke-interface" || op == "invoke-interface/range" ||
                   op == "invoke-virtual" || op == "invoke-virtual/range") {
          auto reference = std::get_if<MethodRef>(&insn.reference);
          if (!reference)
            frame.invalid("missing method reference");
          const bool static_call = op.starts_with("invoke-static"),
                     direct = op.starts_with("invoke-direct"),
                     super = op.starts_with("invoke-super"),
                     ctor = reference->name == "<init>";
          std::vector<uint64_t> values;
          size_t word = 0;
          uint64_t receiver = 0;
          if (!static_call) {
            if (r.empty())
              frame.invalid("missing invocation receiver");
            receiver = frame.ref(r[word++]);
            if (!ctor)
              compatible(receiver, reference->owner, &frame);
            if (!receiver)
              fail(ExecutionStop::Thrown, "null method receiver",
                   "java/lang/NullPointerException");
            values.push_back(receiver);
          }
          for (const auto &type : reference->parameters) {
            const bool wide = type == "J";
            // Exact catalogued leaves consume F as an IEEE-754 bit carrier.
            // This does not admit floating arithmetic or unknown F callees.
            if (!integral(type) && !referenceType(type) &&
                !(type == "F" && libraryMethod(*reference, static_call)))
              fail(ExecutionStop::Unsupported, "floating method invocation");
            if (word >= r.size() ||
                (wide && (word + 1 >= r.size() || r[word + 1] != r[word] + 1)))
              frame.invalid("invoke argument word mismatch");
            auto bits = referenceType(type) ? frame.ref(r[word])
                                            : frame.get(r[word], wide);
            if (referenceType(type))
              compatible(bits, type);
            if (bits != narrow(bits, type) || (type == "Z" && bits > 1))
              frame.invalid(
                  "invoke argument is not canonical for its declared type");
            values.push_back(bits);
            word += wide ? 2 : 1;
          }
          if (word != r.size())
            frame.invalid("invoke argument count mismatch");
          auto selected = *reference;
          const bool chain = ctor && receiver == frame.constructing;
          if (ctor) {
            auto value = state.heap->get(receiver);
            if (!direct || !value || value->Initialized ||
                (!chain && objectDescriptor(*value) != reference->owner) ||
                (chain && (frame.base_initialized ||
                           (reference->owner != method.reference.owner &&
                            reference->owner !=
                                program.parent(method.reference.owner)))))
              frame.invalid("invalid constructor receiver or chaining order");
          }
          const Method *callee = program.findMethod(selected, static_call);
          if (!static_call && !direct) {
            auto owner = program.findClass(reference->owner);
            if (owner &&
                op.starts_with("invoke-interface") !=
                    has(owner->access, "interface") &&
                !super)
              frame.invalid("invoke interface/class kind mismatch");
            // Resolve the symbolic declaration before selecting an override.
            bool resolved = callee != nullptr;
            auto declaration = *reference;
            for (unsigned n = 0;
                 !resolved && !declaration.owner.empty() &&
                 n < ::neverd::limits::dalvik::kMaxRuntimeMethodHierarchyDepth;
                 ++n) {
              resolved = libraryMethod(declaration, false) != nullptr;
              declaration.owner = program.parent(declaration.owner);
            }
            if (!resolved)
              fail(ExecutionStop::Unsupported,
                   "unresolved virtual declaration " + reference->identity());
            if (callee && has(callee->access, "private"))
              frame.invalid("virtual invocation names a private method");
          }
          if (super) {
            if (has(method.access, "static") || ctor ||
                !program.assignable(method.reference.owner, reference->owner))
              frame.invalid("invalid super invocation");
            selected.owner = program.parent(method.reference.owner);
            callee = program.findMethod(selected, false);
          } else if (!static_call && !direct) {
            callee = program.virtualMethod(selected,
                                           objectDescriptor(*object(receiver)));
          }
          uint64_t result = 0;
          if (callee) {
            if (!accessible(callee->reference.owner, callee->access,
                            method.reference.owner))
              fail(ExecutionStop::Unsupported, "inaccessible cross-class call");
            if (!static_call &&
                direct != (ctor || has(callee->access, "private")))
              frame.invalid("invoke kind does not match method access");
            result = co_await run(*callee, values, depth + 1, false, chain);
          } else {
            const LibraryMethod *library = libraryMethod(selected, static_call);
            if (!library && !static_call && !ctor) {
              // Resolve the finite platform base only after supplied virtual
              // overrides.
              auto owner = selected.owner;
              for (unsigned i = 0; !owner.empty() &&
                                   i < ::neverd::limits::dalvik::
                                           kMaxRuntimeMethodHierarchyDepth &&
                                   !library;
                   ++i) {
                selected.owner = owner;
                library = libraryMethod(selected, false);
                owner = program.parent(owner);
              }
            }
            if (!library)
              fail(ExecutionStop::Unsupported,
                   "unmodeled method " + reference->identity());
            if (!library->Public)
              fail(ExecutionStop::Unsupported,
                   "nonpublic library method access requires an explicit JNI "
                   "call");
            if (direct != ctor && !static_call)
              frame.invalid("invalid library invoke kind");
            if (static_call && program.android_framework_initializers.contains(
                                   program.findClass(selected.owner)))
              co_await initialize(selected.owner, depth + 1);
            auto args = std::span<const uint64_t>(values).subspan(!static_call);
            auto executed =
                executeLibrary(*state.heap, *library, receiver, args, admit,
                               super || direct, &program, &state.framework);
            if (executed.stop == ExecutionStop::Suspended &&
                executed.external) {
              executed.external->caller = method.reference.owner;
              result = co_await request(std::move(*executed.external));
            } else {
              if (executed.stop != ExecutionStop::Returned)
                throw Halt{std::move(executed)};
              result = executed.value;
            }
            if (ctor && !chain)
              state.heap->get(receiver)->Initialized = true;
          }
          if (chain)
            frame.base_initialized = true;
          pending_type = reference->returns;
          if (pending_type != "V")
            pending = result;
        } else if (op.starts_with("sget") || op.starts_with("sput") ||
                   op.starts_with("iget") || op.starts_with("iput")) {
          bool static_field = op.starts_with('s'),
               write = op.substr(1, 3) == "put";
          count(static_field ? 1 : 2);
          auto reference = std::get_if<FieldRef>(&insn.reference);
          if (!reference)
            frame.invalid("missing field reference");
          auto field = program.findField(*reference, static_field);
          if (!field)
            halt(ExecutionStop::Unsupported, method, frame.pc,
                 "unmodeled field");
          if (!accessible(field->reference.owner, field->access,
                          method.reference.owner))
            halt(ExecutionStop::Unsupported, method, frame.pc,
                 "inaccessible cross-class field");
          auto suffix = op.substr(4);
          std::string_view type = suffix.empty()         ? "I"
                                  : suffix == "-wide"    ? "J"
                                  : suffix == "-object"  ? "L"
                                  : suffix == "-boolean" ? "Z"
                                  : suffix == "-byte"    ? "B"
                                  : suffix == "-char"    ? "C"
                                  : suffix == "-short"   ? "S"
                                                         : "";
          if (type.empty() || (type == "L" ? !referenceType(reference->type)
                                           : reference->type != type))
            frame.invalid("field opcode/type mismatch");
          auto value = co_await fieldValue(
              *field, static_field ? 0 : frame.ref(r[1]),
              write ? std::optional<uint64_t>(
                          type == "L" ? frame.ref(r[0])
                                      : frame.get(r[0], type == "J"))
                    : std::nullopt,
              depth + 1, &frame);
          if (!write) {
            if (type == "L")
              frame.putRef(r[0], value);
            else
              frame.put(r[0], value, type == "J");
          }
        } else if (op == "cmp-long") {
          count(3);
          auto a = std::bit_cast<int64_t>(frame.get(r[1], true));
          auto b = std::bit_cast<int64_t>(frame.get(r[2], true));
          frame.put(r[0], uint32_t(a < b ? -1 : a > b ? 1 : 0));
        } else if (op == "int-to-long" || op == "long-to-int" ||
                   op == "int-to-byte" || op == "int-to-char" ||
                   op == "int-to-short") {
          count(2);
          auto a = frame.get(r[1], op == "long-to-int");
          if (op == "int-to-long")
            a = uint64_t(int64_t(std::bit_cast<int32_t>(uint32_t(a))));
          else if (op == "int-to-byte")
            a = narrow(a, "B");
          else if (op == "int-to-char")
            a = narrow(a, "C");
          else if (op == "int-to-short")
            a = narrow(a, "S");
          frame.put(r[0], a, op == "int-to-long");
        } else if (op == "neg-int" || op == "not-int" || op == "neg-long" ||
                   op == "not-long") {
          count(2);
          bool wide = op.ends_with("long");
          auto a = frame.get(r[1], wide);
          frame.put(r[0], op.starts_with("neg") ? uint64_t(0) - a : ~a, wide);
        } else {
          size_t dash = op.find('-');
          auto type =
              dash == op.npos ? std::string_view() : op.substr(dash + 1);
          auto base_type = type.substr(0, type.find('/'));
          if (base_type != "int" && base_type != "long")
            halt(ExecutionStop::Unsupported, method, frame.pc,
                 "unsupported opcode " + insn.opcode);
          bool wide = base_type == "long", inplace = type.ends_with("/2addr");
          bool literal = type.ends_with("/lit8") || type.ends_with("/lit16") ||
                         op == "rsub-int";
          auto operation = op.substr(0, dash);
          bool shift =
              operation == "shl" || operation == "shr" || operation == "ushr";
          count(inplace || literal ? 2 : 3);
          uint64_t left = frame.get(r[inplace ? 0 : 1], wide), right;
          if (literal) {
            auto value = std::get_if<int64_t>(&insn.literal);
            if (!value || wide)
              frame.invalid("integer literal mismatch");
            right = uint32_t(*value);
          } else
            right = frame.get(r[inplace ? 1 : 2], wide && !shift);
          frame.put(
              r[0],
              wide ? binary(operation, left, right, frame)
                   : binary(operation, uint32_t(left), uint32_t(right), frame),
              wide);
        }
      } catch (Halt &failure) {
        state.throwable(failure.result);
        bool handled = false;
        if (failure.result.stop == ExecutionStop::Thrown) {
          for (const auto &region : method.tries) {
            if (frame.pc < region.start || frame.pc >= region.end)
              continue;
            for (const auto &handler : region.handlers) {
              if (handler.type &&
                  !program.assignable("L" + failure.result.exception + ";",
                                      *handler.type))
                continue;
              branch(handler.target);
              caught = failure.result.throwable;
              pending.reset();
              handled = true;
              break;
            }
            break; // DEX try ranges do not overlap.
          }
        }
        if (handled)
          continue;
        if (failure.result.diagnostic.find(" at code-unit ") ==
            std::string::npos)
          failure.result.diagnostic =
              method.reference.identity() + " at code-unit " +
              std::to_string(frame.pc) + ": " + failure.result.diagnostic;
        throw;
      }
    }
    frame.invalid("execution fell off method body");
  }
};

Runtime::State::~State() = default;
ExecutionResult
Runtime::State::start(std::function<ExecutionTask(Engine &)> entry,
                      const AdmitInstruction &admit, uint64_t stack,
                      uint64_t owner) {
  if (stopped)
    return *stopped;
  if (!admit ||
      executions.size() >=
          ::neverd::limits::dalvik::kRuntimeStateStartExecutionsSizeLimit ||
      next_execution == UINT64_MAX) {
    stopped = ExecutionResult{
        ExecutionStop::StackLimit, 0, {}, "Java invocation budget exhausted"};
    return *stopped;
  }
  for (const auto &[id, prior] : executions)
    if (prior->owner == owner)
      stack = std::min(stack, prior->remaining_stack);
  auto engine = std::make_unique<Engine>(*this, admit, stack, owner);
  engine->entry = std::move(entry);
  engine->task.emplace(engine->entry(*engine));
  const auto id = next_execution++;
  executions.emplace(id, std::move(engine));
  return advance(id);
}
ExecutionResult Runtime::State::advance(uint64_t id) {
  auto &engine = *executions.at(id);
  if (stopped)
    return *stopped;
  if (engine.suspended) {
    auto handle = std::exchange(engine.suspended, {});
    handle.resume();
  } else
    engine.task->resume();
  if (!engine.task->done()) {
    ExecutionResult result;
    result.stop = ExecutionStop::Suspended;
    result.external = engine.external;
    // A fresh token authenticates this occurrence, including two consecutive
    // calls from the same retained Java activation.
    if (next_execution == UINT64_MAX) {
      stopped = ExecutionResult{ExecutionStop::StackLimit,
                                0,
                                {},
                                "Java continuation identity budget exhausted"};
      return *stopped;
    }
    result.continuation = next_execution++;
    auto node = executions.extract(id);
    node.key() = result.continuation;
    executions.insert(std::move(node));
    return result;
  }
  auto result = invoke([&] { return engine.task->await_resume(); });
  executions.erase(id);
  return result;
}

Program::Program(
    std::vector<Class> declarations,
    std::optional<std::map<std::string, std::string>> android_properties) {
  for (auto &item : declarations) {
    if (item.name.starts_with("Ljava/") || item.name.starts_with("Ljavax/") ||
        item.name.starts_with("Landroid/") ||
        item.name.starts_with("Ldalvik/") ||
        item.name == "Lcom/android/internal/util/StatLogger;")
      throw Error("DEX cannot replace platform classes");
    auto name = item.name;
    auto [entry, inserted] = classes.emplace(std::move(name), std::move(item));
    if (!inserted)
      throw Error("duplicate supplied DEX class");
    supplied_classes.insert(&entry->second);
  }
  if (android_properties) {
    addAndroidVersion(*android_properties);
    addAndroidFramework();
  }
}
bool Program::containsClass(std::string_view name) const {
  return findClass(name) != nullptr;
}
const Class *Program::findClass(std::string_view name) const {
  auto it = classes.find(std::string(name));
  return it == classes.end() ? nullptr : &it->second;
}
bool Program::knownReference(std::string_view name) const {
  while (name.starts_with('['))
    name.remove_prefix(1);
  if (integral(name))
    return true;
  if (name.starts_with('L') && name.ends_with(';') &&
      platformClass(name.substr(1, name.size() - 2)))
    return true;
  return containsClass(name) && !unsupportedClass(name);
}
std::string Program::parent(std::string_view name) const {
  if (auto c = findClass(name))
    return c->superclass.value_or("");
  if (!name.starts_with('L') || !name.ends_with(';'))
    return "Ljava/lang/Object;";
  auto p = javaParent(name.substr(1, name.size() - 2));
  return p.empty() ? "" : "L" + std::string(p) + ";";
}
bool Program::assignable(std::string_view source,
                         std::string_view destination) const {
  if (source == destination)
    return true;
  if (!referenceType(source) || !referenceType(destination))
    return false;
  if (source.starts_with('[')) {
    if (destination == "Ljava/lang/Object;" ||
        destination == "Ljava/lang/Cloneable;" ||
        destination == "Ljava/io/Serializable;")
      return true;
    return destination.starts_with('[') && referenceType(source.substr(1)) &&
           referenceType(destination.substr(1)) &&
           assignable(source.substr(1), destination.substr(1));
  }
  if (destination.starts_with('['))
    return false;
  std::vector<std::string> pending{std::string(source)};
  std::set<std::string> seen;
  while (!pending.empty()) {
    auto name = std::move(pending.back());
    pending.pop_back();
    if (name == destination)
      return true;
    if (!seen.insert(name).second)
      continue;
    if (auto c = findClass(name)) {
      pending.insert(pending.end(), c->interfaces.begin(), c->interfaces.end());
      if (c->superclass)
        pending.push_back(*c->superclass);
    } else {
      if (referenceAssignable(name, destination))
        return true;
      if (auto base = parent(name); !base.empty())
        pending.push_back(std::move(base));
    }
  }
  return false;
}
const Field *Program::findField(const FieldRef &reference,
                                bool is_static) const {
  auto name = reference.owner;
  for (unsigned depth = 0;
       depth < ::neverd::limits::dalvik::kRuntimeProgramFindFieldDepthLimit;
       ++depth) {
    const auto *klass = findClass(name);
    if (!klass)
      break;
    for (const auto &field : klass->fields)
      if (field.reference.name == reference.name &&
          field.reference.type == reference.type)
        return has(field.access, "static") == is_static ? &field : nullptr;
    name = parent(name);
  }
  return nullptr;
}
const Field *Program::findStaticField(const FieldRef &reference) const {
  return findField(reference, true);
}
const Method *Program::findMethod(const MethodRef &reference,
                                  std::optional<bool> is_static) const {
  std::set<std::string> seen;
  std::function<const Method *(std::string_view, unsigned)> find =
      [&](std::string_view name, unsigned depth) -> const Method * {
    const auto *klass = findClass(name);
    if (!klass ||
        depth >= ::neverd::limits::dalvik::
                     kRuntimeProgramFindMethodFindFindDepthLimit ||
        !seen.insert(klass->name).second)
      return nullptr;
    for (const auto &method : klass->methods)
      if (method.reference.name == reference.name &&
          method.reference.parameters == reference.parameters &&
          method.reference.returns == reference.returns)
        return !is_static || has(method.access, "static") == *is_static
                   ? &method
                   : nullptr;
    if (reference.name.starts_with('<'))
      return nullptr;
    if (auto base = find(parent(name), depth + 1))
      return base;
    // The admitted interfaces contain public abstract declarations only.
    if (is_static && !*is_static)
      for (const auto &interface : klass->interfaces)
        if (auto method = find(interface, depth + 1))
          return method;
    return nullptr;
  };
  return find(reference.owner, 0);
}
const Method *Program::virtualMethod(const MethodRef &reference,
                                     std::string_view receiver) const {
  if (!assignable(receiver, reference.owner))
    return nullptr;
  auto name = std::string(receiver);
  for (unsigned depth = 0;
       depth < ::neverd::limits::dalvik::kRuntimeProgramVirtualMethodDepthLimit;
       ++depth) {
    const auto *klass = findClass(name);
    if (!klass)
      break;
    for (const auto &method : klass->methods)
      if (method.reference.name == reference.name &&
          method.reference.parameters == reference.parameters &&
          method.reference.returns == reference.returns &&
          !has(method.access, "static") && !has(method.access, "private"))
        return &method;
    name = parent(name);
  }
  return nullptr;
}
const Method *Program::findStatic(const MethodRef &reference) const {
  return findMethod(reference, true);
}
std::optional<std::string>
Program::unsupportedClass(std::string_view name) const {
  std::set<std::string> visiting, done;
  std::function<std::optional<std::string>(std::string_view, unsigned)> check =
      [&](std::string_view name, unsigned depth) -> std::optional<std::string> {
    if (done.contains(std::string(name)))
      return std::nullopt;
    const auto *klass = findClass(name);
    if (!klass) {
      if (name == "Ljava/lang/Object;" ||
          (name.starts_with('L') && name.ends_with(';') &&
           platformClass(name.substr(1, name.size() - 2)) &&
           referenceAssignable(name, "Ljava/lang/Throwable;")))
        return std::nullopt;
      return "class is not supplied by DEX";
    }
    if (depth >= ::neverd::limits::dalvik::
                     kRuntimeProgramUnsupportedClassCheckDepthLimit ||
        !visiting.insert(klass->name).second)
      return "DEX cyclic or excessively deep hierarchy";
    if (!klass->superclass)
      return "DEX class has no superclass";
    if (const auto *base = findClass(*klass->superclass);
        base && (has(base->access, "final") || has(base->access, "interface")))
      return "DEX invalid superclass";
    if (auto reason = check(*klass->superclass, depth + 1))
      return reason;
    for (const auto &iface : klass->interfaces) {
      const auto *base = findClass(iface);
      if (!base || !has(base->access, "interface"))
        return "unmodeled DEX interface";
      if (auto reason = check(iface, depth + 1))
        return reason;
    }
    if (has(klass->access, "interface")) {
      if (!klass->fields.empty())
        return "interface fields are outside the execution profile";
      for (const auto &method : klass->methods)
        if (!has(method.access, "abstract") || !has(method.access, "public"))
          return "default or static interface bodies are unsupported";
    }
    for (const auto &field : klass->fields)
      if ((!integral(field.reference.type) &&
           !referenceType(field.reference.type) &&
           !(field.reference.type == "F" && androidFrameworkField(field))) ||
          (has(field.access, "volatile") && !androidFrameworkField(field)))
        return "DEX floating or volatile field is unsupported";
    for (const auto &method : klass->methods) {
      for (const auto &inherited : libraryMethods()) {
        if (!inherited.Final || method.reference.name != inherited.Name ||
            method.reference.signature() != inherited.Signature)
          continue;
        auto parent_name = *klass->superclass;
        const auto owner = "L" + std::string(inherited.Class) + ";";
        for (unsigned i = 0;
             i < ::neverd::limits::dalvik::
                     kRuntimeProgramUnsupportedClassCheckDepthLimit &&
             !parent_name.empty();
             ++i) {
          if (parent_name == owner)
            return "DEX declaration conflicts with a final library method";
          parent_name = parent(parent_name);
        }
      }
    }
    visiting.erase(klass->name);
    done.insert(klass->name);
    return std::nullopt;
  };
  // Unknown ancestors are never recursively admitted through knownReference.
  if (!findClass(name))
    return "class is not supplied by DEX";
  return check(name, 0);
}
std::optional<std::string> Program::unsupported(const Method &method,
                                                bool allow_native) const {
  auto owner = classes.find(method.reference.owner);
  if (owner == classes.end() ||
      findMethod(method.reference, has(method.access, "static")) != &method)
    return "method is not an admitted DEX body";
  if (auto reason = unsupportedClass(method.reference.owner))
    return reason;
  if ((!allow_native && has(method.access, "native")) ||
      has(method.access, "abstract") || has(method.access, "synchronized") ||
      has(method.access, "declared-synchronized") ||
      (method.instructions.empty() && !has(method.access, "native")))
    return "DEX native, abstract or synchronized body is "
           "unsupported";
  if (method.reference.returns != "V" && !integral(method.reference.returns) &&
      !knownReference(method.reference.returns))
    return "DEX return type is unsupported";
  for (const auto &type : method.reference.parameters)
    if (!integral(type) && !knownReference(type))
      return "DEX argument type is unsupported";
  return std::nullopt;
}
ExecutionResult Program::execute(const Method &method,
                                 std::span<const uint64_t> args,
                                 const AdmitInstruction &admit,
                                 uint64_t stack_bytes) const {
  Runtime runtime(*this);
  return runtime.execute(method, args, admit, stack_bytes);
}
Runtime::Runtime(const Program &program, std::shared_ptr<Heap> heap,
                 std::optional<AndroidApplicationSnapshot> application,
                 RuntimeServices services)
    : state(std::make_unique<State>(
          program, heap ? std::move(heap) : std::make_shared<Heap>(),
          AndroidFramework(application, program, std::move(services)))) {
  if (application && !program.containsClass("Landroid/app/ActivityThread;"))
    throw Error("Android application snapshot requires an Android program");
}
Runtime::~Runtime() = default;
RuntimeSnapshot Runtime::snapshot() const {
  RuntimeSnapshot result;
  result.terminal = state->stopped.has_value();
  result.framework = state->framework.inspect(*state->heap, state->program);
  for (const auto &[name, declaration] : state->program.classes) {
    JavaClassState row;
    row.descriptor = name;
    row.source_id = declaration.source_id;
    row.superclass = declaration.superclass;
    row.supplied = state->program.supplied_classes.contains(&declaration);
    if (name.size() >= 2 && name.front() == 'L' && name.back() == ';')
      row.class_object = state->heap->classObjectIdentity(
          std::string_view(name).substr(1, name.size() - 2));
    row.projected_loader =
        state->framework.definingLoader(name, state->program);
    using Initialization = JavaClassState::Initialization;
    if (auto it = state->classes.find(&declaration);
        it != state->classes.end()) {
      switch (it->second.status) {
      case State::ClassState::Prepared:
        row.initialization = Initialization::Prepared;
        break;
      case State::ClassState::Initializing:
        row.initialization = Initialization::Initializing;
        break;
      case State::ClassState::Initialized:
        row.initialization = Initialization::Initialized;
        break;
      case State::ClassState::Erroneous:
        row.initialization = Initialization::Erroneous;
        break;
      }
    }
    result.classes.push_back(std::move(row));
  }
  return result;
}
ExecutionResult Runtime::allocate(std::string_view descriptor, bool initialized,
                                  const AdmitInstruction &admit,
                                  uint64_t stack_bytes, uint64_t owner) {
  return state->start(
      [name = std::string(descriptor), initialized](auto &engine) {
        return engine.allocate(name, initialized, 0);
      },
      admit, stack_bytes, owner);
}
ExecutionResult Runtime::field(const Field &field, uint64_t receiver,
                               std::optional<uint64_t> write,
                               const AdmitInstruction &admit,
                               uint64_t stack_bytes, uint64_t owner) {
  return state->start(
      [&field, receiver, write](auto &engine) {
        return engine.fieldValue(field, receiver, write, 0);
      },
      admit, stack_bytes, owner);
}
ExecutionResult Runtime::initialize(std::string_view descriptor,
                                    const AdmitInstruction &admit,
                                    uint64_t stack_bytes, uint64_t owner) {
  return state->start([name = std::string(descriptor)](
                          auto &engine) { return engine.initialize(name, 0); },
                      admit, stack_bytes, owner);
}
ExecutionResult Runtime::execute(const Method &method,
                                 std::span<const uint64_t> args,
                                 const AdmitInstruction &admit,
                                 uint64_t stack_bytes, uint64_t owner) {
  return state->start(
      [&method, values = std::vector<uint64_t>(args.begin(), args.end())](
          auto &engine) { return engine.run(method, values, 0); },
      admit, stack_bytes, owner);
}
ExecutionResult Runtime::resume(uint64_t id, uint64_t owner,
                                ExecutionResult completion) {
  auto it = state->executions.find(id);
  if (!id || !owner || it == state->executions.end() ||
      it->second->owner != owner || !it->second->suspended ||
      completion.stop == ExecutionStop::Suspended || completion.external ||
      completion.continuation)
    return {
        ExecutionStop::Invalid, 0, {}, "invalid Java continuation completion"};
  for (auto later = std::next(it); later != state->executions.end(); ++later)
    if (later->second->owner == owner)
      return {ExecutionStop::Invalid,
              0,
              {},
              "Java continuations must resume in owner stack order"};
  if ((completion.stop == ExecutionStop::Returned &&
       (completion.throwable || !completion.exception.empty())) ||
      (completion.stop == ExecutionStop::Thrown &&
       (!state->program.knownReference("L" + completion.exception + ";") ||
        !state->program.assignable("L" + completion.exception + ";",
                                   "Ljava/lang/Throwable;"))))
    return {
        ExecutionStop::Invalid, 0, {}, "invalid Java continuation exception"};
  if (completion.stop == ExecutionStop::Thrown && completion.throwable) {
    auto object = state->heap->get(completion.throwable);
    if (!object || !object->Initialized ||
        !state->program.assignable(objectDescriptor(*object),
                                   "Ljava/lang/Throwable;") ||
        object->ClassName != completion.exception)
      return {
          ExecutionStop::Invalid, 0, {}, "invalid Java continuation throwable"};
  }
  it->second->completion = std::move(completion);
  return state->advance(id);
}
ExecutionResult Runtime::library(const LibraryMethod &method, uint64_t receiver,
                                 std::span<const uint64_t> args,
                                 const AdmitInstruction &admit,
                                 bool nonvirtual) {
  const auto owner = "L" + std::string(method.Class) + ";";
  if (method.Static && state->program.android_framework_initializers.contains(
                           state->program.findClass(owner))) {
    // These bounded initializers need no DEX stack frames. JNI and DEX use
    // this same class state; a terminal initializer cannot be bypassed here.
    auto result = initialize(owner, admit, 0);
    if (result.stop != ExecutionStop::Returned)
      return result;
  }
  return state->invoke([&] {
    if (!admit)
      throw Halt{{ExecutionStop::Invalid, 0, {}, "missing Java work budget"}};
    auto result =
        executeLibrary(*state->heap, method, receiver, args, admit, nonvirtual,
                       &state->program, &state->framework);
    if (result.stop == ExecutionStop::Suspended)
      result = {ExecutionStop::Unsupported,
                0,
                {},
                "Java library loading requires a DEX caller"};
    if (result.stop != ExecutionStop::Returned)
      throw Halt{std::move(result)};
    return result.value;
  });
}
} // namespace neverd::mobile::dalvik
