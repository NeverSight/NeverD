/* Original freestanding callers of the public JNI 1.6 LP64 table ABI. */
typedef unsigned long long U64;
typedef unsigned char U8;
typedef unsigned short U16;
typedef const U64 *Table;
typedef Table *Handle;
typedef U64 Ref;
extern int JNI_GetCreatedJavaVMs(Handle *, int, int *);
extern int pthread_create(U64 *, const void *, void *(*)(void *), void *);
extern int pthread_join(U64, void **);
extern int __system_property_get(const char *, char *);
#define C(E, R, S, ...) (((R (*)(Handle, ...))(*E)[S])(E, ##__VA_ARGS__))
#define CHECK(X)                                                               \
  do {                                                                         \
    if (!(X))                                                                  \
      return __LINE__;                                                         \
  } while (0)
static Handle vm;
static Handle attach(void) {
  int count = 0;
  Handle env = 0;
  if (JNI_GetCreatedJavaVMs(&vm, 1, &count) || count != 1 ||
      C(vm, int, 4, &env, (void *)0))
    return 0;
  return env;
}
int jni_instrumentation(int mode) {
  Handle e = attach();
  CHECK(e);
  Ref type = C(e, Ref, 6, "android/app/ActivityThread");
  Ref thread_id = C(e, Ref, 113, type, "currentActivityThread",
                    "()Landroid/app/ActivityThread;");
  Ref thread = C(e, Ref, 114, type, thread_id);
  CHECK(thread);
  Ref field =
      C(e, Ref, 94, type, "mInstrumentation", "Landroid/app/Instrumentation;");
  CHECK(field && field == C(e, Ref, 94, type, "mInstrumentation",
                            "Landroid/app/Instrumentation;"));
  if (mode == 3)
    C(e, void, 104, thread, field, (Ref)0);
  Ref value = C(e, Ref, 95, thread, field);
  CHECK(!!value == (mode == 2));
  Ref getter = C(e, Ref, 33, type, "getInstrumentation",
                 "()Landroid/app/Instrumentation;");
  Ref via_method = C(e, Ref, 34, thread, getter);
  CHECK(C(e, int, 24, value, via_method));
  Ref global_thread = C(e, Ref, 21, thread);
  Ref global_value = value ? C(e, Ref, 21, value) : 0;
  if (value) {
    Ref declared = C(e, Ref, 6, "android/app/Instrumentation");
    Ref actual = C(e, Ref, 31, value);
    CHECK(C(e, int, 24, declared, actual));
    C(e, void, 23, value);
    C(e, void, 23, via_method);
  }
  CHECK(!C(vm, int, 5));
  e = attach();
  CHECK(e);
  Ref retained = C(e, Ref, 95, global_thread, field);
  CHECK(C(e, int, 24, global_value, retained));
  C(e, void, 104, global_thread, field, (Ref)0);
  CHECK(!C(e, Ref, 34, global_thread, getter));
  C(e, void, 104, global_thread, field, global_value);
  Ref restored = C(e, Ref, 34, global_thread, getter);
  CHECK(C(e, int, 24, global_value, restored));
  C(e, void, 22, global_thread);
  if (global_value)
    C(e, void, 22, global_value);
  CHECK(!C(vm, int, 5));
  return 0;
}

