//===- LuaRecoveryDialog.cpp - Desktop Lua source recovery ===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// One report drives function selection, mapping navigation, copy and both
/// exports.
//===----------------------------------------------------------------------===//

#include "LuaRecoveryDialog.h"

#include <QApplication>
#include <QClipboard>
#include <QComboBox>
#include <QFile>
#include <QFileDialog>
#include <QFontDatabase>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QJsonDocument>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QSplitter>
#include <QTabWidget>
#include <QTextBlock>
#include <QVBoxLayout>
using namespace neverd::gui;
namespace {
QString diagnostics(const QJsonObject &Object) {
  QStringList Errors;
  for (auto Entry : Object["diagnostics"].toArray()) {
    auto E = Entry.toObject();
    Errors << E.value("message").toString();
  }
  return Errors.join(QStringLiteral("\n"));
}
} // namespace
LuaRecoveryDialog::LuaRecoveryDialog(const QString &Worker, QWidget *Parent)
    : QDialog(Parent) {
  setAttribute(Qt::WA_DeleteOnClose);
  setWindowTitle(tr("Recover Lua source"));
  auto *Layout = new QVBoxLayout(this);
  auto *Inputs = new QFormLayout;
  auto *FileRow = new QHBoxLayout;
  Path = new QLineEdit(this);
  Path->setObjectName("luaPath");
  Browse = new QPushButton(tr("Browse…"), this);
  FileRow->addWidget(Path);
  FileRow->addWidget(Browse);
  Inputs->addRow(tr("&Artifact"), FileRow);
  Profile = new QComboBox(this);
  Profile->setObjectName("luaProfile");
  Profile->addItem(tr("Detect from artifact"), "auto");
  Profile->addItem(tr("PUC Lua"), "puc");
  Profile->addItem(tr("LuaJIT"), "luajit");
  Profile->addItem(tr("Upstream Luau 0.659"), "luau-0.659");
  Inputs->addRow(tr("&Profile"), Profile);
  Layout->addLayout(Inputs);
  auto *Run = new QHBoxLayout;
  Recover = new QPushButton(tr("Recover"), this);
  Recover->setObjectName("luaRecover");
  Cancel = new QPushButton(tr("Cancel"), this);
  Cancel->setObjectName("luaCancel");
  Recover->setDefault(true);
  Run->addWidget(Recover);
  Run->addWidget(Cancel);
  Run->addStretch();
  Layout->addLayout(Run);
  Status = new QLabel(tr("Starting analysis worker…"), this);
  Status->setObjectName("luaStatus");
  Status->setTextFormat(Qt::PlainText);
  Status->setWordWrap(true);
  Status->setTextInteractionFlags(Qt::TextSelectableByMouse);
  Layout->addWidget(Status);
  Prototypes = new QComboBox(this);
  Prototypes->setObjectName("luaPrototypes");
  Prototypes->setAccessibleName(tr("Prototype"));
  Layout->addWidget(Prototypes);
  auto *Panes = new QSplitter(this);
  Source = new QPlainTextEdit(this);
  Source->setObjectName("luaSource");
  Source->setReadOnly(true);
  Source->setAccessibleName(tr("Source"));
  Source->setFont(QFontDatabase::systemFont(QFontDatabase::FixedFont));
  Mappings = new QListWidget(this);
  Mappings->setObjectName("luaMappings");
  Mappings->setAccessibleName(tr("Bytecode mappings"));
  auto *Tabs = new QTabWidget(this);
  Tabs->addTab(Source, tr("Source"));
  Evidence = new QPlainTextEdit(this);
  Evidence->setReadOnly(true);
  Evidence->setObjectName("luaEvidence");
  Evidence->setFont(Source->font());
  Tabs->addTab(Evidence, tr("Evidence"));
  Panes->addWidget(Tabs);
  Panes->addWidget(Mappings);
  Panes->setStretchFactor(0, 3);
  Panes->setStretchFactor(1, 1);
  Layout->addWidget(Panes, 1);
  auto *Exports = new QHBoxLayout;
  Copy = new QPushButton(tr("Copy source"), this);
  Copy->setObjectName("luaCopy");
  Save = new QPushButton(tr("Export source…"), this);
  SaveReport = new QPushButton(tr("Export report…"), this);
  Exports->addStretch();
  for (auto *Button : {Copy, Save, SaveReport})
    Exports->addWidget(Button);
  Layout->addLayout(Exports);
  for (auto *Button : {Browse, Cancel, Copy, Save, SaveReport})
    Button->setAutoDefault(false);
  connect(Browse, &QPushButton::clicked, this, [this] {
    auto F = QFileDialog::getOpenFileName(this, tr("Open Lua artifact"));
    if (!F.isEmpty())
      Path->setText(F);
  });
  auto Invalidate = [this] {
    clearReport();
    if (Ready)
      Status->clear();
    updateActions();
  };
  connect(Path, &QLineEdit::textChanged, this, Invalidate);
  connect(Profile, &QComboBox::currentIndexChanged, this, Invalidate);
  connect(Recover, &QPushButton::clicked, this, &LuaRecoveryDialog::start);
  connect(Cancel, &QPushButton::clicked, this, [this] {
    Client.request("cancel", {{"request_id", Active}});
    Cancel->setEnabled(false);
    Status->setText(tr("Cancelling…"));
  });
  connect(Prototypes, &QComboBox::currentIndexChanged, this,
          &LuaRecoveryDialog::select);
  connect(Mappings, &QListWidget::currentRowChanged, this, [this](int Row) {
    if (Row < 0)
      return;
    int Line = Mappings->item(Row)->data(Qt::UserRole).toInt();
    if (!Line)
      return;
    auto Block = Source->document()->findBlockByNumber(Line - 1);
    if (Block.isValid()) {
      Source->setTextCursor(QTextCursor(Block));
      Source->centerCursor();
    }
  });
  connect(Copy, &QPushButton::clicked, this, [this] {
    auto Row =
        Report["functions"].toArray().at(Prototypes->currentIndex()).toObject();
    QApplication::clipboard()->setText(Row["source"].toString());
  });
  auto Export = [this](bool Manifest) {
    auto File = QFileDialog::getSaveFileName(
        this, tr("Export to a new file"), {},
        Manifest ? tr("JSON (*.json)") : tr("Lua source (*.lua *.luau)"),
        nullptr, QFileDialog::DontConfirmOverwrite);
    if (!File.isEmpty())
      exportFile(File, Manifest);
  };
  connect(Save, &QPushButton::clicked, this, [Export] { Export(false); });
  connect(SaveReport, &QPushButton::clicked, this, [Export] { Export(true); });
  connect(&Client, &EngineClient::message, this, &LuaRecoveryDialog::receive);
  connect(&Client, &EngineClient::failure, this, [this](const QString &Error) {
    Ready = false;
    Active.clear();
    clearReport();
    Status->setText(Error);
    updateActions();
  });
  connect(&Client, &EngineClient::stopped, this, [this] {
    if (Ready || !Active.isEmpty()) {
      Ready = false;
      Active.clear();
      clearReport();
      Status->setText(tr("Analysis worker stopped."));
      updateActions();
    }
  });
  updateActions();
  resize(860, 560);
  Client.start(Worker);
}
void LuaRecoveryDialog::clearReport() {
  Report = {};
  Prototypes->clear();
  Source->clear();
  Mappings->clear();
  Evidence->clear();
}
void LuaRecoveryDialog::updateActions() {
  bool Idle = Active.isEmpty();
  Browse->setEnabled(Idle);
  Path->setEnabled(Idle);
  Profile->setEnabled(Idle);
  Recover->setEnabled(Ready && Idle && !Path->text().isEmpty());
  Cancel->setEnabled(!Idle);
  bool Has = Prototypes->currentIndex() >= 0;
  Prototypes->setEnabled(Has);
  Copy->setEnabled(Has);
  Save->setEnabled(Has);
  SaveReport->setEnabled(!Report.isEmpty());
}
void LuaRecoveryDialog::start() {
  if (!Ready || !Active.isEmpty() || Path->text().isEmpty())
    return;
  clearReport();
  Active = Client.request(
      "lua_recover",
      {{"path", Path->text()}, {"profile", Profile->currentData().toString()}});
  Status->setText(tr("Recovering source…"));
  updateActions();
}
void LuaRecoveryDialog::receive(const QJsonObject &M) {
  if (M["type"] == "hello") {
    Ready = M["protocol_major"] == 1 &&
            M["capabilities"].toArray().contains("lua_recover");
    Status->setText(Ready ? QString()
                          : tr("This worker does not support Lua recovery."));
    updateActions();
    return;
  }
  if (Active.isEmpty() || M["request_id"].toString() != Active)
    return;
  if (M["status"] == "progress") {
    Status->setText(tr("Reading artifact…"));
    return;
  }
  Active.clear();
  if (M["status"] != "ok") {
    Status->setText(M["status"] == "cancelled"
                        ? tr("Cancelled")
                        : M["error"].toObject()["message"].toString());
    updateActions();
    return;
  }
  Report = M["payload"].toObject();
  int Root = -1;
  auto Functions = Report["functions"].toArray();
  for (auto Item : Functions) {
    auto F = Item.toObject();
    int Id = F["id"].toInt();
    if (Report["root"].isDouble() && Id == Report["root"].toInt())
      Root = Prototypes->count();
    auto Name = F["name"].toString();
    Prototypes->addItem(tr("Prototype %1").arg(Id) +
                        (Name.isEmpty() ? QString() : " · " + Name));
  }
  Prototypes->setCurrentIndex(Root >= 0 ? Root : Functions.isEmpty() ? -1 : 0);
  select(Prototypes->currentIndex());
  updateActions();
}
void LuaRecoveryDialog::select(int Index) {
  Source->clear();
  Mappings->clear();
  auto Row = Report["functions"].toArray().at(Index).toObject();
  auto E = Report["evidence"].toObject();
  QJsonObject Details = Report;
  Details.remove("functions");
  auto Metadata = Row;
  Metadata.remove("source");
  Metadata.remove("instructions");
  Details["prototype"] = Metadata;
  Evidence->setPlainText(
      QJsonDocument(Details).toJson(QJsonDocument::Indented));
  QStringList State;
  for (auto Text :
       {E["dialect"].toString(), Report["status"].toString(),
        Row["status"].toString(), diagnostics(Report), diagnostics(Row)})
    if (!Text.isEmpty() && !State.contains(Text))
      State << Text;
  Status->setText(State.join(" · "));
  Source->setPlainText(Row["source"].toString());
  for (auto Value : Row["instructions"].toArray()) {
    auto I = Value.toObject();
    auto *Item = new QListWidgetItem(tr("PC %1   0x%2   %3")
                                         .arg(I["pc"].toInt())
                                         .arg(I["offset"].toInteger(), 0, 16)
                                         .arg(I["opcode"].toString()),
                                     Mappings);
    Item->setData(Qt::UserRole, I["source_line"].toInt());
    if (I.contains("original_line"))
      Item->setToolTip(tr("Original line %1").arg(I["original_line"].toInt()));
  }
}
bool LuaRecoveryDialog::exportFile(const QString &File, bool Manifest) {
  if (Report.isEmpty() || (!Manifest && Prototypes->currentIndex() < 0))
    return false;
  auto Bytes = Manifest
                   ? QJsonDocument(Report).toJson(QJsonDocument::Compact) + '\n'
                   : Report["functions"]
                         .toArray()
                         .at(Prototypes->currentIndex())
                         .toObject()["source"]
                         .toString()
                         .toUtf8();
  QFile Out(File);
  if (!Out.open(QIODevice::WriteOnly | QIODevice::NewOnly)) {
    Status->setText(Out.errorString());
    return false;
  }
  if (Out.write(Bytes) != Bytes.size() || !Out.flush()) {
    auto Error = Out.errorString();
    Out.remove();
    Status->setText(Error);
    return false;
  }
  return true;
}
