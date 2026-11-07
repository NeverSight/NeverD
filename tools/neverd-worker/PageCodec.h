//===- PageCodec.h - Compact form of kept engine pages ---------*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// A small byte-oriented LZ77 codec for the engine pages the worker keeps:
/// JSON text with long repeated keys and C source.  The worker writes and
/// reads its own memory and RenderStore's files, which a different build of it
/// does not read; nothing here is a wire format.
//===----------------------------------------------------------------------===//
#ifndef NEVERD_TOOLS_NEVERD_WORKER_PAGECODEC_H
#define NEVERD_TOOLS_NEVERD_WORKER_PAGECODEC_H

#include <optional>
#include <string>
#include <string_view>

namespace neverd::worker {

/// \p raw in the compact form decompressPage reads.
std::string compressPage(std::string_view raw);
/// The bytes compressPage was given, or nothing for anything it did not
/// write whole.
std::optional<std::string> decompressPage(std::string_view compact);

} // namespace neverd::worker

#endif // NEVERD_TOOLS_NEVERD_WORKER_PAGECODEC_H