int jni_application_files(void) {
  Handle e = attach();
  CHECK(e);
  Ref type = C(e, Ref, 6, "android/app/ActivityThread");
  Ref id =
      C(e, Ref, 113, type, "currentApplication", "()Landroid/app/Application;");
  Ref app = C(e, Ref, 114, type, id);
  Ref context = C(e, Ref, 6, "android/content/Context");
  Ref files_id = C(e, Ref, 33, context, "getFilesDir", "()Ljava/io/File;");
  Ref file = C(e, Ref, 34, app, files_id);
  CHECK(file);
  Ref again = C(e, Ref, 34, app, files_id);
  CHECK(C(e, int, 24, file, again));
  Ref global = C(e, Ref, 21, file);
  C(e, void, 23, file);
  CHECK(!C(vm, int, 5));
  e = attach();
  CHECK(e);
  Ref cls = C(e, Ref, 6, "java/io/File");
  const char *Names[] = {"getPath", "getAbsolutePath", "toString"};
  for (unsigned i = 0; i < 3; ++i) {
    Ref path_id = C(e, Ref, 33, cls, Names[i], "()Ljava/lang/String;");
    Ref path = C(e, Ref, 34, global, path_id);
    const char *text = C(e, const char *, 169, path, (void *)0);
    const char Expected[] = "/fixture/files";
    CHECK(text);
    for (unsigned j = 0; j < sizeof(Expected); ++j)
      CHECK(text[j] == Expected[j]);
    C(e, void, 170, path, text);
  }
  C(e, void, 22, global);
  CHECK(!C(vm, int, 5));
  return 0;
}
static Ref application_global, application_loader_global;
static void *application_child(void *unused) {
  (void)unused;
  Handle e = attach();
  if (!e)
    return (void *)1;
  Ref type = C(e, Ref, 6, "android/app/ActivityThread");
  Ref id =
      C(e, Ref, 113, type, "currentApplication", "()Landroid/app/Application;");
  Ref app = C(e, Ref, 114, type, id);
  if (!C(e, int, 24, app, application_global))
    return (void *)2;
  Ref context = C(e, Ref, 6, "android/content/Context");
  Ref loader_id =
      C(e, Ref, 33, context, "getClassLoader", "()Ljava/lang/ClassLoader;");
  Ref loader = C(e, Ref, 34, app, loader_id);
  if (!C(e, int, 24, loader, application_loader_global))
    return (void *)3;
  return (void *)(U64)C(vm, int, 5);
}
int jni_application_snapshot(int phase) {
  Handle e = attach();
  CHECK(e);
  Ref type = C(e, Ref, 6, "android/app/ActivityThread");
  Ref thread_id = C(e, Ref, 113, type, "currentActivityThread",
                    "()Landroid/app/ActivityThread;");
  Ref thread = C(e, Ref, 114, type, thread_id);
  CHECK(!!thread == (phase != 0));
  Ref current_id =
      C(e, Ref, 113, type, "currentApplication", "()Landroid/app/Application;");
  Ref app = C(e, Ref, 114, type, current_id);
  CHECK(!!app == (phase == 2));
  if (thread) {
    Ref get_id =
        C(e, Ref, 33, type, "getApplication", "()Landroid/app/Application;");
    Ref again = C(e, Ref, 34, thread, get_id);
    CHECK(C(e, int, 24, app, again));
  }
  if (phase != 2) {
    CHECK(!C(vm, int, 5));
    return 0;
  }
  Ref app_type = C(e, Ref, 6, "android/app/Application");
  Ref actual_class = C(e, Ref, 31, app);
  CHECK(C(e, int, 24, app_type, actual_class));
  Ref base_id =
      C(e, Ref, 33, app_type, "getBaseContext", "()Landroid/content/Context;");
  Ref base = C(e, Ref, 34, app, base_id);
  CHECK(base && !C(e, int, 24, app, base));
  Ref context = C(e, Ref, 6, "android/content/Context");
  Ref app_id = C(e, Ref, 33, context, "getApplicationContext",
                 "()Landroid/content/Context;");
  Ref again = C(e, Ref, 34, app, app_id);
  CHECK(C(e, int, 24, again, app));
  again = C(e, Ref, 34, base, app_id);
  CHECK(C(e, int, 24, again, app));
  Ref loader_id =
      C(e, Ref, 33, context, "getClassLoader", "()Ljava/lang/ClassLoader;");
  Ref loader = C(e, Ref, 34, app, loader_id);
  again = C(e, Ref, 34, base, loader_id);
  CHECK(loader && C(e, int, 24, loader, again));
  Ref class_type = C(e, Ref, 6, "java/lang/Class");
  Ref class_loader_id =
      C(e, Ref, 33, class_type, "getClassLoader", "()Ljava/lang/ClassLoader;");
  Ref boot = C(e, Ref, 34, app_type, class_loader_id);
  Ref loader_type = C(e, Ref, 6, "java/lang/ClassLoader");
  Ref parent_id =
      C(e, Ref, 33, loader_type, "getParent", "()Ljava/lang/ClassLoader;");
  Ref system_id = C(e, Ref, 113, loader_type, "getSystemClassLoader",
                    "()Ljava/lang/ClassLoader;");
  Ref system = C(e, Ref, 114, loader_type, system_id);
  CHECK(boot && system && !C(e, int, 24, loader, system));
  CHECK(!C(e, int, 24, boot, loader) && !C(e, int, 24, boot, system));
  again = C(e, Ref, 34, loader, parent_id);
  CHECK(C(e, int, 24, boot, again));
  again = C(e, Ref, 34, system, parent_id);
  CHECK(C(e, int, 24, boot, again));
  CHECK(!C(e, Ref, 34, boot, parent_id));
  Ref load_id = C(e, Ref, 33, loader_type, "loadClass",
                  "(Ljava/lang/String;)Ljava/lang/Class;");
  Ref name = C(e, Ref, 167, "android.app.Application");
  again = C(e, Ref, 34, loader, load_id, name);
  CHECK(C(e, int, 24, app_type, again));
  CHECK(!C(e, Ref, 34, loader, load_id, (Ref)0) && C(e, int, 228));
  C(e, void, 17);
  application_global = C(e, Ref, 21, app);
  application_loader_global = C(e, Ref, 21, loader);
  C(e, void, 23, app);
  C(e, void, 23, loader);
  U64 child = 0;
  void *status = (void *)99;
  CHECK(!pthread_create(&child, (void *)0, application_child, (void *)0));
  CHECK(!pthread_join(child, &status) && !status);
  CHECK(!C(vm, int, 5));
  e = attach();
  CHECK(e);
  type = C(e, Ref, 6, "android/app/ActivityThread");
  CHECK(C(e, Ref, 113, type, "currentApplication",
          "()Landroid/app/Application;") == current_id);
  again = C(e, Ref, 114, type, current_id);
  CHECK(C(e, int, 24, again, application_global));
  again = C(e, Ref, 34, application_global, loader_id);
  CHECK(C(e, int, 24, again, application_loader_global));
  C(e, void, 22, application_global);
  C(e, void, 22, application_loader_global);
  CHECK(!C(vm, int, 5));
  return 0;
}
int jni_application_package(void) {
  Handle e = attach();
  Ref type = C(e, Ref, 6, "android/app/ActivityThread");
  Ref current =
      C(e, Ref, 113, type, "currentApplication", "()Landroid/app/Application;");
  Ref app = C(e, Ref, 114, type, current);
  Ref context = C(e, Ref, 6, "android/content/Context");
  Ref id = C(e, Ref, 33, context, "getPackageName", "()Ljava/lang/String;");
  Ref name = C(e, Ref, 34, app, id);
  const char *text = C(e, const char *, 169, name, (void *)0);
  const char expected[] = "test.app";
  for (unsigned i = 0; i < sizeof(expected); ++i)
    CHECK(text[i] == expected[i]);
  CHECK(C(e, int, 164, name) == 8);
  C(e, void, 170, name, text);
  CHECK(!C(vm, int, 5));
  return 0;
}
int jni_build_version(int expected_sdk, int distinct) {
  Handle e = attach();
  CHECK(e);
  Ref klass = C(e, Ref, 6, "android/os/Build$VERSION");
  Ref release_id = C(e, Ref, 144, klass, "RELEASE", "Ljava/lang/String;");
  Ref incremental_id =
      C(e, Ref, 144, klass, "INCREMENTAL", "Ljava/lang/String;");
  Ref sdk_id = C(e, Ref, 144, klass, "SDK_INT", "I");
  CHECK(release_id && incremental_id && sdk_id);
  Ref release = C(e, Ref, 145, klass, release_id);
  Ref again = C(e, Ref, 145, klass, release_id);
  Ref incremental = C(e, Ref, 145, klass, incremental_id);
  CHECK(C(e, int, 24, release, again));
  CHECK(C(e, int, 24, release, incremental) == !distinct);
  CHECK(C(e, int, 150, klass, sdk_id) == expected_sdk);
  char property[92];
  int length = __system_property_get("ro.build.version.release", property);
  const char *expected = length ? property : "unknown";
  const char *text = C(e, const char *, 169, release, (void *)0);
  unsigned i = 0;
  do {
    CHECK(text[i] == expected[i]);
  } while (expected[i++]);
  C(e, void, 170, release, text);
  Ref global = C(e, Ref, 21, release);
  CHECK(!C(vm, int, 5));
  e = attach();
  CHECK(e);
  klass = C(e, Ref, 6, "android/os/Build$VERSION");
  CHECK(C(e, Ref, 144, klass, "RELEASE", "Ljava/lang/String;") == release_id);
  again = C(e, Ref, 145, klass, release_id);
  CHECK(C(e, int, 24, global, again));
  C(e, void, 22, global);
  CHECK(!C(vm, int, 5));
  return 0;
}
int jni_dex_class_pair(const char *first, const char *last) {
  Handle e = attach();
  CHECK(e);
  Ref a = C(e, Ref, 6, first);
  Ref b = C(e, Ref, 6, last);
  CHECK(a && b && !C(e, int, 24, a, b));
  CHECK(!C(e, int, 228));
  CHECK(!C(vm, int, 5));
  return 0;
}
int jni_build_version_wrong_accessor(void) {
  Handle e = attach();
  Ref klass = C(e, Ref, 6, "android/os/Build$VERSION");
  Ref id = C(e, Ref, 144, klass, "RELEASE", "Ljava/lang/String;");
  return C(e, int, 150, klass, id);
}
int jni_build_version_write(void) {
  Handle e = attach();
  Ref klass = C(e, Ref, 6, "android/os/Build$VERSION");
  Ref id = C(e, Ref, 144, klass, "SDK_INT", "I");
  C(e, void, 159, klass, id, 999);
  return 0;
}
int jni_build_version_unknown_field(void) {
  Handle e = attach();
  Ref klass = C(e, Ref, 6, "android/os/Build$VERSION");
  return !C(e, Ref, 144, klass, "ACTIVE_CODENAMES", "[Ljava/lang/String;");
}
int jni_vm_lifecycle(void) {
  int count = 7;
  CHECK(!JNI_GetCreatedJavaVMs((void *)0, 0, &count) && count == 0);
  CHECK(!JNI_GetCreatedJavaVMs(&vm, 1, &count));
  CHECK(count == 1);
  U64 aliased = 0;
  CHECK(!JNI_GetCreatedJavaVMs((Handle *)&aliased, 1, (int *)&aliased));
  CHECK(aliased == (U64)vm);
  Handle env = (Handle)123;
  CHECK(C(vm, int, 6, &env, 0x10006) == -2 && !env);
  env = (Handle)123;
  CHECK(C(vm, int, 6, &env, 0x10008) == -3 && env == (Handle)123);
  CHECK(!C(vm, int, 4, &env, (void *)0) && env);
  Handle first = env;
  CHECK(C(env, int, 4) == 0x10006);
  CHECK(!C(vm, int, 7, &env, (void *)1) && env == first);
  CHECK(!C(vm, int, 6, &env, 0x10001) && env == first);
  Handle owner = 0;
  CHECK(!C(env, int, 219, &owner) && owner == vm);
  CHECK(!C(vm, int, 5));
  CHECK(C(vm, int, 5) == -1);
  CHECK(!C(vm, int, 4, &env, (void *)0) && env != first);
  CHECK(!C(vm, int, 5));
  return 0;
}
int jni_string_values(void) {
  Handle e = attach();
  CHECK(e);
  const char text[] = "A\300\200\355\240\275\355\270\200";
  Ref s = C(e, Ref, 167, text);
  CHECK(s);
  CHECK(C(e, int, 164, s) == 4 && C(e, int, 168, s) == 7);
  U8 copy = 0;
  const volatile U16 *p = C(e, const U16 *, 165, s, &copy);
  CHECK(copy == 1 && p[0] == 65 && p[1] == 0 && p[2] == 0xd83d &&
        p[3] == 0xde00);
  Ref same = C(e, Ref, 163, p, 4);
  CHECK(same && same != s);
  C(e, void, 166, s, p);
  const volatile U8 *q = C(e, const U8 *, 169, same, &copy);
  const U8 encoded[] = {65, 0xc0, 0x80, 0xf0, 0x9f, 0x98, 0x80, 0};
  for (unsigned i = 0; i < sizeof(encoded); ++i)
    CHECK(q[i] == encoded[i]);
  C(e, void, 170, same, q);
  volatile U8 region[8];
  for (unsigned i = 0; i < 8; ++i)
    region[i] = 0x7d;
  C(e, void, 221, s, 1, 2, region);
  CHECK(region[0] == 0xc0 && region[1] == 0x80 && region[2] == 0xed &&
        region[3] == 0xa0 && region[4] == 0xbd && region[5] == 0x7d);
  C(e, void, 220, s, -1, 1, region);
  CHECK(C(e, U8, 228) == 1 && region[0] == 0xc0);
  Ref error = C(e, Ref, 15);
  CHECK(error);
  C(e, void, 17);
  Ref kind = C(e, Ref, 6, "java/lang/StringIndexOutOfBoundsException");
  CHECK(C(e, U8, 32, error, kind) == 1);
  CHECK(!C(vm, int, 5));
  return 0;
}
int jni_arrays(void) {
  Handle e = attach();
  CHECK(e);
  Ref b = C(e, Ref, 176, 4);
  CHECK(b && C(e, int, 171, b) == 4);
  U8 input[] = {0x80, 2, 3, 4};
  C(e, void, 208, b, 0, 4, input);
  U8 copy = 0;
  volatile U8 *p = C(e, U8 *, 184, b, &copy);
  CHECK(copy && p[0] == 0x80);
  p[1] = 9;
  C(e, void, 192, b, p, 1);
  volatile U8 output[4] = {0};
  C(e, void, 200, b, 0, 4, output);
  CHECK(output[1] == 9);
  p[1] = 77;
  C(e, void, 192, b, p, 2);
  C(e, void, 200, b, 0, 4, output);
  CHECK(output[1] == 9 && output[0] == 0x80);
  C(e, void, 208, b, 3, 2, input);
  CHECK(C(e, U8, 228));
  C(e, void, 17);
  Ref string = C(e, Ref, 167, "value");
  Ref cls = C(e, Ref, 6, "java/lang/String");
  Ref array = C(e, Ref, 172, 2, cls, string);
  Ref element = C(e, Ref, 173, array, 1);
  CHECK(array && C(e, U8, 24, element, string));
  C(e, void, 174, array, 1, b);
  CHECK(C(e, U8, 228));
  C(e, void, 17);
  element = C(e, Ref, 173, array, 1);
  CHECK(C(e, U8, 24, element, string));
  C(e, void, 174, array, 1, (Ref)0);
  CHECK(!C(e, Ref, 173, array, 1));
  CHECK(!C(vm, int, 5));
  return 0;
}
int jni_references(void) {
  Handle e = attach();
  CHECK(e);
  CHECK(!C(e, int, 19, 8));
  Ref s = C(e, Ref, 167, "retained");
  Ref global = C(e, Ref, 21, s);
  CHECK(global != s && C(e, int, 232, global) == 2);
  Ref promoted = C(e, Ref, 20, s);
  CHECK(promoted != s && C(e, int, 232, promoted) == 1);
  CHECK(C(e, U8, 24, global, promoted));
  C(e, void, 23, promoted);
  CHECK(!C(vm, int, 5));
  e = attach();
  CHECK(e);
  CHECK(C(e, int, 164, global) == 8);
  C(e, void, 22, global);
  CHECK(!C(vm, int, 5));
  return 0;
}
int jni_primitive_bits(void) {
  Handle e = attach();
  CHECK(e);
  const unsigned widths[] = {1, 1, 2, 2, 4, 8, 4, 8};
  volatile U8 input[24], output[24];
  for (unsigned kind = 0; kind < 8; ++kind) {
    unsigned bytes = widths[kind] * 3;
    for (unsigned i = 0; i < bytes; ++i) {
      input[i] = kind ? (U8)(i * 67 + 0x81) : (i & 1);
      output[i] = 0x55;
    }
    Ref array = C(e, Ref, 175 + kind, 3);
    CHECK(array);
    C(e, void, 207 + kind, array, 0, 3, input);
    C(e, void, 199 + kind, array, 0, 3, output);
    for (unsigned i = 0; i < bytes; ++i)
      CHECK(input[i] == output[i]);
    U8 copy = 0;
    volatile U8 *p = C(e, U8 *, 183 + kind, array, &copy);
    CHECK(copy);
    p[0] = 0;
    C(e, void, 191 + kind, array, p, 0);
    C(e, void, 199 + kind, array, 0, 3, output);
    CHECK(!output[0]);
    for (unsigned i = 1; i < bytes; ++i)
      CHECK(input[i] == output[i]);
    C(e, void, 23, array);
  }
  CHECK(!C(vm, int, 5));
  return 0;
}
static Handle parent_env;
static Ref shared;
static void *thread_value(void *arg) {
  Handle e = 0;
  if (C(vm, int, 6, &e, 0x10006) != -2 || e)
    return (void *)1;
  if (C(vm, int, 4, &e, (void *)0) || e == parent_env)
    return (void *)2;
  if (arg)
    return (void *)(U64)C(parent_env, int, 4);
  if (C(e, int, 164, shared) != 6)
    return (void *)3;
  return (void *)(U64)C(vm, int, 5);
}
int jni_threads(void) {
  parent_env = attach();
  CHECK(parent_env);
  Ref local = C(parent_env, Ref, 167, "shared");
  shared = C(parent_env, Ref, 21, local);
  U64 thread;
  void *result = (void *)99;
  CHECK(!pthread_create(&thread, 0, thread_value, 0));
  CHECK(!pthread_join(thread, &result) && !result);
  C(parent_env, void, 22, shared);
  CHECK(!C(vm, int, 5));
  return 0;
}
int jni_wrong_thread(void) {
  parent_env = attach();
  CHECK(parent_env);
  U64 thread;
  CHECK(!pthread_create(&thread, 0, thread_value, (void *)1));
  pthread_join(thread, 0);
  return 0;
}
int jni_stale_env(void) {
  Handle e = attach();
  C(vm, int, 5);
  return C(e, int, 4);
}
int jni_stale_ref(void) {
  Handle e = attach();
  Ref s = C(e, Ref, 167, "gone");
  C(e, void, 23, s);
  return C(e, int, 164, s);
}
int jni_unknown_method(void) {
  Handle e = attach();
  Ref cls = C(e, Ref, 6, "java/lang/String");
  return (int)C(e, Ref, 33, cls, "trim", "()Ljava/lang/String;");
}
int jni_invalid_utf8(void) {
  Handle e = attach();
  return (int)C(e, Ref, 167, "\360\237\230");
}
int jni_double_release(void) {
  Handle e = attach();
  Ref s = C(e, Ref, 167, "once");
  const char *p = C(e, const char *, 169, s, (void *)0);
  C(e, void, 170, s, p);
  C(e, void, 170, s, p);
  return 0;
}

