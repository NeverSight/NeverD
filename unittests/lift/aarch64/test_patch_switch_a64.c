/* Switch patterns for AArch64 ELF patch testing — no inline asm. */

volatile int sink;

int patch_switch_a64(int x) {
    switch (x) {
    case 0: return 10;
    case 1: return 20;
    case 2: return 30;
    case 3: return 40;
    case 4: return 50;
    case 5: return 60;
    case 6: return 70;
    case 7: return 80;
    default: return 0;
    }
}

int patch_switch_offset_a64(int x) {
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
    sink = patch_switch_a64(3);
    sink = patch_switch_offset_a64(102);
    while(1) {}
}
