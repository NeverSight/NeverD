/* Original LP64 imports and syscall wrappers, with no host transport. */
typedef unsigned long U;
struct PollFD {
  int fd;
  unsigned short events, revents;
};
struct Event {
  unsigned events;
  U data;
};
extern int *__errno(void);
extern int socket(int, int, int);
extern int connect(int, const void *, unsigned);
extern int poll(struct PollFD *, unsigned, int);
extern int ppoll(struct PollFD *, unsigned, const void *, const void *);
extern int epoll_create1(int);
extern int epoll_ctl(int, int, int, const struct Event *);
extern int epoll_wait(int, struct Event *, int, int);
extern int epoll_pwait(int, struct Event *, int, int, const void *);
extern long syscall(long, ...);
extern int pthread_create(U *, const void *, void *(*)(void *), void *);
extern int pthread_join(U, void **);
extern int nanosleep(const void *, void *);
extern int clock_gettime(int, void *);
static const unsigned char peer[16] = {2, 0, 0x23, 0x29, 127, 0, 0, 1};
struct State {
  U *out;
  U route;
  int fd, ep;
};
static void *waiter(void *arg) {
  struct State *s = arg;
  struct PollFD p = {s->fd, 1, 0xa5a5};
  struct Event event;
  U timeout[2] = {0, 1000000};
  *__errno() = 73;
  switch (s->route) {
  case 0:
    s->out[0] = (U)(long)poll(&p, 1, 1);
    break;
  case 1:
    s->out[0] = (U)(long)ppoll(&p, 1, timeout, 0);
    break;
  case 2:
    s->out[0] = (U)(long)epoll_wait(s->ep, &event, 1, 1);
    break;
  case 3:
    s->out[0] = (U)(long)epoll_pwait(s->ep, &event, 1, 1, 0);
    break;
  case 4:
    s->out[0] = syscall(73, &p, 1UL, timeout, (void *)0, 0UL);
    break;
  default:
    s->out[0] = syscall(22, s->ep, &event, 1UL, 1UL, (void *)0, 0UL);
    break;
  }
  s->out[1] = (unsigned)*__errno();
  clock_gettime(1, s->out + 2);
  int epoll = s->route == 2 || s->route == 3 || s->route == 5;
  if (!epoll || (long)s->out[0] > 0) {
    s->out[4] = epoll ? event.events : p.revents;
    s->out[5] = epoll ? event.data : 0;
  }
  s->out[6] = timeout[0];
  s->out[7] = timeout[1];
  return (void *)42;
}
static void *other(void *arg) {
  struct State *s = arg;
  U delay[2] = {0, 3};
  nanosleep(delay, 0);
  *__errno() = 91;
  clock_gettime(1, s->out + 12);
  s->out[14] = 456;
  return (void *)43;
}
U poll_workers(U *out, U route, U invalid) {
  int fd = socket(2, 1, 6);
  if (fd < 0 || connect(fd, peer, 16))
    return 1;
  int ep = -1;
  if (route == 2 || route == 3 || route == 5) {
    ep = epoll_create1(0);
    struct Event event = {1, 0x1234567887654321UL};
    if (ep < 0 || epoll_ctl(ep, 1, fd, &event))
      return 2;
  }
  struct State s = {out, route, fd, invalid ? 99 : ep};
  U threads[2];
  if (pthread_create(threads, 0, waiter, &s) ||
      pthread_create(threads + 1, 0, other, &s))
    return 3;
  if (pthread_join(threads[0], (void **)(out + 10)) ||
      pthread_join(threads[1], (void **)(out + 11)))
    return 4;
  return 0;
}
