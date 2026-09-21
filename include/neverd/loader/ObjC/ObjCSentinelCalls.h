#ifndef NEVERD_LOADER_OBJC_OBJCSENTINELCALLS_H
#define NEVERD_LOADER_OBJC_OBJCSENTINELCALLS_H

#include "neverd/ir/SourceCallTypeHint.h"

#include "llvm/ADT/ArrayRef.h"
#include "llvm/ADT/StringRef.h"

namespace neverd {
struct BinaryImage;

/// The bounded first sentinel contract: an exact platform NSSet class object
/// and the SDK's +setWithObjects: declaration with sentinel(0,1). This does not
/// inspect outgoing machine arguments or select a method implementation.
bool objcSentinelReceiverValid(const BinaryImage &Image,
                               llvm::StringRef Selector,
                               const ObjCReceiverTypeHint &Receiver);

/// Non-null objects occur in original argument order; an empty sequence means
/// the fixed firstObject itself is nil. Each object must be a verified
/// immutable Objective-C string. The caller separately proves the complete
/// machine values through the first nil; this function supplies their real
/// variadic ABI.
std::optional<SourceCallTypeHint>
objcSentinelSourceCallHint(const BinaryImage &Image, llvm::StringRef Selector,
                           const ObjCReceiverTypeHint &Receiver,
                           llvm::ArrayRef<va_t> Objects);
/// Revalidate the exact selector-loading ARM64 dispatch used by this first
/// sentinel projection. Generic dynamic-selector calls remain unsupported.
std::optional<SourceCallTypeHint>
objcSelectorStubSentinelSourceCallHint(const BinaryImage &Image, va_t Address,
                                       const ObjCReceiverTypeHint &Receiver,
                                       llvm::ArrayRef<va_t> Objects);
} // namespace neverd
#endif
