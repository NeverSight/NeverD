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
extern int usleep(unsigned);
struct ClockTime {
  long seconds, nanoseconds;
};
extern int clock_gettime(int, struct ClockTime *);
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
static int same_ref(Handle e, Ref first, Ref second) {
  return C(e, int, 24, first, second);
}
static Ref stat_logger_new_v(Handle e, Ref type, Ref constructor, ...) {
  __builtin_va_list args;
  __builtin_va_start(args, constructor);
  Ref result = C(e, Ref, 29, type, constructor, args);
  __builtin_va_end(args);
  return result;
}
static U64 stat_logger_time_v(Handle e, Ref logger, Ref method, ...) {
  __builtin_va_list args;
  __builtin_va_start(args, method);
  U64 result = C(e, U64, 53, logger, method, args);
  __builtin_va_end(args);
  return result;
}
int jni_stat_logger(unsigned mode) {
  Handle e = attach();
  CHECK(e);
  Ref type = C(e, Ref, 6, "com/android/internal/util/StatLogger");
  Ref init = C(e, Ref, 33, type, "<init>", "([Ljava/lang/String;)V");
  Ref time = C(e, Ref, 33, type, "getTime", "()J");
  CHECK(type && init && time);
  if (mode == 2)
    return C(e, Ref, 33, type, "logDurationStat", "(IJ)J");
  if (mode == 1) {
    CHECK(!C(e, Ref, 28, type, init, (Ref)0));
    Ref exception = C(e, Ref, 15);
    C(e, void, 17);
    Ref npe = C(e, Ref, 6, "java/lang/NullPointerException");
    CHECK(exception && C(e, int, 32, exception, npe));
    return C(vm, int, 5);
  }
  Ref string = C(e, Ref, 6, "java/lang/String");
  Ref size = C(e, Ref, 94, type, "SIZE", "I");
  Ref next = C(e, Ref, 94, type, "mNextTickTime", "J");
  Ref lock_field = C(e, Ref, 94, type, "mLock", "Ljava/lang/Object;");
  Ref labels_field = C(e, Ref, 94, type, "mLabels", "[Ljava/lang/String;");
  const char *names[] = {"mCountStats",        "mDurationStats",
                         "mCallsPerSecond",    "mMaxCallsPerSecond",
                         "mDurationPerSecond", "mMaxDurationPerSecond",
                         "mMaxDurationStats"};
  Ref prior_lock = 0, identities[21];
  unsigned count = 0;
  Ref logger = 0, labels = 0;
  for (unsigned form = 0; form != 3; ++form) {
    const int length = form == 0 ? 0 : 2;
    labels = C(e, Ref, 172, length, string, (Ref)0);
    U64 args[] = {labels};
    struct ClockTime before, after;
    CHECK(!clock_gettime(7, &before));
    logger = form == 0   ? C(e, Ref, 28, type, init, labels)
             : form == 1 ? stat_logger_new_v(e, type, init, labels)
                         : C(e, Ref, 30, type, init, args);
    CHECK(!clock_gettime(7, &after));
    CHECK(logger && C(e, int, 100, logger, size) == length);
    U64 tick = C(e, U64, 101, logger, next);
    CHECK((U64)before.seconds * 1000 + before.nanoseconds / 1000000 + 1000 <=
              tick &&
          tick <=
              (U64)after.seconds * 1000 + after.nanoseconds / 1000000 + 1000);
    Ref lock = C(e, Ref, 95, logger, lock_field);
    CHECK(lock && !same_ref(e, lock, prior_lock));
    prior_lock = lock;
    CHECK(same_ref(e, C(e, Ref, 95, logger, labels_field), labels));
    for (unsigned i = 0; i != 7; ++i) {
      const int is_int = i == 0 || i == 2 || i == 3;
      Ref field = C(e, Ref, 94, type, names[i], is_int ? "[I" : "[J");
      Ref array = C(e, Ref, 95, logger, field);
      CHECK(array && C(e, int, 171, array) == length);
      for (unsigned j = 0; j != count; ++j)
        CHECK(!same_ref(e, array, identities[j]));
      identities[count++] = array;
      if (is_int) {
        int values[2] = {-1, -1};
        C(e, void, 203, array, 0, length, values);
        CHECK(!length || (!values[0] && !values[1]));
        values[0] = 17;
        C(e, void, 211, array, 0, length, values);
      } else {
        U64 values[2] = {~0ULL, ~0ULL};
        C(e, void, 204, array, 0, length, values);
        CHECK(!length || (!values[0] && !values[1]));
        values[0] = 0x123456789abcdefULL;
        C(e, void, 212, array, 0, length, values);
      }
      CHECK(same_ref(e, C(e, Ref, 95, logger, field), array));
    }
    CHECK(!clock_gettime(7, &before));
    U64 micros = form == 0   ? C(e, U64, 52, logger, time)
                 : form == 1 ? stat_logger_time_v(e, logger, time)
                             : C(e, U64, 54, logger, time, (void *)0);
    CHECK(!clock_gettime(7, &after));
    CHECK((U64)before.seconds * 1000000 + before.nanoseconds / 1000 <= micros &&
          micros <= (U64)after.seconds * 1000000 + after.nanoseconds / 1000);
  }
  Ref saved_logger = C(e, Ref, 21, logger),
      saved_labels = C(e, Ref, 21, labels);
  CHECK(!C(vm, int, 5));
  e = attach();
  CHECK(e);
  Ref value = C(e, Ref, 167, "changed");
  C(e, void, 174, saved_labels, 1, value);
  Ref retained = C(e, Ref, 95, saved_logger, labels_field);
  CHECK(same_ref(e, retained, saved_labels));
  CHECK(same_ref(e, C(e, Ref, 173, retained, 1), value));
  CHECK(!C(e, Ref, 173, retained, 0));
  C(e, void, 22, saved_logger);
  C(e, void, 22, saved_labels);
  CHECK(!C(e, int, 228));
  return C(vm, int, 5);
}
static Ref map_get_v(Handle e, Ref map, Ref method, ...) {
  __builtin_va_list args;
  __builtin_va_start(args, method);
  Ref result = C(e, Ref, 35, map, method, args);
  __builtin_va_end(args);
  return result;
}
int jni_hash_map(unsigned mode) {
  Handle e = attach();
  CHECK(e);
  Ref type = C(e, Ref, 6, "java/util/HashMap");
  Ref init = C(e, Ref, 33, type, "<init>", "()V");
  Ref put = C(e, Ref, 33, type, "put",
              "(Ljava/lang/Object;Ljava/lang/Object;)Ljava/lang/Object;");
  Ref get =
      C(e, Ref, 33, type, "get", "(Ljava/lang/Object;)Ljava/lang/Object;");
  Ref size = C(e, Ref, 33, type, "size", "()I");
  Ref contains = C(e, Ref, 33, type, "containsKey", "(Ljava/lang/Object;)Z");
  Ref remove =
      C(e, Ref, 33, type, "remove", "(Ljava/lang/Object;)Ljava/lang/Object;");
  Ref clear = C(e, Ref, 33, type, "clear", "()V");
  Ref empty = C(e, Ref, 33, type, "isEmpty", "()Z");
  Ref map = mode == 2 ? C(e, Ref, 27, type) : C(e, Ref, 28, type, init);
  CHECK(map && get && size && put && contains && remove && clear && empty);
  if (mode == 1)
    return C(e, Ref, 34, map, get, map); // Unmodeled custom key hashing.
  if (mode == 2)
    return C(e, int, 49, map,
             size); // Raw AllocObject is not a constructed map.
  if (mode == 3) {
    Ref equals = C(e, Ref, 33, type, "equals", "(Ljava/lang/Object;)Z");
    return C(e, int, 37, map, equals, map);
  }
  Ref other = C(e, Ref, 30, type, init, (void *)0);
  Ref key = C(e, Ref, 167, "Aa"), equal = C(e, Ref, 167, "Aa");
  Ref collision = C(e, Ref, 167, "BB");
  CHECK(C(e, int, 37, map, empty) && !C(e, int, 49, map, size));
  CHECK(!C(e, Ref, 34, map, put, key, map));
  U64 args[] = {collision, other};
  CHECK(!C(e, Ref, 36, map, put, args));
  CHECK(same_ref(e, map_get_v(e, map, get, equal), map));
  CHECK(same_ref(e, C(e, Ref, 34, map, put, equal, (Ref)0), map));
  CHECK(C(e, int, 49, map, size) == 2);
  CHECK(C(e, int, 37, map, contains, key));
  CHECK(!C(e, Ref, 34, map, get, key));
  CHECK(!C(e, int, 37, other, contains, key));
  CHECK(!C(e, Ref, 34, map, put, (Ref)0, other));
  CHECK(same_ref(e, C(e, Ref, 34, map, get, (Ref)0), other));
  CHECK(same_ref(e, C(e, Ref, 34, map, remove, (Ref)0), other));
  CHECK(!C(e, Ref, 34, map, remove, (Ref)0));
  Ref saved_map = C(e, Ref, 21, map), saved_other = C(e, Ref, 21, other);
  CHECK(!C(vm, int, 5));
  e = attach();
  CHECK(e);
  collision = C(e, Ref, 167, "BB");
  Ref iface = C(e, Ref, 6, "java/util/Map");
  Ref interface_get =
      C(e, Ref, 33, iface, "get", "(Ljava/lang/Object;)Ljava/lang/Object;");
  CHECK(same_ref(e, C(e, Ref, 34, saved_map, interface_get, collision),
                 saved_other));
  C(e, void, 61, saved_map, clear);
  CHECK(C(e, int, 37, saved_map, empty));
  CHECK(!C(e, int, 49, saved_map, size));
  C(e, void, 22, saved_map);
  C(e, void, 22, saved_other);
  CHECK(!C(e, int, 228));
  return C(vm, int, 5);
}
static int property_int_v(Handle e, Ref type, Ref method, ...) {
  __builtin_va_list args;
  __builtin_va_start(args, method);
  int result = C(e, int, 130, type, method, args);
  __builtin_va_end(args);
  return result;
}
extern const void *__system_property_find(const char *);
extern int mprotect(void *, U64, int);
extern int munmap(void *, U64);
int jni_property_int(unsigned mode) {
  Handle e = attach();
  CHECK(e);
  Ref type = C(e, Ref, 6, "android/os/SystemProperties");
  Ref method = C(e, Ref, 113, type, "getInt", "(Ljava/lang/String;I)I");
  CHECK(type && method);
  if (mode == 1 || mode == 2) {
    const void *record = __system_property_find("test.valid");
    CHECK(record);
    void *page = (void *)((U64)record & ~4095ULL);
    if (mode == 1) {
      CHECK(!mprotect(page, 4096, 3));
      *(volatile char *)record ^= 1;
    } else {
      CHECK(!munmap(page, 4096));
    }
    Ref key = C(e, Ref, 167, "test.valid");
    return C(e, int, 129, type, method, key, 71);
  }
  if (mode == 3)
    return C(e, Ref, 113, type, "set",
             "(Ljava/lang/String;Ljava/lang/String;)V");
  const char *names[] = {"test.valid",     "test.hex",         "test.min",
                         "test.max",       "test.octal",       "test.sign",
                         "test.empty",     "test.missing",     "test.overflow",
                         "test.underflow", "test.trailing",    "test.signedhex",
                         "test.spacehex",  "test.hexoverflow", ""};
  const int expected[] = {28,         28,  (-2147483647 - 1),
                          2147483647, 28,  -28,
                          -41,        -41, -41,
                          -41,        -41, -41,
                          -41,        -41, -41};
  for (unsigned i = 0; i != sizeof(expected) / sizeof(expected[0]); ++i) {
    Ref key = C(e, Ref, 167, names[i]);
    U64 args[] = {key, (unsigned)-41};
    int value = i % 3 == 0   ? C(e, int, 129, type, method, key, -41)
                : i % 3 == 1 ? property_int_v(e, type, method, key, -41)
                             : C(e, int, 131, type, method, args);
    CHECK(value == expected[i]);
  }
  char native[92];
  CHECK(__system_property_get("test.valid", native) == 2);
  CHECK(native[0] == '2' && native[1] == '8' && !native[2]);
  U16 encoded_key[] = {0, 0xd800, 'a', 0xd83d, 0xde00};
  Ref encoded = C(e, Ref, 163, encoded_key, 5);
  CHECK(C(e, int, 129, type, method, encoded, 41) == -7);
  (void)C(e, int, 129, type, method, (Ref)0, 41);
  CHECK(C(e, int, 228));
  Ref exception = C(e, Ref, 15);
  C(e, void, 17);
  Ref npe = C(e, Ref, 6, "java/lang/NullPointerException");
  CHECK(C(e, int, 32, exception, npe));
  return C(vm, int, 5);
}
static U64 clock_long_v(Handle e, Ref type, Ref method, ...) {
  __builtin_va_list args;
  __builtin_va_start(args, method);
  U64 result = C(e, U64, 133, type, method, args);
  __builtin_va_end(args);
  return result;
}
U64 jni_system_clock(unsigned mode) {
  Handle e = attach();
  CHECK(e);
  Ref type = C(e, Ref, 6, "android/os/SystemClock");
  CHECK(type);
  const char *names[] = {"uptimeMillis", "elapsedRealtime",
                         "elapsedRealtimeNanos", "currentThreadTimeMillis"};
  if (mode >= 1 && mode <= 4) {
    Ref method = C(e, Ref, 113, type, names[mode - 1], "()J");
    U64 value = C(e, U64, 132, type, method);
    CHECK(!C(vm, int, 5));
    return value;
  }
  Ref methods[3];
  for (unsigned i = 0; i != 3; ++i) {
    methods[i] = C(e, Ref, 113, type, names[i], "()J");
    CHECK(methods[i]);
  }
  for (unsigned round = 0; round != 2; ++round) {
    U64 values[3], before[3], after[3];
    for (unsigned i = 0; i != 3; ++i) {
      struct ClockTime time;
      CHECK(!clock_gettime(i ? 7 : 1, &time));
      before[i] = (U64)time.seconds * 1000000000 + time.nanoseconds;
      values[i] = i == 0   ? C(e, U64, 132, type, methods[i])
                  : i == 1 ? clock_long_v(e, type, methods[i])
                           : C(e, U64, 134, type, methods[i], (void *)0);
      CHECK(!clock_gettime(i ? 7 : 1, &time));
      after[i] = (U64)time.seconds * 1000000000 + time.nanoseconds;
      U64 divisor = i == 2 ? 1 : 1000000;
      CHECK(before[i] / divisor <= values[i] &&
            values[i] <= after[i] / divisor);
    }
    if (mode == 0) {
      U64 advance = round * 2500000ULL;
      CHECK(values[0] == (10123456789ULL + advance) / 1000000);
      CHECK(values[1] == (50987654321ULL + advance) / 1000000);
      CHECK(values[2] == 50987654321ULL + advance);
      CHECK(after[0] == 10123456789ULL + advance);
      CHECK(after[2] == 50987654321ULL + advance);
    } else {
      CHECK(before[0] < after[0] && before[2] < values[2] &&
            values[2] < after[2]);
    }
    if (!round)
      CHECK(!usleep(2500));
  }
  CHECK(!C(e, int, 228));
  return C(vm, int, 5);
}
static Ref locale_saved, locale_type, locale_get_default;
static Ref posted_handler, post_method;
static void *post_thread(void *unused) {
  (void)unused;
  Handle e = attach();
  if (!e)
    return (void *)1;
  U64 arguments[] = {0};
  int result = C(e, int, 39, posted_handler, post_method, arguments);
  return (void *)(U64)(result != 1 || C(vm, int, 5));
}
int jni_handler_post(int mode) {
  Handle e = attach();
  CHECK(e);
  Ref looper_type = C(e, Ref, 6, "android/os/Looper");
  Ref main =
      C(e, Ref, 113, looper_type, "getMainLooper", "()Landroid/os/Looper;");
  Ref looper = C(e, Ref, 114, looper_type, main);
  Ref handler_type = C(e, Ref, 6, "android/os/Handler");
  Ref init = C(e, Ref, 33, handler_type, "<init>", "(Landroid/os/Looper;)V");
  Ref handler = C(e, Ref, 28, handler_type, init, looper);
  if (mode == 1)
    handler = C(e, Ref, 27, handler_type);
  post_method = C(e, Ref, 33, handler_type, "post", "(Ljava/lang/Runnable;)Z");
  CHECK(handler && post_method);
  CHECK(C(e, int, 37, handler, post_method, (Ref)0) == 1);
  CHECK(usleep(2000) == 0);
  U64 arguments[] = {0};
  CHECK(C(e, int, 39, handler, post_method, arguments) == 1);
  posted_handler = C(e, Ref, 21, handler);
  CHECK(C(vm, int, 5) == 0);
  U64 child = 0;
  void *result = (void *)1;
  CHECK(pthread_create(&child, (void *)0, post_thread, (void *)0) == 0);
  CHECK(pthread_join(child, &result) == 0 && !result);
  e = attach();
  CHECK(e);
  C(e, void, 22, posted_handler);
  return C(vm, int, 5);
}
static Ref main_looper_type, main_looper_get, main_looper_saved;
static void *main_looper_thread(void *unused) {
  (void)unused;
  Handle e = attach();
  if (!e)
    return (void *)1;
  Ref value = C(e, Ref, 114, main_looper_type, main_looper_get);
  int mismatch = !value || !C(e, int, 24, value, main_looper_saved);
  return (void *)(U64)(mismatch || C(vm, int, 5));
}
int jni_main_looper(int mode) {
  Handle e = attach();
  CHECK(e);
  Ref type = C(e, Ref, 6, "android/os/Looper");
  main_looper_type = C(e, Ref, 21, type);
  main_looper_get =
      C(e, Ref, 113, type, "getMainLooper", "()Landroid/os/Looper;");
  Ref value = C(e, Ref, 114, type, main_looper_get);
  Ref handler = C(e, Ref, 6, "android/os/Handler");
  Ref init = C(e, Ref, 33, handler, "<init>", "(Landroid/os/Looper;)V");
  if (mode == 1) {
    CHECK(!value);
    CHECK(!C(e, Ref, 28, handler, init, (Ref)0));
    CHECK(C(e, int, 228));
    C(e, void, 17);
    return C(vm, int, 5);
  }
  CHECK(value);
  main_looper_saved = C(e, Ref, 21, value);
  Ref again = C(e, Ref, 116, type, main_looper_get, (void *)0);
  CHECK(C(e, int, 24, value, again));
  if (mode == 2)
    value = C(e, Ref, 27, type);
  Ref first = C(e, Ref, 28, handler, init, value);
  U64 args[] = {value};
  Ref second = C(e, Ref, 30, handler, init, args);
  CHECK(first && second && !C(e, int, 24, first, second));
  Ref stored_looper = C(e, Ref, 94, handler, "mLooper", "Landroid/os/Looper;");
  Ref first_looper = C(e, Ref, 95, first, stored_looper);
  Ref second_looper = C(e, Ref, 95, second, stored_looper);
  CHECK(C(e, int, 24, value, first_looper));
  CHECK(C(e, int, 24, value, second_looper));
  Ref source_queue = C(e, Ref, 94, type, "mQueue", "Landroid/os/MessageQueue;");
  Ref queue = C(e, Ref, 95, value, source_queue);
  CHECK((mode == 2) == !queue);
  Ref destination_queue =
      C(e, Ref, 94, handler, "mQueue", "Landroid/os/MessageQueue;");
  Ref retained_queue = C(e, Ref, 95, first, destination_queue);
  CHECK(C(e, int, 24, queue, retained_queue));
  Ref callback =
      C(e, Ref, 94, handler, "mCallback", "Landroid/os/Handler$Callback;");
  Ref async = C(e, Ref, 94, handler, "mAsynchronous", "Z");
  CHECK(!C(e, Ref, 95, first, callback));
  CHECK(!C(e, int, 96, first, async));
  Ref blank = C(e, Ref, 27, handler);
  CHECK(!C(e, Ref, 95, blank, stored_looper));
  U64 child = 0;
  void *result = (void *)1;
  CHECK(pthread_create(&child, (void *)0, main_looper_thread, (void *)0) == 0);
  CHECK(pthread_join(child, &result) == 0 && !result);
  C(e, void, 22, main_looper_saved);
  C(e, void, 22, main_looper_type);
  return C(vm, int, 5);
}
int jni_classloader_paths(int mode) {
  Handle e = attach();
  CHECK(e);
  Ref thread = C(e, Ref, 6, "android/app/ActivityThread");
  Ref current = C(e, Ref, 113, thread, "currentApplication",
                  "()Landroid/app/Application;");
  Ref app = C(e, Ref, 114, thread, current);
  Ref context = C(e, Ref, 6, "android/content/Context");
  Ref get_loader =
      C(e, Ref, 33, context, "getClassLoader", "()Ljava/lang/ClassLoader;");
  Ref app_loader = C(e, Ref, 34, app, get_loader);
  Ref loader_type = C(e, Ref, 6, "java/lang/ClassLoader");
  Ref get_system = C(e, Ref, 113, loader_type, "getSystemClassLoader",
                     "()Ljava/lang/ClassLoader;");
  Ref system = C(e, Ref, 114, loader_type, get_system);
  Ref actual_type = C(e, Ref, 31, app_loader);
  Ref render = C(e, Ref, 33, actual_type, "toString", "()Ljava/lang/String;");
  Ref base_type = C(e, Ref, 6, "dalvik/system/BaseDexClassLoader");
  CHECK(render == C(e, Ref, 33, base_type, "toString", "()Ljava/lang/String;"));
  if (mode == 1)
    app_loader = C(e, Ref, 27, actual_type);
  if (mode == 2)
    app_loader = system;
  Ref first = C(e, Ref, 34, app_loader, render);
  Ref second = C(e, Ref, 36, app_loader, render, (void *)0);
  CHECK(first && second && !C(e, int, 24, first, second));
  Ref system_text = C(e, Ref, 34, system, render);
  const char *expected[] = {
      "dalvik.system.PathClassLoader[DexPathList[[zip file \"/one.apk\"],"
      "nativeLibraryDirectories=[/app/lib, /system/lib]]]",
      "dalvik.system.PathClassLoader[DexPathList[[],"
      "nativeLibraryDirectories=[/system/lib]]]"};
  Ref values[] = {first, system_text};
  for (unsigned n = 0; n != 2; ++n) {
    const char *bytes = C(e, const char *, 169, values[n], (void *)0);
    CHECK(bytes);
    for (unsigned i = 0;; ++i) {
      CHECK(bytes[i] == expected[n][i]);
      if (!bytes[i])
        break;
    }
    C(e, void, 170, values[n], bytes);
  }
  return C(vm, int, 5);
}
static int check_locale_thread(void) {
  Handle e = attach();
  CHECK(e);
  Ref value = C(e, Ref, 114, locale_type, locale_get_default);
  CHECK(value && C(e, int, 24, value, locale_saved));
  return C(vm, int, 5);
}
static void *locale_thread(void *unused) {
  (void)unused;
  return (void *)(U64)check_locale_thread();
}
int jni_default_locale(int mode) {
  Handle e = attach();
  CHECK(e);
  Ref type = C(e, Ref, 6, "java/util/Locale");
  locale_get_default =
      C(e, Ref, 113, type, "getDefault", "()Ljava/util/Locale;");
  Ref value = C(e, Ref, 114, type, locale_get_default);
  CHECK(value);
  Ref again = C(e, Ref, 116, type, locale_get_default, (void *)0);
  CHECK(C(e, int, 24, value, again));
  Ref actual = C(e, Ref, 31, value);
  CHECK(C(e, int, 24, actual, type));
  if (mode == 3)
    value = C(e, Ref, 27, type);
  Ref language = C(e, Ref, 33, type, "getLanguage", "()Ljava/lang/String;");
  Ref country = C(e, Ref, 33, type, "getCountry", "()Ljava/lang/String;");
  Ref lang = C(e, Ref, 34, value, language);
  Ref region = C(e, Ref, 36, value, country, (void *)0);
  CHECK(lang && region);
  Ref strings = C(e, Ref, 6, "java/lang/String");
  Ref intern = C(e, Ref, 33, strings, "intern", "()Ljava/lang/String;");
  const char *names[] = {mode == 1 ? "" : "en", mode == 1 ? "" : "US"};
  Ref values[] = {lang, region};
  for (unsigned i = 0; i != 2; ++i) {
    Ref expected = C(e, Ref, 167, names[i]);
    expected = C(e, Ref, 34, expected, intern);
    CHECK(C(e, int, 24, expected, values[i]));
  }
  if (mode == 2) {
    C(e, Ref, 113, type, "setDefault", "(Ljava/util/Locale;)V");
    return __LINE__;
  }
  locale_saved = C(e, Ref, 21, value);
  locale_type = C(e, Ref, 21, type);
  U64 child = 0;
  void *result = (void *)1;
  CHECK(!pthread_create(&child, 0, locale_thread, 0));
  CHECK(!pthread_join(child, &result) && !result);
  C(e, void, 22, locale_saved);
  C(e, void, 22, locale_type);
  return C(vm, int, 5);
}
static Ref info_app, info_saved, info_value, info_getter, info_field;
static int check_info_thread(void) {
  Handle e = attach();
  CHECK(e);
  Ref repeated = C(e, Ref, 34, info_app, info_getter);
  CHECK(C(e, int, 24, repeated, info_saved));
  CHECK(!C(e, Ref, 95, repeated, info_field));
  C(e, void, 104, repeated, info_field, info_value);
  CHECK(!C(e, int, 228));
  return C(vm, int, 5);
}
static void *info_thread(void *unused) {
  (void)unused;
  return (void *)(U64)check_info_thread();
}
int jni_application_info(int mode, int native_directory) {
  Handle e = attach();
  CHECK(e);
  Ref thread = C(e, Ref, 6, "android/app/ActivityThread");
  Ref current = C(e, Ref, 113, thread, "currentApplication",
                  "()Landroid/app/Application;");
  Ref app = C(e, Ref, 114, thread, current);
  Ref type = C(e, Ref, 31, app);
  info_getter = C(e, Ref, 33, type, "getApplicationInfo",
                  "()Landroid/content/pm/ApplicationInfo;");
  Ref info = C(e, Ref, 34, app, info_getter);
  CHECK(info);
  Ref base_id =
      C(e, Ref, 33, type, "getBaseContext", "()Landroid/content/Context;");
  Ref base = C(e, Ref, 34, app, base_id);
  Ref again = C(e, Ref, 34, base, info_getter);
  CHECK(C(e, int, 24, info, again));
  Ref declared = C(e, Ref, 6, "android/content/pm/ApplicationInfo");
  Ref actual = C(e, Ref, 31, info);
  CHECK(C(e, int, 24, declared, actual));
  Ref parent = C(e, Ref, 6, "android/content/pm/PackageItemInfo");
  Ref parcelable = C(e, Ref, 6, "android/os/Parcelable");
  Ref superclass = C(e, Ref, 10, declared);
  CHECK(C(e, int, 24, parent, superclass));
  CHECK(C(e, int, 32, info, parcelable));
  const char *field_name = native_directory ? "nativeLibraryDir" : "sourceDir";
  info_field = C(e, Ref, 94, declared, field_name, "Ljava/lang/String;");
  CHECK(info_field && info_field == C(e, Ref, 94, declared, field_name,
                                      "Ljava/lang/String;"));
  if (mode == 2) {
    Ref written = C(e, Ref, 167, "written");
    C(e, void, 104, info, info_field, written);
  }
  Ref value = C(e, Ref, 95, info, info_field);
  CHECK(!!value == (mode != 1));
  Ref repeated = C(e, Ref, 95, info, info_field);
  CHECK(C(e, int, 24, value, repeated));
  if (value) {
    const char *expected = mode == 2          ? "written"
                           : native_directory ? "/test/lib/arm64"
                                              : "/test/base.apk";
    const char *bytes = C(e, const char *, 169, value, (void *)0);
    CHECK(bytes);
    for (unsigned i = 0;; ++i) {
      CHECK(bytes[i] == expected[i]);
      if (!bytes[i])
        break;
    }
    C(e, void, 170, value, bytes);
  }
  if (mode == 3) {
    C(e, Ref, 94, declared, "unmodeledField", "Ljava/lang/String;");
    return __LINE__;
  }
  info_app = C(e, Ref, 21, app);
  info_saved = C(e, Ref, 21, info);
  info_value = value ? C(e, Ref, 21, value) : 0;
  C(e, void, 104, info, info_field, (Ref)0);
  C(e, void, 23, info);
  CHECK(!C(vm, int, 5));
  U64 child = 0;
  void *result = (void *)1;
  CHECK(!pthread_create(&child, 0, info_thread, 0));
  CHECK(!pthread_join(child, &result) && !result);
  e = attach();
  CHECK(e);
  Ref retained = C(e, Ref, 95, info_saved, info_field);
  Ref retained_info = C(e, Ref, 34, info_app, info_getter);
  CHECK(C(e, int, 24, info_value, retained));
  CHECK(C(e, int, 24, info_saved, retained_info));
  C(e, void, 22, info_app);
  C(e, void, 22, info_saved);
  if (info_value)
    C(e, void, 22, info_value);
  CHECK(!C(e, int, 228));
  return C(vm, int, 5);
}
int jni_secure_key_constants(int unknown) {
  Handle e = attach();
  CHECK(e);
  Ref secure = C(e, Ref, 6, "android/provider/Settings$Secure");
  const char *names[] = {"ADB_ENABLED", "ANDROID_ID"};
  const char *values[] = {"adb_enabled", "android_id"};
  Ref string = C(e, Ref, 6, "java/lang/String");
  Ref intern = C(e, Ref, 33, string, "intern", "()Ljava/lang/String;");
  for (unsigned i = 0; i < 2; ++i) {
    Ref field = C(e, Ref, 144, secure, names[i], "Ljava/lang/String;");
    CHECK(field && !C(e, int, 228));
    CHECK(field == C(e, Ref, 144, secure, names[i], "Ljava/lang/String;"));
    Ref value = C(e, Ref, 145, secure, field);
    Ref fresh = C(e, Ref, 167, values[i]);
    CHECK(value && fresh && !C(e, int, 24, value, fresh));
    Ref literal = C(e, Ref, 34, fresh, intern);
    CHECK(C(e, int, 24, value, literal));
    const char *bytes = C(e, const char *, 169, value, (void *)0);
    CHECK(bytes);
    for (unsigned j = 0;; ++j) {
      CHECK(bytes[j] == values[i][j]);
      if (!bytes[j])
        break;
    }
    C(e, void, 170, value, bytes);
  }
  if (unknown)
    C(e, Ref, 144, secure, "UNMODELED_KEY", "Ljava/lang/String;");
  return C(vm, int, 5);
}
int jni_settings_int(int expected, int mode) {
  Handle e = attach();
  CHECK(e);
  Ref thread = C(e, Ref, 6, "android/app/ActivityThread");
  Ref current = C(e, Ref, 113, thread, "currentApplication",
                  "()Landroid/app/Application;");
  Ref app = C(e, Ref, 114, thread, current);
  Ref context = C(e, Ref, 6, "android/content/Context");
  Ref getter = C(e, Ref, 33, context, "getContentResolver",
                 "()Landroid/content/ContentResolver;");
  Ref resolver = C(e, Ref, 34, app, getter);
  Ref secure = C(e, Ref, 6, "android/provider/Settings$Secure");
  Ref field = C(e, Ref, 144, secure, "ADB_ENABLED", "Ljava/lang/String;");
  Ref key = C(e, Ref, 145, secure, field);
  if (mode == 1)
    key = C(e, Ref, 167, "unmodeled_key");
  Ref read = C(e, Ref, 113, secure, "getInt",
               "(Landroid/content/ContentResolver;Ljava/lang/String;I)I");
  int value = C(e, int, 129, secure, read, mode == 2 ? 0 : resolver, key, -23);
  if (mode == 2) {
    CHECK(!value && C(e, int, 228));
    C(e, void, 17);
  } else {
    CHECK(!C(e, int, 228) && value == expected);
    U64 args[] = {resolver, key, (U64)-23};
    CHECK(C(e, int, 131, secure, read, args) == expected);
    CHECK(!C(e, int, 228));
  }
  return C(vm, int, 5);
}
int jni_secure_settings(int mode) {
  Handle e = attach();
  CHECK(e);
  Ref thread = C(e, Ref, 6, "android/app/ActivityThread");
  Ref current = C(e, Ref, 113, thread, "currentApplication",
                  "()Landroid/app/Application;");
  Ref app = C(e, Ref, 114, thread, current);
  Ref context = C(e, Ref, 6, "android/content/Context");
  Ref getter = C(e, Ref, 33, context, "getContentResolver",
                 "()Landroid/content/ContentResolver;");
  Ref resolver = C(e, Ref, 34, app, getter);
  Ref secure = C(e, Ref, 6, "android/provider/Settings$Secure");
  Ref read = C(e, Ref, 113, secure, "getString",
               "(Landroid/content/ContentResolver;Ljava/lang/String;)Ljava/"
               "lang/String;");
  Ref name = C(e, Ref, 167, mode == 2 ? "unmodeled_key" : "android_id");
  Ref value = C(e, Ref, 114, secure, read, mode == 3 ? 0 : resolver, name);
  if (mode == 3) {
    CHECK(!value && C(e, int, 228));
    C(e, void, 17);
  } else {
    CHECK(!C(e, int, 228));
    if (mode == 1) {
      CHECK(!value);
    } else {
      CHECK(value);
      U64 args[] = {resolver, name};
      Ref again = C(e, Ref, 116, secure, read, args);
      CHECK(again && C(e, int, 24, value, again));
      const char *bytes = C(e, const char *, 169, value, (void *)0);
      const char expected[] = "fixture";
      CHECK(bytes);
      for (unsigned i = 0; i < sizeof(expected); ++i)
        CHECK(bytes[i] == expected[i]);
      C(e, void, 170, value, bytes);
    }
  }
  return C(vm, int, 5);
}
int jni_string_builder(int unknown) {
  Handle e = attach();
  CHECK(e);
  Ref type = C(e, Ref, 6, "java/lang/StringBuilder");
  Ref ctor = C(e, Ref, 33, type, "<init>", "()V");
  Ref add = C(e, Ref, 33, type, "append", "(C)Ljava/lang/StringBuilder;");
  Ref text = C(e, Ref, 33, type, "toString", "()Ljava/lang/String;");
  Ref builder = C(e, Ref, 28, type, ctor);
  CHECK(builder);
  Ref empty = C(e, Ref, 34, builder, text);
  Ref empty_again = C(e, Ref, 34, builder, text);
  CHECK(empty && C(e, int, 24, empty, empty_again));
  const U16 expected[] = {0, 0xd800, 0xdc00, 0x4e2d};
  for (unsigned i = 0; i < 4; ++i) {
    U64 arg = expected[i];
    Ref returned = i & 1 ? C(e, Ref, 36, builder, add, &arg)
                         : C(e, Ref, 34, builder, add, (int)arg);
    CHECK(returned && C(e, int, 24, returned, builder));
  }
  Ref first = C(e, Ref, 34, builder, text);
  Ref second = C(e, Ref, 34, builder, text);
  CHECK(first && second && !C(e, int, 24, first, second));
  C(e, Ref, 34, builder, add, 'X');
  CHECK(C(e, int, 164, first) == 4);
  const U16 *chars = C(e, const U16 *, 165, first, (void *)0);
  CHECK(chars);
  for (unsigned i = 0; i < 4; ++i)
    CHECK(chars[i] == expected[i]);
  C(e, void, 166, first, chars);
  const char *interfaces[] = {"java/io/Serializable", "java/lang/Appendable",
                              "java/lang/CharSequence"};
  for (unsigned i = 0; i < 3; ++i) {
    Ref interface = C(e, Ref, 6, interfaces[i]);
    CHECK(C(e, int, 32, builder, interface));
  }
  Ref parent = C(e, Ref, 10, type);
  Ref abstract = C(e, Ref, 6, "java/lang/AbstractStringBuilder");
  CHECK(C(e, int, 24, parent, abstract));
  CHECK(!C(e, int, 228));
  if (unknown) {
    C(e, Ref, 33, type, "capacity", "()I");
    return __LINE__;
  }
  return C(vm, int, 5);
}
static int loader_failure(const char *query, const char *jni_name,
                          int unknown) {
  Handle e = attach();
  CHECK(e);
  Ref type = C(e, Ref, 6, "java/lang/ClassLoader");
  Ref system_id =
      C(e, Ref, 113, type, "getSystemClassLoader", "()Ljava/lang/ClassLoader;");
  Ref system = C(e, Ref, 114, type, system_id);
  Ref parent_id = C(e, Ref, 33, type, "getParent", "()Ljava/lang/ClassLoader;");
  Ref boot = C(e, Ref, 34, system, parent_id);
  Ref load_id =
      C(e, Ref, 33, type, "loadClass", "(Ljava/lang/String;)Ljava/lang/Class;");
  Ref name = C(e, Ref, 167, query);
  Ref global_loader = C(e, Ref, 21, boot);
  for (int i = 0; i < 2; ++i) {
    CHECK(!C(e, Ref, 34, global_loader, load_id, name));
    CHECK(C(e, int, 228));
    Ref failure = C(e, Ref, 15);
    Ref repeated = C(e, Ref, 15);
    C(e, void, 17);
    CHECK(failure && C(e, int, 24, failure, repeated));
    Ref exception_type = C(e, Ref, 6, "java/lang/ClassNotFoundException");
    CHECK(C(e, int, 32, failure, exception_type));
    Ref throwable = C(e, Ref, 6, "java/lang/Throwable");
    Ref message_id =
        C(e, Ref, 33, throwable, "getMessage", "()Ljava/lang/String;");
    Ref message = C(e, Ref, 34, failure, message_id);
    const char *text = C(e, const char *, 169, message, (void *)0);
    const char expected[] = "declared missing";
    CHECK(text);
    for (unsigned j = 0; j < sizeof(expected); ++j)
      CHECK(text[j] == expected[j]);
    C(e, void, 170, message, text);
    CHECK(!C(vm, int, 5));
    e = attach();
    CHECK(e);
    name = C(e, Ref, 167, query);
  }
  if (unknown) {
    // Boot failure must not close the system namespace or affect FindClass.
    if (unknown == 1) {
      type = C(e, Ref, 6, "java/lang/ClassLoader");
      system = C(e, Ref, 114, type, system_id);
      C(e, Ref, 34, system, load_id, name);
    } else if (unknown == 2) {
      C(e, Ref, 6, jni_name);
    } else {
      Ref trimmed = C(e, Ref, 167, "fixture.Optional");
      C(e, Ref, 34, global_loader, load_id, trimmed);
    }
    return 1;
  }
  Ref known = C(e, Ref, 167, "java.lang.String");
  CHECK(C(e, Ref, 34, global_loader, load_id, known));
  CHECK(!C(e, int, 228));
  C(e, void, 22, global_loader);
  return C(vm, int, 5);
}

