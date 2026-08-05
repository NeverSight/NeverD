/* Switch / jump-table patterns for AArch64 */

volatile int sink;

int switch_simple(int x) {
    switch (x) {
    case 0: return 10;
    case 1: return 20;
    case 2: return 30;
    case 3: return 40;
    case 4: return 50;
    default: return -1;
    }
}

int switch_dense(int x) {
    switch (x) {
    case 0: return 100;
    case 1: return 200;
    case 2: return 300;
    case 3: return 400;
    case 4: return 500;
    case 5: return 600;
    case 6: return 700;
    case 7: return 800;
    default: return 0;
    }
}

int switch_with_fallthrough(int x) {
    int r = 0;
    switch (x) {
    case 0: r += 1; /* fallthrough */
    case 1: r += 2; break;
    case 2: r += 4; break;
    case 3: r += 8; break;
    default: r = -1; break;
    }
    return r;
}

int switch_gaps(int x) {
    switch (x) {
    case 0:  return 1;
    case 5:  return 2;
    case 10: return 3;
    case 15: return 4;
    case 20: return 5;
    default: return 0;
    }
}

int switch_nested(int x, int y) {
    switch (x) {
    case 0:
        switch (y) {
        case 0: return 1;
        case 1: return 2;
        case 2: return 3;
        default: return 4;
        }
    case 1: return 10;
    case 2: return 20;
    case 3: return 30;
    default: return -1;
    }
}

int switch_large(int x) {
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

int switch_with_computation(int x, int y) {
    int r;
    switch (x) {
    case 0: r = y * 2; break;
    case 1: r = y + 10; break;
    case 2: r = y - 5; break;
    case 3: r = y / 3; break;
    case 4: r = y << 2; break;
    default: r = 0; break;
    }
    return r;
}

int switch_shared_cases(int x) {
    switch (x) {
    case 0: case 1: case 2: return 100;
    case 3: case 4: return 200;
    case 5: return 300;
    default: return 0;
    }
}

int switch_offset_base(int x) {
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

int switch_enum_like(int cmd) {
    switch (cmd) {
    case 10: return 1000;
    case 11: return 1100;
    case 12: return 1200;
    case 13: return 1300;
    case 14: return 1400;
    case 15: return 1500;
    case 16: return 1600;
    case 17: return 1700;
    default: return -1;
    }
}

void _start(void) {
    sink = switch_simple(2);
    sink = switch_dense(5);
    sink = switch_with_fallthrough(0);
    sink = switch_gaps(10);
    sink = switch_nested(0, 1);
    sink = switch_large(7);
    sink = switch_with_computation(2, 10);
    sink = switch_shared_cases(3);
    sink = switch_offset_base(103);
    sink = switch_enum_like(14);
    __asm__ volatile ("mov x8, #93\n\t" "mov x0, #0\n\t" "svc #0");
}
