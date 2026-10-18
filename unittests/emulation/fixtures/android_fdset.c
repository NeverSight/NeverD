/* Authored LP64 callers. No host fd_set, libc or descriptor table is used. */
typedef unsigned long U;
extern void __FD_SET_chk(int, void *, U);
extern void __FD_CLR_chk(int, void *, U);
extern int __FD_ISSET_chk(int, const void *, U);
extern int *__errno(void);
extern void *dlopen(const char *, int);
extern void *dlsym(void *, const char *);
extern int dlclose(void *);
extern int mprotect(void *, U, int);

U fdset_call(U *out, int fd, void *set, U size, U mode) {
  void (*modify)(int, void *, U) =
      (mode & 3) == 0 ? __FD_SET_chk : __FD_CLR_chk;
  int (*query)(int, const void *, U) = __FD_ISSET_chk;
  if (mode & 4) {
    void *library = dlopen("libfdset.so", 2);
    if (!library)
      return 97;
    if ((mode & 3) == 2)
      query = dlsym(library, "__FD_ISSET_chk");
    else
      modify =
          dlsym(library, (mode & 3) == 0 ? "__FD_SET_chk" : "__FD_CLR_chk");
    if (!query || !modify)
      return 98;
    if (mode & 8)
      dlclose(library);
  }
  if (mode & 16)
    if (mprotect((void *)(((U)set) & ~4095UL), 4096, 1))
      return 99;
  *__errno() = 73;
  if ((mode & 3) == 2)
    out[0] = query(fd, set, size);
  else {
    modify(fd, set, size);
    out[0] = 0;
  }
  out[1] = (unsigned)*__errno();
  out[2] = 0x12345678;
  return 0;
}

U fdset_sequence(U *out, void *set) {
  *__errno() = 73;
  __FD_SET_chk(0, set, 128);
  __FD_SET_chk(63, set, 128);
  __FD_SET_chk(64, set, 128);
  __FD_SET_chk(1023, set, 128);
  __FD_SET_chk(64, set, 128);
  out[0] = __FD_ISSET_chk(0, set, 128);
  out[1] = __FD_ISSET_chk(63, set, 128);
  out[2] = __FD_ISSET_chk(64, set, 128);
  out[3] = __FD_ISSET_chk(1023, set, 128);
  __FD_CLR_chk(63, set, 128);
  __FD_CLR_chk(63, set, 128);
  out[4] = __FD_ISSET_chk(63, set, 128);
  out[5] = (unsigned)*__errno();
  return 0;
}

/* Leave the upper x0 bits intact to check the imported signed-int ABI. */
__asm__(".global fdset_wide_index\n"
        ".type fdset_wide_index, @function\n"
        "fdset_wide_index:\n"
        "b __FD_ISSET_chk\n");
__asm__(".type __FD_SET_chk, @function\n"
        ".type __FD_CLR_chk, @function\n"
        ".type __FD_ISSET_chk, @function\n");

extern long syscall(long, ...);
extern int open(const char *, int, ...);
extern int close(int);
static long raw_select(U n, void *read_set, void *write_set, void *except_set,
                       void *timeout) {
  register U x0 __asm__("x0") = n;
  register void *x1 __asm__("x1") = read_set;
  register void *x2 __asm__("x2") = write_set;
  register void *x3 __asm__("x3") = except_set;
  register void *x4 __asm__("x4") = timeout;
  register U x5 __asm__("x5") = 0;
  register U x8 __asm__("x8") = 72;
  __asm__ volatile("svc #0"
                   : "+r"(x0)
                   : "r"(x1), "r"(x2), "r"(x3), "r"(x4), "r"(x5), "r"(x8)
                   : "memory", "cc");
  return (long)x0;
}
U fdset_select(U *out, U mode) {
  int fd = open("/select-input", 0);
  if (fd < 0 || fd >= 64)
    return 1;
  U sets[3][16] = {{0}, {0}, {0}}, time[2] = {0, 0};
  for (unsigned i = 0; i != 3; ++i)
    __FD_SET_chk(fd, sets[i], sizeof(sets[i]));
  *__errno() = 73;
  out[0] = mode
               ? syscall(72, fd + 1, sets[0], sets[1], sets[2], time, (void *)0)
               : raw_select(fd + 1, sets[0], sets[1], sets[2], time);
  out[1] = *__errno();
  for (unsigned i = 0; i != 3; ++i)
    out[2 + i] = __FD_ISSET_chk(fd, sets[i], sizeof(sets[i]));
  if (close(fd))
    return 2;
  __FD_SET_chk(fd, sets[0], sizeof(sets[0]));
  out[5] =
      mode ? syscall(72, fd + 1, sets[0], (void *)0, (void *)0, time, (void *)0)
           : raw_select(fd + 1, sets[0], 0, 0, time);
  out[6] = *__errno();
  return 0;
}