int jni_loader_failure(int unknown) {
  return loader_failure("fixture.Optional", "fixture/Optional", unknown);
}
int jni_loader_exact_query(int unknown) {
  return loader_failure("fixture.Optional ", "fixture/Optional ", unknown);
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

static Ref resolver_app, resolver_saved;
static int check_resolver_thread(void) {
  Handle e = attach();
  CHECK(e);
  Ref context = C(e, Ref, 6, "android/content/Context");
  Ref getter = C(e, Ref, 33, context, "getContentResolver",
                 "()Landroid/content/ContentResolver;");
  Ref repeated = C(e, Ref, 34, resolver_app, getter);
  CHECK(repeated && C(e, int, 24, repeated, resolver_saved));
  CHECK(!C(e, int, 228));
  return C(vm, int, 5);
}
static void *resolver_thread(void *unused) {
  (void)unused;
  return (void *)(U64)check_resolver_thread();
}
int jni_content_resolver(int request_provider_operation) {
  Handle e = attach();
  CHECK(e);
  Ref thread = C(e, Ref, 6, "android/app/ActivityThread");
  Ref current = C(e, Ref, 113, thread, "currentApplication",
                  "()Landroid/app/Application;");
  Ref app = C(e, Ref, 114, thread, current);
  CHECK(app);
  Ref type = C(e, Ref, 31, app);
  Ref getter = C(e, Ref, 33, type, "getContentResolver",
                 "()Landroid/content/ContentResolver;");
  Ref resolver = C(e, Ref, 34, app, getter);
  Ref wrapper = C(e, Ref, 6, "android/content/ContextWrapper");
  Ref base_id =
      C(e, Ref, 33, wrapper, "getBaseContext", "()Landroid/content/Context;");
  Ref base = C(e, Ref, 34, app, base_id);
  Ref again = C(e, Ref, 34, base, getter);
  CHECK(resolver && C(e, int, 24, resolver, again));
  Ref declared = C(e, Ref, 6, "android/content/ContentResolver");
  Ref concrete =
      C(e, Ref, 6, "android/app/ContextImpl$ApplicationContentResolver");
  Ref actual = C(e, Ref, 31, resolver);
  CHECK(C(e, int, 24, actual, concrete));
  Ref parent = C(e, Ref, 10, concrete);
  CHECK(C(e, int, 24, parent, declared));
  CHECK(C(e, int, 32, resolver, declared));
  if (request_provider_operation) {
    C(e, Ref, 33, declared, "getType", "(Landroid/net/Uri;)Ljava/lang/String;");
    return __LINE__;
  }
  resolver_app = C(e, Ref, 21, app);
  resolver_saved = C(e, Ref, 21, resolver);
  C(e, void, 23, resolver);
  C(e, void, 23, again);
  CHECK(!C(vm, int, 5));
  CHECK(!check_resolver_thread());
  U64 child = 0;
  void *result = (void *)1;
  CHECK(!pthread_create(&child, 0, resolver_thread, 0));
  CHECK(!pthread_join(child, &result) && !result);
  e = attach();
  CHECK(e);
  C(e, void, 22, resolver_app);
  C(e, void, 22, resolver_saved);
  CHECK(!C(e, int, 228));
  return C(vm, int, 5);
}

static Ref package_manager_app, package_manager_saved;
static Ref query_manager, query_first, query_name, query_getter, query_source;
static int query_text(Handle e, Ref string, const char *expected) {
  if (!string)
    return 0;
  const char *bytes = C(e, const char *, 169, string, (void *)0);
  if (!bytes)
    return 0;
  int equal = 1;
  for (unsigned i = 0; bytes[i] || expected[i]; ++i)
    if (bytes[i] != expected[i]) {
      equal = 0;
      break;
    }
  C(e, void, 170, string, bytes);
  return equal;
}
static void *query_thread(void *unused) {
  (void)unused;
  Handle e = attach();
  if (!e)
    return (void *)1;
  Ref fresh = C(e, Ref, 34, query_manager, query_getter, query_name, 0);
  Ref fresh_value = C(e, Ref, 95, fresh, query_source);
  Ref changed = C(e, Ref, 95, query_first, query_source);
  int ok = fresh && !C(e, int, 24, fresh, query_first) &&
           query_text(e, fresh_value, "/query.apk") &&
           query_text(e, changed, "changed");
  int detach = C(vm, int, 5);
  return (void *)(U64)(!ok || detach);
}
int jni_package_application_info(int mode) {
  Handle e = attach();
  CHECK(e);
  Ref thread = C(e, Ref, 6, "android/app/ActivityThread");
  Ref current = C(e, Ref, 113, thread, "currentApplication",
                  "()Landroid/app/Application;");
  Ref app = C(e, Ref, 114, thread, current);
  Ref context = C(e, Ref, 6, "android/content/Context");
  Ref manager_id = C(e, Ref, 33, context, "getPackageManager",
                     "()Landroid/content/pm/PackageManager;");
  Ref manager = C(e, Ref, 34, app, manager_id);
  Ref cls = C(e, Ref, 31, manager);
  query_getter = C(e, Ref, 33, cls, "getApplicationInfo",
                   "(Ljava/lang/String;I)Landroid/content/pm/ApplicationInfo;");
  CHECK(query_getter);
  if (mode == 5)
    return C(vm, int, 5);
  Ref name = C(e, Ref, 167, mode == 1 ? "other.app" : "test.app");
  if (mode == 3)
    name = 0;
  if (mode == 4)
    manager = C(e, Ref, 27, cls);
  Ref first = C(e, Ref, 34, manager, query_getter, name, 0);
  CHECK(first);
  U64 arguments[] = {name, 0};
  Ref second = C(e, Ref, 36, manager, query_getter, arguments);
  CHECK(second && !C(e, int, 24, first, second));
  Ref context_id = C(e, Ref, 33, context, "getApplicationInfo",
                     "()Landroid/content/pm/ApplicationInfo;");
  Ref context_info = C(e, Ref, 34, app, context_id);
  CHECK(context_info && !C(e, int, 24, first, context_info));
  Ref info = C(e, Ref, 6, "android/content/pm/ApplicationInfo");
  query_source = C(e, Ref, 94, info, "sourceDir", "Ljava/lang/String;");
  Ref native = C(e, Ref, 94, info, "nativeLibraryDir", "Ljava/lang/String;");
  Ref value = C(e, Ref, 95, first, query_source);
  CHECK(query_text(e, value, "/query.apk"));
  if (mode == 2) {
    C(e, Ref, 95, first, native);
    return __LINE__;
  }
  CHECK(!C(e, Ref, 95, first, native));
  Ref changed = C(e, Ref, 167, "changed");
  C(e, void, 104, first, query_source, changed);
  Ref unchanged = C(e, Ref, 95, second, query_source);
  Ref context_value = C(e, Ref, 95, context_info, query_source);
  CHECK(query_text(e, unchanged, "/query.apk"));
  CHECK(query_text(e, context_value, "/context.apk"));
  query_manager = C(e, Ref, 21, manager);
  query_first = C(e, Ref, 21, first);
  query_name = C(e, Ref, 21, name);
  C(e, void, 23, first);
  CHECK(!C(vm, int, 5));
  U64 child = 0;
  void *result = (void *)1;
  CHECK(!pthread_create(&child, 0, query_thread, 0));
  CHECK(!pthread_join(child, &result) && !result);
  e = attach();
  CHECK(e);
  C(e, void, 22, query_manager);
  C(e, void, 22, query_first);
  C(e, void, 22, query_name);
  CHECK(!C(e, int, 228));
  return C(vm, int, 5);
}
static Ref package_info_manager, package_info_first, package_info_name;
static Ref package_info_getter, package_info_version, package_info_code;
static void *package_info_thread(void *unused) {
  (void)unused;
  Handle e = attach();
  if (!e)
    return (void *)1;
  Ref fresh = C(e, Ref, 34, package_info_manager, package_info_getter,
                package_info_name, 0);
  Ref version = C(e, Ref, 95, fresh, package_info_version);
  Ref changed = C(e, Ref, 95, package_info_first, package_info_version);
  int code = C(e, int, 100, fresh, package_info_code);
  int mutated = C(e, int, 100, package_info_first, package_info_code);
  int ok = fresh && !C(e, int, 24, fresh, package_info_first) &&
           query_text(e, version, "fixture-version") &&
           query_text(e, changed, "changed") && code == 7 && mutated == -1;
  int detach = C(vm, int, 5);
  return (void *)(U64)(!ok || detach);
}
int jni_package_info(int mode) {
  Handle e = attach();
  CHECK(e);
  Ref thread = C(e, Ref, 6, "android/app/ActivityThread");
  Ref current = C(e, Ref, 113, thread, "currentApplication",
                  "()Landroid/app/Application;");
  Ref app = C(e, Ref, 114, thread, current);
  Ref context = C(e, Ref, 6, "android/content/Context");
  Ref manager_id = C(e, Ref, 33, context, "getPackageManager",
                     "()Landroid/content/pm/PackageManager;");
  Ref manager = C(e, Ref, 34, app, manager_id);
  Ref cls = C(e, Ref, 31, manager);
  package_info_getter =
      C(e, Ref, 33, cls, "getPackageInfo",
        "(Ljava/lang/String;I)Landroid/content/pm/PackageInfo;");
  CHECK(package_info_getter);
  if (mode == 4)
    return C(vm, int, 5);
  Ref name = C(e, Ref, 167, "test.app");
  if (mode == 2)
    name = 0;
  if (mode == 3)
    manager = C(e, Ref, 27, cls);
  Ref first =
      C(e, Ref, 34, manager, package_info_getter, name, mode == 1 ? 1 : 0);
  CHECK(first);
  U64 arguments[] = {name, 0};
  Ref second = C(e, Ref, 36, manager, package_info_getter, arguments);
  CHECK(second && !C(e, int, 24, first, second));
  Ref info = C(e, Ref, 6, "android/content/pm/PackageInfo");
  Ref package = C(e, Ref, 94, info, "packageName", "Ljava/lang/String;");
  package_info_version =
      C(e, Ref, 94, info, "versionName", "Ljava/lang/String;");
  package_info_code = C(e, Ref, 94, info, "versionCode", "I");
  if (mode == 6)
    first = C(e, Ref, 27, info);
  Ref version = C(e, Ref, 95, first, package_info_version);
  if (mode == 5) {
    CHECK(!version);
    return C(vm, int, 5);
  }
  CHECK(query_text(e, version, "fixture-version"));
  Ref package_name = C(e, Ref, 95, first, package);
  CHECK(query_text(e, package_name, "result.app"));
  CHECK(C(e, int, 100, first, package_info_code) == 7);
  Ref changed = C(e, Ref, 167, "changed");
  C(e, void, 104, first, package_info_version, changed);
  C(e, void, 109, first, package_info_code, -1);
  Ref unchanged = C(e, Ref, 95, second, package_info_version);
  CHECK(query_text(e, unchanged, "fixture-version"));
  CHECK(C(e, int, 100, second, package_info_code) == 7);
  package_info_manager = C(e, Ref, 21, manager);
  package_info_first = C(e, Ref, 21, first);
  package_info_name = C(e, Ref, 21, name);
  C(e, void, 23, first);
  CHECK(!C(vm, int, 5));
  U64 child = 0;
  void *result = (void *)1;
  CHECK(!pthread_create(&child, 0, package_info_thread, 0));
  CHECK(!pthread_join(child, &result) && !result);
  e = attach();
  CHECK(e);
  C(e, void, 22, package_info_manager);
  C(e, void, 22, package_info_first);
  C(e, void, 22, package_info_name);
  CHECK(!C(e, int, 228));
  return C(vm, int, 5);
}
static Ref package_application_manager, package_application_name;
static Ref package_application_first, package_application_child;
static Ref package_application_getter, package_application_field;
static Ref package_application_source;
static void *package_application_thread(void *unused) {
  (void)unused;
  Handle e = attach();
  if (!e)
    return (void *)1;
  Ref old = C(e, Ref, 95, package_application_first, package_application_field);
  Ref changed =
      C(e, Ref, 95, package_application_child, package_application_source);
  Ref fresh = C(e, Ref, 34, package_application_manager,
                package_application_getter, package_application_name, 0);
  Ref nested = C(e, Ref, 95, fresh, package_application_field);
  Ref source = C(e, Ref, 95, nested, package_application_source);
  int ok = !old && nested &&
           !C(e, int, 24, nested, package_application_child) &&
           query_text(e, changed, "changed.apk") &&
           query_text(e, source, "parcel.apk");
  C(e, void, 104, package_application_first, package_application_field, nested);
  int detach = C(vm, int, 5);
  return (void *)(U64)(!ok || detach);
}
int jni_package_application(int mode) {
  Handle e = attach();
  CHECK(e);
  Ref info = C(e, Ref, 6, "android/content/pm/PackageInfo");
  package_application_field = C(e, Ref, 94, info, "applicationInfo",
                                "Landroid/content/pm/ApplicationInfo;");
  CHECK(package_application_field);
  if (mode == 1)
    return C(vm, int, 5);
  Ref thread = C(e, Ref, 6, "android/app/ActivityThread");
  Ref current = C(e, Ref, 113, thread, "currentApplication",
                  "()Landroid/app/Application;");
  Ref app = C(e, Ref, 114, thread, current);
  Ref context = C(e, Ref, 6, "android/content/Context");
  Ref manager_id = C(e, Ref, 33, context, "getPackageManager",
                     "()Landroid/content/pm/PackageManager;");
  Ref manager = C(e, Ref, 34, app, manager_id);
  Ref cls = C(e, Ref, 31, manager);
  package_application_getter =
      C(e, Ref, 33, cls, "getPackageInfo",
        "(Ljava/lang/String;I)Landroid/content/pm/PackageInfo;");
  Ref name = C(e, Ref, 167, "test.app");
  Ref first = C(e, Ref, 34, manager, package_application_getter, name, 0);
  CHECK(first);
  if (mode == 3)
    first = C(e, Ref, 27, info);
  Ref context_id = C(e, Ref, 33, context, "getApplicationInfo",
                     "()Landroid/content/pm/ApplicationInfo;");
  Ref retained = C(e, Ref, 34, app, context_id);
  CHECK(retained);
  if (mode == 4) {
    C(e, void, 104, first, package_application_field, retained);
    Ref written = C(e, Ref, 95, first, package_application_field);
    CHECK(C(e, int, 24, written, retained));
    C(e, void, 104, first, package_application_field, (Ref)0);
    CHECK(!C(e, Ref, 95, first, package_application_field));
    return C(vm, int, 5);
  }
  Ref child = C(e, Ref, 95, first, package_application_field);
  if (mode == 2) {
    CHECK(!child);
    return C(vm, int, 5);
  }
  CHECK(child && !C(e, int, 24, child, retained));
  Ref again = C(e, Ref, 95, first, package_application_field);
  CHECK(C(e, int, 24, child, again));
  U64 arguments[] = {name, 0};
  Ref second = C(e, Ref, 36, manager, package_application_getter, arguments);
  Ref separate = C(e, Ref, 95, second, package_application_field);
  CHECK(separate && !C(e, int, 24, child, separate));
  Ref application_info = C(e, Ref, 6, "android/content/pm/ApplicationInfo");
  package_application_source =
      C(e, Ref, 94, application_info, "sourceDir", "Ljava/lang/String;");
  Ref source = C(e, Ref, 95, child, package_application_source);
  CHECK(query_text(e, source, "parcel.apk"));
  Ref changed = C(e, Ref, 167, "changed.apk");
  C(e, void, 104, child, package_application_source, changed);
  Ref untouched = C(e, Ref, 95, separate, package_application_source);
  Ref context_source = C(e, Ref, 95, retained, package_application_source);
  CHECK(query_text(e, untouched, "parcel.apk"));
  CHECK(query_text(e, context_source, "context.apk"));
  C(e, void, 104, first, package_application_field, retained);
  Ref replaced = C(e, Ref, 95, first, package_application_field);
  CHECK(C(e, int, 24, replaced, retained));
  C(e, void, 104, first, package_application_field, (Ref)0);
  package_application_manager = C(e, Ref, 21, manager);
  package_application_name = C(e, Ref, 21, name);
  package_application_first = C(e, Ref, 21, first);
  package_application_child = C(e, Ref, 21, child);
  C(e, void, 23, first);
  CHECK(!C(vm, int, 5));
  U64 tid = 0;
  void *result = (void *)1;
  CHECK(!pthread_create(&tid, 0, package_application_thread, 0));
  CHECK(!pthread_join(tid, &result) && !result);
  e = attach();
  CHECK(e);
  Ref from_thread =
      C(e, Ref, 95, package_application_first, package_application_field);
  Ref after = C(e, Ref, 95, from_thread, package_application_source);
  CHECK(query_text(e, after, "parcel.apk"));
  C(e, void, 22, package_application_manager);
  C(e, void, 22, package_application_name);
  C(e, void, 22, package_application_first);
  C(e, void, 22, package_application_child);
  CHECK(!C(e, int, 228));
  return C(vm, int, 5);
}
static Ref target_info_saved, target_query_saved, target_field;
static Ref target_manager, target_name, target_getter;
static void *target_sdk_thread(void *unused) {
  (void)unused;
  Handle e = attach();
  if (!e)
    return (void *)1;
  int context = C(e, int, 100, target_info_saved, target_field);
  int query = C(e, int, 100, target_query_saved, target_field);
  Ref fresh = C(e, Ref, 34, target_manager, target_getter, target_name, 0);
  int initial = C(e, int, 100, fresh, target_field);
  int ok = context == -1 && query == 31 && initial == 29 &&
           !C(e, int, 24, fresh, target_query_saved);
  int detach = C(vm, int, 5);
  return (void *)(U64)(!ok || detach);
}
int jni_target_sdk(int mode) {
  Handle e = attach();
  CHECK(e);
  Ref thread = C(e, Ref, 6, "android/app/ActivityThread");
  Ref current = C(e, Ref, 113, thread, "currentApplication",
                  "()Landroid/app/Application;");
  Ref app = C(e, Ref, 114, thread, current);
  Ref context = C(e, Ref, 6, "android/content/Context");
  Ref info_get = C(e, Ref, 33, context, "getApplicationInfo",
                   "()Landroid/content/pm/ApplicationInfo;");
  Ref info = C(e, Ref, 34, app, info_get);
  Ref type = C(e, Ref, 6, "android/content/pm/ApplicationInfo");
  target_field = C(e, Ref, 94, type, "targetSdkVersion", "I");
  CHECK(target_field);
  if (mode == 4)
    return C(vm, int, 5);
  if (mode == 2)
    info = C(e, Ref, 27, type);
  CHECK(C(e, int, 100, info, target_field) == (mode == 1 ? 0 : 28));
  C(e, void, 109, info, target_field, -1);
  Ref repeated = C(e, Ref, 34, app, info_get);
  CHECK(C(e, int, 24, repeated, info));
  CHECK(C(e, int, 100, repeated, target_field) == -1);
  Ref manager_get = C(e, Ref, 33, context, "getPackageManager",
                      "()Landroid/content/pm/PackageManager;");
  Ref manager = C(e, Ref, 34, app, manager_get);
  Ref pm = C(e, Ref, 6, "android/content/pm/PackageManager");
  target_getter =
      C(e, Ref, 33, pm, "getApplicationInfo",
        "(Ljava/lang/String;I)Landroid/content/pm/ApplicationInfo;");
  Ref name = C(e, Ref, 167, "test.app");
  Ref query = C(e, Ref, 34, manager, target_getter, name, 0);
  CHECK(query && !C(e, int, 24, query, info));
  CHECK(C(e, int, 100, query, target_field) == 29);
  C(e, void, 109, query, target_field, 31);
  U64 arguments[] = {name, 0};
  Ref second = C(e, Ref, 36, manager, target_getter, arguments);
  CHECK(second && !C(e, int, 24, second, query));
  CHECK(C(e, int, 100, second, target_field) == 29);
  target_info_saved = C(e, Ref, 21, info);
  target_query_saved = C(e, Ref, 21, query);
  target_manager = C(e, Ref, 21, manager);
  target_name = C(e, Ref, 21, name);
  C(e, void, 23, info);
  C(e, void, 23, query);
  CHECK(!C(vm, int, 5));
  U64 child = 0;
  void *result = (void *)1;
  CHECK(!pthread_create(&child, 0, target_sdk_thread, 0));
  CHECK(!pthread_join(child, &result) && !result);
  e = attach();
  CHECK(e);
  C(e, void, 22, target_info_saved);
  C(e, void, 22, target_query_saved);
  C(e, void, 22, target_manager);
  C(e, void, 22, target_name);
  CHECK(!C(e, int, 228));
  return C(vm, int, 5);
}
static Ref resource_context_saved, resource_value_saved, resource_getter;
static void *resource_path_thread(void *unused) {
  (void)unused;
  Handle e = attach();
  if (!e)
    return (void *)1;
  Ref value = C(e, Ref, 36, resource_context_saved, resource_getter, (void *)0);
  int ok = C(e, int, 24, value, resource_value_saved);
  int detach = C(vm, int, 5);
  return (void *)(U64)(!ok || detach);
}
int jni_package_resource_path(int mode) {
  Handle e = attach();
  CHECK(e);
  Ref thread = C(e, Ref, 6, "android/app/ActivityThread");
  Ref current = C(e, Ref, 113, thread, "currentApplication",
                  "()Landroid/app/Application;");
  Ref app = C(e, Ref, 114, thread, current);
  Ref context = C(e, Ref, 6, "android/content/Context");
  resource_getter =
      C(e, Ref, 33, context, "getPackageResourcePath", "()Ljava/lang/String;");
  CHECK(resource_getter);
  if (mode == 4)
    return C(vm, int, 5);
  Ref receiver = app;
  if (mode == 2) {
    Ref concrete = C(e, Ref, 31, app);
    CHECK(concrete);
    receiver = C(e, Ref, 27, concrete);
  }
  if (mode == 3)
    receiver = 0;
  Ref path = C(e, Ref, 34, receiver, resource_getter);
  if (mode == 1)
    CHECK(!path);
  else
    CHECK(query_text(e, path, "/public//fixture.apk"));
  Ref wrapper = C(e, Ref, 6, "android/content/ContextWrapper");
  Ref base_id =
      C(e, Ref, 33, wrapper, "getBaseContext", "()Landroid/content/Context;");
  Ref base = C(e, Ref, 34, app, base_id);
  Ref info_id = C(e, Ref, 33, context, "getApplicationInfo",
                  "()Landroid/content/pm/ApplicationInfo;");
  Ref info = C(e, Ref, 34, app, info_id);
  Ref info_class = C(e, Ref, 6, "android/content/pm/ApplicationInfo");
  Ref source = C(e, Ref, 94, info_class, "sourceDir", "Ljava/lang/String;");
  Ref changed = C(e, Ref, 167, "/replacement.apk");
  C(e, void, 104, info, source, changed);
  Ref repeated = C(e, Ref, 36, base, resource_getter, (void *)0);
  CHECK(C(e, int, 24, path, repeated));
  resource_context_saved = C(e, Ref, 21, base);
  resource_value_saved = C(e, Ref, 21, path);
  C(e, void, 23, base);
  CHECK(!C(vm, int, 5));
  U64 child = 0;
  void *result = (void *)1;
  CHECK(!pthread_create(&child, 0, resource_path_thread, 0));
  CHECK(!pthread_join(child, &result) && !result);
  e = attach();
  CHECK(e);
  C(e, void, 22, resource_context_saved);
  C(e, void, 22, resource_value_saved);
  CHECK(!C(e, int, 228));
  return C(vm, int, 5);
}
static Ref label_manager_saved, label_info_saved, label_value_saved;
static Ref label_getter, label_field;
static void *label_thread(void *unused) {
  (void)unused;
  Handle e = attach();
  if (!e)
    return (void *)1;
  Ref value =
      C(e, Ref, 34, label_manager_saved, label_getter, label_info_saved);
  Ref field_value = C(e, Ref, 95, label_info_saved, label_field);
  int ok = C(e, int, 24, value, label_value_saved) &&
           C(e, int, 24, value, field_value);
  int detach = C(vm, int, 5);
  return (void *)(U64)(!ok || detach);
}
int jni_application_label(int mode) {
  Handle e = attach();
  CHECK(e);
  Ref thread = C(e, Ref, 6, "android/app/ActivityThread");
  Ref current = C(e, Ref, 113, thread, "currentApplication",
                  "()Landroid/app/Application;");
  Ref app = C(e, Ref, 114, thread, current);
  Ref context = C(e, Ref, 6, "android/content/Context");
  Ref manager_id = C(e, Ref, 33, context, "getPackageManager",
                     "()Landroid/content/pm/PackageManager;");
  Ref manager = C(e, Ref, 34, app, manager_id);
  Ref manager_class = C(e, Ref, 31, manager);
  label_getter =
      C(e, Ref, 33, manager_class, "getApplicationLabel",
        "(Landroid/content/pm/ApplicationInfo;)Ljava/lang/CharSequence;");
  CHECK(label_getter);
  if (mode == 4)
    return C(vm, int, 5);
  Ref context_info_id = C(e, Ref, 33, context, "getApplicationInfo",
                          "()Landroid/content/pm/ApplicationInfo;");
  Ref info = C(e, Ref, 34, app, context_info_id);
  if (mode == 1) {
    Ref value = C(e, Ref, 34, manager, label_getter, (Ref)0);
    CHECK(!value);
    Ref exception = C(e, Ref, 15);
    C(e, void, 17);
    Ref npe = C(e, Ref, 6, "java/lang/NullPointerException");
    CHECK(exception && C(e, int, 32, exception, npe));
    return C(vm, int, 5);
  }
  if (mode == 2) {
    Ref type = C(e, Ref, 31, info);
    info = C(e, Ref, 27, type);
  }
  Ref label = C(e, Ref, 34, manager, label_getter, info);
  CHECK(query_text(e, label, "  fixture  "));
  Ref item = C(e, Ref, 6, "android/content/pm/PackageItemInfo");
  label_field =
      C(e, Ref, 94, item, "nonLocalizedLabel", "Ljava/lang/CharSequence;");
  Ref field_value = C(e, Ref, 95, info, label_field);
  CHECK(C(e, int, 24, label, field_value));
  Ref builder_type = C(e, Ref, 6, "java/lang/StringBuilder");
  Ref ctor = C(e, Ref, 33, builder_type, "<init>", "()V");
  Ref builder = C(e, Ref, 28, builder_type, ctor);
  CHECK(builder);
  Ref append =
      C(e, Ref, 33, builder_type, "append", "(C)Ljava/lang/StringBuilder;");
  C(e, Ref, 34, builder, append, '!');
  C(e, void, 104, info, label_field, builder);
  U64 args[] = {info};
  Ref mutable_label = C(e, Ref, 36, manager, label_getter, args);
  CHECK(C(e, int, 24, mutable_label, builder));
  Ref changed = C(e, Ref, 167, "changed");
  C(e, void, 104, info, label_field, changed);
  label_manager_saved = C(e, Ref, 21, manager);
  label_info_saved = C(e, Ref, 21, info);
  label_value_saved = C(e, Ref, 21, changed);
  C(e, void, 23, info);
  CHECK(!C(vm, int, 5));
  U64 child = 0;
  void *result = (void *)1;
  CHECK(!pthread_create(&child, 0, label_thread, 0));
  CHECK(!pthread_join(child, &result) && !result);
  e = attach();
  CHECK(e);
  C(e, void, 22, label_manager_saved);
  C(e, void, 22, label_info_saved);
  C(e, void, 22, label_value_saved);
  CHECK(!C(e, int, 228));
  return C(vm, int, 5);
}
static int check_package_manager_thread(void) {
  Handle e = attach();
  CHECK(e);
  Ref context = C(e, Ref, 6, "android/content/Context");
  Ref getter = C(e, Ref, 33, context, "getPackageManager",
                 "()Landroid/content/pm/PackageManager;");
  Ref repeated = C(e, Ref, 34, package_manager_app, getter);
  CHECK(repeated && C(e, int, 24, repeated, package_manager_saved));
  CHECK(!C(e, int, 228));
  return C(vm, int, 5);
}
static void *package_manager_thread(void *unused) {
  (void)unused;
  return (void *)(U64)check_package_manager_thread();
}
int jni_package_manager(int mode) {
  Handle e = attach();
  CHECK(e);
  Ref thread = C(e, Ref, 6, "android/app/ActivityThread");
  Ref current = C(e, Ref, 113, thread, "currentApplication",
                  "()Landroid/app/Application;");
  Ref app = C(e, Ref, 114, thread, current);
  CHECK(app);
  Ref type = C(e, Ref, 31, app);
  Ref getter = C(e, Ref, 33, type, "getPackageManager",
                 "()Landroid/content/pm/PackageManager;");
  if (mode == 5) {
    CHECK(getter && !C(e, int, 228));
    return C(vm, int, 5);
  }
  if (mode == 3) {
    // JNI rejects the null receiver before any Java invocation.
    C(e, Ref, 34, (Ref)0, getter);
    return __LINE__;
  }
  if (mode == 4) {
    Ref foreign = C(e, Ref, 27, type);
    C(e, Ref, 34, foreign, getter);
    return __LINE__;
  }
  Ref package_manager = C(e, Ref, 34, app, getter);
  Ref wrapper = C(e, Ref, 6, "android/content/ContextWrapper");
  Ref base_id =
      C(e, Ref, 33, wrapper, "getBaseContext", "()Landroid/content/Context;");
  Ref base = C(e, Ref, 34, app, base_id);
  Ref again = C(e, Ref, 34, base, getter);
  if (mode == 2) {
    CHECK(!package_manager && !again);
    CHECK(!C(e, int, 228));
    return C(vm, int, 5);
  }
  CHECK(package_manager && C(e, int, 24, package_manager, again));
  Ref declared = C(e, Ref, 6, "android/content/pm/PackageManager");
  Ref concrete = C(e, Ref, 6, "android/app/ApplicationPackageManager");
  Ref actual = C(e, Ref, 31, package_manager);
  CHECK(C(e, int, 24, actual, concrete));
  Ref parent = C(e, Ref, 10, concrete);
  CHECK(C(e, int, 24, parent, declared));
  CHECK(C(e, int, 32, package_manager, declared));
  if (mode == 1) {
    Ref query = C(e, Ref, 33, declared, "getPackageInfo",
                  "(Ljava/lang/String;I)Landroid/content/pm/PackageInfo;");
    CHECK(query);
    Ref name = C(e, Ref, 167, "test.app");
    C(e, Ref, 34, package_manager, query, name, 0);
    return __LINE__;
  }
  package_manager_app = C(e, Ref, 21, app);
  package_manager_saved = C(e, Ref, 21, package_manager);
  C(e, void, 23, package_manager);
  C(e, void, 23, again);
  CHECK(!C(vm, int, 5));
  CHECK(!check_package_manager_thread());
  U64 child = 0;
  void *result = (void *)1;
  CHECK(!pthread_create(&child, 0, package_manager_thread, 0));
  CHECK(!pthread_join(child, &result) && !result);
  e = attach();
  CHECK(e);
  C(e, void, 22, package_manager_app);
  C(e, void, 22, package_manager_saved);
  CHECK(!C(e, int, 228));
  return C(vm, int, 5);
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
int jni_loader_before_application(void) {
  Handle e = attach();
  CHECK(e);
  Ref thread_type = C(e, Ref, 6, "android/app/ActivityThread");
  Ref current_id = C(e, Ref, 113, thread_type, "currentApplication",
                     "()Landroid/app/Application;");
  CHECK(!C(e, Ref, 114, thread_type, current_id) && !C(e, int, 228));
  Ref type = C(e, Ref, 6, "registration/Child");
  Ref class_type = C(e, Ref, 6, "java/lang/Class");
  Ref get_loader =
      C(e, Ref, 33, class_type, "getClassLoader", "()Ljava/lang/ClassLoader;");
  Ref loader = C(e, Ref, 34, type, get_loader);
  CHECK(loader);
  Ref loader_type = C(e, Ref, 6, "java/lang/ClassLoader");
  Ref parent_id =
      C(e, Ref, 33, loader_type, "getParent", "()Ljava/lang/ClassLoader;");
  Ref boot = C(e, Ref, 34, loader, parent_id);
  CHECK(boot && !C(e, int, 24, loader, boot));
  Ref load_id = C(e, Ref, 33, loader_type, "loadClass",
                  "(Ljava/lang/String;)Ljava/lang/Class;");
  Ref name = C(e, Ref, 167, "registration.Child");
  Ref again = C(e, Ref, 34, loader, load_id, name);
  CHECK(C(e, int, 24, type, again));
  CHECK(!C(e, Ref, 114, thread_type, current_id));
  Ref retained = C(e, Ref, 21, loader);
  CHECK(!C(vm, int, 5));
  e = attach();
  CHECK(e);
  type = C(e, Ref, 6, "registration/Child");
  again = C(e, Ref, 34, type, get_loader);
  CHECK(C(e, int, 24, retained, again));
  C(e, void, 22, retained);
  CHECK(!C(vm, int, 5));
  return 0;
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
static Ref service_get_v(Handle e, Ref type, Ref method, ...) {
  __builtin_va_list args;
  __builtin_va_start(args, method);
  Ref result = C(e, Ref, 115, type, method, args);
  __builtin_va_end(args);
  return result;
}
int jni_service_manager_cache(unsigned mode) {
  Handle e = attach();
  CHECK(e);
  Ref type = C(e, Ref, 6, "android/os/ServiceManager");
  Ref get = C(e, Ref, 113, type, "getService",
              "(Ljava/lang/String;)Landroid/os/IBinder;");
  Ref check = C(e, Ref, 113, type, "checkService",
                "(Ljava/lang/String;)Landroid/os/IBinder;");
  CHECK(get && check);
  Ref alpha = C(e, Ref, 167, "alpha");
  Ref first = C(e, Ref, 114, type, get, alpha);
  if (mode)
    return (int)first; // Missing-state/RPC cases stop at the original call.
  CHECK(first);
  Ref beta = C(e, Ref, 167, "beta");
  CHECK(same_ref(e, first, service_get_v(e, type, get, beta)));
  U64 arguments[] = {beta};
  CHECK(same_ref(e, first, C(e, Ref, 116, type, check, arguments)));
  Ref proxy = C(e, Ref, 6, "android/os/BinderProxy");
  Ref warning = C(e, Ref, 94, proxy, "mWarnOnBlocking", "Z");
  CHECK(warning && C(e, U8, 96, first, warning));
  Ref binder = C(e, Ref, 6, "android/os/Binder");
  Ref allow = C(e, Ref, 113, binder, "allowBlocking",
                "(Landroid/os/IBinder;)Landroid/os/IBinder;");
  CHECK(same_ref(e, first, C(e, Ref, 114, binder, allow, first)));
  CHECK(!C(e, U8, 96, first, warning));
  Ref ibinder = C(e, Ref, 6, "android/os/IBinder");
  Ref local = C(e, Ref, 33, ibinder, "queryLocalInterface",
                "(Ljava/lang/String;)Landroid/os/IInterface;");
  CHECK(!C(e, Ref, 34, first, local, (Ref)0));
  Ref install = C(e, Ref, 113, type, "initServiceCache", "(Ljava/util/Map;)V");
  C(e, void, 141, type, install, (Ref)0);
  Ref thrown = C(e, Ref, 15);
  CHECK(thrown);
  C(e, void, 17);
  Ref illegal = C(e, Ref, 6, "java/lang/IllegalStateException");
  CHECK(C(e, U8, 32, thrown, illegal));
  Ref global = C(e, Ref, 21, first);
  CHECK(!C(vm, int, 5));
  e = attach();
  CHECK(e);
  type = C(e, Ref, 6, "android/os/ServiceManager");
  alpha = C(e, Ref, 167, "alpha");
  CHECK(same_ref(e, global, C(e, Ref, 114, type, get, alpha)));
  C(e, void, 22, global);
  CHECK(!C(e, U8, 228));
  CHECK(!C(vm, int, 5));
  return 0;
}
int jni_service_manager_clock(unsigned advancing) {
  Handle e = attach();
  CHECK(e);
  Ref type = C(e, Ref, 6, "android/os/ServiceManager");
  CHECK(type);
  struct ClockTime before, after;
  CHECK(!clock_gettime(7, &before));
  Ref logger_field = C(e, Ref, 144, type, "sStatLogger",
                       "Lcom/android/internal/util/StatLogger;");
  CHECK(logger_field);
  Ref logger = C(e, Ref, 145, type, logger_field);
  CHECK(!clock_gettime(7, &after));
  Ref logger_type = C(e, Ref, 6, "com/android/internal/util/StatLogger");
  Ref next_field = C(e, Ref, 94, logger_type, "mNextTickTime", "J");
  CHECK(logger && next_field);
  U64 next = C(e, U64, 101, logger, next_field);
  CHECK((U64)before.seconds * 1000 + before.nanoseconds / 1000000 + 1000 <=
            next &&
        next <= (U64)after.seconds * 1000 + after.nanoseconds / 1000000 + 1000);
  if (!advancing)
    CHECK(next == 51987);

  // Idling advances the shared clock, but does not reconstruct the logger or
  // its deadline on another active use of the initialized class.
  CHECK(!usleep(2500));
  Ref repeated = C(e, Ref, 144, type, "sStatLogger",
                   "Lcom/android/internal/util/StatLogger;");
  CHECK(same_ref(e, logger, C(e, Ref, 145, type, repeated)));
  CHECK(C(e, U64, 101, logger, next_field) == next);
  Ref clock_type = C(e, Ref, 6, "android/os/SystemClock");
  Ref elapsed = C(e, Ref, 113, clock_type, "elapsedRealtime", "()J");
  CHECK(clock_type && elapsed);
  CHECK(!clock_gettime(7, &before));
  U64 now = C(e, U64, 132, clock_type, elapsed);
  CHECK(!clock_gettime(7, &after));
  CHECK((U64)before.seconds * 1000 + before.nanoseconds / 1000000 <= now &&
        now <= (U64)after.seconds * 1000 + after.nanoseconds / 1000000);
  if (!advancing) {
    CHECK(now == 50990);
    CHECK(!clock_gettime(1, &after));
    CHECK(after.seconds == 10 && after.nanoseconds == 125956789);
    CHECK(!clock_gettime(4, &after));
    CHECK(after.seconds == 30 && after.nanoseconds == 502500000);
  }
  CHECK(!C(e, U8, 228));
  return C(vm, int, 5);
}
int jni_service_manager_lookup(unsigned remove_cached) {
  Handle e = attach();
  CHECK(e);
  Ref type = C(e, Ref, 6, "android/os/ServiceManager");
  Ref get = C(e, Ref, 113, type, "getService",
              "(Ljava/lang/String;)Landroid/os/IBinder;");
  Ref check = C(e, Ref, 113, type, "checkService",
                "(Ljava/lang/String;)Landroid/os/IBinder;");
  CHECK(type && get && check);
  Ref key = C(e, Ref, 167, "alpha");
  Ref equal_key = C(e, Ref, 167, "alpha");
  CHECK(key && equal_key && !same_ref(e, key, equal_key));
  Ref first = C(e, Ref, 114, type, get, key);
  CHECK(first);
  U64 arguments[] = {equal_key};
  for (unsigned i = 0; i != 2; ++i) {
    Ref method = i ? check : get;
    CHECK(same_ref(e, first, C(e, Ref, 114, type, method, equal_key)));
    CHECK(same_ref(e, first, service_get_v(e, type, method, equal_key)));
    CHECK(same_ref(e, first, C(e, Ref, 116, type, method, arguments)));
  }
  if (remove_cached) {
    Ref cache_field = C(e, Ref, 144, type, "sCache", "Ljava/util/HashMap;");
    Ref cache = C(e, Ref, 145, type, cache_field);
    Ref map_type = C(e, Ref, 6, "java/util/HashMap");
    Ref remove = C(e, Ref, 33, map_type, "remove",
                   "(Ljava/lang/Object;)Ljava/lang/Object;");
    CHECK(cache_field && cache && map_type && remove);
    CHECK(same_ref(e, first, C(e, Ref, 34, cache, remove, equal_key)));
    // The next call must use the changed shared map and reach the miss path.
    C(e, Ref, 114, type, get, key);
    return __LINE__;
  }
  CHECK(!C(e, U8, 228));
  return C(vm, int, 5);
}
int jni_service_manager_metadata(unsigned mode) {
  Handle e = attach();
  CHECK(e);
  Ref type = C(e, Ref, 6, "android/os/ServiceManager");
  CHECK(type);
  if (mode == 1)
    return !C(e, Ref, 113, type, "getService",
              "(Ljava/lang/String;)Landroid/os/IBinder;");
  if (mode == 2)
    return !C(e, Ref, 144, type, "sCache", "Ljava/util/HashMap;");
  if (mode == 3)
    return !C(e, Ref, 33, type, "toString", "()Ljava/lang/String;");
  if (mode == 4)
    return !C(e, Ref, 27, type);
  Ref again = C(e, Ref, 6, "android/os/ServiceManager");
  Ref object = C(e, Ref, 6, "java/lang/Object");
  Ref parent = C(e, Ref, 10, type);
  CHECK(C(e, int, 24, type, again));
  CHECK(C(e, int, 24, parent, object));
  CHECK(C(e, U8, 11, type, object));
  CHECK(!C(e, U8, 11, object, type));
  Ref array = C(e, Ref, 6, "[Landroid/os/ServiceManager;");
  CHECK(array && !C(e, int, 24, array, type));
  CHECK(!C(e, U8, 228));
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

/* Original guest functions. Registration must dispatch to these instructions,
 * including nested JNI calls, instead of inventing a return value. */
static unsigned native_mode, native_calls;
static Ref native_leaked, native_global, native_base;
static Handle native_env;
static int native_number(Handle e, Ref type, int input) {
  if (native_mode != 22)
    ++native_calls;
  if ((native_mode != 22 && e != native_env) ||
      !C(e, int, 24, type, native_base))
    return -7000;
  if (native_mode == 22 &&
      (C(vm, int, 5) != -1 || usleep(100) || C(e, unsigned, 4) != 0x10006))
    return -7001;
  if (native_mode == 16)
    for (;;)
      __asm__ volatile("" ::: "memory");
  if (native_mode == 17) {
    Ref id = C(e, Ref, 113, type, "number", "(I)I");
    return C(e, int, 129, type, id, input);
  }
  return input + 0x1234;
}
static int native_rebound(Handle e, Ref type, int input) {
  return native_number(e, type, input) + 7;
}
static U64 native_wide(Handle e, Ref object, U64 input) {
  return C(e, int, 32, object, native_base) ? input ^ 0x8000000000000001ULL : 0;
}
static U64 native_wide_child(Handle e, Ref object, U64 input) {
  return native_wide(e, object, input) + 5;
}
static Ref native_self(Handle e, Ref object) { return C(e, Ref, 25, object); }
static U64 native_mixed(Handle e, Ref type, U8 z, signed char b, U16 c, short s,
                        int i, U64 j) {
  return e == native_env && C(e, int, 24, type, native_base) && z == 1 &&
                 b == -127 && c == 0xff01 && s == -32765 && i == -1234567
             ? j ^ 0x123456789abcdef0ULL
             : 0;
}
static Ref native_echo(Handle e, Ref type, Ref input) {
  ++native_calls;
  if (native_mode == 24) {
    signed char bytes[] = {-128, 127};
    C(e, void, 208, input, 2, 2, bytes);
    return input;
  }
  if (native_mode == 9)
    return C(e, Ref, 20, input);
  if (native_mode == 6)
    return type;
  if (native_mode == 7)
    return 0x1234;
  if (native_mode == 21) {
    Ref ex = C(e, Ref, 6, "java/lang/IllegalArgumentException");
    C(e, int, 14, ex, (const char *)0);
    return 0xdeadbeef; /* Pending exception makes this carrier unspecified. */
  }
  if (e != native_env || !C(e, int, 24, type, native_base) ||
      C(vm, int, 5) != -1)
    return 0;
  Ref id = C(e, Ref, 113, type, "number", "(I)I");
  if (C(e, int, 129, type, id, 7) != 0x123b)
    return 0;
  if (C(e, int, 19, 8))
    return 0;
  if (native_mode == 23)
    input = C(e, Ref, 167, "from native");
  native_leaked = C(e, Ref, 25, input);
  native_global = C(e, Ref, 21, input);
  /* The implicit return boundary must clean up this still-open local frame. */
  return native_leaked;
}
static void native_visit(Handle e, Ref type) {
  native_calls += C(e, int, 24, type, native_base) ? 11 : 999;
}
static U64 native_bits(Handle e, Ref type) {
  (void)e;
  (void)type;
  return native_mode == 14 ? 2 : 0xfedcba987654ff81ULL;
}
static int native_number_v(Handle e, Ref type, Ref id, ...) {
  __builtin_va_list ap;
  __builtin_va_start(ap, id);
  int result = C(e, int, 130, type, id, ap);
  __builtin_va_end(ap);
  return result;
}
static void *native_call_thread(void *unused) {
  (void)unused;
  Handle local_vm = 0, e = 0;
  int count = 0;
  if (JNI_GetCreatedJavaVMs(&local_vm, 1, &count) || count != 1 ||
      C(local_vm, int, 4, &e, (void *)0))
    return (void *)1;
  Ref id = C(e, Ref, 113, native_base, "number", "(I)I");
  int answer = C(e, int, 129, native_base, id, 17);
  if (C(local_vm, int, 5))
    return (void *)2;
  return (void *)(U64)(unsigned)answer;
}
int jni_native_calls(unsigned mode) {
  native_mode = mode;
  Handle e = attach();
  CHECK(e);
  native_env = e;
  Ref base = C(e, Ref, 6, "nativecall/Base");
  Ref child = C(e, Ref, 6, "nativecall/Child");
  native_base = C(e, Ref, 21, base);
  /* Resolve before registration. IDs are independent of current bindings. */
  Ref number = C(e, Ref, 113, child, "number", "(I)I");
  CHECK(number && !native_calls);
  struct NativeBinding bindings[] = {
      {"number", "(I)I", (U64)native_number},
      {"echo", "(Ljava/lang/Object;)Ljava/lang/Object;", (U64)native_echo},
      {"self", "()Ljava/lang/Object;", (U64)native_self},
      {"wide", "(J)J", (U64)native_wide},
      {"mixed", "(ZBCSIJ)J", (U64)native_mixed},
      {"visit", "()V", (U64)native_visit},
      {"text", "(Ljava/lang/String;)Ljava/lang/String;", (U64)native_echo},
      {"booleanValue", "()Z", (U64)native_bits},
      {"byteValue", "()B", (U64)native_bits},
      {"shortValue", "()S", (U64)native_bits},
      {"charValue", "()C", (U64)native_bits},
      {"locked", "()V", (U64)native_visit},
      {"overflow", "(IIIIIII)I", (U64)native_number},
      {"floating", "(F)F", (U64)native_number}};
  if (mode == 3)
    bindings[0].target += 1;
  if (mode == 4)
    bindings[0].target = 0x12340000;
  if (mode != 1)
    CHECK(!C(e, int, 215, base, bindings,
             (int)(sizeof(bindings) / sizeof(bindings[0]))));
  if (mode == 2)
    CHECK(!C(e, int, 216, base));
  if (mode == 24) {
    signed char before[] = {1, 2, 3, 4, 5, 6};
    signed char after[6] = {0};
    Ref array = C(e, Ref, 176, 6);
    C(e, void, 208, array, 0, 6, before);
    Ref id =
        C(e, Ref, 113, base, "echo", "(Ljava/lang/Object;)Ljava/lang/Object;");
    Ref returned = C(e, Ref, 114, base, id, array);
    CHECK(C(e, int, 24, returned, array));
    C(e, void, 200, returned, 0, 6, after);
    for (int i = 0; i < 6; ++i)
      CHECK(after[i] == (i == 2 ? -128 : i == 3 ? 127 : before[i]));
    CHECK(native_calls == 1);
    return C(vm, int, 5);
  }
  if (mode == 22) {
    U64 thread;
    CHECK(!pthread_create(&thread, (void *)0, native_call_thread, (void *)0));
    CHECK(C(e, int, 129, base, number, 3) == 0x1237);
    void *answer = 0;
    CHECK(!pthread_join(thread, &answer) && (U64)answer == 0x1245);
    C(e, void, 22, native_base);
    return C(vm, int, 5);
  }
  if (mode == 18) {
    Ref bad = C(e, Ref, 6, "nativecall/Bad");
    struct NativeBinding after = {"after", "()V", (U64)native_visit};
    CHECK(!C(e, int, 215, bad, &after, 1));
    CHECK(!C(e, Ref, 113, bad, "after", "()V"));
    CHECK(C(e, int, 228) && !native_calls);
    Ref thrown = C(e, Ref, 15);
    C(e, void, 17);
    Ref ex = C(e, Ref, 6, "java/lang/ExceptionInInitializerError");
    CHECK(C(e, int, 32, thrown, ex));
    return C(vm, int, 5);
  }
  if (mode == 10 || mode == 11 || mode == 12 || mode == 15) {
    const char *name = mode == 10   ? "locked"
                       : mode == 11 ? "overflow"
                       : mode == 12 ? "floating"
                                    : "missing";
    const char *sig = mode == 10   ? "()V"
                      : mode == 11 ? "(IIIIIII)I"
                      : mode == 12 ? "(F)F"
                                   : "()I";
    Ref id = C(e, Ref, 113, base, name, sig);
    CHECK(id);
    if (mode == 10)
      C(e, void, 141, base, id);
    if (mode == 11)
      C(e, int, 129, base, id, 1, 2, 3, 4, 5, 6, 7);
    if (mode == 12) {
      U64 arg = 0;
      C(e, float, 137, base, id, &arg);
    }
    if (mode == 15)
      C(e, int, 129, base, id);
    return 999;
  }
  CHECK(C(e, int, 129, child, number, -7) == 0x122d);
  CHECK(native_number_v(e, child, number, 11) == 0x123f);
  U64 argument = 0xffffffff0000000dULL;
  CHECK(C(e, int, 131, child, number, &argument) == 0x1241);
  if (mode == 13 || mode == 14) {
    if (mode == 13) {
      Ref id = C(e, Ref, 113, base, "mixed", "(ZBCSIJ)J");
      C(e, U64, 132, base, id, 2, -127, 0xff01, -32765, -1234567, (U64)1);
    } else {
      Ref id = C(e, Ref, 113, base, "booleanValue", "()Z");
      C(e, U8, 117, base, id);
    }
    return 999;
  }
  Ref text = C(e, Ref, 167, "native argument");
  Ref echo =
      C(e, Ref, 113, base, "text", "(Ljava/lang/String;)Ljava/lang/String;");
  Ref result = C(e, Ref, 114, base, echo, mode == 5 ? base : text);
  if (mode == 21) {
    CHECK(!result && C(e, int, 228));
    Ref thrown = C(e, Ref, 15);
    C(e, void, 17);
    Ref ex = C(e, Ref, 6, "java/lang/IllegalArgumentException");
    CHECK(C(e, int, 32, thrown, ex));
    CHECK(C(e, int, 129, base, number, 0) == 0x1234);
    return C(vm, int, 5);
  }
  if (mode == 8) {
    C(e, int, 232, native_leaked);
    return 999;
  }
  CHECK(result && result != native_leaked);
  if (mode == 23) {
    CHECK(!C(e, int, 24, result, text));
    const char *bytes = C(e, const char *, 169, result, (void *)0);
    const char expected[] = "from native";
    for (unsigned i = 0; i < sizeof(expected); ++i)
      CHECK(bytes[i] == expected[i]);
    C(e, void, 170, result, bytes);
  } else
    CHECK(C(e, int, 24, result, text));
  CHECK(C(e, int, 24, native_global, result));
  CHECK(C(e, int, 232, result) == 1 && C(e, int, 232, native_global) == 2);
  C(e, void, 22, native_global);
  Ref mixed = C(e, Ref, 113, base, "mixed", "(ZBCSIJ)J");
  U64 args[] = {
      0x101, 0x81, 0xff01, 0x8003, (unsigned)-1234567, 0xfedcba9876543210ULL};
  CHECK(C(e, U64, 134, base, mixed, args) == 0xece8ece0ece8ece0ULL);
  Ref obj = C(e, Ref, 27, child);
  Ref wide = C(e, Ref, 33, base, "wide", "(J)J");
  struct NativeBinding override = {"wide", "(J)J", (U64)native_wide_child};
  CHECK(!C(e, int, 215, child, &override, 1));
  CHECK(C(e, U64, 52, obj, wide, (U64)7) == 0x800000000000000bULL);
  CHECK(C(e, U64, 82, obj, base, wide, (U64)7) == 0x8000000000000006ULL);
  Ref self = C(e, Ref, 33, base, "self", "()Ljava/lang/Object;");
  Ref same = C(e, Ref, 34, obj, self);
  CHECK(C(e, int, 24, same, obj));
  Ref b = C(e, Ref, 113, base, "byteValue", "()B");
  Ref s = C(e, Ref, 113, base, "shortValue", "()S");
  Ref c = C(e, Ref, 113, base, "charValue", "()C");
  CHECK(C(e, signed char, 120, base, b) == -127);
  CHECK(C(e, short, 126, base, s) == -127);
  CHECK(C(e, U16, 123, base, c) == 0xff81);
  unsigned before = native_calls;
  Ref visit = C(e, Ref, 113, base, "visit", "()V");
  C(e, void, 141, base, visit);
  CHECK(native_calls == before + 11);
  struct NativeBinding rebound = {"number", "(I)I", (U64)native_rebound};
  CHECK(!C(e, int, 215, base, &rebound, 1));
  CHECK(C(e, int, 129, child, number, 0) == 0x123b);
  CHECK(!C(vm, int, 5));
  e = attach();
  CHECK(e);
  native_env = e;
  base = C(e, Ref, 6, "nativecall/Base");
  CHECK(C(e, int, 129, base, number, 1) == 0x123c);
  C(e, void, 22, native_base);
  return C(vm, int, 5);
}

/* Java -> loadLibrary -> original JNI_OnLoad -> Java -> native -> Java.
 * State/counters distinguish one execution from replay or canned success. */
static unsigned resume_mode, resume_loads, resume_calls, resume_helpers;
static Ref resume_onload_local;
static int resume_helper(Handle e, Ref type, int input) {
  ++resume_helpers;
  CHECK(C(vm, int, 5) == -1);
  CHECK(!C(e, int, 19, 4)); /* Leave a nested frame for implicit cleanup. */
  CHECK(C(e, Ref, 25, type));
  return input * 2;
}
static int resume_apply(Handle e, Ref type, int input) {
  ++resume_calls;
  Ref step = C(e, Ref, 113, type, "step", "(I)I");
  CHECK(step && !C(e, int, 228));
  return C(e, int, 129, type, step, input);
}
static Ref resume_echo(Handle e, Ref type, Ref input) {
  (void)type;
  if (C(e, int, 19, 4))
    return 0;
  return C(e, Ref, 25, input);
}
static int resume_throw(Handle e, Ref type) {
  (void)type;
  Ref failure = C(e, Ref, 6, "java/lang/IllegalArgumentException");
  C(e, int, 14, failure, (void *)0);
  return 0x7341;
}
int JNI_OnLoad(Handle machine, void *reserved) {
  Handle e = 0;
  CHECK(machine == vm && !reserved && !C(machine, int, 6, &e, 0x10006));
  CHECK(e && ++resume_loads == 1);
  CHECK(C(vm, int, 5) == -1);
  Ref type = C(e, Ref, 6, "resumption/Library");
  resume_onload_local = type;
  Ref count = C(e, Ref, 144, type, "count", "I");
  CHECK(count && C(e, int, 150, type, count) == 1);
  struct {
    const char *name, *signature;
    void *target;
  } methods[] = {
      {"apply", "(I)I", resume_apply},
      {"helper", "(I)I", resume_helper},
      {"echo", "(Ljava/lang/Object;)Ljava/lang/Object;", resume_echo},
      {"fail", "()I", resume_throw}};
  CHECK(!C(e, int, 215, type, methods, 4));
  Ref again = C(e, Ref, 113, type, "again", "()V");
  C(e, void, 141, type,
    again); /* Recursive same-loader load must not call us again. */
  Ref step = C(e, Ref, 113, type, "step", "(I)I");
  CHECK(step && C(e, int, 129, type, step, 10) == 27);
  CHECK(!C(e, int, 228) && resume_helpers == 1);
  if (resume_mode == 5) {
    Ref failure = C(e, Ref, 6, "java/lang/IllegalArgumentException");
    C(e, int, 14, failure, (void *)0);
  }
  if (resume_mode == 6)
    CHECK(!usleep(100));
  return resume_mode == 2   ? 0x10008
         : resume_mode == 3 ? 0x10002
         : resume_mode == 4 ? 0x10004
                            : 0x10006;
}
int jni_java_resume(unsigned mode) {
  resume_mode = mode;
  Handle e = attach();
  CHECK(e);
  Ref type = C(e, Ref, 6, "resumption/Library");
  CHECK(type && !resume_loads);
  Ref allocated = 0;
  if (mode == 1)
    allocated = C(e, Ref, 27, type);
  if (mode == 7)
    CHECK(C(e, Ref, 144, type, "count", "I"));
  Ref run = C(e, Ref, 113, type, "run", "(I)I");
  if (mode == 9) {
    CHECK(run && !C(e, int, 228) && !resume_loads);
    Ref count = C(e, Ref, 144, type, "count", "I");
    CHECK(C(e, int, 150, type, count) == 2);
    return C(vm, int, 5);
  }
  if (mode == 2 || mode == 5) {
    CHECK(!run && C(e, int, 228) && resume_loads == 1);
    Ref thrown = C(e, Ref, 15);
    C(e, void, 17);
    Ref error = C(e, Ref, 6,
                  mode == 2 ? "java/lang/UnsatisfiedLinkError"
                            : "java/lang/ExceptionInInitializerError");
    CHECK(C(e, int, 32, thrown, error));
    CHECK(!C(e, Ref, 113, type, "run", "(I)I") && C(e, int, 228));
    thrown = C(e, Ref, 15);
    C(e, void, 17);
    error = C(e, Ref, 6, "java/lang/NoClassDefFoundError");
    CHECK(C(e, int, 32, thrown, error) && resume_loads == 1);
    return C(vm, int, 5);
  }
  CHECK(run && !C(e, int, 228) && resume_loads == 1);
  if (mode == 8)
    return C(e, int, 24, resume_onload_local,
             type); /* Retired on-load local. */
  if (mode == 1)
    CHECK(allocated && C(e, int, 32, allocated, type));
  Ref count = C(e, Ref, 144, type, "count", "I");
  CHECK(C(e, int, 150, type, count) == 2);
  CHECK(C(e, int, 129, type, run, 11) == 32);
  CHECK(C(e, int, 129, type, run, -5) == 0);
  CHECK(resume_calls == 2 && resume_helpers == 3);
  Ref ctor = C(e, Ref, 33, type, "<init>", "()V");
  Ref object = C(e, Ref, 28, type, ctor);
  CHECK(object && !C(e, int, 228) && resume_calls == 3 && resume_helpers == 4);
  Ref ref = C(e, Ref, 113, type, "reference",
              "(Ljava/lang/Object;)Ljava/lang/Object;");
  Ref echoed = C(e, Ref, 114, type, ref, object);
  CHECK(C(e, int, 24, object, echoed));
  Ref failure = C(e, Ref, 113, type, "throwing", "()I");
  CHECK(!C(e, int, 129, type, failure) && C(e, int, 228));
  Ref thrown = C(e, Ref, 15);
  C(e, void, 17);
  Ref error = C(e, Ref, 6, "java/lang/IllegalArgumentException");
  CHECK(C(e, int, 32, thrown, error));
  Ref again = C(e, Ref, 113, type, "again", "()V");
  for (unsigned i = 0; i < 200; ++i)
    C(e, void, 141, type, again);
  Ref loop = C(e, Ref, 113, type, "loop", "()V");
  C(e, void, 141, type, loop);
  CHECK(!C(e, int, 228) && resume_loads == 1 &&
        C(e, int, 150, type, count) == 2);
  return C(vm, int, 5);
}
static Ref split_info, split_array, split_field;
static void *split_sources_thread(void *unused) {
  (void)unused;
  Handle e = attach();
  if (!e)
    return (void *)1;
  Ref array = C(e, Ref, 95, split_info, split_field);
  if (!C(e, int, 24, array, split_array))
    return (void *)2;
  Ref first = C(e, Ref, 173, array, 0);
  if (!query_text(e, first, "changed"))
    return (void *)3;
  Ref value = C(e, Ref, 167, "thread");
  C(e, void, 174, array, 1, value);
  Ref string = C(e, Ref, 6, "java/lang/String");
  Ref replacement = C(e, Ref, 172, 1, string, value);
  C(e, void, 104, split_info, split_field, replacement);
  return (void *)(U64)C(vm, int, 5);
}
static int jni_split_directories(int mode, int public_sources) {
  Handle e = attach();
  CHECK(e);
  Ref type = C(e, Ref, 6, "android/content/pm/ApplicationInfo");
  split_field = C(e, Ref, 94, type,
                  public_sources ? "splitPublicSourceDirs" : "splitSourceDirs",
                  "[Ljava/lang/String;");
  CHECK(split_field);
  if (mode == 1)
    return C(vm, int, 5);
  Ref thread = C(e, Ref, 6, "android/app/ActivityThread");
  Ref current = C(e, Ref, 113, thread, "currentApplication",
                  "()Landroid/app/Application;");
  Ref app = C(e, Ref, 114, thread, current);
  Ref context = C(e, Ref, 6, "android/content/Context");
  Ref getter = C(e, Ref, 33, context, "getApplicationInfo",
                 "()Landroid/content/pm/ApplicationInfo;");
  Ref info = C(e, Ref, 34, app, getter);
  CHECK(info);
  if (mode == 6)
    info = C(e, Ref, 27, type);
  if (mode == 5) {
    Ref string = C(e, Ref, 6, "java/lang/String");
    Ref array = C(e, Ref, 172, 0, string, (Ref)0);
    C(e, void, 104, info, split_field, array);
    Ref read = C(e, Ref, 95, info, split_field);
    CHECK(C(e, int, 24, read, array));
    C(e, void, 104, info, split_field, (Ref)0);
    CHECK(!C(e, Ref, 95, info, split_field));
    return C(vm, int, 5);
  }
  Ref array = C(e, Ref, 95, info, split_field);
  if (mode == 2) {
    CHECK(!array);
    return C(vm, int, 5);
  }
  CHECK(array);
  Ref same = C(e, Ref, 95, info, split_field);
  CHECK(C(e, int, 24, array, same));
  if (mode == 3) {
    CHECK(C(e, int, 171, array) == 0);
    return C(vm, int, 5);
  }
  CHECK(C(e, int, 171, array) == 4);
  Ref other_field =
      C(e, Ref, 94, type,
        public_sources ? "splitSourceDirs" : "splitPublicSourceDirs",
        "[Ljava/lang/String;");
  CHECK(other_field);
  Ref other = C(e, Ref, 95, info, other_field);
  CHECK(other && !C(e, int, 24, other, array));
  CHECK(C(e, int, 171, other) == 1);
  CHECK(query_text(e, C(e, Ref, 173, other, 0), "other.apk"));
  Ref retained_other = C(e, Ref, 21, other);
  Ref first = C(e, Ref, 173, array, 0);
  Ref empty = C(e, Ref, 173, array, 2);
  Ref repeated = C(e, Ref, 173, array, 3);
  CHECK(query_text(e, first, "first.apk") && query_text(e, empty, "") &&
        query_text(e, repeated, "first.apk"));
  CHECK(!C(e, Ref, 173, array, 1));
  Ref changed = C(e, Ref, 167, "changed");
  C(e, void, 174, array, 0, changed);
  Ref manager_id = C(e, Ref, 33, context, "getPackageManager",
                     "()Landroid/content/pm/PackageManager;");
  Ref manager = C(e, Ref, 34, app, manager_id);
  Ref manager_type = C(e, Ref, 31, manager);
  Ref query = C(e, Ref, 33, manager_type, "getApplicationInfo",
                "(Ljava/lang/String;I)Landroid/content/pm/ApplicationInfo;");
  Ref name = C(e, Ref, 167, "test.app");
  U64 arguments[] = {name, 0};
  Ref separate = C(e, Ref, 36, manager, query, arguments);
  Ref separate_array = C(e, Ref, 95, separate, split_field);
  CHECK(!C(e, int, 24, array, separate_array));
  Ref untouched = C(e, Ref, 173, separate_array, 0);
  CHECK(query_text(e, untouched, "first.apk"));
  split_info = C(e, Ref, 21, info);
  split_array = C(e, Ref, 21, array);
  C(e, void, 23, array);
  CHECK(!C(vm, int, 5));
  U64 tid = 0;
  void *result = (void *)1;
  CHECK(!pthread_create(&tid, 0, split_sources_thread, 0));
  CHECK(!pthread_join(tid, &result) && !result);
  e = attach();
  CHECK(e);
  Ref from_thread = C(e, Ref, 95, split_info, split_field);
  CHECK(!C(e, int, 24, from_thread, split_array));
  CHECK(C(e, int, 171, from_thread) == 1);
  Ref thread_value = C(e, Ref, 173, from_thread, 0);
  Ref old_array_value = C(e, Ref, 173, split_array, 1);
  CHECK(query_text(e, thread_value, "thread") &&
        query_text(e, old_array_value, "thread"));
  Ref unchanged_other = C(e, Ref, 95, split_info, other_field);
  CHECK(C(e, int, 24, unchanged_other, retained_other));
  CHECK(C(e, int, 171, unchanged_other) == 1);
  CHECK(query_text(e, C(e, Ref, 173, unchanged_other, 0), "other.apk"));
  C(e, void, 22, retained_other);
  C(e, void, 22, split_info);
  C(e, void, 22, split_array);
  CHECK(!C(e, int, 228));
  return C(vm, int, 5);
}

int jni_split_source_dirs(int mode) { return jni_split_directories(mode, 0); }
int jni_split_public_source_dirs(int mode) {
  return jni_split_directories(mode, 1);
}

int jni_url_values(int mode) {
  Handle e = attach();
  CHECK(e);
  Ref type = C(e, Ref, 6, "java/net/URL");
  Ref ctor = C(e, Ref, 33, type, "<init>", "(Ljava/lang/String;)V");
  const char *spec =
      mode == 1 ? "file:///local" : "https://example.invalid:8443/a?x=1#f";
  Ref input = C(e, Ref, 167, spec);
  U64 argument = input;
  Ref url = mode == 5 ? C(e, Ref, 30, type, ctor, &argument)
                      : C(e, Ref, 28, type, ctor, input);
  CHECK(url);
  Ref serial = C(e, Ref, 6, "java/io/Serializable");
  CHECK(C(e, int, 32, url, serial));
  Ref parent = C(e, Ref, 6, "java/lang/Object");
  Ref superclass = C(e, Ref, 10, type);
  CHECK(C(e, int, 24, superclass, parent));
  const char *methods[] = {"toString", "toExternalForm"};
  for (unsigned i = 0; i != 2; ++i) {
    Ref method = C(e, Ref, 33, type, methods[i], "()Ljava/lang/String;");
    Ref value = C(e, Ref, 34, url, method);
    CHECK(value && !C(e, int, 24, value, url));
    const char *text = C(e, const char *, 169, value, (void *)0);
    CHECK(text);
    unsigned j = 0;
    for (; spec[j]; ++j)
      CHECK(text[j] == spec[j]);
    CHECK(!text[j]);
    C(e, void, 170, value, text);
  }
  if (mode == 2) {
    Ref open =
        C(e, Ref, 33, type, "openConnection", "()Ljava/net/URLConnection;");
    C(e, Ref, 34, url, open);
    return __LINE__;
  }
  if (mode == 3 || mode == 4 || mode == 6) {
    Ref equal = C(e, Ref, 33, parent, "equals", "(Ljava/lang/Object;)Z");
    if (mode == 3)
      C(e, int, 37, url, equal, url);
    else
      CHECK(C(e, int, 67, url, mode == 6 ? type : parent, equal, url) == 1);
    if (mode != 4)
      return __LINE__;
  }
  CHECK(!C(e, int, 228));
  return C(vm, int, 5);
}

static Ref url_call_v(Handle e, Ref url, Ref method, ...) {
  __builtin_va_list ap;
  __builtin_va_start(ap, method);
  Ref result = C(e, Ref, 35, url, method, ap);
  __builtin_va_end(ap);
  return result;
}
int jni_url_handler(int mode, int form) {
  Handle e = attach();
  CHECK(e);
  Ref url_type = C(e, Ref, 6, "java/net/URL");
  Ref ctor = C(e, Ref, 33, url_type, "<init>", "(Ljava/lang/String;)V");
  Ref open =
      C(e, Ref, 33, url_type, "openConnection", "()Ljava/net/URLConnection;");
  Ref io_type = C(e, Ref, 6, "java/io/IOException");
  Ref exception_type = C(e, Ref, 6, "java/lang/Exception");
  Ref runtime_type = C(e, Ref, 6, "java/lang/RuntimeException");
  Ref get_message =
      C(e, Ref, 33, io_type, "getMessage", "()Ljava/lang/String;");
  const char *specs[] = {"http://example.invalid/a",
                         "https://example.invalid/b"};
  for (unsigned scheme = 0; scheme != 2; ++scheme) {
    Ref saved_exception = 0, saved_message = 0;
    for (unsigned round = 0; round != 2; ++round) {
      Ref input = C(e, Ref, 167, specs[scheme]);
      U64 args[] = {input};
      Ref url = form == 0   ? C(e, Ref, 28, url_type, ctor, input)
                : form == 1 ? dex_new_v(e, url_type, ctor, input)
                            : C(e, Ref, 30, url_type, ctor, args);
      CHECK(url && !C(e, int, 228));
      Ref connection = form == 0   ? C(e, Ref, 34, url, open)
                       : form == 1 ? url_call_v(e, url, open)
                                   : C(e, Ref, 36, url, open, (void *)0);
      CHECK(!connection && C(e, int, 228));
      Ref thrown = C(e, Ref, 15);
      Ref repeated = C(e, Ref, 15);
      C(e, void, 17);
      CHECK(!C(e, int, 228) && thrown && repeated);
      CHECK(C(e, int, 24, thrown, repeated));
      CHECK(!saved_exception || !C(e, int, 24, thrown, saved_exception));
      CHECK(C(e, int, 32, thrown, io_type));
      CHECK(C(e, int, 32, thrown, exception_type));
      CHECK(!C(e, int, 32, thrown, runtime_type));
      Ref message = C(e, Ref, 34, thrown, get_message);
      if (!mode)
        CHECK(!message);
      else {
        CHECK(message);
        if (round)
          CHECK(C(e, int, 24, message, saved_message));
        CHECK(C(e, int, 164, message) == (mode == 1 ? 0 : 8));
        if (mode == 2) {
          const U16 expected[] = {'l', 'o', 'c', 'a', 'l', 0, 0x79bb, 0x7ebf};
          const U16 *units = C(e, const U16 *, 165, message, (void *)0);
          CHECK(units);
          for (unsigned i = 0; i != 8; ++i)
            CHECK(units[i] == expected[i]);
          C(e, void, 166, message, units);
        }
      }
      CHECK(C(e, int, 13, thrown) == 0 && C(e, int, 228));
      Ref rethrown = C(e, Ref, 15);
      C(e, void, 17);
      CHECK(C(e, int, 24, rethrown, thrown));
      saved_exception = thrown;
      saved_message = message;
    }
  }
  CHECK(!C(e, int, 228));
  return C(vm, int, 5);
}
