/* Enhanced jump-table patterns for AArch64 testing — alignment checks,
   multi-stage recovery, and architecture-specific TBB/TBH patterns. */

volatile int sink;

int jt_dense_a64(int x) {
    switch (x) {
    case 0:  return 10;
    case 1:  return 20;
    case 2:  return 30;
    case 3:  return 40;
    case 4:  return 50;
    case 5:  return 60;
    case 6:  return 70;
    case 7:  return 80;
    default: return 0;
    }
}

int jt_offset_a64(int x) {
    switch (x) {
    case 100: return 1;
    case 101: return 2;
    case 102: return 3;
    case 103: return 4;
    case 104: return 5;
    case 105: return 6;
    default:  return 0;
    }
}

int jt_nested_a64(int x, int y) {
    switch (x) {
    case 0:
        switch (y) {
        case 0: return 100;
        case 1: return 200;
        case 2: return 300;
        case 3: return 400;
        default: return -1;
        }
    case 1: return 10;
    case 2: return 20;
    case 3: return 30;
    case 4: return 40;
    default: return -2;
    }
}

int jt_large_a64(int x) {
    switch (x) {
    case 0:  return 0;
    case 1:  return 1;
    case 2:  return 4;
    case 3:  return 9;
    case 4:  return 16;
    case 5:  return 25;
    case 6:  return 36;
    case 7:  return 49;
    case 8:  return 64;
    case 9:  return 81;
    case 10: return 100;
    case 11: return 121;
    case 12: return 144;
    case 13: return 169;
    case 14: return 196;
    case 15: return 225;
    case 16: return 256;
    case 17: return 289;
    case 18: return 324;
    case 19: return 361;
    default: return -1;
    }
}

int jt_shared_targets_a64(int x) {
    switch (x) {
    case 0:
    case 1:
    case 2:
        return 10;
    case 3:
    case 4:
        return 20;
    case 5:
        return 30;
    case 6:
    case 7:
        return 40;
    default:
        return 0;
    }
}

void _start(void) {
    sink = jt_dense_a64(3);
    sink = jt_offset_a64(103);
    sink = jt_nested_a64(0, 2);
    sink = jt_large_a64(10);
    sink = jt_shared_targets_a64(4);
    __asm__ volatile ("mov x8, #93\nsvc #0" ::: "x8");
}
