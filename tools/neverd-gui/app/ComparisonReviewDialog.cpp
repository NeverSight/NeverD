//===- ComparisonReviewDialog.cpp - Manual review without inferred proof --===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Every append is an explicit user action. Mapping edits retain history; draft
/// text survives navigation but cannot silently bind to different inputs/pairs.
//===----------------------------------------------------------------------===//

#include "ComparisonReviewDialog.h"

#include "../ResourceLimits.h"
#include "ComparisonModel.h"
#include "ComparisonSession.h"
#include "Icons.h"

#include "neverd/diff/Limits.h"

#include <QCheckBox>
#include <QComboBox>
#include <QGridLayout>
#include <QHeaderView>
#include <QItemSelectionModel>
#include <QLabel>
#include <QLineEdit>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QSplitter>
#include <QTabWidget>
#include <QTableView>
#include <QVBoxLayout>
#include <utility>

namespace neverd::gui {
ComparisonReviewDialog::ComparisonReviewDialog(ComparisonSession &S,
                                               QWidget *Parent)
    : QDialog(Parent), Session(S), Revision(S.revision()) {
  setObjectName("comparisonReviewDialog");
  setWindowTitle(tr("Analyst review — NeverD"));
  setWindowIcon(icon("compare"));
  resize(::neverd::limits::gui::kComparisonReviewInitialWidth,
         ::neverd::limits::gui::kComparisonReviewInitialHeight);
  setMinimumSize(::neverd::limits::gui::kComparisonReviewMinimumWidth,
                 ::neverd::limits::gui::kComparisonReviewMinimumHeight);
  auto *Root = new QVBoxLayout(this);
  auto *Filters = new QGridLayout;
  CaseFilter = new QLineEdit(this);
  CaseFilter->setObjectName("reviewCaseFilter");
  CaseFilter->setPlaceholderText(
      tr("Exact case identifier (empty: all cases)"));
  CaseFilter->setAccessibleName(tr("Case filter"));
  CaseFilter->setMaxLength(::neverd::limits::diff::kMaxReviewCaseBytes);
  Latest = new QCheckBox(tr("Latest assessments only"), this);
  Latest->setObjectName("reviewLatest");
  Refresh = new QPushButton(tr("Refresh history"), this);
  Refresh->setObjectName("reviewRefresh");
  Filters->addWidget(CaseFilter, 0, 0);
  Filters->addWidget(Latest, 0, 1);
  Filters->addWidget(Refresh, 0, 2);
  Filters->setColumnStretch(0, 1);
  Root->addLayout(Filters);
  Summary = new QLabel(this);
  Summary->setObjectName("reviewSummary");
  Summary->setWordWrap(true);
  Root->addWidget(Summary);
  auto *Split = new QSplitter(Qt::Vertical, this);
  Split->setChildrenCollapsible(false);
  History = new ComparisonModel(Session, ComparisonModel::Kind::Reviews, this);
  Table = new QTableView(Split);
  Table->setObjectName("reviewHistory");
  Table->setModel(History);
  Table->setSelectionBehavior(QAbstractItemView::SelectRows);
  Table->setSelectionMode(QAbstractItemView::SingleSelection);
  Table->setEditTriggers(QAbstractItemView::NoEditTriggers);
  Table->setAlternatingRowColors(true);
  Table->setWordWrap(false);
  Table->verticalHeader()->hide();
  Table->horizontalHeader()->setStretchLastSection(true);
  Table->setColumnWidth(0, 70);
  Table->setColumnWidth(1, 170);
  Table->setColumnWidth(2, 150);
  Table->setColumnWidth(3, 150);
  Table->setColumnWidth(4, 220);
  auto *Tabs = new QTabWidget(Split);
  Tabs->setObjectName("reviewTabs");
  auto *Form = new QWidget(Tabs);
  auto *FormLayout = new QVBoxLayout(Form);
  Target = new QLabel(Form);
  Target->setObjectName("reviewTarget");
  Target->setWordWrap(true);
  FormLayout->addWidget(Target);
  auto *Fields = new QGridLayout;
  const auto Edit = [&](const QString &Title, const char *Name, int Row,
                        int Column, int Max) {
    auto *E = new QLineEdit(Form);
    E->setObjectName(Name);
    E->setMaxLength(Max);
    auto *L = new QLabel(Title, Form);
    L->setBuddy(E);
    Fields->addWidget(L, Row, Column);
    Fields->addWidget(E, Row, Column + 1);
    return E;
  };
  Case = Edit(tr("Case"), "reviewCase", 0, 0,
              ::neverd::limits::diff::kMaxReviewCaseBytes);
  Author = Edit(tr("Author"), "reviewAuthor", 0, 2,
                ::neverd::limits::diff::kMaxReviewAuthorBytes);
  Assessment = new QComboBox(Form);
  Assessment->setObjectName("reviewAssessment");
  for (const auto &[Title, Value] :
       {std::pair{tr("Investigating"), "investigating"},
        {tr("Relevant change"), "relevant_change"},
        {tr("Unrelated change"), "unrelated_change"},
        {tr("Candidate fix"), "candidate_fix"},
        {tr("Patch present"), "patch_present"},
        {tr("Patch absent"), "patch_absent"},
        {tr("Partial patch"), "partial_patch"},
        {tr("Reverted patch"), "reverted_patch"},
        {tr("Inconclusive"), "inconclusive"}})
    Assessment->addItem(Title, Value);
  auto *AssessmentLabel = new QLabel(tr("Assessment"), Form);
  AssessmentLabel->setBuddy(Assessment);
  Fields->addWidget(AssessmentLabel, 1, 0);
  Fields->addWidget(Assessment, 1, 1);
  Reference = Edit(tr("Reference"), "reviewReference", 1, 2,
                   ::neverd::limits::diff::kMaxReviewReferenceBytes);
  FormLayout->addLayout(Fields);
  Note = new QPlainTextEdit(Form);
  Note->setObjectName("reviewNote");
  Note->setPlaceholderText(
      tr("Record the evidence and limitations behind your assessment."));
  Note->setAccessibleName(tr("Assessment note"));
  FormLayout->addWidget(Note, 1);
  auto *Buttons = new QHBoxLayout;
  NewDraft = new QPushButton(tr("New draft for selected pair"), Form);
  NewDraft->setObjectName("reviewNewDraft");
  Append = new QPushButton(tr("Append assessment"), Form);
  Append->setObjectName("reviewAppend");
  Buttons->addWidget(NewDraft);
  Buttons->addStretch();
  Buttons->addWidget(Append);
  FormLayout->addLayout(Buttons);
  Tabs->addTab(Form, tr("New assessment"));
  Record = new QPlainTextEdit(Tabs);
  Record->setObjectName("reviewRecord");
  Record->setReadOnly(true);
  Tabs->addTab(Record, tr("Selected record"));
  Root->addWidget(Split, 1);
  auto *Footer = new QHBoxLayout;
  auto *Scope = new QLabel(tr("Analyst statements do not change computed "
                              "matching or validate patch behavior. "
                              "Authors are self-described. Save/export the "
                              "comparison to keep this history on disk."),
                           this);
  Scope->setWordWrap(true);
  Footer->addWidget(Scope, 1);
  Save = new QPushButton(tr("Save comparison…"), this);
  Save->setObjectName("reviewSave");
  Footer->addWidget(Save);
  Root->addLayout(Footer);
  connect(Save, &QPushButton::clicked, this,
          &ComparisonReviewDialog::saveComparison);
  connect(Refresh, &QPushButton::clicked, this,
          &ComparisonReviewDialog::reload);
  connect(CaseFilter, &QLineEdit::returnPressed, this,
          &ComparisonReviewDialog::reload);
  connect(Latest, &QCheckBox::toggled, this, [this] { reload(); });
  connect(NewDraft, &QPushButton::clicked, this,
          &ComparisonReviewDialog::newDraft);
  connect(Append, &QPushButton::clicked, this, &ComparisonReviewDialog::append);
  for (auto *E : {Case, Author, Reference})
    connect(E, &QLineEdit::textChanged, this, [this] { controls(); });
  connect(Note, &QPlainTextEdit::textChanged, this, [this] { controls(); });
  connect(
      Table->selectionModel(), &QItemSelectionModel::currentRowChanged, this,
      [this] {
        const auto R = History->row(Table->currentIndex().row());
        Record->setPlainText(
            R.isEmpty()
                ? QString()
                : tr("Case: %1\nAuthor (self-described): %2\nAssessment: "
                     "%3\nPair: %4 ↔ %5\n"
                     "Pair binding: %6 · %7 assessment\nReference: "
                     "%8\n\n%9\n\nEvent: %10\nSupersedes: %11")
                      .arg(R["case"].toString(), R["author"].toString(),
                           R["assessment"].toString().replace('_', ' '),
                           R["primary"].toString(), R["secondary"].toString(),
                           R["pair_active"].toBool() ? tr("accepted")
                                                     : tr("inactive"),
                           R["latest"].toBool() ? tr("latest") : tr("earlier"),
                           R["reference"].toString(), R["note"].toString())
                      .arg(R["id"].toString(), R["supersedes"].toString()));
      });
  connect(History, &ComparisonModel::pageReady, this,
          [this](const QJsonObject &Page) {
            Head = Page["head"].toString();
            const auto Selection = Page["selection"].toObject();
            PrimaryAddress =
                Selection["primary"].toObject()["address"].toString();
            SecondaryAddress =
                Selection["secondary"].toObject()["address"].toString();
            PairActive = Selection["pair_active"].toBool();
            if (DraftBinding.isEmpty()) {
              DraftBinding = binding();
              DraftPrimary = PrimaryAddress;
              DraftSecondary = SecondaryAddress;
            }
            Summary->setText(tr("%1 matching / %2 total events · newest first "
                                "· manual assessments")
                                 .arg(Page["total"].toInt())
                                 .arg(Page["record_count"].toInt()));
            if (History->rowCount() && !Table->currentIndex().isValid())
              Table->setCurrentIndex(Table->model()->index(0, 0));
            controls();
          });
  connect(History, &ComparisonModel::failed, this,
          [this](const QString &Error) {
            PairActive = false;
            Summary->setText(Error);
            controls();
          });
  connect(&Session, &ComparisonSession::failed, this,
          [this](const QString &Error) { Summary->setText(Error); });
  connect(&Session, &ComparisonSession::changed, this, [this] {
    if (Revision != Session.revision()) {
      Revision = Session.revision();
      if (Recording)
        Note->clear();
      Recording = false;
      reload();
    } else if (Recording && !Session.busy()) {
      Recording = false;
    }
    controls();
  });
  controls();
}
QString ComparisonReviewDialog::binding() const {
  const auto S = Session.summary();
  return S["primary"].toObject()["sha256"].toString() + ':' +
         S["primary"].toObject()["revision"].toString() + ':' +
         S["secondary"].toObject()["sha256"].toString() + ':' +
         S["secondary"].toObject()["revision"].toString();
}
void ComparisonReviewDialog::review(const QString &Side,
                                    const QString &Address) {
  Query = Address.isEmpty() ? QJsonObject()
                            : QJsonObject{{"side", Side}, {"address", Address}};
  if (Case->text().isEmpty() && Note->toPlainText().isEmpty() &&
      Reference->text().isEmpty())
    DraftBinding.clear();
  reload();
}
void ComparisonReviewDialog::reload() {
  PairActive = false;
  Record->clear();
  auto Request = Query;
  Request["case"] = CaseFilter->text();
  Request["latest"] = Latest->isChecked();
  History->setQuery(Request);
  Summary->setText(tr("Loading analyst history…"));
  controls();
}
void ComparisonReviewDialog::newDraft() {
  DraftBinding = binding();
  DraftPrimary = PrimaryAddress;
  DraftSecondary = SecondaryAddress;
  Case->clear();
  Reference->clear();
  Note->clear();
  Assessment->setCurrentIndex(0);
  controls();
}
void ComparisonReviewDialog::controls() {
  const bool Idle =
      Session.ready() && !Session.busy() && !Session.cancellable();
  const bool Bound = DraftBinding == binding() &&
                     DraftPrimary == PrimaryAddress &&
                     DraftSecondary == SecondaryAddress;
  const bool Fits = Case->text().toUtf8().size() <=
                        ::neverd::limits::diff::kMaxReviewCaseBytes &&
                    Author->text().toUtf8().size() <=
                        ::neverd::limits::diff::kMaxReviewAuthorBytes &&
                    Reference->text().toUtf8().size() <=
                        ::neverd::limits::diff::kMaxReviewReferenceBytes &&
                    Note->toPlainText().toUtf8().size() <=
                        ::neverd::limits::diff::kMaxReviewNoteBytes;
  Append->setEnabled(Idle && PairActive && Bound && Fits &&
                     !Case->text().trimmed().isEmpty() &&
                     !Author->text().trimmed().isEmpty() &&
                     !Note->toPlainText().trimmed().isEmpty());
  NewDraft->setEnabled(Idle && PairActive);
  Refresh->setEnabled(Idle && !Session.revision().isEmpty());
  Save->setEnabled(Idle && !Session.revision().isEmpty());
  CaseFilter->setEnabled(Idle);
  Latest->setEnabled(Idle);
  Target->setText(
      !Fits    ? tr("Draft exceeds the UTF-8 text allowance.")
      : !Bound ? tr("Draft retained for %1 ↔ %2 on its original inputs. Choose "
                    "New draft to use the selected pair.")
                     .arg(DraftPrimary, DraftSecondary)
      : !PairActive
          ? tr("Select an accepted function pair to append an assessment.")
          : tr("Manual assessment of %1 ↔ %2")
                .arg(DraftPrimary, DraftSecondary));
}
void ComparisonReviewDialog::append() {
  if (!Append->isEnabled())
    return;
  Recording = true;
  Session.command("comparison_review_record",
                  {{"primary", DraftPrimary},
                   {"secondary", DraftSecondary},
                   {"case", Case->text()},
                   {"author", Author->text()},
                   {"note", Note->toPlainText()},
                   {"reference", Reference->text()},
                   {"assessment", Assessment->currentData().toString()},
                   {"expected_head", Head}});
}
} // namespace neverd::gui
