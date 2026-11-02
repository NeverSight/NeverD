//===- LineageDialog.cpp - Evidence-preserving version review -------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Native paged views keep source evidence, uncertain associations and manual
/// assessments distinct. Navigation never creates new engine conclusions.
//===----------------------------------------------------------------------===//

#include "LineageDialog.h"

#include "../ResourceLimits.h"
#include "ElidedLabel.h"
#include "FlowLayout.h"
#include "Icons.h"
#include "LineageGraph.h"
#include "LineagePageView.h"
#include "LineageReleaseView.h"
#include "ModelessDialog.h"
#include "ShrinkableRow.h"

#include <QCloseEvent>
#include <QComboBox>
#include <QDialogButtonBox>
#include <QEvent>
#include <QFileDialog>
#include <QFontMetrics>
#include <QFormLayout>
#include <QHeaderView>
#include <QJsonArray>
#include <QLabel>
#include <QLineEdit>
#include <QMessageBox>
#include <QPlainTextEdit>
#include <QProgressBar>
#include <QPushButton>
#include <QRegularExpressionValidator>
#include <QScreen>
#include <QSettings>
#include <QSignalBlocker>
#include <QSplitter>
#include <QTabWidget>
#include <QTableView>
#include <QVBoxLayout>
#include <algorithm>
#include <limits>
#include <tuple>
#include <utility>

