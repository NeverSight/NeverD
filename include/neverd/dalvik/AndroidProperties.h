//===- AndroidProperties.h - Android property value conversion --*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// API 28 integer conversion shared by Build.VERSION and live property reads.
//===----------------------------------------------------------------------===//
#ifndef NEVERD_DALVIK_ANDROIDPROPERTIES_H
#define NEVERD_DALVIK_ANDROIDPROPERTIES_H

#include <cstdint>
#include <string_view>

namespace neverd::mobile::dalvik {
int32_t androidPropertyInt(std::string_view text, int32_t fallback);
} // namespace neverd::mobile::dalvik

#endif // NEVERD_DALVIK_ANDROIDPROPERTIES_H
