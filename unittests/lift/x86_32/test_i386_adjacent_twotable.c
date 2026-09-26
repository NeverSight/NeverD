int x86tt_twomachine(int a) {
  unsigned x = (unsigned)a | 1u;
  unsigned h = 0;
  unsigned st = 1;
  int g = 0;
  while (st != 0u && g++ < 4000) {
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
  unsigned st2 = 1;
  int g2 = 0;
  while (st2 != 0u && g2++ < 4000) {
    switch (st2) {
    case 1:
      x = x * 22695477u + 1u;
      h ^= x;
      st2 = 2;
      break;
    case 2:
      h = h * 33u + x;
      st2 = ((x >> 7) & 1u) ? 3u : 4u;
      break;
    case 3:
      h += x >> 2;
      st2 = ((x >> 11) & 1u) ? 4u : 5u;
      break;
    case 4:
      h ^= x << 2;
      st2 = ((x >> 12) & 1u) ? 2u : 5u;
      break;
    case 5:
      h -= x >> 6;
      st2 = ((h >> 3) & 7u) == 0u ? 0u : 1u;
      break;
    default:
      st2 = 0u;
      break;
    }
  }
  return (int)(unsigned long)(h + (unsigned)(g + g2));
}
