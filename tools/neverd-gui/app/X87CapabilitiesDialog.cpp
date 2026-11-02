//===- X87CapabilitiesDialog.cpp - Inspect x87 support
//---------------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Uses the normal dispatcher to inspect static support, without loading or
/// executing a binary or interpreting decode availability as qualification.
//===----------------------------------------------------------------------===//

#include "X87CapabilitiesDialog.h"

#include "Icons.h"
#include "QueryService.h"
#include "Theme.h"
#include "X87CapabilitiesModel.h"

#include <QComboBox>
#include <QDialogButtonBox>
#include <QGridLayout>
#include <QHeaderView>
#include <QItemSelectionModel>
#include <QJsonObject>
#include <QLabel>
#include <QLineEdit>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QSignalBlocker>
#include <QSortFilterProxyModel>
#include <QSplitter>
#include <QTableView>
#include <QTimer>
#include <QVBoxLayout>

namespace neverd::gui {
X87CapabilitiesDialog::X87CapabilitiesDialog(QueryService &Service,
                                             QWidget *Parent)
    : QDialog(Parent), Queries(Service), Model(new X87CapabilitiesModel(this)),
      Filter(new QSortFilterProxyModel(this)) {
  setObjectName("x87Capabilities");
  setWindowTitle(tr("x87 support"));
  setWindowIcon(icon("help"));
  // Initial geometry is a presentation hint, in font-relative units.
  resize(fontMetrics().averageCharWidth() * 130, fontMetrics().height() * 40);
  auto *Layout = new QVBoxLayout(this);
  auto *Intro =
      new QLabel(tr("Inspect support for an instruction and execution "
                    "profile. Numerical results and architectural "
                    "state are qualified separately."),
                 this);
  Intro->setWordWrap(true);
  Layout->addWidget(Intro);
  auto *Dimensions = new QGridLayout;
  const struct {
    const char *Key;
    QString Label;
  } Fields[] = {{"source_mode", tr("&Source mode:")},
                {"abi", tr("Source &ABI:")},
                {"state_path", tr("State &path:")},
                {"consumer", tr("&Consumer:")},
                {"backend", tr("&Backend:")},
                {"destination", tr("&Destination:")}};
  int Position = 0;
  for (const auto &Field : Fields) {
    auto *Selector = new QComboBox(this);
    Selector->setObjectName(QString::fromLatin1(Field.Key));
    Selector->setSizeAdjustPolicy(
        QComboBox::AdjustToMinimumContentsLengthWithIcon);
    Selector->setMinimumContentsLength(14);
    Selector->setEnabled(false);
    auto *Label = new QLabel(Field.Label, this);
    Label->setBuddy(Selector);
    Dimensions->addWidget(Label, Position / 3 * 2, Position % 3);
    Dimensions->addWidget(Selector, Position / 3 * 2 + 1, Position % 3);
    Selectors.insert(QString::fromLatin1(Field.Key), Selector);
    connect(Selector, &QComboBox::currentTextChanged, this,
            &X87CapabilitiesDialog::selectDimensions);
    ++Position;
  }
  Layout->addLayout(Dimensions);
  auto *Search = new QLineEdit(this);
  Search->setObjectName("x87Filter");
  Search->setPlaceholderText(tr("Filter instructions, operands or support…"));
  Search->setAccessibleName(tr("Filter x87 instructions"));
  Search->setClearButtonEnabled(true);
  Layout->addWidget(Search);
  Status = new QLabel(this);
  Status->setObjectName("x87Status");
  Status->setWordWrap(true);
  Status->setTextFormat(Qt::PlainText);
  Status->setTextInteractionFlags(Qt::TextSelectableByMouse);
  Layout->addWidget(Status);
  auto *Split = new QSplitter(Qt::Vertical, this);
  Split->setChildrenCollapsible(false);
  Layout->addWidget(Split, 1);
  Table = new QTableView(Split);
  Table->setObjectName("x87Forms");
  Table->setAccessibleName(tr("x87 instruction support"));
  Filter->setSourceModel(Model);
  Filter->setFilterKeyColumn(-1);
  Filter->setFilterCaseSensitivity(Qt::CaseInsensitive);
  Table->setModel(Filter);
  Table->setSelectionBehavior(QAbstractItemView::SelectRows);
  Table->setSelectionMode(QAbstractItemView::SingleSelection);
  Table->setEditTriggers(QAbstractItemView::NoEditTriggers);
  Table->setWordWrap(false);
  Table->setAlternatingRowColors(true);
  Table->setShowGrid(false);
  Table->verticalHeader()->hide();
  Table->horizontalHeader()->setStretchLastSection(true);
  Table->horizontalHeader()->setDefaultAlignment(Qt::AlignLeft |
                                                 Qt::AlignVCenter);
  connect(&Theme::instance(), &Theme::changed, this, [this] {
    // Style sheets can change header metrics after the report first arrives.
    QTimer::singleShot(0, Table, &QTableView::resizeColumnsToContents);
  });
  Details = new QPlainTextEdit(Split);
  Details->setObjectName("x87Details");
  Details->setAccessibleName(
      tr("Selected instruction requirements and limits"));
  Details->setReadOnly(true);
  Split->setStretchFactor(0, 3);
  Split->setStretchFactor(1, 2);
  connect(Search, &QLineEdit::textChanged, Filter,
          &QSortFilterProxyModel::setFilterFixedString);
  connect(Filter, &QAbstractItemModel::rowsRemoved, this,
          &X87CapabilitiesDialog::showDetails);
  connect(Table->selectionModel(), &QItemSelectionModel::currentRowChanged,
          this, &X87CapabilitiesDialog::showDetails);
  auto *Buttons = new QDialogButtonBox(QDialogButtonBox::Close, this);
  Reload = Buttons->addButton(tr("&Reload"), QDialogButtonBox::ActionRole);
  Reload->setObjectName("x87Reload");
  Reload->setEnabled(false);
  connect(Reload, &QPushButton::clicked, this, &X87CapabilitiesDialog::refresh);
  connect(Buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
  Layout->addWidget(Buttons);
  Status->setText(tr("Waiting for the analysis worker."));
}

X87CapabilitiesDialog::~X87CapabilitiesDialog() {
  Queries.unsubscribeOwner(this);
}

void X87CapabilitiesDialog::setProviderAvailable(bool IsConnected,
                                                 bool IsSupported) {
  if (ProviderKnown && Connected == IsConnected && Supported == IsSupported)
    return;
  ProviderKnown = true;
  Connected = IsConnected;
  Supported = IsSupported;
  refresh();
}

void X87CapabilitiesDialog::refresh() {
  const auto Current = ++Generation;
  Queries.unsubscribeOwner(this);
  Model->clear();
  Details->clear();
  for (auto *Selector : Selectors)
    Selector->setEnabled(false);
  Reload->setEnabled(Connected && Supported);
  if (!Connected) {
    Status->setText(tr("The analysis worker is disconnected."));
    return;
  }
  if (!Supported) {
    Status->setText(tr("This engine does not provide an x87 support report."));
    return;
  }
  Status->setText(tr("Loading x87 support…"));
  Queries.subscribe(
      {"x87_capabilities", {}}, this,
      [this, Current](const QJsonObject &Response) {
        if (Current != Generation)
          return;
        if (Response.value("status") != QLatin1String("ok")) {
          const auto Error = Response.value("error").toObject();
          Status->setText(tr("Could not load x87 support: %1")
                              .arg(Error.value("message").toString(
                                  Response.value("status").toString())));
          return;
        }
        QString Error;
        if (!Model->setReport(Response.value("payload").toObject(), Error)) {
          Status->setText(Error);
          return;
        }
        const QJsonObject Defaults{
            {"source_mode", "x86-64"},       {"abi", "system-v"},
            {"state_path", "owned-context"}, {"consumer", "concrete"},
            {"backend", "portable"},         {"destination", "arm64"}};
        for (auto It = Selectors.begin(); It != Selectors.end(); ++It) {
          const QSignalBlocker Block(It.value());
          auto Previous = It.value()->currentText();
          if (Previous.isEmpty())
            Previous = Defaults.value(It.key()).toString();
          It.value()->clear();
          It.value()->addItems(Model->values(It.key()));
          const int Index = It.value()->findText(Previous);
          if (Index >= 0)
            It.value()->setCurrentIndex(Index);
          It.value()->setEnabled(true);
        }
        selectDimensions();
        Table->resizeColumnsToContents();
        if (Filter->rowCount())
          Table->setCurrentIndex(Filter->index(0, 0));
      });
}

void X87CapabilitiesDialog::selectDimensions() {
  QJsonObject Dimensions;
  for (auto It = Selectors.begin(); It != Selectors.end(); ++It)
    Dimensions.insert(It.key(), It.value()->currentText());
  Model->setDimensions(Dimensions);
  Status->setText(tr("%1 instruction forms. Static inventory; no host probe, "
                     "execution or proof.")
                      .arg(Model->rowCount()));
  showDetails();
}

void X87CapabilitiesDialog::showDetails() {
  const auto Index = Filter->mapToSource(Table->currentIndex());
  if (!Index.isValid()) {
    Details->setPlainText(
        tr("Select an instruction to inspect its requirements. "
           "Clear the filter if no forms are visible."));
    return;
  }
  const auto Form = Model->form(Index.row());
  const auto Cell = Model->capability(Index.row());
  const auto Path = Model->sourcePath();
  const auto &Report = Model->report();
  QStringList Lines{Form.value("id").toString()};
  if (Cell.isEmpty()) {
    Lines << tr("This combination is not listed; it is unsupported.");
  } else {
    Lines << tr("Reason: %1").arg(Cell.value("reason").toString())
          << tr("Requirements: %1").arg(Cell.value("requirements").toString())
          << tr("Regression sources: %1")
                 .arg(Cell.value("test_sources").toString());
  }
  Lines << QString();
  if (Path.isEmpty()) {
    Lines << tr("This source mode / ABI / state path is not listed.");
  } else {
    Lines
        << tr("Source path available: %1; default for this ABI: %2")
               .arg(Path.value("available").toBool() ? tr("Yes") : tr("No"),
                    Path.value("default_path").toBool() ? tr("Yes") : tr("No"))
        << tr("long double: %1").arg(Path.value("long_double").toString())
        << tr("Call contract: %1").arg(Path.value("call_contract").toString());
  }
  Lines << QString() << Report.value("precision").toString()
        << Report.value("evidence").toString()
        << Report.value("matrix_contract").toString();
  Details->setPlainText(Lines.join(QLatin1Char('\n')));
}
} // namespace neverd::gui
