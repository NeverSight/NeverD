//===- NativeMobileSession.h - Private mobile load transaction -*- C++ -*-===//
#pragma once

#include "neverd/sdk/NeverDCAPITypes.h"

namespace neverd {
struct BinaryImage;
namespace sdk {

// Same-build C++ bridge only. This header is not installed with the SDK.
// The image is borrowed for the callback only, before normalization, debug
// discovery, decoder setup or session publication. The observer must not retain
// its address, mutate it, or reenter the session. Its exceptions propagate and
// leave any previously loaded session intact.
using NativeMobileMetadataObserver = void (*)(const BinaryImage &, void *);
NEVERD_API int loadNativeMobileSession(neverd_session_t Session,
                                       const char *UTF8Path,
                                       NativeMobileMetadataObserver Observer,
                                       void *Context);

} // namespace sdk
} // namespace neverd
