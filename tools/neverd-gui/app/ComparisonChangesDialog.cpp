//===- ComparisonChangesDialog.cpp - Local native/SSA change review -------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Shared observations, original instructions and dependency slices. The GUI
/// presents retained scope and attribution without inferring security effects.
//===----------------------------------------------------------------------===//

#include "ComparisonChangesDialog.h"

#include "../ResourceLimits.h"
#include "ComparisonModel.h"
#include "ComparisonSession.h"
#include "Icons.h"
#include "Theme.h"

#include <QComboBox>
#include <QGridLayout>
#include <QHeaderView>
#include <QItemSelectionModel>
#include <QJsonArray>
#include <QLabel>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QSplitter>
#include <QTabWidget>
#include <QTableView>
#include <QVBoxLayout>

namespace neverd::gui {
namespace {
QString words(const QJsonArray &Values) {
  QStringList Out;
  for (const auto &V : Values)
    Out << V.toString().replace('_', ' ');
  return Out.join(", ");
}
QLabel *label(QWidget *Parent, const char *Name) {
  auto *Out = new QLabel(Parent);
  Out->setObjectName(Name);
  Out->setWordWrap(true);
  Out->setTextInteractionFlags(Qt::TextSelectableByMouse);
  return Out;
}
QTableView *table(QWidget *Parent, ComparisonModel *Model, const char *Name) {
  auto *View = new QTableView(Parent);
  View->setObjectName(Name);
  View->setModel(Model);
  View->setSelectionBehavior(QAbstractItemView::SelectRows);
  View->setSelectionMode(QAbstractItemView::SingleSelection);
  View->setEditTriggers(QAbstractItemView::NoEditTriggers);
  View->setAlternatingRowColors(true);
  View->setWordWrap(false);
  View->verticalHeader()->hide();
  View->horizontalHeader()->setStretchLastSection(true);
  return View;
}
} // namespace
ComparisonChangesDialog::ComparisonChangesDialog(ComparisonSession &S,
                                                 QWidget *Parent)
    : QDialog(Parent), Session(S), Revision(S.revision()) {
  setObjectName("comparisonChangesDialog");
  setWindowTitle(tr("Change review — NeverD"));
  setWindowIcon(icon("compare"));
  resize(::neverd::limits::gui::kComparisonChangesInitialWidth,
         ::neverd::limits::gui::kComparisonChangesInitialHeight);
  setMinimumSize(::neverd::limits::gui::kComparisonChangesMinimumWidth,
                 ::neverd::limits::gui::kComparisonChangesMinimumHeight);
  auto *Root = new QVBoxLayout(this);
  auto *Options = new QGridLayout;
  Context = new QComboBox(this);
  Context->setObjectName("changesContext");
  Context->setAccessibleName(tr("Change context filter"));
  Context->addItem(tr("All local changes"), "all");
  Context->addItem(tr("Predicate dependencies"), "predicate_dependency");
  Context->addItem(tr("Return dependencies"), "return_dependency");
  Context->addItem(tr("Call argument dependencies"),
                   "call_argument_dependency");
  Context->addItem(tr("Memory write dependencies"), "memory_write_dependency");
  Refresh = new QPushButton(tr("Refresh review"), this);
  Refresh->setObjectName("changesRefresh");
  Cancel = new QPushButton(tr("Cancel"), this);
  Cancel->setObjectName("changesCancel");
  Options->addWidget(Context, 0, 0);
  Options->setColumnStretch(1, 1);
  Options->addWidget(Refresh, 0, 2);
  Options->addWidget(Cancel, 0, 3);
  Root->addLayout(Options);
  Summary = label(this, "changesSummary");
  Summary->setText(
      tr("Choose an accepted function pair to review its changes."));
  Root->addWidget(Summary);
  auto *Split = new QSplitter(Qt::Vertical, this);
  Split->setChildrenCollapsible(false);
  Changes = new ComparisonModel(Session, ComparisonModel::Kind::Changes, this);
  ChangeTable = table(Split, Changes, "changesTable");
  ChangeTable->setColumnWidth(0, 80);
  ChangeTable->setColumnWidth(1, 150);
  ChangeTable->setColumnWidth(2, 200);
  ChangeTable->setColumnWidth(3, 230);
  ChangeTable->setColumnWidth(4, 230);
  Details = new QTabWidget(Split);
  Details->setObjectName("changesDetails");
  Evidence = new QPlainTextEdit(Details);
  Evidence->setObjectName("changesEvidence");
  Evidence->setReadOnly(true);
  Evidence->setFont(Theme::instance().codeFont());
  Details->addTab(Evidence, tr("Original evidence"));
  auto *SlicePane = new QWidget(Details);
  auto *SliceLayout = new QVBoxLayout(SlicePane);
  SliceStatus = label(SlicePane, "changesSliceStatus");
  SliceStatus->setText(tr("Select a change, then trace its inputs or uses."));
  SliceLayout->addWidget(SliceStatus);
  Slice = new ComparisonModel(Session, ComparisonModel::Kind::FlowSlice, this);
  SliceTable = table(SlicePane, Slice, "changesSlice");
  SliceTable->setColumnWidth(0, 70);
  SliceTable->setColumnWidth(1, 150);
  SliceTable->setColumnWidth(2, 160);
  SliceTable->setColumnWidth(3, 70);
  SliceTable->setColumnWidth(4, 270);
  SliceLayout->addWidget(SliceTable, 1);
  Details->addTab(SlicePane, tr("Dependency slice"));
  Root->addWidget(Split, 1);
  auto *Actions = new QGridLayout;
  SliceSide = new QComboBox(this);
  SliceSide->setObjectName("changesSide");
  SliceSide->setAccessibleName(tr("Evidence input"));
  SliceSide->addItem(tr("Primary input"), "primary");
  SliceSide->addItem(tr("Secondary input"), "secondary");
  Inspect = new QPushButton(tr("Inspect function"), this);
  Inspect->setObjectName("changesInspect");
  Calls = new QPushButton(tr("Call paths…"), this);
  Calls->setObjectName("changesCalls");
  Backward = new QPushButton(tr("Trace inputs"), this);
  Backward->setObjectName("changesBackward");
  Forward = new QPushButton(tr("Trace uses"), this);
  Forward->setObjectName("changesForward");
  Actions->addWidget(SliceSide, 0, 0);
  Actions->addWidget(Inspect, 0, 1);
  Actions->addWidget(Calls, 0, 2);
  Actions->setColumnStretch(3, 1);
  Actions->addWidget(Backward, 0, 4);
  Actions->addWidget(Forward, 0, 5);
  Root->addLayout(Actions);
  auto *Scope = label(this, "changesScope");
  Scope->setText(
      tr("Priority orders review leads; it is not severity or confidence. "
         "Context is an observed dependency. Local operands and unique "
         "direct synthetic operands are compared; deeper expressions, "
         "security effects and fix presence remain unassessed."));
  Root->addWidget(Scope);
  connect(Context, &QComboBox::currentIndexChanged, this,
          [this] { reload(false); });
  connect(Refresh, &QPushButton::clicked, this, [this] { reload(true); });
  connect(Cancel, &QPushButton::clicked, &Session, &ComparisonSession::cancel);
  connect(ChangeTable->selectionModel(),
          &QItemSelectionModel::currentRowChanged, this,
          [this] { selected(); });
  connect(SliceSide, &QComboBox::currentIndexChanged, this, [this] {
    Slice->clear();
    SliceStatus->setText(tr("Select a change, then trace its inputs or uses."));
    controls();
  });
  connect(Inspect, &QPushButton::clicked, this, [this] {
    emit inspectFunction(SliceSide->currentData().toString(),
                         SliceSide->currentIndex() ? SecondaryAddress
                                                   : PrimaryAddress);
  });
  connect(Calls, &QPushButton::clicked, this, [this] {
    emit traceCalls(SliceSide->currentData().toString(),
                    SliceSide->currentIndex() ? SecondaryAddress
                                              : PrimaryAddress);
  });
  connect(Backward, &QPushButton::clicked, this, [this] { slice("backward"); });
  connect(Forward, &QPushButton::clicked, this, [this] { slice("forward"); });
  connect(
      Changes, &ComparisonModel::pageReady, this,
      [this](const QJsonObject &Page) {
        PrimaryAddress = Page["primary"].toObject()["address"].toString();
        SecondaryAddress = Page["secondary"].toObject()["address"].toString();
        const auto Availability = [&](const char *Side) {
          const auto SSA = Page[Side].toObject();
          auto Text = SSA["availability"].toString().replace('_', ' ');
          if (SSA["availability"] == QJsonValue("available") &&
              !SSA["indexed"].toBool())
            Text += tr("; indexing incomplete");
          if (!SSA["reason"].toString().isEmpty())
            Text += ": " + SSA["reason"].toString();
          return Text;
        };
        Summary->setText(
            tr("%1 ↔ %2 · %3 · %4 matching / %5 retained observations\n"
               "Native rows %6 / %7; coverage %8; ambiguous associations %9. "
               "SSA: primary %10; secondary %11.\nBoundaries: primary %12; "
               "secondary %13. Reference identities: %14.")
                .arg(PrimaryAddress, SecondaryAddress,
                     Page["state"].toString().replace('_', ' '))
                .arg(Page["total"].toInt())
                .arg(Page["observation_count"].toInt())
                .arg(Page["visited_native_rows"].toInt())
                .arg(Page["total_native_rows"].toInt())
                .arg(Page["native_analysis_complete"].toBool() ? tr("complete")
                                                               : tr("partial"))
                .arg(Page["ambiguous_origins"].toInt())
                .arg(Availability("primary_ssa"), Availability("secondary_ssa"),
                     Page["primary_gaps"].toArray().isEmpty()
                         ? tr("none recorded")
                         : words(Page["primary_gaps"].toArray()),
                     Page["secondary_gaps"].toArray().isEmpty()
                         ? tr("none recorded")
                         : words(Page["secondary_gaps"].toArray()))
                .arg(Page["reference_comparison_complete"].toBool()
                         ? tr("compared")
                         : tr("unresolved")));
        if (Changes->rowCount() && !ChangeTable->currentIndex().isValid())
          ChangeTable->setCurrentIndex(ChangeTable->model()->index(0, 0));
        controls();
      });
  connect(Slice, &ComparisonModel::pageReady, this,
          [this](const QJsonObject &Page) {
            SliceStatus->setText(
                tr("%1 slice · %2 operations · %3 · gaps: %4")
                    .arg(Page["direction"].toString())
                    .arg(Page["total"].toInt())
                    .arg(Page["state"].toString().replace('_', ' '),
                         words(Page["gaps"].toArray())));
            controls();
          });
  connect(Changes, &ComparisonModel::failed, this,
          [this](const QString &Error) { Summary->setText(Error); });
  connect(Slice, &ComparisonModel::failed, this,
          [this](const QString &Error) { SliceStatus->setText(Error); });
  connect(&Session, &ComparisonSession::changed, this, [this] {
    if (Revision != Session.revision()) {
      Revision = Session.revision();
      Query = {};
      PrimaryAddress.clear();
      SecondaryAddress.clear();
      Changes->clear();
      selected();
      Summary->setText(
          tr("Comparison changed. Choose a function pair for a fresh review."));
    }
    controls();
  });
  controls();
}
void ComparisonChangesDialog::review(const QString &Side,
                                     const QString &Address) {
  Query = {{"side", Side}, {"address", Address}};
  SliceSide->setCurrentIndex(Side == "secondary");
  reload(false);
}
void ComparisonChangesDialog::reload(bool Fresh) {
  if (Query.isEmpty())
    return;
  Query["context"] = Context->currentData().toString();
  auto Request = Query;
  if (Fresh)
    Request["refresh"] = true;
  PrimaryAddress.clear();
  SecondaryAddress.clear();
  Changes->setQuery(Request);
  selected();
  Summary->setText(tr("Reviewing local changes…"));
  controls();
}
void ComparisonChangesDialog::selected() {
  Slice->clear();
  SliceStatus->setText(tr("Select a change, then trace its inputs or uses."));
  const auto Row = Changes->row(ChangeTable->currentIndex().row());
  QStringList Lines;
  if (!Row.isEmpty()) {
    Lines << tr("Changes: %1").arg(words(Row["kinds"].toArray()))
          << tr("Association: %1; native row: %2 (%3)")
                 .arg(Row["association"].toString().replace('_', ' '))
                 .arg(Row["native_row"].toInt())
                 .arg(Row["native_kind"].toString());
    for (const auto &Side : {QString("primary"), QString("secondary")}) {
      const auto I = Row[Side].toObject(), N = Row[Side + "_node"].toObject();
      Lines << QString() << Side.toUpper();
      Lines << (I.isEmpty()
                    ? tr("No original instruction on this side")
                    : I["address"].toString() + "  " + I["bytes"].toString() +
                          "  " + I["mnemonic"].toString() + ' ' +
                          I["operands"].toString());
      Lines << tr("Context: %1").arg(words(Row[Side + "_context"].toArray()));
      if (N.isEmpty()) {
        Lines << tr("Native evidence only; no unique SSA association.");
        continue;
      }
      Lines << tr("SSA node %1: %2, result %3 bytes; native anchor node %4")
                   .arg(N["id"].toInt())
                   .arg(N["opcode"].toString())
                   .arg(N["result_bytes"].toInt())
                   .arg(Row[Side + "_anchor_node"].toInt());
      if (N["origin_sequence"].toInt(-1) < 0)
        Lines << tr("Synthetic operation: linked by a direct operand; its "
                    "address is not an original instruction.");
      for (const auto &Input : N["inputs"].toArray()) {
        const auto V = Input.toArray();
        if (V.size() < 4)
          continue;
        Lines << tr("  %1 bytes · %2")
                     .arg(V[2].toInt())
                     .arg(V[0].toInt() == 0 ? tr("node %1").arg(V[1].toInt())
                          : V[0].toInt() == 1
                              ? tr("constant %1").arg(V[3].toString())
                          : V[0].toInt() == 2 ? tr("parameter")
                                              : tr("unknown input"));
      }
      Lines << tr("Operation gaps: %1").arg(words(N["gaps"].toArray()));
    }
  }
  Evidence->setPlainText(Lines.join('\n'));
  controls();
}
void ComparisonChangesDialog::controls() {
  const bool Idle =
      Session.ready() && !Session.busy() && !Session.cancellable();
  const auto Row = Changes->row(ChangeTable->currentIndex().row());
  const auto Side = SliceSide->currentData().toString();
  const bool Address =
      !(SliceSide->currentIndex() ? SecondaryAddress : PrimaryAddress)
           .isEmpty();
  const bool Node = Row[Side + "_node"].isObject();
  Context->setEnabled(Idle && !Query.isEmpty());
  Refresh->setEnabled(Idle && !Query.isEmpty());
  Cancel->setEnabled(Session.cancellable() && !Session.cancelRequested());
  SliceSide->setEnabled(Idle);
  Inspect->setEnabled(Idle && Address);
  Calls->setEnabled(Idle && Address);
  Backward->setEnabled(Idle && Address && Node);
  Forward->setEnabled(Idle && Address && Node);
}
void ComparisonChangesDialog::slice(const QString &Direction) {
  const auto Side = SliceSide->currentData().toString();
  const auto Node =
      Changes->row(ChangeTable->currentIndex().row())[Side + "_node"]
          .toObject();
  if (Node.isEmpty())
    return;
  SliceStatus->setText(tr("Tracing %1 dependencies…").arg(Direction));
  Slice->setQuery({{"side", Side},
                   {"address", SliceSide->currentIndex() ? SecondaryAddress
                                                         : PrimaryAddress},
                   {"seeds", QJsonArray{Node["id"]}},
                   {"direction", Direction},
                   {"control", true},
                   {"refresh", true}});
  Details->setCurrentIndex(1);
}
} // namespace neverd::gui
