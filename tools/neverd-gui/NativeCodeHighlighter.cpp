#include "NativeCodeHighlighter.h"

#include <QRegularExpression>
#include <QSet>
#include <QTextBlock>
#include <QTextCharFormat>

namespace {
class Syntax final : public QSyntaxHighlighter {
public:
  explicit Syntax(QTextDocument *document) : QSyntaxHighlighter(document) {}
  QSet<int> mappedLines;
  void setMappedLines(const QSet<int> &lines) {
    const auto changed = (mappedLines - lines) + (lines - mappedLines);
    mappedLines = lines;
    for (const auto line : changed) {
      const auto block = document()->findBlockByNumber(line);
      if (block.isValid())
        rehighlightBlock(block);
    }
  }

protected:
  void highlightBlock(const QString &text) override {
    static const QList<QPair<QRegularExpression, QColor>> rules{
        {QRegularExpression(
             R"(\b(?:void|bool|char|int|long|short|float|double|signed|unsigned|const|static|struct|enum|typedef|volatile|auto|uint\d+_t|int\d+_t|i\d+|ptr)\b)"),
         QColor("#569cd6")},
        {QRegularExpression(
             R"(\b(?:if|else|while|for|do|switch|case|default|break|continue|return|goto|phi|br|ret|call|load|store|select)\b)"),
         QColor("#c586c0")},
        {QRegularExpression(R"(\b(?:0x[0-9a-fA-F]+|\d+(?:\.\d+)?)\b)"),
         QColor("#b5cea8")},
        {QRegularExpression(R"(\b[A-Za-z_]\w*(?=\s*\())"), QColor("#dcdcaa")},
        {QRegularExpression(QStringLiteral("\"(?:\\\\.|[^\"\\\\])*\"")),
         QColor("#ce9178")},
        {QRegularExpression(R"(//.*$|^\s*;.*$)"), QColor("#6a9955")}};
    for (const auto &rule : rules) {
      auto matches = rule.first.globalMatch(text);
      while (matches.hasNext()) {
        const auto match = matches.next();
        setFormat(match.capturedStart(), match.capturedLength(), rule.second);
      }
    }
    setCurrentBlockState(0);
    qsizetype start = previousBlockState() == 1 ? 0 : text.indexOf("/*");
    while (start >= 0) {
      const auto end = text.indexOf(
          "*/", start + (previousBlockState() == 1 && start == 0 ? 0 : 2));
      if (end < 0) {
        setFormat(start, text.size() - start, QColor("#6a9955"));
        setCurrentBlockState(1);
        break;
      }
      setFormat(start, end + 2 - start, QColor("#6a9955"));
      start = text.indexOf("/*", end + 2);
    }
    if (mappedLines.contains(currentBlock().blockNumber())) {
      for (int position = 0; position < text.size(); ++position) {
        auto style = format(position);
        style.setBackground(QColor("#264f78"));
        setFormat(position, 1, style);
      }
    }
  }
};
} // namespace
NativeCodeHighlighter::~NativeCodeHighlighter() { delete highlighter_; }
void NativeCodeHighlighter::setDocument(QQuickTextDocument *document) {
  if (document_ == document)
    return;
  document_ = document;
  attach();
  emit documentChanged();
}
void NativeCodeHighlighter::setEnabled(bool enabled) {
  if (enabled_ == enabled)
    return;
  enabled_ = enabled;
  attach();
  emit enabledChanged();
}
void NativeCodeHighlighter::attach() {
  delete highlighter_;
  highlighter_ = nullptr;
  if (enabled_ && document_) {
    highlighter_ = new Syntax(document_->textDocument());
    updateMappedLines();
  }
}

void NativeCodeHighlighter::setMappings(const QVariantList &mappings) {
  if (mappings_ == mappings)
    return;
  mappings_ = mappings;
  updateMappedLines();
  emit mappingsChanged();
}
void NativeCodeHighlighter::setSelectedAddress(const QString &address) {
  if (selectedAddress_ == address)
    return;
  selectedAddress_ = address;
  updateMappedLines();
  emit selectedAddressChanged();
}
void NativeCodeHighlighter::updateMappedLines() {
  if (!highlighter_)
    return;
  QSet<int> lines;
  if (!selectedAddress_.isEmpty())
    for (const auto &value : mappings_) {
      const auto row = value.toMap();
      for (const auto &address : row["addresses"].toList()) {
        if (address.toString() == selectedAddress_) {
          lines.insert(row["line"].toInt());
          break;
        }
      }
    }
  static_cast<Syntax *>(highlighter_.data())->setMappedLines(lines);
}
int NativeCodeHighlighter::lineAtPosition(int position) const {
  if (!document_ || position < 0)
    return -1;
  const auto block = document_->textDocument()->findBlock(position);
  return block.isValid() ? block.blockNumber() : -1;
}

int NativeCodeHighlighter::firstMappedPosition() const {
  if (!document_ || selectedAddress_.isEmpty())
    return -1;
  for (const auto &value : mappings_) {
    const auto row = value.toMap();
    for (const auto &address : row["addresses"].toList()) {
      if (address.toString() != selectedAddress_)
        continue;
      const auto block =
          document_->textDocument()->findBlockByNumber(row["line"].toInt());
      if (block.isValid())
        return block.position();
    }
  }
  return -1;
}
