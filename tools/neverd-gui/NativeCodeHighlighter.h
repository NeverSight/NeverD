#pragma once
#include <QObject>
#include <QPointer>
#include <QQuickTextDocument>
#include <QSyntaxHighlighter>
#include <QVariantList>

// Formatting only; source text and source-address semantics are never
// rewritten.
class NativeCodeHighlighter : public QObject {
  Q_OBJECT
  Q_PROPERTY(QQuickTextDocument *document READ document WRITE setDocument NOTIFY
                 documentChanged)
  Q_PROPERTY(bool enabled READ enabled WRITE setEnabled NOTIFY enabledChanged)
  Q_PROPERTY(QVariantList mappings READ mappings WRITE setMappings NOTIFY
                 mappingsChanged)
  Q_PROPERTY(QString selectedAddress READ selectedAddress WRITE
                 setSelectedAddress NOTIFY selectedAddressChanged)
public:
  explicit NativeCodeHighlighter(QObject *parent = nullptr) : QObject(parent) {}
  ~NativeCodeHighlighter() override;
  QQuickTextDocument *document() const { return document_; }
  bool enabled() const { return enabled_; }
  void setDocument(QQuickTextDocument *document);
  void setEnabled(bool enabled);
  QVariantList mappings() const { return mappings_; }
  QString selectedAddress() const { return selectedAddress_; }
  void setMappings(const QVariantList &mappings);
  void setSelectedAddress(const QString &address);
  Q_INVOKABLE int lineAtPosition(int position) const;
  Q_INVOKABLE int firstMappedPosition() const;
signals:
  void documentChanged();
  void enabledChanged();
  void mappingsChanged();
  void selectedAddressChanged();

private:
  void attach();
  void updateMappedLines();
  QPointer<QQuickTextDocument> document_;
  QPointer<QSyntaxHighlighter> highlighter_;
  bool enabled_ = true;
  QVariantList mappings_;
  QString selectedAddress_;
};
