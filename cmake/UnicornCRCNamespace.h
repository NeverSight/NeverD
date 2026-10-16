//===- UnicornCRCNamespace.h - Private CPU dependency symbol --------*- C -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
#ifndef NEVERD_CMAKE_UNICORNCRCNAMESPACE_H
#define NEVERD_CMAKE_UNICORNCRCNAMESPACE_H

// Bare crc32 tokens also name generated QEMU helpers; preserve those names.
#define crc32(...) neverd_unicorn_crc32(__VA_ARGS__)

#endif
