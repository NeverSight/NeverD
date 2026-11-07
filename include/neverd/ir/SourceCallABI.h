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
/// Place a bounded analyst declaration with the ordinary image ABI. Fixed and
/// variadic Darwin ARM64 calls include 128-bit values and supported direct
/// records. Indirect parameters retain their original pointer transport; they
/// are not converted to new source-level by-value copies. This does not
/// authenticate the declaration or authorize native effects.
bool assignAnalystCallABI(SourceCallTypeHint &Hint, Arch Architecture,
                          BinaryFormat Format, std::string &Diagnostic);
/// Value type for an ABI-lowered call parameter of a validated C signature.
/// An indirect aggregate transports its existing address, without an extra
/// load/copy, alignment promise, lifetime change or aliasing attribute.
TypeRef sourceCallParameterType(const SourceParameterTypeHint &Parameter);
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
/// Bit-preserving copies/slices require only the source bytes they observe;
/// unused record padding cannot supply a defined argument value.
bool sourceCallArgumentsDefined(const MedFunc &Function,
                                const SourceCallOccurrenceKey &Site,
                                std::string &Diagnostic);
} // namespace neverd
