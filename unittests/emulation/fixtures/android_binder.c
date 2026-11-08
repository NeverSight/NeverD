/* Independent callers of the Linux LP64 Binder protocol-8 control ABI. */
typedef unsigned long long U64;
extern int open(const char *, int, ...);
extern int close(int);
extern int dup(int);
extern int ioctl(int, unsigned long, void *);
extern int *__errno(void);
extern int pthread_create(U64 *, const void *, void *(*)(void *), void *);
extern int pthread_join(U64, void **);
struct WriteRead {
  U64 size, consumed, data, read_size, read_consumed, read_data;
};
#define CHECK(X)                                                               \
  do {                                                                         \
    if (!(X))                                                                  \
      return __LINE__;                                                         \
  } while (0)
static int exchange(int fd, struct WriteRead *wr) {
  return ioctl(fd, 0xc0306201UL, wr);
}
int binder_control(void) {
  int fd = open("/dev/binder", 2 | 0x800);
  CHECK(fd >= 0);
  unsigned version[3] = {0xaabbccdd, 99, 0xaabbccdd};
  CHECK(!ioctl(fd, 0xc0046209UL, &version[1]));
  CHECK(version[0] == 0xaabbccdd && version[1] == 8 &&
        version[2] == 0xaabbccdd);
  CHECK(ioctl(fd, 0xc0046209UL, (void *)1) == -1 && *__errno() == 22);
  CHECK(ioctl(fd, 0x40046205UL, (void *)1) == -1 && *__errno() == 22);
  unsigned max = 1;
  CHECK(!ioctl(fd, 0x40046205UL, &max));
  unsigned commands[] = {0x630c, 0x630d};
  unsigned output[3] = {0x88776655, 0x88776655, 0x88776655};
  struct WriteRead wr = {8, 0, (U64)commands, 8, 0, (U64)output};
  CHECK(exchange(fd, &wr) == -1 && *__errno() == 11);
  CHECK(wr.consumed == 8 && !wr.read_consumed && output[0] == 0x720c);
  CHECK(output[1] == 0x88776655 && output[2] == 0x88776655);
  CHECK(!ioctl(fd, 0x40046208UL, (void *)1));
  wr = (struct WriteRead){4, 0, (U64)commands, 8, 0, (U64)output};
  CHECK(!exchange(fd, &wr));
  CHECK(wr.consumed == 4 && wr.read_consumed == 4 && output[0] == 0x720d);
  commands[0] = 0x630b;
  wr = (struct WriteRead){4, 0, (U64)commands, 0, 0, 1};
  CHECK(!exchange(fd, &wr) && wr.consumed == 4);
  max = 0xffffffff;
  CHECK(!ioctl(fd, 0x40046205UL, &max));
  CHECK(!ioctl(fd, 0x40046208UL, (void *)1));
  commands[0] = 0x630c;
  wr = (struct WriteRead){4, 0, (U64)commands, 8, 0, (U64)output};
  CHECK(!exchange(fd, &wr) && wr.read_consumed == 4 && output[0] == 0x720c);
  CHECK(!close(fd));
  return 0;
}
int binder_descriptions(void) {
  int fd = open("/dev/binder", 2 | 0x800);
  CHECK(fd >= 0);
  unsigned word = 0x99;
  struct WriteRead wr = {0, 0, 1, 4, 0, (U64)&word};
  CHECK(!exchange(fd, &wr) && wr.read_consumed == 4 && word == 0x720c);
  int alias = dup(fd), other = open("/dev/binder", 2 | 0x800);
  CHECK(alias >= 0 && other >= 0);
  CHECK(!close(fd));
  wr.read_consumed = 0;
  CHECK(exchange(alias, &wr) == -1 && *__errno() == 11 && !wr.read_consumed);
  CHECK(!exchange(other, &wr) && wr.read_consumed == 4);
  CHECK(!ioctl(alias, 0x40046208UL, (void *)1));
  wr.read_consumed = 0;
  CHECK(!exchange(alias, &wr) && wr.read_consumed == 4);
  CHECK(!close(alias) && !close(other));
  return 0;
}
static void *child(void *arg) {
  int fd = (int)(U64)arg;
  unsigned word = 0;
  struct WriteRead wr = {0, 0, 1, 4, 0, (U64)&word};
  if (exchange(fd, &wr) || wr.read_consumed != 4 || word != 0x720c)
    return (void *)1;
  return (void *)(U64)ioctl(fd, 0x40046208UL, 0);
}
int binder_threads(void) {
  int fd = open("/dev/binder", 2 | 0x800);
  CHECK(fd >= 0);
  unsigned version = 0;
  CHECK(!ioctl(fd, 0xc0046209UL, &version));
  U64 id = 0;
  void *result = (void *)99;
  CHECK(!pthread_create(&id, 0, child, (void *)(U64)fd));
  CHECK(!pthread_join(id, &result) && !result);
  unsigned word = 0;
  struct WriteRead wr = {0, 0, 0, 4, 0, (U64)&word};
  CHECK(exchange(fd, &wr) == -1 && *__errno() == 11 && !wr.read_consumed);
  CHECK(!close(fd));
  return 0;
}
int binder_unknown_transaction(void) {
  int fd = open("/dev/binder", 2);
  unsigned command = 0x40406300;
  struct WriteRead wr = {4, 0, (U64)&command, 0, 0, 0};
  return exchange(fd, &wr);
}
int binder_blocking(void) {
  int fd = open("/dev/binder", 2);
  unsigned word = 0;
  CHECK(!ioctl(fd, 0xc0046209UL, &word));
  struct WriteRead wr = {0, 0, 0, 4, 0, (U64)&word};
  return exchange(fd, &wr);
}
int binder_partial_command(void) {
  int fd = open("/dev/binder", 2);
  unsigned command = 0x630c;
  struct WriteRead wr = {3, 0, (U64)&command, 0, 0, 0};
  return exchange(fd, &wr);
}

