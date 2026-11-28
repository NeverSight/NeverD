//===- JVMRecoveryDialog.cpp - JVM artifact views ------------------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Browse the backend's physical inventory and navigate original bytecode
/// and generated Java using its bidirectional source map.
//===----------------------------------------------------------------------===//

#include "JVMRecoveryDialog.h"

#include "../Config.h"

#include "neverd/analysis/JVMVersions.h"

#include <QApplication>
#include <QCache>
#include <QCheckBox>
#include <QClipboard>
#include <QComboBox>
#include <QFile>
#include <QFileDialog>
#include <QFont>
#include <QFontDatabase>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QJsonDocument>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QPlainTextDocumentLayout>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QScrollBar>
#include <QSignalBlocker>
#include <QSplitter>
#include <QStringTokenizer>
#include <QStringView>
#include <QTabWidget>
#include <QTextBlock>
#include <QTextCursor>
#include <QTextDocument>
#include <QVBoxLayout>
#include <algorithm>

using namespace neverd::gui;
namespace {
QString json(QJsonValue V) {
  if (V.isObject())
    return QString::fromUtf8(
        QJsonDocument(V.toObject()).toJson(QJsonDocument::Indented));
  if (V.isArray())
    return QString::fromUtf8(
        QJsonDocument(V.toArray()).toJson(QJsonDocument::Indented));
  return {};
}
QString diagnostics(QJsonObject R) {
  QStringList L;
  for (auto V : R["diagnostics"].toArray()) {
    const auto D = V.toObject();
    auto Code = D["code"].toString();
    if (D.contains("offset"))
      Code += QStringLiteral(" @0x%1").arg(D["offset"].toInteger(), 0, 16);
    L << Code + QStringLiteral(" · ") + D["message"].toString();
  }
  return L.join(QStringLiteral("\n"));
}
QFont literalFont(QFont Font) {
  Font.setStyleStrategy(static_cast<QFont::StyleStrategy>(
      Font.styleStrategy() | QFont::PreferNoShaping));
  return Font;
}
// Measuring and painting reuse the first screen's glyphs. Keep the cache
// proportional to the viewport even when a single JSON string is very long.
void cacheFirstScreen(QPlainTextEdit &View) {
  const auto Metrics = View.fontMetrics();
  const int Lines = View.viewport()->height() / Metrics.lineSpacing() + 1;
  const int Columns =
      View.viewport()->width() / qMax(1, Metrics.averageCharWidth()) + 1;
  auto Block = View.document()->begin();
  for (int I = 0; I < Lines && Block.isValid(); ++I, Block = Block.next())
    if (Block.length() <= Columns)
      Block.layout()->setCacheEnabled(true);
}
// An ASCII JSON line occupies one document block. Replace the text within
// changed blocks so the matching rows keep their native layouts.
void updateJSONLines(QTextDocument &Document, const QString &Text,
                     QStringView SavedText) {
  QTextCursor Edit(&Document);
  Edit.beginEditBlock();
  auto Block = Document.begin();
  qsizetype Offset = 0;
  for (QStringView Line : QStringView(Text).tokenize(u'\n')) {
    const int Length = Block.length() - 1;
    const bool Matches = SavedText.isNull()
                             ? Line == Block.text()
                             : Line == SavedText.sliced(Offset, Length);
    if (!Matches) {
      Edit.setPosition(Block.position());
      Edit.setPosition(Block.position() + Length, QTextCursor::KeepAnchor);
      Edit.insertText(Line.toString());
    }
    Offset += Length + 1;
    Block = Block.next();
  }
  Edit.endEditBlock();
  Document.setModified(false);
}

using DocumentCache = QCache<int, QTextDocument>;

void setDocumentFont(QTextDocument &Document, const QFont &Font) {
  if (Document.defaultFont() != Font)
    Document.setDefaultFont(Font);
}

// The block count selects a document with reusable paragraphs. The incoming
// report still supplies every line, including when a different method fits.
bool updateJSONDocument(QPlainTextEdit &View, DocumentCache &Inactive,
                        const QString &Text, const QFont &Font) {
  auto *Current = View.document();
  if (Current->isEmpty() || Text.isEmpty())
    return false;
  const int Blocks = int(Text.count(u'\n')) + 1;
  auto *Next = Current;
  if (Blocks != Current->blockCount()) {
    if (Text.size() + 1 > Inactive.maxCost())
      return false;
    Next = Inactive.take(Blocks);
    if (!Next) {
      if (Current->characterCount() > Inactive.maxCost())
        return false;
      Next = new QTextDocument(&View);
      Next->setDocumentLayout(new QPlainTextDocumentLayout(Next));
      Next->setDocumentMargin(Current->documentMargin());
      Next->setDefaultTextOption(Current->defaultTextOption());
      Next->setUndoRedoEnabled(false);
    }
  }
  setDocumentFont(*Next, Font);
  if (Next->isEmpty()) {
    Next->setPlainText(Text);
    Next->setModified(false);
  } else {
    // A detached document fits the per-pane budget. Read its underlying text
    // once before editing. The active document can be arbitrarily larger.
    const QString SavedText = Next == Current ? QString{} : Next->toRawText();
    updateJSONLines(*Next, Text, SavedText);
  }
  if (Next != Current) {
    // The editor owns the installed document and the cache owns detached ones.
    Current->setParent(nullptr);
    Next->setParent(&View);
    View.setDocument(Next);
    Inactive.insert(Current->blockCount(), Current, Current->characterCount());
  }
  View.setTextCursor(QTextCursor(Next));
  View.horizontalScrollBar()->setValue(View.horizontalScrollBar()->minimum());
  View.verticalScrollBar()->setValue(View.verticalScrollBar()->minimum());
  return true;
}

void setCodeText(QPlainTextEdit &View, const QString &Text,
                 DocumentCache *Inactive = nullptr) {
  const bool ASCII = std::all_of(Text.cbegin(), Text.cend(),
                                 [](QChar C) { return C.unicode() < 0x80; });
  const QFont Font = ASCII ? literalFont(View.font()) : View.font();
  if (!Inactive || !ASCII || !updateJSONDocument(View, *Inactive, Text, Font)) {
    setDocumentFont(*View.document(), Font);
    View.setPlainText(Text);
  }
  cacheFirstScreen(View);
}
} // namespace

