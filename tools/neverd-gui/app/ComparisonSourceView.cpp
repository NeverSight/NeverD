//===- ComparisonSourceView.cpp - Paired source inspection ----------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Exact textual source differences and independent source inspection.
//===----------------------------------------------------------------------===//

#include "ComparisonSourceView.h"

#include "../ResourceLimits.h"
#include "ComparisonSession.h"
#include "ElidedLabel.h"
#include "Theme.h"
#include "TypeSourceHighlighter.h"

#include "neverd/diff/Limits.h"

#include <QAction>
#include <QCheckBox>
#include <QComboBox>
#include <QHBoxLayout>
#include <QJsonArray>
#include <QKeyEvent>
#include <QLabel>
#include <QMenu>
#include <QPainter>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QResizeEvent>
#include <QScrollBar>
#include <QShowEvent>
#include <QSignalBlocker>
#include <QSpinBox>
#include <QSplitter>
#include <QStringList>
#include <QTextBlock>
#include <QTextEdit>
#include <QTimer>
#include <QToolButton>
#include <QVBoxLayout>
#include <algorithm>
#include <climits>
#include <utility>

namespace neverd::gui {
namespace {
constexpr int PageLines = limits::gui::kCodeViewPageLines;
// A committed source line must not activate the containing dialog's default
// button (which can open an input file chooser).
class SourceLineSelector final : public QSpinBox {
public:
  using QSpinBox::QSpinBox;

protected:
  void keyPressEvent(QKeyEvent *Event) override {
    QSpinBox::keyPressEvent(Event);
    if (Event->key() == Qt::Key_Return || Event->key() == Qt::Key_Enter)
      Event->accept();
  }
};

class SourceCode final : public QPlainTextEdit {
public:
  explicit SourceCode(QWidget *Parent) : QPlainTextEdit(Parent) {
    Gutter = new QWidget(this);
    Gutter->installEventFilter(this);
    connect(this, &QPlainTextEdit::updateRequest, this,
            [this](const QRect &, int) { Gutter->update(); });
    updateMinimumHeight();
  }
  void setRows(const QJsonArray &NewRows) {
    Rows = NewRows;
    updateGutter();
  }

protected:
  void resizeEvent(QResizeEvent *Event) override {
    QPlainTextEdit::resizeEvent(Event);
    updateGutter();
  }
  void changeEvent(QEvent *Event) override {
    QPlainTextEdit::changeEvent(Event);
    if (Event->type() == QEvent::FontChange) {
      updateMinimumHeight();
      updateGutter();
    }
  }
  bool eventFilter(QObject *Object, QEvent *Event) override {
    if (Object != Gutter || Event->type() != QEvent::Paint)
      return QPlainTextEdit::eventFilter(Object, Event);
    QPainter Painter(Gutter);
    Painter.fillRect(Gutter->rect(), palette().base());
    Painter.setFont(font());
    Painter.setPen(Theme::instance().color(ColorRole::CodeLineNumber));
    auto Block = firstVisibleBlock();
    while (Block.isValid()) {
      const int Top = qRound(
          blockBoundingGeometry(Block).translated(contentOffset()).top());
      if (Top >= viewport()->height())
        break;
      const int Index = Block.blockNumber();
      if (Index < Rows.size()) {
        const auto Row = Rows[Index].toObject();
        QString Text;
        if (Row.value("line").isDouble())
          Text = QString::number(Row.value("line").toInteger() + 1);
        const auto Kind = Row.value("difference_kind").toString();
        const QString Mark = Kind == "changed"    ? "~"
                             : Kind == "inserted" ? "+"
                             : Kind == "deleted"  ? "−"
                                                  : " ";
        if (!Row.value("gap").toBool())
          Text += " " + Mark;
        Painter.drawText(0, Top,
                         Gutter->width() - fontMetrics().horizontalAdvance(' '),
                         fontMetrics().height(), Qt::AlignRight, Text);
      }
      Block = Block.next();
    }
    return true;
  }

private:
  QWidget *Gutter = nullptr;
  QJsonArray Rows;
  void updateMinimumHeight() {
    setMinimumHeight(fontMetrics().height() *
                         limits::gui::kComparisonSourceMinimumVisibleLines +
                     2 * frameWidth() +
                     horizontalScrollBar()->sizeHint().height());
  }
  void updateGutter() {
    if (!Gutter)
      return;
    int Digits = 1;
    for (const auto &Value : Rows)
      Digits = std::max(
          Digits,
          int(QString::number(Value.toObject().value("line").toInteger() + 1)
                  .size()));
    const int Width = (Digits + 4) * fontMetrics().horizontalAdvance('9');
    setViewportMargins(Width, 0, 0, 0);
    Gutter->setGeometry(contentsRect().left(), viewport()->geometry().top(),
                        Width, viewport()->height());
    Gutter->update();
  }
};

QLabel *label(QWidget *Parent, const QString &Text = {}) {
  auto *L = new QLabel(Text, Parent);
  L->setTextFormat(Qt::PlainText);
  L->setWordWrap(true);
  L->setTextInteractionFlags(Qt::TextSelectableByMouse);
  return L;
}
} // namespace

ComparisonSourceView::ComparisonSourceView(ComparisonSession &S,
                                           QWidget *Parent)
    : QWidget(Parent), Session(S) {
  setObjectName("comparisonSource");
  auto *Root = new QVBoxLayout(this);
  Root->setContentsMargins(0, 0, 0, 0);
  auto *Toolbar = new QHBoxLayout;
  auto *ViewLabel = new QLabel(tr("View:"), this);
  Representation = new QComboBox(this);
  Representation->setObjectName("comparisonSourceRepresentation");
  Representation->addItem("C", "c");
  Representation->addItem("LLVM C", "llvmc");
  Representation->addItem(tr("Input language"), "source");
  for (const auto &[Name, Key] :
       {std::pair{"C++", "cpp"}, {"Rust", "rust"}, {"Go", "go"}})
    Representation->addItem(Name, Key);
  ViewLabel->setBuddy(Representation);
  AlignmentMode = new QCheckBox(tr("Align source lines"), this);
  AlignmentMode->setObjectName("comparisonSourceAlignment");
  AlignmentMode->setChecked(true);
  auto *Reload = new QPushButton(tr("Reload source"), this);
  Reload->setObjectName("comparisonSourceReload");
  auto *Options = new QToolButton(this);
  Options->setText(tr("Options"));
  Options->setPopupMode(QToolButton::InstantPopup);
  auto *Menu = new QMenu(Options);
  Unlimited = Menu->addAction(tr("Remove instruction allowance"));
  Unlimited->setCheckable(true);
  UnlimitedWork = Menu->addAction(tr("Remove alignment work allowance"));
  UnlimitedWork->setCheckable(true);
  UnlimitedBytes = Menu->addAction(tr("Remove source evidence byte allowance"));
  UnlimitedBytes->setCheckable(true);
  IncludePrelude = Menu->addAction(tr("Include declarations in alignment"));
  IncludePrelude->setCheckable(true);
  Options->setMenu(Menu);
  Toolbar->addWidget(ViewLabel);
  Toolbar->addWidget(Representation);
  Toolbar->addWidget(AlignmentMode);
  Toolbar->addStretch();
  Toolbar->addWidget(Reload);
  Toolbar->addWidget(Options);
  Root->addLayout(Toolbar);
  Hint = label(this);
  Root->addWidget(Hint);
  auto *Split = new QSplitter(this);
  Split->setChildrenCollapsible(false);
  Root->addWidget(Split, 1);
  for (int Side = 0; Side < 2; ++Side) {
    auto &P = Panes[Side];
    auto *Container = new QWidget(Split);
    auto *Layout = new QVBoxLayout(Container);
    Layout->setContentsMargins(0, 0, 0, 0);
    P.Title = new ElidedLabel(Container);
    P.Title->setObjectName(Side ? "comparisonSecondarySourceTitle"
                                : "comparisonPrimarySourceTitle");
    Layout->addWidget(P.Title);
    P.Text = new SourceCode(Container);
    P.Text->setObjectName(Side ? "comparisonSecondarySource"
                               : "comparisonPrimarySource");
    P.Text->setReadOnly(true);
    P.Text->setLayoutDirection(Qt::LeftToRight);
    P.Text->setLineWrapMode(QPlainTextEdit::NoWrap);
    P.Text->setFont(Theme::instance().codeFont());
    P.Text->setAccessibleName(Side ? tr("Secondary source")
                                   : tr("Primary source"));
    P.Highlighter = new TypeSourceHighlighter(P.Text->document());
    P.Highlighter->setLanguage("c");
    Layout->addWidget(P.Text, 1);
    P.Status = label(Container);
    P.Status->setObjectName(Side ? "comparisonSecondarySourceStatus"
                                 : "comparisonPrimarySourceStatus");
    Layout->addWidget(P.Status);
    auto *Paging = new QHBoxLayout;
    P.Previous = new QToolButton(Container);
    P.Previous->setArrowType(isRightToLeft() ? Qt::RightArrow : Qt::LeftArrow);
    P.Previous->setToolTip(tr("Previous source page"));
    P.Previous->setAccessibleName(P.Previous->toolTip());
    P.Next = new QToolButton(Container);
    P.Next->setArrowType(isRightToLeft() ? Qt::LeftArrow : Qt::RightArrow);
    P.Next->setToolTip(tr("Next source page"));
    P.Next->setAccessibleName(P.Next->toolTip());
    P.Line = new SourceLineSelector(Container);
    P.Line->setObjectName(Side ? "comparisonSecondarySourceLine"
                               : "comparisonPrimarySourceLine");
    P.Line->setMinimum(1);
    P.Line->setKeyboardTracking(false);
    P.Line->setToolTip(tr("Go to source line"));
    P.Line->setAccessibleName(P.Line->toolTip());
    Paging->addWidget(P.Previous);
    Paging->addWidget(P.Line, 1);
    Paging->addWidget(P.Next);
    Layout->addLayout(Paging);
    auto *Navigation = new QHBoxLayout;
    P.Anchors = new QComboBox(Container);
    P.Anchors->setSizeAdjustPolicy(
        QComboBox::AdjustToMinimumContentsLengthWithIcon);
    P.Anchors->setMinimumContentsLength(8);
    P.Anchors->setAccessibleName(tr("Original instruction anchors"));
    P.Instruction = new QPushButton(tr("Show instruction"), Container);
    P.Instruction->setObjectName(Side ? "comparisonSecondarySourceInstruction"
                                      : "comparisonPrimarySourceInstruction");
    Navigation->addWidget(P.Anchors, 1);
    Navigation->addWidget(P.Instruction);
    Layout->addLayout(Navigation);
    connect(P.Text, &QPlainTextEdit::cursorPositionChanged, this,
            [this, Side] { cursor(Side); });
    connect(P.Previous, &QToolButton::clicked, this, [this, Side] {
      load(Side, std::max(0, Panes[Side].Offset - PageLines));
    });
    connect(P.Next, &QToolButton::clicked, this, [this, Side] {
      load(Side, Panes[Side].Document.value("next_offset").toInt());
    });
    connect(P.Line, &QSpinBox::editingFinished, this, [this, Side] {
      const int Offset = Panes[Side].Line->value() - 1;
      if (Offset != Panes[Side].Offset)
        load(Side, Offset);
    });
    connect(P.Anchors, &QComboBox::currentIndexChanged, this, [this, Side] {
      auto &P = Panes[Side];
      P.Instruction->setEnabled(!P.Pending &&
                                P.Anchors->currentData().isValid());
    });
    connect(P.Instruction, &QPushButton::clicked, this, [this, Side] {
      emit instructionSelected(Side ? "secondary" : "primary",
                               Panes[Side].Anchors->currentData().toInt());
    });
    connect(&Theme::instance(), &Theme::changed, this, [this, Side] {
      Panes[Side].Text->setFont(Theme::instance().codeFont());
      Panes[Side].Highlighter->rehighlight();
      highlight(Side);
    });
    connect(P.Text->verticalScrollBar(), &QScrollBar::valueChanged, this,
            [this, Side](int Value) {
              if (aligned()) {
                auto *Other = Panes[1 - Side].Text->verticalScrollBar();
                const QSignalBlocker Block(Other);
                Other->setValue(Value);
              }
            });
  }
  connect(Representation, &QComboBox::currentIndexChanged, this,
          [this] { reset(); });
  connect(Unlimited, &QAction::toggled, this, [this] { reset(); });
  for (auto *Option : {UnlimitedWork, UnlimitedBytes, IncludePrelude})
    connect(Option, &QAction::toggled, this, [this] { reset(); });
  connect(AlignmentMode, &QCheckBox::toggled, this, [this] { reset(); });
  connect(Reload, &QPushButton::clicked, this, [this] { reset(true); });
  connect(&Session, &ComparisonSession::mappingChanged, this,
          [this] { setFunctions({}, {}); });
  connect(&Session, &ComparisonSession::changed, this, [this] {
    for (int Side = 0; Side < 2; ++Side) {
      updateControls(Side);
      if (Panes[Side].NeedsLoad)
        QTimer::singleShot(0, this, [this, Side] {
          if (Panes[Side].NeedsLoad)
            load(Side);
        });
    }
  });
  reset();
}
void ComparisonSourceView::setFunctions(const QJsonObject &A,
                                        const QJsonObject &B) {
  if (Panes[0].Function == A && Panes[1].Function == B &&
      Revision == Session.revision())
    return;
  Panes[0].Function = A;
  Panes[1].Function = B;
  Revision = Session.revision();
  reset();
}
void ComparisonSourceView::reset(bool Refresh) {
  ++Generation;
  AlignmentMode->setEnabled(Session.supports("comparison_source_difference") &&
                            !Panes[0].Function.isEmpty() &&
                            !Panes[1].Function.isEmpty());
  Hint->setText(aligned()
                    ? tr("Text differences: + inserted, − deleted, ~ changed. "
                         "Select a line to inspect its original instructions.")
                    : tr("Each pane shows its own source lines. Select a "
                         "line to inspect its original instructions."));
  for (auto *Option : {UnlimitedWork, UnlimitedBytes, IncludePrelude})
    Option->setEnabled(aligned());
  for (int Side = 0; Side < 2; ++Side) {
    auto &P = Panes[Side];
    P.Document = {};
    P.Offset = 0;
    P.Initial = true;
    P.NeedsLoad = true;
    P.Refresh = Refresh;
    P.RangeText.clear();
    P.Text->clear();
    static_cast<SourceCode *>(P.Text)->setRows({});
    P.Text->setExtraSelections({});
    const auto Navigation =
        aligned() ? tr("Go to aligned row") : tr("Go to source line");
    P.Line->setToolTip(Navigation);
    P.Line->setAccessibleName(Navigation);
    static_cast<ElidedLabel *>(P.Title)->setFullText(
        (Side ? tr("Secondary: %1 %2") : tr("Primary: %1 %2"))
            .arg(P.Function.value("name").toString(),
                 P.Function.value("address").toString()));
    P.Status->setText(P.Function.isEmpty()
                          ? tr("No function selected on this side.")
                          : tr("Source is ready to load."));
    cursor(Side);
    updateControls(Side);
  }
  if (isVisible())
    for (int Side = 0; Side < 2; ++Side)
      load(Side);
}
void ComparisonSourceView::showEvent(QShowEvent *Event) {
  QWidget::showEvent(Event);
  for (int Side = 0; Side < 2; ++Side)
    if (Panes[Side].NeedsLoad)
      load(Side);
}
void ComparisonSourceView::load(int Side, int Offset) {
  if (aligned()) {
    loadDifference(Offset);
    return;
  }
  auto &P = Panes[Side];
  if (!isVisible() || P.Pending || P.Function.isEmpty() || !Session.ready() ||
      Session.busy())
    return;
  P.NeedsLoad = false;
  if (!Session.supports("comparison_source_page")) {
    P.Status->setText(tr("This worker does not support source inspection."));
    return;
  }
  P.Pending = true;
  const bool Refresh = std::exchange(P.Refresh, false);
  const auto Serial = Generation;
  const auto ExpectedRevision = Revision;
  P.Status->setText(tr(
      "Preparing source… Cancellation waits for the current analysis step."));
  updateControls(Side);
  Session.request(
      "comparison_source_page",
      {{"side", Side ? "secondary" : "primary"},
       {"address", P.Function.value("address")},
       {"representation", Representation->currentData().toString()},
       {"offset", Offset},
       {"limit", PageLines},
       {"refresh", Refresh},
       {"max_native_instructions",
        Unlimited->isChecked()
            ? 0
            : qint64(limits::diff::kDefaultSourceNativeInstructions)}},
      this, [this, Side, Offset, Serial, ExpectedRevision](const auto &Reply) {
        auto &P = Panes[Side];
        P.Pending = false;
        if (Serial != Generation || ExpectedRevision != Session.revision()) {
          updateControls(Side);
          QTimer::singleShot(0, this, [this, Side] { load(Side); });
          return;
        }
        if (Reply["status"] != QJsonValue("ok")) {
          P.Status->setText(Reply["error"].toObject()["message"].toString());
          updateControls(Side);
          return;
        }
        const auto Result = Reply["payload"].toObject();
        if (Result["availability"] != QJsonValue("available")) {
          const auto State = Result["state"].toString();
          P.Status->setText(
              State == "resource_limited"
                  ? tr("Source exceeds the instruction allowance. Adjust "
                       "Options to inspect this function.")
              : State == "cancelled" || State == "timed_out"
                  ? tr("Source inspection stopped. Reload source to try again.")
                  : tr("Source unavailable: %1")
                        .arg(Result["reason"].toString()));
          P.Document = {};
          P.RangeText.clear();
          P.Text->clear();
          static_cast<SourceCode *>(P.Text)->setRows({});
          updateControls(Side);
          return;
        }
        P.Document = Result["document"].toObject();
        const int Prelude =
            P.Document.value("prelude").toObject()["lines"].toInt();
        if (P.Initial && Offset == 0 && Prelude > 0 &&
            Prelude < P.Document.value("total_lines").toInt()) {
          P.Initial = false;
          load(Side, Prelude);
          return;
        }
        P.Initial = false;
        P.Offset = Offset;
        P.Highlighter->setLanguage(P.Document.value("dialect").toString("c"));
        P.Text->setPlainText(P.Document.value("text").toString());
        const auto Rows = P.Document.value("rows").toArray();
        static_cast<SourceCode *>(P.Text)->setRows(Rows);
        P.RangeText = tr("Lines %1–%2 of %3")
                          .arg(Rows.isEmpty() ? Offset : Offset + 1)
                          .arg(Offset + Rows.size())
                          .arg(P.Document.value("total_lines").toInt());
        if (!Result["native_analysis_complete"].toBool())
          P.RangeText += tr(" · Native coverage is incomplete");
        {
          const QSignalBlocker Block(P.Line);
          P.Line->setMaximum(
              std::max(1, P.Document.value("total_lines").toInt()));
          P.Line->setValue(Offset + 1);
        }
        cursor(Side);
        updateControls(Side);
      });
}
bool ComparisonSourceView::aligned() const {
  return AlignmentMode->isChecked() &&
         Session.supports("comparison_source_difference") &&
         !Panes[0].Function.isEmpty() && !Panes[1].Function.isEmpty();
}
void ComparisonSourceView::loadDifference(int Offset) {
  if (!isVisible() || Panes[0].Pending || Panes[1].Pending ||
      !Session.ready() || Session.busy())
    return;
  const bool Refresh = Panes[0].Refresh || Panes[1].Refresh;
  for (int Side = 0; Side < 2; ++Side) {
    auto &P = Panes[Side];
    P.Pending = true;
    P.NeedsLoad = P.Refresh = false;
    P.Status->setText(tr(
        "Aligning source… Cancellation waits for the current analysis step."));
    updateControls(Side);
  }
  const auto Serial = Generation;
  const auto ExpectedRevision = Revision;
  Session.request(
      "comparison_source_difference",
      {{"side", "primary"},
       {"address", Panes[0].Function.value("address")},
       {"representation", Representation->currentData().toString()},
       {"offset", Offset},
       {"limit", PageLines},
       {"refresh", Refresh},
       {"include_prelude", IncludePrelude->isChecked()},
       {"max_native_instructions",
        Unlimited->isChecked()
            ? 0
            : qint64(limits::diff::kDefaultSourceNativeInstructions)},
       {"max_work", UnlimitedWork->isChecked()
                        ? 0
                        : qint64(limits::diff::kDefaultSourceAlignmentWork)},
       {"max_source_bytes",
        UnlimitedBytes->isChecked()
            ? 0
            : qint64(limits::diff::kDefaultSourceAlignmentBytes)}},
      this, [this, Offset, Serial, ExpectedRevision](const auto &Reply) {
        Panes[0].Pending = Panes[1].Pending = false;
        if (Serial != Generation || ExpectedRevision != Session.revision()) {
          for (int Side = 0; Side < 2; ++Side) {
            updateControls(Side);
            QTimer::singleShot(0, this, [this, Side] { load(Side); });
          }
          return;
        }
        const auto Result = Reply.value("payload").toObject();
        if (Reply.value("status") != QJsonValue("ok") ||
            Result.value("availability") != QJsonValue("available")) {
          const auto State = Result.value("state").toString();
          const auto Reason =
              Reply.value("status") != QJsonValue("ok")
                  ? Reply.value("error").toObject().value("message").toString()
                  : Result.value("reason").toString();
          const auto Message =
              State == "resource_limited"
                  ? tr("Source alignment exceeds its allowance. Adjust Options "
                       "or view independent source.")
              : State == "cancelled" || State == "timed_out"
                  ? tr("Source alignment stopped. Reload source to try again.")
                  : tr("Source unavailable: %1").arg(Reason);
          for (int Side = 0; Side < 2; ++Side) {
            auto &P = Panes[Side];
            P.Document = {};
            P.RangeText.clear();
            P.Text->clear();
            static_cast<SourceCode *>(P.Text)->setRows({});
            P.Text->setExtraSelections({});
            P.Status->setText(Message);
            cursor(Side);
            updateControls(Side);
          }
          return;
        }
        const auto Rows = Result.value("rows").toArray();
        for (int Side = 0; Side < 2; ++Side) {
          auto &P = Panes[Side];
          const auto Key = Side ? "secondary" : "primary";
          const auto Metadata = Result.value(Key).toObject();
          QJsonArray SourceRows;
          QStringList Lines;
          for (const auto &Value : Rows) {
            const auto Pair = Value.toObject();
            auto Line = Pair.value(Key).toObject();
            const bool Gap = Pair.value(Key).isNull();
            Line["gap"] = Gap;
            Line["difference_kind"] = Pair.value("kind");
            auto Text = Line.value("text").toString();
            if (Text.endsWith('\n'))
              Text.chop(1);
            Lines.push_back(Text);
            SourceRows.append(Line);
          }
          P.Offset = Offset;
          P.Document = {{"rows", SourceRows},
                        {"next_offset", Result.value("next_offset")}};
          P.RangeText = tr("Aligned rows %1–%2 of %3")
                            .arg(Rows.isEmpty() ? Offset : Offset + 1)
                            .arg(Offset + Rows.size())
                            .arg(Result.value("total_rows").toInteger());
          if (!Metadata.value("native_analysis_complete").toBool())
            P.RangeText += tr(" · Native coverage is incomplete");
          P.Highlighter->setLanguage(Metadata.value("dialect").toString("c"));
          P.Text->setPlainText(Lines.join('\n'));
          static_cast<SourceCode *>(P.Text)->setRows(SourceRows);
          {
            const QSignalBlocker Block(P.Line);
            P.Line->setMaximum(int(std::clamp<qint64>(
                Result.value("total_rows").toInteger(), 1, INT_MAX)));
            P.Line->setValue(Offset + 1);
          }
          highlight(Side);
          cursor(Side);
          updateControls(Side);
        }
      });
}
void ComparisonSourceView::highlight(int Side) {
  auto &P = Panes[Side];
  QList<QTextEdit::ExtraSelection> Selections;
  const auto Rows = P.Document.value("rows").toArray();
  for (int I = 0; I < Rows.size(); ++I) {
    const auto Kind = Rows[I].toObject().value("difference_kind").toString();
    if (Kind.isEmpty() || Kind == "unchanged")
      continue;
    const auto Role = Kind == "inserted"  ? ColorRole::DiffAdded
                      : Kind == "deleted" ? ColorRole::DiffRemoved
                                          : ColorRole::DiffChanged;
    auto Color = Theme::instance().color(Role);
    Color.setAlpha(Theme::instance().dark() ? 40 : 25);
    QTextEdit::ExtraSelection Selection;
    Selection.cursor = QTextCursor(P.Text->document()->findBlockByNumber(I));
    Selection.format.setBackground(Color);
    Selection.format.setProperty(QTextFormat::FullWidthSelection, true);
    Selections.push_back(Selection);
  }
  P.Text->setExtraSelections(Selections);
}
void ComparisonSourceView::cursor(int Side) {
  auto &P = Panes[Side];
  P.Anchors->clear();
  const auto Rows = P.Document.value("rows").toArray();
  const int Row = P.Text->textCursor().blockNumber();
  if (!P.RangeText.isEmpty()) {
    auto Text = P.RangeText;
    if (Row >= 0 && Row < Rows.size()) {
      const auto Line = Rows[Row].toObject();
      if (Line.value("line").isDouble())
        Text += tr(" · Source line %1").arg(Line.value("line").toInteger() + 1);
      const auto Kind = Line.value("difference_kind").toString();
      Text += Line.value("gap").toBool() ? tr(" · No line on this side")
              : Kind == "changed"        ? tr(" · Changed")
              : Kind == "inserted"       ? tr(" · Inserted")
              : Kind == "deleted"        ? tr(" · Deleted")
                                         : QString();
    }
    P.Status->setText(Text);
  }
  if (Row >= 0 && Row < Rows.size())
    for (const auto &Value :
         Rows[Row].toObject()["comparison_anchors"].toArray()) {
      const auto A = Value.toObject();
      const auto Change = A["native_change"].toString();
      P.Anchors->addItem(A["address"].toString() +
                             (Change.isEmpty() ? QString() : " · " + Change),
                         A["listing_row"].isDouble()
                             ? QVariant(A["listing_row"].toInt())
                             : QVariant());
    }
  if (!P.Anchors->count())
    P.Anchors->addItem(tr("Unmapped line"));
  P.Instruction->setEnabled(!P.Pending && P.Anchors->currentData().isValid());
}
void ComparisonSourceView::updateControls(int Side) {
  auto &P = Panes[Side];
  const bool Ready = !P.Pending && Session.ready() && !Session.busy();
  P.Previous->setEnabled(Ready && P.Offset > 0 && !P.Document.isEmpty());
  P.Next->setEnabled(Ready && P.Document.value("next_offset").isDouble());
  P.Line->setEnabled(Ready && !P.Document.isEmpty());
  P.Anchors->setEnabled(Ready);
  P.Instruction->setEnabled(Ready && P.Anchors->currentData().isValid());
}
} // namespace neverd::gui
