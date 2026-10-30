/* Mimics the linker layouts MinGW produces in a linked i686 PE: a callable
   dispatch table whose entries the code reaches through call [table+idx*s],
   and a dense switch whose jump table the linker places in .rdata.  Every
   entry is relocation-covered, so lifting demands the tables-among-code
   handling instead of a per-slot callable identity. */

typedef void (*Op)(int);

static void op_zero(int x) {
  volatile int v = x + 1;
  (void)v;
}

static void op_one(int x) {
  volatile int v = x + 2;
  (void)v;
}

static Op const Ops[2] = {op_zero, op_one};

int dispatch(int x) {
  /* The masked parameter keeps the read a runtime-indexed table access. */
  Ops[x & 1](x);
  return 0;
}

int classify(int x) {
  switch (x & 15) {
  case 0:
    return 1;
  case 1:
    return 8;
  case 2:
    return 27;
  case 3:
    return 64;
  case 4:
    return 125;
  case 5:
    return 216;
  case 6:
    return 343;
  case 7:
    return 512;
  case 8:
    return 729;
  case 9:
    return 1000;
  case 10:
    return 1331;
  case 11:
    return 1728;
  case 12:
    return 2197;
  case 13:
    return 2744;
  case 14:
    return 3375;
  default:
    return x;
  }
}

int main(void) { return dispatch(0) + classify(3); }
