// Minimal host-native strict-sanitizer runtime fixture.
//
// The sole counted write targets an authenticated typed eight-byte global
// array with exactly five bytes remaining at output_dst+3. stdin controls its
// length through read(0), while write(1) makes successful behavior observable
// without introducing another counted memory write.

#if defined(_WIN32)
typedef unsigned long long fixture_size_t;
extern int _read(int, void *, unsigned);
extern int _write(int, const void *, unsigned);
#else
typedef unsigned long fixture_size_t;
extern long read(int, void *, unsigned long);
extern long write(int, const void *, unsigned long);
#endif

extern void *memcpy(void *, const void *, fixture_size_t);

unsigned char output_dst[8];

int main(void) {
  unsigned char input_src[16];
#if defined(_WIN32)
  int n = _read(0, input_src, 16);
#else
  long n = read(0, input_src, 16);
#endif
  if (n < 0)
    return 2;

  memcpy(output_dst + 3, input_src, (fixture_size_t)n);

  // Give in-place rewriting a deliberately large original function span.
  // The unconditional branch makes the NOP region unreachable to NeverD's
  // CFG lift, so regenerated guarded code compacts it away instead of growing
  // by the same amount.  Both supported host architectures use 512 bytes.
#if defined(__x86_64__) || defined(_M_X64)
  __asm__ volatile("jmp 1f\n"
                   ".rept 512\n"
                   "nop\n"
                   ".endr\n"
                   "1:\n");
#elif defined(__aarch64__) || defined(_M_ARM64)
  __asm__ volatile("b 1f\n"
                   ".rept 128\n"
                   "nop\n"
                   ".endr\n"
                   "1:\n");
#else
#error "runtime sanitizer CLI fixture supports only x86-64 and AArch64 hosts"
#endif

#if defined(_WIN32)
  int wrote = _write(1, output_dst + 3, (unsigned)n);
#else
  long wrote = write(1, output_dst + 3, (unsigned long)n);
#endif
  return wrote == n ? 0 : 3;
}