int jni_frame_underflow(void) {
  Handle e = attach();
  return (int)C(e, Ref, 20, (Ref)0);
}
int jni_detach_borrow(void) {
  Handle e = attach();
  Ref s = C(e, Ref, 167, "live");
  C(e, const char *, 169, s, (void *)0);
  return C(vm, int, 5);
}
int jni_wrong_borrow_object(void) {
  Handle e = attach();
  Ref a = C(e, Ref, 176, 4), b = C(e, Ref, 176, 4);
  void *p = C(e, void *, 184, a, (void *)0);
  C(e, void, 192, b, p, 0);
  return 0;
}
int jni_wrong_reference_kind(void) {
  Handle e = attach();
  Ref s = C(e, Ref, 167, "local");
  C(e, void, 22, s);
  return 0;
}
int jni_pending_cleanup(void) {
  Handle e = attach();
  CHECK(e);
  Ref array = C(e, Ref, 176, 1);
  U8 *p = C(e, U8 *, 184, array, (void *)0);
  CHECK(p);
  p[0] = 0x8a;
  C(e, void, 208, array, 1, 1, p);
  CHECK(C(e, U8, 228));
  CHECK(!C(e, int, 19, 2));
  Ref error = C(e, Ref, 15);
  CHECK(error);
  Ref saved = C(e, Ref, 20, error);
  CHECK(saved && C(e, U8, 228));
  C(e, void, 192, array, p, 0);
  C(e, void, 17);
  volatile U8 byte = 0;
  C(e, void, 200, array, 0, 1, &byte);
  CHECK(byte == 0x8a && !C(e, U8, 228));
  C(e, void, 23, saved);
  CHECK(!C(vm, int, 5));
  return 0;
}
int jni_pending_illegal(void) {
  Handle e = attach();
  Ref array = C(e, Ref, 176, 1);
  C(e, void, 208, array, 1, 1, (void *)1);
  return C(e, int, 4);
}
int jni_table_replaced(void) {
  extern int mprotect(void *, U64, int);
  Handle e = attach();
  U64 page = (U64)e & ~(U64)4095;
  CHECK(!mprotect((void *)page, 4096, 3));
  *(volatile U64 *)((U64)e + 8) = 0;
  return C(e, int, 4);
}
int jni_invalid_copyout(void) {
  Handle e = attach();
  Ref s = C(e, Ref, 167, "guard");
  return (int)(U64)C(e, const char *, 169, s, (void *)1);
}

