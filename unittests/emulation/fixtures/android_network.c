/* Original API 28 ABI callers. No host headers or network dependencies. */
typedef unsigned long u64;
typedef unsigned int u32;
struct address_info {
  int flags, family, socket_type, protocol;
  u32 length;
  char *canonical;
  void *address;
  struct address_info *next;
};
_Static_assert(sizeof(struct address_info) == 48, "Bionic LP64 addrinfo");
_Static_assert(__builtin_offsetof(struct address_info, address) == 32,
               "address offset");
extern int getaddrinfo(const char *, const char *, const struct address_info *,
                       struct address_info **);
extern void freeaddrinfo(struct address_info *);
extern int *__errno(void);
extern int socket(int, int, int);
extern int connect(int, const void *, u32);
extern long send(int, const void *, u64, int);
extern long recv(int, void *, u64, int);
extern long recvfrom(int, void *, u64, int, void *, u32 *);
extern long sendto(int, const void *, u64, int, const void *, u32);
extern int close(int);
extern int dup(int);
extern long write(int, const void *, u64);
extern long read(int, void *, u64);
extern long syscall(long, ...);
extern void *dlopen(const char *, int);
extern void *dlsym(void *, const char *);
extern void *checked_copy(void *, const void *, u64,
                          u64) __asm__("__memcpy_chk");
extern void *checked_move(void *, const void *, u64,
                          u64) __asm__("__memmove_chk");
extern void *checked_fill(void *, int, u64, u64) __asm__("__memset_chk");
/* These address-only imports have no PLT calls. Supply the function type
 * normally inherited from libc.so when linking against the Android sysroot. */
__asm__(".type __memcpy_chk, %function\n"
        ".type __memmove_chk, %function\n"
        ".type __memset_chk, %function\n");

// Different call surfaces consume the same failure count. Mode 1 makes
// one more fresh-socket attempt; mode 2 retries the failed description.
u64 network_connect_failures(u64 *out, u64 mode) {
  unsigned char peer4[16] = {2, 0, 0x23, 0x29, 127, 0, 0, 1};
  unsigned char peer6[28] = {10, 0, 0x23, 0x29, 0, 0, 0, 0};
  peer6[23] = 1;
  int fd = socket(2, 1, 6);
  out[0] = connect(fd, peer4, 16);
  if (out[0] || close(fd))
    return 1;
  void *handle = dlopen("libc.so", 2);
  int (*dynamic_connect)(int, const void *, u32) = dlsym(handle, "connect");
  if (!dynamic_connect)
    return 2;
  for (unsigned i = 0; i != 4; ++i) {
    int ipv6 = i & 1;
    void *peer = ipv6 ? peer6 : peer4;
    u32 size = ipv6 ? 28 : 16;
    ++((unsigned char *)peer)[3];
    fd = socket(ipv6 ? 10 : 2, 1, 6);
    *__errno() = 73;
    if (i == 0)
      out[1 + 2 * i] = connect(fd, peer, size);
    else if (i == 1)
      out[1 + 2 * i] = dynamic_connect(fd, peer, size);
    else if (i == 2)
      out[1 + 2 * i] = syscall(203, fd, peer, size);
    else {
      register long x0 __asm__("x0") = fd;
      register void *x1 __asm__("x1") = peer;
      register long x2 __asm__("x2") = size;
      register long x8 __asm__("x8") = 203;
      __asm__ volatile("svc #0"
                       : "+r"(x0)
                       : "r"(x1), "r"(x2), "r"(x8)
                       : "memory", "cc");
      out[1 + 2 * i] = x0;
    }
    out[2 + 2 * i] = *__errno();
    if (mode == 2 && i == 0)
      out[10] = connect(fd, peer, size);
    if (close(fd))
      return 3;
  }
  if (mode == 1) {
    fd = socket(2, 1, 6);
    *__errno() = 73;
    out[10] = connect(fd, peer4, 16);
    return 4;
  }
  return 0;
}

