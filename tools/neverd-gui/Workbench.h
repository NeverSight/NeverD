#pragma once

#include "EngineClient.h"
#include "PageModel.h"
#include "PaneController.h"
#include "PaneRegistry.h"
#include "QueryService.h"

#include <QSettings>
#include <QTimer>
#include <QTranslator>
#include <QUrl>
#include <functional>

class QQmlEngine;
class Workbench final : public QObject {
  Q_OBJECT
  Q_PROPERTY(QObject *panes READ panes CONSTANT)
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
  Q_PROPERTY(QObject *instructionsModel READ instructionsModel NOTIFY changed)
  Q_PROPERTY(QObject *xrefsModel READ xrefsModel NOTIFY changed)
  Q_PROPERTY(QString dockLayoutPath READ dockLayoutPath CONSTANT)
  Q_PROPERTY(bool canUndo READ canUndo NOTIFY changed)
  Q_PROPERTY(bool canRedo READ canRedo NOTIFY changed)
  Q_PROPERTY(QVariantList contributions READ contributions NOTIFY changed)
  Q_PROPERTY(QString contributionResult READ contributionResult NOTIFY changed)
public:
  explicit Workbench(QString workerPath, QObject *parent = nullptr);
  ~Workbench() override;
  void setQmlEngine(QQmlEngine *engine);
  bool loaded() const { return loaded_; }
  bool unsavedChanges() const { return dirty_ || pendingWrites_ > 0; }
  bool busy() const {
    return opening_ || queries_.hasPending() || panes_.hasPendingReads();
  }
  bool workerConnected() const { return connected_; }
  QString error() const { return error_; }
  QString status() const { return tr(status_.toUtf8().constData()); }
  double progress() const { return busy() ? -1 : 1; }
  QString fileName() const;
  QString filePath() const { return filePath_; }
  QString architecture() const { return metadata_["architecture"].toString(); }
  QString format() const { return metadata_["format"].toString(); }
  QString selectedAddress() const { return selectedPane()->selectedAddress(); }
  QString selectedFunctionName() const {
    return selectedPane()->selectedFunctionName();
  }
  QString selectedFunctionAddress() const {
    return selectedPane()->selectedFunctionAddress();
  }
  QString selectedComment() const { return selectedPane()->selectedComment(); }
  QString representation() const {
    return representationPane()->representation();
  }
  bool representationPinned() const { return representationPane()->pinned(); }
  QString representationFunctionName() const {
    return representationPane()->selectedFunctionName();
  }
  QString representationText() const {
    return representationPane()->representationText();
  }
  QVariantList textMappings() const {
    return representationPane()->textMappings();
  }
  QString mappingStatus() const {
    return representationPane()->mappingStatus();
  }
  QString representationStatus() const {
    return representationPane()->representationStatus();
  }
  bool hasMoreText() const { return representationPane()->hasMoreText(); }
  QString hexText() const { return machinePane()->hexText(); }
  QString logText() const { return log_; }
  QVariantList graphNodes() const { return machinePane()->graphNodes(); }
  QVariantMap graphSummary() const { return machinePane()->graphSummary(); }
  QString graphViewportStatus() const {
    return machinePane()->graphViewportStatus();
  }
  QVariantList graphEdges() const { return machinePane()->graphEdges(); }
  int functionCount() const { return functionCount_; }
  bool canGoBack() const { return selectedPane()->canGoBack(); }
  bool canGoForward() const { return selectedPane()->canGoForward(); }
  QString language() const { return language_; }
  QStringList languages() const;
  QObject *functionsModel() { return &functions_; }
  QObject *instructionsModel() { return machinePane()->instructionsModel(); }
  QObject *xrefsModel() { return selectedPane()->xrefsModel(); }
  QString dockLayoutPath() const;
  bool canUndo() const { return historyState_["can_undo"].toBool(); }
  bool canRedo() const { return historyState_["can_redo"].toBool(); }
  QVariantList contributions() const { return contributions_; }
  QString contributionResult() const { return contributionResult_; }
  void setDockLayoutPath(const QString &path) { dockLayoutOverride_ = path; }
  QObject *panes() { return &panes_; }
  PaneRegistry *paneRegistry() { return &panes_; }
  QJsonObject selection() const;
  Q_INVOKABLE QVariantMap captureCommandTarget() const;
  Q_INVOKABLE void renameFunctionAt(const QVariantMap &target,
                                    const QString &name);
  Q_INVOKABLE void setCommentAt(const QVariantMap &target,
                                const QString &comment);
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
  QString send(const QString &operation, const QJsonObject &payload,
               Callback callback, bool mutation = false,
               FailureCallback failed = {});
  PaneController *selectedPane() const;
  PaneController *machinePane() const;
  PaneController *representationPane() const;
  bool validCommandTarget(const QVariantMap &target) const;
  void receive(const QJsonObject &message);
  void resetSessionRequests();
  void performRestart();
  void finishTransition();
  void requestTransition(const QString &action);
  void openPending();
  void clearViews();
  void setError(const QString &message);
  void log(const QString &message);
  void publishAnalysisCompletion(quint64 epoch);
  void loadFunctions(bool append);
  void loadFunctionPage(int offset);
  void refreshHistory();
  void applyHistory(const QString &operation);
  EngineClient client_;
  QueryService queries_;
  QObject workspaceReads_;
  PaneRegistry panes_;
  PageModel functions_{{"name", "address", "size"}};
  QTimer filterTimer_;
  QString workerPath_, filePath_, pendingFile_, error_, status_, log_,
      language_ = "en";
  QString revision_, projectId_, filter_, transition_, dockLayoutOverride_,
      contributionResult_;
  QVariantList contributions_;
  QJsonObject historyState_, metadata_;
  quint64 filterGeneration_ = 0, sessionEpoch_ = 0, analysisPublishedEpoch_ = 0;
  int functionCount_ = 0, nextFunction_ = 0, pendingWrites_ = 0;
  bool loaded_ = false, connected_ = false, opening_ = false;
  bool dirty_ = false, transitionReady_ = false, functionRequest_ = false;
  QQmlEngine *qmlEngine_ = nullptr;
  QTranslator translator_;
};
