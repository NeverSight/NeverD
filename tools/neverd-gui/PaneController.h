#pragma once

#include "PageModel.h"
#include "PaneRegistry.h"
#include "QueryService.h"

#include <QJsonObject>
#include <QTimer>
#include <array>
#include <functional>

// A pane owns its location, history and bounded display pages. It borrows the
// workspace dispatcher; creating a pane never creates another worker/session.
class PaneController final : public QObject {
  Q_OBJECT
  Q_PROPERTY(QString id READ id CONSTANT)
  Q_PROPERTY(QString dockId READ dockId CONSTANT)
  Q_PROPERTY(QString kind READ kind CONSTANT)
  Q_PROPERTY(int ordinal READ ordinal CONSTANT)
  Q_PROPERTY(QString groupId READ groupId NOTIFY changed)
  Q_PROPERTY(bool pinned READ pinned NOTIFY changed)
  Q_PROPERTY(bool open READ open NOTIFY changed)
  Q_PROPERTY(bool contentVisible READ contentVisible NOTIFY changed)
  Q_PROPERTY(bool loaded READ loaded NOTIFY changed)
  Q_PROPERTY(bool busy READ busy NOTIFY changed)
  Q_PROPERTY(QString error READ error NOTIFY changed)
  Q_PROPERTY(QString selectedAddress READ selectedAddress NOTIFY changed)
  Q_PROPERTY(QString selectedFunctionAddress READ selectedFunctionAddress NOTIFY
                 changed)
  Q_PROPERTY(
      QString selectedFunctionName READ selectedFunctionName NOTIFY changed)
  Q_PROPERTY(QString selectedComment READ selectedComment NOTIFY changed)
  Q_PROPERTY(bool commentReady READ commentReady NOTIFY changed)
  Q_PROPERTY(QString centralView READ centralView NOTIFY changed)
  Q_PROPERTY(QString representation READ representation NOTIFY changed)
  Q_PROPERTY(bool representationPinned READ pinned NOTIFY changed)
  Q_PROPERTY(QString representationFunctionName READ selectedFunctionName NOTIFY
                 changed)
  Q_PROPERTY(QString representationText READ representationText NOTIFY changed)
  Q_PROPERTY(QVariantList textMappings READ textMappings NOTIFY changed)
  Q_PROPERTY(QString mappingStatus READ mappingStatus NOTIFY changed)
  Q_PROPERTY(
      QString representationStatus READ representationStatus NOTIFY changed)
  Q_PROPERTY(bool hasMoreText READ hasMoreText NOTIFY changed)
  Q_PROPERTY(QString hexText READ hexText NOTIFY changed)
  Q_PROPERTY(QVariantMap graphSummary READ graphSummary NOTIFY changed)
  Q_PROPERTY(QVariantList graphNodes READ graphNodes NOTIFY changed)
  Q_PROPERTY(QVariantList graphEdges READ graphEdges NOTIFY changed)
  Q_PROPERTY(
      QString graphViewportStatus READ graphViewportStatus NOTIFY changed)
  Q_PROPERTY(QObject *instructionsModel READ instructionsModel CONSTANT)
  Q_PROPERTY(QObject *xrefsModel READ xrefsModel CONSTANT)
  Q_PROPERTY(bool canGoBack READ canGoBack NOTIFY changed)
  Q_PROPERTY(bool canGoForward READ canGoForward NOTIFY changed)
public:
  PaneController(QString id, QString kind, int ordinal, QueryService *queries,
                 PaneRegistry *registry, QObject *parent = nullptr);
  ~PaneController() override;
  QString id() const { return id_; }
  QString dockId() const;
  QString kind() const { return kind_; }
  int ordinal() const { return ordinal_; }
  QString groupId() const { return groupId_; }
  bool pinned() const { return pinned_; }
  bool open() const { return open_; }
  bool contentVisible() const { return contentVisible_; }
  bool loaded() const { return loaded_; }
  bool busy() const;
  QString error() const { return error_; }
  QString selectedAddress() const { return location_.address; }
  QString selectedFunctionAddress() const { return location_.functionAddress; }
  QString selectedFunctionName() const { return location_.functionName; }
  QString selectedComment() const { return location_.comment; }
  bool commentReady() const { return location_.commentKnown; }
  QString centralView() const { return centralView_; }
  QString representation() const { return representation_; }
  QString representationText() const { return text_; }
  QVariantList textMappings() const;
  QString mappingStatus() const;
  QString representationStatus() const;
  bool hasMoreText() const { return nextText_ > 0; }
  QString hexText() const { return hexText_; }
  QVariantMap graphSummary() const;
  QVariantList graphNodes() const;
  QVariantList graphEdges() const;
  QString graphViewportStatus() const;
  QObject *instructionsModel() { return &instructions_; }
  QObject *xrefsModel() { return &xrefs_; }
  bool canGoBack() const { return historyIndex_ > 0; }
  bool canGoForward() const { return historyIndex_ + 1 < history_.size(); }
  QJsonObject selection() const;
  Q_INVOKABLE void navigate(const QString &query);
  Q_INVOKABLE void selectFunction(const QString &address) { navigate(address); }
  Q_INVOKABLE void selectInstruction(const QString &address);
  Q_INVOKABLE void selectTextLine(int line);
  Q_INVOKABLE void goBack();
  Q_INVOKABLE void goForward();
  Q_INVOKABLE void loadMoreInstructions();
  Q_INVOKABLE void loadMoreText();
  Q_INVOKABLE void reloadRepresentation();
  Q_INVOKABLE void setRepresentation(const QString &representation);
  Q_INVOKABLE void toggleRepresentationPin();
  Q_INVOKABLE void requestView(const QString &view);
  Q_INVOKABLE void requestGraphViewport(double x, double y, double width,
                                        double height, double scale);
  Q_INVOKABLE void copyText(const QString &text);
signals:
  void changed();
  void selectionChanged();
  void errorOccurred(const QString &message);

private:
  friend class PaneRegistry;
  enum Channel {
    Navigation,
    Instructions,
    Text,
    Hex,
    Graph,
    References,
    Comment,
    ChannelCount
  };
  using Completion = std::function<void(const QJsonObject &)>;
  bool request(Channel channel, QueryService::QuerySpec spec,
               Completion complete, bool graph = false);
  void cancelChannel(Channel channel);
  void cancelRequests();
  // False means a synchronous observer invalidated this operation or its owner.
  bool clearResults();
  void applyLocation(const PaneLocation &location, PaneNavigation kind,
                     int historyTarget = -1, bool recordHistory = true);
  void navigateTo(const QString &query, int historyTarget);
  void loadInstructions(bool append);
  void loadText(bool append = false);
  void loadXrefs();
  void loadComment();
  struct DetailContext {
    quint64 epoch = 0, navigation = 0, membership = 0, selection = 0;
    QString address;
  };
  bool currentDetails(const DetailContext &context) const;
  void queueDetail(Channel channel);
  void loadSelectionDetail();
  void finishSelectionDetail(quint64 generation);
  void resetSelectionDetails();
  void refreshVisible();
  void reloadRepresentationImpl();
  void requestViewImpl(const QString &view);
  void fail(const QJsonObject &response);
  bool canFetch() const {
    return loaded_ && open_ && contentVisible_ && !removing_;
  }
  QueryService *queries_;
  PaneRegistry *registry_;
  QString id_, kind_, groupId_, error_;
  int ordinal_;
  PaneLocation location_;
  QString centralView_ = "disasm", representation_ = "c";
  bool pinned_ = false, open_ = true, contentVisible_ = false;
  bool loaded_ = false, removing_ = false;
  bool navigationPending_ = false;
  bool analysisRefreshSuppressed_ = false;
  quint64 navigationSequence_ = 0, membershipEpoch_ = 0;
  std::array<quint64, ChannelCount> generations_{};
  std::array<QueryService::SubscriptionId, ChannelCount> pending_{};
  QTimer selectionTimer_;
  DetailContext queuedDetailContext_;
  unsigned queuedDetails_ = 0;
  bool detailActive_ = false;
  quint64 selectionGeneration_ = 0, detailRequestGeneration_ = 0;
  PageModel instructions_{
      {"address", "bytes", "mnemonic", "operands", "comment"}};
  PageModel xrefs_{{"from", "to", "kind"}};
  QString nextInstruction_, text_, textStatus_, mappingRevision_, mappingState_;
  QString hexText_, graphRevision_;
  int nextText_ = 0, textStartLine_ = 0;
  QVariantList textMappings_, nodes_, edges_;
  QJsonObject graphSummary_, graphViewport_;
  QStringList history_;
  int historyIndex_ = -1;
};
