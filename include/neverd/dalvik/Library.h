//===- Library.h - Shared bounded Java value methods ------------*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// One intrinsic catalogue and implementation for DEX and JNI calls.
//===----------------------------------------------------------------------===//
#pragma once
#include "neverd/dalvik/Runtime.h"
namespace neverd::mobile::dalvik {
class AndroidFramework;
enum LibraryOperation {
  GetClass,
  IdentityEquals,
  ClassName,
  IsArray,
  Length,
  Empty,
  CharAt,
  Hash,
  Equals,
  ToString,
  Concat,
  Substring,
  Compare,
  StartsWith,
  EndsWith,
  IndexOf,
  GetChars,
  ToChars,
  Construct,
  ValueOf,
  Intern,
  ThrowableMessage,
  ThrowableConstruct,
  ObjectConstruct,
  CurrentActivityThread,
  CurrentApplication,
  ThreadApplication,
  ApplicationContext,
  BaseContext,
  ContextClassLoader,
  PackageName,
  ClassLoaderOf,
  LoaderParent,
  SystemClassLoader,
  LoadClass,
  FilesDirectory,
  FilePath,
  FileName,
  FileParent,
  FileIsAbsolute,
  ThreadInstrumentation,
  LoadNativeLibrary,
  ContextContentResolver,
  BuilderAppendChar,
  BuilderString,
  SecureSettingString,
  SecureSettingIntDefault,
  ContextApplicationInfo,
  ContextPackageManager,
  PackageApplicationInfo,
  PackageApplicationLabel,
  PackageResourcePath,
  PackageInfoQuery,
  LocaleDefault,
  LocaleLanguage,
  LocaleCountry,
  ClassLoaderString,
  MainLooper,
  HandlerPost,
  LooperQueue,
  MessageQueueNext,
  MessageCallback,
  MessageTarget,
  URLOpenConnection,
  StringGetBytes,
  UptimeMillis,
  ElapsedRealtime,
  ElapsedRealtimeNanos,
  PropertyInt,
  MapSize,
  MapIsEmpty,
  MapGet,
  MapPut,
  MapContainsKey,
  MapRemove,
  MapClear,
  StatLoggerTime,
  MapPutAll,
  ServiceGet,
  ServiceCheck,
  ServiceInitCache,
  BinderAllowBlocking,
  BinderQueryLocalInterface,
  ServiceAsBinder,
  StatLoggerRecord,
  EventLogWrite,
  ParcelData,
  BinderGetStrictMode,
  BinderSetStrictMode
};
struct LibraryMethod {
  const char *Class, *Name, *Signature;
  LibraryOperation Op;
  bool Static = false;
  bool Final = false;
  // JNI may resolve nonpublic methods. Direct DEX library dispatch must not
  // turn that permission into public Java access.
  bool Public = true;
};

std::span<const LibraryMethod> libraryMethods();
const LibraryMethod *libraryMethod(const MethodRef &reference, bool is_static);
ExecutionResult
executeLibrary(Heap &heap, const LibraryMethod &method, uint64_t receiver,
               std::span<const uint64_t> args, const AdmitInstruction &admit,
               bool nonvirtual = false, const Program *program = nullptr,
               AndroidFramework *framework = nullptr);
} // namespace neverd::mobile::dalvik
