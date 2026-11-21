/* DWARF 2 records the bit offset from the high bit of the storage unit.
 * Build each fixture with clang -target mips{,el}-unknown-linux-gnu -gdwarf-2
 * -c dwarf_mips_bitfields.c. The initialized data also records native packing.
 */
struct HeaderBits {
  unsigned version : 3;
  unsigned kind : 7;
  unsigned length : 22;
};

union Header {
  struct HeaderBits fields;
  unsigned word;
};

struct Packet {
  unsigned tag;
  union Header header;
};

const struct Packet sample = {0x01020304, {.fields = {5, 0x53, 0x234567}}};