int jni_named_env_spoof(void) {
  extern int fake_version(Handle) __asm__("JNIEnv.4");
  Handle e = attach();
  return fake_version(e);
}
int jni_named_vm_spoof(void) {
  extern int fake_detach(Handle) __asm__("JavaVM.DetachCurrentThread");
  attach();
  return fake_detach(vm);
}

int jni_art_string_edges(void) {
  Handle e = attach();
  CHECK(e);
  CHECK(C(e, int, 19, -1) == -1 && C(e, int, 26, -1) == -1);
  CHECK(!C(e, Ref, 167, (void *)0));
  Ref empty = C(e, Ref, 163, (void *)0, 0);
  CHECK(empty && C(e, int, 164, empty) == 0);
  volatile U8 copy = 0x55;
  CHECK(!C(e, const char *, 169, (Ref)0, &copy) && copy == 0x55);
  CHECK(!C(e, const char *, 169, (Ref)0, (void *)1));
  const char text[] = "\360\220\200\200\364\217\277\277";
  Ref string = C(e, Ref, 167, text);
  CHECK(string && C(e, int, 164, string) == 4 && C(e, int, 168, string) == 8);
  const volatile U16 *units = C(e, const U16 *, 165, string, (void *)0);
  CHECK(units[0] == 0xd800 && units[1] == 0xdc00 && units[2] == 0xdbff &&
        units[3] == 0xdfff);
  C(e, void, 166, string, units);
  const volatile U8 *encoded = C(e, const U8 *, 169, string, (void *)0);
  for (unsigned i = 0; i < sizeof(text); ++i)
    CHECK(encoded[i] == (U8)text[i]);
  C(e, void, 170, string, encoded);
  volatile U8 region[8] = {0x55, 0x55, 0x55, 0x55, 0x55, 0x55, 0x55, 0x55};
  C(e, void, 221, string, 1, 2, region);
  const U8 isolated[] = {0xed, 0xb0, 0x80, 0xed, 0xaf, 0xbf, 0x55, 0x55};
  for (unsigned i = 0; i < 8; ++i)
    CHECK(region[i] == isolated[i]);
  CHECK(!C(e, U8, 228));
  CHECK(!C(vm, int, 5));
  return 0;
}
int jni_negative_array(void) {
  Handle e = attach();
  return (int)C(e, Ref, 176, -1);
}
int jni_negative_object_array(void) {
  Handle e = attach();
  Ref cls = C(e, Ref, 6, "java/lang/Object");
  return (int)C(e, Ref, 172, -1, cls, (Ref)0);
}
int jni_bad_array_initializer(void) {
  Handle e = attach();
  Ref cls = C(e, Ref, 6, "java/lang/String");
  Ref array = C(e, Ref, 176, 1);
  return (int)C(e, Ref, 172, 1, cls, array);
}

