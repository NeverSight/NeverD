//===- ComparisonDialog.cpp - Binary comparison review window ------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Native resizable review UI. Views share the published revision and preserve
/// explicit partial, unavailable and stale states alongside their evidence.
//===----------------------------------------------------------------------===//

#include "ComparisonDialog.h"

#include "../ResourceLimits.h"
#include "ComparisonChangesDialog.h"
#include "ComparisonCounterpartDialog.h"
#include "ComparisonFlowDialog.h"
#include "ComparisonGraph.h"
#include "ComparisonImpactDialog.h"
#include "ComparisonModel.h"
#include "ComparisonRegionsDialog.h"
#include "ComparisonReviewDialog.h"
#include "ComparisonSourceView.h"
#include "ComparisonValidationDialog.h"
#include "Icons.h"
#include "ModelessDialog.h"
#include "Theme.h"

#include <QCheckBox>
#include <QCloseEvent>
#include <QComboBox>
#include <QCoreApplication>
#include <QDoubleSpinBox>
#include <QFileDialog>
#include <QFileInfo>
#include <QFormLayout>
#include <QGroupBox>
#include <QGuiApplication>
#include <QHeaderView>
#include <QJsonArray>
#include <QKeySequence>
#include <QLabel>
#include <QLineEdit>
#include <QLocale>
#include <QMenu>
#include <QPlainTextEdit>
#include <QProgressBar>
#include <QPushButton>
#include <QScreen>
#include <QSettings>
#include <QSpinBox>
#include <QSplitter>
#include <QTabWidget>
#include <QTableView>
#include <QToolButton>
#include <QVBoxLayout>
#include <algorithm>
#include <utility>

