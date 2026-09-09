#pragma once

#include "EngineClient.h"
#include "PageModel.h"

#include <QHash>
#include <QQueue>
#include <QSettings>
#include <QTimer>
#include <QTranslator>
#include <QUrl>
#include <functional>

class QQmlEngine;
class Workbench final : public QObject {
  Q_OBJECT
  Q_PROPERTY(bool loaded READ loaded NOTIFY changed)
  Q_PROPERTY(bool unsavedChanges READ unsavedChanges NOTIFY changed)
  Q_PROPERTY(bool busy READ busy NOTIFY changed)
  Q_PROPERTY(bool workerConnected READ workerConnected NOTIFY changed)
  Q_PROPERTY(QString error READ error NOTIFY changed)
  Q_PROPERTY(QString status READ status NOTIFY changed)
  Q_PROPERTY(double progress READ progress NOTIFY changed)
  Q_PROPERTY(QString fileName READ fileName NOTIFY changed)
  Q_PROPERTY(QString filePath READ filePath NOTIFY changed)
  Q_PROPERTY(QString architecture READ architecture NOTIFY changed)
  Q_PROPERTY(QString format READ format NOTIFY changed)
  Q_PROPERTY(QString selectedAddress READ selectedAddress NOTIFY changed)
  Q_PROPERTY(
      QString selectedFunctionName READ selectedFunctionName NOTIFY changed)
  Q_PROPERTY(QString selectedFunctionAddress READ selectedFunctionAddress NOTIFY
                 changed)
  Q_PROPERTY(QString selectedComment READ selectedComment NOTIFY changed)
  Q_PROPERTY(QString representation READ representation NOTIFY changed)
  Q_PROPERTY(bool representationPinned READ representationPinned NOTIFY changed)
  Q_PROPERTY(QString representationFunctionName READ representationFunctionName
                 NOTIFY changed)
  Q_PROPERTY(QString representationText READ representationText NOTIFY changed)
  Q_PROPERTY(QVariantList textMappings READ textMappings NOTIFY changed)
  Q_PROPERTY(QString mappingStatus READ mappingStatus NOTIFY changed)
  Q_PROPERTY(
      QString representationStatus READ representationStatus NOTIFY changed)
  Q_PROPERTY(bool hasMoreText READ hasMoreText NOTIFY changed)
  Q_PROPERTY(QString hexText READ hexText NOTIFY changed)
  Q_PROPERTY(QString logText READ logText NOTIFY changed)
  Q_PROPERTY(QVariantList graphNodes READ graphNodes NOTIFY changed)
  Q_PROPERTY(QVariantMap graphSummary READ graphSummary NOTIFY changed)
  Q_PROPERTY(
      QString graphViewportStatus READ graphViewportStatus NOTIFY changed)
  Q_PROPERTY(QVariantList graphEdges READ graphEdges NOTIFY changed)
  Q_PROPERTY(int functionCount READ functionCount NOTIFY changed)
  Q_PROPERTY(bool canGoBack READ canGoBack NOTIFY changed)
  Q_PROPERTY(bool canGoForward READ canGoForward NOTIFY changed)
  Q_PROPERTY(QString language READ language NOTIFY languageChanged)
  Q_PROPERTY(QStringList languages READ languages CONSTANT)
  Q_PROPERTY(QObject *functionsModel READ functionsModel CONSTANT)
  Q_PROPERTY(QObject *instructionsModel READ instructionsModel CONSTANT)
  Q_PROPERTY(QObject *xrefsModel READ xrefsModel CONSTANT)
  Q_PROPERTY(QString dockLayoutPath READ dockLayoutPath CONSTANT)
  Q_PROPERTY(bool canUndo READ canUndo NOTIFY changed)
  Q_PROPERTY(bool canRedo READ canRedo NOTIFY changed)
  Q_PROPERTY(QVariantList contributions READ contributions NOTIFY changed)
  Q_PROPERTY(QString contributionResult READ contributionResult NOTIFY changed)
public:
  explicit Workbench(QString workerPath, QObject *parent = nullptr);
  void setQmlEngine(QQmlEngine *engine);
  bool loaded() const { return loaded_; }
  bool unsavedChanges() const {
    return dirty_ || mutationActive_ || !mutations_.isEmpty();
  }
  bool busy() const { return busy_; }
  bool workerConnected() const { return connected_; }
  QString error() const { return error_; }
  QString status() const { return tr(status_.toUtf8().constData()); }
  double progress() const { return busy_ ? -1 : 1; }
  QString fileName() const;
  QString filePath() const { return filePath_; }
  QString architecture() const { return metadata_["architecture"].toString(); }
  QString format() const { return metadata_["format"].toString(); }
  QString selectedAddress() const { return selectedAddress_; }
  QString selectedFunctionName() const { return functionName_; }
  QString selectedFunctionAddress() const { return functionAddress_; }
  QString selectedComment() const { return selectedComment_; }
  QString representation() const { return representation_; }
  bool representationPinned() const { return representationPinned_; }
  QString representationFunctionName() const {
    return representationPinned_ ? pinnedName_ : functionName_;
  }
  QString representationText() const { return text_; }
  QVariantList textMappings() const {
    return mappingRevision_ == revision_ ? textMappings_ : QVariantList{};
  }
  QString mappingStatus() const;
  QString representationStatus() const {
    return tr(textStatus_.toUtf8().constData());
  }
  bool hasMoreText() const { return nextText_ > 0; }
  QString hexText() const { return hexText_; }
  QString logText() const { return log_; }
  QVariantList graphNodes() const { return nodes_; }
  QVariantMap graphSummary() const { return graphSummary_.toVariantMap(); }
  QString graphViewportStatus() const;
  QVariantList graphEdges() const { return edges_; }
  int functionCount() const { return functionCount_; }
  bool canGoBack() const { return historyIndex_ > 0; }
  bool canGoForward() const { return historyIndex_ + 1 < history_.size(); }
  QString language() const { return language_; }
  QStringList languages() const;
  QObject *functionsModel() { return &functions_; }
  QObject *instructionsModel() { return &instructions_; }
  QObject *xrefsModel() { return &xrefs_; }
  QString dockLayoutPath() const;
  bool canUndo() const { return historyState_["can_undo"].toBool(); }
  bool canRedo() const { return historyState_["can_redo"].toBool(); }
  QVariantList contributions() const { return contributions_; }
  QString contributionResult() const { return contributionResult_; }
  void setDockLayoutPath(const QString &path) { dockLayoutOverride_ = path; }
  QJsonObject selection() const;
  Q_INVOKABLE void openFile(const QUrl &url);
  Q_INVOKABLE bool requestClose();
  Q_INVOKABLE void resolveSessionChange(const QString &choice);
  Q_INVOKABLE void copyText(const QString &text);
  Q_INVOKABLE void clampWindows();
  Q_INVOKABLE void selectFunction(const QString &address);
  Q_INVOKABLE void selectInstruction(const QString &address);
  Q_INVOKABLE void selectTextLine(int line);
  Q_INVOKABLE void navigate(const QString &query);
  Q_INVOKABLE void goBack();
  Q_INVOKABLE void goForward();
  Q_INVOKABLE void filterFunctions(const QString &filter);
  Q_INVOKABLE void loadMoreFunctions();
  Q_INVOKABLE void loadMoreInstructions();
  Q_INVOKABLE void loadMoreText();
  Q_INVOKABLE void reloadRepresentation();
  Q_INVOKABLE void setRepresentation(const QString &representation);
  Q_INVOKABLE void toggleRepresentationPin();
  Q_INVOKABLE void requestView(const QString &view);
  Q_INVOKABLE void requestGraphViewport(double x, double y, double width,
                                        double height, double scale);
  Q_INVOKABLE void analyze();
  Q_INVOKABLE void cancel();
  Q_INVOKABLE void restartWorker();
  Q_INVOKABLE void renameFunction(const QString &name);
  Q_INVOKABLE void setComment(const QString &comment);
  Q_INVOKABLE void saveAnnotations();
  Q_INVOKABLE void loadAnnotations();
  Q_INVOKABLE void setLanguage(const QString &language);
  Q_INVOKABLE void undo();
  Q_INVOKABLE void redo();
  Q_INVOKABLE void importContributions(const QUrl &url);
  Q_INVOKABLE void unloadContributions(const QString &nameSpace);
  Q_INVOKABLE void runContribution(const QString &id);
  void externalQuery(const QString &id, const QString &operation,
                     const QJsonObject &payload, const QString &revision);
signals:
  void changed();
  void languageChanged();
  void selectionChanged(const QJsonObject &selection);
  void externalResponse(const QString &id, const QJsonObject &response);
  void ready();
  void confirmSessionChange();
  void closeReady();

private:
  using Callback =
      std::function<void(const QJsonObject &, const QJsonObject &)>;
  using FailureCallback = std::function<void(const QJsonObject &)>;
  struct Pending {
    QString operation;
    quint64 generation;
    bool contextual;
    Callback callback;
    QString cacheKey;
    FailureCallback failed;
    bool mutation = false;
  };
  struct Mutation {
    QString operation;
    QJsonObject payload;
    Callback callback;
    FailureCallback failed;
  };
  QString send(const QString &operation, const QJsonObject &payload,
               Callback callback, bool contextual = false,
               bool mutation = false, FailureCallback failed = {});
  void receive(const QJsonObject &message);
  void dispatchMutation();
  void resetSessionRequests();
  void performRestart();
  void finishTransition();
  void requestTransition(const QString &action);
  void openPending();
  void clearViews();
  void setError(const QString &message);
  void log(const QString &message);
  void loadFunctions(bool append);
  void loadFunctionPage(int offset);
  void loadInstructions(bool append);
  void loadText(bool append = false);
  void loadXrefs();
  void loadComment();
  void refreshHistory();
  void applyHistory(const QString &operation);
  void moveTo(const QString &address, const QString &name,
              bool recordHistory = true);
  EngineClient client_;
  PageModel functions_{{"name", "address", "size"}};
  PageModel instructions_{
      {"address", "bytes", "mnemonic", "operands", "comment"}};
  PageModel xrefs_{{"from", "to", "kind"}};
  QHash<QString, Pending> pending_;
  QHash<QString, QString> external_;
  QQueue<Mutation> mutations_;
  bool mutationActive_ = false, dirty_ = false, transitionReady_ = false;
  QString transition_;
  QCache<QString, QJsonObject> cache_{
      256 * 1024}; // Cost in conservatively estimated KiB.
  QTimer filterTimer_;
  QString workerPath_, filePath_, pendingFile_, error_, status_,
      selectedAddress_;
  QString functionAddress_, functionName_, selectedComment_,
      representation_ = "c";
  QString text_, textStatus_, hexText_, log_, language_ = "en", revision_,
                                              projectId_;
  QString filter_, nextInstruction_, centralView_ = "disasm", activeJob_;
  QString pinnedAddress_, pinnedName_;
  QString dockLayoutOverride_;
  QString contributionResult_;
  QVariantList contributions_, textMappings_;
  QString mappingRevision_, mappingState_;
  int textStartLine_ = 0;
  QJsonObject historyState_;
  QJsonObject metadata_, graphSummary_, graphViewport_;
  QVariantList nodes_, edges_;
  QStringList history_;
  int historyIndex_ = -1, functionCount_ = 0, nextFunction_ = 0, nextText_ = 0;
  quint64 generation_ = 0, filterGeneration_ = 0, graphGeneration_ = 0,
          textGeneration_ = 0;
  bool loaded_ = false, busy_ = false, connected_ = false;
  bool functionRequest_ = false, instructionRequest_ = false,
       textRequest_ = false;
  bool replayHistory_ = false;
  bool representationPinned_ = false;
  QQmlEngine *qmlEngine_ = nullptr;
  QTranslator translator_;
};
