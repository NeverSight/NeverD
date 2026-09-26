// Three sequential O0 PIC switches share a spilled GOT base.
// Keep the normal compiler frame loads and independent loop predicates.
int i386_three_switch_loops(int a) {
  unsigned x = (unsigned)a | 1u;
  unsigned h = 0;
  unsigned st = 1;
  int g = 0;
  while (st != 0u && g++ < 3000) {
    switch (st) {
    case 1:
      x = x * 1103515245u + 12345u;
      h ^= x;
      st = 2;
      break;
    case 2:
      h = h * 31u + x;
      st = ((x >> 8) & 1u) ? 3u : 4u;
      break;
    case 3:
      h += x >> 3;
      st = ((x >> 9) & 1u) ? 4u : 5u;
      break;
    case 4:
      h ^= x << 1;
      st = ((x >> 10) & 1u) ? 2u : 5u;
      break;
    case 5:
      h -= x >> 5;
      st = ((h >> 4) & 7u) == 0u ? 0u : 1u;
      break;
    default:
      st = 0u;
      break;
    }
  }
  unsigned s2 = 1;
  int g2 = 0;
  while (s2 != 0u && g2++ < 3000) {
    switch (s2) {
    case 1:
      x = x * 22695477u + 1u;
      h ^= x;
      s2 = 2;
      break;
    case 2:
      h = h * 33u + x;
      s2 = ((x >> 7) & 1u) ? 3u : 4u;
      break;
    case 3:
      h += x >> 2;
      s2 = ((x >> 11) & 1u) ? 4u : 5u;
      break;
    case 4:
      h ^= x << 2;
      s2 = ((x >> 12) & 1u) ? 2u : 5u;
      break;
    case 5:
      h -= x >> 6;
      s2 = ((h >> 3) & 7u) == 0u ? 0u : 1u;
      break;
    default:
      s2 = 0u;
      break;
    }
  }
  unsigned s3 = 1;
  int g3 = 0;
  while (s3 != 0u && g3++ < 3000) {
    switch (s3) {
    case 1:
      x = x * 214013u + 2531011u;
      h ^= x;
      s3 = 2;
      break;
    case 2:
      h = h * 29u + x;
      s3 = ((x >> 6) & 1u) ? 3u : 4u;
      break;
    case 3:
      h += x >> 4;
      s3 = ((x >> 13) & 1u) ? 4u : 5u;
      break;
    case 4:
      h ^= x << 3;
      s3 = ((x >> 14) & 1u) ? 2u : 5u;
      break;
    case 5:
      h -= x >> 7;
      s3 = ((h >> 5) & 7u) == 0u ? 0u : 1u;
      break;
    default:
      s3 = 0u;
      break;
    }
  }
  return (int)(unsigned long)(h + (unsigned)(g + g2 + g3));
}