/* Method execution uses independently authored standard JNI calls. */
static Ref method(Handle e, Ref c, const char *n, const char *s) {
  return C(e, Ref, 33, c, n, s);
}
static U16 char_v(Handle e, Ref s, Ref m, ...) {
  __builtin_va_list args;
  __builtin_va_start(args, m);
  U16 v = C(e, U16, 44, s, m, args);
  __builtin_va_end(args);
  return v;
}
static U16 char_v_stack(Handle e, Ref s, Ref m, int a, int b, int c, int d,
                        int last, ...) {
  __builtin_va_list args;
  __builtin_va_start(args, last);
  U16 v = C(e, U16, 44, s, m, args);
  __builtin_va_end(args);
  return v;
}
int jni_method_values(void) {
  Handle e = attach();
  CHECK(e);
  Ref cls = C(e, Ref, 6, "java/lang/String");
  Ref root = C(e, Ref, 6, "java/lang/Object");
  Ref s = C(e, Ref, 167, "A\300\200\355\240\275\355\270\200Z");
  Ref same = C(e, Ref, 167, "A\300\200\360\237\230\200Z");
  Ref length = method(e, cls, "length", "()I");
  CHECK(length && length == method(e, cls, "length", "()I"));
  CHECK(C(e, int, 49, s, length) == 5);
  CHECK(C(e, int, 51, s, length, (void *)1) == 5); /* No argument reads. */
  CHECK(!C(e, U8, 37, s, method(e, cls, "isEmpty", "()Z")));
  Ref ch = method(e, cls, "charAt", "(I)C");
  CHECK(C(e, U16, 43, s, ch, 2) == 0xd83d);
  CHECK(char_v(e, s, ch, 3) == 0xde00);
  CHECK(char_v_stack(e, s, ch, 0, 0, 0, 0, 0, 1) == 0);
  U64 args[4] = {0xdeadbeef00000004ULL, 0, 0, 0};
  CHECK(C(e, U16, 45, s, ch, args) == 90);
  CHECK(C(e, int, 49, s, method(e, cls, "hashCode", "()I")) == 114988824);
  Ref eq = method(e, root, "equals", "(Ljava/lang/Object;)Z");
  CHECK(C(e, U8, 37, s, eq, same));        /* Virtual override. */
  CHECK(!C(e, U8, 67, s, root, eq, same)); /* Object identity. */
  CHECK(C(e, U8, 67, s, root, eq, s));
  CHECK(!C(e, U8, 37, s, eq, (Ref)0));
  Ref identity =
      C(e, Ref, 34, s, method(e, cls, "toString", "()Ljava/lang/String;"));
  CHECK(C(e, U8, 24, s, identity));
  Ref sub = method(e, cls, "substring", "(II)Ljava/lang/String;");
  args[0] = 2;
  args[1] = 4;
  Ref pair = C(e, Ref, 36, s, sub, args);
  CHECK(C(e, int, 164, pair) == 2);
  U16 units[8] = {0x7e7e, 0x7e7e, 0x7e7e, 0x7e7e,
                  0x7e7e, 0x7e7e, 0x7e7e, 0x7e7e};
  C(e, void, 220, pair, 0, 2, units + 1);
  CHECK(units[0] == 0x7e7e && units[1] == 0xd83d && units[2] == 0xde00 &&
        units[3] == 0x7e7e);
  identity = C(e, Ref, 34, s, sub, 0, 5);
  CHECK(C(e, U8, 24, s, identity));
  Ref suffix = C(e, Ref, 167, "Z");
  Ref concat =
      C(e, Ref, 34, pair,
        method(e, cls, "concat", "(Ljava/lang/String;)Ljava/lang/String;"),
        suffix);
  CHECK(C(e, int, 164, concat) == 3);
  CHECK(C(e, int, 49, s, method(e, cls, "indexOf", "(Ljava/lang/String;)I"),
          concat) == 2);
  CHECK(C(e, int, 49, s, method(e, cls, "indexOf", "(Ljava/lang/String;I)I"),
          pair, 3) == -1);
  Ref empty = C(e, Ref, 167, "");
  CHECK(C(e, int, 49, s, method(e, cls, "indexOf", "(Ljava/lang/String;I)I"),
          empty, 99) == 5);
  CHECK(C(e, U8, 37, s, method(e, cls, "startsWith", "(Ljava/lang/String;I)Z"),
          pair, 2));
  CHECK(!C(e, U8, 37, s, method(e, cls, "startsWith", "(Ljava/lang/String;I)Z"),
           pair, -1));
  CHECK(C(e, U8, 37, s, method(e, cls, "endsWith", "(Ljava/lang/String;)Z"),
          suffix));
  CHECK(C(e, int, 49, s, method(e, cls, "compareTo", "(Ljava/lang/String;)I"),
          suffix) == -25);
  Ref array = C(e, Ref, 34, s, method(e, cls, "toCharArray", "()[C"));
  CHECK(C(e, int, 171, array) == 5);
  Ref ctor = method(e, cls, "<init>", "([CII)V");
  args[0] = array;
  args[1] = 2;
  args[2] = 2;
  Ref constructed = C(e, Ref, 30, cls, ctor, args);
  CHECK(C(e, U8, 37, pair, eq, constructed));
  CHECK(!C(e, U8, 24, pair, constructed));
  Ref copy = method(e, cls, "getChars", "(II[CI)V");
  C(e, void, 61, suffix, copy, 0, 1, array, 0);
  C(e, void, 201, array, 0, 5, units + 1);
  CHECK(units[1] == 90 && units[2] == 0 && units[3] == 0xd83d &&
        units[4] == 0xde00 && units[5] == 90 && units[6] == 0x7e7e);
  C(e, void, 220, s, 0, 1, units);
  CHECK(units[0] ==
        65); /* toCharArray and constructor copies are independent. */
  Ref classClass = C(e, Ref, 6, "java/lang/Class");
  Ref name = C(e, Ref, 34, cls,
               method(e, classClass, "getName", "()Ljava/lang/String;"));
  Ref name_again = C(e, Ref, 34, cls,
                     method(e, classClass, "getName", "()Ljava/lang/String;"));
  CHECK(C(e, U8, 24, name, name_again));
  Ref expect = C(e, Ref, 167, "java.lang.String");
  CHECK(C(e, U8, 37, name, eq, expect));
  CHECK(!C(e, U8, 37, cls, method(e, classClass, "isArray", "()Z")));
  Ref actualClass =
      C(e, Ref, 34, s, method(e, root, "getClass", "()Ljava/lang/Class;"));
  CHECK(C(e, U8, 24, cls, actualClass));
  Ref value = C(e, Ref, 113, cls, "valueOf", "(J)Ljava/lang/String;");
  Ref number = C(e, Ref, 114, cls, value, (U64)0x8000000000000000ULL);
  expect = C(e, Ref, 167, "-9223372036854775808");
  CHECK(C(e, U8, 37, number, eq, expect));
  Ref integer = C(e, Ref, 113, cls, "valueOf", "(I)Ljava/lang/String;");
  Ref small = C(e, Ref, 114, cls, integer, -99);
  identity = C(e, Ref, 114, cls, integer, -99);
  CHECK(C(e, U8, 24, small, identity));
  small = C(e, Ref, 114, cls, integer, -100);
  identity = C(e, Ref, 114, cls, integer, -100);
  CHECK(!C(e, U8, 24, small, identity) && C(e, U8, 37, small, eq, identity));
  Ref boolean = C(e, Ref, 113, cls, "valueOf", "(Z)Ljava/lang/String;");
  small = C(e, Ref, 114, cls, boolean, 1);
  identity = C(e, Ref, 114, cls, boolean, 1);
  expect = C(e, Ref, 167, "true");
  CHECK(C(e, U8, 24, small, identity) && C(e, U8, 37, small, eq, expect));
  Ref character = C(e, Ref, 113, cls, "valueOf", "(C)Ljava/lang/String;");
  args[0] = 0xffff00000000005aULL;
  small = C(e, Ref, 116, cls, character, args);
  CHECK(C(e, U8, 37, small, eq, suffix));
  constructed = C(e, Ref, 28, cls, method(e, cls, "<init>", "()V"));
  CHECK(C(e, U8, 37, constructed, method(e, cls, "isEmpty", "()Z")));
  constructed =
      C(e, Ref, 28, cls, method(e, cls, "<init>", "(Ljava/lang/String;)V"), s);
  CHECK(!C(e, U8, 24, s, constructed) && C(e, U8, 37, s, eq, constructed));
  Ref array_class =
      C(e, Ref, 34, array, method(e, root, "getClass", "()Ljava/lang/Class;"));
  CHECK(C(e, U8, 37, array_class, method(e, classClass, "isArray", "()Z")));
  /* Method IDs outlive the local reference used to resolve them. */
  C(e, void, 23, cls);
  CHECK(C(e, int, 49, s, length) == 5);
  CHECK(!C(vm, int, 5));
  return 0;
}
int jni_method_exceptions(void) {
  Handle e = attach();
  CHECK(e);
  Ref cls = C(e, Ref, 6, "java/lang/String"), s = C(e, Ref, 167, "abc");
  Ref ch = method(e, cls, "charAt", "(I)C");
  CHECK(!C(e, U16, 43, s, ch, -1));
  CHECK(C(e, U8, 228));
  C(e, void, 17);
  Ref copy = method(e, cls, "getChars", "(II[CI)V");
  C(e, void, 61, s, copy, -1, 99, (Ref)0, -1);
  Ref ex = C(e, Ref, 15);
  C(e, void, 17);
  Ref exception_class = C(e, Ref, 6, "java/lang/NullPointerException");
  CHECK(C(e, U8, 32, ex, exception_class));
  Ref a = C(e, Ref, 177, 2);
  U16 input[] = {11, 22};
  C(e, void, 209, a, 0, 2, input);
  C(e, void, 61, s, copy, 0, 3, a, 0);
  CHECK(C(e, U8, 228));
  C(e, void, 17);
  U16 out[] = {0, 0};
  C(e, void, 201, a, 0, 2, out);
  CHECK(out[0] == 11 && out[1] == 22);
  CHECK(!C(vm, int, 5));
  return 0;
}
int jni_method_wrong_return(void) {
  Handle e = attach();
  Ref cls = C(e, Ref, 6, "java/lang/String");
  Ref s = C(e, Ref, 167, "abc");
  return C(e, U8, 37, s, method(e, cls, "length", "()I"));
}
int jni_method_wrong_receiver(void) {
  Handle e = attach();
  Ref cls = C(e, Ref, 6, "java/lang/String");
  Ref array = C(e, Ref, 176, 2);
  return C(e, int, 49, array, method(e, cls, "length", "()I"));
}
int jni_method_forged_id(void) {
  Handle e = attach();
  Ref s = C(e, Ref, 167, "abc");
  return C(e, int, 49, s, s);
}
int jni_method_unreadable_arguments(void) {
  Handle e = attach();
  Ref cls = C(e, Ref, 6, "java/lang/String");
  Ref s = C(e, Ref, 167, "abc");
  return C(e, U16, 45, s, method(e, cls, "charAt", "(I)C"), (void *)1);
}

/* Generic supplied-method caller: no Java result or method body lives here. */
static U64 dex_v(Handle e, Ref cl, Ref method, unsigned kind, ...) {
  __builtin_va_list ap;
  __builtin_va_start(ap, kind);
  U64 result = 0;
  switch (kind) {
  case 0:
    result = (unsigned)C(e, int, 130, cl, method, ap);
    break;
  case 1:
    result = C(e, U64, 133, cl, method, ap);
    break;
  case 2:
    result = C(e, U8, 118, cl, method, ap);
    break;
  case 3:
    result = (unsigned)(int)C(e, signed char, 121, cl, method, ap);
    break;
  case 4:
    result = C(e, U16, 124, cl, method, ap);
    break;
  case 5:
    result = (unsigned)(int)C(e, short, 127, cl, method, ap);
    break;
  }
  __builtin_va_end(ap);
  return result;
}
U64 jni_dex_call(const char *class_name, const char *name,
                 const char *signature, const U64 *args, U64 kind, U64 form) {
  Handle e = attach();
  Ref cl = C(e, Ref, 6, class_name);
  Ref method = C(e, Ref, 113, cl, name, signature);
  U64 result = 0;
#define DEX_ARGS                                                               \
  args[0], args[1], args[2], args[3], args[4], args[5], args[6], args[7],      \
      args[8], args[9]
  if (form == 1)
    result = dex_v(e, cl, method, kind, DEX_ARGS);
  else if (form == 2) {
    switch (kind) {
    case 0:
      result = (unsigned)C(e, int, 131, cl, method, args);
      break;
    case 1:
      result = C(e, U64, 134, cl, method, args);
      break;
    case 2:
      result = C(e, U8, 119, cl, method, args);
      break;
    case 3:
      result = (unsigned)(int)C(e, signed char, 122, cl, method, args);
      break;
    case 4:
      result = C(e, U16, 125, cl, method, args);
      break;
    case 5:
      result = (unsigned)(int)C(e, short, 128, cl, method, args);
      break;
    }
  } else {
    switch (kind) {
    case 0:
      result = (unsigned)C(e, int, 129, cl, method, DEX_ARGS);
      break;
    case 1:
      result = C(e, U64, 132, cl, method, DEX_ARGS);
      break;
    case 2:
      result = C(e, U8, 117, cl, method, DEX_ARGS);
      break;
    case 3:
      result = (unsigned)(int)C(e, signed char, 120, cl, method, DEX_ARGS);
      break;
    case 4:
      result = C(e, U16, 123, cl, method, DEX_ARGS);
      break;
    case 5:
      result = (unsigned)(int)C(e, short, 126, cl, method, DEX_ARGS);
      break;
    }
  }
#undef DEX_ARGS
  if (C(e, U8, 228)) {
    Ref exception = C(e, Ref, 15);
    C(e, void, 17);
    Ref type = C(e, Ref, 6, "java/lang/ArithmeticException");
    if (!C(e, U8, 32, exception, type))
      return 0xeeee;
    C(e, void, 23, exception);
    return 0xdec0de;
  }
  return result;
}

