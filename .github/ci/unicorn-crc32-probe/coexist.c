#include <stdint.h>
#include <string.h>
#include <zlib.h>
extern uint32_t crc32c(uint32_t, const uint8_t *, unsigned);
int main(void) {
 const unsigned char data[] = "123456789";
 unsigned char packed[128], unpacked[128];
 uLongf packed_size=sizeof(packed), unpacked_size=sizeof(unpacked);
 if (crc32c(0xffffffff, data, 9) != 0xe3069283u) return 1;
 if (crc32(0, data, 9) != 0xcbf43926u) return 2;
 if (crc32_z(0, data, 9) != 0xcbf43926u) return 5;
 if (compress(packed, &packed_size, data, 9) != Z_OK) return 3;
 if (uncompress(unpacked, &unpacked_size, packed, packed_size) != Z_OK) return 4;
 return unpacked_size != 9 || memcmp(data, unpacked, 9);
}
