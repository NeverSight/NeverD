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
#include <QFile>
#include <QFileDialog>
#include <QFormLayout>
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
                   "source", "key", "op_type"})
    if (Row[Key].isString())
      return Row[Key].toString();
  return {};
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
  Detail = new QPlainTextEdit(Split);
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
    Offset = 0;
    Evidence->clear();
    Detail->clear();
    Status->clear();
    updateActions();
  };
  for (auto *Line : {Input, Baseline})
    connect(Line, &QLineEdit::textChanged, this, invalidate);
  connect(ScaleKiB, &QSpinBox::valueChanged, this, invalidate);
  connect(Evidence, &QTreeWidget::currentItemChanged, this,
          [this](QTreeWidgetItem *Item) {
            Detail->setPlainText(Item ? Item->data(0, Qt::UserRole).toString()
                                      : QString{});
          });
  connect(Save, &QPushButton::clicked, this, [this] {
    auto Name = QFileDialog::getSaveFileName(
        this, tr("Save report to a new file"), {}, tr("JSON (*.json)"), nullptr,
        QFileDialog::DontConfirmOverwrite);
    if (Name.isEmpty())
      return;
    QFile F(Name);
    auto Data = QJsonDocument(Report).toJson();
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
  if (ScaleKiB->value())
    O["scale_bytes"] = qint64(ScaleKiB->value()) * 1024;
  if (!Baseline->text().isEmpty())
    O["baseline"] = Baseline->text();
  Active =
      Client.request("model_scan", {{"path", Input->text()}, {"options", O}});
  Status->setText(tr("Reading model artifacts…"));
  updateActions();
}
void ModelScanDialog::receive(const QJsonObject &M) {
  if (M["type"] == "hello") {
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
    Report = M["payload"].toObject();
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
  auto row = [](QTreeWidgetItem *Group, const QJsonObject &Obj) {
    auto *Item = new QTreeWidgetItem(
        Group,
        {label(Obj), Obj["severity"].toString(Obj["type"].toString(
                         Obj["format"].toString(Obj["status"].toString())))});
    Item->setData(0, Qt::UserRole,
                  QString::fromUtf8(QJsonDocument(Obj).toJson()));
    return Item;
  };
  auto group = [&](QTreeWidgetItem *Parent, const QString &Title,
                   const QJsonArray &Rows) {
    if (Rows.isEmpty())
      return;
    auto *G =
        new QTreeWidgetItem(Parent, {Title, QString::number(Rows.size())});
    for (auto V : Rows)
      row(G, V.toObject());
    G->setExpanded(true);
  };
  for (auto Value : Report["packages"].toArray()) {
    auto P = Value.toObject();
    auto *Root = new QTreeWidgetItem(
        Evidence, {P["id"].toString(),
                   tr("%1 tensors").arg(P["tensor_count"].toInteger())});
    Root->setExpanded(true);
    group(Root, tr("Findings"), P["findings"].toArray());
    group(Root, tr("Coverage gaps"), P["coverage_gaps"].toArray());
    group(Root, tr("Artifacts"), P["artifacts"].toArray());
    group(Root, tr("Templates"), P["templates"].toArray());
    group(Root, tr("Tensors"), P["tensors"].toArray());
    group(Root, tr("Inferred projection layouts"),
          P["inferred_projection_layouts"].toArray());
    group(Root, tr("Metadata"), P["metadata"].toArray());
    auto Graphs = P["graphs"].toArray();
    if (!Graphs.isEmpty()) {
      auto *G = new QTreeWidgetItem(
          Root, {tr("Exported graphs"), QString::number(Graphs.size())});
      for (auto Value : Graphs) {
        auto Graph = Value.toObject();
        auto *GraphRow = row(G, Graph);
        for (auto V : Graph["nodes"].toArray()) {
          auto Node = V.toObject();
          auto *Child = row(GraphRow, Node);
          Child->setText(1, Node["op_type"].toString());
        }
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
    G->setData(0, Qt::UserRole, QString::fromUtf8(QJsonDocument(C).toJson()));
    G->setExpanded(true);
    group(G, tr("Tensor comparison"), C["tensor_rows"].toArray());
    group(G, tr("Component changes"), C["component_changes"].toArray());
  }
}
void ModelScanDialog::updateActions() {
  bool Idle = Active.isEmpty();
  Run->setEnabled(Ready && Idle && !Input->text().isEmpty());
  Cancel->setEnabled(!Idle);
  Save->setEnabled(Idle && !Report.isEmpty());
  qint64 Total = Report["comparison"].toObject()["total_rows"].toInteger();
  for (auto V : Report["packages"].toArray()) {
    auto P = V.toObject();
    Total = std::max({Total, P["tensor_count"].toInteger(),
                      P["metadata_count"].toInteger()});
    for (auto G : P["graphs"].toArray())
      Total = std::max(Total, G.toObject()["node_count"].toInteger());
  }
  Previous->setEnabled(Ready && Idle && Offset > 0);
  Next->setEnabled(Ready && Idle && Offset + PageSize < Total);
}
} // namespace neverd::gui
