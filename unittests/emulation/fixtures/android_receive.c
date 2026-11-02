/* Independently authored callers; the socket contracts never use a transport.
 */
typedef unsigned long U;
extern long syscall(long, ...);
extern int *__errno(void);
extern int socket(int, int, int);
extern int connect(int, const void *, unsigned);
extern int setsockopt(int, int, int, const void *, unsigned);
extern int close(int);
extern int dup(int);
extern int mprotect(void *, U, int);
extern long recv(int, void *, U, int);
extern long recvfrom(int, void *, U, int, void *, unsigned *);
extern long read(int, void *, U);
extern long __read_chk(int, void *, U, U);
extern int pthread_create(U *, const void *, void *(*)(void *), void *);
extern int pthread_join(U, void **);
extern int nanosleep(const void *, void *);
extern int clock_gettime(int, void *);
extern void *fdopen(int, const char *);
extern int setvbuf(void *, char *, int, U);
extern U fread(void *, U, U, void *);

static const unsigned char peer[16] = {2, 0, 0x23, 0x29, 127, 0, 0, 1};
static long raw_receive(U number, U fd, void *buffer) {
  register U x0 __asm__("x0") = fd;
  register void *x1 __asm__("x1") = buffer;
  register U x2 __asm__("x2") = 1, x3 __asm__("x3") = 0;
  register U x4 __asm__("x4") = 0, x5 __asm__("x5") = 0;
  register U x8 __asm__("x8") = number;
  __asm__ volatile("svc #0"
                   : "+r"(x0)
                   : "r"(x1), "r"(x2), "r"(x3), "r"(x4), "r"(x5), "r"(x8)
                   : "memory", "cc");
  return (long)x0;
}
struct State {
  U *out;
  U route, action;
  int fd, alias;
};
static void *reader(void *arg) {
  struct State *s = arg;
  void *buffer = s->out + 512;
  *__errno() = 73;
  switch (s->route) {
  case 0:
    s->out[0] = raw_receive(207, s->fd, buffer);
    break;
  case 1:
    s->out[0] = recv(s->fd, buffer, 1, 0);
    break;
  case 2:
    s->out[0] = raw_receive(63, s->fd, buffer);
    break;
  case 3:
    s->out[0] = read(s->fd, buffer, 1);
    break;
  case 4:
    s->out[0] = syscall(207, s->fd, buffer, 1UL, 0UL, (void *)0, (void *)0);
    break;
  case 6:
    s->out[0] = __read_chk(s->fd, buffer, 1, 1);
    break;
  default:
    s->out[20] = 16;
    s->out[0] =
        recvfrom(s->fd, buffer, 1, 0, s->out + 32, (unsigned *)(s->out + 20));
    break;
  }
  s->out[1] = (unsigned)*__errno();
  clock_gettime(1, s->out + 2);
  s->out[4] = 123;
  s->out[5] = *(unsigned char *)buffer;
  return (void *)42;
}
static void *other(void *arg) {
  struct State *s = arg;
  U delay[2] = {0, 3};
  if (nanosleep(delay, 0))
    return (void *)1;
  *__errno() = 91;
  clock_gettime(1, s->out + 8);
  if (s->action == 1) {
    if (close(s->fd))
      return (void *)2;
    int reused = socket(2, 1, 6);
    if (reused != s->fd || connect(reused, peer, 16))
      return (void *)3;
    s->out[10] = read(reused, s->out + 16, 1);
  } else if (s->action == 2) {
    if (mprotect(s->out + 512, 4096, 1))
      return (void *)4;
  } else if (s->action == 3) {
    U timeout[2] = {0, 0};
    if (setsockopt(s->fd, 1, 20, timeout, 16))
      return (void *)5;
  } else if (s->action == 4) {
    s->out[10] = recv(s->alias, s->out + 520, 1, 0);
    s->out[16] = *(unsigned char *)(s->out + 520);
  }
  s->out[11] = (unsigned)*__errno();
  clock_gettime(1, s->out + 14);
  s->out[6] = 456;
  return (void *)43;
}
U receive_workers(U *out, U route, U action, U type) {
  int fd = socket(2, type, 0);
  if (fd < 0 || connect(fd, peer, 16))
    return 1;
  U timeout[2] = {2, 0};
  if (setsockopt(fd, 1, 20, timeout, 16))
    return 2;
  struct State s = {out, route, action, fd, action == 4 ? dup(fd) : -1};
  U threads[2];
  if (pthread_create(threads, 0, reader, &s) ||
      pthread_create(threads + 1, 0, other, &s))
    return 3;
  if (pthread_join(threads[0], (void **)(out + 12)) ||
      pthread_join(threads[1], (void **)(out + 13)))
    return 4;
  return 0;
}

U receive_buffered(U *out) {
  int fd = socket(2, 1, 6);
  if (fd < 0 || connect(fd, peer, 16))
    return 9;
  void *stream = fdopen(fd, "r");
  if (!stream || setvbuf(stream, 0, 2, 0))
    return 8;
  out[0] = fread(out + 1, 1, 1, stream);
  return out[0];
}
