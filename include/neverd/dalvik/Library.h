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
  ThreadInstrumentation,
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
  FileIsAbsolute
};
struct LibraryMethod {
  const char *Class, *Name, *Signature;
  LibraryOperation Op;
  bool Static = false;
};

std::span<const LibraryMethod> libraryMethods();
const LibraryMethod *libraryMethod(const MethodRef &reference, bool is_static);
ExecutionResult
executeLibrary(Heap &heap, const LibraryMethod &method, uint64_t receiver,
               std::span<const uint64_t> args, const AdmitInstruction &admit,
               bool nonvirtual = false, const Program *program = nullptr,
               AndroidFramework *framework = nullptr);
} // namespace neverd::mobile::dalvik