class JVMRecoveryDialog::DetailView final : public QPlainTextEdit {
public:
  explicit DetailView(JVMRecoveryDialog &Dialog) : Dialog(Dialog) {
    setUndoRedoEnabled(false);
  }

  void clearReport() {
    Inactive.clear();
    clear();
  }
  void invalidate() { Dirty = true; }
  void prepare() {
    if (!Dirty)
      return;
    Dirty = false;
    setCodeText(*this, Dialog.detailText(this), &Inactive);
  }
  void setVisible(bool Visible) override {
    // Showing a plain-text editor lays out its document to size the scrollbars.
    // Install the selected method's text before Qt measures the page.
    if (Visible)
      prepare();
    QPlainTextEdit::setVisible(Visible);
  }

private:
  DocumentCache Inactive{
      neverd::limits::gui_config::kJVMDetailInactiveCharacters};
  JVMRecoveryDialog &Dialog;
  bool Dirty = false;
};

JVMRecoveryDialog::JVMRecoveryDialog(const QString &Worker, QWidget *Parent)
    : QDialog(Parent) {
  setAttribute(Qt::WA_DeleteOnClose);
  setWindowTitle(tr("Inspect JVM classes and JARs"));
  auto *Layout = new QVBoxLayout(this);
  auto *Form = new QFormLayout;
  auto *Files = new QHBoxLayout;
  Path = new QLineEdit;
  Path->setObjectName("jvmPath");
  Browse = new QPushButton(tr("Browse…"));
  Files->addWidget(Path);
  Files->addWidget(Browse);
  Form->addRow(tr("Artifact"), Files);
  auto *Profile = new QHBoxLayout;
  Release = new QComboBox;
  Release->setObjectName("jvmRelease");
  Release->addItem(tr("Base JAR entries"), 0);
  for (unsigned R = 1; R <= neverd::jvm::LastRelease; ++R)
    Release->addItem(
        (R <= 4 ? QStringLiteral("Java 1.%1") : QStringLiteral("Java %1"))
            .arg(R),
        int(R));
  Preview = new QCheckBox(tr("Enable preview"));
  Profile->addWidget(Release);
  Profile->addWidget(Preview);
  Profile->addStretch();
  Form->addRow(tr("Runtime"), Profile);
  auto *DepRow = new QHBoxLayout;
  Deps = new QPushButton(tr("Local dependencies…"));
  Clear = new QPushButton(tr("Clear"));
  auto *DepNames = new QLabel;
  DepNames->setTextFormat(Qt::PlainText);
  DepNames->setWordWrap(true);
  DepRow->addWidget(Deps);
  DepRow->addWidget(Clear);
  DepRow->addWidget(DepNames, 1);
  Form->addRow(tr("Dependencies"), DepRow);
  Classes = new QComboBox;
  Classes->setObjectName("jvmClasses");
  Methods = new QComboBox;
  Methods->setObjectName("jvmMethods");
  Form->addRow(tr("Class or member"), Classes);
  Form->addRow(tr("Method"), Methods);
  Layout->addLayout(Form);
  auto *Controls = new QHBoxLayout;
  Inspect = new QPushButton(tr("Inspect inventory"));
  Inspect->setObjectName("jvmInspect");
  Analyze = new QPushButton(tr("Analyze selection"));
  Analyze->setObjectName("jvmAnalyze");
  Cancel = new QPushButton(tr("Cancel"));
  Controls->addWidget(Inspect);
  Controls->addWidget(Analyze);
  Controls->addWidget(Cancel);
  Controls->addStretch();
  Layout->addLayout(Controls);
  Status = new QLabel;
  Status->setObjectName("jvmStatus");
  Status->setTextFormat(Qt::PlainText);
  Status->setWordWrap(true);
  Layout->addWidget(Status);
  Results = new QComboBox;
  Results->setObjectName("jvmResults");
  Layout->addWidget(Results);
  auto *Split = new QSplitter;
  Tabs = new QTabWidget;
  Tabs->setDocumentMode(true);
  auto pane = [&](auto *P, QString Title, QString Name) {
    P->setReadOnly(true);
    P->setObjectName(Name);
    P->setFont(QFontDatabase::systemFont(QFontDatabase::FixedFont));
    Tabs->addTab(P, Title);
    return P;
  };
  Java = pane(new QPlainTextEdit, tr("Java source"), "jvmSource");
  Bytecode = pane(new DetailView(*this), tr("Bytecode"), "jvmBytecode");
  IR = pane(new DetailView(*this), tr("Typed IR"), "jvmIR");
  Evidence = pane(new DetailView(*this), tr("Evidence"), "jvmEvidence");
  Mappings = new QListWidget;
  Mappings->setObjectName("jvmMappings");
  // Each row contains a decimal PC, a hex offset and an ASCII JVM mnemonic.
  Mappings->setFont(literalFont(Mappings->font()));
  Mappings->setAccessibleName(tr("Bytecode mappings"));
  // The splitter asks for its children's minimum sizes on each page switch.
  // Let a layout cache the tab widget's size hints between geometry changes.
  auto *TabPane = new QWidget;
  TabPane->setSizePolicy(Tabs->sizePolicy());
  // The inherited window background lets Qt clip ancestor paints at the pane.
  TabPane->setAutoFillBackground(true);
  auto *TabLayout = new QVBoxLayout(TabPane);
  TabLayout->setContentsMargins(0, 0, 0, 0);
  TabLayout->addWidget(Tabs);
  Split->addWidget(TabPane);
  Split->addWidget(Mappings);
  Split->setStretchFactor(0, 3);
  Split->setStretchFactor(1, 1);
  Layout->addWidget(Split, 1);
  auto *Actions = new QHBoxLayout;
  Copy = new QPushButton(tr("Copy view"));
  Copy->setObjectName("jvmCopy");
  Save = new QPushButton(tr("Export view…"));
  Export = new QPushButton(tr("Export report…"));
  Actions->addStretch();
  for (auto *B : {Copy, Save, Export})
    Actions->addWidget(B);
  Layout->addLayout(Actions);
  connect(Browse, &QPushButton::clicked, this, [this] {
    auto P = QFileDialog::getOpenFileName(this, tr("Open JVM artifact"));
    if (!P.isEmpty())
      Path->setText(P);
  });
  connect(Deps, &QPushButton::clicked, this, [this, DepNames] {
    auto D = QFileDialog::getOpenFileNames(this, tr("Local dependencies…"));
    if (D.isEmpty())
      return;
    Dependencies.append(D);
    DepNames->setText(Dependencies.join(QStringLiteral("\n")));
    resetIndex();
  });
  connect(Clear, &QPushButton::clicked, this, [this, DepNames] {
    Dependencies.clear();
    DepNames->clear();
    resetIndex();
  });
  connect(Path, &QLineEdit::textChanged, this, [this] { resetIndex(); });
  connect(Release, &QComboBox::currentIndexChanged, this, [this] {
    if (Release->currentData().toInt() < 12)
      Preview->setChecked(false);
    resetIndex();
  });
  connect(Preview, &QCheckBox::toggled, this, [this] { resetIndex(); });
  connect(Inspect, &QPushButton::clicked, this, [this] { request(true); });
  connect(Analyze, &QPushButton::clicked, this, [this] { request(false); });
  connect(Cancel, &QPushButton::clicked, this, [this] {
    if (!Active.isEmpty())
      Client.request("cancel", {{"request_id", Active}});
  });
  connect(Classes, &QComboBox::currentIndexChanged, this,
          [this] { methods(); });
  connect(Results, &QComboBox::currentIndexChanged, this, [this] { select(); });
  connect(Tabs, &QTabWidget::currentChanged, this,
          [this] { showCurrentTab(); });
  connect(Mappings, &QListWidget::currentRowChanged, this, [this](int N) {
    if (N < 0)
      return;
    auto L = Mappings->item(N)->data(Qt::UserRole).toList();
    if (L.isEmpty())
      return;
    auto B = Java->document()->findBlockByNumber(L.front().toInt() - 1);
    if (B.isValid()) {
      QSignalBlocker Guard(Java);
      Java->setTextCursor(QTextCursor(B));
      Java->centerCursor();
    }
  });
  connect(Java, &QPlainTextEdit::cursorPositionChanged, this, [this] {
    if (!Java->hasFocus())
      return;
    int Line = Java->textCursor().blockNumber() + 1;
    QSignalBlocker Guard(Mappings);
    Mappings->clearSelection();
    for (int N = 0; N < Mappings->count(); ++N) {
      auto *I = Mappings->item(N);
      if (I->data(Qt::UserRole).toList().contains(Line))
        I->setSelected(true);
    }
  });
  Mappings->setSelectionMode(QAbstractItemView::ExtendedSelection);
  connect(Copy, &QPushButton::clicked, this, [this] {
    QApplication::clipboard()->setText(QString::fromUtf8(selectedText()));
  });
  auto save = [this](bool Full) {
    auto Dest = QFileDialog::getSaveFileName(this, Full ? tr("Export report…")
                                                        : tr("Export view…"));
    if (Dest.isEmpty())
      return;
    exportView(Dest, Full);
  };
  connect(Save, &QPushButton::clicked, this, [save] { save(false); });
  connect(Export, &QPushButton::clicked, this, [save] { save(true); });
  connect(&Client, &EngineClient::message, this,
          [this](const QJsonObject &R) { receive(R); });
  connect(&Client, &EngineClient::failure, this, [this](const QString &Why) {
    Ready = false;
    Active.clear();
    resetIndex();
    Status->setText(Why);
  });
  connect(&Client, &EngineClient::stopped, this, [this] {
    Ready = false;
    Active.clear();
    updateActions();
  });
  connect(this, &QDialog::finished, this, [this] { Client.stop(); });
  resetIndex();
  resize(900, 640);
  Client.start(Worker);
}
void JVMRecoveryDialog::resetIndex() {
  Index = {};
  Report = {};
  for (auto *View : {Bytecode, IR, Evidence})
    View->clearReport();
  Results->clear();
  classes();
  select();
  updateActions();
}
void JVMRecoveryDialog::classes() {
  QSignalBlocker Guard(Classes);
  Classes->clear();
  Classes->addItem(tr("Selected runtime view"), -1);
  for (auto V : Index["components"].toArray()) {
    auto C = V.toObject();
    if (C["kind"] != "class" && C["kind"] != "source")
      continue;
    Classes->addItem(QStringLiteral("%1 · %2")
                         .arg(C["id"].toInt())
                         .arg(C["name"].toString()),
                     C["id"].toInt());
  }
  methods();
}
void JVMRecoveryDialog::methods() {
  Methods->clear();
  Methods->addItem(tr("All methods"), QString());
  for (auto V : Index["components"].toArray()) {
    auto C = V.toObject();
    if (C["id"].toInt() != Classes->currentData().toInt())
      continue;
    for (auto F : C["methods"].toArray()) {
      auto M = F.toObject();
      auto Key = M["name"].toString() + M["descriptor"].toString();
      Methods->addItem(Key, Key);
    }
  }
}
void JVMRecoveryDialog::request(bool Only) {
  if (!Ready || !Active.isEmpty() || Path->text().isEmpty())
    return;
  QJsonObject O{{"release", Release->currentData().toInt()},
                {"preview", Preview->isChecked()},
                {"inventory", Only},
                {"dependencies", QJsonArray::fromStringList(Dependencies)}};
  if (!Only) {
    O["component"] = Classes->currentData().toInt();
    O["method"] = Methods->currentData().toString();
  }
  Inventory = Only;
  Active =
      Client.request("jvm_recover", {{"path", Path->text()}, {"options", O}});
  updateActions();
}
void JVMRecoveryDialog::receive(const QJsonObject &M) {
  if (M["type"] == "hello") {
    Ready = M["protocol_major"] == 1 &&
            M["capabilities"].toArray().contains("jvm_recover");
    updateActions();
    return;
  }
  if (Active.isEmpty() || M["request_id"].toString() != Active ||
      M["status"] == "progress")
    return;
  Active.clear();
  if (M["status"] != "ok") {
    Status->setText(
        M["error"].toObject()["message"].toString(M["status"].toString()));
    updateActions();
    return;
  }
  Report = M["payload"].toObject();
  if (Inventory || Index.isEmpty()) {
    Index = Report;
    classes();
  }
  {
    QSignalBlocker G(Results);
    Results->clear();
    for (auto V : Report["functions"].toArray()) {
      auto F = V.toObject();
      Results->addItem(F["class"].toString() + QStringLiteral(" ") +
                       F["name"].toString() + F["descriptor"].toString());
    }
  }

  select(true);
  updateActions();
}
QJsonObject JVMRecoveryDialog::currentFunction() const {
  return Report["functions"].toArray().at(Results->currentIndex()).toObject();
}
void JVMRecoveryDialog::select(bool ChooseTab) {
  const auto F = currentFunction();
  for (auto *View : {Bytecode, IR, Evidence})
    View->invalidate();
  QSignalBlocker Guard(Java);
  setCodeText(*Java, F["source"].toString());
  const auto Instructions = F["instructions"].toArray();
  Mappings->clear();
  for (auto V : Instructions) {
    auto I = V.toObject();
    auto *Item = new QListWidgetItem(QStringLiteral("%1   0x%2   %3")
                                         .arg(I["pc"].toInt())
                                         .arg(I["offset"].toInteger(), 0, 16)
                                         .arg(I["opcode"].toString()));
    QVariantList Lines;
    for (auto S : I["source_spans"].toArray())
      Lines.push_back(S.toObject()["line"].toInt());
    Item->setData(Qt::UserRole, Lines);
    Mappings->addItem(Item);
  }
  auto Flags = QStringList{Report["status"].toString(), F["status"].toString(),
                           diagnostics(Report), diagnostics(F)};
  Flags.removeAll(QString());
  Flags.removeDuplicates();
  Status->setText(Flags.join(QStringLiteral(" · ")));
  if (ChooseTab) {
    QPlainTextEdit *View = Evidence;
    if (!Inventory && !Java->document()->isEmpty())
      View = Java;
    else if (!Inventory && (F["ir"].isObject() || F["ir"].isArray()))
      View = IR;
    else if (!Inventory && !Instructions.isEmpty())
      View = Bytecode;
    if (Tabs->currentWidget() != View) {
      Tabs->setCurrentWidget(View);
      return;
    }
  }
  showCurrentTab();
}
void JVMRecoveryDialog::showCurrentTab() {
  if (auto *View = Tabs->currentWidget(); View != Java)
    static_cast<DetailView *>(View)->prepare();
  updateViewActions();
}
QString JVMRecoveryDialog::detailText(const DetailView *View) const {
  const auto F = currentFunction();
  if (View == Bytecode) {
    const auto Instructions = F["instructions"].toArray();
    return Instructions.isEmpty() ? QString() : json(Instructions);
  }
  if (View == IR)
    return json(F["ir"]);
  if (Report.isEmpty())
    return {};
  auto Details = F.isEmpty() ? Report : F;
  if (F.isEmpty())
    Details.remove("functions");
  else
    for (auto K : {"source", "instructions", "ir"})
      Details.remove(K);
  return json(Details);
}
bool JVMRecoveryDialog::exportView(const QString &Dest, bool Full) {
  if (Report.isEmpty() || (!Full && !Save->isEnabled()))
    return false;
  auto Data = Full ? QJsonDocument(Report).toJson(QJsonDocument::Compact) + '\n'
                   : selectedText();
  QFile F(Dest);
  if (!F.open(QIODevice::WriteOnly | QIODevice::NewOnly)) {
    Status->setText(F.errorString());
    return false;
  }
  if (F.write(Data) != Data.size() || !F.flush()) {
    auto Why = F.errorString();
    F.remove();
    Status->setText(Why);
    return false;
  }
  return true;
}
QByteArray JVMRecoveryDialog::selectedText() const {
  if (Tabs->currentWidget() == Java)
    return currentFunction()["source"].toString().toUtf8();
  return qobject_cast<QPlainTextEdit *>(Tabs->currentWidget())
      ->toPlainText()
      .toUtf8();
}
void JVMRecoveryDialog::updateActions() {
  bool Idle = Active.isEmpty(), Can = Idle && Ready && !Path->text().isEmpty();
  Inspect->setEnabled(Can);
  Analyze->setEnabled(Can);
  Cancel->setEnabled(!Idle);
  Browse->setEnabled(Idle);
  Deps->setEnabled(Idle);
  Clear->setEnabled(Idle);
  Path->setEnabled(Idle);
  Release->setEnabled(Idle);
  Preview->setEnabled(Idle && Release->currentData().toInt() >= 12);
  Classes->setEnabled(Idle);
  Methods->setEnabled(Idle);
  Export->setEnabled(!Report.isEmpty());
}
void JVMRecoveryDialog::updateViewActions() {
  bool Has = Tabs->currentWidget() == Java
                 ? currentFunction()["source"].isString()
                 : !qobject_cast<QPlainTextEdit *>(Tabs->currentWidget())
                        ->document()
                        ->isEmpty();
  Copy->setEnabled(Has);
  Save->setEnabled(Has);
}
