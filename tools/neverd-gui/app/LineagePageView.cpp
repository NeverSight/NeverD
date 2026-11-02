//===- LineagePageView.cpp - Revision-bound table navigation --------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// One retained page with full-value copy, keyboard row activation and explicit
/// retry. Labels are presentation only; engine coordinates remain unchanged.
//===----------------------------------------------------------------------===//

#include "LineagePageView.h"

#include "../ResourceLimits.h"
#include "ComparisonModel.h"
#include "Icons.h"
#include "LineageSession.h"
#include "ShrinkableRow.h"
#include "SingleLineText.h"

#include <QAction>
#include <QApplication>
#include <QClipboard>
#include <QEvent>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QJsonDocument>
#include <QLabel>
#include <QLocale>
#include <QMenu>
#include <QResizeEvent>
#include <QScrollBar>
#include <QSpinBox>
#include <QStandardItemModel>
#include <QTableView>
#include <QToolButton>
#include <QVBoxLayout>
#include <algorithm>

namespace neverd::gui {
namespace {
class EvidenceTable final : public QTableView {
public:
  using QTableView::QTableView;

protected:
  void resizeEvent(QResizeEvent *Event) override {
    const auto Before = visualRect(currentIndex());
    const bool Visible = Before.isValid() && Before.top() >= 0 &&
                         Before.bottom() < Event->oldSize().height();
    QTableView::resizeEvent(Event);
    const auto After = visualRect(currentIndex());
    if (Visible &&
        (After.top() < 0 || After.bottom() >= viewport()->height())) {
      // A growing status message or a smaller window must not clip a row the
      // user was inspecting. Keep an intentional horizontal scroll unchanged.
      const auto Horizontal = horizontalScrollBar()->value();
      scrollTo(currentIndex());
      horizontalScrollBar()->setValue(Horizontal);
    }
  }
};

QString cell(const QJsonValue &Value) {
  if (Value.isString())
    return Value.toString();
  if (Value.isBool())
    return Value.toBool() ? LineagePageView::tr("Yes")
                          : LineagePageView::tr("No");
  if (Value.isDouble())
    return Value.toDouble() == double(Value.toInteger())
               ? QLocale().toString(Value.toInteger())
               : QLocale().toString(Value.toDouble());
  if (Value.isArray()) {
    QStringList Items;
    for (const auto &V : Value.toArray())
      Items.append(cell(V));
    return Items.join(", ");
  }
  return QString::fromUtf8("—");
}
QString display(const QJsonObject &Row, const QString &Key) {
  if (Key == "change_state" && Row[Key].isString())
    return ComparisonModel::changeStateText(Row[Key].toString());
  const auto Value = singleLineDisplayText(cell(Row[Key]));
  if (Key == "name" && Value.isEmpty())
    return Row["address"].toString();
  if ((Key == "id" || Key == "report") && Value.size() == 64)
    return Value.left(::neverd::limits::gui::kLineageDigestDisplayCharacters);
  if (Key == "state" || Key == "kind" || Key == "assessment") {
    if (Value == "multiple_functions")
      return LineagePageView::tr("Conflicting counterparts");
    if (Value == "rejected_pair")
      return LineagePageView::tr("Rejected pair connected");
    if (Value == "complete")
      return LineagePageView::tr("Complete");
    if (Value == "cancelled")
      return LineagePageView::tr("Cancelled");
    if (Value == "timed_out")
      return LineagePageView::tr("Deadline reached");
    if (Value == "resource_limited")
      return LineagePageView::tr("Resource limit reached");
    if (Value == "match")
      return LineagePageView::tr("Match");
    if (Value == "unmatch")
      return LineagePageView::tr("Unmatch");
    if (Value == "confirm")
      return LineagePageView::tr("Confirm");
    if (Value == "investigating")
      return LineagePageView::tr("Investigating");
    if (Value == "relevant_change")
      return LineagePageView::tr("Relevant change");
    if (Value == "unrelated_change")
      return LineagePageView::tr("Unrelated change");
    if (Value == "candidate_fix")
      return LineagePageView::tr("Candidate fix");
    if (Value == "patch_present")
      return LineagePageView::tr("Patch present");
    if (Value == "patch_absent")
      return LineagePageView::tr("Patch absent");
    if (Value == "partial_patch")
      return LineagePageView::tr("Partial patch");
    if (Value == "reverted_patch")
      return LineagePageView::tr("Reverted patch");
    if (Value == "inconclusive")
      return LineagePageView::tr("Inconclusive");
  }
  return Value;
}
QString identity(const QJsonObject &Row) {
  for (const auto *Key : {"id", "node", "edge", "conflict", "index"})
    if (Row.contains(Key))
      return QString::fromLatin1(Key) + ':' + cell(Row[Key]);
  return {};
}
} // namespace

LineagePageView::LineagePageView(LineageSession &S, const QStringList &Headers,
                                 const QStringList &Fields, QWidget *Parent)
    : QWidget(Parent), Session(S), Keys(Fields) {
  auto *Root = new QVBoxLayout(this);
  Root->setContentsMargins(0, 0, 0, 0);
  Model = new QStandardItemModel(this);
  Model->setHorizontalHeaderLabels(Headers);
  Table = new EvidenceTable(this);
  Table->setModel(Model);
  Table->setSelectionBehavior(QAbstractItemView::SelectRows);
  Table->setSelectionMode(QAbstractItemView::SingleSelection);
  Table->setEditTriggers(QAbstractItemView::NoEditTriggers);
  Table->setAlternatingRowColors(true);
  Table->setWordWrap(false);
  Table->setTextElideMode(Qt::ElideMiddle);
  Table->verticalHeader()->hide();
  Table->verticalHeader()->setSectionResizeMode(QHeaderView::ResizeToContents);
  Table->horizontalHeader()->setStretchLastSection(true);
  const auto Metrics = Table->horizontalHeader()->fontMetrics();
  for (int I = 0; I < Headers.size(); ++I)
    Table->setColumnWidth(I, std::max(Table->columnWidth(I),
                                      Metrics.horizontalAdvance(Headers[I]) +
                                          Metrics.height() * 2));
  Table->setContextMenuPolicy(Qt::CustomContextMenu);
  auto *Copy = new QAction(tr("Copy row evidence"), Table);
  Copy->setShortcut(QKeySequence::Copy);
  Copy->setShortcutContext(Qt::WidgetWithChildrenShortcut);
  Table->addAction(Copy);
  connect(Copy, &QAction::triggered, this, [this] {
    if (!selected().isEmpty())
      QApplication::clipboard()->setText(QString::fromUtf8(
          QJsonDocument(selected()).toJson(QJsonDocument::Indented)));
  });
  Root->addWidget(Table, 1);
  auto *Navigation = new QHBoxLayout;
  Status = new QLabel(tr("Choose a collection to begin."), this);
  Status->setObjectName("lineagePageStatus");
  Status->setTextFormat(Qt::PlainText);
  makeStatusLine(*Status);
  Root->addWidget(Status);
  RowsPerPage = new QSpinBox(this);
  RowsPerPage->setObjectName("lineagePageRows");
  RowsPerPage->setAccessibleName(tr("Rows per page"));
  RowsPerPage->setToolTip(
      tr("Rows per page. Reduce this value if a page exceeds its byte "
         "allowance."));
  RowsPerPage->setPrefix(tr("Rows: "));
  RowsPerPage->setRange(1, ::neverd::limits::diff::kMaxPageSize);
  RowsPerPage->setValue(::neverd::limits::gui::kComparisonPageSize);
  RowsPerPage->setKeyboardTracking(false);
  Previous = new QToolButton(this);
  Next = new QToolButton(this);
  Previous->setObjectName("lineagePreviousPage");
  Next->setObjectName("lineageNextPage");
  Previous->setAccessibleName(tr("Previous page"));
  Previous->setToolTip(tr("Previous page"));
  Next->setAccessibleName(tr("Next page"));
  Next->setToolTip(tr("Next page"));
  Retry = new QToolButton(this);
  Retry->setIcon(icon("refresh"));
  Retry->setAccessibleName(tr("Refresh page"));
  Retry->setToolTip(tr("Refresh page"));
  Navigation->addWidget(RowsPerPage);
  Navigation->addStretch();
  Navigation->addWidget(Previous);
  Navigation->addWidget(Next);
  Navigation->addWidget(Retry);
  Root->addLayout(Navigation);
  connect(Previous, &QToolButton::clicked, this,
          [this] { page(std::max<qint64>(0, Offset - RowsPerPage->value())); });
  connect(Next, &QToolButton::clicked, this, [this] { page(NextOffset); });
  connect(Retry, &QToolButton::clicked, this, &LineagePageView::refresh);
  connect(RowsPerPage, &QSpinBox::valueChanged, this,
          &LineagePageView::refresh);
  connect(&Session, &LineageSession::changed, this, &LineagePageView::buttons);
  connect(Table->selectionModel(), &QItemSelectionModel::currentRowChanged,
          this, [this] { emit selectionChanged(selected()); });
  connect(Table, &QTableView::activated, this,
          [this] { emit activated(selected()); });
  connect(Table, &QWidget::customContextMenuRequested, this,
          [this](const QPoint &Point) {
            const auto Index = Table->indexAt(Point);
            if (!Index.isValid())
              return;
            Table->setCurrentIndex(Index);
            QMenu Menu(this);
            auto *Value = Menu.addAction(tr("Copy full value"));
            auto *Evidence = Menu.addAction(tr("Copy row evidence"));
            const auto Choice =
                Menu.exec(Table->viewport()->mapToGlobal(Point));
            if (Choice == Value)
              QApplication::clipboard()->setText(
                  cell(selected()[Keys[Index.column()]]));
            if (Choice == Evidence)
              QApplication::clipboard()->setText(QString::fromUtf8(
                  QJsonDocument(selected()).toJson(QJsonDocument::Indented)));
          });
  updateTableMinimumHeight();
  buttons();
}
QJsonObject LineagePageView::selected() const {
  const auto Row = Table->currentIndex().row();
  return Row >= 0 && Row < Rows.size() ? Rows[Row].toObject() : QJsonObject{};
}
void LineagePageView::setQuery(QJsonObject NextQuery, const QString &SelectedID,
                               qint64 First) {
  const bool Same = Query == NextQuery;
  Query = std::move(NextQuery);
  PreferredSelection = SelectedID.isEmpty() ? QString() : "id:" + SelectedID;
  page(Same ? Offset : First);
}
void LineagePageView::clear() {
  ++Generation;
  Pending = false;
  Query = {};
  PreferredSelection.clear();
  Rows = {};
  Offset = 0;
  NextOffset = -1;
  Model->removeRows(0, Model->rowCount());
  Status->setText(tr("Select evidence to view."));
  buttons();
}
void LineagePageView::refresh() { page(Offset); }
void LineagePageView::changeEvent(QEvent *Event) {
  QWidget::changeEvent(Event);
  if (Event->type() == QEvent::LayoutDirectionChange)
    buttons();
  if (Event->type() == QEvent::FontChange ||
      Event->type() == QEvent::StyleChange)
    updateTableMinimumHeight();
}
void LineagePageView::updateTableMinimumHeight() {
  // Header and scrollbar geometry vary with the interface language, style
  // and font. Preserve space for a complete evidence row in a narrow window.
  const auto RowHeight = std::max(
      {Table->verticalHeader()->defaultSectionSize(),
       Table->verticalHeader()->minimumSectionSize(), Table->rowHeight(0)});
  Table->setMinimumHeight(Table->horizontalHeader()->sizeHint().height() +
                          RowHeight +
                          Table->horizontalScrollBar()->sizeHint().height() +
                          Table->frameWidth() * 2);
}
void LineagePageView::buttons() {
  Previous->setArrowType(isRightToLeft() ? Qt::RightArrow : Qt::LeftArrow);
  Next->setArrowType(isRightToLeft() ? Qt::LeftArrow : Qt::RightArrow);
  const bool Available = Session.ready() && !Session.cancelRequested() &&
                         !Query.isEmpty() && !Pending;
  RowsPerPage->setEnabled(Available);
  Previous->setEnabled(Available && Offset > 0);
  Next->setEnabled(Available && NextOffset >= 0);
  Retry->setEnabled(Available);
}
void LineagePageView::page(qint64 First) {
  if (First < 0 || Query.isEmpty() || Session.revision().isEmpty())
    return;
  const auto Current = ++Generation;
  const auto Revision = Session.revision();
  const auto Selection =
      PreferredSelection.isEmpty() ? identity(selected()) : PreferredSelection;
  Pending = true;
  buttons();
  Status->setText(tr("Loading evidence…"));
  auto Request = Query;
  Request["revision"] = Revision;
  if (!Request.contains("focus"))
    Request["offset"] = First;
  Request["limit"] = RowsPerPage->value();
  Session.request(
      "lineage_query", {{"query", Request}}, this,
      [this, Current, Revision, Selection, First](const QJsonObject &Reply) {
        if (Current != Generation)
          return;
        Pending = false;
        if (Revision != Session.revision()) {
          buttons();
          return;
        }
        if (Reply["status"] != QJsonValue("ok")) {
          const auto Failure = Reply["error"].toObject();
          auto Error = Failure["message"].toString();
          if (Failure["code"] == QJsonValue("resource_limited"))
            Error += tr(" Try fewer rows or adjust Resource settings.");
          Status->setText(Rows.isEmpty()
                              ? Error
                              : tr("%1 — previous page retained").arg(Error));
          Status->setToolTip(Status->text());
          buttons();
          return;
        }
        const auto Page = Reply["payload"].toObject();
        if (Page["revision"].toString() != Revision) {
          Status->setText(tr("The graph changed. Refresh this page."));
          buttons();
          return;
        }
        PreferredSelection.clear();
        Query.remove("focus");
        Rows = Page["items"].toArray();
        Offset = Page["offset"].toInteger(First);
        NextOffset = Page["next_offset"].isDouble()
                         ? Page["next_offset"].toInteger()
                         : -1;
        Model->removeRows(0, Model->rowCount());
        int Selected = 0;
        for (int R = 0; R < Rows.size(); ++R) {
          if (!Rows[R].isObject())
            Rows[R] = QJsonObject{{"warning", Rows[R]}};
          const auto Row = Rows[R].toObject();
          if (!Selection.isEmpty() && identity(Row) == Selection)
            Selected = R;
          QList<QStandardItem *> Items;
          for (const auto &Key : Keys) {
            const auto Full = cell(Row[Key]);
            auto *Item = new QStandardItem(display(Row, Key));
            Item->setToolTip(Full);
            Items.append(Item);
          }
          Model->appendRow(Items);
        }
        updateTableMinimumHeight();
        if (!Rows.isEmpty())
          Table->setCurrentIndex(Model->index(Selected, 0));
        Status->setText(tr("%1–%2 of %3")
                            .arg(Rows.isEmpty() ? 0 : Offset + 1)
                            .arg(Offset + Rows.size())
                            .arg(Page["total"].toInteger()));
        Status->setToolTip(Status->text());
        buttons();
        emit pageChanged(Rows);
      });
}
} // namespace neverd::gui
