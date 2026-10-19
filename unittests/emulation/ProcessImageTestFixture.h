// Original minimal PE64 file used as an image-section data fixture.
///
/// \file
/// ProcessImageTestFixture.
///

#ifndef NEVERD_TEST_PROCESS_IMAGE_FIXTURE_H
#define NEVERD_TEST_PROCESS_IMAGE_FIXTURE_H
#include "neverd/emulation/DriverProcessImages.h"

#include "llvm/Support/Endian.h"
namespace neverd::emulation {
inline DriverProcessImageMapping processImageFixture() {
  DriverProcessImageMapping Image;
  Image.MappedFile = "\\Device\\HarddiskVolume1\\app\\image.exe";
  Image.DeviceName = "\\Device\\HarddiskVolume1";
  auto &B = Image.FileBytes;
  B.resize(0x600);
  auto W16 = [&](size_t O, uint16_t V) {
    llvm::support::endian::write16le(B.data() + O, V);
  };
  auto W32 = [&](size_t O, uint32_t V) {
    llvm::support::endian::write32le(B.data() + O, V);
  };
  auto W64 = [&](size_t O, uint64_t V) {
    llvm::support::endian::write64le(B.data() + O, V);
  };
  W16(0, 0x5a4d);
  W32(60, 0x80);
  W32(0x80, 0x4550);
  W16(0x84, 0x8664);
  W16(0x86, 2);
  W16(0x94, 240);
  W16(0x96, 0x23);
  const size_t O = 0x98;
  W16(O, 0x20b);
  B[O + 2] = 14;
  W32(O + 4, 512);
  W32(O + 8, 512);
  W32(O + 16, 0x1000);
  W32(O + 20, 0x1000);
  W64(O + 24, 0x400000);
  W32(O + 32, 4096);
  W32(O + 36, 512);
  W16(O + 40, 6);
  W16(O + 48, 6);
  W32(O + 56, 0x3000);
  W32(O + 60, 0x200);
  W16(O + 68, 3);
  W64(O + 72, 0x100000);
  W64(O + 80, 0x1000);
  W64(O + 88, 0x100000);
  W64(O + 96, 0x1000);
  W32(O + 108, 16);
  const size_t T = O + 240, D = T + 40;
  B[T] = '.';
  B[T + 1] = 't';
  B[T + 2] = 'e';
  B[T + 3] = 'x';
  B[T + 4] = 't';
  W32(T + 8, 1);
  W32(T + 12, 0x1000);
  W32(T + 16, 512);
  W32(T + 20, 0x200);
  W32(T + 36, 0x60000020);
  B[D] = '.';
  B[D + 1] = 'd';
  B[D + 2] = 'a';
  B[D + 3] = 't';
  B[D + 4] = 'a';
  W32(D + 8, 4);
  W32(D + 12, 0x2000);
  W32(D + 16, 512);
  W32(D + 20, 0x400);
  W32(D + 36, 0xc0000040);
  B[0x200] = 0xc3;
  W32(0x400, 0x31415926);
  return Image;
}
} // namespace neverd::emulation
#endif
