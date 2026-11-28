//===- ModelScanDialog.cpp - Browse model findings and storage evidence ---===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Browse model findings and storage evidence.
//===----------------------------------------------------------------------===//

#include "ModelScanDialog.h"

#include "neverd/model/Limits.h"

#include <QDialogButtonBox>
#include <QDir>
#include <QFile>
#include <QFileDialog>
#include <QFileInfo>
#include <QFormLayout>
#include <QGraphicsItem>
#include <QGraphicsScene>
#include <QGraphicsView>
#include <QHeaderView>
#include <QJsonArray>
#include <QJsonDocument>
#include <QLabel>
#include <QLineEdit>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QSpinBox>
#include <QSplitter>
#include <QTreeWidget>
#include <QVBoxLayout>
#include <limits>

namespace neverd::gui {
namespace {
constexpr int PageSize = model::limits::kPage;
QString label(const QJsonObject &Row) {
  for (auto Key : {"rule", "reason", "name", "tensor", "component", "id",
                   "source", "key", "op_type", "field", "target", "value"})
    if (Row[Key].isString())
      return Row[Key].toString();
  if (Row.contains("layer"))
    return QString::number(Row["layer"].toInteger());
  return {};
}
QTreeWidgetItem *evidenceRow(QTreeWidgetItem *Group, const QJsonObject &Obj) {
  auto *Item = new QTreeWidgetItem(
      Group,
      {label(Obj), Obj["severity"].toString(Obj["type"].toString(
                       Obj["format"].toString(Obj["status"].toString())))});
  Item->setData(0, Qt::UserRole, Obj);
  return Item;
}
} // namespace
ModelScanDialog::ModelScanDialog(const QString &Worker, QWidget *Parent)
    : QDialog(Parent) {
  setWindowTitle(tr("Model Security Scan"));
  resize(fontMetrics().averageCharWidth() * 100, fontMetrics().height() * 35);
  auto *L = new QVBoxLayout(this);
  auto *Form = new QFormLayout;
  L->addLayout(Form);
  auto inputRow = [&](const QString &Title) {
    auto *Line = new QLineEdit(this);
    auto *Row = new QHBoxLayout;
    Row->addWidget(Line, 1);
    for (bool Directory : {false, true}) {
      auto *Browse =
          new QPushButton(Directory ? tr("Folder…") : tr("File…"), this);
      Browse->setAutoDefault(false);
      Row->addWidget(Browse);
      connect(Browse, &QPushButton::clicked, this, [this, Line, Directory] {
        auto P =
            Directory
                ? QFileDialog::getExistingDirectory(this,
                                                    tr("Select model package"))
                : QFileDialog::getOpenFileName(this, tr("Select model file"));
        if (!P.isEmpty())
          Line->setText(P);
      });
    }
    auto *Label = new QLabel(Title, this);
    Label->setBuddy(Line);
    Form->addRow(Label, Row);
    return Line;
  };
  Input = inputRow(tr("&Model"));
  Input->setObjectName("modelPath");
  Baseline = inputRow(tr("&Baseline (optional)"));
  OptionsJSON = new QLineEdit(this);
  OptionsJSON->setObjectName("modelOptionsJSON");
  Form->addRow(tr("Advanced options (JSON)"), OptionsJSON);
  Search = new QLineEdit(this);
  Search->setObjectName("modelSearch");
  Scope = new QLineEdit(this);
  Scope->setObjectName("modelGraphScope");
  Form->addRow(tr("Search tensors or operators"), Search);
  Form->addRow(tr("Graph scope"), Scope);
  ScaleKiB = new QSpinBox(this);
  ScaleKiB->setObjectName("scaleSampleKiB");
  ScaleKiB->setRange(0, std::numeric_limits<int>::max());
  ScaleKiB->setSpecialValueText(tr("Off"));
  Form->addRow(tr("&Quantization sample (KiB)"), ScaleKiB);
  auto *Controls = new QHBoxLayout;
  L->addLayout(Controls);
  Run = new QPushButton(tr("Scan"), this);
  Run->setObjectName("runModelScan");
  Run->setDefault(true);
  Cancel = new QPushButton(tr("Cancel scan"), this);
  Controls->addWidget(Run);
  Controls->addWidget(Cancel);
  Rebuild = new QPushButton(tr("Rebuild GGUF…"), this);
  Rebuild->setAutoDefault(false);
  Controls->addWidget(Rebuild);
  connect(Rebuild, &QPushButton::clicked, this, [this] {
    auto Options =
        QJsonDocument::fromJson(OptionsJSON->text().toUtf8()).object();
    if (!Options.contains("template_policy")) {
      Status->setText(tr("Set template_policy to keep, remove or replace in "
                         "Advanced options."));
      return;
    }
    auto Dest = QFileDialog::getSaveFileName(
        this, tr("Rebuild GGUF…"), {}, QStringLiteral("GGUF (*.gguf)"), nullptr,
        QFileDialog::DontConfirmOverwrite);
    if (Dest.isEmpty())
      return;
    Active =
        Client.request("model_rebuild_gguf", {{"path", Input->text()},
                                              {"output", Dest},
                                              {"options", Options},
                                              {"serialized_report", true}});
    Status->setText(tr("Reading model artifacts…"));
    updateActions();
  });
  Controls->addStretch();
  Status = new QLabel(tr("Starting analysis worker…"), this);
  Status->setTextFormat(Qt::PlainText);
  Status->setWordWrap(true);
  L->addWidget(Status);
  auto *Split = new QSplitter(Qt::Horizontal, this);
  Evidence = new QTreeWidget(Split);
  Evidence->setObjectName("modelEvidence");
  Evidence->setHeaderLabels({tr("Evidence"), tr("Status or type")});
  Evidence->header()->setSectionResizeMode(0, QHeaderView::Stretch);
  auto *Right = new QSplitter(Qt::Vertical, Split);
  GraphView = new QGraphicsView(Right);
  GraphView->setScene(new QGraphicsScene(GraphView));
  GraphView->setObjectName("modelOperatorGraph");
  GraphView->setAccessibleName(tr("Exported graphs"));
  GraphView->hide();
  Detail = new QPlainTextEdit(Right);
  Detail->setReadOnly(true);
  Detail->setAccessibleName(tr("Selected evidence"));
  L->addWidget(Split, 1);
  auto *Footer = new QDialogButtonBox(QDialogButtonBox::Close, this);
  L->addWidget(Footer);
  Previous =
      Footer->addButton(tr("Previous page"), QDialogButtonBox::ActionRole);
  Next = Footer->addButton(tr("Next page"), QDialogButtonBox::ActionRole);
  Save = Footer->addButton(tr("Export report…"), QDialogButtonBox::ActionRole);
  connect(Footer, &QDialogButtonBox::rejected, this, &QDialog::reject);
  connect(Run, &QPushButton::clicked, this, [this] {
    Offset = 0;
    start();
  });
  connect(Previous, &QPushButton::clicked, this, [this] {
    Offset -= PageSize;
    start();
  });
  connect(Next, &QPushButton::clicked, this, [this] {
    Offset += PageSize;
    start();
  });
  connect(Cancel, &QPushButton::clicked, this,
          [this] { Client.request("cancel", {{"request_id", Active}}); });
  auto invalidate = [this] {
    if (!Active.isEmpty()) {
      Client.request("cancel", {{"request_id", Active}});
      Active.clear();
    }
    Report = {};
    SerializedReport.clear();
    GraphView->hide();
    Offset = 0;
    Evidence->clear();
    Detail->clear();
    Status->clear();
    updateActions();
  };
  for (auto *Line : {Input, Baseline, OptionsJSON, Search, Scope})
    connect(Line, &QLineEdit::textChanged, this, invalidate);
  connect(ScaleKiB, &QSpinBox::valueChanged, this, invalidate);
  connect(
      Evidence, &QTreeWidget::currentItemChanged, this,
      [this](QTreeWidgetItem *Item) {
        auto Obj =
            Item ? Item->data(0, Qt::UserRole).toJsonObject() : QJsonObject{};
        Detail->setPlainText(
            Obj.isEmpty() ? QString{}
                          : QString::fromUtf8(QJsonDocument(Obj).toJson()));
        auto *Graph = Item;
        while (
            Graph &&
            !Graph->data(0, Qt::UserRole).toJsonObject().contains("node_count"))
          Graph = Graph->parent();
        showGraph(Graph ? Graph->data(0, Qt::UserRole).toJsonObject()
                        : QJsonObject{});
      });
  connect(Evidence, &QTreeWidget::itemDoubleClicked, this,
          [this](QTreeWidgetItem *Item) {
            const auto Row = Item->data(0, Qt::UserRole).toJsonObject();
            if (!Row.value("native_analysis_available").toBool())
              return;
            const QFileInfo Selected(Input->text());
            QDir Root(Selected.isDir() ? Selected.canonicalFilePath()
                                       : Selected.absolutePath());
            const auto File =
                QFileInfo(
                    Root.filePath(Item->data(0, Qt::UserRole + 1).toString() +
                                  Row["id"].toString()))
                    .canonicalFilePath();
            const auto Relative = Root.relativeFilePath(File);
            if (!File.isEmpty() && !QDir::isAbsolutePath(Relative) &&
                Relative != ".." && !Relative.startsWith("../"))
              emit nativeComponentRequested(File);
          });
  connect(Evidence, &QTreeWidget::itemExpanded, this,
          &ModelScanDialog::expandGraph);
  connect(GraphView->scene(), &QGraphicsScene::selectionChanged, this, [this] {
    const auto Items = GraphView->scene()->selectedItems();
    if (!Items.empty())
      Detail->setPlainText(QString::fromUtf8(
          QJsonDocument(Items.front()->data(0).toJsonObject()).toJson()));
  });
  connect(Save, &QPushButton::clicked, this, [this] {
    auto Name = QFileDialog::getSaveFileName(
        this, tr("Save report to a new file"), {}, tr("JSON (*.json)"), nullptr,
        QFileDialog::DontConfirmOverwrite);
    if (Name.isEmpty())
      return;
    QFile F(Name);
    auto Data = SerializedReport + '\n';
    if (!F.open(QIODevice::WriteOnly | QIODevice::NewOnly)) {
      Status->setText(F.errorString());
      return;
    }
    if (F.write(Data) != Data.size() || !F.flush()) {
      Status->setText(F.errorString());
      F.remove();
    }
  });
  connect(&Client, &EngineClient::message, this,
          [this](const QJsonObject &M) { receive(M); });
  connect(&Client, &EngineClient::failure, this, [this](const QString &E) {
    Ready = false;
    Active.clear();
    Status->setText(E);
    updateActions();
  });
  connect(&Client, &EngineClient::stopped, this, [this] {
    Ready = false;
    Active.clear();
    Status->setText(tr("Analysis worker stopped."));
    updateActions();
  });
  updateActions();
  Client.start(Worker);
}
void ModelScanDialog::start() {
  if (!Ready || !Active.isEmpty() || Input->text().isEmpty())
    return;
  QJsonObject O{{"offset", Offset}, {"limit", PageSize}};
  if (!Search->text().isEmpty())
    O["search"] = Search->text();
  if (!Scope->text().isEmpty())
    O["graph_scope"] = Scope->text();
  if (!OptionsJSON->text().isEmpty())
    O["options_json"] = OptionsJSON->text();
  if (ScaleKiB->value())
    O["scale_bytes"] = qint64(ScaleKiB->value()) * 1024;
  if (!Baseline->text().isEmpty())
    O["baseline"] = Baseline->text();
  Active = Client.request(
      "model_scan",
      {{"path", Input->text()}, {"options", O}, {"serialized_report", true}});
  Status->setText(tr("Reading model artifacts…"));
  updateActions();
}
void ModelScanDialog::receive(const QJsonObject &M) {
  if (M["type"] == "hello") {
    CanRebuild = M["capabilities"].toArray().contains("model_rebuild_gguf");
    Ready = M["protocol_major"] == 1 &&
            M["capabilities"].toArray().contains("model_scan");
    Status->setText(Ready
                        ? QString()
                        : tr("Model scanning is unavailable in this worker."));
    updateActions();
    return;
  }
  if (Active.isEmpty() || M["request_id"].toString() != Active ||
      M["status"] == "progress")
    return;
  Active.clear();
  Report = {};
  if (M["status"] == "ok") {
    SerializedReport =
        M["payload"].toObject()["report_json"].toString().toUtf8();
    Report = QJsonDocument::fromJson(SerializedReport).object();
    Status->setText(Report["error"].toString(Report["status"].toString()));
  } else
    Status->setText(M["status"] == "cancelled"
                        ? tr("Cancelled")
                        : M["error"].toObject()["message"].toString());
  showReport();
  updateActions();
}
void ModelScanDialog::showReport() {
  Evidence->clear();
  Detail->clear();
  auto group = [&](QTreeWidgetItem *Parent, const QString &Title,
                   const QJsonArray &Rows) {
    if (Rows.isEmpty())
      return;
    auto *G =
        new QTreeWidgetItem(Parent, {Title, QString::number(Rows.size())});
    for (auto V : Rows) {
      auto *Row = evidenceRow(G, V.toObject());
      Row->setData(0, Qt::UserRole + 1, Parent->data(0, Qt::UserRole + 1));
    }
    G->setExpanded(true);
  };
  if (Report["schema"] == "neverd.gguf-rebuild.v1") {
    auto *Root = new QTreeWidgetItem(
        Evidence, {tr("Rebuild GGUF…"), Report["status"].toString()});
    Root->setData(0, Qt::UserRole, Report);
    group(Root, tr("Tensors"), Report["tensors"].toArray());
    group(Root, tr("Metadata"), Report["changed_fields"].toArray());
    Root->setExpanded(true);
  }
  for (auto Value : Report["packages"].toArray()) {
    auto P = Value.toObject();
    auto *Root = new QTreeWidgetItem(
        Evidence, {P["id"].toString(),
                   tr("%1 tensors").arg(P["tensor_count"].toInteger())});
    Root->setExpanded(true);
    Root->setData(0, Qt::UserRole + 1, P["relative_root"].toString());
    group(Root, tr("Dependencies"), P["dependencies"].toArray());
    group(Root, tr("Provenance"), P["provenance"].toArray());
    group(Root, tr("Adapter matrices"), P["adapter_pairs"].toArray());
    group(Root, tr("Findings"), P["findings"].toArray());
    group(Root, tr("Coverage gaps"), P["coverage_gaps"].toArray());
    group(Root, tr("Artifacts"), P["artifacts"].toArray());
    group(Root, tr("Templates"), P["templates"].toArray());
    auto Tensors = P["tensors"].toArray();
    if (!Tensors.isEmpty()) {
      auto *Tree = new QTreeWidgetItem(Root, {tr("Tensors")});
      QMap<QString, QTreeWidgetItem *> Members, Layers;
      for (const auto &Value : Tensors) {
        const auto T = Value.toObject();
        const auto Member = T["artifact"].toString();
        auto *&Shard = Members[Member];
        if (!Shard)
          Shard = new QTreeWidgetItem(Tree, {Member});
        auto *Parent = Shard;
        if (T.contains("named_layer")) {
          const auto Layer = QString::number(T["named_layer"].toInteger());
          auto *&Group = Layers[Member + QChar::Null + Layer];
          if (!Group)
            Group = new QTreeWidgetItem(Shard, {Layer});
          Parent = Group;
        }
        evidenceRow(Parent, T);
      }
      Tree->setExpanded(true);
    }
    group(Root, tr("Declarations"), P["declarations"].toArray());
    group(Root, tr("Inferred projection layouts"),
          P["inferred_projection_layouts"].toArray());
    group(Root, tr("Metadata"), P["metadata"].toArray());
    auto Graphs = P["graphs"].toArray();
    if (!Graphs.isEmpty()) {
      auto *G = new QTreeWidgetItem(
          Root, {tr("Exported graphs"), QString::number(Graphs.size())});
      for (auto Value : Graphs) {
        auto Graph = Value.toObject();
        auto *GraphRow = evidenceRow(G, Graph);
        if (!Graph["nodes"].toArray().empty())
          GraphRow->setChildIndicatorPolicy(QTreeWidgetItem::ShowIndicator);
      }
      G->setExpanded(true);
    }
    auto Layers = P["named_layer_tensor_counts"].toObject();
    if (!Layers.isEmpty()) {
      auto *G = new QTreeWidgetItem(Root, {tr("Layers inferred from names")});
      for (auto It = Layers.begin(); It != Layers.end(); ++It)
        new QTreeWidgetItem(
            G, {It.key(), tr("%1 tensors").arg(It.value().toInteger())});
    }
  }
  if (Report.contains("comparison")) {
    auto C = Report["comparison"].toObject();
    auto *G = new QTreeWidgetItem(Evidence, {tr("Baseline comparison")});
    G->setData(0, Qt::UserRole, C);
    G->setExpanded(true);
    group(G, tr("Tensor comparison"), C["tensor_rows"].toArray());
    group(G, tr("Component changes"), C["component_changes"].toArray());
    group(G, tr("Layers inferred from names"), C["numeric_layers"].toArray());
  }
}
void ModelScanDialog::expandGraph(QTreeWidgetItem *Item) {
  if (Item->childCount())
    return;
  for (auto Value :
       Item->data(0, Qt::UserRole).toJsonObject()["nodes"].toArray()) {
    const auto N = Value.toObject();
    auto *Child = evidenceRow(Item, N);
    Child->setText(1, N["op_type"].toString());
    for (auto Edge : N["input_edges"].toArray()) {
      auto E = Edge.toObject();
      auto *Port = evidenceRow(Child, E);
      Port->setText(0, E["value"].toString());
      Port->setText(1, E["source"].toString());
    }
  }
}

void ModelScanDialog::showGraph(const QJsonObject &Graph) {
  auto *Scene = GraphView->scene();
  Scene->clear();
  GraphView->setVisible(!Graph.isEmpty());
  const auto Width = fontMetrics().averageCharWidth() * 22;
  const auto Height = fontMetrics().lineSpacing() * 2;
  QHash<QString, QPointF> Positions;
  QHash<QString, int> Columns;
  QMap<int, int> Rows;
  const auto Nodes = Graph["nodes"].toArray();
  for (auto Value : Nodes) {
    auto N = Value.toObject();
    int Column = 0;
    for (auto Value : N["input_edges"].toArray()) {
      auto Parent = Value.toObject()["node"].toString();
      if (Columns.contains(Parent))
        Column = std::max(Column, Columns[Parent] + 1);
    }
    auto ID = N["id"].toString();
    QPointF At(Column * (Width + Height), Rows[Column]++ * Height * 2);
    Columns.insert(ID, Column);
    Positions.insert(ID, At);
    auto *Box = Scene->addRect(0, 0, Width, Height,
                               QPen(palette().color(QPalette::Text)),
                               palette().brush(QPalette::Base));
    Box->setPos(At);
    Box->setFlag(QGraphicsItem::ItemIsSelectable);
    Box->setData(0, N);
    Box->setToolTip(ID.toHtmlEscaped());
    auto *Text = new QGraphicsSimpleTextItem(
        fontMetrics().elidedText(N["op_type"].toString(), Qt::ElideRight,
                                 Width - 8),
        Box);
    Text->setBrush(palette().brush(QPalette::Text));
    Text->setPos(4, (Height - Text->boundingRect().height()) / 2);
  }
  for (auto Value : Nodes) {
    auto N = Value.toObject();
    auto To = Positions.value(N["id"].toString()) + QPointF(0, Height / 2);
    for (auto Value : N["input_edges"].toArray()) {
      auto E = Value.toObject();
      auto From = Positions.constFind(E["node"].toString());
      if (From == Positions.cend())
        continue;
      auto *Line =
          Scene->addLine(QLineF(*From + QPointF(Width, Height / 2), To),
                         QPen(palette().color(QPalette::Text)));
      Line->setZValue(-1);
      Line->setData(0, E);
      Line->setFlag(QGraphicsItem::ItemIsSelectable);
    }
  }
  Scene->setSceneRect(Scene->itemsBoundingRect());
}
void ModelScanDialog::updateActions() {
  bool Idle = Active.isEmpty();
  Run->setEnabled(Ready && Idle && !Input->text().isEmpty());
  Rebuild->setEnabled(CanRebuild && Ready && Idle &&
                      QFileInfo(Input->text()).isFile());
  Cancel->setEnabled(!Idle);
  Save->setEnabled(Idle && !Report.isEmpty());
  qint64 Total = Report["comparison"].toObject()["total_rows"].toInteger();
  for (auto V : Report["packages"].toArray()) {
    auto P = V.toObject();
    Total = std::max({Total, P["matching_tensor_count"].toInteger(),
                      P["metadata_count"].toInteger(),
                      P["adapter_pair_count"].toInteger(),
                      P["dependency_count"].toInteger()});
    for (auto G : P["graphs"].toArray())
      Total = std::max(Total, G.toObject()["matching_node_count"].toInteger());
  }
  Previous->setEnabled(Ready && Idle && Offset > 0);
  Next->setEnabled(Ready && Idle && Offset + PageSize < Total);
}
} // namespace neverd::gui