u64 network_checked_memory(u64 *out, u64 mode) {
  unsigned char *dst = (void *)(out + 32);
  const unsigned char src[] = {0, 0xff, 0x42};
  void *(*volatile copy_call)(void *, const void *, u64, u64) = checked_copy;
  void *(*volatile move_call)(void *, const void *, u64, u64) = checked_move;
  void *(*volatile fill_call)(void *, int, u64, u64) = checked_fill;
  if (mode >= 1 && mode <= 3) {
    if (mode == 1)
      copy_call((void *)1, (void *)1, 2, 1);
    if (mode == 2)
      move_call((void *)1, (void *)1, 2, 1);
    if (mode == 3)
      fill_call((void *)1, 0, 2, 1);
    return 99;
  }
  if (mode == 4) {
    out[0] = (u64)copy_call((void *)1, (void *)1, 0, 0);
    out[1] = (u64)move_call((void *)1, (void *)1, 0, 0);
    out[2] = (u64)fill_call((void *)1, 0, 0, 0);
    return 0;
  }
  if (mode == 6) {
    copy_call(dst, dst + 1, 3, 3);
    return 99;
  }
  if (mode == 7) {
    copy_call((void *)1, src, 3, 3);
    return 99;
  }
  if (mode == 5) {
    void *handle = dlopen("libc.so", 2);
    void *(*copy)(void *, const void *, u64, u64) =
        dlsym(handle, "__memcpy_chk");
    void *(*move)(void *, const void *, u64, u64) =
        dlsym(handle, "__memmove_chk");
    void *(*fill)(void *, int, u64, u64) = dlsym(handle, "__memset_chk");
    if (!copy || !move || !fill)
      return 1;
    out[0] = copy(dst, src, 3, 3) == dst;
    out[1] = move(dst + 1, dst, 3, 4096) == dst + 1;
    out[2] = fill(dst + 4, 0x1ab, 2, (u64)-1) == dst + 4;
  } else {
    out[0] = copy_call(dst, src, 3, 3) == dst;
    out[1] = move_call(dst + 1, dst, 3, 4096) == dst + 1;
    out[2] = fill_call(dst + 4, 0x1ab, 2, (u64)-1) == dst + 4;
  }
  return 0;
}

u64 network_datagram(u64 *out, u64 mode) {
  unsigned char peer[28] = {2, 0, 0x23, 0x29, 127, 0, 0, 1};
  const int ipv6 = (mode & 2) != 0, raw = mode & 1;
  if (ipv6) {
    peer[0] = 10;
    peer[4] = peer[7] = 0;
    peer[23] = 1;
  }
  u32 size = ipv6 ? 28 : 16;
  int tcp = socket(ipv6 ? 10 : 2, 1, 6);
  out[0] = connect(tcp, peer, size);
  out[1] = *__errno();
  if (close(tcp))
    return 1;
  int fd =
      raw ? syscall(198, ipv6 ? 10 : 2, 2, 17) : socket(ipv6 ? 10 : 2, 2, 17);
  out[2] = fd == tcp;
  out[3] = raw ? syscall(203, fd, peer, size) : connect(fd, peer, size);
  int alias = dup(fd);
  if (alias < 0 || close(fd))
    return 2;
  const unsigned char packet[] = {0, 0xff, 0x42};
  out[4] = raw ? syscall(206, alias, packet, 3, 0, 0, 0)
               : sendto(alias, packet, 3, 0, 0, 0);
  unsigned char *payload = (void *)(out + 32), *source = (void *)(out + 40);
  u32 length = 3;
  out[5] = raw ? syscall(207, alias, payload, 2, 2 | 0x20, source, &length)
               : recvfrom(alias, payload, 2, 2 | 0x20, source, &length);
  out[6] = length;
  out[7] = recv(alias, payload + 8, 1, 0);      /* discard packet suffix */
  out[8] = send(alias, (void *)1, 0, 0);        /* a genuine empty datagram */
  out[9] = read(alias, (void *)1, 0);           /* no dequeue */
  out[10] = recv(alias, (void *)1, 0, 2);       /* peek empty datagram */
  out[11] = recv(alias, (void *)1, 0, 0);       /* consume it */
  out[12] = recv(alias, payload + 16, 8, 0x40); /* explicit EAGAIN */
  out[13] = *__errno();
  void *handle = dlopen("libc.so", 2);
  long (*receive)(int, void *, u64, int) = dlsym(handle, "recv");
  if (!receive)
    return 3;
  out[14] = receive(alias, payload + 16, 8, 0);
  out[15] = *__errno();
  return close(alias);
}