namespace neverd::gui {
namespace {
namespace limits = ::neverd::limits::gui;
QLabel *label(const QString &Text, QWidget *Parent) {
  auto *L = new QLabel(Text, Parent);
  L->setTextFormat(Qt::PlainText);
  L->setWordWrap(true);
  return L;
}
QPushButton *button(const QString &Text, const char *Name, QWidget *Parent) {
  auto *B = new QPushButton(Text, Parent);
  B->setObjectName(Name);
  B->setAutoDefault(false);
  return B;
}
LineagePageView *page(LineageSession &Session, const QStringList &Titles,
                      const QStringList &Keys, const char *Name,
                      QWidget *Parent) {
  auto *P = new LineagePageView(Session, Titles, Keys, Parent);
  P->setObjectName(Name);
  P->table()->setObjectName(QString::fromLatin1(Name) + "Table");
  P->table()->setAccessibleName(Titles.join(", "));
  P->table()->setShowGrid(false);
  return P;
}
} // namespace

LineageDialog::LineageDialog(const QString &Worker, QWidget *Parent)
    : QDialog(Parent), Session(Worker, this) {
  setObjectName("versionComparisonGraph");
  setWindowTitle(tr("Version comparison graph"));
  setWindowIcon(icon("compare"));
  setWindowFlags((windowFlags() | Qt::WindowMinMaxButtonsHint) &
                 ~Qt::WindowContextHelpButtonHint);
  setAttribute(Qt::WA_DeleteOnClose);
  setSizeGripEnabled(true);
  resize(limits::kLineageInitialWidth, limits::kLineageInitialHeight);
  setMinimumSize(limits::kLineageMinimumWidth, limits::kLineageMinimumHeight);
  const auto Geometry = QSettings().value("lineage/geometry").toByteArray();
  if (!Geometry.isEmpty())
    restoreGeometry(Geometry);
  if (screen())
    resize(size().boundedTo(screen()->availableGeometry().size()));
  auto *Root = new QVBoxLayout(this);
  auto *Actions = new FlowLayout;
  Add = button(tr("&Add comparisons…"), "lineageAdd", this);
  Add->setIcon(icon("open"));
  Open = button(tr("&Open graph…"), "lineageOpen", this);
  Save = button(tr("&Save graph…"), "lineageSave", this);
  OptionsButton = button(tr("Resource settings…"), "lineageOptions", this);
  Cancel = button(tr("Cancel"), "lineageCancel", this);
  for (auto *B : {Add, Open, Save})
    Actions->addWidget(B);
  Actions->addWidget(OptionsButton);
  Actions->addWidget(Cancel);
  Root->addLayout(Actions);
  Summary =
      label(tr("Add saved comparisons between releases to trace functions, "
               "inspect conflicting paths and review the original evidence."),
            this);
  Summary->setObjectName("lineageSummary");
  Root->addWidget(Summary);
  auto *Scope = label(tr("Associations come from saved reports. Release order "
                         "and patch presence are not inferred."),
                      this);
  makeStatusLine(*Scope);
  Root->addWidget(Scope);
  auto *Split = new QSplitter(this);
  Split->setChildrenCollapsible(false);
  Root->addWidget(Split, 1);
  auto *Left = new QWidget(Split);
  auto *LeftLayout = new QVBoxLayout(Left);
  // Keep control frames inside the pane, including the outer edge that Qt's
  // right-to-left splitter geometry can place one pixel beyond its viewport.
  LeftLayout->setContentsMargins(Root->spacing(), 0, Root->spacing(), 0);
  LeftLayout->addWidget(label(tr("Analyzed snapshots"), Left));
  Versions = page(Session, {tr("Label"), tr("Branch"), tr("Functions")},
                  {"label", "branch", "functions"}, "lineageVersions", Left);
  LeftLayout->addWidget(Versions, 1);
  Rename = button(tr("Edit &label / branch…"), "lineageMetadata", Left);
  LeftLayout->addWidget(Rename);
  Tabs = new QTabWidget(Split);
  Tabs->setObjectName("lineageTabs");
  auto *FunctionPane = new QWidget(Tabs);
  auto *FL = new QVBoxLayout(FunctionPane);
  FunctionTitle = new ElidedLabel(FunctionPane);
  FunctionTitle->setFullText(
      tr("Select a snapshot to browse all retained functions."));
  FL->addWidget(FunctionTitle);
  Search = new QLineEdit(FunctionPane);
  Search->setObjectName("lineageSearch");
  Search->setPlaceholderText(tr("Find a function by name or address"));
  Search->setAccessibleName(tr("Function filter"));
  FL->addWidget(Search);
  Functions = page(
      Session, {tr("Function"), tr("Address"), tr("Size"), tr("Instructions")},
      {"name", "address", "size", "instructions"}, "lineageFunctions",
      FunctionPane);
  FL->addWidget(Functions, 1);
  Trace = button(tr("&Trace selected function"), "lineageTrace", FunctionPane);
  FL->addWidget(Trace);
  Tabs->addTab(FunctionPane, tr("Functions"));
  auto *TracePane = new QWidget(Tabs);
  auto *TL = new QVBoxLayout(TracePane);
  TraceTitle = new ElidedLabel(TracePane);
  TraceTitle->setFullText(
      tr("Trace a function to see every reachable association "
         "and its shortest-path witness."));
  TL->addWidget(TraceTitle);
  auto *TraceTabs = new QTabWidget(TracePane);
  TraceTabs->setObjectName("lineageTraceTabs");
  TL->addWidget(TraceTabs, 1);
  TraceNodes = page(Session,
                    {tr("Function"), tr("Snapshot"), tr("Address"),
                     tr("Distance"), tr("Predecessor"), tr("Witness edge")},
                    {"name", "version_label", "address", "distance",
                     "predecessor", "witness_edge"},
                    "lineageTraceNodes", TraceTabs);
  TraceEdges =
      page(Session,
           {tr("Edge"), tr("State"), tr("Primary node"), tr("Secondary node"),
            tr("Source report"), tr("Method"), tr("Changes")},
           {"edge", "change_state", "primary", "secondary", "report", "step",
            "changes"},
           "lineageTraceEdges", TraceTabs);
  TraceConflicts =
      page(Session, {tr("Conflict"), tr("Kind"), tr("Snapshot"), tr("Members")},
           {"conflict", "kind", "version_label", "members"},
           "lineageTraceConflicts", TraceTabs);
  TraceTabs->addTab(TraceNodes, tr("Functions & witnesses"));
  TraceTabs->addTab(TraceEdges, tr("Direct evidence"));
  TraceTabs->addTab(TraceConflicts, tr("Conflicts"));
  auto *TraceHelp =
      label(tr("Distance counts report links. It is not a confidence score. "
               "Double-click a conflict to inspect its members."),
            TracePane);
  makeStatusLine(*TraceHelp);
  TL->addWidget(TraceHelp);
  Tabs->addTab(TracePane, tr("Trace"));
  auto *SourcesPane = new QWidget(Tabs);
  auto *SL = new QVBoxLayout(SourcesPane);
  SL->addWidget(label(tr("Diagram of the current source-report page; "
                         "layout does not indicate release order."),
                      SourcesPane));
  auto *GraphActions = new FlowLayout;
  auto *Fit = button(tr("Fit"), "lineageGraphFit", SourcesPane);
  auto *ZoomIn = button(tr("Zoom in"), "lineageGraphZoomIn", SourcesPane);
  auto *ZoomOut = button(tr("Zoom out"), "lineageGraphZoomOut", SourcesPane);
  for (auto *B : {Fit, ZoomIn, ZoomOut})
    GraphActions->addWidget(B);
  SL->addLayout(GraphActions);
  auto *SourceSplit = new QSplitter(Qt::Vertical, SourcesPane);
  SourceSplit->setChildrenCollapsible(false);
  SL->addWidget(SourceSplit, 1);
  Graph = new LineageGraph(SourceSplit);
  Graph->setObjectName("lineageGraph");
  Graph->setMinimumHeight(limits::kLineageGraphMinimumHeight);
  Sources =
      page(Session,
           {tr("Report"), tr("Primary snapshot"), tr("Secondary snapshot"),
            tr("State"), tr("Matches"), tr("Incomplete")},
           {"id", "primary_label", "secondary_label", "state", "matches",
            "incomplete"},
           "lineageSources", SourceSplit);
  SourceSplit->setStretchFactor(0, 1);
  SourceSplit->setStretchFactor(1, 1);
  SourceSplit->setSizes({limits::kLineageGraphInitialHeight,
                         limits::kLineageSourceInitialHeight});
  auto *SourceActions = new FlowLayout;
  Inspect = button(tr("Inspect source &evidence…"), "lineageInspectSource",
                   SourcesPane);
  Export =
      button(tr("Export original report…"), "lineageExportSource", SourcesPane);
  SourceActions->addWidget(Inspect);
  SourceActions->addWidget(Export);
  SL->addLayout(SourceActions);
  Tabs->addTab(SourcesPane, tr("Graph / sources"));
  auto *ConflictPane = new QWidget(Tabs);
  auto *CL = new QVBoxLayout(ConflictPane);
  CL->addWidget(label(tr("Multiple functions from one snapshot, or an "
                         "explicitly rejected pair reached through other "
                         "reports, remain visible as conflicting evidence."),
                      ConflictPane));
  Conflicts = page(Session,
                   {tr("Conflict"), tr("Kind"), tr("Snapshot"),
                    tr("Source report"), tr("Members")},
                   {"conflict", "kind", "version_label", "report", "members"},
                   "lineageConflicts", ConflictPane);
  CL->addWidget(Conflicts, 1);
  Members = button(tr("Inspect conflict &members…"), "lineageConflictMembers",
                   ConflictPane);
  CL->addWidget(Members);
  Tabs->addTab(ConflictPane, tr("Conflicts"));
  auto *ReviewPane = new QWidget(Tabs);
  auto *RL = new QVBoxLayout(ReviewPane);
  RL->addWidget(label(tr("Latest manual assessments stay attached to their "
                         "original report and function pair. They do not "
                         "establish a patch verdict in other snapshots."),
                      ReviewPane));
  Reviews = page(Session,
                 {tr("Case"), tr("Assessment"), tr("Author"), tr("Pair active"),
                  tr("Source report")},
                 {"case", "assessment", "author", "active_pair", "report"},
                 "lineageReviews", ReviewPane);
  RL->addWidget(Reviews, 1);
  Details = new QPlainTextEdit(ReviewPane);
  Details->setReadOnly(true);
  Details->setAccessibleName(tr("Original assessment note and reference"));
  Details->setMaximumHeight(Details->fontMetrics().height() * 7);
  RL->addWidget(Details);
  Tabs->addTab(ReviewPane, tr("Assessments"));
  Releases = new LineageReleaseView(Session, Tabs);
  Tabs->addTab(Releases, tr("Releases"));
  connect(Releases, &LineageReleaseView::snapshotRequested, this,
          &LineageDialog::navigateToVersion);
  Split->setStretchFactor(0, 1);
  Split->setStretchFactor(1, 3);
  Split->setSizes(
      {limits::kLineageInitialWidth / 4, limits::kLineageInitialWidth * 3 / 4});
  auto *Bottom = new QHBoxLayout;
  Status = label(tr("Starting version graph worker…"), this);
  Status->setObjectName("lineageStatus");
  makeStatusLine(*Status);
  Progress = new QProgressBar(this);
  Progress->setRange(0, 0);
  Progress->setTextVisible(false);
  Bottom->addWidget(Status, 1);
  Bottom->addWidget(Progress);
  Root->addLayout(Bottom);
  Filter.setSingleShot(true);
  Filter.setInterval(limits::kComparisonGraphDebounceMs);
  connect(Search, &QLineEdit::textChanged, &Filter,
          qOverload<>(&QTimer::start));
  connect(&Filter, &QTimer::timeout, this,
          [this] { selectVersion(CurrentVersion); });
  connect(&Session, &LineageSession::changed, this, &LineageDialog::update);
  connect(&Session, &LineageSession::collectionChanged, this,
          &LineageDialog::loaded);
  connect(&Session, &LineageSession::failed, this, [this] {
    OpeningPath.clear();
    PendingOpenPath.clear();
    CloseAfterSave = false;
    SavingSource = false;
  });
  connect(&Session, &LineageSession::saved, this, [this](const QString &Path) {
    if (!SavingSource) {
      Dirty = false;
      SavedPath = Path;
    }
    SavingSource = false;
    if (!PendingOpenPath.isEmpty()) {
      const auto NextPath = std::exchange(PendingOpenPath, {});
      QTimer::singleShot(0, this, [this, NextPath] {
        OpeningPath = NextPath;
        Session.load({{"path", NextPath}, {"options", Options}});
      });
    }
    if (CloseAfterSave) {
      CloseAfterSave = false;
      close();
    }
  });
  connect(&Session, &LineageSession::progress, this,
          [this](const QJsonObject &P) {
            const auto Phase = P["phase"].toString();
            Status->setText(Session.cancelRequested() ? tr("Stopping…")
                            : Phase == "lineage:read" || Phase == "lineage:scan"
                                ? tr("Reading saved evidence…")
                            : Phase == "lineage:trace"
                                ? tr("Tracing function associations…")
                            : Phase == "lineage:save_publish"
                                ? tr("Saving evidence…")
                                : tr("Checking saved evidence…"));
          });
  connect(Add, &QPushButton::clicked, this, [this] {
    const auto Files = QFileDialog::getOpenFileNames(
        this, tr("Add saved comparisons"), {},
        tr("NeverD comparisons (*.nddiff);;All files (*)"));
    if (Files.isEmpty())
      return;
    OpeningPath.clear();
    Session.load({{"reports", QJsonArray::fromStringList(Files)},
                  {"append", !Session.revision().isEmpty()},
                  {"options", Options}});
  });
  connect(Open, &QPushButton::clicked, this, [this] {
    const auto Path = QFileDialog::getOpenFileName(
        this, tr("Open version comparison graph"), {},
        tr("NeverD version graphs (*.ndlineage);;All files (*)"));
    if (Path.isEmpty())
      return;
    if (Dirty) {
      const auto Choice = QMessageBox::question(
          this, tr("Save current graph?"),
          tr("Save the current graph and snapshot labels before opening "
             "another graph?"),
          QMessageBox::Save | QMessageBox::Discard | QMessageBox::Cancel,
          QMessageBox::Save);
      if (Choice == QMessageBox::Cancel)
        return;
      if (Choice == QMessageBox::Save) {
        PendingOpenPath = Path;
        save();
        return;
      }
    }
    OpeningPath = Path;
    Session.load({{"path", Path}, {"options", Options}});
  });
  connect(Save, &QPushButton::clicked, this, &LineageDialog::save);
  connect(Cancel, &QPushButton::clicked, &Session, &LineageSession::cancel);
  connect(OptionsButton, &QPushButton::clicked, this, &LineageDialog::options);
  connect(Rename, &QPushButton::clicked, this, &LineageDialog::metadata);
  connect(Versions, &LineagePageView::selectionChanged, this,
          [this](const QJsonObject &R) {
            if (!R.isEmpty())
              selectVersion(R["id"].toString());
            update();
          });
  connect(Versions, &LineagePageView::pageChanged, this,
          [this](const QJsonArray &Rows) {
            Labels.clear();
            for (const auto &R : Rows) {
              const auto V = R.toObject();
              Labels[V["id"].toString()] = V["label"].toString();
            }
            Graph->setLabels(Labels);
            updateTitles();
          });
  connect(Functions, &LineagePageView::selectionChanged, this,
          [this] { update(); });
  connect(Trace, &QPushButton::clicked, this,
          [this] { trace(Functions->selected()); });
  connect(Functions, &LineagePageView::activated, this, &LineageDialog::trace);
  connect(Sources, &LineagePageView::selectionChanged, this,
          [this] { update(); });
  connect(Sources, &LineagePageView::pageChanged, this,
          [this](const QJsonArray &Rows) {
            Graph->setReports(Rows);
            updateTitles();
          });
  connect(Sources, &LineagePageView::activated, this,
          [this](const QJsonObject &R) { source(R["id"].toString()); });
  connect(Inspect, &QPushButton::clicked, this,
          [this] { source(Sources->selected()["id"].toString()); });
  connect(Export, &QPushButton::clicked, this, [this] {
    const auto ID = Sources->selected()["id"].toString();
    const auto Path =
        QFileDialog::getSaveFileName(this, tr("Export original comparison"), {},
                                     tr("NeverD comparison (*.nddiff)"));
    if (!ID.isEmpty() && !Path.isEmpty()) {
      SavingSource = true;
      Session.save(Path, ID);
    }
  });
  connect(Conflicts, &LineagePageView::selectionChanged, this,
          [this] { update(); });
  connect(Conflicts, &LineagePageView::activated, this,
          &LineageDialog::members);
  connect(TraceConflicts, &LineagePageView::activated, this,
          &LineageDialog::members);
  connect(Members, &QPushButton::clicked, this,
          [this] { members(Conflicts->selected()); });
  connect(TraceEdges, &LineagePageView::activated, this,
          [this](const QJsonObject &R) { source(R["report"].toString()); });
  connect(Reviews, &LineagePageView::selectionChanged, this,
          [this](const QJsonObject &R) {
            Details->setPlainText(
                tr("Note: %1\nReference: %2\nPrimary: %3  Secondary: "
                   "%4\nSupersedes: %5")
                    .arg(R["note"].toString(), R["reference"].toString(),
                         R["primary"].toString(), R["secondary"].toString(),
                         R["supersedes"].toString()));
          });
  connect(Graph, &LineageGraph::versionActivated, this,
          &LineageDialog::navigateToVersion);
  connect(Graph, &LineageGraph::reportActivated, this, &LineageDialog::source);
  connect(Fit, &QPushButton::clicked, Graph, &LineageGraph::fit);
  connect(ZoomIn, &QPushButton::clicked, Graph, [this] { Graph->zoom(true); });
  connect(ZoomOut, &QPushButton::clicked, Graph,
          [this] { Graph->zoom(false); });
  update();
}

void LineageDialog::update() {
  const bool Ready = Session.ready() && !Session.busy();
  Add->setEnabled(Ready);
  Open->setEnabled(Ready);
  OptionsButton->setEnabled(Ready);
  Save->setEnabled(Ready && !Session.revision().isEmpty());
  Rename->setEnabled(Ready && !Versions->selected().isEmpty());
  Trace->setEnabled(Ready && !Functions->selected().isEmpty());
  Inspect->setEnabled(Ready && !Sources->selected().isEmpty());
  Export->setEnabled(Inspect->isEnabled());
  Members->setEnabled(Ready && !Conflicts->selected().isEmpty());
  Cancel->setEnabled(Session.cancellable() && !Session.cancelRequested());
  Progress->setVisible(Session.busy());
  if (!Session.busy())
    Status->setText(Session.message().isEmpty() ? tr("Ready")
                                                : Session.message());
  if (!Session.summary().isEmpty()) {
    const auto &S = Session.summary();
    Summary->setText(tr("%1 snapshots · %2 source comparisons · %3 functions · "
                        "%4 conflict groups · %5 incomplete reports%6")
                         .arg(S["versions"].toInteger())
                         .arg(S["reports"].toInteger())
                         .arg(S["nodes"].toInteger())
                         .arg(S["conflicts"].toInteger())
                         .arg(S["incomplete_reports"].toInteger())
                         .arg(Dirty ? tr(" · unsaved") : QString()));
  }
  if (CloseWhenIdle && !Session.busy()) {
    CloseWhenIdle = false;
    QTimer::singleShot(0, this, &QWidget::close);
  }
}
void LineageDialog::loaded(bool Appended) {
  const auto SelectedTab = Appended ? Tabs->currentIndex() : 0;
  const auto SelectedVersion =
      Appended ? Versions->selected().value("id").toString() : QString();
  const auto VersionOffset =
      Appended && Versions->offset() < Session.summary()["versions"].toInteger()
          ? Versions->offset()
          : 0;
  Filter.stop();
  if (!Appended) {
    SavedPath.clear();
    const QSignalBlocker BlockSearch(Search);
    Search->clear();
  }
  if (!Appended || Session.summary()["versions"].toInteger() == 0) {
    CurrentVersion.clear();
    TraceVersion.clear();
    TraceAddress.clear();
    FunctionTitle->setFullText(
        tr("Select a snapshot to browse all retained functions."));
    TraceTitle->setFullText(
        tr("Trace a function to see every reachable association "
           "and its shortest-path witness."));
  }
  Dirty = OpeningPath.isEmpty();
  if (!OpeningPath.isEmpty())
    SavedPath = OpeningPath;
  OpeningPath.clear();
  Labels.clear();
  for (auto *P : {Versions, Functions, Sources, Conflicts, Reviews, TraceNodes,
                  TraceEdges, TraceConflicts})
    P->clear();
  Graph->setReports({});
  Details->clear();
  Versions->setQuery({{"kind", "versions"}}, SelectedVersion, VersionOffset);
  Sources->setQuery({{"kind", "reports"}});
  Conflicts->setQuery({{"kind", "conflicts"}});
  Reviews->setQuery({{"kind", "reviews"}});
  Releases->setOptions(Options);
  Releases->reload(Appended);
  if (!TraceVersion.isEmpty())
    trace({{"version", TraceVersion}, {"address", TraceAddress}});
  Tabs->setCurrentIndex(SelectedTab);
  update();
}
QString LineageDialog::versionLabel(const QString &ID) const {
  if (Labels.contains(ID))
    return Labels[ID];
  for (const auto &Value : Sources->rows()) {
    const auto Row = Value.toObject();
    for (const auto &Side :
         {QStringLiteral("primary"), QStringLiteral("secondary")})
      if (Row[Side + "_version"].toString() == ID)
        return Row[Side + "_label"].toString(ID);
  }
  return ID;
}
void LineageDialog::updateTitles() {
  Releases->setSnapshot(CurrentVersion, versionLabel(CurrentVersion));
  if (!CurrentVersion.isEmpty())
    FunctionTitle->setFullText(
        tr("Functions in %1").arg(versionLabel(CurrentVersion)));
  if (!TraceVersion.isEmpty())
    TraceTitle->setFullText(tr("Trace from %1 in %2")
                                .arg(TraceAddress, versionLabel(TraceVersion)));
}
void LineageDialog::navigateToVersion(const QString &ID) {
  bool Selected = false;
  const auto &Rows = Versions->rows();
  for (int I = 0; I < Rows.size(); ++I)
    if (Rows[I].toObject()["id"].toString() == ID) {
      if (Versions->selected()["id"].toString() == ID)
        selectVersion(ID);
      else
        Versions->table()->setCurrentIndex(
            Versions->table()->model()->index(I, 0));
      Selected = true;
      break;
    }
  if (!Selected) {
    Versions->table()->setCurrentIndex({});
    selectVersion(ID);
  }
  Tabs->setCurrentIndex(0);
}
void LineageDialog::selectVersion(const QString &ID) {
  if (ID.isEmpty())
    return;
  CurrentVersion = ID;
  updateTitles();
  Functions->setQuery(
      {{"kind", "functions"}, {"version", ID}, {"search", Search->text()}});
}
void LineageDialog::trace(const QJsonObject &Row) {
  if (Row["version"].toString().isEmpty() ||
      Row["address"].toString().isEmpty())
    return;
  TraceVersion = Row["version"].toString();
  TraceAddress = Row["address"].toString();
  updateTitles();
  for (const auto &[View, Kind] : {std::pair{TraceNodes, "trace_nodes"},
                                   {TraceEdges, "trace_edges"},
                                   {TraceConflicts, "trace_conflicts"}})
    View->setQuery(
        {{"kind", Kind}, {"version", TraceVersion}, {"address", TraceAddress}});
  Tabs->setCurrentIndex(1);
}
void LineageDialog::members(const QJsonObject &Conflict) {
  if (!Conflict.contains("conflict"))
    return;
  auto *D = new QDialog(this);
  D->setWindowTitle(tr("Conflicting function associations"));
  D->setObjectName("lineageConflictDetail");
  connect(&Session, &LineageSession::collectionChanged, D, &QDialog::close);
  D->resize(limits::kComparisonCounterpartInitialWidth,
            limits::kComparisonCounterpartInitialHeight);
  auto *L = new QVBoxLayout(D);
  L->addWidget(label(
      tr("These members remain in conflict. No counterpart or patch assessment "
         "is selected automatically. Activate a member to trace its evidence."),
      D));
  auto *P =
      page(Session, {tr("Function"), tr("Snapshot"), tr("Address"), tr("Node")},
           {"name", "version_label", "address", "node"},
           "lineageConflictDetailPage", D);
  L->addWidget(P);
  P->setQuery({{"kind", "conflict_members"}, {"index", Conflict["conflict"]}});
  connect(P, &LineagePageView::activated, this, &LineageDialog::trace);
  showModelessDialog(D, this);
}
void LineageDialog::source(const QString &Report) {
  if (Report.isEmpty())
    return;
  auto *D = new QDialog(this);
  D->setObjectName("lineageSourceEvidence");
  D->setWindowTitle(tr("Original comparison evidence"));
  connect(&Session, &LineageSession::collectionChanged, D, &QDialog::close);
  D->resize(limits::kComparisonCounterpartInitialWidth,
            limits::kComparisonCounterpartInitialHeight);
  auto *L = new QVBoxLayout(D);
  L->addWidget(label(
      tr("Source report: %1\nAmbiguities and manual decisions "
         "remain report-local. Region hypotheses retain their "
         "original coordinates; full records remain in the exported report.")
          .arg(Report),
      D));
  auto *T = new QTabWidget(D);
  L->addWidget(T);
  const auto AddPage = [&](const QString &Title, const char *Kind,
                           const QStringList &Titles, const QStringList &Keys) {
    auto *P = page(Session, Titles, Keys, Kind, T);
    T->addTab(P, Title);
    P->setQuery({{"kind", Kind}, {"report", Report}, {"latest", false}});
    return P;
  };
  auto *ReviewsPage = AddPage(
      tr("Assessment history"), "reviews",
      {tr("Case"), tr("Assessment"), tr("Author"), tr("Latest"), tr("Note")},
      {"case", "assessment", "author", "latest", "note"});
  (void)ReviewsPage;
  const auto AddOther = [&](const QString &Title, const char *Kind,
                            const QStringList &Titles,
                            const QStringList &Keys) {
    auto *P = page(Session, Titles, Keys, Kind, T);
    T->addTab(P, Title);
    P->setQuery({{"kind", Kind}, {"report", Report}});
    return P;
  };
  auto *Ambiguities =
      AddOther(tr("Ambiguous candidates"), "ambiguities",
               {tr("Group"), tr("Method"), tr("Primary candidates"),
                tr("Secondary candidates")},
               {"index", "step", "primary_count", "secondary_count"});
  AddOther(tr("Decision history"), "decisions",
           {tr("Event"), tr("Decision"), tr("Primary"), tr("Secondary")},
           {"index", "kind", "primary", "secondary"});
  AddOther(tr("Warnings"), "warnings", {tr("Report and history warnings")},
           {"warning"});
  for (const auto &[Title, Kind, AnchorKind] :
       {std::tuple{tr("Native regions"), "native_regions",
                   "native_region_anchors"},
        std::tuple{tr("SSA regions"), "flow_regions", "flow_region_anchors"}}) {
    auto *Regions = AddOther(
        Title, Kind,
        {tr("Hypothesis"), tr("Explanation"), tr("Anchors"), tr("Ambiguous")},
        {"id", "explanations", "anchor_count", "ambiguous"});
    connect(
        Regions, &LineagePageView::activated, D,
        [this, D, Report, AnchorKind](const QJsonObject &Row) {
          if (!Row.contains("index"))
            return;
          auto *Detail = new QDialog(D);
          Detail->setObjectName("lineageRegionAnchors");
          Detail->setWindowTitle(tr("Original region anchors"));
          Detail->resize(limits::kComparisonCounterpartInitialWidth,
                         limits::kComparisonCounterpartInitialHeight);
          auto *Layout = new QVBoxLayout(Detail);
          Layout->addWidget(label(
              tr("Saved region evidence is not an accepted function "
                 "association. Copy a row to inspect all retained fields; "
                 "export the source for complete SSA node mappings."),
              Detail));
          if (Row["call_evidence"] == "current_function_correspondence") {
            auto *Notice = label(
                tr("Calls in these saved spans rely on the source report's "
                   "function pairs. They do not prove equivalent callee "
                   "behavior."),
                Detail);
            Notice->setObjectName("lineageRegionCallEvidence");
            Layout->addWidget(Notice);
          }
          const bool Native = QByteArray(AnchorKind) == "native_region_anchors";
          auto *P = page(
              Session,
              Native ? QStringList{tr("Anchor"), tr("Primary function index"),
                                   tr("Secondary function index"),
                                   tr("Primary instructions"),
                                   tr("Secondary instructions")}
                     : QStringList{tr("Anchor"), tr("Primary function"),
                                   tr("Secondary function"), tr("SSA nodes"),
                                   tr("Open control frontier")},
              Native ? QStringList{"index", "primary_function_index",
                                   "secondary_function_index",
                                   "primary_instruction_count",
                                   "secondary_instruction_count"}
                     : QStringList{"index", "primary_function",
                                   "secondary_function", "node_count",
                                   "open_control_frontier"},
              AnchorKind, Detail);
          Layout->addWidget(P);
          P->setQuery({{"kind", AnchorKind},
                       {"report", Report},
                       {"index", Row["index"]}});
          showModelessDialog(Detail, D);
        });
    const bool Native = QByteArray(AnchorKind) == "native_region_anchors";
    if (!Session.summary()["supports_all_region_anchors"].toBool())
      continue;
    const auto *AllKind = Native ? "native_anchors" : "flow_anchors";
    auto *AllPane = new QWidget(T);
    auto *AllLayout = new QVBoxLayout(AllPane);
    AllLayout->addWidget(
        label(tr("Saved region evidence is not an accepted function "
                 "association. Copy a row to inspect all retained fields; "
                 "export the source for complete SSA node mappings."),
              AllPane));
    auto *CallNotice = label(
        tr("Calls in these saved spans rely on the source report's "
           "function pairs. They do not prove equivalent callee behavior."),
        AllPane);
    CallNotice->setObjectName("lineageRegionCallEvidence");
    CallNotice->hide();
    AllLayout->addWidget(CallNotice);
    auto *All = page(
        Session,
        Native ? QStringList{tr("Anchor"), tr("Primary function index"),
                             tr("Secondary function index"),
                             tr("Primary instructions"),
                             tr("Secondary instructions")}
               : QStringList{tr("Anchor"), tr("Primary function"),
                             tr("Secondary function"), tr("SSA nodes"),
                             tr("Open control frontier")},
        Native ? QStringList{"index", "primary_function_index",
                             "secondary_function_index",
                             "primary_instruction_count",
                             "secondary_instruction_count"}
               : QStringList{"index", "primary_function", "secondary_function",
                             "node_count", "open_control_frontier"},
        AllKind, AllPane);
    AllLayout->addWidget(All);
    T->addTab(AllPane, Native ? tr("All native spans") : tr("All SSA spans"));
    connect(All, &LineagePageView::pageChanged, CallNotice,
            [CallNotice](const QJsonArray &Rows) {
              CallNotice->setVisible(!Rows.isEmpty() &&
                                     Rows.first().toObject()["call_evidence"] ==
                                         "current_function_correspondence");
            });
    All->setQuery({{"kind", AllKind}, {"report", Report}});
  }
  connect(
      Ambiguities, &LineagePageView::activated, D,
      [this, D, Report](const QJsonObject &Row) {
        if (!Row.contains("index"))
          return;
        auto *Members = new QDialog(D);
        Members->setObjectName("lineageAmbiguityMembers");
        Members->setWindowTitle(tr("Unresolved candidates"));
        Members->resize(limits::kComparisonCounterpartInitialWidth,
                        limits::kComparisonCounterpartInitialHeight);
        auto *Layout = new QVBoxLayout(Members);
        Layout->addWidget(label(tr("Candidates are not accepted associations. "
                                   "Both sides retain every listed candidate."),
                                Members));
        auto *Sides = new QTabWidget(Members);
        Layout->addWidget(Sides);
        for (const auto &[Title, Kind] :
             {std::pair{tr("Primary"), "ambiguity_primary"},
              {tr("Secondary"), "ambiguity_secondary"}}) {
          auto *P =
              page(Session, {tr("Function"), tr("Address"), tr("Snapshot")},
                   {"name", "address", "version_label"}, Kind, Sides);
          Sides->addTab(P, Title);
          P->setQuery(
              {{"kind", Kind}, {"report", Report}, {"index", Row["index"]}});
        }
        showModelessDialog(Members, D);
      });
  showModelessDialog(D, this);
}
void LineageDialog::metadata() {
  const auto Row = Versions->selected();
  if (Row.isEmpty())
    return;
  QDialog D(this);
  D.setWindowTitle(tr("Snapshot label and branch"));
  auto *L = new QFormLayout(&D);
  auto *Name = new QLineEdit(Row["label"].toString(), &D);
  auto *Branch = new QLineEdit(Row["branch"].toString(), &D);
  Name->setMaxLength(std::numeric_limits<int>::max());
  Branch->setMaxLength(std::numeric_limits<int>::max());
  L->addRow(tr("&Label"), Name);
  L->addRow(tr("&Branch"), Branch);
  L->addRow(label(tr("These labels describe your analysis. They do not assert "
                     "release ancestry."),
                  &D));
  auto *Buttons = new QDialogButtonBox(
      QDialogButtonBox::Save | QDialogButtonBox::Cancel, &D);
  L->addRow(Buttons);
  connect(Buttons, &QDialogButtonBox::accepted, &D, &QDialog::accept);
  connect(Buttons, &QDialogButtonBox::rejected, &D, &QDialog::reject);
  connect(
      Name, &QLineEdit::textChanged, Buttons, [Buttons](const QString &Text) {
        Buttons->button(QDialogButtonBox::Save)->setEnabled(!Text.isEmpty());
      });
  if (D.exec() != QDialog::Accepted)
    return;
  OpeningPath.clear();
  Session.load({{"append", true},
                {"options", Options},
                {"version_updates",
                 QJsonArray{QJsonObject{{"id", Row["id"]},
                                        {"label", Name->text()},
                                        {"branch", Branch->text()}}}}});
}
void LineageDialog::options() {
  QDialog D(this);
  D.setWindowTitle(tr("Version graph resource settings"));
  auto *L = new QFormLayout(&D);
  QMap<QString, QLineEdit *> Inputs;
  const auto AddField = [&](const QString &Key, const QString &Title,
                            qint64 Default) {
    auto *E = new QLineEdit(
        QString::number(Options.value(Key).toInteger(Default)), &D);
    E->setValidator(
        new QRegularExpressionValidator(QRegularExpression("[0-9]{1,19}"), E));
    E->setObjectName("lineageOption_" + Key);
    Inputs[Key] = E;
    L->addRow(Title, E);
  };
  AddField("max_source_bytes", tr("Report text and labels (bytes)"),
           ::neverd::limits::diff::kDefaultLineageSourceBytes);
  AddField("max_nodes", tr("Function and release records"),
           ::neverd::limits::diff::kDefaultLineageNodes);
  AddField("max_edges", tr("Association and parent link records"),
           ::neverd::limits::diff::kDefaultLineageEdges);
  AddField("max_work", tr("Processing work allowance"),
           ::neverd::limits::diff::kDefaultLineageWork);
  AddField("timeout_ms", tr("Deadline (milliseconds, 0: none)"), 0);
  L->addRow(label(
      tr("Replacement accounts for the current collection, its trace and the "
         "candidate. A limit stops the operation and preserves the current "
         "graph. Report text allowance is not a process-memory limit."),
      &D));
  auto *Buttons = new QDialogButtonBox(
      QDialogButtonBox::Save | QDialogButtonBox::Cancel, &D);
  L->addRow(Buttons);
  auto *Error = label({}, &D);
  Error->setObjectName("lineageOptionsError");
  L->insertRow(L->rowCount() - 1, Error);
  QJsonObject Next;
  connect(Buttons, &QDialogButtonBox::accepted, &D, [&] {
    Next = {};
    for (auto It = Inputs.begin(); It != Inputs.end(); ++It) {
      bool OK = false;
      const auto N = It.value()->text().toLongLong(&OK);
      const auto Maximum =
          It.key() == "max_source_bytes"
              ? qint64(::neverd::limits::diff::kMaxSavedComparisonBytes)
          : It.key() == "timeout_ms"
              ? qint64(::neverd::limits::diff::kMaxTimeoutMilliseconds)
          : It.key() == "max_nodes" || It.key() == "max_edges"
              ? qint64(std::numeric_limits<uint32_t>::max() - 1)
              : std::numeric_limits<qint64>::max();
      if (!OK || N < 0 || N > Maximum || (It.key() != "timeout_ms" && N == 0)) {
        Error->setText(
            tr("Enter a value from %1 to %2 for the highlighted field.")
                .arg(It.key() == "timeout_ms" ? 0 : 1)
                .arg(Maximum));
        It.value()->setFocus();
        It.value()->selectAll();
        return;
      }
      Next[It.key()] = N;
    }
    D.accept();
  });
  connect(Buttons, &QDialogButtonBox::rejected, &D, &QDialog::reject);
  if (D.exec() != QDialog::Accepted)
    return;
  Options = Next;
  Session.setQueryOptions(Options);
  Releases->setOptions(Options);
  Status->setText(
      tr("Resource settings will apply to subsequent loads and queries."));
}
void LineageDialog::save() {
  const auto Path = QFileDialog::getSaveFileName(
      this, tr("Save version comparison graph"), SavedPath,
      tr("NeverD version graphs (*.ndlineage)"));
  if (Path.isEmpty()) {
    PendingOpenPath.clear();
    CloseAfterSave = false;
    return;
  }
  SavingSource = false;
  Session.save(Path);
}
bool LineageDialog::event(QEvent *Event) {
  const auto Result = QDialog::event(Event);
  if (layout() && (Event->type() == QEvent::LayoutRequest ||
                   Event->type() == QEvent::Resize)) {
    // Include the custom frame's content margins, as well as the layout's
    // own margins. Explicit baseline minima otherwise prevent Qt from growing
    // either axis when translated controls or theme fonts need more room.
    auto Minimum = layout()->totalMinimumSize().expandedTo(
        QSize(limits::kLineageMinimumWidth, limits::kLineageMinimumHeight));
    Minimum.setHeight(
        std::max(Minimum.height(), layout()->totalMinimumHeightForWidth(
                                       std::max(width(), Minimum.width()))));
    setMinimumSize(Minimum);
  }
  return Result;
}

void LineageDialog::closeEvent(QCloseEvent *Event) {
  if (Session.busy()) {
    CloseWhenIdle = true;
    Session.cancel();
    Event->ignore();
    return;
  }
  if (Dirty) {
    const auto Choice = QMessageBox::question(
        this, tr("Save version graph?"),
        tr("Save the current graph and snapshot labels before closing?"),
        QMessageBox::Save | QMessageBox::Discard | QMessageBox::Cancel,
        QMessageBox::Save);
    if (Choice == QMessageBox::Cancel) {
      Event->ignore();
      return;
    }
    if (Choice == QMessageBox::Save) {
      CloseAfterSave = true;
      save();
      Event->ignore();
      return;
    }
  }
  QSettings().setValue("lineage/geometry", saveGeometry());
  QDialog::closeEvent(Event);
}
} // namespace neverd::gui
