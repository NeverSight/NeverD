//===- android_dladdr.c - Independent LP64 linker observation caller -----===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Independent Android LP64 Dl_info caller; no host headers or libraries.
///
//===----------------------------------------------------------------------===//

typedef struct {
  const char *dli_fname;
  void *dli_fbase;
  const char *dli_sname;
  void *dli_saddr;
} Dl_info;
extern int dladdr(const void *, Dl_info *);
extern void *dlopen(const char *, int);
extern void *dlsym(void *, const char *);
extern int dlclose(void *);
extern char *dlerror(void);
extern int mprotect(void *, unsigned long, int);
extern int munmap(void *, unsigned long);
enum { RTLD_NOW = 2, PROT_READ = 1, PROT_WRITE = 2 };

_Static_assert(sizeof(Dl_info) == 32, "LP64 Dl_info ABI");
typedef unsigned long word;
extern int *__errno(void);
extern int pthread_create(word *, const void *, void *(*)(void *), void *);
extern int pthread_join(word, void **);

static void copy_text(char *out, const char *text) {
  do {
    *out++ = *text;
  } while (*text++);
}

word dladdr_call(void *address, Dl_info *info) { return dladdr(address, info); }

static void *thread_query(void *out) {
  return (void *)(word)dladdr((void *)0x40001004, (Dl_info *)out);
}

word dladdr_fixture(unsigned char *out, word mode) {
  Dl_info *first = (Dl_info *)(out + 64);
  Dl_info *second = first + 1;
  int (*query)(const void *, Dl_info *) = dladdr;
  void *handle = 0;
  if (mode == 1 || mode == 2) {
    handle = dlopen("libfixture-dl.so", RTLD_NOW);
    if (!handle)
      return 11;
    query = (int (*)(const void *, Dl_info *))dlsym(handle, "dladdr");
    if (!query)
      return 12;
    if (mode == 2) {
      if (dlclose(handle))
        return 13;
      handle = 0;
    }
  }
  if (dlopen("absent-fixture.so", RTLD_NOW))
    return 14;
  *__errno() = 77;
  int returned = query((void *)0x40001004, first);
  if (returned != 1 || *__errno() != 77 || !dlerror() || dlerror())
    return 15;
  if (mode == 3) {
    word child;
    void *result = 0;
    if (pthread_create(&child, 0, thread_query, second) ||
        pthread_join(child, &result) || result != (void *)1)
      return 16;
  } else {
    if (query((void *)0x40001004, second) != 1)
      return 17;
  }
  if (first->dli_fname != second->dli_fname ||
      first->dli_fbase != second->dli_fbase ||
      first->dli_sname != second->dli_sname ||
      first->dli_saddr != second->dli_saddr)
    return 18;
  if (mode == 4) {
    *(volatile char *)first->dli_fname = '!';
    return 19;
  }
  if (mode == 5 || mode == 6) {
    void *page = (void *)((word)first->dli_fname & ~(word)4095);
    if (mode == 5) {
      if (mprotect(page, 4096, PROT_READ | PROT_WRITE))
        return 20;
      *(volatile char *)first->dli_fname = '!';
    } else if (munmap(page, 4096))
      return 21;
    return query((void *)0x40001004, second + 1);
  }
  if (query((void *)0x40002004, second + 1) != 1)
    return 22;
  if (first->dli_fbase != (void *)0x40000000 ||
      first->dli_saddr != (void *)0x40001000 || !first->dli_sname ||
      second[1].dli_fname != first->dli_fname ||
      second[1].dli_fbase != first->dli_fbase || second[1].dli_sname ||
      second[1].dli_saddr)
    return 23;
  copy_text((char *)(out + 512), first->dli_fname);
  copy_text((char *)(out + 576), first->dli_sname);
  *(word *)out = 1;
  if (handle && dlclose(handle))
    return 24;
  return 0;
}

word dladdr_readonly_tail(unsigned char *out) {
  if (mprotect(out + 4096, 4096, PROT_READ))
    return 25;
  return dladdr((void *)0x40001004, (Dl_info *)(out + 4096 - 16));
}
