//===- LineageReleaseView.cpp - Release timeline workbench ----------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Paged release metadata and parent/child navigation without copying complete
/// inventories or deriving ancestry from comparison direction.
//===----------------------------------------------------------------------===//

#include "LineageReleaseView.h"

#include "../ResourceLimits.h"
#include "ElidedLabel.h"
#include "FlowLayout.h"
#include "LineagePageView.h"
#include "LineageSession.h"
#include "ShrinkableRow.h"

#include <QCheckBox>
#include <QDateTimeEdit>
#include <QDialog>
#include <QDialogButtonBox>
#include <QEvent>
#include <QFormLayout>
#include <QGridLayout>
#include <QJsonArray>
#include <QLabel>
#include <QLineEdit>
#include <QMessageBox>
#include <QPushButton>
#include <QSignalBlocker>
#include <QTabWidget>
#include <QTableView>
#include <QTimeZone>
#include <QUuid>
#include <QVBoxLayout>
#include <algorithm>
#include <limits>
#include <utility>

namespace neverd::gui {
namespace {
namespace limits = ::neverd::limits::gui;
QPushButton *button(const QString &Text, const char *Name, QWidget *Owner) {
  auto *B = new QPushButton(Text, Owner);
  B->setObjectName(Name);
  B->setAutoDefault(false);
  return B;
}
LineagePageView *page(LineageSession &Session, const QStringList &Headers,
                      const QStringList &Keys, const char *Name,
                      QWidget *Owner) {
  auto *P = new LineagePageView(Session, Headers, Keys, Owner);
  P->setObjectName(Name);
  P->table()->setObjectName(QString::fromLatin1(Name) + "Table");
  P->table()->setAccessibleName(Headers.join(", "));
  P->table()->setShowGrid(false);
  return P;
}
} // namespace

LineageReleaseView::LineageReleaseView(LineageSession &S, QWidget *Owner)
    : QWidget(Owner), Session(S) {
  setObjectName("lineageReleaseView");
  auto *Root = new QVBoxLayout(this);
  Status = new QLabel(this);
  Status->setObjectName("lineageReleaseStatus");
  Status->setTextFormat(Qt::PlainText);
  Status->setWordWrap(true);
  makeStatusLine(*Status);
  Root->addWidget(Status);
  SelectedRelease = new ElidedLabel(this);
  Root->addWidget(SelectedRelease);
  auto *Relations = new QTabWidget(this);
  Relations->setObjectName("lineageReleaseRelations");
  Root->addWidget(Relations, 1);
  auto *Upper = new QWidget(Relations);
  auto *UL = new QVBoxLayout(Upper);
  Search = new QLineEdit(Upper);
  Search->setObjectName("lineageReleaseSearch");
  Search->setPlaceholderText(tr("Find a release or branch"));
  Search->setAccessibleName(tr("Release filter"));
  UL->addWidget(Search);
  Releases = page(Session,
                  {tr("Release"), tr("Branch"), tr("Published (UTC)"),
                   tr("Snapshot"), tr("Parents"), tr("Children")},
                  {"label", "branch", "published_at", "version_label",
                   "parent_count", "child_count"},
                  "lineageReleases", Upper);
  UL->addWidget(Releases, 1);
  auto *Actions = new QGridLayout;
  Add = button(tr("&Add release…"), "lineageReleaseAdd", Upper);
  Edit = button(tr("&Edit release…"), "lineageReleaseEdit", Upper);
  Remove = button(tr("Remove release…"), "lineageReleaseRemove", Upper);
  OpenSnapshot = button(tr("Open snapshot"), "lineageReleaseSnapshot", Upper);
  int Position = 0;
  for (auto *B : {Add, Edit, Remove, OpenSnapshot}) {
    Actions->addWidget(B, Position / 2, Position % 2);
    ++Position;
  }
  UL->addLayout(Actions);
  Relations->addTab(Upper, tr("Timeline"));
  auto *ParentPane = new QWidget(Relations);
  auto *PL = new QVBoxLayout(ParentPane);
  Parents = page(
      Session,
      {tr("Release"), tr("Branch"), tr("Published (UTC)"), tr("Release ID")},
      {"label", "branch", "published_at", "id"}, "lineageReleaseParents",
      ParentPane);
  PL->addWidget(Parents, 1);
  auto *ParentActions = new FlowLayout;
  AddParent = button(tr("Add parent…"), "lineageReleaseAddParent", ParentPane);
  RemoveParent = button(tr("Remove parent link"), "lineageReleaseRemoveParent",
                        ParentPane);
  ParentActions->addWidget(AddParent);
  ParentActions->addWidget(RemoveParent);
  PL->addLayout(ParentActions);
  Relations->addTab(ParentPane, tr("Declared parents"));
  Children = page(
      Session,
      {tr("Release"), tr("Branch"), tr("Published (UTC)"), tr("Release ID")},
      {"label", "branch", "published_at", "id"}, "lineageReleaseChildren",
      Relations);
  Relations->addTab(Children, tr("Declared children"));
  Filter.setSingleShot(true);
  Filter.setInterval(limits::kChooserViewFilterDebounceMs);
  connect(Search, &QLineEdit::textChanged, &Filter,
          qOverload<>(&QTimer::start));
  connect(&Filter, &QTimer::timeout, this, [this] {
    Releases->setQuery({{"kind", "releases"}, {"search", Search->text()}});
  });
  connect(&Session, &LineageSession::changed, this,
          &LineageReleaseView::update);
  connect(&Session, &LineageSession::releasesEdited, this,
          [this](const QString &ID) {
            Focus = ID;
            Edited = true;
          });
  connect(Releases, &LineagePageView::selectionChanged, this,
          &LineageReleaseView::selected);
  connect(Parents, &LineagePageView::selectionChanged, this,
          [this] { update(); });
  for (auto *P : {Parents, Children})
    connect(P, &LineagePageView::activated, this, [this](const QJsonObject &R) {
      const QSignalBlocker Block(Search);
      Search->clear();
      Releases->setQuery({{"kind", "releases"}, {"focus", R["id"]}},
                         R["id"].toString());
    });
  connect(Add, &QPushButton::clicked, this, [this] { edit(false); });
  connect(Edit, &QPushButton::clicked, this, [this] { edit(true); });
  connect(Releases, &LineagePageView::activated, this, [this] {
    if (Edit->isEnabled())
      edit(true);
  });
  connect(OpenSnapshot, &QPushButton::clicked, this, [this] {
    emit snapshotRequested(Releases->selected()["version"].toString());
  });
  connect(Remove, &QPushButton::clicked, this, [this] {
    const auto Row = Releases->selected();
    if (QMessageBox::question(this, tr("Remove release?"),
                              tr("Remove %1 from this graph? Its snapshot and "
                                 "saved comparisons remain available.")
                                  .arg(Row["label"].toString()),
                              QMessageBox::Yes | QMessageBox::Cancel,
                              QMessageBox::Cancel) != QMessageBox::Yes)
      return;
    Session.editReleases({{"revision", Row["lineage_revision"]},
                          {"remove", QJsonArray{Row["id"]}}},
                         Admission, {});
  });
  connect(AddParent, &QPushButton::clicked, this, [this] { parent(true); });
  connect(RemoveParent, &QPushButton::clicked, this, [this] { parent(false); });
  update();
}

bool LineageReleaseView::event(QEvent *Event) {
  const auto Result = QWidget::event(Event);
  if (isVisible() && layout() &&
      (Event->type() == QEvent::Show || Event->type() == QEvent::Resize ||
       Event->type() == QEvent::LayoutRequest))
    // The outer splitter cannot propagate height-for-width through this tab.
    // Reserve wrapped guidance here so it cannot overlap the timeline controls.
    setMinimumHeight(std::max(layout()->minimumSize().height(),
                              layout()->minimumHeightForWidth(width())));
  return Result;
}

void LineageReleaseView::update() {
  const bool Available = Session.ready() && Session.releaseEditingSupported();
  const bool Ready =
      Available && !Session.busy() && !Session.revision().isEmpty();
  const auto Row = Releases->selected();
  const bool Current = !Row.isEmpty() &&
                       Row["lineage_revision"].toString() == Session.revision();
  Search->setEnabled(Available && !Session.revision().isEmpty());
  Add->setEnabled(Ready && !Snapshot.isEmpty());
  Edit->setEnabled(Ready && Current);
  OpenSnapshot->setEnabled(Ready && Current);
  Remove->setEnabled(Ready && Current && Row["child_count"].toInteger() == 0);
  Remove->setToolTip(Row["child_count"].toInteger() > 0
                         ? tr("Remove this release's parent links from its "
                              "children before deleting it.")
                         : QString());
  AddParent->setEnabled(Ready && Current);
  RemoveParent->setEnabled(Ready && Current && !Parents->selected().isEmpty() &&
                           Parents->selected()["lineage_revision"].toString() ==
                               Session.revision());
  Status->setText(
      !Available
          ? tr("This engine does not support release timelines.")
          : tr("%1 releases · %2 declared parent links. Dates and ancestry are "
               "your declarations; patch presence is not inferred.")
                .arg(Session.summary()["releases"].toInteger())
                .arg(Session.summary()["release_links"].toInteger()));
}

void LineageReleaseView::setSnapshot(const QString &ID, const QString &Label) {
  Snapshot = ID;
  SnapshotLabel = Label;
  update();
}

void LineageReleaseView::reload(bool Preserve) {
  Filter.stop();
  const auto ID = Edited     ? Focus
                  : Preserve ? Releases->selected()["id"].toString()
                             : QString();
  RestoreRelations =
      Preserve && ID == Releases->selected()["id"].toString() ? ID : QString();
  ParentOffset = Parents->offset();
  ChildOffset = Children->offset();
  RestoreParent = Parents->selected()["id"].toString();
  RestoreChild = Children->selected()["id"].toString();
  const auto Offset =
      Preserve && Releases->offset() < Session.summary()["releases"].toInteger()
          ? Releases->offset()
          : 0;
  if (!Preserve || Edited) {
    const QSignalBlocker Block(Search);
    Search->clear();
  }
  Edited = false;
  Focus.clear();
  Releases->clear();
  Parents->clear();
  Children->clear();
  if (Session.releaseEditingSupported() && !Session.revision().isEmpty()) {
    QJsonObject Q{{"kind", "releases"}, {"search", Search->text()}};
    if (!ID.isEmpty())
      Q["focus"] = ID;
    Releases->setQuery(Q, ID, Offset);
  }
  update();
}

void LineageReleaseView::selected(const QJsonObject &Row) {
  SelectedRelease->setFullText(
      Row.isEmpty()
          ? tr("Select a release to inspect its declared parents and children.")
          : tr("Selected release: %1 · %2")
                .arg(Row["label"].toString(),
                     Row["id"].toString().left(
                         limits::kLineageDigestDisplayCharacters)));
  Parents->clear();
  Children->clear();
  if (!Row.isEmpty()) {
    const bool Restore = Row["id"].toString() == RestoreRelations;
    Parents->setQuery({{"kind", "release_parents"}, {"release", Row["id"]}},
                      Restore ? RestoreParent : QString(),
                      Restore && ParentOffset < Row["parent_count"].toInteger()
                          ? ParentOffset
                          : 0);
    Children->setQuery({{"kind", "release_children"}, {"release", Row["id"]}},
                       Restore ? RestoreChild : QString(),
                       Restore && ChildOffset < Row["child_count"].toInteger()
                           ? ChildOffset
                           : 0);
    RestoreRelations.clear();
  }
  update();
}

QJsonObject LineageReleaseView::choose(const QString &Kind,
                                       const QString &Title,
                                       const QString &Excluded) {
  QDialog D(this);
  D.setObjectName("lineageReleaseChooser");
  D.setWindowTitle(Title);
  D.resize(limits::kLineageReleaseChooserWidth,
           limits::kLineageReleaseChooserHeight);
  auto *L = new QVBoxLayout(&D);
  auto *Find = new QLineEdit(&D);
  Find->setObjectName("lineageReleaseChooserSearch");
  Find->setPlaceholderText(tr("Find a release or branch"));
  Find->setVisible(Kind == "releases");
  L->addWidget(Find);
  auto *P = page(Session, {tr("Label"), tr("Branch"), tr("ID")},
                 {"label", "branch", "id"}, "lineageReleaseChoices", &D);
  L->addWidget(P, 1);
  auto *Buttons =
      new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, &D);
  L->addWidget(Buttons);
  const auto Update = [&] {
    const auto Row = P->selected();
    Buttons->button(QDialogButtonBox::Ok)
        ->setEnabled(!Session.busy() && !Row.isEmpty() &&
                     Row["id"].toString() != Excluded &&
                     Row["lineage_revision"].toString() == Session.revision());
  };
  connect(P, &LineagePageView::selectionChanged, &D, Update);
  connect(&Session, &LineageSession::changed, &D, Update);
  connect(Buttons, &QDialogButtonBox::accepted, &D, &QDialog::accept);
  connect(Buttons, &QDialogButtonBox::rejected, &D, &QDialog::reject);
  connect(P, &LineagePageView::activated, &D, [&] {
    if (Buttons->button(QDialogButtonBox::Ok)->isEnabled())
      D.accept();
  });
  QTimer Timer;
  Timer.setSingleShot(true);
  Timer.setInterval(limits::kChooserViewFilterDebounceMs);
  connect(Find, &QLineEdit::textChanged, &Timer, qOverload<>(&QTimer::start));
  connect(&Timer, &QTimer::timeout, &D,
          [&] { P->setQuery({{"kind", Kind}, {"search", Find->text()}}); });
  P->setQuery({{"kind", Kind}});
  Update();
  return D.exec() == QDialog::Accepted ? P->selected() : QJsonObject{};
}

