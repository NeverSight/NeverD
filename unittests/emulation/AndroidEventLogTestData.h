//===- AndroidEventLogTestData.h - Independent API 28 event bytes -*- C++
//-*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Original JNI fixture expectations shared by runtime and public API tests.
//===----------------------------------------------------------------------===//
#ifndef NEVERD_UNITTESTS_EMULATION_ANDROID_EVENT_LOG_TEST_DATA_H
#define NEVERD_UNITTESTS_EMULATION_ANDROID_EVENT_LOG_TEST_DATA_H

#include "neverd/emulation/AndroidNative.h"

#include <cstdint>
#include <utility>
#include <vector>

namespace neverd::emulation::test {
inline std::vector<NativeEventLogWrite> androidEventLogWrites(unsigned mode) {
  std::vector<NativeEventLogWrite> result;
  auto append = [&](std::vector<uint8_t> data) {
    std::vector<uint8_t> bytes{0x78, 0x56, 0x34, 0x12};
    bytes.insert(bytes.end(), data.begin(), data.end());
    result.push_back({std::move(bytes), -11});
  };
  if (mode == 0) {
    append({0, 0xf9, 0xff, 0xff, 0xff});
    append({1, 0x11, 0x22, 0x33, 0x44, 0x55, 0x66, 0x77, 0x88});
    append({4, 0, 0, 0, 0x80});
    append({2, 7, 0, 0, 0, 'A', 0xc0, 0x80, 0xf0, 0x9f, 0x98, 0x80});
    append({2, 4, 0, 0, 0, 'N', 'U', 'L', 'L'});
    append({2, 6, 0, 0, 0, '[', 'N', 'U', 'L', 'L', ']'});
    append({});
    append({0, 0xf9, 0xff, 0xff, 0xff});
    append({3,    5,    0,    0xf9, 0xff, 0xff, 0xff, 1,    0x11, 0x22, 0x33,
            0x44, 0x55, 0x66, 0x77, 0x88, 4,    0x45, 0x23, 0xc1, 0x7f, 2,
            7,    0,    0,    0,    'A',  0xc0, 0x80, 0xf0, 0x9f, 0x98, 0x80,
            2,    4,    0,    0,    0,    'N',  'U',  'L',  'L'});
  } else if (mode == 1) {
    std::vector<uint8_t> singleton{2, 0xd9, 0x0f, 0, 0};
    singleton.insert(singleton.end(), 4057, 'x');
    singleton.insert(singleton.end(), {0, 0});
    append(std::move(singleton));
    std::vector<uint8_t> list{3,    2, 0,    0xf9, 0xff, 0xff,
                              0xff, 2, 0xd4, 0x0f, 0,    0};
    list.insert(list.end(), 4052, 'x');
    append(std::move(list));
  } else if (mode == 2) {
    std::vector<uint8_t> list{3, 255};
    for (unsigned i = 0; i != 255; ++i)
      list.insert(list.end(), {0, 0xf9, 0xff, 0xff, 0xff});
    append(std::move(list));
  } else {
    for (uint32_t bits : {0u, 0x3f800000u, 1u, 0x7f7fffffu, 0x7f800000u,
                          0xff800000u, 0x7fc12345u, 0x80000001u})
      append({4, uint8_t(bits), uint8_t(bits >> 8), uint8_t(bits >> 16),
              uint8_t(bits >> 24)});
  }
  result.push_back({{123, 0, 0, 0, 0, 42, 0, 0, 0}, -11});
  return result;
}
} // namespace neverd::emulation::test

#endif // NEVERD_UNITTESTS_EMULATION_ANDROID_EVENT_LOG_TEST_DATA_H
