/* Switch patterns for COFF/PE patch testing — Windows PE format. */

volatile int sink;

int coff_switch_simple(int x) {
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

int coff_switch_offset(int x) {
    switch (x) {
    case 100: return 1;
    case 101: return 2;
    case 102: return 3;
    case 103: return 4;
    case 104: return 5;
    default:  return 0;
    }
}

__declspec(noinline) int coff_unwind_helper(int x) {
    volatile int slots[8];
    slots[0] = x;
    return slots[0] + 1;
}

void mainCRTStartup(void) {
    sink = coff_switch_simple(3);
    sink = coff_switch_offset(102);
    sink = coff_unwind_helper(41);
    while(1) {}
}
