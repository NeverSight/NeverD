/* Enhanced jump-table patterns for testing multi-stage recovery,
   alignment checks, and complex switch patterns. */

volatile int sink;

int jt_sparse_wide(int x) {
    switch (x) {
    case 0:   return 1;
    case 10:  return 2;
    case 20:  return 3;
    case 30:  return 4;
    case 40:  return 5;
    case 50:  return 6;
    case 60:  return 7;
    case 70:  return 8;
    case 80:  return 9;
    case 90:  return 10;
    case 100: return 11;
    default:  return 0;
    }
}

int jt_two_level(int x, int y) {
    int r = 0;
    switch (x) {
    case 0:
        switch (y) {
        case 0: r = 100; break;
        case 1: r = 101; break;
        case 2: r = 102; break;
        case 3: r = 103; break;
        case 4: r = 104; break;
        default: r = -1; break;
        }
        break;
    case 1:
        switch (y) {
        case 0: r = 200; break;
        case 1: r = 201; break;
        case 2: r = 202; break;
        case 3: r = 203; break;
        default: r = -2; break;
        }
        break;
    case 2: r = 300; break;
    case 3: r = 400; break;
    case 4: r = 500; break;
    default: r = -3; break;
    }
    return r;
}

int jt_with_side_effects(int x, int *arr) {
    int r = 0;
    switch (x) {
    case 0: r = arr[0]; arr[0] += 1; break;
    case 1: r = arr[1]; arr[1] *= 2; break;
    case 2: r = arr[2]; arr[2] -= 3; break;
    case 3: r = arr[3]; arr[3] ^= 0xFF; break;
    case 4: r = arr[4]; arr[4] &= 0x0F; break;
    default: r = -1; break;
    }
    return r;
}

int jt_guard_with_sub(int x) {
    int idx = x - 50;
    switch (idx) {
    case 0:  return 1000;
    case 1:  return 1001;
    case 2:  return 1002;
    case 3:  return 1003;
    case 4:  return 1004;
    case 5:  return 1005;
    case 6:  return 1006;
    case 7:  return 1007;
    default: return -1;
    }
}

int jt_medium_32(int x) {
    switch (x) {
    case 0:  return 10;
    case 1:  return 20;
    case 2:  return 30;
    case 3:  return 40;
    case 4:  return 50;
    case 5:  return 60;
    case 6:  return 70;
    case 7:  return 80;
    case 8:  return 90;
    case 9:  return 100;
    case 10: return 110;
    case 11: return 120;
    case 12: return 130;
    case 13: return 140;
    case 14: return 150;
    case 15: return 160;
    case 16: return 170;
    case 17: return 180;
    case 18: return 190;
    case 19: return 200;
    case 20: return 210;
    case 21: return 220;
    case 22: return 230;
    case 23: return 240;
    case 24: return 250;
    case 25: return 260;
    case 26: return 270;
    case 27: return 280;
    case 28: return 290;
    case 29: return 300;
    case 30: return 310;
    case 31: return 320;
    default: return 0;
    }
}

int jt_char_dispatch(unsigned char c) {
    switch (c) {
    case 'A': return 1;
    case 'B': return 2;
    case 'C': return 3;
    case 'D': return 4;
    case 'E': return 5;
    case 'F': return 6;
    default:  return 0;
    }
}

/* Negative-base switch with distinct side-effecting bodies so it lowers to a
   real jump table (not a value-lookup collapse).  The lowest case label is
   negative, so the compiler normalizes the variable to a zero-based table index
   (idx = x + 6); switch recovery must present the source variable with the true
   negative case labels rather than the 0-based index. */
int jt_neg_dispatch(int x) {
    switch (x) {
    case -6: sink += 1;    return sink;
    case -5: sink *= 3;    return sink;
    case -4: sink -= 7;    return sink;
    case -3: sink ^= 0xaa; return sink;
    case -2: sink <<= 2;   return sink;
    case -1: sink |= 0x11; return sink;
    case  0: sink &= 0x7f; return sink;
    case  1: sink = 66;    return sink;
    case  2: sink = 77;    return sink;
    case  3: sink = 88;    return sink;
    default: sink = -1;    return sink;
    }
}

void _start(void) {
    sink = jt_sparse_wide(50);
    sink = jt_two_level(0, 2);
    int arr[5] = {1, 2, 3, 4, 5};
    sink = jt_with_side_effects(1, arr);
    sink = jt_guard_with_sub(53);
    sink = jt_medium_32(15);
    sink = jt_char_dispatch('C');
    sink = jt_neg_dispatch(-2);
    __asm__ volatile ("syscall" :: "a"(60), "D"(0));
}