extern void *mmap(void *, U64, int, int, int, U64);
extern int munmap(void *, U64);
extern void *memset(void *, int, U64);
extern int memcmp(const void *, const void *, U64);
struct Transaction {
  U64 target, cookie;
  unsigned code, flags, sender_pid, sender_euid;
  U64 data_size, offsets_size, buffer, offsets;
};
_Static_assert(sizeof(struct Transaction) == 64, "Binder LP64 record");
static void bytes(void *to, const void *from, unsigned n) {
  for (unsigned i = 0; i < n; ++i)
    ((unsigned char *)to)[i] = ((const unsigned char *)from)[i];
}
static int send_message(int fd, unsigned command,
                        const struct Transaction *tx) {
  unsigned char stream[68];
  bytes(stream, &command, 4);
  bytes(stream + 4, tx, 64);
  struct WriteRead wr = {68, 0, (U64)stream, 0, 0, 0};
  return exchange(fd, &wr) || wr.consumed != 68;
}
static int free_buffer(int fd, U64 pointer) {
  unsigned char stream[12];
  unsigned command = 0x40086303;
  bytes(stream, &command, 4);
  bytes(stream + 4, &pointer, 8);
  struct WriteRead wr = {12, 0, (U64)stream, 0, 0, 0};
  return exchange(fd, &wr) || wr.consumed != 12;
}
struct ServiceArgs {
  int fd;
  unsigned prefix, flags, status;
};
static int serve(struct ServiceArgs *args) {
  int fd = args->fd;
  unsigned enter = 0x630c;
  struct WriteRead wr = {4, 0, (U64)&enter, 0, 0, 0};
  CHECK(!exchange(fd, &wr));
  unsigned output[32];
  for (unsigned i = 0; i < 32; ++i)
    output[i] = 0xaabbccdd;
  wr = (struct WriteRead){0, 0, 0, sizeof(output), 0, (U64)output};
  CHECK(!exchange(fd, &wr));
  CHECK(wr.read_consumed == 72 && output[0] == args->prefix &&
        output[1] == 0x80407202);
  CHECK(output[18] == 0xaabbccdd && output[31] == 0xaabbccdd);
  struct Transaction tx;
  bytes(&tx, output + 2, 64);
  CHECK(!tx.target && !tx.cookie && tx.code == 17 && tx.flags == args->flags &&
        tx.data_size == 12 && !tx.offsets_size);
  CHECK(tx.sender_pid != 0 && tx.offsets == tx.buffer + 16);
  unsigned request[3];
  bytes(request, (void *)tx.buffer, 12);
  unsigned reply[3] = {request[0] + request[1], request[1] ^ request[2],
                       request[0] * 7};
  U64 old = tx.buffer;
  tx = (struct Transaction){0, 0, 29, 0, 0, 0, sizeof(reply), 0, (U64)reply, 0};
  if (args->status) {
    reply[0] = 0u - (request[0] + 1); /* service-produced signed status_t */
    tx.flags = 8;
    tx.data_size = 4;
  }
  CHECK(!send_message(fd, 0x40406301, &tx));
  CHECK(!free_buffer(fd, old));
  wr = (struct WriteRead){0, 0, 0, sizeof(output), 0, (U64)output};
  CHECK(!exchange(fd, &wr) && wr.read_consumed == 8 && output[0] == 0x720c &&
        output[1] == 0x7206);
  return 0;
}
static void *service_thread(void *args) { return (void *)(U64)serve(args); }
int binder_service_transactions(void) {
  int server = open("/dev/binder", 2 | 0x800),
      client = open("/dev/binder", 2 | 0x800);
  CHECK(server >= 0 && client >= 0);
  void *sm = mmap(0, 4096, 1, 2 | 0x4000, server, 0);
  void *cm = mmap(0, 4096, 1, 2, client, 0);
  CHECK(sm != (void *)-1 && cm != (void *)-1 && sm != cm);
  CHECK(!ioctl(server, 0x40046207UL, (void *)1));
  unsigned max = 1;
  CHECK(!ioctl(server, 0x40046205UL, &max));
  CHECK(ioctl(client, 0x40046207UL, (void *)1) == -1 && *__errno() == 16);
  int alias = dup(client);
  CHECK(alias >= 0);
  CHECK(!close(client));
  for (unsigned i = 1; i <= 4; ++i) {
    unsigned data[3] = {i * 13, 0x10203040 + i, 0x81234567};
    unsigned flags = i % 2 ? 0 : 16, status = i > 2;
    struct Transaction tx = {0, 0, 17, flags, 999, 999, 12, 0, (U64)data, 1};
    CHECK(!send_message(alias, 0x40406300, &tx));
    /* The request is copied; later writes to the sender cannot change it. */
    data[0] = 99;
    U64 tid;
    void *result = (void *)99;
    struct ServiceArgs args = {server, i == 1 ? 0x720d : 0x720c, flags, status};
    CHECK(!pthread_create(&tid, 0, service_thread, &args));
    CHECK(!pthread_join(tid, &result) && !result);
    unsigned output[32];
    for (unsigned j = 0; j < 32; ++j)
      output[j] = 0xabcddcba;
    struct WriteRead wr = {0, 0, 0, sizeof(output), 0, (U64)output};
    CHECK(!exchange(alias, &wr));
    CHECK(wr.read_consumed == 76 && output[0] == 0x720c &&
          output[1] == 0x7206 && output[2] == 0x80407203);
    CHECK(output[19] == 0xabcddcba && output[31] == 0xabcddcba);
    bytes(&tx, output + 3, 64);
    CHECK(tx.code == 29 && tx.flags == (status ? 8u : 0u) && !tx.sender_pid &&
          tx.data_size == (status ? 4u : 12u) && !tx.offsets_size);
    CHECK(tx.buffer >= (U64)cm && tx.buffer + 16 <= (U64)cm + 4096);
    CHECK(tx.offsets == tx.buffer + (status ? 8 : 16));
    unsigned answer[3];
    bytes(answer, (void *)tx.buffer, tx.data_size);
    if (status) {
      CHECK(answer[0] == 0u - (i * 13 + 1));
    } else {
      CHECK(answer[0] == i * 13 + 0x10203040 + i &&
            answer[1] == ((0x10203040 + i) ^ 0x81234567) &&
            answer[2] == i * 91);
    }
    CHECK(!free_buffer(alias, tx.buffer));
  }
  CHECK(!munmap(sm, 4096) && !munmap(cm, 4096));
  CHECK(!close(alias) && !close(server));
  return 0;
}
int binder_missing_targets(void) {
  int fd = open("/dev/binder", 2 | 0x800);
  CHECK(fd >= 0);
  for (unsigned handle = 0; handle < 2; ++handle) {
    struct Transaction tx = {handle, 0, 9, 0, 0, 0, 8, 0, 1, 0};
    unsigned char stream[72];
    unsigned command = 0x40406300, enter = 0x630c;
    bytes(stream, &command, 4);
    bytes(stream + 4, &tx, 64);
    bytes(stream + 68, &enter, 4);
    struct WriteRead wr = {72, 0, (U64)stream, 0, 0, 0};
    CHECK(!exchange(fd, &wr) && wr.consumed == 68);
    unsigned out[32] = {0};
    /* A pending error gates writes even when their bytes are unreadable. */
    wr = (struct WriteRead){4, 0, 1, 4, 0, (U64)out};
    CHECK(!exchange(fd, &wr) && !wr.consumed && wr.read_consumed == 4 &&
          out[0] == 0x720c);
    wr = (struct WriteRead){0, 0, 0, sizeof(out), 0, (U64)out};
    CHECK(!exchange(fd, &wr) && wr.read_consumed == 8 &&
          out[1] == (handle ? 0x7211 : 0x7205));
    wr = (struct WriteRead){72, 68, (U64)stream, 0, 0, 0};
    CHECK(!exchange(fd, &wr) && wr.consumed == 72);
  }
  CHECK(!close(fd));
  return 0;
}
int binder_object_transaction(void) {
  int fd = open("/dev/binder", 2 | 0x800);
  CHECK(fd >= 0);
  /* A missing target fails before even copying the unreadable object table. */
  struct Transaction tx = {0, 0, 9, 0, 0, 0, 0, 8, 0, 0};
  CHECK(!send_message(fd, 0x40406300, &tx));
  unsigned output[32] = {0};
  struct WriteRead wr = {0, 0, 0, sizeof(output), 0, (U64)output};
  CHECK(!exchange(fd, &wr) && wr.read_consumed == 8 && output[0] == 0x720c &&
        output[1] == 0x7205);
  int server = open("/dev/binder", 2 | 0x800);
  CHECK(server >= 0);
  void *arena = mmap(0, 4096, 1, 2, server, 0);
  CHECK(arena != (void *)-1);
  CHECK(!ioctl(server, 0x40046207UL, 0));
  /* Once the target exists, the copied offset names a missing flat object. */
  U64 offset = 0;
  tx.offsets = (U64)&offset;
  CHECK(!send_message(fd, 0x40406300, &tx));
  wr = (struct WriteRead){0, 0, 0, sizeof(output), 0, (U64)output};
  CHECK(!exchange(fd, &wr) && wr.read_consumed == 8 && output[0] == 0x720c &&
        output[1] == 0x7211);
  CHECK(!munmap(arena, 4096) && !close(server) && !close(fd));
  return 0;
}
int binder_large_arena_transactions(void) {
  int server = open("/dev/binder", 2 | 0x800);
  int clients[2] = {open("/dev/binder", 2 | 0x800),
                    open("/dev/binder", 2 | 0x800)};
  CHECK(server >= 0 && clients[0] >= 0 && clients[1] >= 0);
  const U64 arena_size = 2 * 1024 * 1024 - 8192;
  unsigned char *arena = mmap(0, arena_size, 1, 2 | 0x4000, server, 0);
  CHECK(arena != (void *)-1);
  void *client_arenas[2];
  static unsigned char payload[65536];
  static unsigned char expected[65536];
  CHECK(!ioctl(server, 0x40046207UL, 0));
  unsigned enter = 0x630c;
  struct WriteRead wr = {4, 0, (U64)&enter, 0, 0, 0};
  CHECK(!exchange(server, &wr));
  for (unsigned i = 0; i < 2; ++i) {
    client_arenas[i] = mmap(0, 4096, 1, 2, clients[i], 0);
    CHECK(client_arenas[i] != (void *)-1);
    memset(payload, 17 + i, sizeof(payload));
    payload[65535] = 63 + i;
    struct Transaction tx = {
        0, 0, 31 + i, 16, 0, 0, sizeof(payload), 0, (U64)payload, 0};
    CHECK(!send_message(clients[i], 0x40406300, &tx));
  }
  memset(payload, 0, sizeof(payload));
  U64 buffers[2];
  for (unsigned i = 0; i < 2; ++i) {
    unsigned output[32] = {0};
    wr = (struct WriteRead){0, 0, 0, sizeof(output), 0, (U64)output};
    CHECK(!exchange(server, &wr));
    CHECK(wr.read_consumed == 72 && output[0] == 0x720c);
    CHECK(output[1] == 0x80407202);
    struct Transaction tx;
    bytes(&tx, output + 2, 64);
    CHECK(tx.code == 31 + i && tx.data_size == 65536 && !tx.offsets_size);
    buffers[i] = tx.buffer;
    CHECK(tx.buffer == (U64)arena + i * 65536);
    CHECK(((unsigned char *)tx.buffer)[0] == 17 + i);
    CHECK(((unsigned char *)tx.buffer)[65535] == 63 + i);
    memset(expected, 17 + i, sizeof(expected));
    expected[65535] = 63 + i;
    CHECK(!memcmp((void *)tx.buffer, expected, sizeof(expected)));
    unsigned reply =
        ((unsigned char *)tx.buffer)[0] + ((unsigned char *)tx.buffer)[65535];
    tx = (struct Transaction){0, 0, 73, 0, 0, 0, 4, 0, (U64)&reply, 0};
    CHECK(!send_message(server, 0x40406301, &tx));
    /* Private completion work is drained before taking another process call. */
    wr = (struct WriteRead){0, 0, 0, sizeof(output), 0, (U64)output};
    CHECK(!exchange(server, &wr) && wr.read_consumed == 8 &&
          output[1] == 0x7206);
  }
  /* Both live requests occupy the larger arena; release each original owner. */
  CHECK(!free_buffer(server, buffers[0]) && !free_buffer(server, buffers[1]));
  for (unsigned i = 0; i < 2; ++i) {
    unsigned output[32] = {0};
    wr = (struct WriteRead){0, 0, 0, sizeof(output), 0, (U64)output};
    CHECK(!exchange(clients[i], &wr) && wr.read_consumed == 76 &&
          output[1] == 0x7206 && output[2] == 0x80407203);
    struct Transaction tx;
    bytes(&tx, output + 3, 64);
    CHECK(tx.data_size == 4 && *(unsigned *)tx.buffer == 80 + 2 * i);
    CHECK(!free_buffer(clients[i], tx.buffer));
    CHECK(!munmap(client_arenas[i], 4096) && !close(clients[i]));
  }
  CHECK(!munmap(arena, arena_size) && !close(server));
  return 0;
}
int binder_oneway_transaction(void) {
  int fd = open("/dev/binder", 2 | 0x800);
  struct Transaction tx = {0, 0, 9, 1, 0, 0, 0, 0, 0, 0};
  return send_message(fd, 0x40406300, &tx);
}
