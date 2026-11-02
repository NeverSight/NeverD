//===- FindInViewDialog.cpp - Search the originating pane -----------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Search loaded text without changing the analysis or following dialog focus.
//===----------------------------------------------------------------------===//

#include "FindInViewDialog.h"

#include "../ResourceLimits.h"
#include "CodeView.h"
#include "ListingView.h"
#include "ShortcutHint.h"

#include <QCheckBox>
#include <QHBoxLayout>
#include <QKeyEvent>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QScopedValueRollback>
#include <QVBoxLayout>
#include <algorithm>

namespace neverd::gui {

FindInViewDialog::FindInViewDialog(QWidget *parent)
    : QDialog(parent), Input(new QLineEdit(this)),
      MatchCase(new QCheckBox(tr("Match case"), this)),
      WholeWords(new QCheckBox(tr("Whole words"), this)),
      Wrap(new QCheckBox(tr("Wrap around"), this)), Scope(new QLabel(this)),
      Status(new QLabel(this)), Previous(new QPushButton(tr("Previous"), this)),
      Next(new QPushButton(tr("Next"), this)) {
  setObjectName(QStringLiteral("findInViewDialog"));
  setWindowTitle(tr("Find in view"));
  setWindowFlag(Qt::Tool);
  setAttribute(Qt::WA_DeleteOnClose);
  Input->setObjectName(QStringLiteral("findInViewText"));
  Input->setPlaceholderText(tr("Find text"));
  Input->setClearButtonEnabled(true);
  MatchCase->setObjectName(QStringLiteral("findInViewCase"));
  WholeWords->setObjectName(QStringLiteral("findInViewWords"));
  Wrap->setObjectName(QStringLiteral("findInViewWrap"));
  Wrap->setChecked(true);
  Status->setObjectName(QStringLiteral("findInViewStatus"));
  Scope->setObjectName(QStringLiteral("findInViewScope"));
  Previous->setObjectName(QStringLiteral("findInViewPrevious"));
  Next->setObjectName(QStringLiteral("findInViewNext"));
  Next->setDefault(true);
  Previous->setToolTip(tr("Previous match (%1 or %2)")
                           .arg(QKeySequence(Qt::SHIFT | Qt::Key_Return)
                                    .toString(QKeySequence::NativeText),
                                QKeySequence(Qt::SHIFT | Qt::Key_F3)
                                    .toString(QKeySequence::NativeText)));
  Next->setToolTip(
      tr("Next match (%1 or %2)")
          .arg(QKeySequence(Qt::Key_Return).toString(QKeySequence::NativeText),
               QKeySequence(Qt::Key_F3).toString(QKeySequence::NativeText)));
  setShortcutHint(*Previous, QKeySequence(Qt::SHIFT | Qt::Key_F3));
  setShortcutHint(*Next, QKeySequence(Qt::Key_F3));
  auto *layout = new QVBoxLayout(this);
  layout->addWidget(Scope);
  auto *row = new QHBoxLayout;
  auto *label = new QLabel(tr("&Text:"), this);
  label->setBuddy(Input);
  row->addWidget(label);
  row->addWidget(Input, 1);
  layout->addLayout(row);
  auto *options = new QHBoxLayout;
  options->addWidget(MatchCase);
  options->addWidget(WholeWords);
  options->addWidget(Wrap);
  layout->addLayout(options);
  auto *buttons = new QHBoxLayout;
  buttons->addWidget(Status, 1);
  buttons->addWidget(Previous);
  buttons->addWidget(Next);
  auto *close = new QPushButton(tr("Close"), this);
  close->setObjectName(QStringLiteral("findInViewClose"));
  buttons->addWidget(close);
  layout->addLayout(buttons);
  Delay.setSingleShot(true);
  Delay.setInterval(::neverd::limits::gui::kFindInViewDebounceMs);
  connect(&Delay, &QTimer::timeout, this, [this] { rebuildMatches(true); });
  connect(Input, &QLineEdit::textChanged, this, [this] {
    Pending = true;
    Delay.start();
  });
  for (auto *option : {MatchCase, WholeWords})
    connect(option, &QCheckBox::toggled, this,
            [this] { rebuildMatches(true); });
  connect(Next, &QPushButton::clicked, this, [this] { step(true); });
  connect(Previous, &QPushButton::clicked, this, [this] { step(false); });
  connect(close, &QPushButton::clicked, this, &QDialog::reject);
  for (auto *widget : findChildren<QWidget *>())
    widget->installEventFilter(this);
}

FindInViewDialog::~FindInViewDialog() { releaseTarget(); }

QWidget *FindInViewDialog::target() const {
  return Code ? static_cast<QWidget *>(Code.data()) : Listing.data();
}

void FindInViewDialog::releaseTarget() {
  Delay.stop();
  for (const auto &connection : std::as_const(Connections))
    disconnect(connection);
  Connections.clear();
  if (auto *view = target())
    view->removeEventFilter(this);
  if (Code)
    Code->setSearchHighlight({});
  if (Listing)
    Listing->setSearchHighlight({});
  Code.clear();
  Listing.clear();
  Document.clear();
  Matches.clear();
  Ready = Pending = false;
}

void FindInViewDialog::search(CodeText *code, ListingView *listing) {
  releaseTarget();
  Code = code;
  Listing = code ? nullptr : listing;
  if (!target())
    return;
  target()->installEventFilter(this);
  Connections.append(
      connect(target(), &QObject::destroyed, this, [this] { reject(); }));
  if (Code) {
    Scope->setText(tr("%1: current function, including folded code")
                       .arg(CodeView::titleOf(Code->representation())));
    Connections.append(connect(Code, &CodeText::statusChanged, this,
                               &FindInViewDialog::refreshDocument));
    Connections.append(connect(Code, &CodeText::cursorChanged, this,
                               &FindInViewDialog::updateStatus));
  } else {
    Scope->setText(
        tr("Disassembly: loaded lines, including visible addresses"));
    Connections.append(connect(Listing, &ListingView::searchDocumentChanged,
                               this, &FindInViewDialog::refreshDocument));
    Connections.append(connect(Listing, &ListingView::cursorChanged, this,
                               &FindInViewDialog::updateStatus));
  }
  const QString seed = Code ? Code->searchSeed() : Listing->searchSeed();
  if (!seed.isEmpty())
    Input->setText(seed);
  Pending = true;
  refreshDocument();
  show();
  raise();
  activateWindow();
  Input->setFocus();
  Input->selectAll();
}

void FindInViewDialog::refreshDocument() {
  if (Code)
    Scope->setText(tr("%1: current function, including folded code")
                       .arg(CodeView::titleOf(Code->representation())));
  Ready = Code ? Code->complete() : Listing && Listing->searchReady();
  Document =
      Ready ? (Code ? Code->allText() : Listing->searchText()) : QString();
  rebuildMatches(Pending);
}

void FindInViewDialog::rebuildMatches(bool select) {
  Delay.stop();
  Query = {Input->text(),
           MatchCase->isChecked() ? Qt::CaseSensitive : Qt::CaseInsensitive,
           WholeWords->isChecked()};
  Matches = Ready ? Query.matches(Document) : QVector<int>();
  if (Code)
    Code->setSearchHighlight(Query);
  if (Listing)
    Listing->setSearchHighlight(Query);
  Pending = select && !Ready;
  if (select && Ready)
    step(true, false);
  updateStatus();
}

std::pair<int, int> FindInViewDialog::selection() const {
  return Code      ? Code->searchSelection()
         : Listing ? Listing->searchSelection()
                   : std::pair{0, 0};
}

void FindInViewDialog::step(bool forward, bool skipSelected) {
  if (Selecting)
    return;
  const QScopedValueRollback<bool> selecting(Selecting, true);
  if (Delay.isActive()) {
    rebuildMatches(false);
    skipSelected = false;
  }
  if (!Ready || Matches.isEmpty())
    return;
  const auto [begin, end] = selection();
  auto next = std::lower_bound(Matches.cbegin(), Matches.cend(), begin);
  const bool selected = next != Matches.cend() && *next == begin &&
                        end - begin == Query.Text.size();
  int index = int(next - Matches.cbegin());
  if (forward) {
    if (skipSelected && selected)
      ++index;
  } else if (skipSelected || !selected) {
    --index;
  }
  if (index < 0 || index >= Matches.size()) {
    if (!Wrap->isChecked()) {
      Status->setText(forward ? tr("End of search range")
                              : tr("Start of search range"));
      return;
    }
    index = forward ? 0 : int(Matches.size()) - 1;
  }
  if (Code)
    Code->selectSearchRange(Matches[index], Query.Text.size());
  else if (Listing)
    Listing->selectSearchRange(Matches[index], Query.Text.size());
  updateStatus();
}

void FindInViewDialog::updateStatus() {
  Previous->setEnabled(Ready && !Matches.isEmpty());
  Next->setEnabled(Ready && !Matches.isEmpty());
  if (!Ready) {
    Status->setText(Code && Code->loading()
                        ? tr("Waiting for source…")
                        : tr("No searchable text available"));
    return;
  }
  if (Query.Text.isEmpty()) {
    Status->setText(tr("Enter text to find"));
    return;
  }
  if (Matches.isEmpty()) {
    Status->setText(tr("No matches"));
    return;
  }
  const auto [begin, end] = selection();
  const auto at = std::lower_bound(Matches.cbegin(), Matches.cend(), begin);
  const int current =
      at != Matches.cend() && *at == begin && end - begin == Query.Text.size()
          ? int(at - Matches.cbegin()) + 1
          : 0;
  Status->setText(current ? tr("%1 of %2").arg(current).arg(Matches.size())
                          : tr("%n matches", nullptr, Matches.size()));
}

bool FindInViewDialog::eventFilter(QObject *object, QEvent *event) {
  if (isVisible() && (event->type() == QEvent::ShortcutOverride ||
                      event->type() == QEvent::KeyPress)) {
    auto *key = static_cast<QKeyEvent *>(event);
    const bool dialog = object != target();
    const bool find = dialog && key->matches(QKeySequence::Find);
    const bool next =
        key->key() == Qt::Key_F3 && !(key->modifiers() & ~Qt::ShiftModifier);
    const bool enter =
        dialog &&
        (key->key() == Qt::Key_Return || key->key() == Qt::Key_Enter) &&
        !(key->modifiers() & ~Qt::ShiftModifier);
    const bool escape =
        key->key() == Qt::Key_Escape && key->modifiers() == Qt::NoModifier;
    if (find || next || enter || escape) {
      event->accept();
      if (event->type() == QEvent::KeyPress) {
        if (escape)
          reject();
        else if (find) {
          Input->setFocus();
          Input->selectAll();
        } else
          step(!(key->modifiers() & Qt::ShiftModifier));
      }
      return true;
    }
  }
  return QDialog::eventFilter(object, event);
}

void FindInViewDialog::done(int result) {
  const QPointer<QWidget> view = target();
  releaseTarget();
  QDialog::done(result);
  if (view && view->isVisible())
    view->setFocus();
}

} // namespace neverd::gui
