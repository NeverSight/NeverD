/* DWARF 2 records the bit offset from the high bit of the storage unit.
 * Build each fixture with clang -target mips{,el}-unknown-linux-gnu -gdwarf-2
 * -fdebug-compilation-dir=. -c dwarf_mips_bitfields.c. The initialized data also records native packing.
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

struct __attribute__((packed)) PackedBits {
  unsigned prefix : 27;
  unsigned value : 12;
  unsigned char trailer;
};

union PackedHeader {
  struct PackedBits fields;
  unsigned char bytes[sizeof(struct PackedBits)];
};

const union PackedHeader packed_sample = {
    .fields = {0x4545454, 0x91b, 0x82}};
