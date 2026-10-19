//===- DriverResultView.h - Read-only bounded trace serialization -*- C++
//-*-===//
///
/// \file
/// Read-only bounded trace serialization.
///

#ifndef NEVERD_EMULATION_DRIVERRESULTVIEW_H
#define NEVERD_EMULATION_DRIVERRESULTVIEW_H
#include <cstdint>
#include <string>
namespace neverd::emulation {
struct DriverResult;
struct DriverResultView {
  bool Enabled = false;
  uint64_t CallTail = UINT64_MAX, WriteTail = UINT64_MAX;
};
/// Only the serialized view changes. Retained events, model state and budgets
/// remain authoritative and untouched; the legacy report remains complete.
std::string driverResultJSONView(const DriverResult &,
                                 const DriverResultView &);
} // namespace neverd::emulation
#endif
