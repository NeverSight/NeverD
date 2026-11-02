#pragma once

#include <QJsonArray>
#include <QObject>
#include <QSet>
#include <QVariantList>
#include <QVector>

// A reversible projection of one loaded source page. UTF-8 byte offsets are
// validated before conversion to Qt's UTF-16 coordinates. The worker's original
// text remains the only source for copying/exporting or instruction navigation.
// Regions are recognized library operations unless their "kind" names another
// fold, such as the prelude before a definition; only library operations
// count as foldable and follow setFolded.
class LibraryCodeView final : public QObject {
  Q_OBJECT
  Q_PROPERTY(QString text READ text NOTIFY changed)
  Q_PROPERTY(QVariantList mappings READ mappings NOTIFY changed)
  Q_PROPERTY(QVariantList regions READ regions NOTIFY changed)
  Q_PROPERTY(int foldableCount READ foldableCount NOTIFY changed)
  Q_PROPERTY(bool anyFolded READ anyFolded NOTIFY changed)
public:
  explicit LibraryCodeView(QObject *parent = nullptr) : QObject(parent) {}
  void reset(const QString &original, const QVariantList &mappings,
             const QJsonArray &regions, qint64 byteOffset = 0,
             bool keepFolded = false, bool sourceBlocks = false);
  QString text() const { return text_; }
  QVariantList mappings() const { return mappings_; }
  QVariantList regions() const;
  int foldableCount() const;
  /// Some region, library operation or not, shows as its summary.
  bool anyFolded() const { return !folded_.isEmpty(); }
  /// Some library operation shows as its summary.
  bool libraryFolded() const;
  /// Some region of any kind can fold.
  bool canFold() const;
  bool isFolded(const QString &id) const { return folded_.contains(id); }
  /// A region \p id can fold.
  bool hasRegion(const QString &id) const;
  /// The display line showing the start of source line \p sourceLine, -1
  /// past the end.
  int displayLine(int sourceLine) const;
  /// Expand folds intersecting an original source row. Return whether the
  /// display projection changed.
  bool unfoldSourceLine(int sourceLine);
  Q_INVOKABLE void toggleRegion(const QString &id);
  /// Fold or unfold every library operation.
  Q_INVOKABLE void setFolded(bool folded);
  void setRegionFolded(const QString &id, bool folded);
  /// Visible brace-block headers, separate from recognized library operations.
  struct Block {
    QString id;
    int line;
    bool folded;
  };
  QVector<Block> blocks() const;
  /// The innermost visible or enclosing brace block at a display position.
  QString blockAt(int position) const;
  QSet<QString> foldedBlocks() const;
  /// Generated layout and complete exception details, expanded on demand.
  QSet<QString> defaultDetailBlocks() const;
  void setFoldedBlocks(const QSet<QString> &ids);
  /// Brace topology, unaffected by renamed identifiers or added comments.
  QString blockStructure() const { return blockStructure_; }
  int projectedPosition(int originalPosition) const {
    return displayPosition(originalPosition);
  }
  Q_INVOKABLE QString regionAt(int position) const;
  Q_INVOKABLE int sourceLineAt(int position) const;
  int originalPosition(int position) const {
    return sourcePosition(position, false);
  }
  Q_INVOKABLE QString originalSelection(int begin, int end) const;
  /// Display ranges [begin, end) of folded summaries.
  QVector<QPair<int, int>> foldedRanges() const;
  Q_INVOKABLE void copySelection(int begin, int end) const;
signals:
  void changed();

private:
  struct Span {
    int begin = 0, end = 0;
  };
  struct Region {
    QVariantMap details;
    QString id, summary;
    QVector<Span> spans;
    bool available = false;
    bool library = true;
    int header = -1;
  };
  struct Segment {
    int begin, end, sourceBegin, sourceEnd;
    QString region;
    bool block;
  };
  int sourcePosition(int position, bool end) const;
  int displayPosition(int position) const;
  void rebuild();
  void addSourceBlocks();
  QString original_, text_;
  QString blockStructure_;
  QVector<int> sourceLines_, displayLines_;
  QVariantList originalMappings_, mappings_;
  QVector<Region> regions_;
  QVector<Segment> segments_;
  QSet<QString> folded_;
};