/* A sequence of standard JNI calls, with no Java results encoded here. */
struct DexStep {
  char class_name[64];
  U64 input;
};
int jni_dex_state(const struct DexStep *, U64, U64 *, U64);
struct DexWorker {
  const struct DexStep *step;
  U64 *results;
};
static void *dex_worker(void *opaque) {
  const struct DexWorker *worker = opaque;
  U64 result = jni_dex_state(worker->step, 1, worker->results, 0);
  int detached = C(vm, int, 5);
  return (void *)(result ? result : detached ? ~0ULL : 0);
}
static U64 dex_exception(Handle e) {
  if (!C(e, U8, 228))
    return 0;
  Ref exception = C(e, Ref, 15);
  C(e, void, 17);
  const char *names[] = {"java/lang/ExceptionInInitializerError",
                         "java/lang/NoClassDefFoundError",
                         "java/lang/ArithmeticException"};
  U64 status = 4;
  for (unsigned i = 0; i < 3; ++i) {
    Ref type = C(e, Ref, 6, names[i]);
    if (C(e, U8, 32, exception, type))
      status = i + 1;
    C(e, void, 23, type);
  }
  if (status <= 2) {
    Ref base = C(e, Ref, 6, "java/lang/LinkageError");
    if (!C(e, U8, 32, exception, base))
      status = 5;
    C(e, void, 23, base);
  }
  C(e, void, 23, exception);
  return status;
}
int jni_dex_state(const struct DexStep *steps, U64 count, U64 *results,
                  U64 detach) {
  Handle e = attach();
  CHECK(e && count <= 64);
  for (U64 i = 0; i < count; ++i) {
    if (detach == 2 && i % 2) {
      struct DexWorker worker = {steps + i, results + 2 * i};
      U64 thread;
      void *returned = (void *)1;
      CHECK(!pthread_create(&thread, 0, dex_worker, &worker));
      CHECK(!pthread_join(thread, &returned) && !returned);
      continue;
    }
    U64 result = 0;
    Ref cl = C(e, Ref, 6, steps[i].class_name);
    U64 status = dex_exception(e);
    if (!status) {
      Ref method = C(e, Ref, 113, cl, "step", "(I)J");
      status = dex_exception(e);
      if (!status) {
        result = C(e, U64, 134, cl, method, &steps[i].input);
        status = dex_exception(e);
      }
      C(e, void, 23, cl);
    }
    results[2 * i] = status;
    results[2 * i + 1] = result;
    if (detach == 1) {
      CHECK(!C(vm, int, 5));
      e = attach();
      CHECK(e);
    }
  }
  return 0;
}

/* Bidirectional object/field/array access. Java bodies and expected values are
 * supplied externally; this fixture only exercises the standard JNI ABI. */
