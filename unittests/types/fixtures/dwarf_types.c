// Types that exercise NeverD's DWARF type import: anonymous unions and
// structures, bit-fields, typedefs of anonymous records, enumerations,
// arrays, function pointers, packing and alignment.  Regenerate the
// objects with:
//   gcc -g -O0 -c dwarf_types.c -o dwarf_types.gcc.o
//   clang -g -O0 -c dwarf_types.c -o dwarf_types.clang.o

#include <stdint.h>

struct list {
  struct list *next;
  struct list *prev;
};

typedef struct {
  uint32_t low;
  int32_t high;
} pair_t;

typedef union value {
  struct {
    uint32_t lo;
    int32_t hi;
  };
  pair_t pair;
  int64_t quad;
} value_t;

enum color { RED, GREEN = 5, BELOW = -1 };

typedef enum { MODE_A = 1, MODE_B = 2 } mode_t_;

struct flags {
  uint8_t kind;
  uint32_t ready : 1;
  uint32_t level : 3;
  int32_t delta : 4;
  uint64_t wide : 40;
  uint16_t tail;
};

struct device {
  uint16_t type;
  uint16_t size;
  struct list links;
  int (*dispatch[4])(struct device *device, void *irp);
  value_t time;
  enum color color;
  mode_t_ mode;
  struct flags flags;
  char name[2][3];
  const volatile uint32_t status;
  long double precise;
  _Bool on;
};

struct __attribute__((packed)) packed {
  uint8_t tag;
  uint32_t value;
  uint16_t count;
};

struct __attribute__((aligned(16))) aligned {
  uint32_t a;
};

struct tail {
  uint32_t count;
  uint8_t bytes[];
};

struct forward_only;

struct uses_forward {
  struct forward_only *pointer;
};

int use(struct device *d, struct packed *p, struct aligned *a, struct tail *t,
        struct uses_forward *f) {
  return d->size + p->count + (int)a->a + (int)t->count + (f->pointer != 0) +
         d->flags.level;
}
