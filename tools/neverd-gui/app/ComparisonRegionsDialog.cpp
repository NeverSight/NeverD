//===- ComparisonRegionsDialog.cpp - Overlapping region review ------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// A separate review surface keeps transformation hypotheses visibly distinct
/// from accepted function pairs while sharing the worker and bounded models.
//===----------------------------------------------------------------------===//

#include "ComparisonRegionsDialog.h"

#include "../ResourceLimits.h"
#include "ComparisonModel.h"
#include "ComparisonSession.h"
#include "Icons.h"

#include <QAction>
#include <QComboBox>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QItemSelectionModel>
#include <QLabel>
#include <QMenu>
#include <QPushButton>
#include <QSignalBlocker>
#include <QSplitter>
#include <QTableView>
#include <QVBoxLayout>
#include <utility>

namespace neverd::gui {
namespace {
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
  View->setShowGrid(false);
  View->verticalHeader()->hide();
  View->verticalHeader()->setDefaultSectionSize(View->fontMetrics().height() +
                                                8);
  View->horizontalHeader()->setStretchLastSection(true);
  View->horizontalHeader()->setDefaultAlignment(Qt::AlignLeft |
                                                Qt::AlignVCenter);
  for (int Column = 0; Column < Model->columnCount(); ++Column)
    View->setColumnWidth(
        Column, View->horizontalHeader()->fontMetrics().horizontalAdvance(
                    Model->headerData(Column, Qt::Horizontal, Qt::DisplayRole)
                        .toString()) +
                    24);
  QObject::connect(Model, &ComparisonModel::pageReady, View, [View, Model] {
    if (!View->currentIndex().isValid() && Model->rowCount())
      View->setCurrentIndex(Model->index(0, 0));
  });
  return View;
}
QLabel *label(QWidget *Parent, const QString &Text = {}) {
  auto *L = new QLabel(Text, Parent);
  L->setWordWrap(true);
  L->setTextFormat(Qt::PlainText);
  L->setTextInteractionFlags(Qt::TextSelectableByMouse);
  return L;
}
} // namespace
ComparisonRegionsDialog::ComparisonRegionsDialog(ComparisonSession &S,
                                                 QWidget *Parent, bool SSA)
    : QDialog(Parent), Session(S), Flow(SSA) {
  setObjectName(Flow ? "comparisonFlowRegions" : "comparisonRegions");
  setWindowTitle(Flow ? tr("SSA region hypotheses — Binary comparison")
                      : tr("Region hypotheses — Binary comparison"));
  setWindowIcon(icon("compare"));
  setAttribute(Qt::WA_DeleteOnClose);
  resize(::neverd::limits::gui::kComparisonInitialWidth,
         ::neverd::limits::gui::kComparisonInitialHeight);
  setMinimumSize(::neverd::limits::gui::kComparisonMinimumWidth,
                 ::neverd::limits::gui::kComparisonMinimumHeight);
  auto *Layout = new QVBoxLayout(this);
  Layout->addWidget(
      label(this, Flow ? tr("Verified finite SSA neighborhoods suggest "
                            "possible transformations. "
                            "Review gaps and open dependencies; correspondence "
                            "does not validate behavior or patch presence.")
                       : tr("Shared native instruction runs suggest possible "
                            "transformations. "
                            "Review competing explanations and uncovered code; "
                            "these results do "
                            "not validate behavior or patch presence.")));
  Summary = label(this);
  Summary->setObjectName("regionSummary");
  Layout->addWidget(Summary);
  auto *Split = new QSplitter(this);
  Split->setChildrenCollapsible(false);
  Layout->addWidget(Split, 1);
  auto *Left = new QWidget(Split);
  auto *LeftLayout = new QVBoxLayout(Left);
  LeftLayout->setContentsMargins(0, 0, 0, 0);
  Kind = new QComboBox(this);
  Kind->setObjectName("regionKind");
  Kind->setAccessibleName(tr("Possible transformation"));
  for (const auto &[Name, Key] : {std::pair{tr("All hypotheses"), "all"},
                                  {tr("Split"), "split"},
                                  {tr("Merge"), "merge"},
                                  {tr("Inline"), "inline"},
                                  {tr("Outline"), "outline"},
                                  {tr("Clone"), "clone"},
                                  {tr("Shared tail"), "tail_merge"},
                                  {tr("Module movement"), "module_move"},
                                  {tr("Redistribution"), "redistribution"},
                                  {tr("Shared region"), "shared_region"}})
    Kind->addItem(Name, Key);
  Kind->addItem(tr("All shared spans (including unclassified)"), "raw_anchors");
  Layout->insertWidget(Layout->indexOf(Split), Kind);
  Hypotheses = new ComparisonModel(Session,
                                   Flow ? ComparisonModel::Kind::FlowRegions
                                        : ComparisonModel::Kind::Regions,
                                   this);
  HypothesisTable = table(Left, Hypotheses, "regionHypotheses");
  HypothesisTable->setColumnWidth(0, 155);
  HypothesisTable->setColumnHidden(3, true);
  HypothesisTable->setColumnHidden(4, true);
  LeftLayout->addWidget(HypothesisTable, 1);
  auto *Right = new QSplitter(Qt::Vertical, Split);
  Right->setChildrenCollapsible(false);
  auto *AnchorPane = new QWidget(Right);
  auto *AnchorLayout = new QVBoxLayout(AnchorPane);
  AnchorLayout->setContentsMargins(0, 0, 0, 0);
  Evidence =
      label(AnchorPane, tr("Select a hypothesis to inspect its shared spans."));
  AnchorLayout->addWidget(Evidence);
  Anchors = new ComparisonModel(Session,
                                Flow ? ComparisonModel::Kind::FlowRegionAnchors
                                     : ComparisonModel::Kind::RegionAnchors,
                                this);
  AnchorTable = table(AnchorPane, Anchors, "regionAnchors");
  AnchorTable->setColumnWidth(1, 155);
  AnchorTable->setColumnWidth(3, 155);
  AnchorLayout->addWidget(AnchorTable, 1);
  auto *Navigation = new QHBoxLayout;
  for (const auto &[Name, Side] :
       {std::pair{tr("Inspect primary function"), "primary"},
        {tr("Inspect secondary function"), "secondary"}}) {
    auto *Button = new QPushButton(Name, AnchorPane);
    Navigation->addWidget(Button);
    connect(Button, &QPushButton::clicked, this, [this, Side] {
      const auto Address = anchorFunction(Side);
      if (!Address.isEmpty())
        emit inspectFunction(Side, Address);
    });
  }
  CallContext = new QPushButton(tr("Call context…"), AnchorPane);
  CallContext->setObjectName("regionCallContext");
  auto *CallMenu = new QMenu(CallContext);
  CallContext->setMenu(CallMenu);
  for (const auto &[Name, Side] :
       {std::pair{tr("Primary"), "primary"}, {tr("Secondary"), "secondary"}})
    connect(CallMenu->addAction(Name), &QAction::triggered, this, [this, Side] {
      const auto Address = anchorFunction(Side);
      if (!Address.isEmpty())
        emit inspectCallContext(Side, Address);
    });
  Navigation->addWidget(CallContext);
  Navigation->addStretch();
  AnchorLayout->addLayout(Navigation);
  Instructions =
      new ComparisonModel(Session,
                          Flow ? ComparisonModel::Kind::FlowRegionNodes
                               : ComparisonModel::Kind::Instructions,
                          this);
  auto *NodePane = new QWidget(Right);
  auto *NodeLayout = new QVBoxLayout(NodePane);
  NodeLayout->setContentsMargins(0, 0, 0, 0);
  if (Flow) {
    Coordinates = new QComboBox(NodePane);
    Coordinates->setObjectName("flowRegionCoordinates");
    Coordinates->setAccessibleName(tr("Fragment evidence"));
    Coordinates->addItem(tr("Matched operations"), "nodes");
    Coordinates->addItem(tr("External inputs"), "frontier");
    Coordinates->setVisible(false);
    NodeLayout->addWidget(Coordinates);
    FragmentScope = label(NodePane);
    FragmentScope->setObjectName("flowFragmentScope");
    FragmentScope->setVisible(false);
    NodeLayout->addWidget(FragmentScope);
    connect(Coordinates, &QComboBox::currentIndexChanged, this,
            &ComparisonRegionsDialog::selectAnchor);
  }
  InstructionTable = table(NodePane, Instructions, "regionInstructions");
  NodeLayout->addWidget(InstructionTable, 1);
  if (Flow) {
    InstructionTable->setColumnWidth(2, 230);
    InstructionTable->setColumnWidth(5, 230);
  } else {
    InstructionTable->setColumnWidth(2, 170);
    InstructionTable->setColumnWidth(4, 170);
    InstructionTable->setColumnWidth(1, 85);
    InstructionTable->setColumnWidth(5, 85);
    InstructionTable->setColumnWidth(
        3, InstructionTable->fontMetrics().horizontalAdvance("unchanged") + 24);
  }
  Right->setSizes({250, 400});
  Split->setSizes({400, 850});
  connect(Kind, &QComboBox::currentIndexChanged, this,
          &ComparisonRegionsDialog::filter);
  connect(HypothesisTable->selectionModel(),
          &QItemSelectionModel::currentChanged, this,
          &ComparisonRegionsDialog::selectHypothesis);
  // Selecting an unloaded row starts a page request. Its arrival does not
  // change the selection, so resolve the selected evidence again then.
  connect(Hypotheses, &ComparisonModel::pageReady, this,
          &ComparisonRegionsDialog::selectHypothesis);
  connect(AnchorTable->selectionModel(), &QItemSelectionModel::currentChanged,
          this, &ComparisonRegionsDialog::selectAnchor);
  connect(Anchors, &ComparisonModel::pageReady, this,
          &ComparisonRegionsDialog::selectAnchor);
  connect(Anchors, &ComparisonModel::pageReady, this, [this](const auto &Page) {
    const auto H = Page["hypothesis"].toObject();
    if (H.isEmpty()) {
      Evidence->setText(
          (Flow
               ? tr("%1 verified SSA neighborhoods, including unclassified "
                    "groups. "
                    "Select one for paired nodes; hover for operands and "
                    "original bytes.")
               : tr("%1 shared spans. These include unfinished or unclassified "
                    "candidate groups. "
                    "Select a span to inspect its original instructions."))
              .arg(Page["total"].toInt()));
      return;
    }
    Evidence->setText(
        (Flow ? tr("%1 primary / %2 secondary functions · %3 verified "
                   "neighborhoods\n"
                   "Coverage: %4% / %5% of observed SSA nodes in these "
                   "functions. "
                   "Control and external effects remain open; hover for "
                   "original bytes.")
         : Page["analysis"].toObject()["call_evidence"] ==
                 "current_function_correspondence"
             ? tr("%1 primary / %2 secondary functions · %3 shared spans\n"
                  "Coverage: %4% / %5% of eligible instructions, including "
                  "paired direct calls. Function pairs condition this "
                  "evidence; behavior is not validated.")
             : tr("%1 primary / %2 secondary functions · %3 shared spans\n"
                  "Coverage: %4% / %5% of decoded non-transfer instructions. "
                  "Calls "
                  "and control transfers need separate review."))
            .arg(H["primary_function_count"].toInt())
            .arg(H["secondary_function_count"].toInt())
            .arg(H["anchor_count"].toInt())
            .arg(H["primary_coverage"].toDouble() * 100, 0, 'f', 1)
            .arg(H["secondary_coverage"].toDouble() * 100, 0, 'f', 1));
  });
  for (auto *Model : {Hypotheses, Anchors, Instructions})
    connect(Model, &ComparisonModel::failed, Summary, &QLabel::setText);
  connect(&Session, &ComparisonSession::changed, this,
          &ComparisonRegionsDialog::refresh);
  refresh();
}
QString ComparisonRegionsDialog::anchorFunction(const QString &Side) const {
  const auto A = Anchors->row(AnchorTable->currentIndex().row());
  return Flow ? A[Side + "_function"].toString()
              : A[Side].toObject()["function"].toString();
}
void ComparisonRegionsDialog::selectHypothesis() {
  if (Kind->currentData().toString() == "raw_anchors")
    return;
  const auto Id =
      Hypotheses->row(HypothesisTable->currentIndex().row())["id"].toString();
  if (Id == RegionId)
    return;
  RegionId = Id;
  Anchors->setQuery(RegionId.isEmpty() ? QJsonObject{}
                                       : QJsonObject{{"region_id", RegionId}});
  selectAnchor();
}
void ComparisonRegionsDialog::selectAnchor() {
  const auto A = Anchors->row(AnchorTable->currentIndex().row());
  CallContext->setEnabled(!A.isEmpty() && !Session.busy() &&
                          !Session.summary()["stale"].toBool());
  QJsonObject Query;
  if (!A.isEmpty())
    Query = {{"region_id", RegionId}, {"anchor", A["anchor"]}};
  if (Coordinates) {
    const bool Fragment = A["depth"].toInt() > 0;
    Coordinates->setVisible(Fragment);
    FragmentScope->setVisible(Fragment);
    if (!Fragment) {
      const QSignalBlocker Block(Coordinates);
      Coordinates->setCurrentIndex(0);
      FragmentScope->clear();
    } else {
      Query["coordinates"] = Coordinates->currentData().toString();
      FragmentScope->setText(
          tr("Depth %1 · Matched operations: %2 · External inputs: %3\n"
             "External input producers are not compared or counted as shared "
             "code.")
              .arg(A["depth"].toInt())
              .arg(A["node_count"].toInt())
              .arg(A["frontier_count"].toInt()));
    }
  }
  if (Query != InstructionQuery) {
    InstructionQuery = Query;
    Instructions->setQuery(Query);
  }
}
void ComparisonRegionsDialog::refresh() {
  const auto &S = Session.summary();
  const bool Usable = !Session.busy() && !S["stale"].toBool();
  CallContext->setEnabled(Usable && !anchorFunction("primary").isEmpty());
  for (auto *View : {HypothesisTable, AnchorTable, InstructionTable})
    View->setEnabled(Usable);
  HypothesisTable->setEnabled(Usable &&
                              Kind->currentData().toString() != "raw_anchors");
  Kind->setEnabled(Usable);
  if (Coordinates)
    Coordinates->setEnabled(Usable);
  if (!Usable) {
    Summary->setText(Session.busy()
                         ? tr("Waiting for comparison to finish…")
                         : tr("Inputs changed. Reload the comparison."));
    return;
  }
  const auto R = S[Flow ? "flow_regions" : "regions"].toObject();
  if (!R["state"].isString()) {
    Revision = Session.revision();
    RegionId.clear();
    Kind->setEnabled(false);
    for (auto *Model : {Hypotheses, Anchors, Instructions})
      Model->clear();
    for (auto *View : {HypothesisTable, AnchorTable, InstructionTable})
      View->setEnabled(false);
    Evidence->clear();
    selectAnchor();
    Summary->setText(Flow ? tr("Enable SSA region hypotheses in comparison "
                               "Settings, then compare again.")
                          : tr("Enable region analysis in comparison Settings, "
                               "then compare again."));
    return;
  }
  Summary->setText(
      R["state"].isString()
          ? tr("%1 hypotheses · %2 anchors · %3 frequent seed groups skipped · "
               "%4 oversized groups suppressed · %5 weak candidates skipped · "
               "%6")
                .arg(R["hypothesis_count"].toInt())
                .arg(R["anchor_count"].toInt())
                .arg(R["frequent_seed_groups"].toInt())
                .arg(R["suppressed_groups"].toInt())
                .arg(R["weak_candidates"].toInt())
                .arg(R["state"].toString().replace('_', ' '))
          : tr("Enable region analysis in comparison Settings, then compare "
               "again."));
  if (!Flow && R["call_evidence"] == "current_function_correspondence")
    Summary->setText(
        Summary->text() +
        tr("\nCalls use current function pairs; behavior is not validated."));
  if (Flow)
    Summary->setText(
        Summary->text() +
        tr("\nDependency scope: %1 / %2 · %3 ambiguous candidates · %4 graphs "
           "exceeded analysis allowance")
            .arg(R["primary_scope_complete"].toBool() ? tr("complete")
                                                      : tr("partial"),
                 R["secondary_scope_complete"].toBool() ? tr("complete")
                                                        : tr("partial"))
            .arg(R["ambiguous_candidates"].toInt())
            .arg(R["limited_graphs"].toInt()) +
        tr("\nSSA recipe: %1").arg(R["profile"].toString()));
  if (Revision != Session.revision()) {
    Revision = Session.revision();
    if (!R["hypothesis_count"].toInt() && R["anchor_count"].toInt())
      Kind->setCurrentIndex(Kind->findData("raw_anchors"));
    filter();
  }
}
void ComparisonRegionsDialog::filter() {
  const bool Raw = Kind->currentData().toString() == "raw_anchors";
  RegionId.clear();
  InstructionQuery = {};
  HypothesisTable->setEnabled(!Session.busy() && !Raw);
  HypothesisTable->parentWidget()->setVisible(!Raw);
  if (Raw) {
    Hypotheses->clear();
    Anchors->setQuery({{"all_anchors", true}});
  } else {
    Hypotheses->setQuery({{"kind", Kind->currentData().toString()}});
    Anchors->clear();
  }
  Instructions->clear();
  selectAnchor();
}
} // namespace neverd::gui
