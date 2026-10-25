#pragma once
#include "neverd/ir/SourceCallTypeHint.h"
namespace neverd {
struct LowFunc;
struct MedFunc;
struct BinaryImage;
/// Recover the stack bias from the actual call/jump encoding, without decoding
/// a callee or inferring a return transport from source text.
std::optional<int64_t> sourceCallReturnAddressBias(const BinaryImage *Image,
                                                   Arch Architecture,
                                                   va_t Address);
/// Place a bounded analyst scalar/pointer declaration with the ordinary image
/// ABI. This does not authenticate the declaration or authorize native effects.
bool assignAnalystCallABI(SourceCallTypeHint &Hint, Arch Architecture,
                          BinaryFormat Format, std::string &Diagnostic);
/// System V x64 variadic floating operands also need the original AL vector
/// count; an XMM value alone does not prove that the callee saved that bank.
bool sourceCallVariadicStateDefined(const LowFunc &Function,
                                    const SourceCallTypeHint &Hint,
                                    BinaryFormat Format,
                                    std::string &Diagnostic);
/// Require every outgoing stack byte to have a known write in this same basic
/// block, after any prior call or unknown memory write.
bool sourceCallStackArgumentsDefined(const LowFunc &Function,
                                     const SourceCallTypeHint &Hint,
                                     const SourceCallOccurrenceKey &Site,
                                     Arch Architecture,
                                     int64_t ReturnAddressBias,
                                     std::string &Diagnostic);
/// Compatibility entry point for an analyst declaration carrying its site.
bool sourceCallStackArgumentsDefined(const LowFunc &Function,
                                     const SourceCallTypeHint &Hint,
                                     Arch Architecture,
                                     int64_t ReturnAddressBias,
                                     std::string &Diagnostic);
/// Require actual SSA inputs, including an indirect target; unknown clobbers,
/// incomplete live-ins, ambiguous occurrences and unseeded cycles refuse.
bool sourceCallArgumentsDefined(const MedFunc &Function,
                                const SourceCallOccurrenceKey &Site,
                                std::string &Diagnostic);
} // namespace neverd
