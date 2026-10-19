//===- KernelProcessCreate.h - Windows 19041 create notification layout
//----===//
// NeverD Decompiler
///
/// \file
/// Windows 19041 create notification layout.
///

#ifndef NEVERD_KERNELPROCESSCREATE_H
#define NEVERD_KERNELPROCESSCREATE_H
namespace neverd::emulation::process_create {
inline constexpr unsigned Size = 72, Flags = 8, ParentID = 16,
                          CreatorProcessID = 24, CreatorThreadID = 32,
                          FileObject = 40, ImageName = 48, CommandLine = 56,
                          Status = 64;
}
#endif
