//===- ComparisonFlowDialog.cpp - Native dependency evidence review -----===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Paginated immutable function and node records share the comparison worker.
/// Slices preserve control/effect gaps and never certify unchanged behavior.
//===----------------------------------------------------------------------===//

#include "ComparisonFlowDialog.h"

#include "ComparisonModel.h"
#include "ComparisonSession.h"
#include "Icons.h"

#include "neverd/diff/Limits.h"

#include <QCheckBox>
#include <QComboBox>
#include <QHeaderView>
#include <QItemSelectionModel>
#include <QLabel>
#include <QPushButton>
#include <QSplitter>
#include <QTableView>
#include <QVBoxLayout>

namespace neverd::gui {
namespace {
QLabel *label(QWidget *Parent, const QString &Text) {
  auto *Out = new QLabel(Text, Parent);
  Out->setWordWrap(true);
  Out->setTextInteractionFlags(Qt::TextSelectableByMouse);
  return Out;
}
QTableView *table(QWidget *Parent, ComparisonModel *Model, const char *Name) {
  auto *View = new QTableView(Parent);
  View->setObjectName(Name);
  View->setModel(Model);
  View->ensurePolished();
  View->setSelectionBehavior(QAbstractItemView::SelectRows);
  View->setSelectionMode(QAbstractItemView::SingleSelection);
  View->setEditTriggers(QAbstractItemView::NoEditTriggers);
  View->setAlternatingRowColors(true);
  View->setWordWrap(false);
  View->verticalHeader()->hide();
  View->horizontalHeader()->setStretchLastSection(true);
  for (int I = 0; I < Model->columnCount(); ++I)
    View->setColumnWidth(
        I,
        View->fontMetrics().horizontalAdvance(
            Model->headerData(I, Qt::Horizontal, Qt::DisplayRole).toString()) +
            28);
  return View;
}
} // namespace

ComparisonFlowDialog::ComparisonFlowDialog(ComparisonSession &S,
                                           QWidget *Parent)
    : QDialog(Parent), Session(S) {
  setObjectName("comparisonFlowDialog");
  setWindowTitle(tr("Dependency evidence — NeverD"));
  setWindowIcon(icon("compare"));
  resize(1280, 800);
  setMinimumSize(920, 580);
  auto *Root = new QVBoxLayout(this);
  Summary =
      label(this, tr("Enable dependency evidence in comparison settings."));
  Summary->setObjectName("flowSummary");
  Root->addWidget(Summary);
  auto *Split = new QSplitter(Qt::Horizontal, this);
  Split->setChildrenCollapsible(false);
  Root->addWidget(Split, 1);
  auto *Left = new QWidget(Split);
  auto *LeftLayout = new QVBoxLayout(Left);
  LeftLayout->setContentsMargins(0, 0, 0, 0);
  Side = new QComboBox(Left);
  Side->setObjectName("flowSide");
  Side->addItem(tr("Primary input"), "primary");
  Side->addItem(tr("Secondary input"), "secondary");
  LeftLayout->addWidget(Side);
  Functions =
      new ComparisonModel(Session, ComparisonModel::Kind::FlowFunctions, this);
  FunctionTable = table(Left, Functions, "flowFunctions");
  for (int Column : {2, 3, 4, 5})
    FunctionTable->hideColumn(Column);
  FunctionTable->horizontalHeader()->setSectionResizeMode(1,
                                                          QHeaderView::Stretch);
  LeftLayout->addWidget(FunctionTable, 1);
  auto *Right = new QSplitter(Qt::Vertical, Split);
  Right->setChildrenCollapsible(false);
  auto *Top = new QWidget(Right);
  auto *TopLayout = new QVBoxLayout(Top);
  TopLayout->setContentsMargins(0, 0, 0, 0);
  FunctionStatus =
      label(Top, tr("Select a function to inspect its SSA operations."));
  FunctionStatus->setObjectName("flowFunctionStatus");
  TopLayout->addWidget(FunctionStatus);
  Nodes = new ComparisonModel(Session, ComparisonModel::Kind::FlowNodes, this);
  NodeTable = table(Top, Nodes, "flowNodes");
  NodeTable->setColumnWidth(2, 145);
  NodeTable->horizontalHeader()->setStretchLastSection(false);
  NodeTable->horizontalHeader()->setSectionResizeMode(4, QHeaderView::Stretch);
  NodeTable->horizontalHeader()->setResizeContentsPrecision(0);
  NodeTable->horizontalHeader()->setSectionResizeMode(
      5, QHeaderView::ResizeToContents);
  TopLayout->addWidget(NodeTable, 1);
  LoopStatus = label(Top, {});
  LoopStatus->setObjectName("flowLoopStatus");
  LoopStatus->hide();
  TopLayout->addWidget(LoopStatus);
  auto *Actions = new QHBoxLayout;
  Backward = new QPushButton(tr("Trace inputs"), Top);
  Backward->setObjectName("flowBackward");
  Forward = new QPushButton(tr("Trace uses"), Top);
  Forward->setObjectName("flowForward");
  ArgumentAccesses = new QPushButton(tr("Argument accesses"), Top);
  ArgumentAccesses->setObjectName("flowArgumentAccesses");
  ArgumentAccesses->setToolTip(
      tr("Show reads and writes using the same computed address as a recovered "
         "call argument."));
  Control = new QCheckBox(tr("Include control"), Top);
  Control->setToolTip(
      tr("Include control dependencies when tracing inputs or uses."));
  Control->setChecked(true);
  Cancel = new QPushButton(tr("Cancel"), Top);
  Cancel->setObjectName("flowCancel");
  for (auto *B : {Backward, Forward, ArgumentAccesses})
    Actions->addWidget(B);
  Actions->addWidget(Control);
  Actions->addStretch();
  Actions->addWidget(Cancel);
  TopLayout->addLayout(Actions);
  auto *Bottom = new QWidget(Right);
  auto *BottomLayout = new QVBoxLayout(Bottom);
  BottomLayout->setContentsMargins(0, 0, 0, 0);
  SliceStatus =
      label(Bottom, tr("Select an operation, then trace its inputs or uses."));
  SliceStatus->setObjectName("flowSliceStatus");
  BottomLayout->addWidget(SliceStatus);
  Slice = new ComparisonModel(Session, ComparisonModel::Kind::FlowSlice, this);
  Accesses =
      new ComparisonModel(Session, ComparisonModel::Kind::FlowAccesses, this);
  SliceTable = table(Bottom, Slice, "flowSlice");
  SliceTable->setColumnWidth(2, 145);
  SliceTable->horizontalHeader()->setStretchLastSection(false);
  SliceTable->horizontalHeader()->setSectionResizeMode(
      0, QHeaderView::ResizeToContents);
  SliceTable->horizontalHeader()->setSectionResizeMode(4, QHeaderView::Stretch);
  SliceTable->horizontalHeader()->setResizeContentsPrecision(0);
  SliceTable->horizontalHeader()->setSectionResizeMode(
      5, QHeaderView::ResizeToContents);
  BottomLayout->addWidget(SliceTable, 1);
  Inspect = new QPushButton(tr("Inspect original function"), Bottom);
  Inspect->setObjectName("flowInspect");
  // Enter activates the selected evidence row, without also tracing a slice.
  for (auto *Button : {Backward, Forward, ArgumentAccesses, Cancel, Inspect})
    Button->setAutoDefault(false);
  BottomLayout->addWidget(Inspect, 0, Qt::AlignLeft);
  Split->setStretchFactor(0, 1);
  Split->setStretchFactor(1, 3);
  Split->setSizes({270, 960});
  Root->addWidget(
      label(this, tr("Observed SSA dependencies. Calls, aliases and unresolved "
                     "control flow remain explicit gaps. These observations do "
                     "not prove whole-function equivalence.")));
  connect(Side, &QComboBox::currentIndexChanged, this,
          &ComparisonFlowDialog::selectSide);
  connect(&Session, &ComparisonSession::changed, this,
          &ComparisonFlowDialog::refresh);
  for (auto *Model : {Functions, Nodes, Slice, Accesses})
    connect(Model, &ComparisonModel::failed, this,
            [this](const QString &Error) { SliceStatus->setText(Error); });
  connect(Functions, &ComparisonModel::pageReady, this, [this] {
    if (Functions->rowCount() && !FunctionTable->currentIndex().isValid())
      FunctionTable->setCurrentIndex(FunctionTable->model()->index(0, 0));
  });
  connect(FunctionTable->selectionModel(),
          &QItemSelectionModel::currentRowChanged, this,
          [this](const QModelIndex &Index) {
            const auto Row = Functions->row(Index.row());
            Address = Row["function"].toString();
            Slice->clear();
            Accesses->clear();
            AccessesStopped = false;
            SliceStatus->setText(
                tr("Select an operation, then trace its inputs or uses."));
            if (Address.isEmpty())
              Nodes->clear();
            else
              Nodes->setQuery({{"side", Side->currentData().toString()},
                               {"kind", "nodes"},
                               {"address", Address}});
            controls();
          });
  connect(
      Nodes, &ComparisonModel::pageReady, this,
      [this](const QJsonObject &Page) {
        const auto Function = Page["function_info"].toObject();
        if (Page["availability"] != QJsonValue("available"))
          FunctionStatus->setText(tr("%1 · %2\n%3")
                                      .arg(Address,
                                           Page["availability"].toString(),
                                           Page["reason"].toString()));
        else
          FunctionStatus->setText(
              tr("%1 · %2 · %3 SSA operations\nSelect an operation to follow "
                 "dependencies. Synthetic operations have block provenance.")
                  .arg(Function["name"].toString(), Address)
                  .arg(Page["total"].toInt()));
        FunctionStatus->setToolTip({});
        const auto Progress = Page["progress"].toObject();
        if (!Progress.isEmpty()) {
          const auto Kind = Progress["kind"].toString();
          const auto Status =
              Kind == "acyclic"
                  ? tr("Return progress proved: acyclic control flow.")
              : Kind == "counter_loops" && Progress.contains("guard_count")
                  ? tr("Return progress proved · Counter loops: %1 · "
                       "Backedges: %2 · Guards: %3")
                        .arg(Progress["loop_count"].toInt())
                        .arg(Progress["backedge_count"].toInt())
                        .arg(Progress["guard_count"].toInt())
              : Kind == "counter_loops"
                  ? tr("Return progress proved · Counter loops: %1")
                        .arg(Progress["loop_count"].toInt())
                  : tr("Return progress unproved: original contracts "
                       "retained.");
          FunctionStatus->setText(FunctionStatus->text() + '\n' + Status);
          FunctionStatus->setToolTip(
              "<qt>" +
              tr("Source contract:\n%1\nObserved contract:\n%2\n"
                 "Progress does not establish equal loop counts or function "
                 "results.")
                  .arg(Progress["source_contract"].toString(),
                       Progress["observed_contract"].toString())
                  .toHtmlEscaped()
                  .replace('\n', "<br>") +
              "</qt>");
        }
        if (Nodes->rowCount() && !NodeTable->currentIndex().isValid())
          NodeTable->setCurrentIndex(NodeTable->model()->index(0, 0));
        controls();
      });
  connect(NodeTable->selectionModel(), &QItemSelectionModel::currentRowChanged,
          this, [this] { controls(); });
  connect(Slice, &ComparisonModel::pageReady, this,
          [this](const QJsonObject &Page) {
            QStringList Gaps;
            for (const auto &V : Page["gaps"].toArray())
              Gaps << V.toString().replace('_', ' ');
            SliceStatus->setText(
                tr("%1 slice · %2 nodes · %3\nGaps: %4")
                    .arg(Page["direction"].toString())
                    .arg(Page["total"].toInt())
                    .arg(Page["state"].toString().replace('_', ' '),
                         Gaps.isEmpty() ? tr("none observed within this model")
                                        : Gaps.join(", ")));
          });
  connect(Accesses, &ComparisonModel::pageReady, this,
          [this](const QJsonObject &Page) {
            AccessesStopped = Page["state"] != QJsonValue("complete");
            SliceStatus->setText(
                tr("Call n%1 · %2 address observations · %3\n"
                   "Reads and writes at a recovered argument's address. "
                   "Call effects and execution order remain unknown. "
                   "Activate a row to select its operation.")
                    .arg(Page["call"].toObject()["id"].toInt())
                    .arg(Page["total"].toInt())
                    .arg(Page["state"].toString().replace('_', ' ')));
          });
  connect(ArgumentAccesses, &QPushButton::clicked, this, [this] {
    const auto Call = Nodes->row(NodeTable->currentIndex().row());
    if (Call.isEmpty() || Address.isEmpty())
      return;
    Slice->clear();
    SliceTable->setModel(Accesses);
    SliceStatus->setText(tr("Inspecting argument addresses…"));
    Accesses->setQuery({{"side", Side->currentData().toString()},
                        {"kind", "accesses"},
                        {"address", Address},
                        {"call", Call["id"]},
                        {"refresh", AccessesStopped}});
  });
  connect(SliceTable, &QTableView::activated, this,
          [this](const QModelIndex &Index) {
            if (SliceTable->model() != Accesses)
              return;
            const auto Row = Accesses->row(Index.row());
            if (Row.isEmpty())
              return;
            const auto Node = Nodes->index(Row["id"].toInt(), 0);
            NodeTable->setCurrentIndex(Node);
            NodeTable->scrollTo(Node);
          });
  connect(Backward, &QPushButton::clicked, this, [this] { slice("backward"); });
  connect(Forward, &QPushButton::clicked, this, [this] { slice("forward"); });
  connect(Cancel, &QPushButton::clicked, &Session, &ComparisonSession::cancel);
  connect(Inspect, &QPushButton::clicked, this, [this] {
    emit inspectFunction(Side->currentData().toString(), Address);
  });
  refresh();
}
void ComparisonFlowDialog::controls() {
  const auto Loop =
      Nodes->row(NodeTable->currentIndex().row())["progress_loop"].toObject();
  LoopStatus->setVisible(!Loop.isEmpty());
  LoopStatus->setText(
      Loop.isEmpty() ? QString()
      : Loop.contains("latch")
          ? tr("Proved loop: header block %1 → latch block %2 · "
               "counter #%3 → update #%4 · guard #%5 in block %6")
                .arg(Loop["header"].toInt())
                .arg(Loop["latch"].toInt())
                .arg(Loop["counter"].toInt())
                .arg(Loop["update"].toInt())
                .arg(Loop["compare"].toInt())
                .arg(Loop["block"].toInt())
          : tr("Proved loop: header block %1 → latch block %2 · "
               "counter #%3 → update #%4 · guard #%5")
                .arg(Loop.contains("header") ? Loop["header"].toInt()
                                             : Loop["block"].toInt())
                .arg(Loop["block"].toInt())
                .arg(Loop["counter"].toInt())
                .arg(Loop["update"].toInt())
                .arg(Loop["compare"].toInt()));
  if (Loop["proof"] == "nowrap")
    LoopStatus->setText(
        LoopStatus->text() + '\n' +
        tr("Proof: a monotone counter with a no-overflow contract."));
  else if (Loop["proof"] == "modular")
    LoopStatus->setText(
        LoopStatus->text() + '\n' +
        tr("Proof: every counter cycle reaches a shared exit, including after "
           "wraparound."));
  else if (Loop["proof"] == "parameter_modular")
    LoopStatus->setText(
        LoopStatus->text() + '\n' +
        tr("Proof: every counter cycle reaches a shared exit relative to an "
           "input parameter. Undefined behavior may occur before exit."));

  else if (Loop["proof"] == "invariant_modular")
    LoopStatus->setText(
        LoopStatus->text() + '\n' +
        tr("Proof: every counter cycle reaches a shared exit relative to an "
           "input-derived expression. Undefined behavior may occur before "
           "exit."));

  const bool Node = !Nodes->row(NodeTable->currentIndex().row()).isEmpty();
  const bool Idle =
      Session.ready() && !Session.busy() && !Session.cancellable();
  Backward->setEnabled(Idle && Node);
  Forward->setEnabled(Idle && Node);
  const auto Selected = Nodes->row(NodeTable->currentIndex().row());
  const auto Profile = Session.summary()["settings"]
                           .toObject()["dataflow"]
                           .toObject()["profile"]
                           .toString(::neverd::limits::diff::kMedSSAProfile);
  ArgumentAccesses->setEnabled(
      Idle && Profile == ::neverd::limits::diff::kMedSSAProfile &&
      (Selected["opcode"] == QJsonValue("CALL") ||
       Selected["opcode"] == QJsonValue("INDIR_CALL")) &&
      !Selected["call_argument_begin"].isNull() &&
      Selected["call_argument_count"].toInt() > 0);
  Inspect->setEnabled(Idle && !Address.isEmpty());
  Cancel->setEnabled(Session.cancellable() && !Session.cancelRequested());
}
void ComparisonFlowDialog::refresh() {
  const bool WasRequested = Requested;
  Requested = Session.summary()["settings"]
                  .toObject()["dataflow"]
                  .toObject()["enabled"]
                  .toBool();
  if (Revision != Session.revision() || Requested != WasRequested) {
    Revision = Session.revision();
    selectSide();
  }
  controls();
}
void ComparisonFlowDialog::selectSide() {
  Address.clear();
  Functions->clear();
  Nodes->clear();
  Slice->clear();
  Accesses->clear();
  AccessesStopped = false;
  FunctionStatus->setText(
      tr("Select a function to inspect its SSA operations."));
  SliceStatus->setText(
      tr("Select an operation, then trace its inputs or uses."));
  if (!Requested || Revision.isEmpty()) {
    Summary->setText(tr("Enable dependency evidence in comparison settings, "
                        "then compare the inputs."));
  } else {
    const auto SideName = Side->currentData().toString();
    const auto E =
        Session.summary()["dataflow"].toObject()[SideName].toObject();
    const auto Profile = Session.summary()["settings"]
                             .toObject()["dataflow"]
                             .toObject()["profile"]
                             .toString(::neverd::limits::diff::kMedSSAProfile);
    Summary->setText(tr("%1 · %2 available / %3 processed / %4 eligible "
                        "functions · %5 unprocessed\n%6 unsupported, %7 "
                        "resource limited, %8 failed. Collection state: %9.")
                         .arg(Side->currentText())
                         .arg(E["available"].toInt())
                         .arg(E["processed_functions"].toInt())
                         .arg(E["eligible_functions"].toInt())
                         .arg(E["unprocessed_functions"].toInt())
                         .arg(E["unsupported"].toInt())
                         .arg(E["resource_limited"].toInt())
                         .arg(E["failed"].toInt())
                         .arg(E["state"].toString().replace('_', ' ')));
    Summary->setText(
        Summary->text() + '\n' +
        tr("SSA representation: %1")
            .arg(Profile == ::neverd::limits::diff::
                                kLLVMInvariantBoundProgressSSAProfile
                     ? tr("Optimized SSA with expression-bound loop analysis")
                 : Profile == ::neverd::limits::diff::
                                  kLLVMParameterBoundProgressSSAProfile
                     ? tr("Optimized SSA with parameter-bound loop analysis")
                 : Profile ==
                         ::neverd::limits::diff::kLLVMModularProgressSSAProfile
                     ? tr("Optimized SSA with wrapping loop analysis")
                 : Profile ==
                         ::neverd::limits::diff::kLLVMBranchProgressSSAProfile
                     ? tr("Optimized SSA with branching loop analysis")
                 : Profile ==
                         ::neverd::limits::diff::kLLVMNestedProgressSSAProfile
                     ? tr("Optimized SSA with nested loop analysis")
                 : Profile == ::neverd::limits::diff::
                                  kLLVMMultiBlockProgressSSAProfile
                     ? tr("Optimized SSA with multi-block loop analysis")
                 : Profile == ::neverd::limits::diff::kLLVMProgressSSAProfile
                     ? tr("Optimized SSA with loop analysis")
                 : Profile == ::neverd::limits::diff::kLLVMParameterSSAProfile
                     ? tr("Optimized SSA with parameter views")
                 : Profile == ::neverd::limits::diff::kLLVMSSAProfile
                     ? tr("Optimized SSA")
                     : tr("Original SSA")));
    Functions->setQuery({{"side", SideName}, {"kind", "functions"}});
  }
  controls();
}
void ComparisonFlowDialog::slice(const QString &Direction) {
  Accesses->clear();
  SliceTable->setModel(Slice);
  const auto Node = Nodes->row(NodeTable->currentIndex().row());
  if (Node.isEmpty() || Address.isEmpty())
    return;
  SliceStatus->setText(tr("Tracing %1 dependencies…").arg(Direction));
  Slice->setQuery({{"side", Side->currentData().toString()},
                   {"address", Address},
                   {"seeds", QJsonArray{Node["id"]}},
                   {"direction", Direction},
                   {"control", Control->isChecked()},
                   {"refresh", true}});
}
} // namespace neverd::gui
