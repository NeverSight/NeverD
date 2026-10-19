//===- Library.cpp - Shared bounded Java value semantics -----------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// UTF-16 value operations over the workload heap, independent of JNI handles.
//===----------------------------------------------------------------------===//
#include "neverd/dalvik/Library.h"

#include "AndroidFramework.h"

#include <algorithm>
#include <bit>
namespace neverd::mobile::dalvik {
namespace {
// These classes have no observable class initializer in this profile.
static const LibraryMethod Methods[] = {
    {"java/lang/String", "intern", "()Ljava/lang/String;", Intern},
    {"java/lang/Throwable", "getMessage", "()Ljava/lang/String;",
     ThrowableMessage},
    {"java/lang/Object", "<init>", "()V", ObjectConstruct},
    {"java/lang/Object", "getClass", "()Ljava/lang/Class;", GetClass},
    {"java/lang/Object", "equals", "(Ljava/lang/Object;)Z", IdentityEquals},
    {"java/lang/Class", "getName", "()Ljava/lang/String;", ClassName},
    {"java/lang/Class", "isArray", "()Z", IsArray},
    {"java/lang/String", "length", "()I", Length},
    {"java/lang/String", "isEmpty", "()Z", Empty},
    {"java/lang/String", "charAt", "(I)C", CharAt},
    {"java/lang/String", "hashCode", "()I", Hash},
    {"java/lang/String", "equals", "(Ljava/lang/Object;)Z", Equals},
    {"java/lang/String", "toString", "()Ljava/lang/String;", ToString},
    {"java/lang/String", "concat", "(Ljava/lang/String;)Ljava/lang/String;",
     Concat},
    {"java/lang/String", "substring", "(I)Ljava/lang/String;", Substring},
    {"java/lang/String", "substring", "(II)Ljava/lang/String;", Substring},
    {"java/lang/String", "compareTo", "(Ljava/lang/String;)I", Compare},
    {"java/lang/String", "startsWith", "(Ljava/lang/String;)Z", StartsWith},
    {"java/lang/String", "startsWith", "(Ljava/lang/String;I)Z", StartsWith},
    {"java/lang/String", "endsWith", "(Ljava/lang/String;)Z", EndsWith},
    {"java/lang/String", "indexOf", "(Ljava/lang/String;)I", IndexOf},
    {"java/lang/String", "indexOf", "(Ljava/lang/String;I)I", IndexOf},
    {"java/lang/String", "getChars", "(II[CI)V", GetChars},
    {"java/lang/String", "toCharArray", "()[C", ToChars},
    {"java/lang/String", "<init>", "()V", Construct},
    {"java/lang/String", "<init>", "(Ljava/lang/String;)V", Construct},
    {"java/lang/String", "<init>", "([C)V", Construct},
    {"java/lang/String", "<init>", "([CII)V", Construct},
    {"java/lang/String", "valueOf", "(I)Ljava/lang/String;", ValueOf, true},
    {"java/lang/String", "valueOf", "(J)Ljava/lang/String;", ValueOf, true},
    {"java/lang/String", "valueOf", "(Z)Ljava/lang/String;", ValueOf, true},
    {"java/lang/String", "valueOf", "(C)Ljava/lang/String;", ValueOf, true},
    {"java/lang/String", "valueOf", "([C)Ljava/lang/String;", ValueOf, true},
    {"java/lang/Throwable", "<init>", "()V", ThrowableConstruct},
    {"java/lang/Throwable", "<init>", "(Ljava/lang/String;)V",
     ThrowableConstruct},
    {"java/lang/Exception", "<init>", "()V", ThrowableConstruct},
    {"java/lang/Exception", "<init>", "(Ljava/lang/String;)V",
     ThrowableConstruct},
    {"java/lang/RuntimeException", "<init>", "()V", ThrowableConstruct},
    {"java/lang/RuntimeException", "<init>", "(Ljava/lang/String;)V",
     ThrowableConstruct},
    {"java/lang/ArithmeticException", "<init>", "()V", ThrowableConstruct},
    {"java/lang/ArithmeticException", "<init>", "(Ljava/lang/String;)V",
     ThrowableConstruct},
    {"java/lang/NullPointerException", "<init>", "()V", ThrowableConstruct},
    {"java/lang/NullPointerException", "<init>", "(Ljava/lang/String;)V",
     ThrowableConstruct},
    {"java/lang/IllegalArgumentException", "<init>", "()V", ThrowableConstruct},
    {"java/lang/IllegalArgumentException", "<init>", "(Ljava/lang/String;)V",
     ThrowableConstruct},
    {"java/lang/IndexOutOfBoundsException", "<init>", "()V",
     ThrowableConstruct},
    {"java/lang/IndexOutOfBoundsException", "<init>", "(Ljava/lang/String;)V",
     ThrowableConstruct},
    {"java/lang/ArrayIndexOutOfBoundsException", "<init>", "()V",
     ThrowableConstruct},
    {"java/lang/ArrayIndexOutOfBoundsException", "<init>",
     "(Ljava/lang/String;)V", ThrowableConstruct},
    {"java/lang/StringIndexOutOfBoundsException", "<init>", "()V",
     ThrowableConstruct},
    {"java/lang/StringIndexOutOfBoundsException", "<init>",
     "(Ljava/lang/String;)V", ThrowableConstruct},
    {"java/lang/NegativeArraySizeException", "<init>", "()V",
     ThrowableConstruct},
    {"java/lang/NegativeArraySizeException", "<init>", "(Ljava/lang/String;)V",
     ThrowableConstruct},
    {"java/lang/ArrayStoreException", "<init>", "()V", ThrowableConstruct},
    {"java/lang/ArrayStoreException", "<init>", "(Ljava/lang/String;)V",
     ThrowableConstruct},
    {"java/lang/ClassCastException", "<init>", "()V", ThrowableConstruct},
    {"java/lang/ClassCastException", "<init>", "(Ljava/lang/String;)V",
     ThrowableConstruct},
    {"java/lang/Error", "<init>", "()V", ThrowableConstruct},
    {"java/lang/Error", "<init>", "(Ljava/lang/String;)V", ThrowableConstruct},
    {"java/lang/LinkageError", "<init>", "()V", ThrowableConstruct},
    {"java/lang/LinkageError", "<init>", "(Ljava/lang/String;)V",
     ThrowableConstruct},
    {"java/lang/IncompatibleClassChangeError", "<init>", "()V",
     ThrowableConstruct},
    {"java/lang/IncompatibleClassChangeError", "<init>",
     "(Ljava/lang/String;)V", ThrowableConstruct},
    {"java/lang/NoSuchMethodError", "<init>", "()V", ThrowableConstruct},
    {"java/lang/NoSuchMethodError", "<init>", "(Ljava/lang/String;)V",
     ThrowableConstruct},
    {"java/lang/ReflectiveOperationException", "<init>", "()V",
     ThrowableConstruct},
    {"java/lang/ReflectiveOperationException", "<init>",
     "(Ljava/lang/String;)V", ThrowableConstruct},
    {"java/lang/ClassNotFoundException", "<init>", "()V", ThrowableConstruct},
    {"java/lang/ClassNotFoundException", "<init>", "(Ljava/lang/String;)V",
     ThrowableConstruct},
    {"java/lang/NoClassDefFoundError", "<init>", "()V", ThrowableConstruct},
    {"java/lang/NoClassDefFoundError", "<init>", "(Ljava/lang/String;)V",
     ThrowableConstruct},
    {"android/app/ActivityThread", "currentActivityThread",
     "()Landroid/app/ActivityThread;", CurrentActivityThread, true},
    {"android/app/ActivityThread", "currentApplication",
     "()Landroid/app/Application;", CurrentApplication, true},
    {"android/app/ActivityThread", "getApplication",
     "()Landroid/app/Application;", ThreadApplication},
    {"android/content/Context", "getApplicationContext",
     "()Landroid/content/Context;", ApplicationContext},
    {"android/content/ContextWrapper", "getBaseContext",
     "()Landroid/content/Context;", BaseContext},
    {"android/content/Context", "getClassLoader", "()Ljava/lang/ClassLoader;",
     ContextClassLoader},
    {"android/content/Context", "getPackageName", "()Ljava/lang/String;",
     PackageName},
    {"java/lang/Class", "getClassLoader", "()Ljava/lang/ClassLoader;",
     ClassLoaderOf},
    {"java/lang/ClassLoader", "getParent", "()Ljava/lang/ClassLoader;",
     LoaderParent},
    {"java/lang/ClassLoader", "getSystemClassLoader",
     "()Ljava/lang/ClassLoader;", SystemClassLoader, true},
    {"java/lang/ClassLoader", "loadClass",
     "(Ljava/lang/String;)Ljava/lang/Class;", LoadClass},
    {"android/content/Context", "getFilesDir", "()Ljava/io/File;",
     FilesDirectory},
    {"java/io/File", "getPath", "()Ljava/lang/String;", FilePath},
    {"java/io/File", "getAbsolutePath", "()Ljava/lang/String;", FilePath},
    {"java/io/File", "toString", "()Ljava/lang/String;", FilePath},
    {"java/io/File", "getName", "()Ljava/lang/String;", FileName},
    {"java/io/File", "getParent", "()Ljava/lang/String;", FileParent},
    {"java/io/File", "isAbsolute", "()Z", FileIsAbsolute},
    // JNI derives opaque method IDs from this catalog. Append new entries to
    // preserve existing identities in deterministic execution reports.
    {"android/app/ActivityThread", "getInstrumentation",
     "()Landroid/app/Instrumentation;", ThreadInstrumentation},
};

struct LibraryStop {
  ExecutionResult result;
};
} // namespace
std::span<const LibraryMethod> libraryMethods() { return Methods; }
const LibraryMethod *libraryMethod(const MethodRef &reference, bool is_static) {
  for (const auto &m : Methods)
    if (reference.owner == "L" + std::string(m.Class) + ";" &&
        reference.name == m.Name && reference.signature() == m.Signature &&
        m.Static == is_static)
      return &m;
  return nullptr;
}
ExecutionResult executeLibrary(Heap &heap, const LibraryMethod &M,
                               uint64_t receiver,
                               std::span<const uint64_t> Values,
                               const AdmitInstruction &admit, bool Nonvirtual,
                               const Program *program,
                               AndroidFramework *framework) {
  auto value = [](uint64_t v) {
    return ExecutionResult{ExecutionStop::Returned, v, {}, {}};
  };
  auto failure = [](std::string text) -> ExecutionResult {
    throw LibraryStop{{ExecutionStop::Invalid, 0, {}, std::move(text)}};
  };
  auto Throw = [](std::string type) -> ExecutionResult {
    return {ExecutionStop::Thrown, 0, std::move(type),
            "Java library exception"};
  };
  auto tick = [&] {
    auto stop = admit();
    if (stop != ExecutionStop::Returned)
      throw LibraryStop{{stop, 0, {}, "Java library work budget exhausted"}};
  };
  auto work = [&](size_t bytes) {
    for (size_t i = 0; i < bytes; i += 2)
      tick();
  };
  auto assignable = [&](std::string_view source, std::string_view target) {
    return program ? program->assignable(source, target)
                   : referenceAssignable(source, target);
  };
  auto object = [&](uint64_t id) {
    auto o = heap.get(id);
    if (id && !o)
      failure("Java library object identity is invalid");
    return o;
  };
  auto Ref = [&](std::shared_ptr<Object> o) {
    if (!o)
      throw LibraryStop{{ExecutionStop::HeapLimit,
                         0,
                         {},
                         "Java library storage budget exhausted"}};
    return value(o->Identity);
  };
  try {
    tick();
    const std::string_view Signature = M.Signature;
    const auto [Types, Return] = prototype(Signature);
    if (Values.size() != Types.size())
      return failure("Java library argument count mismatch");
    auto ObjectValue = object(receiver);
    const bool Constructor = M.Op == Construct || M.Op == ThrowableConstruct ||
                             M.Op == ObjectConstruct;
    if (!M.Static && !Constructor && !ObjectValue)
      return Throw("java/lang/NullPointerException");
    if (ObjectValue && !ObjectValue->Initialized && !Constructor)
      return failure("Java library uninitialized receiver");
    if (ObjectValue && !Constructor &&
        !assignable(objectDescriptor(*ObjectValue),
                    "L" + std::string(M.Class) + ";"))
      return failure("Java library receiver type mismatch");
    for (size_t i = 0; i < Types.size(); ++i)
      if (referenceType(Types[i])) {
        auto o = object(Values[i]);
        if (o &&
            (!o->Initialized || !assignable(objectDescriptor(*o), Types[i])))
          return failure("Java library reference parameter type mismatch");
      }
    if (M.Op >= CurrentActivityThread && M.Op <= ThreadInstrumentation) {
      if (!program || !framework)
        return {ExecutionStop::Unsupported,
                0,
                {},
                "Android framework state is not supplied"};
      return framework->execute(heap, *program, M, receiver, Values, admit);
    }
    auto Make = [&](std::span<const uint8_t> bytes, bool array = false) {
      work(bytes.size());
      auto o =
          M.Op == Construct && ObjectValue
              ? ObjectValue
              : heap.allocate(array ? Object::PrimitiveArray : Object::String,
                              array ? "[C" : "java/lang/String", bytes.size());
      if (!o)
        return Ref(o);
      if (!heap.replaceBytes(*o, {bytes.begin(), bytes.end()}))
        return Ref(std::shared_ptr<Object>());
      if (array)
        o->ElementSize = 2;
      return Ref(o);
    };
    auto Cached = [&](const std::string &key, std::span<const uint8_t> bytes) {
      work(bytes.size());
      return Ref(heap.stringConstant(key, {bytes.begin(), bytes.end()}));
    };
    const std::vector<uint8_t> EmptyBytes;
    if (M.Op == ObjectConstruct)
      return receiver ? value(receiver)
                      : Ref(heap.allocate(Object::Instance, M.Class, 0));
    if (M.Op == ThrowableConstruct) {
      auto o = ObjectValue ? ObjectValue
                           : heap.allocate(Object::Throwable, M.Class, 0);
      if (!o)
        return Ref(o);
      o->Message = Values.empty() ? 0 : Values[0];
      return Ref(o);
    }
    if (M.Op == ThrowableMessage)
      return value(ObjectValue->Message);
    if (M.Op == Intern) {
      work(ObjectValue->Bytes.size());
      return Ref(heap.intern(ObjectValue));
    }
    const auto &Bytes = ObjectValue ? ObjectValue->Bytes : EmptyBytes;
    auto Unit = [](std::span<const uint8_t> B, size_t I) {
      return uint16_t(B[I * 2] | uint16_t(B[I * 2 + 1]) << 8);
    };
    if (M.Op == GetClass) {
      auto Class = heap.classObject((ObjectValue->Type == Object::Class
                                         ? std::string("java/lang/Class")
                                         : ObjectValue->ClassName));
      return Ref(Class);
    }
    if (M.Op == ClassName) {
      auto Name = ObjectValue->ClassName;
      std::replace(Name.begin(), Name.end(), '/', '.');
      return Cached("class:" + ObjectValue->ClassName, javaStringBytes(Name));
    }
    if (M.Op == IsArray)
      return value(ObjectValue->ClassName.starts_with("["));
    if (M.Op == Length || M.Op == Empty)
      return value(M.Op == Length ? ObjectValue->Length
                                  : ObjectValue->Length == 0);
    if (M.Op == ToString)
      return Ref(ObjectValue);
    if (M.Op == ToChars)
      return Make(Bytes, true);
    if (M.Op == Hash) {
      uint32_t Hash = 0;
      for (size_t I = 0; I < ObjectValue->Length; ++I) {
        tick();
        Hash = Hash * 31 + Unit(Bytes, I);
      }
      return value(Hash);
    }
    if (M.Op == CharAt) {
      if (uint32_t(Values[0]) >= ObjectValue->Length)
        return Throw("java/lang/StringIndexOutOfBoundsException");
      return value(Unit(Bytes, uint32_t(Values[0])));
    }
    if (M.Op == Substring) {
      const uint32_t Begin = Values[0];
      const uint32_t End = Values.size() == 2 ? Values[1] : ObjectValue->Length;
      if (Begin > End || End > ObjectValue->Length)
        return Throw("java/lang/StringIndexOutOfBoundsException");
      if (Begin == 0 && End == ObjectValue->Length)
        return Ref(ObjectValue);
      return Make(std::span(Bytes).subspan(Begin * 2, (End - Begin) * 2));
    }
    if (M.Op == ValueOf && Types[0] != "[C") {
      if (Types[0] == "C") {
        uint8_t B[2];
        B[0] = uint8_t(Values[0]);
        B[1] = uint8_t(Values[0] >> 8);
        return Make(B);
      }
      std::string Text =
          Types[0] == "Z" ? (Values[0] ? "true" : "false")
          : Types[0] == "J"
              ? std::to_string(std::bit_cast<int64_t>(Values[0]))
              : std::to_string(std::bit_cast<int32_t>(uint32_t(Values[0])));
      std::vector<uint8_t> B;
      for (char C : Text) {
        B.push_back(C);
        B.push_back(0);
      }
      if (Types[0] == "Z" ||
          (Types[0] == "I" &&
           std::bit_cast<int32_t>(uint32_t(Values[0])) == INT32_MIN) ||
          (Types[0] == "J" && std::bit_cast<int64_t>(Values[0]) == INT64_MIN))
        return Ref(heap.literal(Text));
      // Android libcore's Integer.toString shares the -99..99 value strings;
      // Long.toString and other integer values allocate independent strings.
      if (Types[0] == "I" &&
          std::bit_cast<int32_t>(uint32_t(Values[0])) > -100 &&
          std::bit_cast<int32_t>(uint32_t(Values[0])) < 100)
        return Cached("integer:" + Text, B);
      return Make(B);
    }
    if (M.Op == Construct && Values.empty())
      return Make({});
    // Resolve reference arguments before any object/array mutation.
    auto Other = object(Values[M.Op == GetChars ? 2 : 0]);
    if (M.Op == IdentityEquals &&
        (Nonvirtual || ObjectValue->Type != Object::String))
      return value(ObjectValue == Other);
    if (M.Op == Equals || M.Op == IdentityEquals) {
      work(Bytes.size());
      return value(Other && Other->Type == Object::String &&
                   Bytes == Other->Bytes);
    }
    if (M.Op == GetChars) {
      const uint32_t Begin = Values[0], End = Values[1],
                     Destination = Values[3];
      if (!Other)
        return Throw("java/lang/NullPointerException");
      if (Other->ClassName != "[C")
        return failure("JNI method char array parameter type mismatch");
      if (Begin > End || End > ObjectValue->Length)
        return Throw("java/lang/StringIndexOutOfBoundsException");
      if (Destination > Other->Length ||
          End - Begin > Other->Length - Destination)
        return Throw("java/lang/ArrayIndexOutOfBoundsException");
      work((End - Begin) * 2);
      std::copy(Bytes.begin() + Begin * 2, Bytes.begin() + End * 2,
                Other->Bytes.begin() + Destination * 2);
      return value(0);
    }
    if (!Other)
      return Throw("java/lang/NullPointerException");
    const bool Array = Signature.find("[C") != Signature.npos;
    if ((Array && Other->ClassName != "[C") ||
        (!Array && Other->Type != Object::String))
      return failure("JNI method reference parameter type mismatch");
    const auto &Right = Other->Bytes;
    if (M.Op == Construct || M.Op == ValueOf) {
      const uint32_t Begin = Values.size() == 3 ? Values[1] : 0;
      const uint32_t Count = Values.size() == 3 ? Values[2] : Other->Length;
      if (Begin > Other->Length || Count > Other->Length - Begin)
        return Throw("java/lang/StringIndexOutOfBoundsException");
      return Make(std::span(Right).subspan(Begin * 2, Count * 2));
    }
    if (M.Op == Concat) {
      if (Right.empty())
        return Ref(ObjectValue);

      std::vector<uint8_t> Joined = Bytes;
      Joined.insert(Joined.end(), Right.begin(), Right.end());
      return Make(Joined);
    }
    if (M.Op == Compare) {
      for (size_t I = 0; I < std::min(ObjectValue->Length, Other->Length);
           ++I) {
        tick();
        if (Unit(Bytes, I) != Unit(Right, I))
          return value(
              uint32_t(int32_t(Unit(Bytes, I)) - int32_t(Unit(Right, I))));
      }
      return value(uint32_t(ObjectValue->Length - Other->Length));
    }
    int64_t Offset =
        Values.size() == 2 ? std::bit_cast<int32_t>(uint32_t(Values[1])) : 0;
    if (M.Op == EndsWith)
      Offset = int64_t(ObjectValue->Length) - int64_t(Other->Length);
    auto Match = [&](uint64_t At) {
      work(Right.size());
      return At <= ObjectValue->Length &&
             Other->Length <= ObjectValue->Length - At &&
             std::equal(Right.begin(), Right.end(), Bytes.begin() + At * 2);
    };
    if (M.Op == StartsWith || M.Op == EndsWith)
      return value(Offset >= 0 && Match(Offset));
    if (M.Op != IndexOf)
      return failure("unknown Java library operation");
    Offset = std::clamp<int64_t>(Offset, 0, ObjectValue->Length);
    // Both strings are bounded by the object budget; also bound quadratic
    // search.
    if (Right.size() &&
        uint64_t(ObjectValue->Length - Offset) > 1048576 / Right.size())
      return failure("JNI string search work budget exhausted");
    for (uint64_t I = Offset; I <= ObjectValue->Length; ++I)
      if (Match(I))
        return value(I);
    return value(uint32_t(-1));
  } catch (const LibraryStop &stop) {
    return stop.result;
  }
}
} // namespace neverd::mobile::dalvik
