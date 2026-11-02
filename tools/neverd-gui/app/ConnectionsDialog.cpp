#include "ConnectionsDialog.h"

#include "Icons.h"
#include "ModelessDialog.h"
#include "Session.h"
#include "Theme.h"
#include "mcp/GuiSessionBroker.h"
#include "mcp/McpConnectionManager.h"

#include <QApplication>
#include <QClipboard>
#include <QComboBox>
#include <QDialogButtonBox>
#include <QHBoxLayout>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QSplitter>
#include <QTabBar>
#include <QVBoxLayout>

namespace neverd::gui {
namespace {
enum CatalogTab { ToolsTab, ResourcesTab, HistoryTab };
constexpr int EntryRole = Qt::UserRole;
} // namespace

QString ConnectionsDialog::callStatus(const QString &status) {
  if (status == QLatin1String("pending"))
    return tr("Pending");
  if (status == QLatin1String("completed"))
    return tr("Completed");
  if (status == QLatin1String("failed"))
    return tr("Failed");
  if (status == QLatin1String("cancelled"))
    return tr("Cancelled");
  if (status == QLatin1String("timed_out"))
    return tr("Timed out");
  if (status == QLatin1String("disconnected"))
    return tr("Disconnected");
  return status;
}

ConnectionsDialog::ConnectionsDialog(Session &session,
                                     McpConnectionManager &client,
                                     GuiSessionBroker &broker, QWidget *parent)
    : QDialog(parent), session_(session), client_(client), broker_(broker) {
  setObjectName(QStringLiteral("connectionsDialog"));
  setWindowTitle(tr("MCP connections"));
  setWindowIcon(icon(QStringLiteral("mcp")));
  resize(1000, 700);
  auto *layout = new QVBoxLayout(this);

  // Session sharing.
  auto *shareRow = new QHBoxLayout;
  shareRow->addWidget(new QLabel(tr("This NeverD session"), this), 1);
  sharing_ = new QPushButton(this);
  copyCredential_ = new QPushButton(tr("Copy credential path"), this);
  shareRow->addWidget(sharing_);
  shareRow->addWidget(copyCredential_);
  layout->addLayout(shareRow);
  sharingStatus_ = new QLabel(this);
  sharingStatus_->setWordWrap(true);
  layout->addWidget(sharingStatus_);
  connect(sharing_, &QPushButton::clicked, this, [this] {
    if (broker_.enabled())
      broker_.stop();
    else
      broker_.start();
  });
  connect(copyCredential_, &QPushButton::clicked, this, [this] {
    QApplication::clipboard()->setText(broker_.credentialFile());
  });

  // Server connection.
  auto *serverRow = new QHBoxLayout;
  transport_ = new QComboBox(this);
  transport_->addItems(
      {QStringLiteral("stdio"), QStringLiteral("Streamable HTTP")});
  transport_->setAccessibleName(tr("Transport"));
  endpoint_ = new QLineEdit(this);
  connect_ = new QPushButton(this);
  serverRow->addWidget(transport_);
  serverRow->addWidget(endpoint_, 1);
  serverRow->addWidget(connect_);
  layout->addLayout(serverRow);
  arguments_ = new QLineEdit(QStringLiteral("[]"), this);
  arguments_->setPlaceholderText(
      tr("Arguments as JSON, for example [\"--help\"]"));
  arguments_->setFont(Theme::instance().codeFont());
  layout->addWidget(arguments_);
  auto *httpRow = new QHBoxLayout;
  token_ = new QLineEdit(this);
  token_->setEchoMode(QLineEdit::Password);
  token_->setPlaceholderText(tr("Bearer token (optional)"));
  token_->setAccessibleName(tr("Bearer token"));
  caFile_ = new QLineEdit(this);
  caFile_->setPlaceholderText(tr("CA certificate path (optional)"));
  caFile_->setAccessibleName(tr("CA certificate path"));
  httpRow->addWidget(token_, 1);
  httpRow->addWidget(caFile_, 1);
  layout->addLayout(httpRow);
  clientStatus_ = new QLabel(this);
  clientStatus_->setWordWrap(true);
  layout->addWidget(clientStatus_);
  connect(transport_, &QComboBox::currentIndexChanged, this,
          &ConnectionsDialog::refresh);
  connect(connect_, &QPushButton::clicked, this,
          &ConnectionsDialog::connectOrDisconnect);

  // Catalog and result.
  auto *splitter = new QSplitter(this);
  auto *left = new QWidget(splitter);
  auto *leftLayout = new QVBoxLayout(left);
  leftLayout->setContentsMargins(0, 0, 0, 0);
  tabs_ = new QTabBar(left);
  tabs_->addTab(tr("Tools"));
  tabs_->addTab(tr("Resources"));
  tabs_->addTab(tr("History"));
  catalog_ = new QListWidget(left);
  leftLayout->addWidget(tabs_);
  leftLayout->addWidget(catalog_, 1);
  auto *right = new QWidget(splitter);
  auto *rightLayout = new QVBoxLayout(right);
  rightLayout->setContentsMargins(0, 0, 0, 0);
  auto *titleRow = new QHBoxLayout;
  selectedTitle_ = new QLabel(tr("Result"), right);
  schema_ = new QPushButton(tr("Schema"), right);
  call_ = new QPushButton(tr("Call tool"), right);
  cancelCall_ = new QPushButton(tr("Cancel call"), right);
  titleRow->addWidget(selectedTitle_, 1);
  titleRow->addWidget(schema_);
  titleRow->addWidget(call_);
  titleRow->addWidget(cancelCall_);
  rightLayout->addLayout(titleRow);
  selectedDescription_ = new QLabel(right);
  selectedDescription_->setWordWrap(true);
  rightLayout->addWidget(selectedDescription_);
  parameters_ = new QPlainTextEdit(QStringLiteral("{}"), right);
  parameters_->setFont(Theme::instance().codeFont());
  parameters_->setMaximumHeight(110);
  rightLayout->addWidget(parameters_);
  result_ = new QPlainTextEdit(right);
  result_->setReadOnly(true);
  result_->setFont(Theme::instance().codeFont());
  result_->setPlaceholderText(
      tr("Select a resource or call a tool to inspect its response."));
  rightLayout->addWidget(result_, 1);
  splitter->setStretchFactor(1, 1);
  splitter->setSizes({300, 680});
  layout->addWidget(splitter, 1);
  auto *buttons = new QDialogButtonBox(QDialogButtonBox::Close, this);
  connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
  layout->addWidget(buttons);

  connect(tabs_, &QTabBar::currentChanged, this, [this](int index) {
    selectedCall_.clear();
    selectedTool_.clear();
    if (client_.connected() && index == ToolsTab)
      client_.listTools();
    else if (client_.connected() && index == ResourcesTab)
      client_.listResources();
    refreshCatalog();
    refresh();
  });
  connect(catalog_, &QListWidget::currentItemChanged, this,
          &ConnectionsDialog::selectEntry);
  connect(schema_, &QPushButton::clicked, this, &ConnectionsDialog::showSchema);
  connect(call_, &QPushButton::clicked, this, [this] {
    client_.callTool(selectedTool_.value("name").toString(),
                     parameters_->toPlainText());
  });
  connect(cancelCall_, &QPushButton::clicked, this,
          [this] { client_.cancelCall(selectedCall_); });
  connect(&client_, &McpConnectionManager::changed, this, [this] {
    refreshCatalog();
    refresh();
  });
  connect(&broker_, &GuiSessionBroker::changed, this,
          &ConnectionsDialog::refresh);
  connect(&session_, &Session::stateChanged, this, &ConnectionsDialog::refresh);
  connect(this, &QDialog::finished, token_, &QLineEdit::clear);
  refreshCatalog();
  refresh();
}

void ConnectionsDialog::refresh() {
  const bool shared = broker_.enabled();
  sharing_->setText(shared ? tr("Disable sharing") : tr("Enable sharing"));
  sharing_->setEnabled(session_.loaded() || shared);
  copyCredential_->setEnabled(shared);
  const QString brokerStatus = broker_.status();
  sharingStatus_->setText(
      !brokerStatus.isEmpty()
          ? brokerStatus
          : tr("Sharing is off. Enabling it grants local agents access to this "
               "session using its private credential file."));
  const bool stdio = transport_->currentIndex() == 0;
  const bool connected = client_.connected();
  endpoint_->setPlaceholderText(
      stdio ? tr("Absolute path to server executable")
            : QStringLiteral("https://localhost:8443/mcp"));
  arguments_->setVisible(stdio);
  token_->setVisible(!stdio);
  caFile_->setVisible(!stdio);
  for (QWidget *field :
       {static_cast<QWidget *>(transport_), static_cast<QWidget *>(endpoint_),
        static_cast<QWidget *>(arguments_), static_cast<QWidget *>(token_),
        static_cast<QWidget *>(caFile_)})
    field->setEnabled(!connected);
  connect_->setText(connected ? tr("Disconnect") : tr("Connect"));
  const QString status = client_.status();
  clientStatus_->setText(!validationError_.isEmpty() ? validationError_
                         : !status.isEmpty() ? status
                                             : tr("No server connected"));
  const bool history = tabs_->currentIndex() == HistoryTab;
  const bool tool = !selectedTool_.isEmpty();
  schema_->setVisible(!history);
  call_->setVisible(!history);
  cancelCall_->setVisible(history);
  schema_->setEnabled(tool);
  call_->setEnabled(tool && connected);
  bool pending = false;
  for (const auto &entry : client_.callHistory()) {
    const auto call = entry.toMap();
    if (call.value("id").toString() == selectedCall_)
      pending = call.value("status").toString() == QLatin1String("pending");
  }
  cancelCall_->setEnabled(pending);
  selectedTitle_->setText(tool ? selectedTool_.value("name").toString()
                               : tr("Result"));
  selectedDescription_->setVisible(tool);
  selectedDescription_->setText(selectedTool_.value("description").toString());
  parameters_->setVisible(tool);
  if (result_->toPlainText() != client_.lastResult())
    result_->setPlainText(client_.lastResult());
}

void ConnectionsDialog::refreshCatalog() {
  const int tab = tabs_->currentIndex();
  const QVariantList entries = tab == ToolsTab       ? client_.tools()
                               : tab == ResourcesTab ? client_.resources()
                                                     : client_.callHistory();
  const QString current = catalog_->currentItem() ? catalog_->currentItem()
                                                        ->data(EntryRole)
                                                        .toMap()
                                                        .value("id")
                                                        .toString()
                                                  : QString();
  QSignalBlocker blocker(catalog_);
  catalog_->clear();
  for (const auto &value : entries) {
    const auto entry = value.toMap();
    QString label = entry.value("name").toString();
    if (label.isEmpty())
      label = entry.value("uri").toString();
    if (label.isEmpty())
      label = entry.value("method").toString();
    if (tab == HistoryTab)
      label +=
          QStringLiteral(" — ") + callStatus(entry.value("status").toString());
    auto *item = new QListWidgetItem(label, catalog_);
    item->setToolTip(entry.value("description").toString());
    item->setData(EntryRole, entry);
    if (tab == HistoryTab && entry.value("id").toString() == selectedCall_)
      catalog_->setCurrentItem(item);
    else if (!current.isEmpty() && entry.value("id").toString() == current)
      catalog_->setCurrentItem(item);
  }
  if (!catalog_->count())
    catalog_->setToolTip(tab == HistoryTab ? tr("No calls yet.")
                         : client_.connected()
                             ? tr("No items published by this server.")
                             : tr("Connect to a server to list its catalog."));
}

void ConnectionsDialog::connectOrDisconnect() {
  validationError_.clear();
  if (client_.connected()) {
    client_.disconnectServer();
    token_->clear();
    refresh();
    return;
  }
  if (transport_->currentIndex() == 0) {
    const auto document = QJsonDocument::fromJson(
        arguments_->text().trimmed().isEmpty() ? QByteArray("[]")
                                               : arguments_->text().toUtf8());
    QStringList arguments;
    bool valid = document.isArray();
    for (const auto &value : document.array()) {
      valid = valid && value.isString();
      arguments.append(value.toString());
    }
    if (!valid) {
      validationError_ = tr("Arguments must be a JSON array of strings.");
      refresh();
      return;
    }
    client_.connectStdio(endpoint_->text(), arguments);
  } else {
    client_.connectHttp(endpoint_->text(), token_->text(), caFile_->text());
  }
  refresh();
}

void ConnectionsDialog::selectEntry() {
  auto *item = catalog_->currentItem();
  if (!item)
    return;
  const auto entry = item->data(EntryRole).toMap();
  selectedCall_.clear();
  selectedTool_.clear();
  switch (tabs_->currentIndex()) {
  case ToolsTab:
    selectedTool_ = entry;
    parameters_->setPlainText(QStringLiteral("{}"));
    break;
  case ResourcesTab:
    client_.readResource(entry.value("uri").toString());
    break;
  default:
    selectedCall_ = entry.value("id").toString();
    client_.inspectCall(selectedCall_);
    break;
  }
  refresh();
}

void ConnectionsDialog::showSchema() {
  auto *dialog = new QDialog(this);
  dialog->setObjectName(QStringLiteral("toolSchemaDialog"));
  dialog->setWindowTitle(tr("Tool input schema"));
  dialog->resize(640, 520);
  auto *layout = new QVBoxLayout(dialog);
  auto *text = new QPlainTextEdit(dialog);
  text->setReadOnly(true);
  text->setFont(Theme::instance().codeFont());
  text->setPlainText(QString::fromUtf8(
      QJsonDocument(QJsonObject::fromVariantMap(
                        selectedTool_.value("inputSchema").toMap()))
          .toJson(QJsonDocument::Indented)));
  layout->addWidget(text);
  auto *buttons = new QDialogButtonBox(QDialogButtonBox::Close, dialog);
  connect(buttons, &QDialogButtonBox::rejected, dialog, &QDialog::reject);
  layout->addWidget(buttons);
  showModelessDialog(dialog, this);
}

} // namespace neverd::gui
