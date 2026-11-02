//===- ResourceLimits.h - Resource limits and defaults -----*- C++ -*-===//
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

#ifndef NEVERD_TOOLS_NEVERD_WORKER_RESOURCELIMITS_H
#define NEVERD_TOOLS_NEVERD_WORKER_RESOURCELIMITS_H

#include "../common/transport/ResourceLimits.h"

#include "neverd/Limits.h"
#include "neverd/patch/Limits.h"
#include "neverd/sdk/Limits.h"
#include "neverd/support/Limits.h"

#include <cstddef>
#include <cstdint>

namespace neverd::limits::worker {
/// Intermediate updates to one load step; boundaries are always sent.
inline constexpr int kLoadProgressIntervalMs = 50;
inline constexpr std::size_t kPatchHexBytes =
    ::neverd::limits::patch::kMaxApplyBytes * 2;
inline constexpr std::size_t kPatchStateBytes =
    ::neverd::limits::transport::kJsonMaxJsonBytes / 2;
inline constexpr std::size_t kPatchTextBytes =
    ::neverd::limits::patch::kMaxAssemblyTextBytes;
/// Smaller inventories cost less to encode with their envelope in one pass.
/// Paired Clang/GCC requests cross over to streamed range rows at 32 entries.
inline constexpr std::size_t kPatchMinEncodedRangeRows = 32;

// tools/neverd-worker/Engine.cpp
// The most source one declaration reads.
inline constexpr std::size_t kEngineMaxDeclarationBytes = 4u << 20;
// The most other instructions one structure offset goes to, which with the
// instruction's own row fill a history command.
inline constexpr std::size_t kEngineMaxOperandFormatRows = 63;
inline constexpr std::size_t kEngineMaxFunctionRows = 1000000;
// Strings need this many display columns unless string_options says
// otherwise; a minimum is at most MaxStringMinLength
// (neverd::strings::MaxMinLength).
inline constexpr int kEngineDefaultStringMinLength =
    ::neverd::limits::support::kDefaultStringMinLength;
inline constexpr std::int64_t kEngineMaxStringMinLength =
    ::neverd::strings::MaxMinLength;
/// Lines of one engine code page.
inline constexpr std::size_t kEngineEnginePageLines =
    ::neverd::sdk::kMaxCodeViewPageLines;
/// Text a renamed whole-function view may hold: the engine's source budget.
inline constexpr std::size_t kEngineMaxNamedViewBytes = 32 * 1024 * 1024;
/// The text of one code page, as the engine bounds its own pages: a page
/// ends before the line that would pass it.
inline constexpr std::size_t kEngineMaxCodePageBytes =
    ::neverd::limits::kMaxViewPageBytes;
inline constexpr std::size_t kEngineMaxNamedViewCacheBytes = 32 * 1024 * 1024;
inline constexpr std::size_t kEngineMaxNamedViews = 8;
inline constexpr std::size_t kEngineMaxEncodedPages = 64;
inline constexpr std::size_t kEngineMaxEncodedBytes = 8 * 1024 * 1024;
// Decode one basic block without following its successors or inventing
// a function. Known instructions are a boundary, and unmodelled control
// flow never invites guessing what bytes follow it.
inline constexpr std::size_t kEngineMaxInstructions = 256;

// tools/neverd-worker/Engine.h
inline constexpr std::size_t kEngineMaxGraphs = 64;

// tools/neverd-worker/GraphLayout.cpp
inline constexpr std::size_t kGraphLayoutMaxDummies = 200000;
inline constexpr int kGraphLayoutPlacementRounds = 8;

// tools/neverd-worker/GraphSnapshot.cpp
inline constexpr std::size_t kGraphSnapshotMaxNodes = 20000;
inline constexpr std::size_t kGraphSnapshotMaxEdges = 100000;
inline constexpr std::size_t kGraphSnapshotNodePage = 256;
inline constexpr std::size_t kGraphSnapshotEdgePage = 512;
inline constexpr double kGraphSnapshotMaxNodeExtent = 1e6;

// tools/neverd-worker/Listing.cpp
inline constexpr std::size_t kListingMaxXrefLines = 2;
/// The strings one neverd_strings_page_json call reads.
inline constexpr int kListingStringPageRows =
    ::neverd::sdk::kMaxStringRowsPerPage;
inline constexpr std::size_t kListingMaxPageLines =
    ::neverd::limits::transport::kListingMaxPageLines;
inline constexpr std::size_t kListingMaxOpcodeBytes = 12;
inline constexpr std::size_t kListingMaxDecodeChunk = 2048;
inline constexpr std::size_t kListingFirstUnsizedChunk = 64;
inline constexpr std::size_t kListingDecodeCacheFunctions = 512;
/// Disposable, per-project browsing cache. Writes happen in idle time.
inline constexpr std::size_t kBrowseCacheMaxEntryBytes = 32 * 1024 * 1024;
inline constexpr std::size_t kBrowseCacheMaxBytes = 128 * 1024 * 1024;
inline constexpr std::size_t kBrowseCacheMaxPendingEntries = 16;
/// A temporary browse cache entry at least this old was left by a writer that
/// stopped before publishing it. One atomic entry write ends well before this.
inline constexpr std::int64_t kBrowseCacheStaleTemporarySeconds = 10 * 60;
/// Nearby entries considered for an otherwise unowned navigation address.
/// Only a recovered instruction, never proximity, establishes ownership.
inline constexpr std::size_t kListingOwnerSearchFunctions = 16;
inline constexpr std::uint64_t kListingOwnerSearchDistance = 0x10000;
inline constexpr std::size_t kListingMaxStringDisplay = 400;
inline constexpr std::size_t kListingMaxUnsizedInstructions =
    ::neverd::sdk::kMaxUnsizedInstructions;
inline constexpr std::uint64_t kListingMaxAlignment = 0x1000;
// Classic listings name padding by the alignment of its end, up to this or the
// smallest power of two longer than the padding.
inline constexpr std::uint64_t kListingClassicAlignmentLimit = 0x20;
/// Bound one repeat-run scan and keep its address-relative page identity
/// stable.
inline constexpr std::uint64_t kListingMaxRepeatBytes = 0x1000;
inline constexpr std::uint64_t kListingMinRepeatBytes = 4;
inline constexpr std::size_t kListingMaxOverviewBuckets = 16384;
inline constexpr int kListingOverviewSamples = 4;
inline constexpr int kListingStringScanMinimum =
    ::neverd::limits::support::kDefaultStringMinLength;
/// Functions one neverd_string_refs_json call decodes.
inline constexpr int kListingStringReferencePageFunctions = 1024;
inline constexpr std::size_t kListingMaxReferencePage = 512;
inline constexpr int kListingPointerPage = ::neverd::sdk::kMaxDataSlotsPerQuery;
/// Functions one neverd_switches_json call covers.
inline constexpr int kListingSwitchPageFunctions =
    ::neverd::sdk::kMaxSwitchFunctionsPerPage;
inline constexpr std::uint64_t kListingMaxDecoded = 65536;

// tools/neverd-worker/ProjectHistory.cpp
// The history and the recovery journal are the user's own records and stay
// compact; the edit tables are the project's shared state, indented for
// version control (sidecarText), and each file has its own bound.
inline constexpr std::size_t kProjectHistoryMaxHistoryBytes = 8 * 1024 * 1024;
inline constexpr std::size_t kProjectHistoryMaxJournalBytes = 24 * 1024 * 1024;
inline constexpr std::size_t kProjectHistoryMaxStateFileBytes =
    16 * 1024 * 1024;
/// The most rows besides its own one history command may change.
inline constexpr std::size_t kProjectHistoryMaxCommandRows = 64;

// tools/neverd-worker/Protocol.h
inline constexpr std::size_t kProtocolMaxBackendBytes = 32 * 1024 * 1024;
inline constexpr std::size_t kProtocolMaxQueueBytes = 16 * 1024 * 1024;
inline constexpr std::size_t kProtocolMaxQueueRequests = 32;

// tools/neverd-worker/StringReferences.cpp
inline constexpr std::size_t kStringReferencesMaxStringReferencePage = 512;

// tools/neverd-worker/CodeEdits.cpp
inline constexpr int kCodeEditsIdentifierNameSizeLimit = 256;
inline constexpr int
    kCodeEditsValidateRowTypeImageHashInputSha256StringFieldMax = 64;
inline constexpr int kCodeEditsValidateRowTypeStringFieldMax = 4096;
inline constexpr int kCodeEditsValidateRowFloatingBytesSizeFieldMax = 16;
inline constexpr int kCodeEditsValidateRowFrameDumpSizeLimit = 128 * 1024;
inline constexpr int kCodeEditsValidateRowSnapshotStringFieldMax = 128;
inline constexpr int kCodeEditsValidateRowFrameValuePointsJsonArraySizeLimit =
    256;
inline constexpr int
    kCodeEditsValidateRowFrameValueVariablesJsonArraySizeLimit = 256;
inline constexpr int kCodeEditsValidateRowCallsSizeLimit = 32;
inline constexpr int kCodeEditsValidateRowCallsDumpSizeLimit = 64 * 1024;
inline constexpr int kCodeEditsValidateRowIdSizeLimit = 256;
inline constexpr int kCodeEditsValidateRowDeclarationSnapshotStringFieldMax =
    128;
inline constexpr int
    kCodeEditsValidateRowDeclarationSnapshotDeclarationStringFieldMax = 4096;
inline constexpr int kCodeEditsValidateRowActualsSizeLimit = 32;
inline constexpr int kCodeEditsValidateRowActualGetRefConstStdStringSizeLimit =
    4096;
inline constexpr int kCodeEditsValidateRowRowDumpSizeLimit = 4 * 1024 * 1024;
inline constexpr int kCodeEditsValidateRowShapeStringFieldMax = 32;
inline constexpr int kCodeEditsValidateRowNamesSizeLimit = 2048;
inline constexpr int kCodeEditsValidateRowCommentsSizeLimit = 2048;
inline constexpr int kCodeEditsValidateRowAnchorStringFieldMax = 65536;
inline constexpr int kCodeEditsValidateRowTextStringFieldMax = 65536;
inline constexpr int kCodeEditsValidateRowVariableEditsSizeLimit = 64;
inline constexpr int kCodeEditsValidateRowKindKindStringFieldMax = 16;
inline constexpr int kCodeEditsValidateRowToMapSnapshotFromFromStringFieldMax =
    256;
inline constexpr int
    kCodeEditsValidateRowToMapSnapshotFromSnapshotStringFieldMax = 128;
inline constexpr int kCodeEditsValidateRowToMapSnapshotFromToStringFieldMax =
    256;
inline constexpr int kCodeEditsValidateRowVariableTypesSizeLimit = 256;
inline constexpr int kCodeEditsValidateRowBytesSizeFieldMax = 16;
inline constexpr int kCodeEditsValidateRowNumbersSizeLimit = 2048;
inline constexpr int kCodeEditsValidateRowRadixesSizeLimit = 256;
inline constexpr int kCodeEditsChangeRepresentationStringFieldMax = 16;
inline constexpr int kCodeEditsChangeKindKindStringFieldMax = 16;
inline constexpr int
    kCodeEditsChangeStackFrameGlobalTypeRowObjectTypeInputSha256StringFieldMax =
        64;
inline constexpr int
    kCodeEditsChangeStackFrameGlobalTypeRowObjectTypeTypeStringFieldMax = 4096;
inline constexpr int
    kCodeEditsChangeStackFrameGlobalTypeRowObjectTypeBytesSizeFieldMax = 16;
inline constexpr int
    kCodeEditsChangeStackFrameGlobalTypeNameIdentityStringFieldMax = 32;
inline constexpr int
    kCodeEditsChangeStackFrameGlobalTypeNameOriginalStringFieldMax = 256;
inline constexpr int
    kCodeEditsChangeStackFrameGlobalTypeNameNameStringFieldMax = 256;
inline constexpr int
    kCodeEditsChangeStackFrameGlobalTypeNameCommentLineLineSizeFieldMax =
        1000000;
inline constexpr int
    kCodeEditsChangeStackFrameGlobalTypeNameCommentTextTextStringFieldMax =
        65536;
inline constexpr int
    kCodeEditsChangeStackFrameGlobalTypeNameCommentViewCommentsKeyAnchorStringFieldMax =
        65536;
inline constexpr int
    kCodeEditsChangeStackFrameGlobalTypeNameCommentNumberKeyLineSizeFieldMax =
        1000000;
inline constexpr int
    kCodeEditsChangeStackFrameGlobalTypeNameCommentNumberIndexSizeFieldMax =
        4095;
inline constexpr int
    kCodeEditsChangeStackFrameGlobalTypeNameCommentNumberAnchorStringFieldMax =
        65536;
inline constexpr int
    kCodeEditsChangeStackFrameGlobalTypeNameCommentNumberRadixStringFieldMax =
        16;
inline constexpr int
    kCodeEditsChangeStackFrameGlobalTypeNameCommentNumberCallTypeVariablesTypeIdVariableIdStringFieldMax =
        256;
inline constexpr int
    kCodeEditsChangeStackFrameGlobalTypeNameCommentNumberCallTypeVariablesTypeViewVariableTypesIdInputSha256StringFieldMax =
        64;
inline constexpr int
    kCodeEditsChangeStackFrameGlobalTypeNameCommentNumberCallTypeVariablesTypeViewVariableTypesIdSnapshotStringFieldMax =
        128;
inline constexpr int
    kCodeEditsChangeStackFrameGlobalTypeNameCommentNumberCallTypeVariablesTypeViewVariableTypesIdTypeStringFieldMax =
        4096;
inline constexpr int
    kCodeEditsChangeStackFrameGlobalTypeNameCommentNumberCallTypeVariablesTypeViewVariableTypesIdBytesSizeFieldMax =
        16;
inline constexpr int
    kCodeEditsValidateChangeCallTypeKindCallSnapshotSnapshotStringFieldMax =
        128;
inline constexpr int
    kCodeEditsValidateChangeCallTypeKindIdCallIdStringFieldMax = 256;
inline constexpr int
    kCodeEditsValidateChangeTypeKindIdVariableIdStringFieldMax = 256;
inline constexpr int
    kCodeEditsValidateChangeNameCommentKindLineLineSizeFieldMax = 1000000;
inline constexpr int
    kCodeEditsValidateChangeNameCommentKindAnchorStringFieldMax = 65536;
inline constexpr int
    kCodeEditsValidateChangeNameCommentNumberKindLineLineSizeFieldMax = 1000000;
inline constexpr int
    kCodeEditsValidateChangeNameCommentNumberKindIndexSizeFieldMax = 4095;
inline constexpr int
    kCodeEditsValidateChangeNameCommentNumberKindAnchorStringFieldMax = 65536;
inline constexpr int
    kCodeEditsValidateChangeNameCommentNumberKindRadixStringFieldMax = 16;
inline constexpr int
    kCodeEditsValidateChangeNameCommentNumberKindSyntaxRepresentationStringFieldMax =
        16;

// tools/neverd-worker/Contributions.cpp
inline constexpr int kContributionsIdentifierTextSizeLimit = 64;
inline constexpr int kContributionsValidateQueryOperationStringFieldMax = 32;
inline constexpr int
    kContributionsValidateQueryXrefsDecompileDisasmAddressStringFieldMax = 32;
inline constexpr int kContributionsValidateQueryDecompileLimitSizeFieldDefault =
    128;
inline constexpr int kContributionsValidateQueryOffsetSizeFieldMax = 1000000000;
inline constexpr int kContributionsValidateQueryFilterStringFieldMax = 256;
inline constexpr int
    kContributionsValidateQueryRepRepresentationStringFieldMax = 16;
inline constexpr int kContributionsValidateQueryDirectionStringFieldMax = 8;
inline constexpr int kContributionsRegisterFileBytesLimit = 65536;
inline constexpr int
    kContributionsRegisterFileNameSpaceNamespaceStringFieldMax = 64;
inline constexpr int kContributionsRegisterFileVersionStringFieldMax = 32;
inline constexpr int kContributionsRegisterFileManifestContributionsSizeLimit =
    32;
inline constexpr int kContributionsRegisterFileIdIdStringFieldMax = 129;
inline constexpr int kContributionsRegisterFileKindKindStringFieldMax = 16;
inline constexpr int kContributionsRegisterFileTitleStringFieldMax = 256;
inline constexpr int kContributionsRegisterFileTitleKeyKeyStringFieldMax = 64;
inline constexpr int kContributionsRegisterFileTitleKeyTitleStringFieldMax =
    128;
inline constexpr int kContributionsRegisterFileCountLimit = 128;

// tools/neverd-worker/Engine.cpp
inline constexpr int kEngineNamedViewJsonBytesDepthLimit = 64;
inline constexpr int kEnginePageLimitSizeFieldDefault = 128;
inline constexpr int kEnginePageLimitSizeFieldMax = 512;
inline constexpr int kEngineSortItemsFieldSortStringFieldMax = 64;
inline constexpr int kEngineAtomicWriteDeadlineSeconds = 1;
inline constexpr int kEngineAtomicWriteMilliseconds = 10;
inline constexpr int kEngineNamedViewOptionsDumpSizeLimit = 4096;
inline constexpr int kEngineNamedViewNameSizeLimit = 4096;
inline constexpr int kEngineReadCodeEditsFsFileSizePathLimit = 16 * 1024 * 1024;
inline constexpr int kEngineReadCodeEditsRowsSizeLimit = 16384;
inline constexpr int kEngineExecuteImplIdentifyPathPathStringFieldMax = 32768;
inline constexpr int kEngineExecuteImplPdbReferencePathPathStringFieldMax =
    32768;
inline constexpr int kEngineExecuteImplDwarfReferencePathPathStringFieldMax =
    32768;
inline constexpr int kEngineExecuteImplMapReferencePathPathStringFieldMax =
    32768;
inline constexpr int
    kEngineExecuteImplSymbolTableReferencePathPathStringFieldMax = 32768;
inline constexpr int kEngineExecuteImplStringOptionsNamesSizeLimit = 64;
inline constexpr int kEngineExecuteImplContributionRegisterPathStringFieldMax =
    32768;
inline constexpr int
    kEngineExecuteImplContributionUnregisterNamespaceStringFieldMax = 64;
inline constexpr int kEngineExecuteImplContributionExecuteIdIdStringFieldMax =
    129;
inline constexpr int
    kEngineExecuteImplContributionExecuteAddressStringFieldMax = 18;
inline constexpr int kEngineExecuteImplOpenPathTextPathStringFieldMax = 32768;
inline constexpr int kEngineExecuteImplOpenLoaderStringFieldMax = 16;
inline constexpr int kEngineExecuteImplOpenOptionsProcessorStringFieldMax = 32;
inline constexpr int kEngineExecuteImplOpenOptionsPlatformStringFieldMax = 16;
inline constexpr int kEngineExecuteImplOpenOptionsFieldStringFieldMax = 32;
inline constexpr int kEngineExecuteImplAnalysisSectionAddPathStringFieldMax =
    32768;
inline constexpr int kEngineExecuteImplPdbLoadPdbInspectPathPathStringFieldMax =
    32768;
inline constexpr int kEngineExecuteImplPdbLoadPdbInspectModeModeStringFieldMax =
    16;
inline constexpr int kEngineExecuteImplMapLoadMapInspectPathPathStringFieldMax =
    32768;
inline constexpr int kEngineExecuteImplMapLoadMapInspectModeModeStringFieldMax =
    16;
inline constexpr int kEngineExecuteImplDwarfLoadPathPathStringFieldMax = 32768;
inline constexpr int kEngineExecuteImplSignaturesLoadPathPathStringFieldMax =
    32768;
inline constexpr int kEngineExecuteImplSignaturesLoadModeModeStringFieldMax =
    16;
inline constexpr int kEngineExecuteImplHistoryLimitSizeFieldDefault = 128;
inline constexpr int kEngineExecuteImplHistoryLimitSizeFieldMax = 512;
inline constexpr int kEngineExecuteImplFunctionsLimitSizeFieldDefault = 128;
inline constexpr int kEngineExecuteImplFunctionsLimitSizeFieldMax = 512;
inline constexpr int kEngineExecuteImplFunctionsSortSortStringFieldMax = 64;
inline constexpr int
    kEngineExecuteImplLocalTypesForgetLocalTypesDeclareOptionsLanguageStringFieldMax =
        16;
inline constexpr int
    kEngineExecuteImplLocalTypesForgetLocalTypesDeclareNamesSizeLimit = 4096;
inline constexpr int
    kEngineExecuteImplLocalTypesFitRequestKeyKeyStringFieldMax = 256;
inline constexpr int
    kEngineExecuteImplLocalTypesFitRequestLimitSizeFieldDefault = 50;
inline constexpr int kEngineExecuteImplLocalTypesFitRequestLimitSizeFieldMax =
    500;
inline constexpr int kEngineExecuteImplLocalTypesFitAccessesSizeLimit = 4096;
inline constexpr int
    kEngineExecuteImplLocalTypesFitRequestFunctionStringFieldMax = 32;
inline constexpr int kEngineExecuteImplLocalTypesFitOperandSizeFieldMax = 7;
inline constexpr int
    kEngineExecuteImplTypeLibrariesSetTypeLibrariesPAtKeySizeLimit = 64;
inline constexpr int
    kEngineExecuteImplTypeLibrariesSetTypeLibrariesNameStringFieldMax = 256;
inline constexpr int
    kEngineExecuteImplLocalTypesSourceLocalTypesOptionsKeyKeyStringFieldMax =
        4096;
inline constexpr int
    kEngineExecuteImplLocalTypesSourceLocalTypesLocalTypeNameNameStringFieldMax =
        4096;
inline constexpr int kEngineExecuteImplSearchKindKindStringFieldMax = 16;
inline constexpr int kEngineExecuteImplSearchPatternStringFieldMax = 4096;
inline constexpr int kEngineExecuteImplSearchLimitSizeFieldDefault = 256;
inline constexpr int kEngineExecuteImplSearchLimitSizeFieldMax = 4096;
inline constexpr int
    kEngineExecuteImplRegisterFlowOptionsKindKindStringFieldMax = 32;
inline constexpr int
    kEngineExecuteImplRegisterFlowOptionsRegisterStringFieldMax = 32;
inline constexpr int kEngineExecuteImplSourceTypeIdVariableIdStringFieldMax =
    256;
inline constexpr int
    kEngineExecuteImplSourceTypeTypeProposalTypeStringFieldMax = 4096;
inline constexpr int
    kEngineExecuteImplCodeEditCallTypeKindIdCallIdStringFieldMax = 256;
inline constexpr int kEngineExecuteImplCodeEditCallTypeKindActualsSizeLimit =
    32;
inline constexpr int
    kEngineExecuteImplCodeEditCallTypeKindTypeGetRefConstStdStringSizeLimit =
        4096;
inline constexpr int
    kEngineExecuteImplCodeEditCallTypeKindCallsIdDeclarationStringFieldMax =
        4096;
inline constexpr int kEngineExecuteImplCodeEditCallTypeKindCallsSizeLimit = 32;
inline constexpr int kEngineExecuteImplCodeEditSnapshotStringFieldMax = 128;
inline constexpr int kEngineExecuteImplCodeEditActionStringFieldMax = 16;
inline constexpr int
    kEngineExecuteImplCodeEditClearUnsplitUnmapIndexAssociationSizeFieldMax =
        63;
inline constexpr int
    kEngineExecuteImplCodeEditClearUnsplitUnmapSplitMapEditsSizeLimit = 64;
inline constexpr int
    kEngineExecuteImplCodeEditClearUnsplitUnmapSplitMapFromVariableIdStringFieldMax =
        256;
inline constexpr int
    kEngineExecuteImplCodeEditClearUnsplitUnmapSplitMapToToStringFieldMax = 256;
inline constexpr int kEngineExecuteImplDisasmLimitSizeFieldDefault = 128;
inline constexpr int kEngineExecuteImplDisasmLimitSizeFieldMax = 512;
inline constexpr int kEngineExecuteImplBytesSizeSizeSizeFieldDefault = 256;
inline constexpr int kEngineExecuteImplBytesSizeSizeSizeFieldMax = 65536;
inline constexpr int kEngineExecuteImplBytesEncodingTextEncodingStringFieldMax =
    64;
inline constexpr int kEngineExecuteImplAnnotationSetTextTextStringFieldMax =
    65536;
inline constexpr int kEngineExecuteImplRenameNameNameStringFieldMax = 4096;
inline constexpr int
    kEngineExecuteImplFunctionTypeOptionsDeclarationStringFieldMax = 4096;
inline constexpr int
    kEngineExecuteImplFunctionTypeOptionsTypeTypeStringFieldMax = 4096;
inline constexpr int kEngineExecuteImplItemDefineActionStringFieldMax = 16;
inline constexpr int
    kEngineExecuteImplItemDefineCodeArrayWidthElementSizeSizeFieldDefault = 1;
inline constexpr int
    kEngineExecuteImplItemDefineCodeArrayWidthElementSizeSizeFieldMax =
        ::neverd::sdk::kMaxDataItemArrayElementBytes;
inline constexpr int kEngineExecuteImplItemDefineCodeArrayCountSizeFieldMax =
    ::neverd::sdk::kMaxDataItemArrayElements;
inline constexpr int
    kEngineExecuteImplItemDefineCodeArrayRowTypeStringFieldMax = 512;
inline constexpr int
    kEngineExecuteImplItemDefineCodeArrayAlignmentSizeFieldDefault = 16;
inline constexpr int
    kEngineExecuteImplItemDefineCodeArrayAlignmentSizeFieldMax =
        ::neverd::sdk::kMaxDataItemAlignment;
inline constexpr int
    kEngineExecuteImplItemDefineCodeArrayAlignmentDataSizeSizeSizeFieldMax = 8;
inline constexpr int kEngineExecuteImplOperandFormatActionStringFieldMax = 16;
inline constexpr int kEngineExecuteImplOperandFormatAskedOperandSizeFieldMax =
    7;
inline constexpr int kEngineExecuteImplOperandFormatEnumNextTypeStringFieldMax =
    512;
inline constexpr int kEngineExecuteImplDecompileRepresentationStringFieldMax =
    16;
inline constexpr int kEngineExecuteImplDecompileOptionsDumpSizeLimit = 4096;
inline constexpr int kEngineExecuteImplDecompileLimitSizeFieldDefault = 512;
inline constexpr int kEngineExecuteImplDecompileLimitSizeFieldMax = 2048;
inline constexpr int kEngineExecuteImplCfgViewportLayoutRevisionStringFieldMax =
    256;
inline constexpr int kEngineExecuteImplCfgResultValueNodesJsonArraySizeLimit =
    500;
inline constexpr int kEngineExecuteImplCfgResultValueEdgesJsonArraySizeLimit =
    2000;
inline constexpr int kEngineExecuteImplXrefsDirectionStringFieldMax = 8;
inline constexpr int kEngineExecuteImplXrefsSourceStringFieldMax = 16;

// tools/neverd-worker/GraphSnapshot.cpp
inline constexpr int kGraphSnapshotIdTextSizeLimit = 128;
inline constexpr int kGraphSnapshotGraphSnapshotNodeAddressStartStringFieldMax =
    32;
inline constexpr int kGraphSnapshotGraphSnapshotNodeEndEndStringFieldMax = 32;
inline constexpr int kGraphSnapshotGraphSnapshotBytesTextSizeLimit = 2048;
inline constexpr int kGraphSnapshotGraphSnapshotTypeStringFieldMax = 64;
inline constexpr int kGraphSnapshotViewportLayoutRevisionStringFieldMax = 256;

// tools/neverd-worker/Listing.cpp
inline constexpr int kListingInstructionTextEnumsEnumValuesSizeLimit = 4096;
inline constexpr int kListingSetPdbDisplayNamesNameSizeLimit = 4096;
inline constexpr int kListingPageAfterSizeFieldDefault = 100;
inline constexpr int kListingPageAddressStringFieldMax = 18;
inline constexpr int kListingPageSubSubSizeFieldMax = 1 << 20;
inline constexpr int kListingOverviewBucketsSizeFieldDefault = 1024;
inline constexpr int kListingReferencesDirectionStringFieldMax = 8;
inline constexpr int kListingReferencesLimitSizeFieldDefault = 128;
inline constexpr int kListingStringsLimitSizeFieldDefault = 128;
inline constexpr int kListingStringsLimitSizeFieldMax = 512;
inline constexpr int kListingStringsTextLimitSizeFieldDefault = 400;
inline constexpr int kListingStringsTextLimitSizeFieldMax = 1024;
inline constexpr int kListingStringsSortSortStringFieldMax = 64;

// tools/neverd-worker/ProjectHistory.cpp
inline constexpr int kProjectHistoryValidateRowTextStringFieldMax = 65536;
inline constexpr int kProjectHistoryValidateRowOriginalStringFieldMax = 4096;
inline constexpr int kProjectHistoryValidateRowRenamedStringFieldMax = 4096;
inline constexpr int kProjectHistoryValidateRowStateStringFieldMax = 16;
inline constexpr int kProjectHistoryValidateRowNamesSizeLimit = 4096;
inline constexpr int
    kProjectHistoryValidateRowNameGetRefConstStdStringSizeLimit = 4096;
inline constexpr int kProjectHistoryValidateRowLanguageStringFieldMax = 16;
inline constexpr unsigned kProjectHistoryValidateRowSourceStringFieldMax =
    4u << 20;
inline constexpr int kProjectHistoryValidateRowKindStringFieldMax = 32;
inline constexpr int kProjectHistoryValidateRowEncodingStringFieldMax = 64;
inline constexpr int kProjectHistoryValidateRowBaseStringFieldMax = 16;
inline constexpr int kProjectHistoryValidateRowTypeTypeStringFieldMax = 512;
inline constexpr int
    kProjectHistoryValidateChangeAnnotationValueGetRefConstStdStringSizeLimit =
        65536;
inline constexpr int
    kProjectHistoryValidateChangeAnnotationFieldStringFieldMax = 65536;
inline constexpr int kProjectHistoryValidateCommandKindKindStringFieldMax = 32;
inline constexpr int kProjectHistoryProjectHistoryCommandsSizeLimit = 128;
inline constexpr int kProjectHistoryStageCommandsSizeLimit = 128;
inline constexpr int kProjectHistoryRecoverCommandsSizeLimit = 128;

// tools/neverd-worker/Protocol.cpp
inline constexpr int kProtocolValidateRequestIdStringFieldMax = 128;
inline constexpr int kProtocolValidateRequestOperationStringFieldMax = 64;
inline constexpr int kProtocolValidateRequestExpectedRevisionStringFieldMax =
    32;
inline constexpr int kProtocolValidateRequestProjectIdStringFieldMax = 128;

// tools/neverd-worker/StringReferences.cpp
inline constexpr int
    kStringReferencesStringReferenceTablePageLimitSizeFieldDefault = 128;
inline constexpr int
    kStringReferencesStringReferenceTablePageSortSortStringFieldMax = 16;
inline constexpr int
    kStringReferencesStringReferenceTablePageFilterStringFieldMax = 4096;

// tools/neverd-worker/main.cpp
inline constexpr int kMainIdleGracePeriodMilliseconds = 150;
inline constexpr int kMainExecuteLoopMilliseconds = 10;
inline constexpr int kMainMainHeartbeatSeconds = 1;
inline constexpr int kMainMainHelloCancelTargetRequestIdStringFieldMax = 128;
inline constexpr int kMainMainValueKeyGetRefConstStdStringSizeLimit = 128;

// tools/neverd-worker/CodeEdits.cpp
inline constexpr int kMaxSourceEditViews = 6;
inline constexpr int kMaxSourceDecoratorTokens = 32;

// tools/neverd-worker/Contributions.cpp
inline constexpr int kMaxContributionColumns = 16;
inline constexpr int kMaxContributionManifests = 16;

// tools/neverd-worker/Engine.cpp
inline constexpr int kMaxItemAttributes = 16;

// tools/neverd-worker/GraphSnapshot.cpp
inline constexpr int kMaxGraphNodeLines = 6;

// tools/neverd-worker/ProjectHistory.cpp
inline constexpr int kMaxOperandHistoryValues = 8;

// tools/neverd-worker/Engine.cpp
inline constexpr int kMaxGraphMetric = 1000;

} // namespace neverd::limits::worker

#endif // NEVERD_TOOLS_NEVERD_WORKER_RESOURCELIMITS_H
