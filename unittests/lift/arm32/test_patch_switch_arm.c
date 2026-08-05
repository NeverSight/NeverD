/* Switch patterns for ARM32 ELF patch testing — no inline asm. */

volatile int sink;

int patch_switch_arm(int x) {
    switch (x) {
    case 0: return 10;
    case 1: return 20;
    case 2: return 30;
    case 3: return 40;
    case 4: return 50;
    case 5: return 60;
    default: return 0;
    }
}

int patch_switch_offset_arm(int x) {
    switch (x) {
    case 50: return 1;
    case 51: return 2;
    case 52: return 3;
    case 53: return 4;
    case 54: return 5;
    default: return 0;
    }
}

void _start(void) {
    sink = patch_switch_arm(3);
    sink = patch_switch_offset_arm(52);
    while(1) {}
}
