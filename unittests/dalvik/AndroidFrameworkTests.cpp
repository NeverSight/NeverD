//===- AndroidFrameworkTests.cpp - Application and loader state ----------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
#include "gtest/gtest.h"

#include "neverd/dalvik/Library.h"

namespace neverd::mobile::dalvik {
namespace {
using Phase = AndroidApplicationSnapshot::Phase;
using Properties = std::map<std::string, std::string>;
const AdmitInstruction Admit = [] { return ExecutionStop::Returned; };
const LibraryMethod &operation(LibraryOperation op) {
  for (const auto &method : libraryMethods())
    if (method.Op == op)
      return method;
  std::abort();
}
class AndroidFrameworkTest : public testing::Test {
protected:
  Program program{{}, Properties{}};
  std::shared_ptr<Heap> heap = std::make_shared<Heap>();
  Runtime runtime{program, heap,
                  AndroidApplicationSnapshot{Phase::Application, "test.app"}};
  uint64_t call(LibraryOperation op, uint64_t receiver = 0,
                std::initializer_list<uint64_t> args = {}) {
    auto result = runtime.library(operation(op), receiver, args, Admit);
    EXPECT_EQ(result.stop, ExecutionStop::Returned) << result.diagnostic;
    return result.value;
  }
};
using Instrumentation = AndroidApplicationSnapshot::Instrumentation;
const FieldRef InstrumentationField{"Landroid/app/ActivityThread;",
                                    "mInstrumentation",
                                    "Landroid/app/Instrumentation;"};
TEST_F(AndroidFrameworkTest,
       InstrumentationStateIsIndependentOfApplicationPhase) {
  const auto *field = program.findField(InstrumentationField, false);
  ASSERT_NE(field, nullptr);
  EXPECT_FALSE(has(field->access, "public"));
  EXPECT_EQ(program.findField(InstrumentationField, true), nullptr);
  for (auto phase : {Phase::Attached, Phase::Application}) {
    Runtime unknown(program, heap, AndroidApplicationSnapshot{phase});
    auto thread =
        unknown.library(operation(CurrentActivityThread), 0, {}, Admit);
    auto missing = unknown.field(*field, thread.value, {}, Admit, 65536);
    EXPECT_EQ(missing.stop, ExecutionStop::Unsupported);
    EXPECT_NE(missing.diagnostic.find("instrumentation state"),
              std::string::npos);
    for (auto input :
         {Instrumentation::Uninitialized, Instrumentation::Platform}) {
      AndroidApplicationSnapshot snapshot{phase};
      snapshot.instrumentation = input;
      Runtime state(program, heap, snapshot);
      auto thread =
          state.library(operation(CurrentActivityThread), 0, {}, Admit);
      auto value = state.field(*field, thread.value, {}, Admit, 65536);
      ASSERT_EQ(value.stop, ExecutionStop::Returned) << value.diagnostic;
      EXPECT_EQ(value.value != 0, input == Instrumentation::Platform);
      auto method = state.library(operation(ThreadInstrumentation),
                                  thread.value, {}, Admit);
      EXPECT_EQ(method.stop, ExecutionStop::Returned) << method.diagnostic;
      EXPECT_EQ(method.value, value.value);
      if (value.value)
        EXPECT_EQ(heap->get(value.value)->ClassName,
                  "android/app/Instrumentation");
      auto cleared = state.field(*field, thread.value, 0, Admit, 65536);
      EXPECT_EQ(cleared.stop, ExecutionStop::Returned);
      EXPECT_EQ(state
                    .library(operation(ThreadInstrumentation), thread.value, {},
                             Admit)
                    .value,
                0u);
      EXPECT_EQ(
          state.field(*field, thread.value, value.value, Admit, 65536).stop,
          ExecutionStop::Returned);
      EXPECT_EQ(state
                    .library(operation(ThreadInstrumentation), thread.value, {},
                             Admit)
                    .value,
                value.value);
    }
  }
}
TEST_F(AndroidFrameworkTest,
       ExplicitFieldWritesDoNotNeedAnInventedInitialValue) {
  auto thread = call(CurrentActivityThread);
  const auto *field = program.findField(InstrumentationField, false);
  ASSERT_NE(field, nullptr);
  EXPECT_EQ(runtime.field(*field, thread, 0, Admit, 65536).stop,
            ExecutionStop::Returned);
  EXPECT_EQ(call(ThreadInstrumentation, thread), 0u);
}
TEST_F(AndroidFrameworkTest,
       InstrumentationRejectsForeignReceiversAndWrongValues) {
  const auto *field = program.findField(InstrumentationField, false);
  ASSERT_NE(field, nullptr);
  for (const char *type : {"android/app/ActivityThread", "java/lang/Object"}) {
    Runtime state(program, heap, AndroidApplicationSnapshot{Phase::Attached});
    state.library(operation(CurrentActivityThread), 0, {}, Admit);
    auto foreign = heap->allocate(Object::Instance, type, 0);
    auto result = state.field(*field, foreign->Identity, 0, Admit, 65536);
    EXPECT_EQ(result.stop,
              std::string_view(type) == "android/app/ActivityThread"
                  ? ExecutionStop::Unsupported
                  : ExecutionStop::Invalid);
  }
  auto thread = call(CurrentActivityThread);
  auto wrong = heap->allocate(Object::Instance, "java/lang/Object", 0);
  EXPECT_EQ(runtime.field(*field, thread, wrong->Identity, Admit, 65536).stop,
            ExecutionStop::Invalid);
}
TEST_F(AndroidFrameworkTest,
       InstrumentationHonorsBudgetAndRejectsMalformedSnapshots) {
  for (auto input :
       {Instrumentation::Uninitialized, Instrumentation::Platform}) {
    AndroidApplicationSnapshot snapshot{Phase::Unattached};
    snapshot.instrumentation = input;
    EXPECT_THROW(Runtime(program, heap, snapshot), Error);
  }
  AndroidApplicationSnapshot invalid{Phase::Attached};
  invalid.instrumentation = Instrumentation(9);
  EXPECT_THROW(Runtime(program, heap, invalid), Error);
  AndroidApplicationSnapshot snapshot{Phase::Attached};
  snapshot.instrumentation = Instrumentation::Platform;
  Runtime state(program, heap, snapshot);
  auto thread =
      state.library(operation(CurrentActivityThread), 0, {}, Admit).value;
  const auto *field = program.findField(InstrumentationField, false);
  ASSERT_NE(field, nullptr);
  auto result = state.field(
      *field, thread, {}, [] { return ExecutionStop::Timeout; }, 65536);
  EXPECT_EQ(result.stop, ExecutionStop::Timeout);
  EXPECT_EQ(state.field(*field, thread, {}, Admit, 65536).stop, result.stop);
}
TEST_F(AndroidFrameworkTest,
       DexGetterAndNativeFieldShareMutableSubclassIdentity) {
  Class reader;
  reader.name = "Ltest/InstrumentationReader;";
  reader.superclass = "Ljava/lang/Object;";
  reader.access = {"public"};
  Method method;
  method.reference = {reader.name,
                      "read",
                      {"Landroid/app/ActivityThread;"},
                      "Landroid/app/Instrumentation;"};
  method.access = {"public", "static"};
  method.registers = 1;
  Instruction get;
  get.opcode = "invoke-virtual";
  get.registers = {0};
  get.reference = MethodRef{"Landroid/app/ActivityThread;",
                            "getInstrumentation",
                            {},
                            "Landroid/app/Instrumentation;"};
  Instruction move;
  move.pc = 3;
  move.opcode = "move-result-object";
  move.registers = {0};
  Instruction ret;
  ret.pc = 4;
  ret.opcode = "return-object";
  ret.registers = {0};
  method.instructions = {get, move, ret};
  reader.methods.push_back(method);
  Class custom;
  custom.name = "Ltest/CustomInstrumentation;";
  custom.superclass = "Landroid/app/Instrumentation;";
  custom.access = {"public"};
  Program code({reader, custom}, Properties{});
  Runtime state(code, heap, AndroidApplicationSnapshot{Phase::Attached});
  auto thread =
      state.library(operation(CurrentActivityThread), 0, {}, Admit).value;
  auto replacement = state.allocate(custom.name, true, Admit, 65536);
  ASSERT_EQ(replacement.stop, ExecutionStop::Returned)
      << replacement.diagnostic;
  const auto *field = code.findField(InstrumentationField, false);
  ASSERT_NE(field, nullptr);
  EXPECT_EQ(state.field(*field, thread, replacement.value, Admit, 65536).stop,
            ExecutionStop::Returned);
  const uint64_t args[]{thread};
  auto dex =
      state.execute(*code.findStatic(method.reference), args, Admit, 65536);
  EXPECT_EQ(dex.stop, ExecutionStop::Returned) << dex.diagnostic;
  EXPECT_EQ(dex.value, replacement.value);
  EXPECT_EQ(state.field(*field, thread, {}, Admit, 65536).value, dex.value);
}

TEST_F(AndroidFrameworkTest,
       FrameworkFieldOwnershipDoesNotTrustSourceMetadata) {
  Class ordinary;
  ordinary.name = "Ltest/Ordinary;";
  ordinary.superclass = "Ljava/lang/Object;";
  ordinary.access = {"public"};
  ordinary.source_id = "android-api28-framework";
  Field field;
  field.reference = {ordinary.name, "value", "I"};
  field.access = {"public"};
  ordinary.fields.push_back(field);
  Program code({ordinary}, Properties{});
  const auto *owned = code.findField(field.reference, false);
  ASSERT_NE(owned, nullptr);
  EXPECT_FALSE(code.androidFrameworkField(*owned));
  const auto *platform = code.findField(InstrumentationField, false);
  ASSERT_NE(platform, nullptr);
  EXPECT_TRUE(code.androidFrameworkField(*platform));
  Field copy = *platform;
  EXPECT_FALSE(code.androidFrameworkField(copy));
  Runtime state(code, heap);
  auto object = state.allocate(ordinary.name, true, Admit, 65536);
  ASSERT_EQ(object.stop, ExecutionStop::Returned);
  EXPECT_EQ(state.field(*owned, object.value, 23, Admit, 65536).stop,
            ExecutionStop::Returned);
  EXPECT_EQ(state.field(*owned, object.value, {}, Admit, 65536).value, 23u);
}
TEST_F(AndroidFrameworkTest, InstrumentationAllocationHonorsSharedHeapLimit) {
  auto bounded = std::make_shared<Heap>(65536, 1);
  AndroidApplicationSnapshot snapshot{Phase::Attached};
  snapshot.instrumentation = Instrumentation::Platform;
  Runtime state(program, bounded, snapshot);
  auto thread = state.library(operation(CurrentActivityThread), 0, {}, Admit);
  ASSERT_EQ(thread.stop, ExecutionStop::Returned);
  const auto *field = program.findField(InstrumentationField, false);
  ASSERT_NE(field, nullptr);
  auto result = state.field(*field, thread.value, {}, Admit, 65536);
  EXPECT_EQ(result.stop, ExecutionStop::HeapLimit);
  EXPECT_EQ(
      state.library(operation(ThreadInstrumentation), thread.value, {}, Admit)
          .stop,
      ExecutionStop::HeapLimit);
}

TEST_F(AndroidFrameworkTest, FilesDirectoryRetainsFileAndStringIdentities) {
  for (const char *input :
       {"/data/user/0/example/files", "//data///files/", "/"}) {
    Runtime state(program, heap,
                  AndroidApplicationSnapshot{Phase::Application, {}, input});
    auto invoke = [&](LibraryOperation op, uint64_t receiver = 0) {
      auto r = state.library(operation(op), receiver, {}, Admit);
      EXPECT_EQ(r.stop, ExecutionStop::Returned) << r.diagnostic;
      return r.value;
    };
    auto app = invoke(CurrentApplication);
    auto base = invoke(BaseContext, app);
    auto file = invoke(FilesDirectory, app);
    EXPECT_EQ(invoke(FilesDirectory, base), file);
    EXPECT_EQ(heap->get(file)->ClassName, "java/io/File");
    EXPECT_TRUE(program.assignable("Ljava/io/File;", "Ljava/io/Serializable;"));
    EXPECT_TRUE(program.assignable("Ljava/io/File;", "Ljava/lang/Comparable;"));
    auto path = invoke(FilePath, file);
    EXPECT_EQ(invoke(FilePath, file), path);
    EXPECT_EQ(invoke(FileIsAbsolute, file), 1u);
    std::string expected = input;
    if (expected.starts_with("//"))
      expected = "/data/files";
    EXPECT_EQ(heap->get(path)->Bytes, javaStringBytes(expected));
    const auto name = invoke(FileName, file);
    EXPECT_EQ(heap->get(name)->Bytes,
              javaStringBytes(expected == "/" ? "" : "files"));
    const auto parent = invoke(FileParent, file);
    if (expected == "/")
      EXPECT_EQ(parent, 0u);
    else
      EXPECT_EQ(heap->get(parent)->Bytes,
                javaStringBytes(expected.substr(0, expected.rfind('/'))));
  }
}
TEST_F(AndroidFrameworkTest,
       FilesDirectoryDoesNotManufactureMissingStateOrForeignFiles) {
  auto app = call(CurrentApplication);
  auto missing = runtime.library(operation(FilesDirectory), app, {}, Admit);
  EXPECT_EQ(missing.stop, ExecutionStop::Unsupported);
  EXPECT_NE(missing.diagnostic.find("files directory"), std::string::npos);
  Runtime state(program, heap,
                AndroidApplicationSnapshot{Phase::Application, {}, "/files"});
  auto foreign = heap->allocate(Object::Instance, "java/io/File", 0);
  auto invalid =
      state.library(operation(FilePath), foreign->Identity, {}, Admit);
  EXPECT_EQ(invalid.stop, ExecutionStop::Unsupported);
  Runtime nullable(
      program, heap,
      AndroidApplicationSnapshot{Phase::Application, {}, "/files"});
  EXPECT_EQ(nullable.library(operation(FilePath), 0, {}, Admit).stop,
            ExecutionStop::Thrown);
}
TEST_F(AndroidFrameworkTest, FilesDirectoryValidatesPhaseTextAndBudget) {
  for (const char *path : {"", "relative", "\xff"}) {
    EXPECT_THROW(
        Runtime(program, heap,
                AndroidApplicationSnapshot{Phase::Application, {}, path}),
        Error);
  }
  EXPECT_THROW(
      Runtime(program, heap,
              AndroidApplicationSnapshot{Phase::Attached, {}, "/files"}),
      Error);
  Runtime state(program, heap,
                AndroidApplicationSnapshot{Phase::Application, {}, "/files"});
  auto app = state.library(operation(CurrentApplication), 0, {}, Admit).value;
  unsigned steps = 0;
  auto r = state.library(operation(FilesDirectory), app, {}, [&] {
    return ++steps <= 2 ? ExecutionStop::Returned
                        : ExecutionStop::InstructionLimit;
  });
  EXPECT_EQ(r.stop, ExecutionStop::InstructionLimit);
  EXPECT_EQ(state.library(operation(FilesDirectory), app, {}, Admit).stop,
            r.stop);
}

TEST_F(AndroidFrameworkTest, PhaseIsExplicitAndAnUnknownStopIsTerminal) {
  Runtime unknown(program, heap);
  auto result = unknown.library(operation(CurrentActivityThread), 0, {}, Admit);
  EXPECT_EQ(result.stop, ExecutionStop::Unsupported);
  EXPECT_NE(result.diagnostic.find("snapshot"), std::string::npos);
  EXPECT_EQ(
      unknown.library(operation(SystemClassLoader), 0, {}, Admit).diagnostic,
      result.diagnostic);
  for (auto phase : {Phase::Unattached, Phase::Attached, Phase::Application}) {
    Runtime state(program, heap, AndroidApplicationSnapshot{phase, {}});
    auto thread = state.library(operation(CurrentActivityThread), 0, {}, Admit);
    auto app = state.library(operation(CurrentApplication), 0, {}, Admit);
    ASSERT_EQ(thread.stop, ExecutionStop::Returned);
    ASSERT_EQ(app.stop, ExecutionStop::Returned);
    EXPECT_EQ(thread.value != 0, phase != Phase::Unattached);
    EXPECT_EQ(app.value != 0, phase == Phase::Application);
    if (thread.value) {
      auto instance =
          state.library(operation(ThreadApplication), thread.value, {}, Admit);
      EXPECT_EQ(instance.stop, ExecutionStop::Returned);
      EXPECT_EQ(instance.value, app.value);
    }
  }
}
TEST_F(AndroidFrameworkTest,
       ApplicationAndBaseContextRetainDistinctIdentities) {
  auto thread = call(CurrentActivityThread);
  auto app = call(ThreadApplication, thread);
  auto base = call(BaseContext, app);
  ASSERT_NE(thread, 0u);
  ASSERT_NE(app, 0u);
  ASSERT_NE(base, 0u);
  EXPECT_NE(thread, app);
  EXPECT_NE(app, base);
  EXPECT_EQ(heap->get(app)->ClassName, "android/app/Application");
  EXPECT_EQ(heap->get(base)->ClassName, "android/app/ContextImpl");
  EXPECT_EQ(call(CurrentActivityThread), thread);
  EXPECT_EQ(call(CurrentApplication), app);
  EXPECT_EQ(call(ApplicationContext, app), app);
  EXPECT_EQ(call(ApplicationContext, base), app);
  EXPECT_EQ(call(BaseContext, app), base);
  EXPECT_EQ(call(ContextClassLoader, app), call(ContextClassLoader, base));
  auto package = call(PackageName, app);
  EXPECT_EQ(call(PackageName, base), package);
  EXPECT_EQ(heap->get(package)->Length, 8u);
  EXPECT_EQ(heap->get(package)->Bytes, javaStringBytes("test.app"));
}
TEST_F(AndroidFrameworkTest, FrameworkHierarchyPreservesActualParentEdges) {
  EXPECT_EQ(program.parent("Landroid/app/ActivityThread;"),
            "Landroid/app/ClientTransactionHandler;");
  EXPECT_EQ(program.parent("Landroid/app/Application;"),
            "Landroid/content/ContextWrapper;");
  EXPECT_TRUE(program.assignable("Landroid/app/Application;",
                                 "Landroid/content/ComponentCallbacks;"));
  EXPECT_TRUE(program.assignable("Landroid/app/Application;",
                                 "Landroid/content/Context;"));
  EXPECT_FALSE(program.assignable("Landroid/app/ContextImpl;",
                                  "Landroid/app/Application;"));
  Class impossible;
  impossible.name = "Ltest/BadThread;";
  impossible.superclass = "Landroid/app/ActivityThread;";
  Program invalid({impossible}, Properties{});
  EXPECT_EQ(invalid.unsupportedClass(impossible.name),
            "DEX invalid superclass");
}
TEST_F(AndroidFrameworkTest, Api28HasDistinctBootSystemAndApplicationLoaders) {
  auto app = call(CurrentApplication);
  auto app_loader = call(ContextClassLoader, app);
  auto system_loader = call(SystemClassLoader);
  auto app_class = call(GetClass, app);
  auto boot = call(ClassLoaderOf, app_class);
  EXPECT_NE(app_loader, system_loader);
  EXPECT_NE(app_loader, boot);
  EXPECT_NE(system_loader, boot);
  EXPECT_EQ(heap->get(app_loader)->ClassName, "dalvik/system/PathClassLoader");
  EXPECT_EQ(heap->get(boot)->ClassName, "java/lang/BootClassLoader");
  EXPECT_EQ(call(LoaderParent, app_loader), boot);
  EXPECT_EQ(call(LoaderParent, system_loader), boot);
  EXPECT_EQ(call(LoaderParent, boot), 0u);
  EXPECT_EQ(call(ClassLoaderOf, call(GetClass, app_loader)), boot);
  for (auto loader : {app_loader, system_loader, boot}) {
    auto name = heap->literal("android.app.Application");
    EXPECT_EQ(call(LoadClass, loader, {name->Identity}), app_class);
  }
  EXPECT_EQ(call(ClassLoaderOf, heap->classObject("[I")->Identity), boot);
  EXPECT_EQ(
      call(ClassLoaderOf, heap->classObject("[[Ljava/lang/String;")->Identity),
      boot);
}
TEST_F(AndroidFrameworkTest, DexAndLibraryCallsShareTheApplicationIdentity) {
  Class reader;
  reader.name = "Ltest/Reader;";
  reader.superclass = "Ljava/lang/Object;";
  reader.access = {"public"};
  Method method;
  method.reference = {
      reader.name, "application", {}, "Landroid/app/Application;"};
  method.access = {"public", "static"};
  method.registers = 1;
  Instruction thread;
  thread.opcode = "invoke-static";
  thread.reference = MethodRef{"Landroid/app/ActivityThread;",
                               "currentActivityThread",
                               {},
                               "Landroid/app/ActivityThread;"};
  Instruction move;
  move.pc = 3;
  move.opcode = "move-result-object";
  move.registers = {0};
  Instruction get;
  get.pc = 4;
  get.opcode = "invoke-virtual";
  get.registers = {0};
  get.reference = MethodRef{"Landroid/app/ActivityThread;",
                            "getApplication",
                            {},
                            "Landroid/app/Application;"};
  Instruction ret;
  ret.pc = 8;
  ret.opcode = "return-object";
  ret.registers = {0};
  method.instructions = {thread, move, get, move, ret};
  method.instructions[3].pc = 7;
  reader.methods.push_back(method);
  Program code({reader}, Properties{});
  Runtime state(code, heap, AndroidApplicationSnapshot{Phase::Application, {}});
  auto direct = state.library(operation(CurrentApplication), 0, {}, Admit);
  for (unsigned i = 0; i != 2; ++i) {
    auto dex =
        state.execute(*code.findStatic(method.reference), {}, Admit, 65536);
    EXPECT_EQ(dex.stop, ExecutionStop::Returned) << dex.diagnostic;
    EXPECT_EQ(dex.value, direct.value);
  }
}
TEST_F(AndroidFrameworkTest, DexAndNativeFileAccessShareTheSameObjects) {
  Class reader;
  reader.name = "Ltest/FileReader;";
  reader.superclass = "Ljava/lang/Object;";
  reader.access = {"public"};
  Method method;
  method.reference = {
      reader.name, "path", {"Landroid/content/Context;"}, "Ljava/lang/String;"};
  method.access = {"public", "static"};
  method.registers = 1;
  Instruction get;
  get.opcode = "invoke-virtual";
  get.registers = {0};
  get.reference = MethodRef{
      "Landroid/content/Context;", "getFilesDir", {}, "Ljava/io/File;"};
  Instruction move;
  move.pc = 3;
  move.opcode = "move-result-object";
  move.registers = {0};
  Instruction path = get;
  path.pc = 4;
  path.reference =
      MethodRef{"Ljava/io/File;", "getAbsolutePath", {}, "Ljava/lang/String;"};
  Instruction move2 = move;
  move2.pc = 7;
  Instruction ret;
  ret.pc = 8;
  ret.opcode = "return-object";
  ret.registers = {0};
  method.instructions = {get, move, path, move2, ret};
  reader.methods.push_back(method);
  Program code({reader}, Properties{});
  ASSERT_TRUE(code.knownReference("Ljava/io/File;"));
  ASSERT_FALSE(code.unsupportedClass("Ljava/io/File;"));
  Runtime state(
      code, heap,
      AndroidApplicationSnapshot{Phase::Application, {}, "/data/files"});
  auto app = state.library(operation(CurrentApplication), 0, {}, Admit).value;
  auto file = state.library(operation(FilesDirectory), app, {}, Admit).value;
  auto direct = state.library(operation(FilePath), file, {}, Admit);
  const uint64_t args[]{app};
  auto dex =
      state.execute(*code.findStatic(method.reference), args, Admit, 65536);
  EXPECT_EQ(dex.stop, ExecutionStop::Returned) << dex.diagnostic;
  EXPECT_EQ(dex.value, direct.value);
}
TEST_F(AndroidFrameworkTest, LoadClassDoesNotRunSuppliedClassInitialization) {
  Class type;
  type.name = "Ltest/Deferred;";
  type.superclass = "Ljava/lang/Object;";
  type.access = {"public"};
  Method init;
  init.reference = {type.name, "<clinit>", {}, "V"};
  init.access = {"static", "constructor"};
  init.registers = 1;
  Instruction zero;
  zero.opcode = "const/4";
  zero.registers = {0};
  zero.literal = int64_t(0);
  Instruction raise;
  raise.pc = 1;
  raise.opcode = "throw";
  raise.registers = {0};
  init.instructions = {zero, raise};
  type.methods.push_back(init);
  Program code({type}, Properties{});
  Runtime state(code, heap, AndroidApplicationSnapshot{Phase::Application, {}});
  auto app = state.library(operation(CurrentApplication), 0, {}, Admit);
  auto loader =
      state.library(operation(ContextClassLoader), app.value, {}, Admit);
  auto text = heap->literal("test.Deferred");
  const uint64_t args[]{text->Identity};
  auto loaded = state.library(operation(LoadClass), loader.value, args, Admit);
  EXPECT_EQ(loaded.stop, ExecutionStop::Returned) << loaded.diagnostic;
  EXPECT_EQ(loaded.value, heap->classObject("test/Deferred")->Identity);
  auto owner = state.library(operation(ClassLoaderOf), loaded.value, {}, Admit);
  EXPECT_EQ(owner.value, loader.value);
  auto initialized = state.initialize(type.name, Admit, 65536);
  EXPECT_EQ(initialized.stop, ExecutionStop::Thrown);
  EXPECT_EQ(initialized.exception, "java/lang/ExceptionInInitializerError");
  auto loaded_again =
      state.library(operation(LoadClass), loader.value, args, Admit);
  EXPECT_EQ(loaded_again.stop, ExecutionStop::Returned);
  EXPECT_EQ(loaded_again.value, loaded.value);
  auto again = state.initialize(type.name, Admit, 65536);
  EXPECT_EQ(again.exception, "java/lang/NoClassDefFoundError");
  EXPECT_EQ(again.stop, ExecutionStop::Thrown);
  auto array = heap->classObject("[[Ltest/Deferred;");
  EXPECT_EQ(
      state.library(operation(ClassLoaderOf), array->Identity, {}, Admit).value,
      loader.value);
}
TEST_F(AndroidFrameworkTest, UnknownPackageAndForeignInstancesAreNotInvented) {
  Runtime no_package(program, heap,
                     AndroidApplicationSnapshot{Phase::Application, {}});
  auto app = no_package.library(operation(CurrentApplication), 0, {}, Admit);
  auto result =
      no_package.library(operation(PackageName), app.value, {}, Admit);
  EXPECT_EQ(result.stop, ExecutionStop::Unsupported);
  EXPECT_NE(result.diagnostic.find("package"), std::string::npos);
  auto foreign =
      heap->allocate(Object::Instance, "android/app/ActivityThread", 0);
  auto denied = runtime.library(operation(ThreadApplication), foreign->Identity,
                                {}, Admit);
  EXPECT_EQ(denied.stop, ExecutionStop::Unsupported);
}
TEST_F(AndroidFrameworkTest,
       NoImplicitMissingClassesOrSystemLoaderAppNamespace) {
  for (const char *text : {"test.Missing", "java.lang.NotAClass", "[I",
                           "java/lang/String", "test.\xc3\xa9"}) {
    SCOPED_TRACE(text);
    Runtime state(program, heap,
                  AndroidApplicationSnapshot{Phase::Application, {}});
    auto loader = state.library(operation(SystemClassLoader), 0, {}, Admit);
    const uint64_t args[]{heap->literal(text)->Identity};
    auto loaded =
        state.library(operation(LoadClass), loader.value, args, Admit);
    EXPECT_EQ(loaded.stop, ExecutionStop::Unsupported) << loaded.diagnostic;
    EXPECT_TRUE(loaded.exception.empty());
  }
  Class supplied;
  supplied.name = "Ltest/Known;";
  supplied.superclass = "Ljava/lang/Object;";
  supplied.access = {"public"};
  Program code({supplied}, Properties{});
  Runtime state(code, heap, AndroidApplicationSnapshot{Phase::Application, {}});
  auto loader = state.library(operation(SystemClassLoader), 0, {}, Admit);
  const uint64_t args[]{heap->literal("test.Known")->Identity};
  auto loaded = state.library(operation(LoadClass), loader.value, args, Admit);
  EXPECT_EQ(loaded.stop, ExecutionStop::Unsupported);
}
TEST_F(AndroidFrameworkTest, NullAndWrongTypeKeepCommonLibraryValidation) {
  auto loader = call(SystemClassLoader);
  const uint64_t null[]{0};
  auto result = runtime.library(operation(LoadClass), loader, null, Admit);
  EXPECT_EQ(result.stop, ExecutionStop::Thrown);
  EXPECT_EQ(result.exception, "java/lang/NullPointerException");
  EXPECT_NE(result.throwable, 0u);
  EXPECT_EQ(call(SystemClassLoader), loader);
  auto wrong = runtime.library(operation(ThreadApplication), loader, {}, Admit);
  EXPECT_EQ(wrong.stop, ExecutionStop::Invalid);
  EXPECT_NE(wrong.diagnostic.find("receiver type"), std::string::npos);
}
TEST_F(AndroidFrameworkTest, WorkAndHeapBudgetsPoisonPartialState) {
  for (auto limit : {0, 1}) {
    Runtime state(program, heap,
                  AndroidApplicationSnapshot{Phase::Application, {}});
    unsigned used = 0;
    auto limited = [&] {
      return used++ < unsigned(limit) ? ExecutionStop::Returned
                                      : ExecutionStop::InstructionLimit;
    };
    auto result = state.library(operation(CurrentApplication), 0, {}, limited);
    EXPECT_EQ(result.stop, ExecutionStop::InstructionLimit);
    EXPECT_EQ(
        state.library(operation(CurrentApplication), 0, {}, Admit).diagnostic,
        result.diagnostic);
  }
  Runtime bounded(program, std::make_shared<Heap>(64, 1),
                  AndroidApplicationSnapshot{Phase::Application, {}});
  auto result = bounded.library(operation(CurrentApplication), 0, {}, Admit);
  EXPECT_EQ(result.stop, ExecutionStop::HeapLimit);
  EXPECT_EQ(bounded.library(operation(CurrentApplication), 0, {}, Admit).stop,
            ExecutionStop::HeapLimit);
}
TEST_F(AndroidFrameworkTest,
       InvalidSnapshotsAndPlatformReplacementAreRejected) {
  Program ordinary({});
  EXPECT_FALSE(ordinary.containsClass("Landroid/app/ActivityThread;"));
  EXPECT_THROW(
      Runtime(ordinary, heap, AndroidApplicationSnapshot{Phase::Attached, {}}),
      Error);
  EXPECT_THROW(
      Runtime(program, heap, AndroidApplicationSnapshot{Phase(99), {}}), Error);
  for (auto phase : {Phase::Unattached, Phase::Attached})
    EXPECT_THROW(
        Runtime(program, heap, AndroidApplicationSnapshot{phase, "app"}),
        Error);
  for (const std::string name : {std::string(), std::string(1025, 'x'),
                                 std::string("a\0b", 3), std::string("\xff")})
    EXPECT_THROW(Runtime(program, heap,
                         AndroidApplicationSnapshot{Phase::Application, name}),
                 Error);
  Class replacement;
  replacement.name = "Landroid/app/Application;";
  EXPECT_THROW(Program({replacement}, Properties{}), Error);
}
} // namespace
} // namespace neverd::mobile::dalvik
