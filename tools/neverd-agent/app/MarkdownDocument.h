//===- MarkdownDocument.h - Native chat document ---------------*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Qt's Markdown parser and text selection, with themed code and tables.
//===----------------------------------------------------------------------===//
#ifndef NEVERD_TOOLS_AGENT_APP_MARKDOWNDOCUMENT_H
#define NEVERD_TOOLS_AGENT_APP_MARKDOWNDOCUMENT_H

#include <QColor>
#include <QPointer>
#include <QQuickTextDocument>
#include <QTextDocument>

namespace neverd::agent {
class MarkdownDocument : public QObject {
  Q_OBJECT
  Q_PROPERTY(QQuickTextDocument *document READ document WRITE setDocument NOTIFY
                 documentChanged)
public:
  explicit MarkdownDocument(QObject *parent = nullptr);
  QQuickTextDocument *document() const { return Target; }
  void setDocument(QQuickTextDocument *document);
  Q_INVOKABLE void render(const QString &source, bool markdown, QColor text,
                          QColor code, QColor link, QColor border,
                          const QFont &font = QFont());
  Q_INVOKABLE void copySource() const;
  Q_INVOKABLE bool hasCodeAt(int position) const;
  Q_INVOKABLE void copyCodeAt(int position) const;
  const QTextDocument &textDocument() const { return Document; }
signals:
  void documentChanged();

private:
  // Model Markdown does not fetch resources or load local images implicitly.
  class TextDocument : public QTextDocument {
  public:
    using QTextDocument::QTextDocument;
    QVariant loadResource(int, const QUrl &) override { return {}; }
  };
  struct CodeBlock {
    int Begin, End;
    QString Text;
  };
  TextDocument Document;
  QPointer<QQuickTextDocument> Target;
  QString Source;
  QList<CodeBlock> CodeBlocks;
};
} // namespace neverd::agent
#endif // NEVERD_TOOLS_AGENT_APP_MARKDOWNDOCUMENT_H
