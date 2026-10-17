/* Independently authored Android 9 LP64 semaphore callers. */
typedef unsigned long u64;
typedef unsigned int u32;
typedef struct {
  u32 word[4];
} semaphore;
extern int sem_init(semaphore *, int, unsigned);
extern int sem_destroy(semaphore *);
extern int sem_post(semaphore *);
extern int sem_wait(semaphore *);
extern int sem_trywait(semaphore *);
extern int sem_getvalue(semaphore *, int *);
extern int sem_timedwait(semaphore *, const void *);
extern int *__errno(void);
extern int pthread_create(u64 *, const void *, void *(*)(void *), void *);
extern int pthread_join(u64, void **);
extern int mprotect(void *, u64, int);
extern void *dlopen(const char *, int);
extern void *dlsym(void *, const char *);
extern int dlclose(void *);

u64 semaphore_sequence(semaphore *s, u64 shared) {
  for (unsigned i = 0; i != 4; ++i)
    s->word[i] = 0xa5a5a5a5;
  *__errno() = 77;
  if (sem_init(s, (int)shared, 2) || s->word[0] != 4 + ((int)shared != 0))
    return 1;
  int count = -7;
  if (sem_wait(s) || sem_trywait(s) || sem_getvalue(s, &count) || count)
    return 2;
  if (*__errno() != 77 || sem_trywait(s) != -1 || *__errno() != 11)
    return 3;
  if (sem_post(s) || sem_wait(s) || *__errno() != 11)
    return 4;
  if (sem_destroy(s) || sem_destroy((semaphore *)1))
    return 5;
  if (s->word[0] != ((int)shared != 0))
    return 6;
  for (unsigned i = 1; i != 4; ++i)
    if (s->word[i] != 0xa5a5a5a5)
      return 7;
  if (sem_init(s, shared, 0x3fffffff) || sem_post(s) != -1 || *__errno() != 75)
    return 8;
  if (s->word[0] != 0x7ffffffeU + ((int)shared != 0))
    return 9;
  if (sem_init((semaphore *)1, 0, 0x40000000) != -1 || *__errno() != 22)
    return 10;
  if (sem_init(s, 0, 0xffffffff) != -1 || *__errno() != 22)
    return 11;
  if (s->word[0] != 0x7ffffffeU + ((int)shared != 0))
    return 12;
  return 0;
}

u64 semaphore_call(semaphore *s, u64 operation, u64 argument) {
  switch (operation) {
  case 0:
    return (u32)sem_wait(s);
  case 1:
    return (u32)sem_post(s);
  case 2:
    return (u32)sem_trywait(s);
  case 3:
    return (u32)sem_getvalue(s, (int *)argument);
  case 4:
    return (u32)sem_init(s, 0, argument);
  case 5:
    return (u32)sem_destroy(s);
  default:
    return (u32)sem_timedwait(s, (void *)argument);
  }
}

struct workload {
  semaphore tokens, completed;
  volatile u32 ready[2], done[2];
  u32 errors[2], values[2];
  u64 mode;
};
struct worker_arg {
  struct workload *w;
  unsigned index;
};
static void *consumer(void *opaque) {
  struct worker_arg *a = opaque;
  struct workload *w = a->w;
  unsigned i = a->index;
  *__errno() = 80 + i;
  w->ready[i] = 1;
  if (sem_wait(&w->tokens))
    return (void *)1;
  w->errors[i] = *__errno();
  w->values[i] = 101 + i;
  w->done[i] = 1;
  if (sem_post(&w->completed))
    return (void *)2;
  return (void *)(u64)(31 + i);
}

u64 semaphore_workers(struct workload *w, u64 mode) {
  u64 handles[2];
  struct worker_arg args[2] = {{w, 0}, {w, 1}};
  if (sem_init(&w->tokens, mode & 1, 0) || sem_init(&w->completed, 0, 0))
    return 1;
  *__errno() = 77;
  for (unsigned i = 0; i != 2; ++i)
    if (pthread_create(&handles[i], 0, consumer, &args[i]))
      return 2;
  while (!w->ready[0] || !w->ready[1] ||
         ((volatile semaphore *)&w->tokens)->word[0] < 0xfffffffeU) {
  }
  int value = -7;
  if (sem_getvalue(&w->tokens, &value) || value)
    return 3;
  if (sem_post(&w->tokens))
    return 4;
  if (mode & 2) {
    /* Large-quantum execution lets the posting caller take its own token.
       A small quantum may let a woken consumer get it first; both are legal. */
    if (!sem_trywait(&w->tokens) && sem_post(&w->tokens))
      return 5;
  }
  if (sem_post(&w->tokens))
    return 6;
  if (sem_wait(&w->completed) || sem_wait(&w->completed))
    return 7;
  for (unsigned i = 0; i != 2; ++i) {
    void *result = 0;
    if (pthread_join(handles[i], &result) || (u64)result != 31 + i)
      return 8;
    if (w->done[i] != 1 || w->errors[i] != 80 + i || w->values[i] != 101 + i)
      return 9;
  }
  if (sem_getvalue(&w->tokens, &value) || value)
    return 10;
  return sem_destroy(&w->tokens) || sem_destroy(&w->completed) ? 11 : 0;
}

static void *blocked(void *s) { return (void *)(u64)sem_wait(s); }
u64 semaphore_wait_failure(semaphore *s, u64 mode) {
  u64 child;
  if (sem_init(s, 0, 0) || pthread_create(&child, 0, blocked, s))
    return 1;
  while (((volatile semaphore *)s)->word[0] != 0xfffffffeU) {
  }
  if (mode) {
    if (sem_post(s))
      return 2;
    if (mode == 1)
      s->word[0] |= 1;
    if (mode == 2 && mprotect(s, 4096, 0))
      return 3;
    if (mode == 3)
      s->word[0] = 0x80000000;
  }
  return pthread_join(child, 0);
}

u64 semaphore_readonly_tail(semaphore *s) {
  if (mprotect((void *)((u64)s + 4), 4096, 1))
    return 1;
  if (sem_init(s, 0, 1) || sem_wait(s) || sem_post(s))
    return 2;
  return sem_getvalue(s, (int *)s);
}

u64 semaphore_dynamic(semaphore *s, u64 closed) {
  typedef int (*post_fn)(semaphore *);
  void *lib = dlopen("libsemaphore-model.so", 2);
  if (!lib)
    return 1;
  post_fn post = (post_fn)dlsym(lib, "sem_post");
  if (!post || sem_init(s, 0, 0))
    return 2;
  if (closed && dlclose(lib))
    return 3;
  return (u32)post(s);
}
