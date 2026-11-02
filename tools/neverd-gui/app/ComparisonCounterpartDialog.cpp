//===- ComparisonCounterpartDialog.cpp - Searchable pairing --------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Full retained ambiguity groups and all-function search share one bounded
/// page cache. Mapping changes invalidate the selection before it can be used.
//===----------------------------------------------------------------------===//

#include "ComparisonCounterpartDialog.h"

#include "../ResourceLimits.h"
#include "ComparisonModel.h"
#include "ComparisonSession.h"
#include "Icons.h"

#include <QComboBox>
#include <QDialogButtonBox>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QItemSelectionModel>
#include <QJsonObject>
#include <QLabel>
#include <QLineEdit>
#include <QLocale>
#include <QPushButton>
#include <QTableView>
#include <QVBoxLayout>
#include <utility>

namespace neverd::gui {
ComparisonCounterpartDialog::ComparisonCounterpartDialog(
    ComparisonSession &S, const QString &Side, const QString &Address,
    const QString &Counterpart, QWidget *Parent)
    : QDialog(Parent), Session(S), SourceSide(Side), SourceAddress(Address),
      CurrentCounterpart(Counterpart), Revision(S.revision()) {
  setObjectName("comparisonCounterpartDialog");
  setWindowTitle(tr("Choose counterpart — NeverD"));
  setWindowIcon(icon("compare"));
  setSizeGripEnabled(true);
  resize(::neverd::limits::gui::kComparisonCounterpartInitialWidth,
         ::neverd::limits::gui::kComparisonCounterpartInitialHeight);
  setMinimumSize(::neverd::limits::gui::kComparisonCounterpartMinimumWidth,
                 ::neverd::limits::gui::kComparisonCounterpartMinimumHeight);
  auto *Root = new QVBoxLayout(this);
  const auto Label = [&](const char *Name) {
    auto *L = new QLabel(this);
    L->setObjectName(Name);
    L->setTextFormat(Qt::PlainText);
    L->setWordWrap(true);
    L->setTextInteractionFlags(Qt::TextSelectableByMouse);
    Root->addWidget(L);
    return L;
  };
  Source = Label("counterpartSource");
  Source->setText(
      tr("Choose a %1 counterpart for %2.")
          .arg(SourceSide == "primary" ? tr("secondary") : tr("primary"),
               SourceAddress));
  auto *Filters = new QHBoxLayout;
  Scope = new QComboBox(this);
  Scope->setObjectName("counterpartScope");
  Scope->setAccessibleName(tr("Candidate scope"));
  Scope->addItem(tr("Ambiguity candidates"), true);
  Scope->addItem(tr("All functions"), false);
  Filters->addWidget(Scope);
  Sort = new QComboBox(this);
  Sort->setObjectName("counterpartSort");
  Sort->setAccessibleName(tr("Sort functions"));
  for (const auto &[Text, Key] :
       {std::pair{tr("Address: low to high"), "address"},
        {tr("Address: high to low"), "address"},
        {tr("Name: A to Z"), "name"},
        {tr("Name: Z to A"), "name"},
        {tr("Size: small to large"), "size"},
        {tr("Size: large to small"), "size"}})
    Sort->addItem(Text, Key);
  Filters->addWidget(Sort);
  Filters->addStretch();
  Root->addLayout(Filters);
  Search = new QLineEdit(this);
  Search->setObjectName("counterpartSearch");
  Search->setAccessibleName(tr("Search counterpart functions"));
  Search->setPlaceholderText(
      tr("Search by function name or hexadecimal address"));
  Search->setClearButtonEnabled(true);
  Root->addWidget(Search);
  Status = Label("counterpartStatus");
  Candidates =
      new ComparisonModel(Session, ComparisonModel::Kind::Candidates, this);
  Table = new QTableView(this);
  Table->setObjectName("counterpartFunctions");
  Table->setAccessibleName(tr("Counterpart functions"));
  Table->setModel(Candidates);
  Table->setSelectionBehavior(QAbstractItemView::SelectRows);
  Table->setSelectionMode(QAbstractItemView::SingleSelection);
  Table->setEditTriggers(QAbstractItemView::NoEditTriggers);
  Table->setAlternatingRowColors(true);
  Table->setWordWrap(false);
  Table->verticalHeader()->hide();
  auto *Header = Table->horizontalHeader();
  Header->setResizeContentsPrecision(
      ::neverd::limits::gui::kComparisonPageSize);
  Header->setSectionResizeMode(QHeaderView::ResizeToContents);
  Header->setSectionResizeMode(1, QHeaderView::Stretch);
  Header->setSectionResizeMode(4, QHeaderView::Stretch);
  Root->addWidget(Table, 1);
  Selection = Label("counterpartSelection");
  auto *Buttons = new QDialogButtonBox(QDialogButtonBox::Cancel, this);
  Pair = Buttons->addButton(tr("Pair functions"), QDialogButtonBox::ActionRole);
  Pair->setObjectName("counterpartPair");
  // Enter in a search field must not commit a mapping.
  Pair->setAutoDefault(false);
  Buttons->button(QDialogButtonBox::Cancel)->setAutoDefault(false);
  Root->addWidget(Buttons);
  connect(Buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
  Filter.setSingleShot(true);
  Filter.setInterval(::neverd::limits::gui::kChooserViewFilterDebounceMs);
  connect(Search, &QLineEdit::textChanged, this, [this] {
    Candidates->clear();
    Status->setText(tr("Searching…"));
    updateSelection();
    Filter.start();
  });
  connect(&Filter, &QTimer::timeout, this,
          &ComparisonCounterpartDialog::reload);
  connect(Search, &QLineEdit::returnPressed, this,
          &ComparisonCounterpartDialog::reload);
  connect(Scope, &QComboBox::currentIndexChanged, this,
          &ComparisonCounterpartDialog::reload);
  connect(Sort, &QComboBox::currentIndexChanged, this,
          &ComparisonCounterpartDialog::reload);
  connect(Table->selectionModel(), &QItemSelectionModel::currentRowChanged,
          this, &ComparisonCounterpartDialog::updateSelection);
  connect(Candidates, &ComparisonModel::modelReset, this,
          &ComparisonCounterpartDialog::updateSelection);
  connect(
      Candidates, &ComparisonModel::pageReady, this,
      [this](const QJsonObject &Page) {
        const auto Group = Page["ambiguity"].toObject();
        if (!Group.isEmpty()) {
          const auto Function = Group["source"].toObject();
          Source->setText(tr("Choose a %1 counterpart for %2 · %3.")
                              .arg(SourceSide == "primary" ? tr("secondary")
                                                           : tr("primary"),
                                   Function["name"].toString(), SourceAddress));
        }
        const auto Total = Page["total"].toInteger();
        if (!Group.isEmpty() && !Group["active"].toBool())
          Status->setText(tr("This function has no retained ambiguity group. "
                             "Choose All functions to select a counterpart."));
        else if (!Total)
          Status->setText(
              Scope->currentData().toBool()
                  ? tr("No ambiguity candidates match. Clear the search or "
                       "choose All functions.")
                  : tr("No functions match. Try another name or address."));
        else if (Scope->currentData().toBool())
          Status->setText(
              tr("%1 of %2 candidates · These are hypotheses; choose using "
                 "your analysis.\n%3")
                  .arg(QLocale().toString(Total),
                       QLocale().toString(Group["candidate_total"].toInteger()),
                       Group["explanation"].toString()));
        else
          Status->setText(
              tr("%1 functions · Select a row to review its current pairing.")
                  .arg(QLocale().toString(Total)));
        updateSelection();
      });
  connect(Candidates, &ComparisonModel::failed, this,
          [this](const QString &Message) {
            Candidates->clear();
            Status->setText(tr("Could not load candidates. %1").arg(Message));
          });
  connect(&Session, &ComparisonSession::mappingChanged, this, [this] {
    invalidate(tr("The comparison changed. Close this window and choose the "
                  "function again."));
  });
  connect(&Session, &ComparisonSession::changed, this, [this] {
    if (!Session.ready())
      invalidate(tr("The comparison worker is unavailable. Close this window "
                    "and retry the comparison."));
    const bool Available = !Invalid && Session.ready() && !Session.busy();
    Search->setEnabled(Available);
    Scope->setEnabled(Available);
    Sort->setEnabled(Available);
    updateSelection();
  });
  connect(Pair, &QPushButton::clicked, this, [this] {
    updateSelection();
    if (!Pair->isEnabled())
      return;
    const auto Target =
        Candidates->row(Table->currentIndex().row())["address"].toString();
    Session.command(
        "comparison_override",
        {{"kind", "match"},
         {"primary", SourceSide == "primary" ? SourceAddress : Target},
         {"secondary", SourceSide == "primary" ? Target : SourceAddress}});
    accept();
  });
  reload();
  Search->setFocus();
}

void ComparisonCounterpartDialog::reload() {
  Filter.stop();
  if (Invalid)
    return;
  Status->setText(tr("Loading functions…"));
  QJsonObject Query{{"side", SourceSide == "primary" ? "secondary" : "primary"},
                    {"search", Search->text()},
                    {"sort", Sort->currentData().toString()},
                    {"descending", Sort->currentIndex() % 2 != 0}};
  if (Scope->currentData().toBool())
    Query["ambiguous_with"] = SourceAddress;
  Candidates->setQuery(Query);
  updateSelection();
}

void ComparisonCounterpartDialog::updateSelection() {
  const auto Row = Candidates->row(Table->currentIndex().row());
  const auto Address = Row["address"].toString();
  const bool Usable = !Invalid && Session.ready() && !Session.busy() &&
                      Session.revision() == Revision && !Address.isEmpty();
  Pair->setEnabled(Usable && Address != CurrentCounterpart);
  if (Invalid) {
    Selection->clear();
    return;
  }
  if (Address.isEmpty()) {
    Selection->setText(CurrentCounterpart.isEmpty()
                           ? tr("Select a function to pair. Browsing leaves "
                                "your comparison unchanged.")
                           : tr("Current counterpart: %1. Select a "
                                "replacement; Undo restores previous pairs.")
                                 .arg(CurrentCounterpart));
    return;
  }
  if (Address == CurrentCounterpart) {
    Selection->setText(
        tr("%1 is already paired with this function.").arg(Address));
    return;
  }
  QString Text = tr("Pair %1 with %2 · %3.")
                     .arg(SourceAddress, Row["name"].toString(), Address);
  const auto Reason = Row["pair_evidence"].toObject()["explanation"].toString();
  if (!Reason.isEmpty())
    Text += '\n' + Reason;
  if (!CurrentCounterpart.isEmpty())
    Text += tr("\nRemoves the existing pair %1 ↔ %2.")
                .arg(SourceAddress, CurrentCounterpart);
  const auto Other = Row["counterpart"].toObject();
  if (!Other.isEmpty())
    Text += tr("\nRemoves the candidate's existing pair %1 ↔ %2 · %3.")
                .arg(Address, Other["name"].toString(),
                     Other["address"].toString());
  if (Row["incomplete"].toBool())
    Text += tr("\nThis function has incomplete analysis.");
  Text += tr("\nThis records your chosen correspondence. Undo restores "
             "previous pairs.");
  Selection->setText(Text);
}

void ComparisonCounterpartDialog::invalidate(const QString &Message) {
  Invalid = true;
  Filter.stop();
  Candidates->clear();
  Search->setEnabled(false);
  Scope->setEnabled(false);
  Sort->setEnabled(false);
  Status->setText(Message);
  updateSelection();
}
} // namespace neverd::gui