u64 network_receive_events(u64 *out, u64 unused) {
  (void)unused;
  const unsigned char peer[16] = {2, 0, 0x23, 0x29, 127, 0, 0, 1};
  int fd = socket(2, 1, 6);
  if (connect(fd, peer, 16))
    return 1;
  unsigned char *payload = (void *)(out + 32);
  out[0] = recv(fd, payload, 8, 0);
  out[1] = *__errno();
  const unsigned char request[] = {0, 0xff, 0x42};
  out[2] = send(fd, request, 3, 0);
  out[3] = recv(fd, payload, 8, 0);
  out[4] = recv(fd, payload + 2, 8, 0);
  out[5] = recv(fd, payload + 3, 8, 0x40);
  out[6] = *__errno();
  out[7] = recv(fd, payload + 3, 8, 0);
  out[8] = recv(fd, payload + 4, 8, 0);
  // The caller's protocol decision is ordinary guest code, not an OS ACK.
  out[9] = payload[0] == 'O' && payload[1] == 'K' && payload[2] == 0 &&
           payload[3] == '!';
  return close(fd);
}

u64 network_resolve(u64 *out, u64 mode) {
  struct address_info hints = {0}, *result = (void *)0x1234;
  hints.flags = 2;
  hints.socket_type = 1;
  if (mode == 1)
    hints.flags = 0x80000000u;
  if (mode == 2)
    hints.family = 999;
  if (mode == 3)
    hints.length = 1;
  const char *node = mode == 4 ? 0 : "node.invalid";
  const char *service = mode == 4 ? 0 : "9001";
  *__errno() = 73;
  out[0] = getaddrinfo(node, service, &hints, &result);
  out[1] = *__errno();
  out[2] = (u64)result;
  if (out[0])
    return 0;
  unsigned n = 0;
  for (struct address_info *p = result; p && n < 4; p = p->next, ++n) {
    u64 *q = out + 4 + n * 16;
    q[0] = p->flags;
    q[1] = p->family;
    q[2] = p->socket_type;
    q[3] = p->protocol;
    q[4] = p->length;
    unsigned char *b = (void *)(q + 5);
    for (unsigned i = 0; i != p->length && i < 28; ++i)
      b[i] = ((unsigned char *)p->address)[i];
    b = (void *)(q + 9);
    if (p->canonical)
      for (unsigned i = 0; i < 40; ++i) {
        b[i] = p->canonical[i];
        if (!b[i])
          break;
      }
  }
  out[3] = n;
  freeaddrinfo(result);
  freeaddrinfo(0);
  out[68] = *__errno();
  if (mode == 5)
    freeaddrinfo(result);
  return 0;
}