void LineageReleaseView::edit(bool Existing) {
  const auto Row = Existing ? Releases->selected() : QJsonObject{};
  const auto Revision = Session.revision();
  QString Version = Existing ? Row["version"].toString() : Snapshot;
  QString VersionLabel =
      Existing ? Row["version_label"].toString() : SnapshotLabel;
  QDialog D(this);
  D.setObjectName("lineageReleaseEditor");
  D.setWindowTitle(Existing ? tr("Edit release") : tr("Add release"));
  auto *L = new QFormLayout(&D);
  auto *Name = new QLineEdit(Row["label"].toString(), &D);
  Name->setObjectName("lineageReleaseLabel");
  Name->setMaxLength(std::numeric_limits<int>::max());
  auto *Branch = new QLineEdit(Row["branch"].toString(), &D);
  Branch->setObjectName("lineageReleaseBranch");
  Branch->setMaxLength(std::numeric_limits<int>::max());
  L->addRow(tr("&Label"), Name);
  L->addRow(tr("&Branch"), Branch);
  auto *SnapshotRow = new QWidget(&D);
  auto *SL = new QHBoxLayout(SnapshotRow);
  SL->setContentsMargins(0, 0, 0, 0);
  auto *Value = new ElidedLabel(SnapshotRow);
  Value->setFullText(VersionLabel);
  SL->addWidget(Value, 1);
  auto *Change =
      button(tr("Choose…"), "lineageReleaseChooseSnapshot", SnapshotRow);
  SL->addWidget(Change);
  L->addRow(tr("Snapshot"), SnapshotRow);
  connect(Change, &QPushButton::clicked, &D, [&] {
    const auto Chosen = choose("versions", tr("Choose analyzed snapshot"));
    if (!Chosen.isEmpty()) {
      Version = Chosen["id"].toString();
      Value->setFullText(Chosen["label"].toString());
    }
  });
  auto *Known = new QCheckBox(tr("Publication time is known (UTC)"), &D);
  Known->setObjectName("lineageReleaseDateKnown");
  auto *Date = new QDateTimeEdit(&D);
  Date->setObjectName("lineageReleaseDate");
  Date->setTimeZone(QTimeZone::utc());
  Date->setDateRange(QDate(1, 1, 1), QDate(9999, 12, 31));
  Date->setDisplayFormat("yyyy-MM-dd HH:mm:ss");
  Date->setCalendarPopup(true);
  const auto Published = Row["published_at"].toString();
  Known->setChecked(!Published.isEmpty());
  Date->setDateTime(Published.isEmpty()
                        ? QDateTime::currentDateTimeUtc()
                        : QDateTime::fromString(Published, Qt::ISODate));
  Date->setEnabled(Known->isChecked());
  connect(Known, &QCheckBox::toggled, Date, &QWidget::setEnabled);
  L->addRow(Known);
  L->addRow(tr("Published (UTC)"), Date);
  auto *Buttons = new QDialogButtonBox(
      QDialogButtonBox::Save | QDialogButtonBox::Cancel, &D);
  L->addRow(Buttons);
  const auto Validate = [&] {
    Buttons->button(QDialogButtonBox::Save)
        ->setEnabled(!Name->text().trimmed().isEmpty() && !Version.isEmpty() &&
                     !Session.busy());
  };
  connect(Name, &QLineEdit::textChanged, &D, Validate);
  connect(&Session, &LineageSession::changed, &D, Validate);
  connect(Buttons, &QDialogButtonBox::accepted, &D, &QDialog::accept);
  connect(Buttons, &QDialogButtonBox::rejected, &D, &QDialog::reject);
  Validate();
  if (D.exec() != QDialog::Accepted)
    return;
  const auto ID = Existing ? Row["id"].toString()
                           : QUuid::createUuid().toString(QUuid::WithoutBraces);
  QJsonObject Record{
      {"id", ID},
      {"label", Name->text()},
      {"branch", Branch->text()},
      {"version", Version},
      {"published_at",
       Known->isChecked()
           ? QJsonValue(Date->dateTime().toUTC().toString(Qt::ISODate))
           : QJsonValue(QJsonValue::Null)}};
  if (!Existing)
    Record["parents"] = QJsonArray{};
  Session.editReleases({{"revision", Revision}, {"upsert", QJsonArray{Record}}},
                       Admission, ID);
}

void LineageReleaseView::parent(bool Adding) {
  const auto Release = Releases->selected();
  const auto ID = Release["id"].toString();
  const auto Chosen = Adding
                          ? choose("releases", tr("Choose parent release"), ID)
                          : Parents->selected();
  if (Chosen.isEmpty())
    return;
  Session.editReleases(
      {{"revision", Release["lineage_revision"]},
       {"parent_updates", QJsonArray{QJsonObject{{"release", ID},
                                                 {Adding ? "add" : "remove",
                                                  QJsonArray{Chosen["id"]}}}}}},
      Admission, ID);
}
} // namespace neverd::gui
