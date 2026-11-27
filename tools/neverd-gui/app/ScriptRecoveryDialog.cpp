//===- ScriptRecoveryDialog.cpp - Desktop script source recovery ===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// One report drives function selection, mapping navigation, copy and both
/// exports.
//===----------------------------------------------------------------------===//

#include "ScriptRecoveryDialog.h"

#include "neverd/analysis/PythonVersions.h"

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
#include <QSignalBlocker>
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
ScriptRecoveryDialog::ScriptRecoveryDialog(const QString &Worker,
                                           QWidget *Parent,
                                           ScriptLanguage Language)
    : QDialog(Parent), Python(Language == ScriptLanguage::Python),
      Operation(Python ? QStringLiteral("python_recover")
                       : QStringLiteral("lua_recover")) {
  setAttribute(Qt::WA_DeleteOnClose);
  setWindowTitle(Python ? tr("Recover Python source")
                        : tr("Recover Lua source"));
  const QString Prefix =
      Python ? QStringLiteral("python") : QStringLiteral("lua");
  auto *Layout = new QVBoxLayout(this);
  auto *Inputs = new QFormLayout;
  auto *FileRow = new QHBoxLayout;
  Path = new QLineEdit(this);
  Path->setObjectName(Prefix + QStringLiteral("Path"));
  Browse = new QPushButton(tr("Browse…"), this);
  FileRow->addWidget(Path);
  FileRow->addWidget(Browse);
  if (Python) {
    Directory = new QPushButton(tr("Folder…"), this);
    Directory->setAutoDefault(false);
    FileRow->addWidget(Directory);
    connect(Directory, &QPushButton::clicked, this, [this] {
      auto Folder = QFileDialog::getExistingDirectory(
          this, tr("Open PyInstaller folder"));
      if (!Folder.isEmpty())
        Path->setText(Folder);
    });
  }
  Inputs->addRow(tr("&Artifact"), FileRow);
  Profile = new QComboBox(this);
  Profile->setObjectName(Prefix + QStringLiteral("Profile"));
  Profile->addItem(tr("Detect from artifact"), "auto");
  if (Python) {
    for (const auto &Release : neverd::python::Releases) {
      auto Number = QString::fromLatin1(Release.Number);
      Profile->addItem(QStringLiteral("CPython ") + Number,
                       QStringLiteral("cpython-") + Number);
    }
    Profile->addItem(QStringLiteral("Ren’Py"), "renpy");
  } else {
    Profile->addItem(tr("PUC Lua"), "puc");
    Profile->addItem(tr("LuaJIT"), "luajit");
    Profile->addItem(QStringLiteral("LuaJIT 1.0"), "luajit-1.0");
    Profile->addItem(QStringLiteral("LuaJIT 1.1"), "luajit-1.1");
    Profile->addItem(tr("Upstream Luau"), "luau-upstream");
    Profile->addItem(tr("Upstream Luau 0.659"), "luau-0.659");
  }
  Inputs->addRow(tr("&Profile"), Profile);
  if (Python) {
    Components = new QComboBox(this);
    Components->setObjectName("pythonComponents");
    resetComponents();
    Inputs->addRow(tr("Component"), Components);
  }
  Layout->addLayout(Inputs);
  auto *Run = new QHBoxLayout;
  Recover = new QPushButton(tr("Recover"), this);
  Recover->setObjectName(Prefix + QStringLiteral("Recover"));
  Cancel = new QPushButton(tr("Cancel"), this);
  Cancel->setObjectName(Prefix + QStringLiteral("Cancel"));
  Recover->setDefault(true);
  Run->addWidget(Recover);
  Run->addWidget(Cancel);
  Run->addStretch();
  Layout->addLayout(Run);
  Status = new QLabel(tr("Starting analysis worker…"), this);
  Status->setObjectName(Prefix + QStringLiteral("Status"));
  Status->setTextFormat(Qt::PlainText);
  Status->setWordWrap(true);
  Status->setTextInteractionFlags(Qt::TextSelectableByMouse);
  Layout->addWidget(Status);
  Prototypes = new QComboBox(this);
  Prototypes->setObjectName(Prefix + QStringLiteral("Prototypes"));
  Prototypes->setAccessibleName(Python ? tr("Function") : tr("Prototype"));
  Layout->addWidget(Prototypes);
  auto *Panes = new QSplitter(this);
  Source = new QPlainTextEdit(this);
  Source->setObjectName(Prefix + QStringLiteral("Source"));
  Source->setReadOnly(true);
  Source->setAccessibleName(tr("Source"));
  Source->setFont(QFontDatabase::systemFont(QFontDatabase::FixedFont));
  Mappings = new QListWidget(this);
  Mappings->setObjectName(Prefix + QStringLiteral("Mappings"));
  Mappings->setAccessibleName(tr("Bytecode mappings"));
  auto *Tabs = new QTabWidget(this);
  Tabs->addTab(Source, tr("Source"));
  Evidence = new QPlainTextEdit(this);
  Evidence->setReadOnly(true);
  Evidence->setObjectName(Prefix + QStringLiteral("Evidence"));
  Evidence->setFont(Source->font());
  Tabs->addTab(Evidence, tr("Evidence"));
  Panes->addWidget(Tabs);
  Panes->addWidget(Mappings);
  Panes->setStretchFactor(0, 3);
  Panes->setStretchFactor(1, 1);
  Layout->addWidget(Panes, 1);
  auto *Exports = new QHBoxLayout;
  Copy = new QPushButton(tr("Copy source"), this);
  Copy->setObjectName(Prefix + QStringLiteral("Copy"));
  Save = new QPushButton(tr("Export source…"), this);
  SaveReport = new QPushButton(tr("Export report…"), this);
  if (Python) {
    Native = new QPushButton(tr("Open in native analysis…"), this);
    Native->setObjectName("pythonNative");
    Native->setAutoDefault(false);
    Exports->addWidget(Native);
    connect(Native, &QPushButton::clicked, this, [this] {
      auto File = QFileDialog::getSaveFileName(
          this, tr("Save native component to a new file"), {}, {}, nullptr,
          QFileDialog::DontConfirmOverwrite);
      if (!File.isEmpty())
        openNative(File);
    });
  }
  Exports->addStretch();
  for (auto *Button : {Copy, Save, SaveReport})
    Exports->addWidget(Button);
  Layout->addLayout(Exports);
  for (auto *Button : {Browse, Cancel, Copy, Save, SaveReport})
    Button->setAutoDefault(false);
  connect(Browse, &QPushButton::clicked, this, [this] {
    auto F = QFileDialog::getOpenFileName(
        this, Python ? tr("Open Python artifact") : tr("Open Lua artifact"));
    if (!F.isEmpty())
      Path->setText(F);
  });
  auto Invalidate = [this] {
    resetComponents();
    clearReport();
    if (Ready)
      Status->clear();
    updateActions();
  };
  connect(Path, &QLineEdit::textChanged, this, Invalidate);
  connect(Profile, &QComboBox::currentIndexChanged, this, Invalidate);
  if (Components)
    connect(Components, &QComboBox::currentIndexChanged, this, [this] {
      if (Ready && Active.isEmpty() && !Report.isEmpty())
        start();
    });
  connect(Recover, &QPushButton::clicked, this, &ScriptRecoveryDialog::start);
  connect(Cancel, &QPushButton::clicked, this, [this] {
    Client.request("cancel", {{"request_id", Active}});
    Cancel->setEnabled(false);
    Status->setText(tr("Cancelling…"));
  });
  connect(Prototypes, &QComboBox::currentIndexChanged, this,
          &ScriptRecoveryDialog::select);
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
        Manifest ? tr("JSON (*.json)")
        : Python ? tr("Python source (*.py)")
                 : tr("Lua source (*.lua *.luau)"),
        nullptr, QFileDialog::DontConfirmOverwrite);
    if (!File.isEmpty())
      exportFile(File, Manifest);
  };
  connect(Save, &QPushButton::clicked, this, [Export] { Export(false); });
  connect(SaveReport, &QPushButton::clicked, this, [Export] { Export(true); });
  connect(&Client, &EngineClient::message, this,
          &ScriptRecoveryDialog::receive);
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
void ScriptRecoveryDialog::resetComponents() {
  if (!Components)
    return;
  QSignalBlocker Block(Components);
  Components->clear();
  Components->addItem(tr("Entry scripts"), -1);
}
int ScriptRecoveryDialog::selectedComponent() const {
  if (!Components)
    return -1;
  return Components->currentData().toInt();
}
bool ScriptRecoveryDialog::openNative(const QString &File) {
  if (!Native || !Native->isEnabled() || File.isEmpty())
    return false;
  const int Id = std::max(0, selectedComponent());
  const auto Row = Report["components"].toArray().at(Id).toObject();
  NativeDestination = File;
  Active = Client.request("python_export_native",
                          {{"path", Path->text()},
                           {"profile", Profile->currentData().toString()},
                           {"component", Id},
                           {"sha256", Row["sha256"]},
                           {"destination", File}});
  Status->setText(tr("Exporting native component…"));
  updateActions();
  return true;
}
void ScriptRecoveryDialog::clearReport() {
  Report = {};
  Prototypes->clear();
  Source->clear();
  Mappings->clear();
  Evidence->clear();
}
void ScriptRecoveryDialog::updateActions() {
  bool Idle = Active.isEmpty();
  Browse->setEnabled(Idle);
  if (Directory)
    Directory->setEnabled(Idle);
  if (Components)
    Components->setEnabled(Idle && Components->count() > 1);
  if (Native)
    Native->setEnabled(Ready && Idle &&
                       Report["components"]
                               .toArray()
                               .at(std::max(0, selectedComponent()))
                               .toObject()["backend"] == "native");
  Path->setEnabled(Idle);
  Profile->setEnabled(Idle);
  Recover->setEnabled(Ready && Idle && !Path->text().isEmpty());
  Cancel->setEnabled(!Idle);
  bool Has = Prototypes->currentIndex() >= 0;
  Prototypes->setEnabled(Has);
  auto Row =
      Report["functions"].toArray().at(Prototypes->currentIndex()).toObject();
  Copy->setEnabled(Has && Row["source"].isString());
  Save->setEnabled(
      Has && (Row["source"].isString() || Row["source_base64"].isString()));
  SaveReport->setEnabled(!Report.isEmpty());
}
void ScriptRecoveryDialog::start() {
  if (!Ready || !Active.isEmpty() || Path->text().isEmpty())
    return;
  clearReport();
  QJsonObject Payload{{"path", Path->text()},
                      {"profile", Profile->currentData().toString()}};
  if (Python)
    Payload["component"] = selectedComponent();
  NativeDestination.clear();
  Active = Client.request(Operation, Payload);
  Status->setText(tr("Recovering source…"));
  updateActions();
}
void ScriptRecoveryDialog::receive(const QJsonObject &M) {
  if (M["type"] == "hello") {
    Ready = M["protocol_major"] == 1 &&
            M["capabilities"].toArray().contains(Operation);
    Status->setText(Ready ? QString()
                    : Python
                        ? tr("Python recovery is unavailable in this worker.")
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
  if (!NativeDestination.isEmpty()) {
    auto Data = M["payload"].toObject();
    if (Data["status"] == "exported") {
      Status->setText(tr("Native component exported."));
      emit nativeReady(NativeDestination);
    } else
      Status->setText(Data["message"].toString(Data["status"].toString()));
    NativeDestination.clear();
    updateActions();
    return;
  }
  Report = M["payload"].toObject();
  if (Components) {
    int Selected = selectedComponent();
    QSignalBlocker Block(Components);
    resetComponents();
    for (auto V : Report["components"].toArray()) {
      auto C = V.toObject();
      auto Id = C["id"].toInt();
      Components->addItem(
          QStringLiteral("%1 · %2 · %3")
              .arg(Id)
              .arg(C["name"].toString(), C["status"].toString()),
          Id);
    }
    Components->setCurrentIndex(std::max(0, Components->findData(Selected)));
  }
  int Root = -1;
  auto Functions = Report["functions"].toArray();
  for (auto Item : Functions) {
    auto F = Item.toObject();
    int Id = F["id"].toInt();
    if (Report["root"].isDouble() && Id == Report["root"].toInt())
      Root = Prototypes->count();
    auto Name = F["name"].toString();
    Prototypes->addItem(
        (Python ? tr("Function %1") : tr("Prototype %1")).arg(Id) +
        (Name.isEmpty() ? QString() : " · " + Name));
  }
  Prototypes->setCurrentIndex(Root >= 0 ? Root : Functions.isEmpty() ? -1 : 0);
  select(Prototypes->currentIndex());
  updateActions();
}
void ScriptRecoveryDialog::select(int Index) {
  Source->clear();
  Mappings->clear();
  auto Row = Report["functions"].toArray().at(Index).toObject();
  auto E = Report["evidence"].toObject();
  QJsonObject Details = Report;
  Details.remove("functions");
  auto Metadata = Row;
  Metadata.remove("source");
  Metadata.remove("instructions");
  Metadata.remove("source_base64");
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
bool ScriptRecoveryDialog::exportFile(const QString &File, bool Manifest) {
  if (Report.isEmpty() || (!Manifest && Prototypes->currentIndex() < 0))
    return false;
  auto Row =
      Report["functions"].toArray().at(Prototypes->currentIndex()).toObject();
  if (!Manifest && !Row["source"].isString() &&
      !Row["source_base64"].isString())
    return false;
  auto Bytes =
      Manifest ? QJsonDocument(Report).toJson(QJsonDocument::Compact) + '\n'
      : Row["source_base64"].isString()
          ? QByteArray::fromBase64(Row["source_base64"].toString().toLatin1())
          : Row["source"].toString().toUtf8();
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
