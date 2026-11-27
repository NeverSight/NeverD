//===- PythonVersions.h - CPython file recovery profiles --------*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Release identities used both for selecting raw code in the workbench and
/// for interpreting the four signature bytes of a compiled file.
//===----------------------------------------------------------------------===//

#ifndef NEVERD_ANALYSIS_PYTHONVERSIONS_H
#define NEVERD_ANALYSIS_PYTHONVERSIONS_H

#include <cstdint>

namespace neverd::python {
struct Release {
  unsigned Version;
  const char *Number;
  uint32_t Signature;
};

inline constexpr Release Releases[] = {
    {8, "0.9.8", 0x00949494},  {9, "0.9.9", 0x00999901},
    {100, "1.0", 0x00999902},  {101, "1.1", 0x00999903},
    {102, "1.2", 0x00999903},  {103, "1.3", 0x0a0d2e89},
    {104, "1.4", 0x0a0d1704},  {105, "1.5", 0x0a0d4e99},
    {106, "1.6", 0x0a0dc4fc},  {200, "2.0", 0x0a0dc687},
    {201, "2.1", 0x0a0deb2a},  {202, "2.2", 0x0a0ded2d},
    {203, "2.3", 0x0a0df23b},  {204, "2.4", 0x0a0df26d},
    {205, "2.5", 0x0a0df2b3},  {206, "2.6", 0x0a0df2d1},
    {207, "2.7", 0x0a0df303},  {300, "3.0", 0x0a0d0c3b},
    {301, "3.1", 0x0a0d0c4f},  {302, "3.2", 0x0a0d0c6c},
    {303, "3.3", 0x0a0d0c9e},  {304, "3.4", 0x0a0d0cee},
    {305, "3.5", 0x0a0d0d17},  {306, "3.6", 0x0a0d0d33},
    {307, "3.7", 0x0a0d0d42},  {308, "3.8", 0x0a0d0d55},
    {309, "3.9", 0x0a0d0d61},  {310, "3.10", 0x0a0d0d6f},
    {311, "3.11", 0x0a0d0da7}, {312, "3.12", 0x0a0d0dcb},
    {313, "3.13", 0x0a0d0df3}, {314, "3.14", 0x0a0d0e2b},
    {315, "3.15", 0x0a0d0e52},
};
} // namespace neverd::python

#endif // NEVERD_ANALYSIS_PYTHONVERSIONS_H