static Ref dex_new_v(Handle e, Ref cl, Ref ctor, ...) {
  __builtin_va_list ap;
  __builtin_va_start(ap, ctor);
  Ref result = C(e, Ref, 29, cl, ctor, ap);
  __builtin_va_end(ap);
  return result;
}
static U64 dex_instance_v(Handle e, Ref object, Ref method, ...) {
  __builtin_va_list ap;
  __builtin_va_start(ap, method);
  U64 result = C(e, U64, 53, object, method, ap);
  __builtin_va_end(ap);
  return result;
}
int jni_dex_objects(int seed, U64 form, U64 scenario, U64 *out) {
  Handle e = attach();
  Ref cl = C(e, Ref, 6, "runtime/objects/Box");
  Ref ctor = C(e, Ref, 33, cl, "<init>", "(I)V");
  U64 arg = (unsigned)seed;
  Ref box = form == 0   ? C(e, Ref, 28, cl, ctor, seed)
            : form == 1 ? dex_new_v(e, cl, ctor, seed)
                        : C(e, Ref, 30, cl, ctor, &arg);
  CHECK(box && !C(e, U8, 228));
  const char *names[] = {"value", "wide",   "flag", "small", "unit",
                         "half",  "serial", "tag",  "bytes", "peers"};
  const char *types[] = {"I",  "J",
                         "Z",  "B",
                         "C",  "S",
                         "I",  "Ljava/lang/Object;",
                         "[B", "[Lruntime/objects/Box;"};
  Ref fields[10];
  for (unsigned i = 0; i < 10; ++i)
    fields[i] = C(e, Ref, 94, cl, names[i], types[i]);
  if (scenario == 1)
    return C(e, U64, 101, box, fields[0]);
  if (scenario == 2) {
    C(e, void, 109, box, fields[6], 3);
    return 0;
  }
  if (scenario == 3)
    return C(e, int, 100, cl, fields[0]);
  if (scenario == 4)
    return C(e, int, 100, box, 0x12345678ULL);
  if (scenario == 5) {
    Ref method = C(e, Ref, 113, cl, "exhaust", "()Ljava/lang/Object;");
    return C(e, Ref, 116, cl, method, (void *)0);
  }
  if (scenario == 6) {
    Ref count = C(e, Ref, 144, cl, "count", "I");
    int before = C(e, int, 150, cl, count);
    Ref raw = C(e, Ref, 27, cl);
    CHECK(raw && !C(e, int, 100, raw, fields[0]) &&
          !C(e, U64, 101, raw, fields[1]));
    CHECK(!C(e, Ref, 95, raw, fields[8]) && !C(e, int, 100, raw, fields[6]));
    C(e, void, 109, raw, fields[0], 19);
    CHECK(C(e, int, 100, raw, fields[0]) == 19 &&
          C(e, int, 150, cl, count) == before);
    return 0;
  }
  out[0] = (unsigned)C(e, int, 100, box, fields[0]);
  out[1] = C(e, U64, 101, box, fields[1]);
  out[2] = C(e, U8, 96, box, fields[2]);
  out[3] = (unsigned)(int)C(e, signed char, 97, box, fields[3]);
  out[4] = C(e, U16, 98, box, fields[4]);
  out[5] = (unsigned)(int)C(e, short, 99, box, fields[5]);
  out[6] = (unsigned)C(e, int, 100, box, fields[6]);
  C(e, void, 109, box, fields[0], seed ^ 0x55555555);
  C(e, void, 110, box, fields[1], 0xfedcba9876543210ULL);
  C(e, void, 105, box, fields[2], 1);
  C(e, void, 106, box, fields[3], -128);
  C(e, void, 107, box, fields[4], 65535);
  C(e, void, 108, box, fields[5], -32768);
  Ref calculate = C(e, Ref, 33, cl, "calculate", "(I)J");
  arg = 17;
  out[7] = form == 0   ? C(e, U64, 52, box, calculate, 17)
           : form == 1 ? dex_instance_v(e, box, calculate, 17)
           : form == 3 ? C(e, U64, 84, box, cl, calculate, &arg)
                       : C(e, U64, 54, box, calculate, &arg);
  out[8] = (unsigned)C(e, int, 100, box, fields[0]);
  Ref bytes = C(e, Ref, 95, box, fields[8]);
  U8 buffer[5];
  C(e, void, 200, bytes, 0, 5, buffer);
  out[9] = buffer[1];
  Ref echo =
      C(e, Ref, 33, cl, "echo", "(Ljava/lang/Object;)Ljava/lang/Object;");
  Ref echoed = C(e, Ref, 36, box, echo, &bytes);
  out[10] = C(e, U8, 24, bytes, echoed);
  Ref tag = C(e, Ref, 95, box, fields[7]);
  out[11] = C(e, U8, 24, bytes, tag);
  U8 *borrowed = C(e, U8 *, 184, bytes, (void *)0);
  borrowed[0] = 42;
  C(e, void, 192, bytes, borrowed, 0);
  C(e, void, 200, bytes, 0, 5, buffer);
  out[12] = buffer[0];
  Ref peers = C(e, Ref, 172, 2, cl, (Ref)0);
  C(e, void, 174, peers, 1, box);
  C(e, void, 104, box, fields[9], peers);
  Ref observed_peers = C(e, Ref, 95, box, fields[9]);
  Ref observed_box = C(e, Ref, 173, observed_peers, 1);
  out[13] = C(e, U8, 24, box, observed_box);
  Ref make = C(e, Ref, 113, cl, "make", "(I)Lruntime/objects/Box;");
  arg = (unsigned)-3;
  Ref made = C(e, Ref, 116, cl, make, &arg);
  Ref made_peers = C(e, Ref, 95, made, fields[9]);
  Ref made_self = C(e, Ref, 173, made_peers, 0);
  out[14] = C(e, U8, 24, made, made_self);
  Ref saved = C(e, Ref, 144, cl, "saved", "Lruntime/objects/Box;");
  Ref shared = C(e, Ref, 144, cl, "shared", "[B");
  Ref count = C(e, Ref, 144, cl, "count", "I");
  Ref saved_value = C(e, Ref, 145, cl, saved);
  out[15] = C(e, U8, 24, made, saved_value);
  Ref shared_value = C(e, Ref, 145, cl, shared);
  Ref made_bytes = C(e, Ref, 95, made, fields[8]);
  out[16] = C(e, U8, 24, made_bytes, shared_value);
  C(e, void, 159, cl, count, 99);
  arg = 0;
  Ref next = C(e, Ref, 30, cl, ctor, &arg);
  out[17] = (unsigned)C(e, int, 100, next, fields[6]);
  C(e, void, 154, cl, saved, box);
  saved_value = C(e, Ref, 145, cl, saved);
  out[18] = C(e, U8, 24, box, saved_value);
  Ref arrays = C(e, Ref, 113, cl, "arrays", "(I)J");
  arg = (unsigned)seed;
  out[19] = C(e, U64, 134, cl, arrays, &arg);
  Ref fault = C(e, Ref, 113, cl, "fault", "(I)I");
  const char *exceptions[] = {"java/lang/NullPointerException",
                              "java/lang/ArrayIndexOutOfBoundsException",
                              "java/lang/NegativeArraySizeException",
                              "java/lang/ArrayStoreException",
                              "java/lang/ClassCastException"};
  for (arg = 0; arg < 5; ++arg) {
    C(e, int, 131, cl, fault, &arg);
    CHECK(C(e, U8, 228));
    Ref exception = C(e, Ref, 15);
    C(e, void, 17);
    Ref type = C(e, Ref, 6, exceptions[arg]);
    out[20 + arg] = C(e, U8, 32, exception, type) ? arg + 1 : 0;
  }
  arg = (unsigned)-21;
  out[25] = C(e, U64, 54, box, calculate, &arg);
  C(e, void, 200, bytes, 0, 5, buffer);
  out[26] = buffer[1];
  out[27] = C(e, U8, 24, peers, observed_peers);
  Ref global = C(e, Ref, 21, box);
  CHECK(!C(vm, int, 5));
  e = attach();
  out[28] = (unsigned)C(e, int, 100, global, fields[0]);
  cl = C(e, Ref, 6, "runtime/objects/Box");
  out[29] = (unsigned)C(e, int, 150, cl, count);
  C(e, void, 22, global);
  Ref leaf = C(e, Ref, 176, 3);
  Ref leaf_class = C(e, Ref, 31, leaf);
  Ref nested = C(e, Ref, 172, 1, leaf_class, leaf);
  Ref nested_class = C(e, Ref, 31, nested);
  CHECK(C(e, U8, 32, nested, nested_class));
  out[30] = C(e, int, 171, nested) + C(e, int, 171, leaf);
  Ref sf = C(e, Ref, 144, cl, "staticFlag", "Z");
  Ref sb = C(e, Ref, 144, cl, "staticSmall", "B");
  Ref sc = C(e, Ref, 144, cl, "staticUnit", "C");
  Ref ss = C(e, Ref, 144, cl, "staticHalf", "S");
  Ref sj = C(e, Ref, 144, cl, "staticWide", "J");
  C(e, void, 155, cl, sf, 1);
  C(e, void, 156, cl, sb, -120);
  C(e, void, 157, cl, sc, 65534);
  C(e, void, 158, cl, ss, -30000);
  C(e, void, 160, cl, sj, 0x123456789abcdef0ULL);
  U64 sum = C(e, U64, 151, cl, sj) + (long long)C(e, signed char, 147, cl, sb) +
            C(e, U16, 148, cl, sc) + (long long)C(e, short, 149, cl, ss) +
            (C(e, U8, 146, cl, sf) ? 17 : 0);
  Ref statics = C(e, Ref, 113, cl, "statics", "()J");
  out[31] = C(e, U64, 134, cl, statics, (void *)0);
  CHECK(sum == out[31]);
  CHECK(!C(e, U8, 228));
  return 0;
}

/* Cross-boundary identities and dispatch; Java values are computed by DEX. */
#define J(E, R, S, ...) ((R (*)(Handle, ...))(*(E))[S])((E), __VA_ARGS__)
int jni_dex_language(int seed, U64 form, U64 *out) {
  Handle e = attach();
  Ref child = C(e, Ref, 6, "runtime/language/Child");
  Ref base = C(e, Ref, 6, "runtime/language/Base");
  Ref iface = C(e, Ref, 6, "runtime/language/Operation");
  Ref object = C(e, Ref, 6, "java/lang/Object");
  Ref language = C(e, Ref, 6, "runtime/language/Language");
  Ref string = C(e, Ref, 6, "java/lang/String");
  U64 args[2] = {(unsigned)seed, 0};
  Ref ctor = C(e, Ref, 33, child, "<init>", "(I)V");
  Ref a = C(e, Ref, 30, child, ctor, args);
  Ref b = C(e, Ref, 30, child, ctor, args);
  Ref compute = C(e, Ref, 33, base, "compute", "(I)J");
  args[0] = 17;
  out[0] = form == 0   ? C(e, U64, 52, a, compute, 17)
           : form == 1 ? dex_instance_v(e, a, compute, 17)
                       : C(e, U64, 54, a, compute, args);
  out[1] = C(e, U64, 84, a, base, compute, args);
  Ref interface_method = C(e, Ref, 33, iface, "compute", "(I)J");
  out[2] = C(e, U64, 54, a, interface_method, args);
  Ref field = C(e, Ref, 94, child, "value", "I");
  out[3] = (unsigned)C(e, int, 100, a, field);
  out[4] = C(e, U8, 24, J(e, Ref, 10, child), base);
  out[5] = C(e, U8, 11, child, iface) + 2 * C(e, U8, 11, base, child);
  Ref shared = C(e, Ref, 144, child, "shared", "I");
  out[6] = C(e, int, 150, child, shared);
  Ref equals = C(e, Ref, 33, object, "equals", "(Ljava/lang/Object;)Z");
  args[0] = b;
  if (form == 3)
    return C(e, U8, 69, a, child, equals, args);
  out[7] = C(e, U8, 39, a, equals, args);
  out[8] = C(e, U8, 69, a, object, equals, args);
  Ref literal = C(e, Ref, 113, language, "literal", "()Ljava/lang/String;");
  Ref text = C(e, Ref, 116, language, literal, (void *)0);
  U16 units[] = {'A', 0, 0xd83d, 0xde00, 0xd800, 0xffff, 'Z'};
  Ref native = C(e, Ref, 163, units, 7);
  Ref intern = C(e, Ref, 33, string, "intern", "()Ljava/lang/String;");
  Ref interned = C(e, Ref, 36, native, intern, (void *)0);
  out[9] = C(e, U8, 24, text, interned);
  out[10] = (unsigned)C(e, int, 51, native,
                        J(e, Ref, 33, string, "hashCode", "()I"), (void *)0);
  Ref name = C(e, Ref, 36, a,
               J(e, Ref, 33, base, "name", "()Ljava/lang/String;"), (void *)0);
  out[11] = C(e, int, 164, name);
  C(e, void, 143, language, J(e, Ref, 113, language, "escape", "()V"),
    (void *)0);
  CHECK(C(e, U8, 228));
  Ref thrown = C(e, Ref, 15);
  C(e, void, 17);
  Ref saved = C(e, Ref, 145, language,
                J(e, Ref, 144, language, "saved", "Ljava/lang/Throwable;"));
  out[12] = C(e, U8, 24, thrown, saved);
  Ref problem = C(e, Ref, 6, "runtime/language/Problem");
  out[13] = C(e, int, 100, thrown, J(e, Ref, 94, problem, "code", "I"));
  Ref throwable = C(e, Ref, 6, "java/lang/Throwable");
  Ref message_method =
      C(e, Ref, 33, throwable, "getMessage", "()Ljava/lang/String;");
  Ref message = C(e, Ref, 36, thrown, message_method, (void *)0);
  args[0] = C(e, Ref, 167, "detail");
  out[14] = C(e, U8, 39, message,
              J(e, Ref, 33, string, "equals", "(Ljava/lang/Object;)Z"), args);
  C(e, int, 13, thrown);
  Ref again = C(e, Ref, 15);
  C(e, void, 17);
  out[15] = C(e, U8, 24, thrown, again);
  out[16] = C(e, U8, 24, J(e, Ref, 31, thrown), problem);
  Ref exception = C(e, Ref, 6, "java/lang/RuntimeException");
  Ref ex = C(e, Ref, 30, exception,
             J(e, Ref, 33, exception, "<init>", "(Ljava/lang/String;)V"), args);
  out[17] = C(e, U8, 32, ex, throwable);
  out[18] = C(e, U8, 24, J(e, Ref, 36, ex, message_method, (void *)0), args[0]);
  Ref global = C(e, Ref, 21, text);
  CHECK(C(vm, int, 5) == 0);
  e = attach();
  language = C(e, Ref, 6, "runtime/language/Language");
  out[19] = C(e, U8, 24, global, J(e, Ref, 116, language, literal, (void *)0));
  C(e, void, 22, global);
  return 0;
}

