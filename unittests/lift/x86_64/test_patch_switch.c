/* Switch patterns for ELF patch testing — no inline asm. */

volatile int sink;

int patch_switch_simple(int x) {
    switch (x) {
    case 0: return 10;
    case 1: return 20;
    case 2: return 30;
    case 3: return 40;
    case 4: return 50;
    default: return -1;
    }
}

int patch_switch_dense(int x) {
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

int patch_switch_offset(int x) {
    switch (x) {
    case 100: return 1;
    case 101: return 2;
    case 102: return 3;
    case 103: return 4;
    case 104: return 5;
    default:  return 0;
    }
}

void _start(void) {
    sink = patch_switch_simple(2);
    sink = patch_switch_dense(5);
    sink = patch_switch_offset(103);
    while(1) {}
}
