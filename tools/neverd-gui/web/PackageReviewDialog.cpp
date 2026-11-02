//===- PackageReviewDialog.cpp - Local evidence and review controls ------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Present explicit capture and evidence selections, immutable classifications
/// and caller assertions without executing targets or disclosing source.
///
//===----------------------------------------------------------------------===//

#include "PackageReviewDialog.h"

#include <QComboBox>
#include <QCoreApplication>
#include <QFileDialog>
#include <QFontInfo>
#include <QFormLayout>
#include <QGroupBox>
#include <QHeaderView>
#include <QJsonDocument>
#include <QLabel>
#include <QLineEdit>
#include <QMessageBox>
#include <QPlainTextEdit>
#include <QProgressBar>
#include <QPushButton>
#include <QSortFilterProxyModel>
#include <QSplitter>
#include <QTableView>
#include <QVBoxLayout>

namespace neverd::gui::web {
namespace {
QString selection(const QComboBox *Combo) {
  return Combo->currentData().toString();
}
void fill(QComboBox *Combo, const QJsonArray &Rows, bool Optional) {
  Combo->clear();
  if (Optional)
    Combo->addItem(QCoreApplication::translate(
                       "neverd::gui::web::PackageReviewDialog", "Not selected"),
                   QString());
  for (qsizetype I = 0; I < Rows.size(); ++I) {
    const auto Row = Rows[I].toObject();
    const auto ID = Row.contains("member_id") ? Row["member_id"].toString()
                                              : Row["artifact_id"].toString();
    if (ID.isEmpty() || Row["kind"] == QJsonValue("directory") ||
        Row["availability"] == QJsonValue("metadata_only"))
      continue;
    Combo->addItem(QStringLiteral("%1 · %2 · %3")
                       .arg(I)
                       .arg(Row["kind"].toString(), ID.left(16)),
                   ID);
    Combo->setItemData(Combo->count() - 1, ID, Qt::ToolTipRole);
  }
}
} // namespace

PackageReviewDialog::PackageReviewDialog(const QString &WorkerPath,
                                         QWidget *Parent)
    : QDialog(Parent), Session(WorkerPath, this), Model(this) {
  setObjectName(QStringLiteral("offlinePackageReview"));
  setWindowTitle(tr("Offline package review"));
  setWindowFlags((windowFlags() | Qt::WindowMinMaxButtonsHint) &
                 ~Qt::WindowContextHelpButtonHint);
  setAttribute(Qt::WA_DeleteOnClose);
  resize(1100, 780);
  setMinimumSize(640, 480);
  auto Font = font();
  Font.setPointSizeF(qMax(12.0, QFontInfo(Font).pointSizeF()));
  setFont(Font);
  auto *Layout = new QVBoxLayout(this);
  Coverage =
      new QLabel(tr("Evidence coverage is partial. Analyst dispositions are "
                    "caller assertions; package safety, publisher "
                    "authentication and runtime behavior are not verified."),
                 this);
  Coverage->setWordWrap(true);
  Coverage->setTextFormat(Qt::PlainText);
  Coverage->setTextInteractionFlags(Qt::TextSelectableByKeyboard |
                                    Qt::TextSelectableByMouse);
  Layout->addWidget(Coverage);

  auto *Capture = new QHBoxLayout;
  CaptureFile = new QPushButton(tr("Preview file…"), this);
  CaptureDirectory = new QPushButton(tr("Preview directory…"), this);
  Commit = new QPushButton(tr("Capture preview"), this);
  for (auto *Button : {CaptureFile, CaptureDirectory, Commit})
    Capture->addWidget(Button);
  Capture->addStretch();
  Layout->addLayout(Capture);
  connect(CaptureFile, &QPushButton::clicked, this, [this] {
    const auto Path =
        QFileDialog::getOpenFileName(this, tr("Select local evidence"));
    if (!Path.isEmpty())
      Session.previewCapture(Path);
  });
  connect(CaptureDirectory, &QPushButton::clicked, this, [this] {
    const auto Path = QFileDialog::getExistingDirectory(
        this, tr("Select local evidence directory"));
    if (!Path.isEmpty())
      Session.previewCapture(Path);
  });
  connect(Commit, &QPushButton::clicked, this, [this] {
    if (discardUnsaved())
      Session.commitPreview();
  });

  auto *Selection = new QFormLayout;
  Selection->setRowWrapPolicy(QFormLayout::WrapLongRows);
  Artifact = new QComboBox(this);
  Artifact->setObjectName(QStringLiteral("packageArtifact"));
  Container = new QComboBox(this);
  Container->setObjectName(QStringLiteral("packageContainer"));
  Container->addItem(tr("Loose manifest / lock file"), QStringLiteral("root"));
  for (const auto *Format : {"tgz", "tar", "zip"})
    Container->addItem(QString::fromLatin1(Format),
                       QString::fromLatin1(Format));
  auto *ArchiveRow = new QHBoxLayout;
  ArchiveRow->addWidget(Container);
  Extract = new QPushButton(tr("Inspect archive"), this);
  Extract->setObjectName(QStringLiteral("packageExtract"));
  ArchiveRow->addWidget(Extract);
  Member = new QComboBox(this);
  Member->setObjectName(QStringLiteral("packageMember"));
  Kind = new QComboBox(this);
  for (const auto *Input : {"package-json", "npm-lock"})
    Kind->addItem(QString::fromLatin1(Input), QString::fromLatin1(Input));
  Selection->addRow(tr("Captured artifact"), Artifact);
  Selection->addRow(tr("Container"), ArchiveRow);
  Selection->addRow(tr("Manifest member"), Member);
  Selection->addRow(tr("Manifest format"), Kind);
  Layout->addLayout(Selection);
  const auto ChangedArchive = [this] {
    Member->clear();
    updateState();
  };
  connect(Container, &QComboBox::currentIndexChanged, this, ChangedArchive);
  connect(Artifact, &QComboBox::currentIndexChanged, this, ChangedArchive);
  connect(Member, &QComboBox::currentIndexChanged, this,
          &PackageReviewDialog::updateState);
  connect(Extract, &QPushButton::clicked, this, [this] {
    if (discardUnsaved())
      Session.extractArchive(selection(Artifact), selection(Container));
  });

  Registry = new QGroupBox(tr("Include captured registry declarations"), this);
  Registry->setObjectName(QStringLiteral("packageRegistry"));
  Registry->setCheckable(true);
  Registry->setChecked(false);
  auto *RegistryOuter = new QVBoxLayout(Registry);
  auto *RegistryFields = new QWidget(Registry);
  RegistryOuter->addWidget(RegistryFields);
  auto *RegistryLayout = new QFormLayout(RegistryFields);
  RegistryLayout->setRowWrapPolicy(QFormLayout::WrapLongRows);
  RegistryOriginal = new QComboBox(Registry);
  RegistryOriginal->setObjectName(QStringLiteral("packageRegistryOriginal"));
  RegistryMetadata = new QComboBox(Registry);
  RegistryMetadata->setObjectName(QStringLiteral("packageRegistryMetadata"));
  RegistryDescriptor = new QComboBox(Registry);
  RegistryDescriptor->setObjectName(
      QStringLiteral("packageRegistryDescriptor"));
  RegistryLayout->addRow(tr("Original bytes"), RegistryOriginal);
  RegistryLayout->addRow(tr("Registry metadata"), RegistryMetadata);
  RegistryLayout->addRow(tr("Capture descriptor"), RegistryDescriptor);
  RegistryFields->setVisible(false);
  connect(Registry, &QGroupBox::toggled, RegistryFields, &QWidget::setVisible);
  Layout->addWidget(Registry);
  Analyze = new QPushButton(tr("Assess selected evidence"), this);
  Analyze->setObjectName(QStringLiteral("packageAssess"));
  Layout->addWidget(Analyze);
  connect(Analyze, &QPushButton::clicked, this, [this] {
    if (!discardUnsaved())
      return;
    RegistrySelection R;
    if (Registry->isChecked())
      R = {selection(RegistryOriginal), selection(RegistryMetadata),
           selection(RegistryDescriptor)};
    Session.assess(manifestID(), selection(Kind), R);
  });

  auto *Search = new QLineEdit(this);
  Search->setObjectName(QStringLiteral("packageFindingFilter"));
  Search->setPlaceholderText(
      tr("Filter evidence, confidence, disposition or ID"));
  Search->setAccessibleName(tr("Filter findings"));
  Layout->addWidget(Search);
  Filter = new QSortFilterProxyModel(this);
  Filter->setSourceModel(&Model);
  Filter->setFilterKeyColumn(-1);
  Filter->setFilterCaseSensitivity(Qt::CaseInsensitive);
  connect(Search, &QLineEdit::textChanged, Filter,
          &QSortFilterProxyModel::setFilterFixedString);
  auto *Splitter = new QSplitter(Qt::Vertical, this);
  Table = new QTableView(Splitter);
  Table->setObjectName(QStringLiteral("packageFindings"));
  Table->setModel(Filter);
  Table->setSelectionBehavior(QAbstractItemView::SelectRows);
  Table->setSelectionMode(QAbstractItemView::SingleSelection);
  Table->setSortingEnabled(true);
  Table->horizontalHeader()->setSectionResizeMode(QHeaderView::Interactive);
  for (int I = 0; I < Model.columnCount(); ++I)
    Table->setColumnWidth(
        I, qMax(110, Table->fontMetrics().horizontalAdvance(
                         Model.headerData(I, Qt::Horizontal, Qt::DisplayRole)
                             .toString()) +
                         30));
  Table->horizontalHeader()->setStretchLastSection(true);
  Details = new QPlainTextEdit(Splitter);
  Details->setObjectName(QStringLiteral("packageFindingDetails"));
  Details->setReadOnly(true);
  Details->setAccessibleName(tr("Selected finding evidence anchors"));
  Splitter->setStretchFactor(0, 3);
  Splitter->setStretchFactor(1, 1);
  Layout->addWidget(Splitter, 1);
  connect(Table->selectionModel(), &QItemSelectionModel::currentChanged, this,
          [this] {
            showFinding();
            updateState();
          });

  auto *ReviewRow = new QHBoxLayout;
  Disposition = new QComboBox(this);
  Disposition->setObjectName(QStringLiteral("packageDisposition"));
  Disposition->addItem(tr("Unreviewed"), QStringLiteral("unreviewed"));
  Disposition->addItem(tr("Needs review"), QStringLiteral("needs_review"));
  Disposition->addItem(tr("Accepted risk (analyst assertion)"),
                       QStringLiteral("accepted_risk"));
  Disposition->addItem(tr("False positive (analyst assertion)"),
                       QStringLiteral("false_positive"));
  Apply = new QPushButton(tr("Record disposition"), this);
  ReviewRow->addWidget(Disposition);
  ReviewRow->addWidget(Apply);
  ReviewRow->addStretch();
  Layout->addLayout(ReviewRow);
  connect(Apply, &QPushButton::clicked, this, [this] {
    Session.review(selectedFinding()["finding_id"].toString(),
                   selection(Disposition));
  });

  auto *JournalRow = new QHBoxLayout;
  Save = new QPushButton(tr("Save review…"), this);
  Save->setObjectName(QStringLiteral("packageSave"));
  Load = new QPushButton(tr("Load review…"), this);
  Restore = new QPushButton(tr("Restore loaded review"), this);
  Cancel = new QPushButton(tr("Stop worker"), this);
  for (auto *Button : {Save, Load, Restore})
    JournalRow->addWidget(Button);
  JournalRow->addStretch();
  JournalRow->addWidget(Cancel);
  Layout->addLayout(JournalRow);
  connect(Save, &QPushButton::clicked, this, &PackageReviewDialog::saveJournal);
  connect(Load, &QPushButton::clicked, this, &PackageReviewDialog::loadJournal);
  connect(Restore, &QPushButton::clicked, this,
          [this] { Session.restore(LoadedJournal); });
  connect(Cancel, &QPushButton::clicked, &Session, &PackageSession::cancel);
  JournalStatus = new QLabel(this);
  JournalStatus->setWordWrap(true);
  JournalStatus->setTextFormat(Qt::PlainText);
  Layout->addWidget(JournalStatus);
  Status = new QLabel(tr("Starting NeverD analysis worker…"), this);
  Status->setWordWrap(true);
  Status->setTextFormat(Qt::PlainText);
  Layout->addWidget(Status);
  Progress = new QProgressBar(this);
  Progress->setRange(0, 0);
  Progress->setAccessibleName(tr("Offline evidence analysis in progress"));
  Layout->addWidget(Progress);
  connect(&Session, &PackageSession::changed, this,
          &PackageReviewDialog::updateState);
  connect(&Session, &PackageSession::artifactsReady, this,
          &PackageReviewDialog::fillArtifacts);
  connect(&Session, &PackageSession::membersReady, this,
          &PackageReviewDialog::fillMembers);
  connect(&Session, &PackageSession::assessmentReady, this,
          &PackageReviewDialog::showAssessment);
  updateState();
}

QString PackageReviewDialog::manifestID() const {
  return selection(Container) == u"root" ? selection(Artifact)
                                         : selection(Member);
}
QJsonObject PackageReviewDialog::selectedFinding() const {
  return Model.finding(Filter->mapToSource(Table->currentIndex()).row());
}
void PackageReviewDialog::fillArtifacts() {
  for (auto *Combo :
       {Artifact, RegistryOriginal, RegistryMetadata, RegistryDescriptor})
    fill(Combo, Session.artifacts(), Combo != Artifact);
  Member->clear();
  Model.setRows({});
  Details->clear();
  SavedHead.clear();
  updateState();
}
void PackageReviewDialog::fillMembers() {
  fill(Member, Session.members(), false);
  updateState();
}
void PackageReviewDialog::showAssessment() {
  if (LoadedJournal["package_assessment_id"] ==
          Session.assessment()["package_assessment_id"] &&
      LoadedJournal["review_head_id"] == Session.assessment()["review_head_id"])
    SavedHead = LoadedJournal["review_head_id"].toString();
  Model.setRows(Session.findings());
  if (Model.rowCount() > 0)
    Table->selectRow(0);
  showFinding();
  updateState();
}
void PackageReviewDialog::showFinding() {
  Details->setPlainText(QString::fromUtf8(
      QJsonDocument(selectedFinding()).toJson(QJsonDocument::Indented)));
}
void PackageReviewDialog::updateState() {
  const bool Idle = Session.ready() && !Session.busy();
  const bool HasAssessment =
      !Session.assessment().isEmpty() && !Session.findings().isEmpty();
  CaptureFile->setEnabled(Idle);
  CaptureDirectory->setEnabled(Idle);
  Commit->setEnabled(Idle && !Session.preview().isEmpty());
  Artifact->setEnabled(Idle);
  Container->setEnabled(Idle);
  Member->setEnabled(Idle && selection(Container) != u"root");
  Kind->setEnabled(Idle);
  Registry->setEnabled(Idle);
  Extract->setEnabled(Idle && !selection(Artifact).isEmpty() &&
                      selection(Container) != u"root");
  Analyze->setEnabled(Idle && !manifestID().isEmpty());
  Apply->setEnabled(Idle && HasAssessment && !selectedFinding().isEmpty());
  Save->setEnabled(!Session.busy() && HasAssessment);
  Load->setEnabled(!Session.busy());
  Restore->setEnabled(Idle && HasAssessment && Session.canRestore() &&
                      !LoadedJournal.isEmpty());
  Cancel->setEnabled(Session.ready() || Session.busy());
  Progress->setVisible(Session.busy());
  if (!Session.error().isEmpty())
    Status->setText(tr("Operation refused: %1").arg(Session.error()));
  else if (Session.busy())
    Status->setText(tr("Reading bounded offline evidence…"));
  else if (!Session.preview().isEmpty())
    Status->setText(tr("Preview: %1 artifacts, %2 bytes. Capture reads local "
                       "bytes; source and names remain private.")
                        .arg(locale().toString(
                            Session.preview()["artifact_count"].toInteger()))
                        .arg(Session.preview()["input_bytes"].toString()));
  else if (HasAssessment)
    Status->setText(
        tr("%1 findings · %2 analyst decisions · partial coverage · head %3")
            .arg(locale().toString(Session.findings().size()))
            .arg(locale().toString(
                Session.assessment()["decision_count"].toInteger()))
            .arg(Session.assessment()["review_head_id"].toString().left(16)));
  else if (Session.ready())
    Status->setText(tr("Select local evidence to preview or assess."));
}
void PackageReviewDialog::saveJournal() {
  const auto Path = QFileDialog::getSaveFileName(
      this, tr("Save local review"), {}, tr("NeverD review (*.ndreview)"));
  if (Path.isEmpty())
    return;
  QString Error;
  if (!ReviewJournal::save(Path, Session.journal(), Error)) {
    JournalStatus->setText(tr("Review save refused: %1").arg(Error));
    return;
  }
  SavedHead = Session.assessment()["review_head_id"].toString();
  JournalStatus->setText(
      tr("Saved opaque evidence IDs and analyst assertions locally."));
}
void PackageReviewDialog::loadJournal() {
  const auto Path = QFileDialog::getOpenFileName(
      this, tr("Load local review"), {}, tr("NeverD review (*.ndreview)"));
  if (Path.isEmpty())
    return;
  QString Error;
  const auto Journal = ReviewJournal::load(Path, Error);
  if (!Journal) {
    JournalStatus->setText(tr("Review load refused: %1").arg(Error));
    return;
  }
  LoadedJournal = *Journal;
  JournalStatus->setText(
      tr("Loaded %1 assertions. Restore validates the entire chain against the "
         "current assessment; nothing has been applied.")
          .arg(locale().toString(LoadedJournal["decisions"].toArray().size())));
  updateState();
}
bool PackageReviewDialog::discardUnsaved() {
  return Session.assessment()["decision_count"].toInteger() == 0 ||
         Session.assessment()["review_head_id"].toString() == SavedHead ||
         QMessageBox::question(
             this, tr("Unsaved review"),
             tr("Discard unsaved analyst assertions and continue?"),
             QMessageBox::Discard | QMessageBox::Cancel,
             QMessageBox::Cancel) == QMessageBox::Discard;
}
void PackageReviewDialog::done(int Result) {
  if (!discardUnsaved())
    return;
  Session.cancel();
  QDialog::done(Result);
}
} // namespace neverd::gui::web
