#pragma once

#include <QObject>
#include <QPointer>
#include <QVariantList>
#include <QVariantMap>
#include <memory>

class PaneController;
class QueryService;

struct PaneLocation {
  QString address, functionAddress, functionName, comment;
  bool commentKnown = false;
};

enum class PaneNavigation { Function, Instruction };

struct PaneNavigationTicket {
  QPointer<PaneController> origin;
  quint64 sessionEpoch = 0, paneSequence = 0, membershipEpoch = 0;
  QString groupId;
  quint64 groupIncarnation = 0, groupSequence = 0;
  PaneNavigation kind = PaneNavigation::Function;
  int historyTarget = -1;
};

// Owns pane identity and navigation intent. Only this object applies group
// commits; a pane's changed signal is never interpreted as a new navigation.
class PaneRegistry final : public QObject {
  Q_OBJECT
  Q_PROPERTY(QVariantList items READ items NOTIFY changed)
  Q_PROPERTY(QVariantList groups READ groups NOTIFY changed)
  Q_PROPERTY(QObject *activePane READ activePaneObject NOTIFY changed)
  Q_PROPERTY(QString activePaneId READ activePaneId NOTIFY changed)
  Q_PROPERTY(QObject *defaultMachine READ defaultMachineObject CONSTANT)
  Q_PROPERTY(
      QObject *defaultRepresentation READ defaultRepresentationObject CONSTANT)
  Q_PROPERTY(QString error READ error NOTIFY changed)
public:
  explicit PaneRegistry(QueryService *queries, QObject *parent = nullptr);
  ~PaneRegistry() override;

  QVariantList items() const;
  QVariantList groups() const;
  QString activePaneId() const;
  QString error() const;
  QObject *activePaneObject() const;
  QObject *defaultMachineObject() const;
  QObject *defaultRepresentationObject() const;
  PaneController *activePane() const;
  PaneController *defaultMachine() const;
  PaneController *defaultRepresentation() const;
  PaneController *findPane(const QString &id) const;
  Q_INVOKABLE QObject *pane(const QString &id) const;
  Q_INVOKABLE QString createPane(const QString &kind,
                                 const QString &sourceId = {});
  Q_INVOKABLE void setActivePane(const QString &id);
  Q_INVOKABLE void setPaneOpen(const QString &id, bool open);
  Q_INVOKABLE void setPaneVisible(const QString &id, bool visible);
  Q_INVOKABLE void setPaneGroup(const QString &id, const QString &groupId);
  Q_INVOKABLE void setPanePinned(const QString &id, bool pinned);
  Q_INVOKABLE QString createGroup(const QString &name);
  Q_INVOKABLE void renameGroup(const QString &id, const QString &name);
  Q_INVOKABLE void removeGroup(const QString &id);
  Q_INVOKABLE bool beginRemovePane(const QString &id);
  Q_INVOKABLE void finishRemovePane(const QString &id);
  Q_INVOKABLE QVariantMap serializeMetadata() const;
  Q_INVOKABLE bool restoreMetadata(const QVariantMap &metadata);
  void setBinaryIdentity(const QString &hash);
  quint64 navigationRevision() const;
  bool hasPendingReads() const;
  void setLoaded(bool loaded);
  void cancelReads();
  // Discovery repairs metadata at the current address; it is not navigation.
  void repairAnalysisLocations(PaneController *only = nullptr);
  void resumeAnalysisReads(PaneController *only = nullptr);
  void refreshAnalysisViews();
  void refreshAnnotations();
  void renamed(const QString &address, const QString &name);
  void commented(const QString &address, const QString &comment);

  PaneNavigationTicket beginNavigation(PaneController *pane,
                                       PaneNavigation kind,
                                       int historyTarget = -1);
  bool isCurrent(const PaneNavigationTicket &ticket) const;
  bool commitNavigation(const PaneNavigationTicket &ticket,
                        const PaneLocation &location);
  void finishNavigation(const PaneNavigationTicket &ticket);
  void retireNavigation(PaneController *pane);
signals:
  void changed();
  void selectionChanged();
  void paneError(const QString &paneId, const QString &message);

private:
  struct State;
  std::unique_ptr<State> state_;
};
