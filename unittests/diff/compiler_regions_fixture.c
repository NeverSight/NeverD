/* Owned unsigned-arithmetic fixture. Compiled for inspection, never executed.
 * Every shift is defined and all arithmetic wraps in the unsigned target type.
 */
#ifndef FINAL_ADD
#define FINAL_ADD 3u
#endif

__attribute__((noinline, used)) unsigned transform(unsigned x, unsigned y) {
  unsigned value = (x + y) ^ 23u;
  value = value * 7u + 9u;
  value = (value >> 5) | (value << 27);
  return (value ^ (y * 13u + 17u)) + FINAL_ADD;
}
