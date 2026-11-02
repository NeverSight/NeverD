//===- ComparisonImpactDialog.cpp - Native caller and entry review --------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Review observed call reachability and unknown transfer scope. Views share
/// bounded worker pages; no path or security inference is made in the GUI.
//===----------------------------------------------------------------------===//

#include "ComparisonImpactDialog.h"

#include "../ResourceLimits.h"
#include "ComparisonModel.h"
#include "ComparisonSession.h"
#include "Icons.h"

#include "neverd/diff/Limits.h"

#include <QComboBox>
#include <QGridLayout>
#include <QHeaderView>
#include <QItemSelectionModel>
#include <QJsonArray>
#include <QLabel>
#include <QLineEdit>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QSpinBox>
#include <QSplitter>
#include <QTabWidget>
#include <QTableView>
#include <QVBoxLayout>
#include <utility>

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

ComparisonImpactDialog::ComparisonImpactDialog(ComparisonSession &S,
                                               QWidget *Parent)
    : QDialog(Parent), Session(S), Revision(S.revision()) {
  setObjectName("comparisonImpactDialog");
  setWindowTitle(tr("Call paths and possible impact — NeverD"));
  setWindowIcon(icon("compare"));
  resize(::neverd::limits::gui::kComparisonImpactInitialWidth,
         ::neverd::limits::gui::kComparisonImpactInitialHeight);
  setMinimumSize(::neverd::limits::gui::kComparisonImpactMinimumWidth,
                 ::neverd::limits::gui::kComparisonImpactMinimumHeight);
  auto *Root = new QVBoxLayout(this);
  auto *Options = new QGridLayout;
  Side = new QComboBox(this);
  Side->setObjectName("impactSide");
  Side->addItem(tr("Primary input"), "primary");
  Side->addItem(tr("Secondary input"), "secondary");
  Seed = new QLineEdit(this);
  Seed->setObjectName("impactSeed");
  Seed->setPlaceholderText(tr("Function entry, e.g. 0x401000"));
  Seed->setAccessibleName(tr("Starting function entry"));
  Direction = new QComboBox(this);
  Direction->setObjectName("impactDirection");
  Direction->addItem(tr("Trace callers"), "callers");
  Direction->addItem(tr("Trace callees"), "callees");
  Depth = new QSpinBox(this);
  Depth->setObjectName("impactDepth");
  Depth->setRange(0, ::neverd::limits::diff::kMaxImpactDepth);
  Depth->setValue(::neverd::limits::diff::kDefaultImpactDepth);
  auto *DepthLabel = new QLabel(tr("Maximum depth"), this);
  DepthLabel->setBuddy(Depth);
  Trace = new QPushButton(tr("Trace"), this);
  Trace->setObjectName("impactTrace");
  Cancel = new QPushButton(tr("Cancel"), this);
  Cancel->setObjectName("impactCancel");
  Options->addWidget(Side, 0, 0);
  Options->addWidget(Seed, 0, 1, 1, 3);
  Options->addWidget(Trace, 0, 4);
  Options->addWidget(Direction, 1, 0);
  Options->addWidget(DepthLabel, 1, 1);
  Options->addWidget(Depth, 1, 2);
  Options->addWidget(Cancel, 1, 4);
  Options->setColumnStretch(3, 1);
  Root->addLayout(Options);
  Summary = label(
      this,
      tr("Choose a starting function, then trace its callers or callees."));
  Summary->setObjectName("impactSummary");
  Root->addWidget(Summary);
  auto *Split = new QSplitter(Qt::Vertical, this);
  Split->setChildrenCollapsible(false);
  Root->addWidget(Split, 1);
  auto *Top = new QWidget(Split);
  auto *TopLayout = new QVBoxLayout(Top);
  TopLayout->setContentsMargins(0, 0, 0, 0);
  FunctionKind = new QComboBox(Top);
  FunctionKind->setObjectName("impactFunctionKind");
  FunctionKind->addItem(tr("All reached functions"), "functions");
  FunctionKind->addItem(tr("Observed program entries and exports"),
                        "entrypoints");
  TopLayout->addWidget(FunctionKind);
  Functions = new ComparisonModel(Session,
                                  ComparisonModel::Kind::ImpactFunctions, this);
  FunctionTable = table(Top, Functions, "impactFunctions");
  FunctionTable->setColumnWidth(1, 200);
  TopLayout->addWidget(FunctionTable, 1);
  Inspect = new QPushButton(tr("Inspect original function"), Top);
  Inspect->setObjectName("impactInspect");
  TopLayout->addWidget(Inspect, 0, Qt::AlignLeft);
  auto *Bottom = new QWidget(Split);
  auto *BottomLayout = new QVBoxLayout(Bottom);
  BottomLayout->setContentsMargins(0, 0, 0, 0);
  SiteKind = new QComboBox(Bottom);
  SiteKind->setObjectName("impactSiteKind");
  SiteKind->addItem(tr("Shortest witness for the selected function"), "path");
  SiteKind->addItem(tr("All traversed call sites"), "sites");
  SiteKind->addItem(tr("Unresolved sites and coverage gaps across the input"),
                    "boundaries");
  SiteKind->addItem(tr("Known edges beyond the depth or function limit"),
                    "frontier");
  SiteKind->addItem(tr("Call contracts in reached functions"), "effects");
  BottomLayout->addWidget(SiteKind);
  SiteStatus = label(Bottom, {});
  SiteStatus->setObjectName("impactSiteStatus");
  BottomLayout->addWidget(SiteStatus);
  Sites =
      new ComparisonModel(Session, ComparisonModel::Kind::ImpactSites, this);
  SiteTabs = new QTabWidget(Bottom);
  SiteTabs->setObjectName("impactSiteTabs");
  SiteTable = table(SiteTabs, Sites, "impactSites");
  SiteTable->setColumnWidth(0, 160);
  SiteTable->setColumnWidth(2, 160);
  SiteTable->setColumnWidth(3, 180);
  SiteTable->setColumnWidth(5, SiteTable->fontMetrics().horizontalAdvance(
                                   tr("Unknown indirect call")) +
                                   28);
  SiteTabs->addTab(SiteTable, tr("Call sites"));
  SiteDetails = new QPlainTextEdit(SiteTabs);
  SiteDetails->setObjectName("impactSiteDetails");
  SiteDetails->setReadOnly(true);
  SiteTabs->addTab(SiteDetails, tr("Selected call"));
  BottomLayout->addWidget(SiteTabs, 1);
  Root->addWidget(label(
      this,
      tr("Observed call paths provide possible impact context. "
         "Path feasibility, parameter and return binding, memory aliases and "
         "external callers "
         "are not modeled. Semantic validation has not been performed.")));
  connect(Trace, &QPushButton::clicked, this, [this] { trace(); });
  connect(Seed, &QLineEdit::returnPressed, this, [this] {
    if (Trace->isEnabled())
      trace();
  });
  connect(Cancel, &QPushButton::clicked, &Session, &ComparisonSession::cancel);
  connect(Side, &QComboBox::currentIndexChanged, this, [this] { clear(); });
  connect(Direction, &QComboBox::currentIndexChanged, this,
          [this] { clear(); });
  connect(Seed, &QLineEdit::textChanged, this, [this] { clear(); });
  connect(Depth, &QSpinBox::valueChanged, this, [this] { clear(); });
  connect(FunctionKind, &QComboBox::currentIndexChanged, this,
          [this] { showFunctions(); });
  connect(SiteKind, &QComboBox::currentIndexChanged, this,
          [this] { showSites(); });
  connect(SiteTable->selectionModel(), &QItemSelectionModel::currentRowChanged,
          this, [this] { showSite(); });
  connect(FunctionTable->selectionModel(),
          &QItemSelectionModel::currentRowChanged, this, [this] {
            showSites();
            controls();
          });
  connect(Inspect, &QPushButton::clicked, this, [this] {
    const auto Row = Functions->row(FunctionTable->currentIndex().row());
    if (!Row.isEmpty())
      emit inspectFunction(Query["side"].toString(), Row["address"].toString());
  });
  connect(SiteTable, &QTableView::doubleClicked, this,
          [this](const QModelIndex &I) {
            const auto Caller = Sites->row(I.row())["caller"].toObject();
            if (!Caller.isEmpty())
              emit inspectFunction(Query["side"].toString(),
                                   Caller["address"].toString());
          });
  connect(
      Functions, &ComparisonModel::pageReady, this,
      [this](const QJsonObject &Page) {
        const auto State = Page["state"].toString();
        const auto Status = State == "complete"    ? tr("Complete")
                            : State == "cancelled" ? tr("Cancelled")
                            : State == "timed_out" ? tr("Time limit reached")
                            : State == "resource_limited"
                                ? tr("Analysis limit reached")
                                : State;
        Summary->setText(
            tr("%1 · %2 functions · %3 entries/exports · %4 call sites · "
               "%5 unresolved or coverage boundaries · %6 frontier sites\n"
               "Indexed %7 / %8 functions; %9; %10.")
                .arg(Status)
                .arg(Page["function_count"].toInt())
                .arg(Page["entry_count"].toInt())
                .arg(Page["site_count"].toInt())
                .arg(Page["boundary_count"].toInt())
                .arg(Page["frontier_count"].toInt())
                .arg(Page["indexed_functions"].toInt())
                .arg(Page["program_functions"].toInt())
                .arg(Page["index_complete"].toBool() ? tr("index scan complete")
                                                     : tr("index scan partial"))
                .arg(Page["discovery_complete"].toBool()
                         ? tr("input discovery complete")
                         : tr("input discovery partial")));
        if (Functions->rowCount() && !FunctionTable->currentIndex().isValid())
          FunctionTable->setCurrentIndex(FunctionTable->model()->index(0, 0));
        controls();
      });
  for (auto *M : {Functions, Sites})
    connect(M, &ComparisonModel::failed, this,
            [this](const QString &Error) { Summary->setText(Error); });
  connect(Sites, &ComparisonModel::pageReady, this,
          [this](const QJsonObject &Page) {
            if (Page["kind"].toString() == "effects") {
              const auto E = Page["effect_review"].toObject();
              SiteStatus->setText(
                  tr("%1 qualified call contracts in %2 retained sites; "
                     "inspection %3. Scope: calls in reached functions, "
                     "including unresolved and frontier targets. Native "
                     "coverage remains independent. Select a row and open "
                     "Selected call for roles and assumptions.")
                      .arg(E["qualified_contracts"].toInt())
                      .arg(Page["total"].toInt())
                      .arg(E["inspection_complete"].toBool()
                               ? tr("complete")
                               : E["state"].toString().replace('_', ' ')));
            }
            if (Sites->rowCount() && !SiteTable->currentIndex().isValid())
              SiteTable->setCurrentIndex(SiteTable->model()->index(0, 0));
            showSite();
          });
  connect(&Session, &ComparisonSession::changed, this, [this] {
    if (Revision != Session.revision()) {
      Revision = Session.revision();
      clear();
    }
    controls();
  });
  clear();
}
void ComparisonImpactDialog::clear() {
  Query = {};
  Functions->clear();
  Sites->clear();
  Summary->setText(
      tr("Choose a starting function, then trace its callers or callees."));
  SiteStatus->clear();
  SiteDetails->clear();
  controls();
}
void ComparisonImpactDialog::controls() {
  const bool Ready = Session.ready() && !Session.busy() &&
                     !Session.revision().isEmpty() &&
                     !Session.summary()["stale"].toBool();
  Trace->setEnabled(Ready && !Session.cancellable() &&
                    !Seed->text().trimmed().isEmpty());
  Cancel->setEnabled(Session.cancellable() && !Session.cancelRequested());
  Inspect->setEnabled(Ready && !Query.isEmpty() &&
                      FunctionTable->currentIndex().isValid());
}
void ComparisonImpactDialog::trace(const QString &Input,
                                   const QString &Address) {
  Side->setCurrentIndex(Input == "secondary" ? 1 : 0);
  Seed->setText(Address);
  trace();
}
void ComparisonImpactDialog::trace() {
  Query = {{"side", Side->currentData().toString()},
           {"seeds", QJsonArray{Seed->text().trimmed()}},
           {"direction", Direction->currentData().toString()},
           {"max_depth", Depth->value()}};
  Sites->clear();
  Summary->setText(tr("Tracing observed call paths…"));
  auto Page = Query;
  Page["kind"] = FunctionKind->currentData().toString();
  Page["refresh"] = true;
  Functions->setQuery(Page);
}
void ComparisonImpactDialog::showFunctions() {
  if (Query.isEmpty())
    return;
  auto Page = Query;
  Page["kind"] = FunctionKind->currentData().toString();
  Functions->setQuery(Page);
  Sites->clear();
}
void ComparisonImpactDialog::showSites() {
  Sites->clear();
  SiteDetails->clear();
  SiteTabs->setCurrentIndex(0);
  if (Query.isEmpty())
    return;
  auto Page = Query;
  const auto Kind = SiteKind->currentData().toString();
  Page["kind"] = Kind;
  if (Kind == "path") {
    const auto Row = Functions->row(FunctionTable->currentIndex().row());
    SiteStatus->setText(
        tr("One shortest observed witness; alternate paths remain in all call "
           "sites. Seeds have an empty path."));
    if (Row.isEmpty())
      return;
    Page["witness"] = Row["row"];
  } else if (Kind == "boundaries") {
    SiteStatus->setText(
        tr("Global scanned scope, including functions with no known path to "
           "the starting function. Unknown callers may be missing."));
  } else if (Kind == "frontier") {
    SiteStatus->setText(
        tr("Known targets omitted by the depth or function limit. Work or "
           "index stops can leave further targets unobserved."));
  } else if (Kind == "effects") {
    SiteStatus->setText(
        tr("Reviewing exact import identities and recovered call arguments. "
           "Collect SSA dependencies when starting the comparison to qualify "
           "contracts. Missing or ambiguous evidence remains visible."));
  } else {
    SiteStatus->setText(
        tr("Observed relations include admitted tail transfers and resolved "
           "imports. Double-click to inspect the caller."));
  }
  Sites->setQuery(Page);
}
void ComparisonImpactDialog::showSite() {
  const auto Row = Sites->row(SiteTable->currentIndex().row());
  if (Row.isEmpty()) {
    SiteDetails->clear();
    return;
  }
  const auto Caller = Row["caller"].toObject();
  const auto Callee = Row["callee"].toObject();
  const auto Instruction = Row["instruction"].toObject();
  const QStringList Original{
      tr("Caller: %1 (%2)")
          .arg(Caller["name"].toString(), Caller["address"].toString()),
      tr("Target: %1 (%2)")
          .arg(Callee["name"].toString(), Callee["address"].toString()),
      tr("Original instruction: %1  %2 %3")
          .arg(Instruction["address"].toString(),
               Instruction["mnemonic"].toString(),
               Instruction["operands"].toString()),
      tr("Bytes: %1").arg(Instruction["bytes"].toString())};
  const auto Effect = Row["effect"].toObject();
  QStringList Lines;
  if (!Effect.isEmpty()) {
    Lines << ComparisonModel::callEffectText(Effect) << Original[1];
    if (!Effect["ssa_node"].isNull())
      Lines << tr("Native-associated SSA node: %1")
                   .arg(Effect["ssa_node"].toInt());
    if (!Effect["argument_count"].isNull())
      Lines << tr("Recovered arguments: %1")
                   .arg(Effect["argument_count"].toInt());
    if (Effect["status"].toString() == "qualified_contract") {
      Lines << tr("Assumption: the imported function resolves to the declared "
                  "built-in call contract.");
      const auto Roles = Effect["argument_roles"].toObject();
      QStringList RoleLabels;
      for (const auto &[Key, Label] :
           {std::pair{"destination", tr("Destination")},
            {"source", tr("Source")},
            {"length", tr("Length")},
            {"capacity", tr("Capacity")},
            {"format", tr("Format")},
            {"handle", tr("Object / handle")},
            {"input_buffer", tr("Input buffer")}})
        if (!Roles[Key].isNull())
          RoleLabels
              << tr("%1: argument %2").arg(Label).arg(Roles[Key].toInt());
      if (!RoleLabels.empty())
        Lines << tr("Roles (zero-based): %1").arg(RoleLabels.join("; "));
      if (Effect["release_may_fail"].toBool())
        Lines << tr(
            "Release can fail; successful release has not been proved.");
      if (Effect["unbounded_write"].toBool())
        Lines << tr(
            "The contract accepts unbounded input; destination capacity "
            "has not been assessed.");
    }
    QStringList Gaps;
    for (const auto &Gap : Effect["ssa_gaps"].toArray())
      Gaps << Gap.toString().replace('_', ' ');
    if (!Gaps.empty())
      Lines << tr("Retained SSA boundaries: %1").arg(Gaps.join(", "));
    Lines << QString() << Original;
    Lines << QString()
          << tr("No parameter/return propagation, alias, capacity, path-safety "
                "or fix-presence proof. Use the original function and SSA "
                "dependency views to continue review.");
  } else
    Lines = Original;
  SiteDetails->setPlainText(Lines.join('\n'));
}
} // namespace neverd::gui
