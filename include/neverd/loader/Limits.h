//===- Limits.h - Component limits and defaults -----------*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Resource ceilings, work budgets and defaults owned by this component.
/// Binary layouts and instruction/ABI constants remain with their models.
///
//===----------------------------------------------------------------------===//

#ifndef NEVERD_LOADER_LIMITS_H
#define NEVERD_LOADER_LIMITS_H

#include "neverd/ir/Limits.h"

#include <cstddef>
#include <cstdint>

namespace neverd::limits::loader {

#define NEVERD_DWARF_A64_LIMIT(Type, Name, Value)                              \
  inline constexpr Type Name = Value;
#include "neverd/loader/DWARF/AArch64ReturnAddressState.def"

#define NEVERD_RA_QUERY_LIMIT(Type, Name, Value)                               \
  inline constexpr Type Name = Value;
#include "neverd/loader/ReturnAddressSigning.def"

#define NEVERD_MACHO_ARM64E_LIMIT(Type, Name, Value)                           \
  inline constexpr Type Name = Value;
#include "neverd/loader/MachO/MachOArm64e.def"

// include/neverd/loader/BinaryImageModel.h
inline constexpr size_t kBinaryImageModelMaxDiagnostics = 64;

// include/neverd/loader/DWARF/LSDA.h
inline constexpr size_t kLSDAMaxRecords = 1u << 16;

// include/neverd/loader/LanguageEHGo.h
inline constexpr uint32_t kLanguageEHGoMaxBitmaps = 1u << 12;
inline constexpr uint32_t kLanguageEHGoMaxBits = 1u << 16;
inline constexpr uint32_t kLanguageEHGoMaxTotalBytes = 1u << 20;

// lib/loader/COFF/COFFLoaderUtils.cpp
/// Whole-image instruction/slot visits for indexed pointer-table evidence.
/// Exhaustion publishes none of the partial scan's table classifications.
inline constexpr size_t kCOFFIndexedTableEvidenceWork = 1u << 20;

// lib/loader/COFF/eh/COFFDelphiEH.cpp
/// Bound on one `except on` arm table.  Delphi allows any number of arms, but
/// a count beyond this is a mis-identified descriptor rather than a program.
inline constexpr uint32_t kCOFFDelphiEHMaxOnExceptionArms = 256;
/// Largest instance size accepted while validating a VMT.
inline constexpr uint32_t kCOFFDelphiEHMaxInstanceSize = 1u << 24;
/// Bound on the number of frames recovered from one image.
inline constexpr size_t kCOFFDelphiEHMaxDelphiFrames = 1u << 18;
/// Bound on one `TExcData`.  A Delphi function can nest any number of scopes,
/// but a count past this says the handler data was not a `TExcData`.
inline constexpr uint32_t kCOFFDelphiEHMaxDelphiScopes = 4096;

// lib/loader/COFF/eh/COFFException.cpp
// Most frames installing the routine cannot prove anything about it: a scope
// with destructors and no handler is cleanup-only, and at `-O0` those come
// first.  So an address is retried on later frames rather than written off by
// the first one that could not settle it, and the cap is what keeps an
// address no frame can prove from costing one decode per frame.
inline constexpr unsigned kCOFFExceptionMaxProofAttempts = 32;

// lib/loader/COFF/eh/COFFExceptionDetail.h
inline constexpr uint32_t kCOFFExceptionDetailMaxLanguageRecords = 1u << 16;
inline constexpr unsigned kCOFFExceptionDetailMaxPersonalityVeneers = 8;

// lib/loader/COFF/eh/COFFExceptionFH4.cpp
inline constexpr uint64_t kCOFFExceptionFH4MaxBytes = 1u << 20;

// lib/loader/COFF/eh/COFFExceptionGS.cpp
inline constexpr size_t kCOFFExceptionGSMaxWrapperBytes = 512;
inline constexpr size_t kCOFFExceptionGSMaxWrapperInstructions = 64;
inline constexpr size_t kCOFFExceptionGSMaxWrapperBlocks = 32;
inline constexpr size_t kCOFFExceptionGSMaxWrapperEdges = 128;

// lib/loader/COFF/eh/COFFRegistrationEHHandler.cpp
/// Bound on veneer/thunk chasing when naming a handler.
inline constexpr unsigned kCOFFRegistrationEHHandlerMaxHandlerThunkHops = 4;
// The `mov`/`jmp` pair is the tail of the thunk, not necessarily its first
// instruction: a /GS build prefixes a cookie check, and an /hotpatch or /Gy
// build prefixes padding.  Scanning for the pair, and requiring the `jmp` to
// sit immediately after the `mov`, keeps both variants readable without
// accepting an unrelated `mov eax, imm32` somewhere in a real function.
inline constexpr size_t kCOFFRegistrationEHHandlerMaxThunkBytes = 64;
// Longest wrapper MSVC emits is well inside this; scanning further would
// start reading whatever routine follows it.
inline constexpr size_t kCOFFRegistrationEHHandlerMaxWrapperBytes = 96;

// lib/loader/COFF/eh/COFFRegistrationEHScan.cpp
/// How far back from an install site the prologue may be searched for the
/// `push` operands.  A frame installs at most a try level, a table pointer,
/// and a handler.
inline constexpr unsigned kCOFFRegistrationEHScanMaxInstallPushes = 4;
/// How far back from a chain-head read the operands of the install may be
/// found.  The record is built immediately before the link, so a window this
/// size covers the widest prologue any of the three shapes below emits while
/// staying far short of the previous function.
inline constexpr size_t kCOFFRegistrationEHScanMaxInstallWindow = 64;

// lib/loader/COFF/eh/COFFRegistrationEHThrowInfo.cpp
// MSVC TypeDescriptor includes two pointer words followed by a bounded
// decorated name. Its cache word may be writable; it is not an object-size
// authority and must not be conflated with the immutable ThrowInfo graph.
inline constexpr uint32_t kCOFFRegistrationEHThrowInfoMaxTypeName = 4096;

// lib/loader/COFF/RTTI.cpp
/// Bound the complete class/struct exception graph, including every base.
inline constexpr uint32_t kMsvcThrowInfoMaxCatchableTypes = 64;

// lib/loader/COFF/eh/COFFRegistrationScalarLeaf.cpp
inline constexpr unsigned kCOFFRegistrationScalarLeafMaxHelperBytes = 256;
inline constexpr unsigned kCOFFRegistrationScalarLeafMaxProofStates = 512;

// lib/loader/DWARF/EHFrame.cpp
/// Largest augmentation string accepted.  The defined characters are a short
/// fixed set, so anything longer indicates the string is not NUL terminated
/// inside the entry.
inline constexpr size_t kEHFrameMaxAugmentationLength = 32;

// lib/loader/DWARF/EHFrameCFI.cpp
/// Largest call-frame instruction expression block accepted.  A DWARF
/// expression describing a CFA rule is a handful of bytes in practice; a
/// length beyond this is a corrupt or hostile record, not a real rule.
inline constexpr uint64_t kEHFrameCFIMaxExpressionBytes = 64 * 1024;

// lib/loader/DWARF/ItaniumEH.cpp
inline constexpr uint64_t kItaniumEHMaxIndex = 32 * 1024 * 1024;
inline constexpr uint64_t kItaniumEHMaxDecode = 64 * 1024 * 1024;

// lib/loader/DWARF/LSDA.cpp
/// Largest LSDA record read from the image in one go.  A record describes one
/// function; a megabyte is already far beyond any real translation unit and
/// bounds the work a crafted header can request.
inline constexpr size_t kLSDAMaxLSDABytes = 1u << 20;
/// Largest mangled RTTI name followed out of a `std::type_info`.
inline constexpr size_t kLSDAMaxTypeNameBytes = 4096;

// lib/loader/ELF/ARMEHABIDetail.h
/// Ceiling on index entries read from one image.  A `.ARM.exidx` covers every
/// function, and the largest shipped ARM32 objects are far below this.
inline constexpr size_t kARMEHABIDetailMaxIndexEntries = 1u << 22;

// lib/loader/ELF/ELFProgramLinking.cpp
inline constexpr uint64_t kELFProgramLinkingMaxRecords = 1 << 20;
inline constexpr uint64_t kELFProgramLinkingStringBudget = 16 * 1024 * 1024;

// lib/loader/Go/GoDeferInfo.cpp
/// Bound on the argument list a Go 1.14-era deferred call could carry.  The
/// count is only read to be skipped, so this exists to stop a misread record
/// from turning into a long walk.
inline constexpr uint32_t kGoDeferInfoMaxLegacyDeferArguments = 64;

// lib/loader/Go/GoRuntimeDetail.h
/// Bounds.  Each is far above anything a real Go link produces and far below
/// what would let a mis-identified table run the decoder out of time.
inline constexpr uint64_t kGoRuntimeDetailMaxFunctionCount = 1u << 22;
inline constexpr uint32_t kGoRuntimeDetailMaxPCDataTables = 64;
inline constexpr uint8_t kGoRuntimeDetailMaxFuncDataTables = 16;
inline constexpr size_t kGoRuntimeDetailMaxSymbolNameLength = 4096;
inline constexpr uint64_t kGoRuntimeDetailMaxFrameSize = 1u << 24;
inline constexpr uint64_t kGoRuntimeDetailMaxFrameSlotOffset = 1u << 20;
inline constexpr unsigned kGoRuntimeDetailMaxTextSections = 64;
/// How far past `moduledata.text` the funcdata base is searched for.  The
/// fields between them are a handful of segment bounds whose count has grown
/// by a few words per release; this covers every layout since Go 1.16 with
/// room to spare.
inline constexpr unsigned kGoRuntimeDetailMaxModuleDataSearchWords = 48;
/// Bound on the number of steps taken while walking one pc-value table.
inline constexpr unsigned kGoRuntimeDetailMaxPCValueSteps = 1u << 16;
/// Bound on how many ranges one pc-value table contributes to a record.  A
/// table can legitimately have more steps than this on a very large function,
/// in which case the record is truncated and marked rather than grown without
/// limit.
inline constexpr unsigned kGoRuntimeDetailMaxPCValueRanges = 1u << 12;

// lib/loader/MachO/CFunctionParameterCalls.cpp
inline constexpr size_t
    kCFunctionParameterCallsBuildCFunctionParameterCallHintsBudget = 262144;

// lib/loader/MachO/MachOCompactUnwind.cpp
/// The best-effort loader parser is used while opening arbitrary binaries and
/// deliberately keeps a fixed work budget.  The strict rewrite parser below
/// instead exposes caller-selected ceilings through its public options.
inline constexpr uint32_t kMachOCompactUnwindMaxEntries = 1u << 22;
inline constexpr uint32_t kMachOCompactUnwindMaxPages = 1u << 16;

// lib/loader/MachO/MachODyldBind.cpp
inline constexpr uint64_t kMachODyldBindMaxBindSlots = 1u << 22;

// lib/loader/MachO/MachODyldRebase.cpp
inline constexpr uint64_t kMachODyldRebaseMaxRebases = 1u << 22;

// lib/loader/MachO/MachOLocalFunction.cpp
inline constexpr size_t kMachOLocalFunctionIsMachOLocalFunctionRangeBudget =
    1024 * 1024;

// lib/loader/ObjC/ObjCBlockCallHints.cpp
inline constexpr size_t kObjCBlockCallHintsDiscardedResultAcrossCFGBudget =
    4096;

// lib/loader/ObjC/ObjCBlocks.cpp
inline constexpr uint64_t kObjCBlocksMaxLiteralSize = 1U << 20;

// lib/loader/ObjC/ObjCCallHints.cpp
inline constexpr int64_t kObjCCallHintsFrameOffsetLimit = 1048576;
inline constexpr size_t kObjCCallHintsScanBudget = 4096;
inline constexpr size_t kObjCCallHintsSourceCallCandidatesBudget = 262144;
inline constexpr size_t kObjCCallHintsObjcFrameCallContractsBudget = 262144;
inline constexpr size_t
    kObjCCallHintsObjcNonEscapingBlockSourceFrameEffectsBudget = 262144;
inline constexpr size_t kObjCCallHintsBuildObjCSourceCallHintsBudget = 262144;

// lib/loader/ObjC/ObjCEH.cpp
/// Upper bound on the bytes of one function body the runtime-call scan reads.
inline constexpr size_t kObjCEHMaxScannedFunctionBytes = 1u << 20;
/// Upper bound on a class name.  Objective-C class names are identifiers, and
/// a longer run than this is evidence the address was not one.
inline constexpr size_t kObjCEHMaxClassNameBytes = 512;

// lib/loader/ObjC/ObjCFormattedCalls.cpp
inline constexpr uint32_t kObjCFormattedCallsMaxUnits = 65536;

// lib/loader/ObjC/ObjCMethods.cpp
inline constexpr size_t kObjCMethodsMaxRecords = 65536;
inline constexpr size_t kObjCMethodsMaxString = 4096;

// lib/loader/ObjC/ObjCProtocols.cpp
inline constexpr size_t kObjCProtocolsMaxRecords = 65536;

// lib/loader/ObjC/ObjCStorage.cpp
inline constexpr uint64_t kObjCStorageMaxRecords = 65536;

// lib/loader/Rust/RustEH.cpp
/// Upper bound on the bytes of one function body the panic-site scan reads.
/// A Rust binary statically links its whole dependency graph, so the scan runs
/// over every function in the image and needs a bound that a pathological
/// record cannot escape.
inline constexpr size_t kRustEHMaxScannedFunctionBytes = 1u << 20;
// A type descriptor name is a short identifier; a longer read would only
// walk off into whatever follows the record.
inline constexpr size_t kRustEHMaxNameBytes = 256;

// lib/loader/Swift/SwiftBooleanProjection.h
inline constexpr size_t
    kSwiftBooleanProjectionQualifySwiftBooleanProjectionsBudget = 1U << 18;

// lib/loader/Swift/SwiftConsumedInputEffects.cpp
inline constexpr size_t
    kSwiftConsumedInputEffectsBuildSwiftConsumedInputCallHintsBudget = 262144;

// lib/loader/Swift/SwiftMangledClassMethodABI.h
inline constexpr size_t kSwiftIdentityInputBytes = 1024;
inline constexpr size_t kSwiftIdentityNodes = 128;
inline constexpr unsigned kSwiftIdentityDepth = 24;
inline constexpr size_t kSwiftIdentityMemoryBytes = 65536;
inline constexpr size_t kSwiftIdentityOperations = 10000;

// lib/loader/Swift/SwiftMangledValueConstructorABI.cpp
inline constexpr size_t
    kSwiftMangledValueConstructorABISwiftFixedRecordConstructorEntryABIBudget =
        262144;
inline constexpr size_t
    kSwiftMangledValueConstructorABIBuildSwiftFixedRecordConstructorCallHintsBudget =
        262144;

// lib/loader/Swift/SwiftMetadata.cpp
inline constexpr size_t kSwiftBindingInputBytes = 8000;
inline constexpr size_t kSwiftBindingNodes = 1024;
inline constexpr unsigned kSwiftBindingDepth = 64;
inline constexpr size_t kSwiftBindingMemoryBytes = 1024 * 1024;
inline constexpr size_t kSwiftBindingOperations = 100000;

// lib/loader/Swift/SwiftOpaqueValueEffects.cpp
inline constexpr size_t
    kSwiftOpaqueValueEffectsSwiftOpaqueValueCallEffectsBudget = 262144;
inline constexpr size_t
    kSwiftOpaqueValueEffectsBuildSwiftOpaqueValueCallHintsBudget = 262144;

// lib/loader/Swift/SwiftValueWitnessCalls.cpp
inline constexpr size_t kSwiftValueWitnessCallsRemainingBudget = 1U << 20;
inline constexpr int kSwiftValueWitnessCallsAtBudget = 4096;

// lib/loader/Swift/SwiftVirtualCalls.cpp
inline constexpr size_t kSwiftVirtualCallsBudget = 16384;
inline constexpr size_t kSwiftVirtualCallsNativeSelfCallsBudget = 1U << 18;

// lib/loader/discovery/ARM/FunctionDiscoveryARM.cpp
inline constexpr size_t kFunctionDiscoveryARMMaxDiscoveredInstructions =
    4'000'000;

// lib/loader/discovery/X86/FunctionDiscoveryX86.cpp
/// The most bytes of literals and padding the check reads, and the most
/// instructions it follows through them.
inline constexpr size_t kFunctionDiscoveryX86MaxLiteralRegion = 64 * 1024;
inline constexpr size_t kFunctionDiscoveryX86MaxLiteralInstructions = 4096;

// include/neverd/loader/ExceptionTable.h
inline constexpr uint64_t kExceptionTableFindFunctionWindowMinArgument2 =
    uint64_t{2} * 1024 * 1024;

// lib/loader/COFF/PEImports.cpp
inline constexpr int kPEImportsReaderMinArgument2 = 8 * 1024 * 1024;

// lib/loader/COFF/eh/COFFRegistrationEH.cpp
inline constexpr int kCOFFRegistrationEHDirectEH4PublicationSizeMinArgument2 =
    128;

// lib/loader/COFF/eh/COFFRegistrationEHFixedFrame.cpp
inline constexpr int
    kCOFFRegistrationEHFixedFrameProveFixedCxxRegistrationLayoutSizeMinArgument2 =
        256;

// lib/loader/COFF/eh/COFFRegistrationEHRealignment.cpp
inline constexpr int
    kCOFFRegistrationEHRealignmentProveRealignedCxxRegistrationLayoutSizeMinArgument2 =
        128;

// lib/loader/MachO/CFunctionParameterCalls.cpp
inline constexpr int
    kCFunctionParameterCallsCFunctionParameterSignatureTypePointeeParamTypesSizeLimit =
        64;
inline constexpr int
    kCFunctionParameterCallsBuildCFunctionParameterCallHintsEntryParametersSizeLimit =
        64;
inline constexpr int
    kCFunctionParameterCallsBuildCFunctionParameterCallHintsFunctionBlocksSizeLimit =
        4096;

// lib/loader/MachO/ImmutableNativeCalls.cpp
inline constexpr int kImmutableNativeCallsAddressDepthLimit = 48;
inline constexpr int kImmutableNativeCallsSlotDepthLimit = 48;
inline constexpr int
    kImmutableNativeCallsImmutableNativeCallTargetsFunctionBlocksSizeLimit =
        4096;
inline constexpr size_t
    kImmutableNativeCallsImmutableNativeCallTargetsRemaining = 262144;
inline constexpr int
    kImmutableNativeCallsIsImmutableNativeCallHintABIParametersSizeLimit = 64;
inline constexpr int
    kImmutableNativeCallsBuildImmutableNativeCallHintsFunctionBlocksSizeLimit =
        4096;
inline constexpr size_t
    kImmutableNativeCallsBuildImmutableNativeCallHintsRemaining = 262144;

// lib/loader/MachO/ImmutableNativeFrame.cpp
inline constexpr int
    kImmutableNativeFrameImmutableNativeInstructionOperationsMatchFunctionDecodedInstructionCountLimit =
        4096;
inline constexpr int
    kImmutableNativeFrameMachineMatchesFunctionDecodedInstructionCountLimit =
        4096;

// lib/loader/MachO/MachOLocalFunction.cpp
inline constexpr int
    kMachOLocalFunctionIsMachOLocalFunctionRangeImageSegmentsSizeLimit = 64;

// lib/loader/MachO/MachOProgramLinking.cpp
inline constexpr int kMachOProgramLinkingReaderMinArgument1 = 65536;
inline constexpr int kMachOProgramLinkingReaderExportsNodeNameSizeLimit = 4096;

// lib/loader/MachO/RuntimeFunctionAddress.cpp
inline constexpr int
    kRuntimeFunctionAddressRuntimeCFunctionAddressHintFunctionParametersSizeLimit =
        64;

// lib/loader/MachO/SourceLocalCall.cpp
inline constexpr int kSourceLocalCallSourceLeafCodeRangeSizeLimit = 68;
inline constexpr int kSourceLocalCallsCallerBlocksSizeLimit = 16384;
inline constexpr size_t kSourceLocalCallSourceLocalCallsRemaining = 262144;

// lib/loader/ObjC/ObjCBlockCallHints.cpp
inline constexpr int
    kObjCBlockCallHintsDiscardedResultAcrossCFGFunctionBlocksSizeLimit = 64;
inline constexpr int kObjCBlockCallHintsDiscardedResultAcrossCFGWorkSizeLimit =
    64;
inline constexpr int
    kObjCBlockCallHintsPreservedEntryContextRegistersFunctionBlocksSizeLimit =
        64;
inline constexpr int kObjCBlockCallHintsAnalyzeBlockOpsSizeLimit = 65536;
inline constexpr int
    kObjCBlockCallHintsBuildObjCBlockCallHintsFunctionBlocksSizeLimit = 64;
inline constexpr int kObjCBlockCallHintsBuildObjCBlockCallHintsPathSizeLimit =
    64;
inline constexpr int
    kObjCBlockCallHintsBuildObjCBlockCallHintsLinearOpsSizePartOpsSize1Limit =
        65536;
inline constexpr int kObjCBlockCallHintsBuildObjCBlockCallHintsPathsSizeLimit =
    64;
inline constexpr int
    kObjCBlockCallHintsBuildObjCBlockCallHintsReverseSizeLimit = 64;

// lib/loader/ObjC/ObjCBlocks.cpp
inline constexpr int kObjCBlocksParseObjCBlockSignatureSizeLimit = 4096;
inline constexpr int kObjCBlocksParseObjCBlockSignatureHintParametersSizeLimit =
    ::neverd::limits::ir::kMaxSourceABIParameters;
inline constexpr int
    kObjCBlocksObjcBlockObjectParameterClassSignatureSizeLimit = 4096;
inline constexpr int kObjCBlocksFindObjCGlobalBlocksCandidatesLimit = 65536;

// lib/loader/ObjC/ObjCCallHints.cpp
inline constexpr int kObjCCallHintsMergeNumberCandidatesSizeLimit = 64;
inline constexpr int kObjCCallHintsWriteFrameBytesVSizeLimit = 4096;
inline constexpr int kObjCCallHintsWriteFrameBytesSizeLimit = 4096;
inline constexpr int
    kObjCCallHintsObjcImmutableSelectorStubSourceCallHintHSelectorSizeLimit =
        1024;
inline constexpr int kObjCCallHintsSourceCallCandidatesCountLimit = 16384;
inline constexpr int
    kObjCCallHintsSourceCallCandidatesTransferStateFrameSlotsSizeLimit = 4096;
inline constexpr int
    kObjCCallHintsSourceCallCandidatesTransferStateTypedFrameSlotsSizeLimit =
        4096;
inline constexpr int
    kObjCCallHintsSourceCallCandidatesTransferStateFrameBytesSizeLimit = 4096;
inline constexpr int
    kObjCCallHintsSourceCallCandidatesTransferObjcMsgSendObjectsResultSizeLimit =
        ::neverd::limits::ir::kMaxObjCSentinelObjects;
inline constexpr int
    kObjCCallHintsSourceCallCandidatesTransferStateDeclaredObjectFrameSlotsSizeLimit =
        4096;
inline constexpr int
    kObjCCallHintsSourceCallCandidatesTransferStateFreshNilFrameSlotsSizeLimit =
        4096;
inline constexpr int kObjCCallHintsSourceCallCandidatesTransferValuesSizeLimit =
    4096;
inline constexpr size_t kObjCCallHintsSourceCallCandidatesRemaining = 1048576;
inline constexpr int
    kObjCCallHintsSourceCallCandidatesIncomingFrameBytesSizeLimit = 4096;
inline constexpr int
    kObjCCallHintsNonEscapingBlockFrameEffectsDLiteralSizeLimit = 4096;

// lib/loader/ObjC/ObjCConstantObjects.cpp
inline constexpr int kObjCConstantObjectsImportedBooleanDepthLimit = 64;
inline constexpr int
    kObjCConstantObjectsImportedBooleanObjectsSizeActiveSizeLimit = 4096;
inline constexpr int kObjCConstantObjectsObjectDepthLimit = 64;
inline constexpr int kObjCConstantObjectsObjectsSizeActiveSizeLimit = 4096;

// lib/loader/ObjC/ObjCConstantStrings.cpp
inline constexpr int kObjCConstantStringsReadObjCConstantStringLengthLimit =
    1024 * 1024;

// lib/loader/ObjC/ObjCEncoding.cpp
inline constexpr int kObjCEncodingSkipPointeeTypeDepthLimit = 16;
inline constexpr int kObjCEncodingParseSourceTypeDepthLimit = 16;
inline constexpr int kObjCEncodingParseSourceTypeFieldsSizeLimit = 64;
inline constexpr int kObjCEncodingParseObjCMethodEncodingSelectorSizeLimit =
    4096;
inline constexpr int
    kObjCEncodingParseObjCMethodEncodingHintParametersSizeLimit =
        ::neverd::limits::ir::kMaxSourceABIParameters;
inline constexpr int
    kObjCEncodingParseObjCFunctionEncodingHintParametersSizeLimit =
        ::neverd::limits::ir::kMaxSourceABIParameters;

// lib/loader/ObjC/ObjCFormattedCalls.cpp
inline constexpr int
    kObjCFormattedCallsBindFormatArgumentsResultSignatureParametersSizeArgumentsSizeLimit =
        64;
inline constexpr int kObjCFormattedCallsObjcFormatArgumentTypesFormatSizeLimit =
    65536;
inline constexpr unsigned
    kObjCFormattedCallsObjcFormatArgumentTypesPositionNMinArgument1 = 65U;
inline constexpr int
    kObjCFormattedCallsObjcFormattedSourceCallHintFormatAddressesSizeLimit = 64;
inline constexpr int
    kObjCFormattedCallsObjcDynamicFormatPointerArgumentsSourceCallHintDeclarationSignatureParametersSizePointerArgumentsLimit =
        64;
inline constexpr int
    kObjCFormattedCallsObjcDynamicFormatInteger64ArgumentsSourceCallHintIntegerArgumentsSizeLimit =
        64;
inline constexpr int
    kObjCFormattedCallsObjcDynamicFormatInteger64ArgumentsSourceCallHintCallSignatureParametersSizeIntegerArgumentsSizeLimit =
        64;

// lib/loader/ObjC/ObjCMethodLists.cpp
inline constexpr int kObjCMethodListsReadMethodListEntrySizeLimit = 4096;

// lib/loader/ObjC/ObjCMethods.cpp
inline constexpr int kObjCMethodsDiagnosticImgObjCMetadataDiagnosticsSizeLimit =
    64;

// lib/loader/ObjC/ObjCProperties.cpp
inline constexpr int kObjCPropertiesDiagnosticMessagesSizeLimit = 64;

// lib/loader/ObjC/ObjCProtocols.cpp
inline constexpr int kObjCProtocolsDiagnosticMessagesSizeLimit = 64;
inline constexpr int kObjCProtocolsReadSizeLimit = 4096;

// lib/loader/ObjC/ObjCSentinelCalls.cpp
inline constexpr int
    kObjCSentinelCallsObjcSentinelSourceCallHintObjectsSizeLimit =
        ::neverd::limits::ir::kMaxObjCSentinelObjects;

// lib/loader/ObjC/ObjCSourceDeclarations.cpp
inline constexpr int
    kObjCSourceDeclarationsHasEmbeddedDDLogFileInfoElementSortedLogFileInfosDDLogFileManagerDefaultReceiverStepsSizeLimit =
        2;
inline constexpr int
    kObjCSourceDeclarationsValidReceiverRootAlternativeStepsSizeReceiverStepsSizeLimit =
        8;
inline constexpr size_t kObjCSourceDeclarationsReceiverIvarRemaining = 4096;
inline constexpr int kObjCSourceDeclarationsReceiverIvarVisitedSizeLimit = 64;
inline constexpr int kObjCSourceDeclarationsReceiverTypeReceiverStepsSizeLimit =
    8;
inline constexpr int kObjCSourceDeclarationsFieldReceiverStepsSizeLimit = 8;
inline constexpr int
    kObjCSourceDeclarationsReceiverDeclarationVisitVisitedSizeActiveSizeLimit =
        256;
inline constexpr int
    kObjCSourceDeclarationsReceiverDeclarationClassesSizeLimit = 256;
inline constexpr int
    kObjCSourceDeclarationsObjcReceiverCallResultTypeHintReceiverStepsSizeLimit =
        8;
inline constexpr int
    kObjCSourceDeclarationsObjcBlockParameterContractDerivesFromVisitedSizeLimit =
        256;

// lib/loader/ObjC/ObjCStorage.cpp
inline constexpr int kObjCStorageDiagnosticsSizeLimit = 64;
inline constexpr int kObjCStorageReadIvarsEntrySizeLimit = 4096;

// lib/loader/ReadOnlyBytes.cpp
inline constexpr int kMaxImageReadBytes = 1024 * 1024;

// lib/loader/Swift/SwiftABI.cpp
inline constexpr int kSwiftABIInspectDepthLimit = 8;
inline constexpr int kSwiftABIInspectFunctionBlocksFrontOpsSizeLimit = 4096;

// lib/loader/Swift/SwiftBooleanProjection.h
inline constexpr int
    kSwiftBooleanProjectionQualifySwiftBooleanProjectionsLowBlocksSizeLimit =
        256;

// lib/loader/Swift/SwiftMangledValueConstructorABI.cpp
inline constexpr int kSwiftMangledValueConstructorABIIdentifierNTextSizeLimit =
    128;
inline constexpr int
    kSwiftMangledValueConstructorABISwiftFixedRecordConstructorEntryABIFunctionDecodedInstructionCountLimit =
        256;
inline constexpr int
    kSwiftMangledValueConstructorABIBuildSwiftFixedRecordConstructorCallHintsCallerDecodedInstructionCountLimit =
        1024;
inline constexpr int
    kSwiftMangledValueConstructorABIBuildSwiftFixedRecordConstructorCallHintsSizeLimit =
        32;

// lib/loader/Swift/SwiftMetadata.cpp
inline constexpr int kSwiftMetadataReflectionStringTextSizeLimit = 1024;
inline constexpr int
    kSwiftMetadataSwiftLocalRegisteredTypeIdentityIdentifierNodeTextSizeLimit =
        128;
inline constexpr int
    kSwiftMetadataSwiftLocalRegisteredTypeIdentityRecordsSize4Limit = 65536;
inline constexpr int kSwiftMetadataSwiftFixedRecordStorageFieldNameSizeLimit =
    128;
inline constexpr int
    kSwiftMetadataSwiftFixedRootClassStorageClassInstanceSizeLimit = 4096;
inline constexpr int kSwiftMetadataSwiftFixedRootClassStorageCountLimit = 32;
inline constexpr int kSwiftMetadataTypeCountLimit = 4096;
inline constexpr int kSwiftMetadataReadSectionSize4Limit = 65536;

// lib/loader/Swift/SwiftRuntimeSource.cpp
inline constexpr int kSwiftRuntimeSourceIdentifierNameSizeLimit = 255;
inline constexpr int kSwiftRuntimeSourceRunFBlocksFrontOpsSizeLimit = 4096;

// lib/loader/Swift/SwiftStringCalls.cpp
inline constexpr int kSwiftStringCallsSwiftLiteralStringCountLimit =
    1024 * 1024;
inline constexpr int kSwiftStringCallsSwiftStaticStringLiteralByteCountLimit =
    1024 * 1024;

// lib/loader/Swift/SwiftValueWitnessCalls.cpp
inline constexpr int kSwiftValueWitnessCallsAddStepsSizeLimit = 32;
inline constexpr int kSwiftValueWitnessCallsLoadStepsSizeLimit = 32;
inline constexpr int kSwiftValueWitnessCallsTracerBlockPredsSizeLimit = 4096;
inline constexpr int kSwiftValueWitnessCallsTracerBlockSuccsSizeLimit = 4096;
inline constexpr int
    kSwiftValueWitnessCallsBuildSwiftValueWitnessCallHintsFunctionBlocksSizeLimit =
        16384;

// lib/loader/Swift/SwiftVirtualCalls.cpp
inline constexpr int kSwiftVirtualCallsResolveAtDepthLimit = 64;
inline constexpr int kSwiftVirtualCallsRegisterTraceFunctionBlocksSizeLimit =
    256;
inline constexpr int kSwiftVirtualCallsRegisterTraceBPredsSizeLimit = 256;
inline constexpr int kSwiftVirtualCallsRegisterTraceBSuccsSizeLimit = 256;
inline constexpr int kSwiftVirtualCallsNativeSelfCallsFunctionBlocksSizeLimit =
    256;
inline constexpr int
    kSwiftVirtualCallsNativeSelfCallsFunctionDecodedInstructionCountLimit =
        4096;
inline constexpr int kSwiftVirtualCallsNativeSelfCallsCandidatesLimit = 64;
inline constexpr int
    kSwiftVirtualCallsBuildSwiftVirtualCallHintsFunctionBlocks0OpsSizeLimit =
        2048;

// lib/loader/Swift/SwiftVirtualSlot.h
inline constexpr int kSwiftVirtualSlotIsVoidClassVirtualSlotCountLimit = 512;

// include/neverd/loader/ExceptionTable.h
inline constexpr int kExceptionTableFindFunctionNestLimit = 16;

// lib/loader/COFF/eh/COFFRegistrationEHPersonality.cpp
inline constexpr int
    kCOFFRegistrationEHPersonalityGetCheckedX86CxxPersonalityABIPaddingLimit =
        8;

// lib/loader/MachO/ImmutableNativeCalls.cpp
inline constexpr int kImmutableNativeCallsImmutableNativeCallTargetsRoundLimit =
    64;

// lib/loader/MachO/MachOChainedFixups.cpp
inline constexpr unsigned
    kMachOChainedFixupsParseChainedFixupsRebasesGuardLimit = (1u << 22);

// lib/loader/MachO/SourceRegisterCopy.cpp
inline constexpr int kSourceRegisterCopyLeafILimit = 16;

// lib/loader/ObjC/ObjCBlocks.cpp
inline constexpr int kObjCBlocksCaptureLayoutIndexLimit = 4096;

// lib/loader/ObjC/ObjCCallHints.cpp
inline constexpr int kObjCCallHintsAddNonEscapingBlockFrameEffectsRoundLimit =
    8;

// lib/loader/ObjC/ObjCRuntimeData.h
inline constexpr int kObjCRuntimeDataStringIndexLimit = 4096;

// lib/loader/COFF/eh/COFFRegistrationEHPersonality.cpp
inline constexpr int kMaxRegistrationPersonalityHops = 4;

// lib/loader/COFF/eh/COFFRegistrationEH4Wrapper.cpp
inline constexpr int kMaxRegistrationEH4WrapperHops = 4;

// lib/loader/COFF/eh/COFFRegistrationEHHandler.cpp
inline constexpr int kMaxRegistrationHandlerHops = 4;

// lib/loader/COFF/eh/COFFRegistrationEHDetail.h
inline constexpr int kMaxRegistrationNullArgumentDepth = 8;

// lib/loader/ObjC/ObjCCallHints.cpp
inline constexpr int kMaxObjCByValueFrameCalls = 8;

// lib/loader/Swift/SwiftABI.cpp
inline constexpr int kMaxSwiftSelfFlowOperations = 16384;

// lib/loader/Swift/SwiftValueWitnessCalls.cpp
inline constexpr int kMaxSwiftWitnessTraceEdges = 131072;

// lib/loader/Swift/SwiftBooleanProjection.h
inline constexpr int kMaxSwiftBooleanBlockBytes = 32768;

// lib/loader/Swift/SwiftMetadata.cpp
inline constexpr int kMaxSwiftMetadataVectorWords = 1048576;

// lib/loader/MachO/MachOLocalFunction.cpp
inline constexpr int kMaxMachOLocalFunctionFileBytes = 64 * 1024 * 1024;

// lib/loader/Swift/SwiftVirtualCalls.cpp
inline constexpr int kMaxSwiftVirtualTraceOperations = 32768;

// lib/loader/MachO/MachOProgramLinking.cpp
inline constexpr int kMaxMachOExportNameBytes = 4096;

// lib/loader/Swift/SwiftVirtualCalls.cpp
inline constexpr int kMaxSwiftVirtualSlotOffset = 4096;

// lib/loader/ObjC/ObjCEncoding.cpp
inline constexpr int kMaxObjCTypeEncodingBytes = 4096;

// lib/loader/Swift/SwiftMetadata.cpp
inline constexpr int kMaxSwiftProtocolRequirements = 512;

// lib/loader/Swift/SwiftVirtualSlot.h
inline constexpr int kMaxSwiftClassMetadataWords = 512;

// lib/loader/Swift/SwiftMetadata.cpp
inline constexpr int kMaxSwiftClassStoredProperties = 64;

} // namespace neverd::limits::loader

#endif // NEVERD_LOADER_LIMITS_H