namespace neverd::gui {
namespace {
namespace limits = ::neverd::limits::gui;
QString progressLabel(const QString &Phase) {
  if (Phase.startsWith("edit:")) {
    const char *Label =
        QT_TRANSLATE_NOOP("ComparisonDialog", "Preparing comparison edit");
    if (Phase == "edit:measure")
      Label =
          QT_TRANSLATE_NOOP("ComparisonDialog", "Updating affected functions");
    else if (Phase == "edit:publish")
      Label = QT_TRANSLATE_NOOP("ComparisonDialog", "Applying comparison edit");
    return QCoreApplication::translate("ComparisonDialog", Label);
  }
  if (Phase.startsWith("restore:")) {
    const char *Label =
        QT_TRANSLATE_NOOP("ComparisonDialog", "Checking saved evidence");
    if (Phase == "restore:read" || Phase == "restore:scan")
      Label = QT_TRANSLATE_NOOP("ComparisonDialog", "Reading saved comparison");
    else if (Phase == "restore:settings" || Phase == "restore:parse" ||
             Phase == "restore:records")
      Label =
          QT_TRANSLATE_NOOP("ComparisonDialog", "Checking saved comparison");
    else if (Phase == "restore:measure")
      Label = QT_TRANSLATE_NOOP("ComparisonDialog",
                                "Recomputing saved measurements");
    else if (Phase == "restore:publish")
      Label = QT_TRANSLATE_NOOP("ComparisonDialog", "Restoring comparison");
    return QCoreApplication::translate("ComparisonDialog", Label);
  }
  const auto Step = Phase.section(':', -1);
  const std::pair<const char *, const char *> Labels[] = {
      {"load", QT_TRANSLATE_NOOP("ComparisonDialog", "Loading input")},
      {"project-open",
       QT_TRANSLATE_NOOP("ComparisonDialog", "Opening saved project")},
      {"project-files",
       QT_TRANSLATE_NOOP("ComparisonDialog", "Reading saved project settings")},
      {"project-unpack",
       QT_TRANSLATE_NOOP("ComparisonDialog",
                         "Unpacking and checking saved input")},
      {"project-apply",
       QT_TRANSLATE_NOOP("ComparisonDialog", "Applying saved analysis")},
      {"image", QT_TRANSLATE_NOOP("ComparisonDialog", "Loading input")},
      {"identify",
       QT_TRANSLATE_NOOP("ComparisonDialog", "Identifying file format")},
      {"read", QT_TRANSLATE_NOOP("ComparisonDialog", "Reading file")},
      {"hash", QT_TRANSLATE_NOOP("ComparisonDialog", "Checking file identity")},
      {"copy", QT_TRANSLATE_NOOP("ComparisonDialog", "Preparing input bytes")},
      {"parse",
       QT_TRANSLATE_NOOP("ComparisonDialog", "Reading format metadata")},
      {"load-discovery",
       QT_TRANSLATE_NOOP("ComparisonDialog", "Finding function entries")},
      {"metadata",
       QT_TRANSLATE_NOOP("ComparisonDialog", "Preparing image metadata")},
      {"platform",
       QT_TRANSLATE_NOOP("ComparisonDialog", "Resolving calling convention")},
      {"debug", QT_TRANSLATE_NOOP("ComparisonDialog", "Reading debug symbols")},
      {"debug-apply",
       QT_TRANSLATE_NOOP("ComparisonDialog", "Applying debug symbols")},
      {"ready", QT_TRANSLATE_NOOP("ComparisonDialog", "Input ready")},
      {"discovery",
       QT_TRANSLATE_NOOP("ComparisonDialog", "Discovering functions")},
      {"decode", QT_TRANSLATE_NOOP("ComparisonDialog", "Reading instructions")},
      {"features",
       QT_TRANSLATE_NOOP("ComparisonDialog", "Comparing function features")},
      {"exact",
       QT_TRANSLATE_NOOP("ComparisonDialog", "Matching exact features")},
      {"context",
       QT_TRANSLATE_NOOP("ComparisonDialog", "Comparing function context")},
      {"approximate",
       QT_TRANSLATE_NOOP("ComparisonDialog", "Finding similar functions")},
      {"ambiguities",
       QT_TRANSLATE_NOOP("ComparisonDialog", "Checking ambiguous matches")},
      {"measure",
       QT_TRANSLATE_NOOP("ComparisonDialog", "Measuring function changes")}};
  const auto Translate = [](const char *Text) {
    return QCoreApplication::translate("ComparisonDialog", Text);
  };
  QString Text =
      Translate(QT_TRANSLATE_NOOP("ComparisonDialog", "Comparing binaries"));
  for (const auto &[Token, Description] : Labels)
    if (Step == QLatin1String(Token)) {
      Text = Translate(Description);
      break;
    }
  if (Phase.startsWith("primary:"))
    return Translate(QT_TRANSLATE_NOOP("ComparisonDialog", "Primary: %1"))
        .arg(Text);
  if (Phase.startsWith("secondary:"))
    return Translate(QT_TRANSLATE_NOOP("ComparisonDialog", "Secondary: %1"))
        .arg(Text);
  return Text;
}
QLabel *label(QWidget *Parent, const QString &Text = {}) {
  auto *L = new QLabel(Text, Parent);
  L->setTextFormat(Qt::PlainText);
  L->setWordWrap(true);
  L->setTextInteractionFlags(Qt::TextSelectableByMouse |
                             Qt::TextSelectableByKeyboard);
  return L;
}
QTableView *table(QWidget *Parent, ComparisonModel *Model, const char *Name) {
  auto *T = new QTableView(Parent);
  T->setObjectName(Name);
  T->setModel(Model);
  T->setSelectionBehavior(QAbstractItemView::SelectRows);
  T->setSelectionMode(QAbstractItemView::SingleSelection);
  T->setEditTriggers(QAbstractItemView::NoEditTriggers);
  T->setAlternatingRowColors(true);
  T->setWordWrap(false);
  T->setShowGrid(false);
  T->verticalHeader()->hide();
  T->verticalHeader()->setDefaultSectionSize(T->fontMetrics().height() + 8);
  T->horizontalHeader()->setStretchLastSection(true);
  return T;
}
QString changeList(const QJsonArray &Rows) {
  QStringList Text;
  for (const auto &V : Rows)
    Text << V.toString();
  return Text.join(", ");
}
} // namespace

ComparisonDialog::ComparisonDialog(const QString &Worker,
                                   const QString &PrimaryPath, QWidget *Parent)
    : QDialog(Parent), Session(Worker, this) {
  setObjectName("binaryComparison");
  setWindowTitle(tr("Binary comparison"));
  setWindowFlags((windowFlags() | Qt::WindowMinMaxButtonsHint) &
                 ~Qt::WindowContextHelpButtonHint);
  setWindowIcon(icon("compare"));
  setAttribute(Qt::WA_DeleteOnClose);
  setSizeGripEnabled(true);
  resize(limits::kComparisonInitialWidth, limits::kComparisonInitialHeight);
  setMinimumSize(limits::kComparisonMinimumWidth,
                 limits::kComparisonMinimumHeight);
  const auto Geometry = QSettings().value("comparison/geometry").toByteArray();
  if (!Geometry.isEmpty())
    restoreGeometry(Geometry);
  if (screen())
    resize(size().boundedTo(screen()->availableGeometry().size()));
  auto *Root = new QVBoxLayout(this);
  auto *Inputs = new QGridLayout;
  for (int I = 0; I < 2; ++I) {
    const auto Name = I ? tr("Secondary") : tr("Primary");
    auto *Label = new QLabel(Name, this);
    Paths[I] = new QLineEdit(this);
    Paths[I]->setObjectName(I ? "comparisonSecondary" : "comparisonPrimary");
    Paths[I]->setPlaceholderText(I ? tr("New binary or saved project")
                                   : tr("Original binary or saved project"));
    Paths[I]->setToolTip(
        tr("Select a binary or an NDDB saved project. Saved projects use their "
           "stored load settings and a private working copy."));
    Label->setBuddy(Paths[I]);
    auto *Browse = new QPushButton(icon("open"), tr("Browse…"), this);
    Loaders[I] = new QComboBox(this);
    Loaders[I]->setObjectName(I ? "secondaryLoader" : "primaryLoader");
    Loaders[I]->addItem(tr("Auto format"), "");
    Loaders[I]->addItem(tr("Raw x64"), "x86_64");
    Loaders[I]->addItem(tr("Raw x86"), "x86");
    Loaders[I]->addItem(tr("Raw AArch64"), "aarch64");
    Loaders[I]->setAccessibleName(Name + tr(" input format"));
    Bases[I] = new QLineEdit("0x0", this);
    Bases[I]->setObjectName(I ? "secondaryBase" : "primaryBase");
    Bases[I]->setPlaceholderText(tr("Image base"));
    Bases[I]->setAccessibleName(Name + tr(" image base"));
    Bases[I]->setVisible(false);
    Inputs->addWidget(Label, I, 0);
    Inputs->addWidget(Paths[I], I, 1);
    Inputs->addWidget(Browse, I, 2);
    Inputs->addWidget(Loaders[I], I, 3);
    Inputs->addWidget(Bases[I], I, 4);
    connect(Browse, &QPushButton::clicked, this, [this, I, Name] {
      const auto Path = QFileDialog::getOpenFileName(
          this, tr("Select %1 input").arg(Name), Paths[I]->text());
      if (!Path.isEmpty())
        Paths[I]->setText(Path);
    });
    connect(Paths[I], &QLineEdit::textChanged, this, &ComparisonDialog::update);
    connect(Loaders[I], &QComboBox::currentIndexChanged, this,
            [this, I](int Index) { Bases[I]->setVisible(Index != 0); });
  }
  Inputs->setColumnStretch(1, 1);
  Root->addLayout(Inputs);
  Paths[0]->setText(PrimaryPath);

  auto *Actions = new QHBoxLayout;
  Start = new QPushButton(icon("compare"), tr("Compare"), this);
  Start->setObjectName("comparisonStart");
  Cancel = new QPushButton(tr("Cancel"), this);
  Cancel->setObjectName("comparisonCancel");
  Reopen = new QPushButton(tr("Reopen…"), this);
  Reopen->setObjectName("comparisonReopen");
  Reopen->setToolTip(
      tr("Select both inputs and their original settings, then reopen a saved "
         "comparison. Input hashes and analysis revisions are checked."));
  Save = new QPushButton(icon("save"), tr("Save / export…"), this);
  Regions = new QPushButton(tr("Regions…"), this);
  Regions->setObjectName("comparisonRegionReview");
  Regions->setToolTip(
      tr("Inspect overlapping region and transformation hypotheses. Enable "
         "analysis in Settings before comparing."));
  auto *RegionMenu = new QMenu(Regions);
  Regions->setMenu(RegionMenu);
  for (const auto &[Name, SSA] :
       {std::pair{tr("Native instruction regions…"), false},
        {tr("SSA dependency regions…"), true}})
    connect(RegionMenu->addAction(Name), &QAction::triggered, this,
            [this, SSA] {
              auto &View = SSA ? FlowRegionsView : RegionsView;
              if (!View) {
                View = new ComparisonRegionsDialog(Session, this, SSA);
                connect(View, &ComparisonRegionsDialog::inspectCallContext,
                        this, &ComparisonDialog::showImpact);
                connect(View, &ComparisonRegionsDialog::inspectFunction, this,
                        [this](const auto &Side, const auto &Address) {
                          selectFunction(Side, Address);
                          raise();
                          activateWindow();
                        });
              }
              View->show();
              View->raise();
              View->activateWindow();
            });
  DataFlow = new QPushButton(tr("Dependencies…"), this);
  DataFlow->setObjectName("comparisonFlowReview");
  connect(DataFlow, &QPushButton::clicked, this, [this] {
    if (!FlowView) {
      FlowView = new ComparisonFlowDialog(Session, this);
      connect(FlowView, &ComparisonFlowDialog::inspectFunction, this,
              [this](const auto &Side, const auto &Address) {
                selectFunction(Side, Address);
                raise();
                activateWindow();
              });
    }
    FlowView->show();
    FlowView->raise();
    FlowView->activateWindow();
  });
  auto *SettingsButton = new QToolButton(this);
  SettingsButton->setObjectName("comparisonSettings");
  SettingsButton->setText(tr("Settings"));
  SettingsButton->setCheckable(true);
  for (auto *B : {Start, Cancel, Reopen, Save, Regions, DataFlow})
    Actions->addWidget(B);
  Actions->addWidget(SettingsButton);
  Actions->addStretch();
  Root->addLayout(Actions);
  auto *Settings = new QWidget(this);
  auto *SettingsLayout = new QGridLayout(Settings);
  SettingsLayout->setContentsMargins(0, 0, 0, 0);
  Names = new QCheckBox(tr("Use symbol names"), Settings);
  Names->setChecked(true);
  AnalyzeRegions = new QCheckBox(tr("Region hypotheses"), Settings);
  AnalyzeRegions->setObjectName("comparisonAnalyzeRegions");
  AnalyzeRegions->setToolTip(
      tr("Bounded native instruction-run analysis for split, merge, inline and "
         "clone hypotheses. This does not validate behavior."));
  RegionCalls = new QCheckBox(tr("Include matched calls"), Settings);
  RegionCalls->setObjectName("comparisonRegionCalls");
  RegionCalls->setToolTip(
      tr("Extend shared regions across direct calls whose targets are paired. "
         "Results depend on the current function pairs and are recomputed when "
         "pairs change. This does not validate call behavior."));
  connect(RegionCalls, &QCheckBox::toggled, this, [this](bool Checked) {
    if (Checked)
      AnalyzeRegions->setChecked(true);
  });
  connect(AnalyzeRegions, &QCheckBox::toggled, this, [this](bool Checked) {
    if (!Checked)
      RegionCalls->setChecked(false);
  });
  AnalyzeDataFlow = new QCheckBox(tr("Dependency evidence"), Settings);
  AnalyzeDataFlow->setObjectName("comparisonAnalyzeDataFlow");
  FlowProfile = new QComboBox(Settings);
  FlowProfile->setObjectName("comparisonFlowProfile");
  FlowProfile->setAccessibleName(tr("SSA representation"));
  FlowProfile->addItem(tr("Original SSA"),
                       ::neverd::limits::diff::kMedSSAProfile);
  FlowProfile->addItem(tr("Optimized SSA"),
                       ::neverd::limits::diff::kLLVMSSAProfile);
  FlowProfile->addItem(tr("Optimized SSA with parameter views"),
                       ::neverd::limits::diff::kLLVMParameterSSAProfile);
  FlowProfile->addItem(tr("Optimized SSA with loop analysis"),
                       ::neverd::limits::diff::kLLVMProgressSSAProfile);
  FlowProfile->addItem(
      tr("Optimized SSA with multi-block loop analysis"),
      ::neverd::limits::diff::kLLVMMultiBlockProgressSSAProfile);
  FlowProfile->addItem(tr("Optimized SSA with nested loop analysis"),
                       ::neverd::limits::diff::kLLVMNestedProgressSSAProfile);
  FlowProfile->addItem(tr("Optimized SSA with branching loop analysis"),
                       ::neverd::limits::diff::kLLVMBranchProgressSSAProfile);
  FlowProfile->addItem(tr("Optimized SSA with wrapping loop analysis"),
                       ::neverd::limits::diff::kLLVMModularProgressSSAProfile);
  FlowProfile->addItem(
      tr("Optimized SSA with parameter-bound loop analysis"),
      ::neverd::limits::diff::kLLVMParameterBoundProgressSSAProfile);
  FlowProfile->addItem(
      tr("Optimized SSA with expression-bound loop analysis"),
      ::neverd::limits::diff::kLLVMInvariantBoundProgressSSAProfile);
  FlowProfile->setEnabled(false);
  FlowProfile->setToolTip(
      tr("Optimized SSA reduces compiler scaffolding for comparison. Functions "
         "outside its supported scope remain listed with a reason.") +
      '\n' +
      tr("Parameter views compare the low 32 bits of eligible 64-bit native "
         "parameters. Original widths and sources remain available for "
         "review.") +
      '\n' +
      tr("Loop analysis proves return progress where supported before "
         "comparing "
         "computations. It includes parameter views and preserves unproved "
         "loop contracts."));
  AnalyzeFlowRegions = new QCheckBox(tr("SSA region hypotheses"), Settings);
  AnalyzeFlowRegions->setObjectName("comparisonAnalyzeFlowRegions");
  AnalyzeFlowRegions->setToolTip(
      tr("Index and verify finite dependency neighborhoods across functions. "
         "Includes dependency evidence."));
  FlowFragments = new QCheckBox(tr("Shared SSA fragments"), Settings);
  FlowFragments->setObjectName("comparisonFlowFragments");
  FlowFragments->setToolTip(
      tr("Find shared computations across changed dependencies. External input "
         "producers remain unverified. Includes SSA region hypotheses."));
  connect(FlowFragments, &QCheckBox::toggled, this, [this](bool On) {
    if (On)
      AnalyzeFlowRegions->setChecked(true);
  });
  connect(AnalyzeFlowRegions, &QCheckBox::toggled, this, [this](bool On) {
    if (On)
      AnalyzeDataFlow->setChecked(true);
    else
      FlowFragments->setChecked(false);
  });
  connect(AnalyzeDataFlow, &QCheckBox::toggled, this, [this](bool On) {
    FlowProfile->setEnabled(On);
    if (!On)
      AnalyzeFlowRegions->setChecked(false);
  });
  AnalyzeDataFlow->setToolTip(
      tr("Capture bounded SSA observations in address order. Pipeline "
         "cancellation is checked between functions. See Dependencies for "
         "omitted and unsupported functions."));
  FlowFunctions = new QSpinBox(Settings);
  FlowFunctions->setRange(1, ::neverd::limits::diff::kMaxFlowFunctions);
  FlowFunctions->setValue(::neverd::limits::diff::kDefaultFlowFunctions);
  FlowFunctions->setAccessibleName(
      tr("Maximum dependency functions per input"));
  Threshold = new QDoubleSpinBox(Settings);
  Threshold->setRange(0, 1);
  Threshold->setSingleStep(0.05);
  Threshold->setValue(::neverd::limits::diff::kDefaultMinimumSimilarity);
  Threshold->setAccessibleName(tr("Minimum similarity"));
  Timeout = new QSpinBox(Settings);
  Timeout->setRange(
      0, int(::neverd::limits::diff::kMaxTimeoutMilliseconds / 1000));
  Timeout->setSuffix(tr(" s"));
  Timeout->setSpecialValueText(tr("No deadline"));
  Timeout->setAccessibleName(tr("Comparison deadline"));
  SettingsLayout->addWidget(Names, 0, 0);
  SettingsLayout->addWidget(AnalyzeRegions, 0, 1);
  SettingsLayout->addWidget(new QLabel(tr("Minimum similarity"), Settings), 0,
                            2);
  SettingsLayout->addWidget(Threshold, 0, 3);
  SettingsLayout->addWidget(new QLabel(tr("Deadline"), Settings), 0, 4);
  SettingsLayout->addWidget(Timeout, 0, 5);
  SettingsLayout->addWidget(RegionCalls, 1, 1, 1, 3);
  SettingsLayout->addWidget(AnalyzeDataFlow, 2, 0);
  SettingsLayout->addWidget(new QLabel(tr("Functions per input"), Settings), 2,
                            1);
  SettingsLayout->addWidget(FlowFunctions, 2, 2);
  SettingsLayout->addWidget(AnalyzeFlowRegions, 2, 3, 1, 3);
  auto *ProfileLabel = new QLabel(tr("SSA representation"), Settings);
  ProfileLabel->setBuddy(FlowProfile);
  SettingsLayout->addWidget(ProfileLabel, 3, 0);
  SettingsLayout->addWidget(FlowProfile, 3, 1, 1, 3);
  SettingsLayout->addWidget(FlowFragments, 3, 4, 1, 2);
  SettingsLayout->setColumnStretch(6, 1);
  Settings->setVisible(false);
  Root->addWidget(Settings);
  connect(SettingsButton, &QToolButton::toggled, Settings,
          &QWidget::setVisible);

  Summary = label(
      this,
      tr("Compare two x86, x64 or AArch64 inputs with the same architecture "
         "and ABI. Matching scores are heuristic."));
  Summary->setObjectName("comparisonSummary");
  Root->addWidget(Summary);
  auto *Split = new QSplitter(this);
  Root->addWidget(Split, 1);
  auto *Inventory = new QWidget(Split);
  auto *InventoryLayout = new QVBoxLayout(Inventory);
  InventoryLayout->setContentsMargins(0, 0, 0, 0);
  auto *Filters = new QHBoxLayout;
  Side = new QComboBox(Inventory);
  Side->addItem(tr("Primary"), "primary");
  Side->addItem(tr("Secondary"), "secondary");
  Side->setAccessibleName(tr("Inventory side"));
  Kind = new QComboBox(Inventory);
  Kind->setObjectName("comparisonFilter");
  Kind->setAccessibleName(tr("Function state"));
  for (const auto &[Name, Key] : {std::pair{tr("All functions"), "all"},
                                  {tr("Changed"), "changed"},
                                  {tr("Uncertain"), "uncertain"},
                                  {tr("Matched"), "matched"},
                                  {tr("Ambiguous"), "ambiguous"},
                                  {tr("Unmatched"), "unmatched"}})
    Kind->addItem(Name, Key);
  Filters->addWidget(Side);
  Filters->addWidget(Kind, 1);
  InventoryLayout->addLayout(Filters);
  Search = new QLineEdit(Inventory);
  Search->setObjectName("comparisonSearch");
  Search->setPlaceholderText(tr("Filter by name or address"));
  Search->setAccessibleName(tr("Filter functions"));
  InventoryLayout->addWidget(Search);
  Functions =
      new ComparisonModel(Session, ComparisonModel::Kind::Functions, this);
  FunctionTable = table(Inventory, Functions, "comparisonFunctions");
  FunctionTable->setSortingEnabled(true);
  FunctionTable->horizontalHeader()->setSectionResizeMode(
      QHeaderView::Interactive);
  FunctionTable->setColumnHidden(0, true);
  FunctionTable->setColumnHidden(2, true);
  FunctionTable->setColumnWidth(1, 125);
  FunctionTable->setColumnWidth(3, 85);
  FunctionTable->setColumnWidth(4, 85);
  FunctionTable->setColumnWidth(5, 95);
  auto *Columns = FunctionTable->horizontalHeader();
  for (int Column = 0; Column < Functions->columnCount(); ++Column)
    if (!Columns->isSectionHidden(Column))
      Columns->resizeSection(Column,
                             std::max(Columns->sectionSize(Column),
                                      Columns->sectionSizeHint(Column)));
  Columns->setContextMenuPolicy(Qt::CustomContextMenu);
  connect(Columns, &QHeaderView::customContextMenuRequested, this,
          [this, Columns](const QPoint &Position) {
            QMenu Menu(Columns);
            for (int I = 0; I < Functions->columnCount(); ++I) {
              auto *Action = Menu.addAction(
                  Functions->headerData(I, Qt::Horizontal, Qt::DisplayRole)
                      .toString());
              Action->setCheckable(true);
              Action->setChecked(!FunctionTable->isColumnHidden(I));
              connect(Action, &QAction::toggled, this, [this, I](bool Visible) {
                FunctionTable->setColumnHidden(I, !Visible);
              });
            }
            Menu.exec(Columns->mapToGlobal(Position));
          });
  InventoryLayout->addWidget(FunctionTable, 1);
  auto *Details = new QWidget(Split);
  auto *DetailsLayout = new QVBoxLayout(Details);
  DetailsLayout->setContentsMargins(0, 0, 0, 0);
  FunctionTitle =
      label(Details, tr("Select a function to inspect its counterpart."));
  FunctionTitle->setObjectName("comparisonFunctionTitle");
  DetailsLayout->addWidget(FunctionTitle);
  auto *Edits = new QGridLayout;
  Counterpart = new QPushButton(tr("Counterpart"), Details);
  Confirm = new QPushButton(tr("Confirm pair"), Details);
  Unmatch = new QPushButton(tr("Unmatch"), Details);
  Remap = new QPushButton(tr("Pair with…"), Details);
  Remap->setObjectName("comparisonPairWith");
  Undo = new QPushButton(icon("undo"), tr("Undo"), Details);
  Undo->setObjectName("comparisonUndo");
  Redo = new QPushButton(icon("redo"), tr("Redo"), Details);
  Redo->setObjectName("comparisonRedo");
  int EditIndex = 0;
  for (auto *B : {Counterpart, Confirm, Unmatch, Remap, Undo, Redo}) {
    Edits->addWidget(B, EditIndex / 3, EditIndex % 3);
    B->setMinimumWidth(B->sizeHint().width());
    ++EditIndex;
  }
  Impact = new QPushButton(tr("Call paths…"), Details);
  Impact->setObjectName("comparisonImpactReview");
  RetryDetail = new QPushButton(tr("Retry difference"), Details);
  RetryDetail->setObjectName("comparisonRetryDetail");
  RetryDetail->setToolTip(
      tr("Recompute a cancelled or timed-out function difference."));
  Edits->addWidget(RetryDetail, 1, 3);
  connect(RetryDetail, &QPushButton::clicked, this, [this] {
    selectFunction(SelectedSide, SelectedSide == "primary" ? PrimaryAddress
                                                           : SecondaryAddress);
  });
  Impact->setToolTip(
      tr("Trace observed callers, callees and entry points of this function."));
  Edits->addWidget(Impact, 0, 3);
  Edits->setColumnStretch(4, 1);
  connect(Impact, &QPushButton::clicked, this, [this] {
    showImpact(SelectedSide,
               SelectedSide == "primary" ? PrimaryAddress : SecondaryAddress);
  });
  Changes = new QPushButton(tr("Change review…"), Details);
  Changes->setObjectName("comparisonChangesReview");
  Edits->addWidget(Changes, 2, 0, Qt::AlignLeft);
  connect(Changes, &QPushButton::clicked, this, [this] {
    if (!ChangesView) {
      ChangesView = new ComparisonChangesDialog(Session, this);
      connect(ChangesView, &ComparisonChangesDialog::inspectFunction, this,
              [this](const QString &Which, const QString &Address) {
                selectFunction(Which, Address);
                raise();
                activateWindow();
              });
      connect(ChangesView, &ComparisonChangesDialog::traceCalls, this,
              &ComparisonDialog::showImpact);
    }
    ChangesView->show();
    ChangesView->raise();
    ChangesView->activateWindow();
    ChangesView->review(SelectedSide, SelectedSide == "primary"
                                          ? PrimaryAddress
                                          : SecondaryAddress);
  });
  Reviews = new QPushButton(tr("Analyst review…"), Details);
  Reviews->setObjectName("comparisonReviewHistory");
  Edits->addWidget(Reviews, 2, 1, Qt::AlignLeft);
  Validate = new QPushButton(tr("Validate returns…"), Details);
  Validate->setObjectName("comparisonValidateReturns");
  Edits->addWidget(Validate, 2, 2, 1, 2, Qt::AlignLeft);
  connect(Validate, &QPushButton::clicked, this, [this] {
    if (!ValidationView)
      ValidationView = new ComparisonValidationDialog(Session, this);
    ValidationView->show();
    ValidationView->raise();
    ValidationView->activateWindow();
    ValidationView->review(SelectedSide, SelectedSide == "primary"
                                             ? PrimaryAddress
                                             : SecondaryAddress);
  });
  connect(Reviews, &QPushButton::clicked, this, [this] {
    if (!ReviewView) {
      ReviewView = new ComparisonReviewDialog(Session, this);
      connect(ReviewView, &ComparisonReviewDialog::saveComparison, Save,
              &QPushButton::click);
    }
    ReviewView->show();
    ReviewView->raise();
    ReviewView->activateWindow();
    ReviewView->review(SelectedSide, SelectedSide == "primary"
                                         ? PrimaryAddress
                                         : SecondaryAddress);
  });
  DetailsLayout->addLayout(Edits);
  auto *Tabs = new QTabWidget(Details);
  DetailsLayout->addWidget(Tabs, 1);
  Instructions =
      new ComparisonModel(Session, ComparisonModel::Kind::Instructions, this);
  InstructionTable = table(Tabs, Instructions, "comparisonInstructions");
  InstructionTable->setColumnWidth(0, 100);
  InstructionTable->setColumnWidth(1, 75);
  InstructionTable->setColumnWidth(2, 175);
  InstructionTable->setColumnWidth(3, 85);
  InstructionTable->setColumnWidth(4, 175);
  InstructionTable->setColumnWidth(5, 75);
  Tabs->addTab(InstructionTable, tr("Disassembly"));
  auto *Graphs = new QWidget(Tabs);
  auto *GraphLayout = new QVBoxLayout(Graphs);
  GraphLayout->setContentsMargins(0, 0, 0, 0);
  auto *GraphHeader = new QHBoxLayout;
  auto *GraphStatus = label(
      Graphs,
      tr("Select blocks to follow their counterparts. %1wheel zooms.")
          .arg(QKeySequence(Qt::CTRL).toString(QKeySequence::NativeText)));
  auto *Fit = new QPushButton(tr("Fit graphs"), Graphs);
  GraphHeader->addWidget(GraphStatus, 1);
  GraphHeader->addWidget(Fit);
  GraphLayout->addLayout(GraphHeader);
  auto *GraphSplit = new QSplitter(Graphs);
  PrimaryGraph = new ComparisonGraph(Session, "primary", GraphSplit);
  SecondaryGraph = new ComparisonGraph(Session, "secondary", GraphSplit);
  GraphLayout->addWidget(GraphSplit, 1);
  Tabs->addTab(Graphs, tr("Control flow"));
  connect(Fit, &QPushButton::clicked, PrimaryGraph, &ComparisonGraph::fit);
  connect(Fit, &QPushButton::clicked, SecondaryGraph, &ComparisonGraph::fit);
  connect(PrimaryGraph, &ComparisonGraph::blockSelected, this,
          [this](const auto &, const auto &Other) {
            SecondaryGraph->focusBlock(Other);
          });
  connect(SecondaryGraph, &ComparisonGraph::blockSelected, this,
          [this](const auto &, const auto &Other) {
            PrimaryGraph->focusBlock(Other);
          });
  for (auto *G : {PrimaryGraph, SecondaryGraph})
    connect(G, &ComparisonGraph::statusChanged, GraphStatus, &QLabel::setText);
  auto *EvidencePane = new QWidget(Tabs);
  auto *EvidenceLayout = new QVBoxLayout(EvidencePane);
  Evidence = new QPlainTextEdit(EvidencePane);
  Evidence->setObjectName("comparisonEvidence");
  Evidence->setAccessibleName(tr("Evidence"));
  Evidence->setReadOnly(true);
  Evidence->setFont(font());
  Evidence->setWordWrapMode(QTextOption::WrapAtWordBoundaryOrAnywhere);
  Evidence->setPlainText(tr("Matching evidence appears here."));
  EvidenceLayout->addWidget(Evidence);
  Tabs->addTab(EvidencePane, tr("Evidence"));
  Source = new ComparisonSourceView(Session, Tabs);
  Tabs->addTab(Source, tr("Pseudocode"));
  connect(Source, &ComparisonSourceView::instructionSelected, this,
          [this, Tabs](const QString &Side, int Row) {
            const int Column = Side == "secondary" ? 6 : 0;
            const auto Index = Instructions->index(Row, Column);
            if (!Index.isValid())
              return;
            Tabs->setCurrentWidget(InstructionTable);
            InstructionTable->setCurrentIndex(Index);
            InstructionTable->scrollTo(Index,
                                       QAbstractItemView::PositionAtCenter);
            InstructionTable->setFocus();
          });
  Split->setHandleWidth(6);
  Split->setChildrenCollapsible(false);
  Split->setStretchFactor(0, 1);
  Split->setStretchFactor(1, 2);
  Split->setSizes({400, 850});
  auto *Footer = new QHBoxLayout;
  Status = label(this, tr("Starting comparison worker…"));
  Status->setObjectName("comparisonStatus");
  Progress = new QProgressBar(this);
  Progress->setTextVisible(false);
  Progress->setVisible(false);
  Footer->addWidget(Status, 1);
  Footer->addWidget(Progress);
  Root->addLayout(Footer);

  connect(Start, &QPushButton::clicked, this, [this] { run(); });
  connect(Cancel, &QPushButton::clicked, &Session, &ComparisonSession::cancel);
  connect(Reopen, &QPushButton::clicked, this, [this] {
    const auto P = QFileDialog::getOpenFileName(
        this, tr("Reopen comparison"), SavedPath,
        tr("NeverD comparison (*.nddiff);;JSON (*.json)"));
    if (!P.isEmpty()) {
      SavedPath = P;
      run(P);
    }
  });
  connect(Save, &QPushButton::clicked, this, [this] {
    const auto P = QFileDialog::getSaveFileName(
        this, tr("Save comparison"),
        SavedPath.isEmpty() ? "comparison.nddiff" : SavedPath,
        tr("NeverD comparison (*.nddiff)"));
    if (!P.isEmpty()) {
      SavedPath = P;
      Session.command("comparison_save", {{"path", P}});
    }
  });
  connect(Undo, &QPushButton::clicked, this,
          [this] { Session.command("comparison_undo"); });
  connect(Redo, &QPushButton::clicked, this,
          [this] { Session.command("comparison_redo"); });
  connect(Confirm, &QPushButton::clicked, this, [this] { edit("confirm"); });
  connect(Unmatch, &QPushButton::clicked, this, [this] { edit("unmatch"); });
  connect(Remap, &QPushButton::clicked, this, [this] { edit("match"); });
  connect(Counterpart, &QPushButton::clicked, this, [this] {
    selectFunction(SelectedSide == "primary" ? "secondary" : "primary",
                   SelectedSide == "primary" ? SecondaryAddress
                                             : PrimaryAddress);
  });
  connect(FunctionTable->selectionModel(),
          &QItemSelectionModel::currentRowChanged, this,
          [this](const QModelIndex &I) {
            const auto R = Functions->row(I.row());
            if (!R.isEmpty())
              selectFunction(Side->currentData().toString(),
                             R["address"].toString());
          });
  connect(InstructionTable, &QTableView::activated, this,
          [this](const QModelIndex &I) {
            const auto R = Instructions->row(I.row());
            const bool Secondary = I.column() > 3;
            const auto Operand =
                R[Secondary ? "secondary" : "primary"].toObject();
            const auto Target = Operand["callee"].toString();
            if (!Target.isEmpty())
              selectFunction(Secondary ? "secondary" : "primary", Target);
          });
  connect(Instructions, &ComparisonModel::pageReady, this,
          &ComparisonDialog::showPage);
  for (auto *M : {Functions, Instructions})
    connect(M, &ComparisonModel::failed, Status, &QLabel::setText);
  Filter.setSingleShot(true);
  Filter.setInterval(limits::kChooserViewFilterDebounceMs);
  connect(Search, &QLineEdit::textChanged, this, [this] { Filter.start(); });
  connect(&Filter, &QTimer::timeout, this, &ComparisonDialog::inventory);
  connect(Side, &QComboBox::currentIndexChanged, this,
          &ComparisonDialog::inventory);
  connect(Kind, &QComboBox::currentIndexChanged, this,
          &ComparisonDialog::inventory);
  connect(&Session, &ComparisonSession::changed, this,
          &ComparisonDialog::update);
  connect(&Session, &ComparisonSession::mappingChanged, this, [this] {
    DetailStopped = false;
    inventory();
    PrimaryAddress.clear();
    SecondaryAddress.clear();
    Instructions->setQuery({});
    PrimaryGraph->showFunction({});
    SecondaryGraph->showFunction({});
    FunctionTitle->setText(tr("Select a function to inspect its counterpart."));
  });
  connect(&Session, &ComparisonSession::failed, Status, &QLabel::setText);
  connect(&Session, &ComparisonSession::saved, this,
          [this](const auto &P) { Status->setText(tr("Saved %1").arg(P)); });
  connect(&Session, &ComparisonSession::progress, this, [this](const auto &P) {
    if (Session.cancelRequested())
      return;
    const auto Phase = P["phase"].toString();
    const auto Done = P["done"].toDouble();
    const auto Total = P["total"].toDouble();
    const bool Bytes = Phase.endsWith(":hash") || Phase.endsWith(":copy") ||
                       Phase == "restore:read" || Phase == "restore:scan" ||
                       Phase == "restore:parse";
    const auto Amount = [&](double Value) {
      return Bytes ? locale().formattedDataSize(qint64(Value))
                   : locale().toString(qulonglong(Value));
    };
    Status->setText(
        Total > 1 ? tr("%1  %2 / %3")
                        .arg(progressLabel(Phase), Amount(Done), Amount(Total))
                  : progressLabel(Phase));
    Progress->setRange(0, Total ? 1000 : 0);
    if (Total)
      Progress->setValue(int(P["done"].toDouble() * 1000 / Total));
  });
  update();
}
void ComparisonDialog::run(const QString &Restore) {
  QJsonObject Inputs[2];
  for (int I = 0; I < 2; ++I) {
    Inputs[I]["path"] = Paths[I]->text();
    if (Loaders[I]->currentIndex())
      Inputs[I]["load_options"] =
          QJsonObject{{"loader", "binary"},
                      {"processor", Loaders[I]->currentData().toString()},
                      {"base", Bases[I]->text()}};
  }
  Status->setText(tr("Loading comparison inputs…"));
  RestoreSettings = !Restore.isEmpty();
  // Saved reports may carry caller-selected search thresholds and allowances.
  // Keep those when rerunning; these controls own only enabled and calls.
  auto NativeRegions =
      Session.summary()["settings"].toObject()["regions"].toObject();
  NativeRegions["enabled"] = AnalyzeRegions->isChecked();
  if (RegionCalls->isChecked())
    NativeRegions["calls"] = true;
  else
    NativeRegions.remove("calls");
  Session.start(
      Inputs[0], Inputs[1],
      {{"use_names", Names->isChecked()},
       {"regions", NativeRegions},
       {"flow_regions",
        QJsonObject{{"enabled", AnalyzeFlowRegions->isChecked()},
                    {"fragments", FlowFragments->isChecked()}}},
       {"dataflow",
        QJsonObject{{"enabled", AnalyzeDataFlow->isChecked()},
                    {"profile", FlowProfile->currentData().toString()},
                    {"max_functions", FlowFunctions->value()}}},
       {"min_similarity", Threshold->value()},
       {"timeout_ms", Timeout->value() * 1000}},
      Restore);
}
void ComparisonDialog::update() {
  // Input signals can arrive while the constructor is still building controls.
  if (!findChild<QPushButton *>("comparisonStart"))
    return;
  const bool Idle = Session.ready() && !Session.busy();
  Start->setEnabled(Idle && !Paths[0]->text().isEmpty() &&
                    !Paths[1]->text().isEmpty());
  Reopen->setEnabled(Start->isEnabled());
  Cancel->setEnabled(Session.cancellable() && !Session.cancelRequested());
  Save->setEnabled(Idle && !Session.revision().isEmpty());
  Regions->setEnabled(Idle && !Session.revision().isEmpty());
  DataFlow->setEnabled(Idle && !Session.revision().isEmpty());
  const auto &S = Session.summary();
  if (RestoreSettings && Idle) {
    RestoreSettings = false;
    const auto Settings = S["settings"].toObject();
    Names->setChecked(Settings["use_names"].toBool());
    AnalyzeRegions->setChecked(
        Settings["regions"].toObject()["enabled"].toBool());
    RegionCalls->setChecked(AnalyzeRegions->isChecked() &&
                            Settings["regions"].toObject()["calls"].toBool());
    const auto Flow = Settings["dataflow"].toObject();
    AnalyzeDataFlow->setChecked(Flow["enabled"].toBool());
    FlowProfile->setCurrentIndex(FlowProfile->findData(
        Flow["profile"].toString(::neverd::limits::diff::kMedSSAProfile)));
    AnalyzeFlowRegions->setChecked(
        Settings["flow_regions"].toObject()["enabled"].toBool());
    FlowFragments->setChecked(
        AnalyzeFlowRegions->isChecked() &&
        Settings["flow_regions"].toObject()["fragments"].toBool());
    FlowFunctions->setValue(Flow["max_functions"].toInt(
        ::neverd::limits::diff::kDefaultFlowFunctions));
    Threshold->setValue(Settings["min_similarity"].toDouble());
  }
  const bool Editable = Idle && !S["stale"].toBool();
  RetryDetail->setEnabled(Editable && DetailStopped && !Session.cancellable());
  Undo->setEnabled(Editable && S["can_undo"].toBool());
  Redo->setEnabled(Editable && S["can_redo"].toBool());
  const bool Pair = !PrimaryAddress.isEmpty() && !SecondaryAddress.isEmpty();
  for (auto *B : {Confirm, Unmatch, Counterpart})
    B->setEnabled(Editable && Pair);
  Changes->setEnabled(Editable && Pair && !Session.cancellable());
  Validate->setEnabled(Editable && Pair && !Session.cancellable());
  Reviews->setEnabled(Editable && !Session.revision().isEmpty() &&
                      !Session.cancellable());
  Remap->setEnabled(Editable &&
                    (!PrimaryAddress.isEmpty() || !SecondaryAddress.isEmpty()));
  Impact->setEnabled(
      Editable && !Session.cancellable() &&
      !(SelectedSide == "primary" ? PrimaryAddress : SecondaryAddress)
           .isEmpty());
  Progress->setVisible(Session.busy());
  if (Session.cancelRequested() && Session.busy())
    Status->setText(
        tr("Cancellation requested — waiting for computation to stop…"));
  else if (!Session.busy() && !S.isEmpty()) {
    const auto Counts = S["counts"].toObject();
    const auto Coverage = S["coverage"].toObject();
    const auto A = Coverage["primary"].toObject(),
               B = Coverage["secondary"].toObject();
    Summary->setText(
        tr("%1 matched  ·  %2 changed  ·  %3 ambiguous  ·  %4 / "
           "%5 unmatched  ·  %6 / %7 incomplete  ·  %8 uncertain")
            .arg(Counts["matches"].toInt())
            .arg(Counts["changed"].toInt())
            .arg(A["ambiguous"].toInt() + B["ambiguous"].toInt())
            .arg(A["unmatched"].toInt())
            .arg(B["unmatched"].toInt())
            .arg(A["incomplete"].toInt())
            .arg(B["incomplete"].toInt())
            .arg(Counts["uncertain_without_observed_change"].toInt()));
    QString Text =
        S["stale"].toBool()
            ? tr("Inputs changed. Reload before continuing review.")
        : DetailStopped
            ? tr("Function difference stopped. Use Retry difference to "
                 "recompute; no completed instruction comparison is shown.")
        : S["state"] == QJsonValue("complete")
            ? tr("Comparison complete. Scores are heuristic; inspect coverage "
                 "and evidence.")
            : tr("Computation stopped: %1. Partial results retained.")
                  .arg(S["state"].toString());
    if (!A["discovery_complete"].toBool() || !B["discovery_complete"].toBool())
      Text += tr(" Function discovery is partial.");
    Status->setText(Text);
  } else if (Idle)
    Status->setText(tr("Ready to compare two native binaries."));
  if (Idle && !Session.operationMessage().isEmpty())
    Status->setText(Session.operationMessage());
}
void ComparisonDialog::inventory() {
  Functions->setQuery({{"side", Side->currentData().toString()},
                       {"kind", Kind->currentData().toString()},
                       {"search", Search->text()}});
}
void ComparisonDialog::showImpact(const QString &Side, const QString &Address) {
  if (!ImpactView) {
    ImpactView = new ComparisonImpactDialog(Session, this);
    connect(ImpactView, &ComparisonImpactDialog::inspectFunction, this,
            [this](const QString &Which, const QString &Entry) {
              selectFunction(Which, Entry);
              raise();
              activateWindow();
            });
  }
  ImpactView->show();
  ImpactView->raise();
  ImpactView->activateWindow();
  ImpactView->trace(Side, Address);
}
void ComparisonDialog::selectFunction(QString Which, QString Address) {
  if (Address.isEmpty() || Session.busy())
    return;
  SelectedSide = Which;
  const bool Refresh = DetailStopped;
  DetailStopped = false;
  PrimaryAddress.clear();
  SecondaryAddress.clear();
  Source->setFunctions({}, {});
  FunctionTitle->setText(tr("Loading %1…").arg(Address));
  Instructions->setQuery(
      {{"side", Which}, {"address", Address}, {"refresh", Refresh}});
  update();
}
void ComparisonDialog::showPage(const QJsonObject &Page) {
  const auto A = Page["primary"].toObject(), B = Page["secondary"].toObject();
  const bool Stopped = !Page["computation_complete"].toBool(true);
  if (PrimaryAddress != A["address"].toString() ||
      SecondaryAddress != B["address"].toString() || DetailStopped || Stopped) {
    PrimaryAddress = A["address"].toString();
    SecondaryAddress = B["address"].toString();
    PrimaryGraph->showFunction(Stopped ? QString() : PrimaryAddress);
    SecondaryGraph->showFunction(Stopped ? QString() : SecondaryAddress);
  }
  DetailStopped = Stopped;
  FunctionTitle->setText(tr("%1 %2  ↔  %3 %4")
                             .arg(A["name"].toString(), PrimaryAddress,
                                  B["name"].toString(), SecondaryAddress));
  if (Stopped)
    FunctionTitle->setText(
        FunctionTitle->text() +
        tr(" (difference %1)").arg(Page["state"].toString().replace('_', ' ')));
  const auto E = Page["evidence"].toObject();
  QString Text =
      E.isEmpty()
          ? tr("No accepted counterpart. Inspect ambiguity candidates or "
               "choose a pair manually.")
          : tr("Match: %1\n%2\n\nSimilarity: %3\nHeuristic confidence: "
               "%4%\nChanges: %5\nAnalyst confirmed: %6\nCoverage: %7")
                .arg(E["step"].toString(), E["explanation"].toString())
                .arg(E["identity_only"].toBool()
                         ? tr("not applicable")
                         : QString::number(E["similarity"].toDouble() * 100,
                                           'f', 1) +
                               "%")
                .arg(E["confidence"].toDouble() * 100, 0, 'f', 1)
                .arg(changeList(E["changes"].toArray()))
                .arg(E["confirmed"].toBool() ? tr("yes") : tr("no"),
                     E["identity_only"].toBool()
                         ? tr("identity only; no bodies available")
                     : E["analysis_complete"].toBool()
                         ? tr("decoded bodies complete")
                         : tr("incomplete"));
  if (!E.isEmpty() && E["measured"].toBool() && !E["identity_only"].toBool() &&
      !E["reference_comparison_complete"].toBool())
    Text += tr("\nReference identities: unresolved. Some call or data targets "
               "cannot be compared. This alone does not establish a change.");
  const auto PrimaryNames = ComparisonModel::functionNamesText(A);
  const auto SecondaryNames = ComparisonModel::functionNamesText(B);
  if (PrimaryNames != A["name"].toString() ||
      SecondaryNames != B["name"].toString())
    Text += tr("\n\nPrimary name: %1\nSecondary name: %2")
                .arg(PrimaryNames, SecondaryNames);
  Text += tr("\n\nPrimary identity: %1\nSecondary identity: %2\n\nStructural "
             "correspondence does not establish semantic equivalence or patch "
             "presence.")
              .arg(A["identity"].toString(), B["identity"].toString());
  if (Stopped)
    Text.prepend(
        tr("Function difference %1. No completed instruction comparison is "
           "available. Use Retry difference to recompute.\n\n")
            .arg(Page["state"].toString().replace('_', ' ')));
  Evidence->setPlainText(Text);
  Source->setFunctions(A, B);
  update();
}
void ComparisonDialog::edit(const QString &Kind) {
  auto A = PrimaryAddress, B = SecondaryAddress;
  if (Kind == "match") {
    const bool NeedPrimary = SelectedSide == "secondary";
    auto *Picker = new ComparisonCounterpartDialog(
        Session, SelectedSide, NeedPrimary ? B : A, NeedPrimary ? A : B, this);
    showModelessDialog(Picker, this);
    return;
  }
  Session.command("comparison_override",
                  {{"kind", Kind}, {"primary", A}, {"secondary", B}});
}
void ComparisonDialog::closeEvent(QCloseEvent *Event) {
  QSettings().setValue("comparison/geometry", saveGeometry());
  QDialog::closeEvent(Event);
}
} // namespace neverd::gui
