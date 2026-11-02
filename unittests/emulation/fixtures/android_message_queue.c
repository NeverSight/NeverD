//===- android_message_queue.c - Original JNI message queue callers -------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Freestanding JNI callers retaining live queue/message references.
///
//===----------------------------------------------------------------------===//

typedef unsigned long long U64;
typedef const U64 *Table;
typedef Table *Handle;
typedef U64 Ref;
extern int JNI_GetCreatedJavaVMs(Handle *, int, int *);

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

int message_queue(U64 *out, int mode) {
  Handle e = attach();
  CHECK(e);
  Ref looper_type = C(e, Ref, 6, "android/os/Looper");
  Ref main =
      C(e, Ref, 113, looper_type, "getMainLooper", "()Landroid/os/Looper;");
  Ref looper = C(e, Ref, 114, looper_type, main);
  Ref get_queue =
      C(e, Ref, 33, looper_type, "getQueue", "()Landroid/os/MessageQueue;");
  Ref queue = C(e, Ref, 34, looper, get_queue);
  Ref again = C(e, Ref, 36, looper, get_queue, (void *)0);
  CHECK(queue && C(e, int, 24, queue, again));
  Ref queue_type = C(e, Ref, 6, "android/os/MessageQueue");
  Ref next = C(e, Ref, 33, queue_type, "next", "()Landroid/os/Message;");
  Ref handler_type = C(e, Ref, 6, "android/os/Handler");
  Ref init = C(e, Ref, 33, handler_type, "<init>", "(Landroid/os/Looper;)V");
  Ref handler = C(e, Ref, 28, handler_type, init, looper);
  Ref post = C(e, Ref, 33, handler_type, "post", "(Ljava/lang/Runnable;)Z");
  CHECK(C(e, int, 37, handler, post, (Ref)0) == 1);
  U64 arguments[] = {0};
  CHECK(C(e, int, 39, handler, post, arguments) == 1);
  if (mode == 2)
    queue = C(e, Ref, 27, queue_type);
  Ref first = C(e, Ref, 34, queue, next);
  CHECK(first);
  Ref message_type = C(e, Ref, 6, "android/os/Message");
  Ref target =
      C(e, Ref, 33, message_type, "getTarget", "()Landroid/os/Handler;");
  Ref callback =
      C(e, Ref, 33, message_type, "getCallback", "()Ljava/lang/Runnable;");
  Ref link = C(e, Ref, 94, message_type, "next", "Landroid/os/Message;");
  Ref flags = C(e, Ref, 94, message_type, "flags", "I");
  Ref held_queue = C(e, Ref, 21, queue);
  Ref held_handler = C(e, Ref, 21, handler);
  Ref held_message = C(e, Ref, 21, first);
  CHECK(C(vm, int, 5) == 0);
  e = attach();
  CHECK(e);
  out[0] = 1;
  Ref first_target = C(e, Ref, 34, held_message, target);
  out[1] = C(e, int, 24, first_target, held_handler);
  out[2] = C(e, Ref, 36, held_message, callback, (void *)0) == 0;
  out[3] = C(e, Ref, 95, held_message, link) == 0;
  out[4] = C(e, int, 100, held_message, flags);
  Ref second = C(e, Ref, 36, held_queue, next, (void *)0);
  CHECK(second);
  out[5] = !C(e, int, 24, second, held_message);
  Ref second_target = C(e, Ref, 36, second, target, (void *)0);
  out[6] = C(e, int, 24, second_target, held_handler);
  out[7] = 73;
  if (mode == 1)
    C(e, Ref, 34, held_queue, next);
  C(e, void, 22, held_queue);
  C(e, void, 22, held_handler);
  C(e, void, 22, held_message);
  CHECK(!C(e, int, 228));
  return C(vm, int, 5);
}
