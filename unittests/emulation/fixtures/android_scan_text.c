/*===- android_scan_text.c - Independent narrow scanf callers -------------===*
 *
 * NeverD Decompiler
 *
 *===----------------------------------------------------------------------===*/
/** \file
 * Exercises direct and variadic Android LP64 scanning with guarded outputs.
 */
typedef __builtin_va_list va_list;
extern int sscanf(const char *, const char *, ...);
extern int vsscanf(const char *, const char *, va_list);
extern int *__errno(void);

typedef unsigned long u64;
static const char tag[] = "authored";
const char *volatile scan_text_relocation = tag;

__attribute__((noinline)) static int scan_va(const char *input,
                                             const char *format, ...) {
  va_list args, copy;
  __builtin_va_start(args, format);
  __builtin_va_copy(copy, args);
  int first = vsscanf(input, format, args);
  int second = vsscanf(input, format, copy);
  __builtin_va_end(copy);
  __builtin_va_end(args);
  return first == second ? first : -99;
}

__attribute__((noinline)) static int scan_stack(const char *input,
                                                const char *format, u64 a,
                                                u64 b, u64 c, u64 d, u64 e,
                                                u64 last, ...) {
  va_list args;
  __builtin_va_start(args, last);
  int result = vsscanf(input, format, args);
  __builtin_va_end(args);
  return a + b + c + d + e + last == 21 ? result : -99;
}

u64 scan_text(unsigned char *out, const char *input, const char *format,
              u64 mode) {
#define OUTPUTS                                                                \
  out, out + 64, out + 128, out + 192, out + 256, out + 320, out + 384,        \
      out + 448, out + 512, out + 576, out + 640, out + 704
  *__errno() = 77;
  int result;
  if (mode == 0)
    result = sscanf(input, format, OUTPUTS);
  else if (mode == 1)
    result = scan_va(input, format, OUTPUTS);
  else
    result = scan_stack(input, format, 1, 2, 3, 4, 5, 6, OUTPUTS);
#undef OUTPUTS
  return *__errno() == 77 ? (unsigned)result : 0x100000000UL;
}

u64 scan_text_raw(const char *input, const char *format, void *list) {
  return (unsigned)vsscanf(input, format, *(va_list *)list);
}

u64 scan_text_alias(unsigned char *out) {
  return (unsigned)sscanf("abcd XYZ", "%3c%*c %s", out, out + 1);
}
