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

#ifndef NEVERD_TOOLS_NEVERD_GUI_RESOURCELIMITS_H
#define NEVERD_TOOLS_NEVERD_GUI_RESOURCELIMITS_H

#include "../common/symbols/ResourceLimits.h"
#include "../common/transport/ResourceLimits.h"

#include "neverd/diff/Limits.h"
#include "neverd/patch/Limits.h"
#include "neverd/sdk/Limits.h"
#include "neverd/support/Limits.h"

#include <QtGlobal>
#include <cstddef>
#include <cstdint>

namespace neverd::limits::gui {
inline constexpr int kInterpreterRecoveryDialogWidth = 640;
inline constexpr int kInterpreterRecoveryDialogHeight = 600;
inline constexpr int kInterpreterRecoveryReportWidth = 760;
inline constexpr int kInterpreterRecoveryReportHeight = 560;
inline constexpr int kPatchPreviewDelayMs = 180;
inline constexpr int kPatchEditorWidth = 620;
inline constexpr int kPatchAnchorGap = 4;
inline constexpr int kPatchPreviewHeight = 200;
inline constexpr int kPatchAssemblyPreviewLines = 4;
inline constexpr int kPatchBytePreviewLines = 2;
inline constexpr int kPatchPreviewHexCharacters = 256;
inline constexpr int kPatchConfirmationHexCharacters = 64;
inline constexpr int kPatchConfirmationInstructions = 4;
inline constexpr int kPatchLogHexCharacters = 64;
inline constexpr int kPatchLogInstructions = 4;
inline constexpr int kRewriteDialogWidth = 960;
inline constexpr int kRewriteDialogHeight = 720;

// Binary comparison views. Rows and request admission stay bounded separately.
inline constexpr int kComparisonPageSize =
    ::neverd::limits::diff::kDefaultPageSize;
inline constexpr int kComparisonCachedPages = 16;
inline constexpr int kComparisonPendingPages = 4;
inline constexpr int kComparisonPendingRequests = 64;
inline constexpr int kComparisonGraphDebounceMs = 40;
inline constexpr int kComparisonGraphVisibleNodes = 2048;
inline constexpr int kComparisonGraphVisibleEdges = 8192;
inline constexpr double kComparisonZoomStep = 1.2;
inline constexpr int kComparisonInitialWidth = 1280;
inline constexpr int kComparisonInitialHeight = 800;
inline constexpr int kComparisonMinimumWidth = 760;
/// Keep source code readable below the comparison and paging controls.
inline constexpr int kComparisonMinimumHeight = 640;
/// Digest display prefix; full identities remain in row evidence and tooltips.
inline constexpr int kLineageDigestDisplayCharacters = 12;
inline constexpr int kLineageInitialWidth = 1180;
inline constexpr int kLineageInitialHeight = 780;
inline constexpr int kLineageMinimumWidth = 760;
inline constexpr int kLineageMinimumHeight = 560;
inline constexpr int kLineageGraphMinimumHeight = 180;
inline constexpr int kLineageGraphInitialHeight = 300;
inline constexpr int kLineageSourceInitialHeight = 220;
inline constexpr int kLineageReleaseChooserWidth = 760;
inline constexpr int kLineageReleaseChooserHeight = 520;
inline constexpr int kComparisonSourceMinimumVisibleLines = 6;
inline constexpr int kComparisonCounterpartInitialWidth = 940;
inline constexpr int kComparisonCounterpartInitialHeight = 620;
inline constexpr int kComparisonCounterpartMinimumWidth = 700;
inline constexpr int kComparisonCounterpartMinimumHeight = 480;
inline constexpr int kComparisonReviewInitialWidth = 1100;
inline constexpr int kComparisonReviewInitialHeight = 800;
inline constexpr int kComparisonReviewMinimumWidth = 880;
inline constexpr int kComparisonReviewMinimumHeight = 640;
inline constexpr int kComparisonValidationInitialWidth = 920;
inline constexpr int kComparisonValidationInitialHeight = 680;
inline constexpr int kComparisonValidationMinimumWidth = 760;
inline constexpr int kComparisonValidationMinimumHeight = 540;
inline constexpr int kComparisonValidationTimeoutMs = 5000;
inline constexpr int kComparisonChangesInitialWidth = 1180;
inline constexpr int kComparisonChangesInitialHeight = 820;
inline constexpr int kComparisonChangesMinimumWidth = 880;
inline constexpr int kComparisonChangesMinimumHeight = 620;
inline constexpr int kComparisonImpactInitialWidth = 1180;
inline constexpr int kComparisonImpactInitialHeight = 820;
inline constexpr int kComparisonImpactMinimumWidth = 880;
inline constexpr int kComparisonImpactMinimumHeight = 620;
inline constexpr double kComparisonMinimumZoom = 0.05;
inline constexpr double kComparisonMaximumZoom = 4.0;

// tools/neverd-gui/EngineClient.cpp
inline constexpr qsizetype kEngineClientMaxFrame = 8 * 1024 * 1024;
inline constexpr qint64 kEngineClientMaxWriteQueue = 16 * 1024 * 1024;
// A line no newline ends is reported once it grows past a burst.
inline constexpr qsizetype kEngineClientMaxPartialLine = 65536;

// tools/neverd-gui/app/ChooserView.cpp
inline constexpr int kChooserViewPageSize = 256;
inline constexpr int kChooserViewMaxCachedPages = 64;
/// Bound address lookups needed when discovery moves selected rows off-page.
inline constexpr int kChooserViewMaxRefreshLookups = kChooserViewMaxCachedPages;
inline constexpr int kChooserViewMaxPendingPages = 4;
inline constexpr int kChooserViewFilterDebounceMs = 150;
inline constexpr int kChooserViewChooserModelRequestPageTextLimit = 400;

// tools/neverd-gui/app/CodeView.cpp
inline constexpr int kCodeViewPageLines = 256;
/// A completed function's display-name refresh can batch existing text. Keep
/// the interactive first load small, and retry it if a batch exceeds bytes.
inline constexpr int kCodeViewNameRefreshPageLines =
    ::neverd::sdk::kMaxCodeViewPageLines;
/// The characters a function's hint holds on a line before its type breaks
/// after a comma.
inline constexpr int kCodeViewHintColumns = 96;

// tools/neverd-gui/app/FindInViewDialog.cpp
inline constexpr int kFindInViewDebounceMs = 120;

// Selected byte export: bound retained bytes and the interactive preview.
inline constexpr quint64 kExportDataMaxBytes = 1024 * 1024;
inline constexpr qsizetype kExportDataChunkBytes = 4096;
inline constexpr qsizetype kExportDataPreviewBytes = 4096;
inline constexpr int kExportDataDebounceMs = 120;

// tools/neverd-gui/app/DisassemblyView.cpp
inline constexpr std::size_t kDisassemblyViewMaxHistory = 512;

// tools/neverd-gui/app/HexView.cpp
inline constexpr int kHexViewChunkBytes = 4096;
inline constexpr int kHexViewMaxChunks = 256;

// tools/neverd-gui/app/InteractionMetrics.cpp
/// Functions sampled for jumps, graphs and pseudocode, spread over the list.
inline constexpr int kInteractionMetricsMaxSamples = 24;
/// Passes over the sampled functions for each painted deep-link series.
inline constexpr int kInteractionMetricsLinkPasses = 8;
/// How long one sample may take before it counts as missed.
inline constexpr int kInteractionMetricsSampleTimeoutMs = 60000;

// tools/neverd-gui/app/JumpDialog.cpp
inline constexpr int kJumpDialogMaxHistory = 25;
inline constexpr int kJumpDialogMaxMatches = 200;
inline constexpr int kJumpDialogSearchDebounceMs = 90;

// tools/neverd-gui/app/ListingView.cpp
inline constexpr int kListingViewPageLines = 240;
/// Bound each side of a refresh to the listing protocol's per-side budget.
inline constexpr int kListingViewRefreshContextLines =
    ::neverd::limits::transport::kListingMaxPageLines;
inline constexpr int kListingViewMaxLoadedLines = 6000;
inline constexpr int kListingViewScrollDebounceMs = 15;
inline constexpr int kListingViewMaxArrowLevels = 10;

// tools/neverd-gui/app/MainWindow.cpp
// String search defaults and bounds (the worker's string_options).
inline constexpr int kMainWindowDefaultStringMinLength =
    ::neverd::limits::support::kDefaultStringMinLength;
inline constexpr int kMainWindowMaxStringMinLength =
    ::neverd::strings::MaxMinLength;
inline constexpr int kMainWindowMaxOpcodeBytes = 12;
// As much as one declaration of the editor takes.
inline constexpr qint64 kMainWindowMaxHeaderBytes = 4 << 20;
/// The stops the Back or Forward button's list shows, nearest first.
inline constexpr std::size_t kMainWindowHistoryListStops = 30;

// tools/neverd-gui/app/MicrosoftSymbols.cpp
/// How long to wait for another modal dialog to close before asking.
inline constexpr int kMicrosoftSymbolsModalRetryMs = 150;

// tools/neverd-gui/app/NavigationBand.cpp
inline constexpr int kNavigationBandRefreshDebounceMs = 120;

// tools/neverd-gui/app/OutputWindow.cpp
inline constexpr int kOutputWindowMaximumLogLines = 20000;
inline constexpr int kOutputWindowMaximumHistory = 200;

// tools/neverd-gui/app/ProjectDatabase.cpp

// tools/neverd-gui/app/Session.cpp
inline constexpr int kSessionCacheMiB = 256;
inline constexpr int kSessionMaxRecentFiles = 10;

// tools/neverd-gui/app/SetTypeDialog.cpp
/// How long typing pauses before the text is read, or names completed.
inline constexpr int kSetTypeDialogCheckDebounceMs = 150;
inline constexpr int kSetTypeDialogCompleteDebounceMs = 80;
/// How many local types a completion offers.
inline constexpr int kSetTypeDialogCompletionLimit = 20;

// tools/neverd-gui/app/SymbolDownload.cpp
inline constexpr qint64 kSymbolDownloadMaxBytes =
    static_cast<qint64>(::neverd::limits::symbol_download::kMaxBytes);
inline constexpr int kSymbolDownloadIdleTimeoutMs =
    ::neverd::limits::symbol_download::kIdleSeconds * 1000;
inline constexpr int kSymbolDownloadDownloadTimeoutMs =
    ::neverd::limits::symbol_download::kTransferSeconds * 1000;
inline constexpr int kSymbolDownloadMaxRedirects =
    ::neverd::limits::symbol_download::kMaxRedirects;
inline constexpr qint64 kSymbolDownloadReadBufferBytes = 256 * 1024;
inline constexpr qint64 kSymbolDownloadReadChunkBytes = 64 * 1024;
/// A transfer the network or the server broke is tried again this many
/// times, after a pause that grows with each.
inline constexpr int kSymbolDownloadTransferRetries =
    ::neverd::limits::symbol_download::kTransferRetries;
inline constexpr int kSymbolDownloadRetryPauseMs =
    ::neverd::limits::symbol_download::kRetryPauseMs;

// tools/neverd-gui/app/TypePickerDialog.cpp
/// How many types a search lists, and how long typing pauses before one.
inline constexpr int kTypePickerDialogSearchLimit = 100;
inline constexpr int kTypePickerDialogSearchDebounceMs = 70;

// tools/neverd-gui/mcp/GuiSessionBroker.cpp
inline constexpr quint32 kGuiSessionBrokerMaxFrame = 8 * 1024 * 1024;

// tools/neverd-gui/mcp/McpConnectionManager.cpp
inline constexpr qsizetype kMcpConnectionManagerMaxMessage = 8 * 1024 * 1024;

// tools/neverd-gui/mcp/McpConnectionManager.h
inline constexpr int kMcpConnectionManagerRetryMs = 1000;

// tools/neverd-gui/web/PackageSession.cpp
inline constexpr qsizetype kPackageSessionMaxInventoryRows = 4096;
inline constexpr qsizetype kPackageSessionMaxFindingRows = 8192;
inline constexpr qsizetype kPackageSessionMaxMetadataBytes = 16 * 1024 * 1024;
inline constexpr int kPackageSessionRequestTimeoutMs = 120000;
inline constexpr int kPackageSessionPageSize = 128;

// tools/neverd-gui/web/ReviewJournal.h
inline constexpr qsizetype kReviewJournalMaxJournalBytes = 256 * 1024;
inline constexpr qsizetype kReviewJournalMaxJournalDecisions = 256;

// tools/neverd-gui/EngineClient.cpp
inline constexpr int
    kEngineClientWorkerTransportStopWaitForFinishedMilliseconds = 2000;

// tools/neverd-gui/LibraryCodeView.cpp
inline constexpr int kLibraryCodeViewResetRegionsSizeLimit = 256;
inline constexpr int kLibraryCodeViewResetRegionAvailableSpansSizeLimit = 1024;
inline constexpr int kLibraryCodeViewResetRegionAvailableSpanCountLimit = 4096;

// tools/neverd-gui/QueryService.cpp
inline constexpr int kQueryServiceCanonicalDepthLimit = 64;
inline constexpr int kQueryServiceStateClampArgument3 = 1024 * 1024 * 1024;
inline constexpr int kQueryServiceStateLimitsMaxRequestBytesClampArgument3 =
    8 * 1024 * 1024;
inline constexpr int kQueryServiceSetCacheBudgetMiBClampArgument3 = 1024;

// tools/neverd-gui/app/MainWindow.cpp
inline constexpr int
    kMainWindowSearchBinaryBeginSearchRequestSearchSubscriptionSingleShotMilliseconds =
        1;

// tools/neverd-gui/app/Session.cpp
inline constexpr int kSessionSessionSetIntervalMilliseconds = 300;
inline constexpr int kSessionSendOpenLargeFunctionCountLimit = 65536;
inline constexpr int kSessionSendOpenLargeQFileInfoFilePathSizeLimit =
    64 * 1024 * 1024;
inline constexpr int kSessionSendOpenFunctionCountLimit = 4096;

// tools/neverd-gui/main.cpp
inline constexpr int kStartupMinBenchmarkTimeoutMilliseconds = 100;
inline constexpr int kStartupMaxBenchmarkTimeoutMilliseconds = 300000;
inline constexpr int kInteractionMinBenchmarkTimeoutMilliseconds = 1000;
inline constexpr int kInteractionMaxBenchmarkTimeoutMilliseconds = 3600000;

// tools/neverd-gui/mcp/GuiSessionBroker.cpp
inline constexpr int kGuiSessionBrokerGuiSessionBrokerSetIntervalMilliseconds =
    1000;
inline constexpr int kGuiSessionBrokerAcceptClientsSingleShotMilliseconds =
    5000;
inline constexpr int kGuiSessionBrokerHandleIdSizeLimit = 128;
inline constexpr int kGuiSessionBrokerHandleRevisionSizeLimit = 128;
inline constexpr int kGuiSessionBrokerHandleRoutesSizeLimit = 32;

// tools/neverd-gui/mcp/McpConnectionManager.cpp
inline constexpr int
    kMcpConnectionManagerMcpConnectionManagerDiagnosticsSizeLimit = 65536;
inline constexpr int
    kMcpConnectionManagerMcpConnectionManagerSetIntervalMilliseconds = 1000;
inline constexpr int
    kMcpConnectionManagerMcpConnectionManagerNowPendingValueIdStartedLimit =
        120000;
inline constexpr int
    kMcpConnectionManagerMcpConnectionManagerWaitForFinishedMilliseconds = 1000;
inline constexpr int
    kMcpConnectionManagerDisconnectServerSingleShotMilliseconds = 1000;
inline constexpr int kMcpConnectionManagerSendPendingSizeLimit = 32;
inline constexpr int kMcpConnectionManagerSendBodySizeLimit = 1024 * 1024;
inline constexpr int kMcpConnectionManagerSendProcessBytesToWriteLimit =
    2 * 1024 * 1024;
inline constexpr int
    kMcpConnectionManagerRequestSetTransferTimeoutMilliseconds = 120000;
inline constexpr int kMcpConnectionManagerPostBodySizeLimit = 1024 * 1024;
inline constexpr int kMcpConnectionManagerReadHttpInitializeSessionSizeLimit =
    4096;
inline constexpr int kMcpConnectionManagerReadHttpIdSizeLimit = 4096;
inline constexpr int kMcpConnectionManagerCallToolJsonArgumentsSizeLimit =
    1024 * 1024;
inline constexpr int
    kMcpConnectionManagerRecordCallTruncatedArgumentsSizeLimit = 4096;
inline constexpr int kMcpConnectionManagerRecordCallHistorySizeLimit = 50;
inline constexpr int kMcpConnectionManagerFinishCallResultSizeLimit = 4096;

// tools/neverd-gui/mcp/GuiSessionBroker.cpp
inline constexpr int kMaxGuiSessionClients = 4;

inline constexpr int kDefaultStartupBenchmarkTimeoutMilliseconds = 30000;
inline constexpr int kDefaultInteractionBenchmarkTimeoutMilliseconds = 600000;
inline constexpr int kMaxArrayElementBytes =
    ::neverd::sdk::kMaxDataItemArrayElementBytes;
inline constexpr int kMaxArrayElements =
    ::neverd::sdk::kMaxDataItemArrayElements;
inline constexpr int kMaxDataAlignment = ::neverd::sdk::kMaxDataItemAlignment;
// tools/neverd-gui/benchmarks/mcp_startup_probe.cpp
inline constexpr int kMaxMcpStartupSamples = 100;

inline constexpr int kDefaultMcpStartupSamples = 3;

// tools/neverd-gui/mcp/GuiSessionBroker.cpp
inline constexpr int kGuiRouteTimeoutMilliseconds = 120000;

// tools/neverd-gui/LibraryCodeView.cpp
inline constexpr int kMaxLibraryViewByteOffset = 32 * 1024 * 1024;

// tools/neverd-gui/LibraryCodeView.cpp
inline constexpr int kMaxLibraryViewOrdinal = 8192;

// tools/neverd-gui/app/PathfinderView.cpp
inline constexpr int kDefaultPathfinderDepth =
    static_cast<int>(::neverd::sdk::kDefaultCallPathDepth);
inline constexpr int kMaxPathfinderDepth =
    static_cast<int>(::neverd::sdk::kMaxCallPathDepth);
inline constexpr int kMaxPathfinderWaypoints =
    static_cast<int>(::neverd::sdk::kMaxCallPathWaypoints);
inline constexpr int kPathfinderSearchDelayMs = 150;

// tools/neverd-gui/app/InstanceServer.cpp
/// The most a request to a workbench may hold, and a reply its client reads:
/// get_info's, with its paths.
inline constexpr int kInstanceServerMaxRequestBytes = 64 * 1024;
inline constexpr qint64 kInstanceServerMaxReplyBytes = 1024 * 1024;
/// How long a client waits for a workbench to answer.
inline constexpr int kInstanceServerRequestTimeoutMs = 2000;
/// Concurrent probes share one discovery deadline.
inline constexpr int kInstanceProbeConcurrency = 8;

// tools/neverd-gui/app/LinkDispatch.cpp
/// How long a workbench may take to open a link, which resolves through its
/// worker.
inline constexpr int kLinkDispatchOpenTimeoutMs = 30000;
/// The desktop's scheme registration helper must not block indefinitely.
inline constexpr int kLinkHandlerCommandTimeoutMs = 5000;

} // namespace neverd::limits::gui

#endif // NEVERD_TOOLS_NEVERD_GUI_RESOURCELIMITS_H
