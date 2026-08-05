/* Enhanced jump-table patterns for ARM32 testing — TBB/TBH patterns,
   alignment verification, and compact table recovery. */

volatile int sink;

int jt_dense_arm(int x) {
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

int jt_offset_arm(int x) {
    switch (x) {
    case 50: return 1;
    case 51: return 2;
    case 52: return 3;
    case 53: return 4;
    case 54: return 5;
    case 55: return 6;
    default: return 0;
    }
}

int jt_nested_arm(int x, int y) {
    switch (x) {
    case 0:
        switch (y) {
        case 0: return 100;
        case 1: return 200;
        case 2: return 300;
        default: return -1;
        }
    case 1: return 10;
    case 2: return 20;
    case 3: return 30;
    default: return -2;
    }
}

int jt_large_arm(int x) {
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
    default: return -1;
    }
}

int jt_shared_arm(int x) {
    switch (x) {
    case 0:
    case 1:
        return 10;
    case 2:
    case 3:
        return 20;
    case 4:
        return 30;
    default:
        return 0;
    }
}

void _start(void) {
    sink = jt_dense_arm(3);
    sink = jt_offset_arm(53);
    sink = jt_nested_arm(0, 1);
    sink = jt_large_arm(7);
    sink = jt_shared_arm(2);
    __asm__ volatile ("mov r7, #1\nswi 0" ::: "r7");
}
