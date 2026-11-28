// Linux places argc, argv, envp and the auxiliary vector at the entry SP.
// n32 uses 32-bit pointers and ELF32 program headers on 64-bit registers.
typedef __UINTPTR_TYPE__ Word;
typedef __UINT32_TYPE__ U32;
typedef __UINT64_TYPE__ U64;

struct ProgramHeader {
#if __SIZEOF_POINTER__ == 8
  U32 type, flags;
  U64 offset, vaddr, paddr, filesz, memsz, align;
#else
  U32 type, offset, vaddr, paddr, filesz, memsz, flags, align;
#endif
};

enum { PT_LOAD = 1, PF_X = 1, PF_R = 4 };
enum { AT_PHDR = 3, AT_PHENT = 4, AT_PHNUM = 5, AT_PAGESZ = 6, AT_ENTRY = 9 };

extern void _start(void);
extern unsigned calc(unsigned);

static int environment_value(const char *text) {
  static const char key[] = "NEVERD_RELINK_INPUT=";
  for (unsigned i = 0; i < sizeof(key) - 1; ++i)
    if (text[i] != key[i])
      return -1;
  char value = text[sizeof(key) - 1];
  if ((value != 'A' && value != 'B') || text[sizeof(key)] != 0)
    return -1;
  return value - 'A';
}

static int check_program_headers(const Word *auxv) {
  Word address = 0, count = 0, stride = 0, entry = 0, page_size = 0;
  for (; auxv[0] != 0; auxv += 2) {
    switch (auxv[0]) {
    case AT_PHDR:
      address = auxv[1];
      break;
    case AT_PHENT:
      stride = auxv[1];
      break;
    case AT_PHNUM:
      count = auxv[1];
      break;
    case AT_PAGESZ:
      page_size = auxv[1];
      break;
    case AT_ENTRY:
      entry = auxv[1];
      break;
    }
  }
  if (entry != (Word)&_start || !address ||
      stride != sizeof(struct ProgramHeader) || count == 0 || count > 64 ||
      page_size < 4096 || (page_size & (page_size - 1)))
    return 124;

  const struct ProgramHeader *table = (const struct ProgramHeader *)address;
  const Word function = (Word)&calc;
  int has_entry = 0, has_function = 0, has_table = 0;
  for (Word i = 0; i < count; ++i) {
    const struct ProgramHeader *p = table + i;
    if (p->type != PT_LOAD)
      continue;
    if (p->flags & PF_X) {
      has_entry |= entry >= p->vaddr && entry - p->vaddr < p->filesz;
      has_function |= function >= p->vaddr && function - p->vaddr < p->filesz;
    }
    if ((p->flags & PF_R) && address >= p->vaddr &&
        address - p->vaddr < p->filesz)
      has_table |= count * stride <= p->filesz - (address - p->vaddr);
  }
  return has_entry && has_function && has_table ? 0 : 125;
}

int check_start(const Word *stack) {
  if (stack[0] != 2 || !stack[1] || !stack[2] || stack[3] != 0)
    return 121;
  const char *argument = (const char *)stack[2];
  if ((argument[0] != 'C' && argument[0] != 'H') || argument[1] != 0)
    return 122;

  int value = -1;
  const Word *envp = stack + 4;
  for (; *envp != 0; ++envp) {
    int found = environment_value((const char *)*envp);
    if (found >= 0)
      value = found;
  }
  if (value < 0)
    return 123;
  int error = check_program_headers(envp + 1);
  if (error)
    return error;
  return calc((unsigned)(argument[0] - 'C' + value * 2));
}
