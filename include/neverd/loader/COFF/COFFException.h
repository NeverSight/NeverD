//===- COFFException.h - Checked PE exception decoding --------*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Declares the bounded PE/COFF exception decoders used by the loader.  The
/// record-level entry point is public so fuzzers and synthetic tests can verify
/// corrupt metadata without manufacturing an entire PE image.
///
//===----------------------------------------------------------------------===//

#ifndef NEVERD_LOADER_COFF_COFFEXCEPTION_H
#define NEVERD_LOADER_COFF_COFFEXCEPTION_H

#include "neverd/loader/BinaryImage.h"

namespace neverd::coff_loader {

/// Decode one AMD64 RUNTIME_FUNCTION and the referenced UNWIND_INFO record.
/// All RVA arithmetic and image reads are checked.  Failure is represented by
/// ExceptionFunction::ParseStatus and diagnostics; partial information such as
/// a sound code range remains available to function discovery.
ExceptionFunction decodeX64ExceptionFunction(const BinaryImage &Img,
                                             va_t ImageBase,
                                             uint32_t RuntimeFunctionRVA,
                                             uint32_t BeginRVA, uint32_t EndRVA,
                                             uint32_t UnwindInfoRVA);

/// Resolve and decode language personalities after imports, symbols, and
/// executable import veneers have been discovered.  Unknown personalities are
/// retained without guessing their handler-data schema.
void resolveExceptionHandlers(BinaryImage &Img);

} // namespace neverd::coff_loader

#endif // NEVERD_LOADER_COFF_COFFEXCEPTION_H
