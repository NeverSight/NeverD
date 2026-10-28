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

#include "neverd/dalvik/Limits.h"

#include <algorithm>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <vector>
namespace neverd::mobile::dalvik {
namespace {
// Java's named UTF-8 encoder replaces each malformed UTF-16 unit with '?'.
// This is distinct from the WTF-8 representation used to retain DEX literals.
template <typename ChargeUnit, typename EmitByte>
void encodeUTF8(std::span<const uint8_t> bytes, ChargeUnit charge,
                EmitByte emit) {
  auto unit = [&](size_t i) {
    return uint32_t(bytes[i] | uint32_t(bytes[i + 1]) << 8);
  };
  for (size_t i = 0; i < bytes.size(); i += 2) {
    charge();
    uint32_t c = unit(i);
    if (c >= 0xd800 && c <= 0xdbff && i + 2 < bytes.size() &&
        unit(i + 2) >= 0xdc00 && unit(i + 2) <= 0xdfff) {
      charge();
      c = 0x10000 + ((c - 0xd800) << 10) + (unit(i + 2) - 0xdc00);
      i += 2;
    } else if (c >= 0xd800 && c <= 0xdfff) {
      c = '?';
    }
    if (c < 0x80) {
      emit(uint8_t(c));
    } else if (c < 0x800) {
      emit(uint8_t(0xc0 | (c >> 6)));
      emit(uint8_t(0x80 | (c & 0x3f)));
    } else if (c < 0x10000) {
      emit(uint8_t(0xe0 | (c >> 12)));
      emit(uint8_t(0x80 | ((c >> 6) & 0x3f)));
      emit(uint8_t(0x80 | (c & 0x3f)));
    } else {
      emit(uint8_t(0xf0 | (c >> 18)));
      emit(uint8_t(0x80 | ((c >> 12) & 0x3f)));
      emit(uint8_t(0x80 | ((c >> 6) & 0x3f)));
      emit(uint8_t(0x80 | (c & 0x3f)));
    }
  }
}

// This bounded value subset needs no normalization or protocol-handler lookup.
// Other URL forms remain unsupported, including forms that may be valid in
// Java.
bool retainedHTTPURL(std::span<const uint8_t> bytes) {
  if (bytes.size() / 2 > ::neverd::limits::dalvik::kMaxJavaURLCodeUnits ||
      bytes.size() % 2)
    return false;
  std::string text;
  for (size_t i = 0; i < bytes.size(); i += 2) {
    if (bytes[i + 1] || bytes[i] <= 32 || bytes[i] >= 127 || bytes[i] == '\\')
      return false;
    text.push_back(char(bytes[i]));
  }
  size_t begin = text.starts_with("http://")    ? 7
                 : text.starts_with("https://") ? 8
                                                : 0;
  if (!begin)
    return false;
  auto end = text.find_first_of("/?#", begin);
  auto authority = std::string_view(text).substr(
      begin, end == text.npos ? text.size() - begin : end - begin);
  auto colon = authority.find(':');
  auto host = authority.substr(0, colon);
  if (host.empty())
    return false;
  for (char c : host)
    if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
          (c >= '0' && c <= '9') || c == '.' || c == '-'))
      return false;
  if (colon != authority.npos) {
    auto port = authority.substr(colon + 1);
    if (port.empty() || port.size() > 5 || (port.size() > 1 && port[0] == '0'))
      return false;
    unsigned number = 0;
    for (char c : port) {
      if (c < '0' || c > '9')
        return false;
      number = number * 10 + unsigned(c - '0');
    }
    if (number > 65535)
      return false;
  }
  return true;
}
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
    {"java/lang/System", "loadLibrary", "(Ljava/lang/String;)V",
     LoadNativeLibrary, true},
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
    {"android/content/Context", "getContentResolver",
     "()Landroid/content/ContentResolver;", ContextContentResolver},
    {"java/lang/StringBuilder", "<init>", "()V", Construct},
    {"java/lang/StringBuilder", "append", "(C)Ljava/lang/StringBuilder;",
     BuilderAppendChar},
    {"java/lang/StringBuilder", "toString", "()Ljava/lang/String;",
     BuilderString},
    {"android/provider/Settings$Secure", "getString",
     "(Landroid/content/ContentResolver;Ljava/lang/String;)Ljava/lang/String;",
     SecureSettingString, true},
    {"android/provider/Settings$Secure", "getInt",
     "(Landroid/content/ContentResolver;Ljava/lang/String;I)I",
     SecureSettingIntDefault, true},
    {"android/content/Context", "getApplicationInfo",
     "()Landroid/content/pm/ApplicationInfo;", ContextApplicationInfo},
    {"android/content/Context", "getPackageManager",
     "()Landroid/content/pm/PackageManager;", ContextPackageManager},
    {"android/content/pm/PackageManager", "getApplicationInfo",
     "(Ljava/lang/String;I)Landroid/content/pm/ApplicationInfo;",
     PackageApplicationInfo},
    {"android/content/pm/PackageManager", "getApplicationLabel",
     "(Landroid/content/pm/ApplicationInfo;)Ljava/lang/CharSequence;",
     PackageApplicationLabel},
    {"android/content/Context", "getPackageResourcePath",
     "()Ljava/lang/String;", PackageResourcePath},
    {"android/content/pm/PackageManager", "getPackageInfo",
     "(Ljava/lang/String;I)Landroid/content/pm/PackageInfo;", PackageInfoQuery},
    {"dalvik/system/BaseDexClassLoader", "toString", "()Ljava/lang/String;",
     ClassLoaderString},
    {"java/util/Locale", "getDefault", "()Ljava/util/Locale;", LocaleDefault,
     true},
    {"java/util/Locale", "getLanguage", "()Ljava/lang/String;", LocaleLanguage},
    {"java/util/Locale", "getCountry", "()Ljava/lang/String;", LocaleCountry},
    {"android/os/Looper", "getMainLooper", "()Landroid/os/Looper;", MainLooper,
     true},
    {"android/os/Handler", "<init>", "(Landroid/os/Looper;)V", Construct},
    {"android/os/Handler", "post", "(Ljava/lang/Runnable;)Z", HandlerPost,
     false, true},
    {"android/os/Looper", "getQueue", "()Landroid/os/MessageQueue;",
     LooperQueue},
    {"android/os/MessageQueue", "next", "()Landroid/os/Message;",
     MessageQueueNext, false, false, false},
    {"android/os/Message", "getCallback", "()Ljava/lang/Runnable;",
     MessageCallback},
    {"android/os/Message", "getTarget", "()Landroid/os/Handler;",
     MessageTarget},
    {"java/net/URL", "<init>", "(Ljava/lang/String;)V", Construct},
    {"java/net/URL", "toString", "()Ljava/lang/String;", ToString},
    {"java/net/URL", "toExternalForm", "()Ljava/lang/String;", ToString},
    // These overrides must not inherit Object identity or String byte
    // semantics.
    {"java/net/URL", "equals", "(Ljava/lang/Object;)Z", Equals},
    {"java/net/URL", "hashCode", "()I", Hash},
    {"java/net/URL", "openConnection", "()Ljava/net/URLConnection;",
     URLOpenConnection},
    {"java/io/IOException", "<init>", "()V", ThrowableConstruct},
    {"java/io/IOException", "<init>", "(Ljava/lang/String;)V",
     ThrowableConstruct},
    // Append new entries to preserve the existing JNI intrinsic method tokens.
    {"java/lang/String", "getBytes", "(Ljava/lang/String;)[B", StringGetBytes},
    {"android/os/SystemClock", "uptimeMillis", "()J", UptimeMillis, true},
    {"android/os/SystemClock", "elapsedRealtime", "()J", ElapsedRealtime, true},
    {"android/os/SystemClock", "elapsedRealtimeNanos", "()J",
     ElapsedRealtimeNanos, true},
    {"java/util/HashMap", "<init>", "()V", Construct},
    {"java/util/HashMap", "size", "()I", MapSize},
    {"java/util/HashMap", "isEmpty", "()Z", MapIsEmpty},
    {"java/util/HashMap", "get", "(Ljava/lang/Object;)Ljava/lang/Object;",
     MapGet},
    {"java/util/HashMap", "put",
     "(Ljava/lang/Object;Ljava/lang/Object;)Ljava/lang/Object;", MapPut},
    {"java/util/HashMap", "containsKey", "(Ljava/lang/Object;)Z",
     MapContainsKey},
    {"java/util/HashMap", "remove", "(Ljava/lang/Object;)Ljava/lang/Object;",
     MapRemove},
    {"java/util/HashMap", "clear", "()V", MapClear},
    {"java/util/AbstractMap", "equals", "(Ljava/lang/Object;)Z", Equals},
    {"java/util/AbstractMap", "hashCode", "()I", Hash},
    {"java/util/AbstractMap", "toString", "()Ljava/lang/String;", ToString},
    {"android/os/SystemProperties", "getInt", "(Ljava/lang/String;I)I",
     PropertyInt, true},
    {"java/util/Map", "size", "()I", MapSize},
    {"java/util/Map", "isEmpty", "()Z", MapIsEmpty},
    {"java/util/Map", "get", "(Ljava/lang/Object;)Ljava/lang/Object;", MapGet},
    {"java/util/Map", "put",
     "(Ljava/lang/Object;Ljava/lang/Object;)Ljava/lang/Object;", MapPut},
    {"java/util/Map", "containsKey", "(Ljava/lang/Object;)Z", MapContainsKey},
    {"java/util/Map", "remove", "(Ljava/lang/Object;)Ljava/lang/Object;",
     MapRemove},
    {"java/util/Map", "clear", "()V", MapClear},
    {"com/android/internal/util/StatLogger", "<init>", "([Ljava/lang/String;)V",
     Construct},
    {"com/android/internal/util/StatLogger", "getTime", "()J", StatLoggerTime},
    {"com/android/internal/util/StatLogger", "logDurationStat", "(IJ)J",
     StatLoggerRecord},
    {"java/util/HashMap", "putAll", "(Ljava/util/Map;)V", MapPutAll},
    {"java/util/Map", "putAll", "(Ljava/util/Map;)V", MapPutAll},
    {"android/os/ServiceManager", "getService",
     "(Ljava/lang/String;)Landroid/os/IBinder;", ServiceGet, true},
    {"android/os/ServiceManager", "checkService",
     "(Ljava/lang/String;)Landroid/os/IBinder;", ServiceCheck, true},
    {"android/os/ServiceManager", "initServiceCache", "(Ljava/util/Map;)V",
     ServiceInitCache, true},
    {"android/os/Binder", "allowBlocking",
     "(Landroid/os/IBinder;)Landroid/os/IBinder;", BinderAllowBlocking, true},
    {"android/os/IBinder", "queryLocalInterface",
     "(Ljava/lang/String;)Landroid/os/IInterface;", BinderQueryLocalInterface},
    {"android/os/BinderProxy", "queryLocalInterface",
     "(Ljava/lang/String;)Landroid/os/IInterface;", BinderQueryLocalInterface,
     false, false, false},
    {"android/os/IInterface", "asBinder", "()Landroid/os/IBinder;",
     ServiceAsBinder},
    {"android/os/IServiceManager", "asBinder", "()Landroid/os/IBinder;",
     ServiceAsBinder},
    {"android/util/EventLog", "writeEvent", "(II)I", EventLogWrite, true},
    {"android/util/EventLog", "writeEvent", "(IJ)I", EventLogWrite, true},
    {"android/util/EventLog", "writeEvent", "(IF)I", EventLogWrite, true},
    {"android/util/EventLog", "writeEvent", "(ILjava/lang/String;)I",
     EventLogWrite, true},
    {"android/util/EventLog", "writeEvent", "(I[Ljava/lang/Object;)I",
     EventLogWrite, true},
    {"java/lang/Integer", "<init>", "(I)V", Construct},
    {"java/lang/Long", "<init>", "(J)V", Construct},
    {"java/lang/Float", "<init>", "(F)V", Construct},
    {"android/os/Parcel", "obtain", "()Landroid/os/Parcel;", ParcelData, true},
    {"android/os/Parcel", "recycle", "()V", ParcelData, false, true},
    {"android/os/Parcel", "dataSize", "()I", ParcelData, false, true},
    {"android/os/Parcel", "dataPosition", "()I", ParcelData, false, true},
    {"android/os/Parcel", "dataAvail", "()I", ParcelData, false, true},
    {"android/os/Parcel", "setDataPosition", "(I)V", ParcelData, false, true},
    {"android/os/Parcel", "writeInt", "(I)V", ParcelData, false, true},
    {"android/os/Parcel", "readInt", "()I", ParcelData, false, true},
    {"android/os/Parcel", "writeLong", "(J)V", ParcelData, false, true},
    {"android/os/Parcel", "readLong", "()J", ParcelData, false, true},
    {"android/os/Parcel", "writeString", "(Ljava/lang/String;)V", ParcelData,
     false, true},
    {"android/os/Parcel", "readString", "()Ljava/lang/String;", ParcelData,
     false, true},
    {"android/os/Parcel", "marshall", "()[B", ParcelData, false, true},
    {"android/os/Parcel", "unmarshall", "([BII)V", ParcelData, false, true},
    {"android/os/Parcel", "writeInterfaceToken", "(Ljava/lang/String;)V",
     ParcelData, false, true},
    {"android/os/Binder", "getThreadStrictModePolicy", "()I",
     BinderGetStrictMode, true, true},
    {"android/os/Binder", "setThreadStrictModePolicy", "(I)V",
     BinderSetStrictMode, true, true},
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
    if (M.Op == LoadNativeLibrary) {
      auto name = object(Values[0]);
      if (!name)
        return Throw("java/lang/NullPointerException");
      if (name->Bytes.size() >
          ::neverd::limits::dalvik::kLibraryExecuteLibraryNameBytesSizeLimit)
        return {ExecutionStop::Unsupported,
                0,
                {},
                "Java library name exceeds its budget"};
      std::string text;
      for (size_t i = 0; i < name->Bytes.size(); i += 2) {
        tick();
        if (name->Bytes[i + 1] || !name->Bytes[i] || name->Bytes[i] > 127)
          return {
              ExecutionStop::Unsupported, 0, {}, "non-ASCII Java library name"};
        text += char(name->Bytes[i]);
      }
      if (text.find('/') != text.npos)
        return Throw("java/lang/UnsatisfiedLinkError");
      ExecutionResult result;
      result.stop = ExecutionStop::Suspended;
      result.external = ExternalCall{};
      result.external->kind = ExternalCall::LoadLibrary;
      result.external->library = std::move(text);
      return result;
    }
    if ((M.Op >= CurrentActivityThread && M.Op <= ThreadInstrumentation) ||
        M.Op == ContextContentResolver || M.Op == SecureSettingString ||
        M.Op == SecureSettingIntDefault || M.Op == ContextApplicationInfo ||
        M.Op == ContextPackageManager || M.Op == PackageApplicationInfo ||
        M.Op == PackageApplicationLabel || M.Op == PackageResourcePath ||
        M.Op == PackageInfoQuery || M.Op == LocaleDefault ||
        M.Op == LocaleLanguage || M.Op == LocaleCountry ||
        M.Op == ClassLoaderString || M.Op == MainLooper ||
        M.Op == HandlerPost || M.Op == LooperQueue ||
        M.Op == MessageQueueNext || M.Op == MessageCallback ||
        M.Op == MessageTarget || M.Op == UptimeMillis ||
        M.Op == ElapsedRealtime || M.Op == ElapsedRealtimeNanos ||
        M.Op == PropertyInt || M.Op == StatLoggerTime ||
        M.Op == StatLoggerRecord || M.Op == ServiceGet ||
        M.Op == ServiceCheck || M.Op == ServiceInitCache ||
        M.Op == BinderAllowBlocking || M.Op == BinderQueryLocalInterface ||
        M.Op == ServiceAsBinder || M.Op == EventLogWrite ||
        M.Op == ParcelData || M.Op == BinderGetStrictMode ||
        M.Op == BinderSetStrictMode ||
        (M.Op == Construct &&
         (std::string_view(M.Class) == "android/os/Handler" ||
          std::string_view(M.Class) ==
              "com/android/internal/util/StatLogger"))) {
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
    if (M.Op == Construct &&
        (std::string_view(M.Class) == "java/lang/Integer" ||
         std::string_view(M.Class) == "java/lang/Long" ||
         std::string_view(M.Class) == "java/lang/Float")) {
      if (!program)
        return {ExecutionStop::Unsupported,
                0,
                {},
                "Java boxed value declaration is not supplied"};
      const auto *field = program->findField(
          {"L" + std::string(M.Class) + ";", "value", Types[0]}, false);
      if (!field)
        return {ExecutionStop::Unsupported,
                0,
                {},
                "Java boxed value field is not supplied"};
      if (ObjectValue &&
          (ObjectValue->Type != Object::Instance ||
           ObjectValue->ClassName != M.Class || ObjectValue->Initialized ||
           !ObjectValue->Fields.contains(field)))
        return failure("Java boxed value constructor receiver mismatch");
      tick();
      auto result =
          ObjectValue
              ? ObjectValue
              : heap.allocate(
                    Object::Instance, M.Class, 0,
                    ::neverd::limits::dalvik::kInstanceFieldStorageBytes);
      if (!result)
        return Ref(result);
      result->Fields[field] = Types[0] == "J" ? Values[0] : uint32_t(Values[0]);
      return Ref(result);
    }
    if (ObjectValue &&
        (ObjectValue->ClassName == "java/lang/Integer" ||
         ObjectValue->ClassName == "java/lang/Long" ||
         ObjectValue->ClassName == "java/lang/Float") &&
        !Nonvirtual &&
        (M.Op == IdentityEquals || M.Op == Hash || M.Op == ToString))
      return {ExecutionStop::Unsupported,
              0,
              {},
              "Java boxed value comparison and formatting are not modeled"};
    if (std::string_view(M.Class) == "java/net/URL") {
      if (ObjectValue && (ObjectValue->Type != Object::Instance ||
                          ObjectValue->ClassName != M.Class))
        return failure("Java URL receiver type mismatch");
      if (M.Op == Construct) {
        if (ObjectValue && ObjectValue->Initialized)
          return failure("Java URL receiver is already initialized");
        auto spec = object(Values[0]);
        if (!spec)
          return {ExecutionStop::Unsupported,
                  0,
                  {},
                  "Java URL null input exception state is unmodeled"};
        work(spec->Bytes.size());
        if (!retainedHTTPURL(spec->Bytes))
          return {ExecutionStop::Unsupported,
                  0,
                  {},
                  "Java URL form is outside the bounded HTTP value contract"};
        auto o = ObjectValue ? ObjectValue
                             : heap.allocate(Object::Instance, M.Class,
                                             spec->Bytes.size());
        if (!o || !heap.replaceBytes(*o, spec->Bytes))
          return Ref(std::shared_ptr<Object>());
        return Ref(o);
      }
      if (M.Op == ToString)
        return Make(ObjectValue->Bytes);
      if (M.Op == URLOpenConnection) {
        work(ObjectValue->Bytes.size());
        if (!retainedHTTPURL(ObjectValue->Bytes))
          return failure("Java URL receiver has no retained HTTP value");
        if (!framework)
          return {ExecutionStop::Unsupported,
                  0,
                  {},
                  "Java URL.openConnection handler state is not supplied"};
        return framework->openURL(
            heap, ObjectValue->Bytes[8] == ':' ? "http" : "https", admit);
      }
      return {ExecutionStop::Unsupported,
              0,
              {},
              "Java URL handler-dependent equality or hashing is unmodeled"};
    }
    if (M.Op == IdentityEquals && !Nonvirtual && ObjectValue &&
        ObjectValue->ClassName == "java/net/URL")
      return {ExecutionStop::Unsupported,
              0,
              {},
              "Java URL handler-dependent equality or hashing is unmodeled"};
    if (std::string_view(M.Class) == "java/util/AbstractMap" ||
        (M.Op == IdentityEquals && !Nonvirtual && ObjectValue &&
         assignable(objectDescriptor(*ObjectValue), "Ljava/util/AbstractMap;")))
      return {ExecutionStop::Unsupported,
              0,
              {},
              "Java Map equality, hashing and string conversion are unmodeled"};
    if (std::string_view(M.Class) == "java/util/HashMap" ||
        std::string_view(M.Class) == "java/util/Map") {
      if (ObjectValue && ObjectValue->ClassName != "java/util/HashMap")
        return {ExecutionStop::Unsupported,
                0,
                {},
                "Java HashMap subclass behavior is unmodeled"};
      if (M.Op == Construct) {
        if (ObjectValue &&
            (ObjectValue->Initialized || ObjectValue->MapEntries))
          return failure("Java HashMap receiver is already constructed");
        auto o = ObjectValue ? ObjectValue
                             : heap.allocate(Object::Instance, M.Class, 0);
        if (!o || !heap.replaceMapEntries(*o, {}))
          return Ref(std::shared_ptr<Object>());
        return Ref(o);
      }
      if (!ObjectValue->MapEntries)
        return {ExecutionStop::Unsupported,
                0,
                {},
                "Java HashMap contents are not supplied"};
      if (M.Op == MapPutAll) {
        const auto source = object(Values[0]);
        if (!source)
          return Throw("java/lang/NullPointerException");
        if (source->ClassName != "java/util/HashMap" || !source->MapEntries)
          return {ExecutionStop::Unsupported,
                  0,
                  {},
                  "Java putAll requires known HashMap contents"};
        // Snapshot before mutation so putAll(this) keeps stable traversal.
        for (size_t i = 0; i < source->MapEntries->size(); ++i)
          tick();
        const auto copy = *source->MapEntries;
        const auto *put =
            libraryMethod({"Ljava/util/HashMap;",
                           "put",
                           {"Ljava/lang/Object;", "Ljava/lang/Object;"},
                           "Ljava/lang/Object;"},
                          false);
        for (const auto &entry : copy) {
          const uint64_t args[] = {entry.Key, entry.Value};
          auto result = executeLibrary(heap, *put, receiver, args, admit, false,
                                       program, framework);
          if (result.stop != ExecutionStop::Returned)
            return result;
        }
        return value(0);
      }
      const auto &entries = *ObjectValue->MapEntries;
      if (M.Op == MapSize)
        return value(entries.size());
      if (M.Op == MapIsEmpty)
        return value(entries.empty());
      if (M.Op == MapClear) {
        for (size_t i = 0; i < entries.size(); ++i)
          tick();
        heap.replaceMapEntries(*ObjectValue, {});
        return value(0);
      }
      const auto key = object(Values[0]);
      if (key && key->Type != Object::String)
        return {ExecutionStop::Unsupported,
                0,
                {},
                "Java HashMap supports only String and null keys"};
      size_t found = entries.size();
      for (size_t i = 0; i < entries.size(); ++i) {
        tick();
        const auto stored = heap.get(entries[i].Key);
        if (key && stored)
          work(key->Bytes.size());
        if (key == stored || (key && stored && key->Bytes == stored->Bytes)) {
          found = i;
          break;
        }
      }
      const bool present = found != entries.size();
      const uint32_t previous = present ? entries[found].Value : 0;
      if (M.Op == MapGet)
        return value(previous);
      if (M.Op == MapContainsKey)
        return value(present);
      // Admit traversal and the complete retained copy before mutation.
      for (size_t i = 0; i < entries.size(); ++i)
        tick();
      auto changed = entries;
      if (M.Op == MapPut) {
        if (present)
          changed[found].Value = uint32_t(Values[1]);
        else
          changed.push_back({uint32_t(Values[0]), uint32_t(Values[1])});
      } else if (M.Op == MapRemove) {
        if (present)
          changed.erase(changed.begin() + found);
      } else {
        return failure("unknown Java HashMap operation");
      }
      if (!heap.replaceMapEntries(*ObjectValue, std::move(changed)))
        return Ref(std::shared_ptr<Object>());
      return value(previous);
    }
    if (std::string_view(M.Class) == "java/lang/StringBuilder") {
      // Constructors share the existing DEX/JNI constructor operation. The
      // mutable UTF-16 value stays in the same retained workload object.
      if (ObjectValue && (ObjectValue->Type != Object::Instance ||
                          ObjectValue->ClassName != M.Class))
        return failure("Java StringBuilder receiver type mismatch");
      if (M.Op == Construct) {
        if (ObjectValue && ObjectValue->Initialized)
          return failure("Java StringBuilder receiver is already initialized");
        auto o = ObjectValue ? ObjectValue
                             : heap.allocate(Object::Instance, M.Class, 0);
        if (!o || !heap.replaceBytes(*o, {}))
          return Ref(std::shared_ptr<Object>());
        return Ref(o);
      }
      if (M.Op == BuilderAppendChar) {
        // Charge the complete copy before publishing any mutation.
        work(ObjectValue->Bytes.size() + 2);
        auto bytes = ObjectValue->Bytes;
        bytes.push_back(uint8_t(Values[0]));
        bytes.push_back(uint8_t(Values[0] >> 8));
        if (!heap.replaceBytes(*ObjectValue, std::move(bytes)))
          return Ref(std::shared_ptr<Object>());
        return Ref(ObjectValue);
      }
      if (M.Op == BuilderString) {
        // API 28 returns the interned empty literal; a nonempty result is a
        // separate immutable String, unaffected by later appends.
        return ObjectValue->Bytes.empty() ? Ref(heap.literal(""))
                                          : Make(ObjectValue->Bytes);
      }
      return failure("unknown Java StringBuilder operation");
    }
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
    if (M.Op == StringGetBytes) {
      auto charset = object(Values[0]);
      if (!charset)
        return Throw("java/lang/NullPointerException");
      if (ObjectValue->Type != Object::String || Bytes.size() % 2 ||
          ObjectValue->Length != Bytes.size() / 2 ||
          charset->Type != Object::String || charset->Bytes.size() % 2 ||
          charset->Length != charset->Bytes.size() / 2)
        return failure("Java UTF-8 encoder string storage is malformed");
      constexpr std::string_view name = "utf-8";
      bool supported = charset->Length == name.size();
      if (supported)
        for (size_t i = 0; i < name.size(); ++i) {
          tick();
          auto c = Unit(charset->Bytes, i);
          if (c >= 'A' && c <= 'Z')
            c += 'a' - 'A';
          supported &= c == uint8_t(name[i]);
        }
      if (!supported)
        return {ExecutionStop::Unsupported,
                0,
                {},
                "Java String.getBytes charset is unmodeled"};
      // Account for both passes before allocating or publishing any result.
      // The retained heap admits the exact byte count, including empty arrays.
      uint64_t size = 0;
      encodeUTF8(
          Bytes,
          [&] {
            tick();
            tick();
          },
          [&](uint8_t) { ++size; });
      auto result = heap.allocate(Object::PrimitiveArray, "[B", size);
      if (!result)
        return Ref(result);
      size_t offset = 0;
      encodeUTF8(
          Bytes, [] {}, [&](uint8_t byte) { result->Bytes[offset++] = byte; });
      result->Length = size;
      result->ElementSize = 1;
      return Ref(result);
    }
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
        uint64_t(ObjectValue->Length - Offset) >
            ::neverd::limits::dalvik::kMaxJNIStringSearchWork / Right.size())
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