u64 network_stream(u64 *out, u64 raw) {
  struct address_info hints = {0}, *result = 0;
  hints.socket_type = 1;
  hints.flags = 2;
  if (getaddrinfo("node.invalid", "9001", &hints, &result))
    return 1;
  int fd =
      raw ? syscall(198, result->family, 1, 0) : socket(result->family, 1, 0);
  if (fd < 0)
    return 2;
  int r = raw ? syscall(203, fd, result->address, result->length)
              : connect(fd, result->address, result->length);
  out[0] = (unsigned)r;
  out[1] = *__errno();
  freeaddrinfo(result);
  if (r)
    return close(fd);
  int alias = dup(fd);
  if (alias < 0 || close(fd))
    return 3;
  const unsigned char input[5] = {0, 0xff, 0x42, 0x13, 0x7f};
  out[2] = send(alias, input, 5, 0x4000);
  out[3] = write(alias, input + 2, 3);
  out[4] = raw ? syscall(206, alias, input + 4, 1, 0, 0, 0)
               : send(alias, input + 4, 1, 0);
  out[5] = recv(alias, (unsigned char *)(out + 16) + 1, 8, 0);
  out[6] = read(alias, (unsigned char *)(out + 16) + 3, 8);
  out[7] = recv(alias, (unsigned char *)(out + 16) + 5, 8, 0);
  out[8] = close(alias);
  out[9] = connect(alias, (void *)1, 16);
  out[10] = *__errno();
  return 0;
}

u64 network_stream_identity(u64 *out, u64 mode) {
  unsigned char peer[16] = {2, 0, 0x23, 0x29, 127, 0, 0, 1};
  int failed = socket(2, 1, 6);
  if (failed < 0 || connect(failed, peer, sizeof(peer)) != -1 ||
      *__errno() != 111 || close(failed))
    return 1;
  int first = socket(2, 1, 6);
  if (first < 0 || connect(first, peer, sizeof(peer)))
    return 2;
  int alias = dup(first);
  if (alias < 0 || close(first))
    return 3;
  int reused = socket(2, 1, 6);
  if (reused < 0 || connect(reused, peer, sizeof(peer)))
    return 4;
  out[0] = first == failed;
  out[1] = reused == first;
  const unsigned char bytes[] = {0, 0xff};
  int selected = mode < 2 ? alias : reused;
  // Both routes must identify the retained description's input contract.
  if (mode & 1)
    syscall(64, selected, bytes, sizeof(bytes));
  else
    send(selected, bytes, sizeof(bytes), 0);
  return 5;
}

u64 network_dynamic(u64 *out) {
  void *lib = dlopen("liboffline.so", 2);
  if (!lib)
    return 1;
  int (*resolve)(const char *, const char *, const struct address_info *,
                 struct address_info **) = dlsym(lib, "getaddrinfo");
  void (*release)(struct address_info *) = dlsym(lib, "freeaddrinfo");
  struct address_info hints = {0}, *p = 0;
  hints.flags = 2;
  hints.socket_type = 1;
  out[0] = resolve("node.invalid", "9001", &hints, &p);
  if (!out[0]) {
    out[1] = p->family;
    release(p);
  }
  return 0;
}

extern const char *inet_ntop(int, const void *, char *, u32);
u64 network_ntop(u64 *out, u64 mode) {
  int family = (u32)mode;
  char *dst = (char *)(out + 8);
  *__errno() = 73;
  const char *p = inet_ntop(family, family == 999 ? 0 : out + 64,
                          family == 999 ? 0 : dst, mode >> 32);
  out[0] = p == dst;
  out[1] = *__errno();
  return 0;
}

extern int setsockopt(int, int, int, const void *, u32);
extern int getsockopt(int, int, int, void *, u32 *);
u64 network_timeout(u64 *out, u64 raw) {
  int fd = socket(2, 1, 6), alias = dup(fd);
  long time[2] = {20, 0};
  *__errno() = 73;
  out[0] = raw ? syscall(208, fd, 1, 20, time, 16)
               : setsockopt(fd, 1, 20, time, 16);
  close(fd);
  u32 size = 24;
  out[1] = raw ? syscall(209, alias, 1, 20, out + 4, &size)
               : getsockopt(alias, 1, 20, out + 4, &size);
  out[2] = size;
  out[3] = *__errno();
  close(alias);
  return 0;
}

extern int inet_pton(int, const char *, void *);
u64 network_pton(u64 *out, u64 family) {
  *__errno() = 73;
  out[0] = inet_pton(family, family == 999 ? 0 : (char *)(out + 64),
                     family == 999 ? 0 : out + 8);
  out[1] = *__errno();
  return 0;
}
