//===- X86VirtualizationFixtures.h - VMX/SVM encodings -------------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Independent synthetic instructions shared by lift and source regressions.
//===----------------------------------------------------------------------===//

#ifndef NEVERD_UNITTESTS_LIFT_X86_64_X86VIRTUALIZATIONFIXTURES_H
#define NEVERD_UNITTESTS_LIFT_X86_64_X86VIRTUALIZATIONFIXTURES_H

#include "neverd/ir/intrinsics/Intrinsics.h"

#include <cstdint>
#include <vector>

namespace neverd::test {
struct Encoding {
  Intrinsic Id;
  std::vector<uint8_t> Bytes;
};

inline const Encoding Encodings[] = {
    {Intrinsic::VmxRead, {0x0f, 0x78, 0xc8}},
    {Intrinsic::VmxReadMemory, {0x0f, 0x78, 0x0c, 0x24}},
    {Intrinsic::VmxWrite, {0x0f, 0x79, 0xca}},
    {Intrinsic::VmxWriteMemory, {0x0f, 0x79, 0x0c, 0x24}},
    {Intrinsic::VmxOn, {0xf3, 0x0f, 0xc7, 0x34, 0x24}},
    {Intrinsic::VmxClear, {0x66, 0x0f, 0xc7, 0x34, 0x24}},
    {Intrinsic::VmxPtrLoad, {0x0f, 0xc7, 0x34, 0x24}},
    {Intrinsic::VmxPtrStore, {0x0f, 0xc7, 0x3c, 0x24}},
    {Intrinsic::VmxOff, {0x0f, 0x01, 0xc4}},
    {Intrinsic::VmxLaunch, {0x0f, 0x01, 0xc2}},
    {Intrinsic::VmxResume, {0x0f, 0x01, 0xc3}},
    {Intrinsic::VmxFunction, {0x0f, 0x01, 0xd4}},
    {Intrinsic::VmxInvalidateEpt, {0x66, 0x0f, 0x38, 0x80, 0x0c, 0x24}},
    {Intrinsic::VmxInvalidateVpid, {0x66, 0x0f, 0x38, 0x81, 0x0c, 0x24}},
    {Intrinsic::SvmLoad, {0x0f, 0x01, 0xda}},
    {Intrinsic::SvmSave, {0x0f, 0x01, 0xdb}},
    {Intrinsic::SvmStgi, {0x0f, 0x01, 0xdc}},
    {Intrinsic::SvmClgi, {0x0f, 0x01, 0xdd}},
    {Intrinsic::SvmInvalidate, {0x0f, 0x01, 0xdf}},
};

} // namespace neverd::test

#endif // NEVERD_UNITTESTS_LIFT_X86_64_X86VIRTUALIZATIONFIXTURES_H