#undef J

int jni_string_intern_identity(void) {
  Handle e = attach();
  Ref cls = C(e, Ref, 6, "java/lang/String");
  Ref intern = C(e, Ref, 33, cls, "intern", "()Ljava/lang/String;");
  Ref a = C(e, Ref, 167, "independent");
  Ref b = C(e, Ref, 167, "independent");
  CHECK(!C(e, U8, 24, a, b));
  Ref first = C(e, Ref, 36, a, intern, (void *)0);
  Ref second = C(e, Ref, 36, b, intern, (void *)0);
  CHECK(C(e, U8, 24, a, first) && C(e, U8, 24, first, second));
  Ref global = C(e, Ref, 21, first);
  CHECK(C(vm, int, 5) == 0);
  e = attach();
  Ref c = C(e, Ref, 167, "independent");
  Ref third = C(e, Ref, 36, c, intern, (void *)0);
  CHECK(C(e, U8, 24, global, third));
  C(e, void, 22, global);
  return 0;
}

/* Lookup failure is an explicit external-loader result, not a missing DEX
 * guess. */
int jni_explicit_lookup_failure(const char *type) {
  Handle e = attach();
  CHECK(e);
  CHECK(!C(e, Ref, 6, "fixture/Absent"));
  CHECK(C(e, int, 228));
  Ref thrown = C(e, Ref, 15);
  CHECK(thrown);
  C(e, void, 17);
  Ref klass = C(e, Ref, 6, type);
  CHECK(C(e, int, 32, thrown, klass));
  Ref wrong = C(e, Ref, 6, "java/lang/RuntimeException");
  CHECK(!C(e, int, 32, thrown, wrong));
  CHECK(!C(e, Ref, 6, "fixture/Absent"));
  Ref second = C(e, Ref, 15);
  C(e, void, 17);
  CHECK(!C(e, int, 24, thrown, second));
  C(e, void, 23, second);
  C(e, void, 23, thrown);
  CHECK(!C(e, int, 228));
  CHECK(!C(vm, int, 5));
  return 0;
}
struct NativeBinding {
  const char *name, *signature;
  U64 target;
};
static int registration_error(Handle e, const char *expected) {
  CHECK(C(e, int, 228));
  Ref exception = C(e, Ref, 15);
  C(e, void, 17);
  Ref type = C(e, Ref, 6, "java/lang/NoSuchMethodError");
  CHECK(C(e, int, 32, exception, type));
  Ref parent = C(e, Ref, 6, "java/lang/IncompatibleClassChangeError");
  CHECK(C(e, int, 32, exception, parent));
  Ref get_message = C(e, Ref, 33, type, "getMessage", "()Ljava/lang/String;");
  Ref message = C(e, Ref, 34, exception, get_message);
  const char *text = C(e, const char *, 169, message, (void *)0);
  unsigned i = 0;
  do {
    CHECK(text[i] == expected[i]);
  } while (expected[i++]);
  C(e, void, 170, message, text);
  return 0;
}
int jni_native_registration(unsigned mode) {
  Handle e = attach();
  CHECK(e);
  Ref child = C(e, Ref, 6, "registration/Child");
  Ref parent = C(e, Ref, 6, "registration/Parent");
  Ref deferred = C(e, Ref, 6, "registration/Deferred");
  /* Deferred.<clinit> throws: neither FindClass nor registration may run it. */
  CHECK(child && parent && deferred && !C(e, int, 228));
  struct NativeBinding all[] = {{"alpha", "(I)I", 0x12340000},
                                {"beta", "(J)J", 0x12340004},
                                {"child", "()V", 0x12340008}};
  CHECK(!C(e, int, 215, child, all, 3));
  struct NativeBinding after = {"after", "()V", 0x1234000c};
  CHECK(!C(e, int, 215, deferred, &after, 1));
  CHECK(!C(e, int, 228));
  CHECK(!C(e, int, 215, child, (void *)0, 0));
  if (mode == 3) {
    struct NativeBinding bad[] = {{"alpha", "!(I)I", 0x12340010},
                                  {"missing", "()V", 0x12340014}};
    CHECK(C(e, int, 215, child, bad, 2) == -1);
    CHECK(!registration_error(
        e,
        "no static or non-static method \"Lregistration/Child;.missing()V\""));
    struct NativeBinding ordinary = {"ordinary", "()V", 0x12340018};
    CHECK(C(e, int, 215, child, &ordinary, 1) == -1);
    CHECK(!registration_error(
        e, "no native method \"Lregistration/Child;.ordinary()V\""));
  } else if (mode == 4) {
    struct NativeBinding twice[] = {{"alpha", "(I)I", 0x12340010},
                                    {"alpha", "(I)I", 0x12340014}};
    CHECK(!C(e, int, 215, parent, twice, 2));
  } else if (mode == 5) {
    CHECK(!C(e, Ref, 113, deferred, "after", "()V"));
    CHECK(C(e, int, 228));
    Ref exception = C(e, Ref, 15);
    C(e, void, 17);
    Ref type = C(e, Ref, 6, "java/lang/ExceptionInInitializerError");
    CHECK(C(e, int, 32, exception, type));
    /* Resolution of an erroneous class still returns its class identity. */
    Ref again = C(e, Ref, 6, "registration/Deferred");
    CHECK(C(e, int, 24, deferred, again));
  } else if (mode >= 6) {
    if (mode == 6)
      C(e, int, 215, child, all, -1);
    if (mode == 7)
      C(e, int, 215, child, (void *)0, 1);
    if (mode == 8)
      C(e, int, 215, (Ref)0, all, 1);
    if (mode == 9) {
      all[0].target = 0x1234001c;
      all[1].target = 0;
      C(e, int, 215, child, all, 2);
    }
    if (mode == 10) {
      all[0].target = 0x1234001c;
      all[1].signature = "invalid";
      C(e, int, 215, child, all, 2);
    }
    if (mode == 11)
      C(e, int, 215, child, all, 4097);
    return 999;
  }
  if (mode == 1)
    CHECK(!C(e, int, 216, child));
  if (mode == 2)
    CHECK(!C(e, int, 216, parent));
  CHECK(!C(vm, int, 5));
  e = attach();
  CHECK(e && !C(e, int, 228));
  CHECK(!C(vm, int, 5));
  return 0;
}
