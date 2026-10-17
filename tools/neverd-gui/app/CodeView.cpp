#include "CodeView.h"

#include "Icons.h"
#include "Session.h"
#include "ShrinkableRow.h"
#include "Theme.h"

#include <QActionGroup>
#include <QApplication>
#include <QCheckBox>
#include <QClipboard>
#include <QComboBox>
#include <QCoreApplication>
#include <QCursor>
#include <QDialog>
#include <QDialogButtonBox>
#include <QHBoxLayout>
#include <QHelpEvent>
#include <QJsonObject>
#include <QKeyEvent>
#include <QLabel>
#include <QMenu>
#include <QPainter>
#include <QRegularExpression>
#include <QScopedValueRollback>
#include <QScrollBar>
#include <QSet>
#include <QTextLine>
#include <QToolButton>
#include <QToolTip>
#include <QVBoxLayout>
#include <algorithm>
#include <limits>
#include <utility>

namespace neverd::gui {
namespace {
constexpr int PageLines = 256;
constexpr int Margin = 6;
constexpr int WheelLines = 3;

/// The goto label \p line defines and the block address it names: HighC
/// labels a goto's block `L_` and the address in hexadecimal (blockLabel in
/// lib/backend/c/HighC/HighCStmtWriter.cpp), defined as `L_140001010:` or,
/// in Rust, as a loop's `'L_140001010: loop`.
std::optional<std::pair<QStringView, Address>>
labelDefinition(QStringView line) {
  line = line.trimmed();
  if (line.startsWith(u'\''))
    line = line.mid(1);
  if (!line.startsWith(u"L_"))
    return std::nullopt;
  qsizetype end = 2;
  while (end < line.size() && ((line[end] >= u'0' && line[end] <= u'9') ||
                               (line[end] >= u'A' && line[end] <= u'F')))
    ++end;
  if (end == 2 || end >= line.size() || line[end] != u':' ||
      line.mid(end).startsWith(u"::"))
    return std::nullopt;
  bool valid = false;
  const Address address = line.mid(2, end - 2).toULongLong(&valid, 16);
  if (!valid)
    return std::nullopt;
  return std::pair{line.left(end), address};
}
/// How often a drag beyond an edge scrolls the text.
constexpr int DragScrollMs = 40;
/// The fold over the lines before a definition: C's includes and
/// declarations, or the declarations of what LLVM IR refers to.
constexpr char PreludeRegion[] = "prelude";
/// The fold over the attribute groups and metadata after LLVM IR's
/// definition.
constexpr char EpilogueRegion[] = "epilogue";

/// How a representation is colored; Source takes the language the page names.
enum class Dialect { C, Cpp, Rust, Go, IR, LLVM, Source };
struct Representation {
  const char *name;
  const char *title;
  Dialect dialect;
};
constexpr Representation Representations[] = {
#define NEVERD_REPRESENTATION(Name, Title, Kind) {Name, Title, Dialect::Kind},
#include "Representations.def"
};

const Representation *representationOf(const QString &name) {
  for (const auto &entry : Representations)
    if (name == QLatin1String(entry.name))
      return &entry;
  return nullptr;
}

// Code token roles, colored through the Code* theme colors.
enum CodeRole : int {
  Default,
  Keyword,
  Control,
  Type,
  Number,
  String,
  Comment,
  Function,
  Variable,
  Constant,
  Punctuation,
  Preprocessor
};

QColor codeColor(int role) {
  static constexpr ColorRole Map[] = {
      ColorRole::CodeDefault,     ColorRole::CodeKeyword,
      ColorRole::CodeControl,     ColorRole::CodeType,
      ColorRole::CodeNumber,      ColorRole::CodeString,
      ColorRole::CodeComment,     ColorRole::CodeFunction,
      ColorRole::CodeVariable,    ColorRole::CodeConstant,
      ColorRole::CodePunctuation, ColorRole::CodePreprocessor};
  return Theme::instance().color(
      Map[role >= 0 && role < int(std::size(Map)) ? role : 0]);
}

/// A source language's words: control flow, other keywords and types.
struct Vocabulary {
  QSet<QString> control, keywords, types;
};

const Vocabulary &vocabularyOf(Dialect dialect) {
  static const Vocabulary C = {{
#define NEVERD_C_CONTROL(Word) QStringLiteral(Word),
#include "CodeVocabulary.def"
                               },
                               {
#define NEVERD_C_KEYWORD(Word) QStringLiteral(Word),
#include "CodeVocabulary.def"
                               },
                               {
#define NEVERD_C_TYPE(Word) QStringLiteral(Word),
#include "CodeVocabulary.def"
                               }};
  static const Vocabulary Rust = {{
#define NEVERD_RUST_CONTROL(Word) QStringLiteral(Word),
#include "CodeVocabulary.def"
                                  },
                                  {
#define NEVERD_RUST_KEYWORD(Word) QStringLiteral(Word),
#include "CodeVocabulary.def"
                                  },
                                  {
#define NEVERD_RUST_TYPE(Word) QStringLiteral(Word),
#include "CodeVocabulary.def"
                                  }};
  static const Vocabulary Go = {{
#define NEVERD_GO_CONTROL(Word) QStringLiteral(Word),
#include "CodeVocabulary.def"
                                },
                                {
#define NEVERD_GO_KEYWORD(Word) QStringLiteral(Word),
#include "CodeVocabulary.def"
                                },
                                {
#define NEVERD_GO_TYPE(Word) QStringLiteral(Word),
#include "CodeVocabulary.def"
                                }};
  static const Vocabulary Cpp = [] {
    Vocabulary Words = C;
    for (const char *Word : {"class",       "namespace",
                             "template",    "typename",
                             "public",      "protected",
                             "private",     "virtual",
                             "override",    "final",
                             "constexpr",   "noexcept",
                             "static_cast", "reinterpret_cast",
                             "const_cast",  "dynamic_cast",
                             "new",         "delete",
                             "this",        "using",
                             "nullptr",     "static_assert",
                             "alignof"})
      Words.keywords.insert(QString::fromLatin1(Word));
    return Words;
  }();
  return dialect == Dialect::Cpp    ? Cpp
         : dialect == Dialect::Rust ? Rust
         : dialect == Dialect::Go   ? Go
                                    : C;
}

/// Whether \p word names one of the integer or floating types the dialect
/// spells by width: Rust's `u64` and `i96`, Go's `uint64`.
bool widthType(Dialect dialect, const QString &word) {
  static const QRegularExpression Rust(QStringLiteral("^[iuf][0-9]+$"));
  static const QRegularExpression Go(QStringLiteral("^(u?int|float)[0-9]+$"));
  return (dialect == Dialect::Rust && Rust.match(word).hasMatch()) ||
         (dialect == Dialect::Go && Go.match(word).hasMatch());
}

/// The dialect a page reads in: its representation's, or for pseudocode in
/// the function's own language the one the page names.
Dialect dialectOf(const QString &representation, const QString &language) {
  const auto *entry = representationOf(representation);
  const Dialect dialect = entry ? entry->dialect : Dialect::C;
  if (dialect != Dialect::Source)
    return dialect;
  if (language == QLatin1String("cpp"))
    return Dialect::Cpp;
  if (language == QLatin1String("rust"))
    return Dialect::Rust;
  if (language == QLatin1String("go"))
    return Dialect::Go;
  return Dialect::C;
}

/// Source languages share C's comments, strings and preprocessor-like lines.
bool sourceDialect(Dialect dialect) {
  return dialect == Dialect::C || dialect == Dialect::Cpp ||
         dialect == Dialect::Rust || dialect == Dialect::Go;
}
const QSet<QString> &llvmWords() {
  static const QSet<QString> words = {
#define NEVERD_LLVM_KEYWORD(Word) QStringLiteral(Word),
#include "CodeVocabulary.def"
  };
  return words;
}

bool identifierStart(QChar c) { return c.isLetter() || c == QLatin1Char('_'); }
bool identifierPart(QChar c) {
  return c.isLetterOrNumber() || c == QLatin1Char('_');
}

/// A token of a line of LLVM IR: a name with its sigil (`@main`, `@"a b"`,
/// `%sub`, `#0`, `!4`, `!llvm.module.flags`), a bare word (`load`, `bb_0`),
/// or a word of the line's comment (`0x3FD8`, `__libc_start_main_ptr`).
struct LLVMToken {
  QString text;
  int begin = 0;
  bool comment = false;
};

/// Whether \p c continues an unquoted LLVM name.
bool llvmNamePart(QChar c) {
  return c.isLetterOrNumber() || c == QLatin1Char('-') ||
         c == QLatin1Char('$') || c == QLatin1Char('.') ||
         c == QLatin1Char('_');
}

/// The tokens of a line of LLVM IR, in order, outside its strings.
QVector<LLVMToken> llvmTokens(QStringView text) {
  QVector<LLVMToken> tokens;
  const int n = int(text.size());
  for (int i = 0; i < n;) {
    const QChar c = text.at(i);
    if (c == QLatin1Char(';')) {
      for (int j = i + 1; j < n;) {
        if (!isTokenCharacter(text.at(j))) {
          ++j;
          continue;
        }
        int end = j;
        while (end < n && isTokenCharacter(text.at(end)))
          ++end;
        tokens.append({text.mid(j, end - j).toString(), j, true});
        j = end;
      }
      break;
    }
    // LLVM escapes a quote in a string, so the next quote ends it.
    if (c == QLatin1Char('"')) {
      const int close = int(text.indexOf(QLatin1Char('"'), i + 1));
      if (close < 0)
        break;
      i = close + 1;
      continue;
    }
    const bool sigil = c == QLatin1Char('@') || c == QLatin1Char('%') ||
                       c == QLatin1Char('#') || c == QLatin1Char('!');
    const int start = i + (sigil ? 1 : 0);
    int end = start;
    if (sigil && end < n && text.at(end) == QLatin1Char('"')) {
      // A quoted global or local name; `!"..."` is a metadata string.
      const int close = int(text.indexOf(QLatin1Char('"'), end + 1));
      if (close < 0)
        break;
      if (c != QLatin1Char('@') && c != QLatin1Char('%')) {
        i = close + 1;
        continue;
      }
      end = close + 1;
    } else {
      while (end < n && llvmNamePart(text.at(end)))
        ++end;
    }
    if (end == start) {
      ++i;
      continue;
    }
    tokens.append({text.mid(i, end - i).toString(), i, false});
    i = end;
  }
  return tokens;
}

/// The token of a line of LLVM IR at \p column, or none.
std::optional<LLVMToken> llvmTokenAt(QStringView text, int column) {
  for (const auto &token : llvmTokens(text))
    if (column >= token.begin && column <= token.begin + int(token.text.size()))
      return token;
  return std::nullopt;
}

/// A global's name without its sigil and quotes, as the binary may name it.
QString llvmGlobalName(const QString &token) {
  QString name = token.mid(1);
  if (name.size() < 2 || !name.startsWith(QLatin1Char('"')) ||
      !name.endsWith(QLatin1Char('"')))
    return name;
  // LLVM escapes what a quoted name cannot hold as two hexadecimal digits.
  const QByteArray quoted = name.mid(1, name.size() - 2).toUtf8();
  QByteArray bytes;
  for (int i = 0; i < quoted.size(); ++i) {
    bool ok = false;
    const int value = quoted[i] == '\\' && i + 2 < quoted.size()
                          ? quoted.mid(i + 1, 2).toInt(&ok, 16)
                          : 0;
    if (ok) {
      bytes.append(char(value));
      i += 2;
    } else {
      bytes.append(quoted[i]);
    }
  }
  return QString::fromUtf8(bytes);
}

/// The lines that define what LLVM IR names: a global or function (`@name`),
/// a type, value or parameter (`%name`), a block (its label `name:`), an
/// attribute group (`#0`) or metadata (`!4`, `!llvm.module.flags`).
QHash<QString, int> llvmDefinitions(QStringView source) {
  QHash<QString, int> found;
  int line = 0;
  for (const QStringView text : source.split(QLatin1Char('\n'))) {
    const auto tokens = llvmTokens(text);
    const auto define = [&](const QString &name) {
      if (!found.contains(name))
        found.insert(name, line);
    };
    if (!tokens.isEmpty() && !tokens.front().comment) {
      const LLVMToken &first = tokens.front();
      const QChar sigil = first.text.front();
      const QStringView after =
          text.mid(first.begin + first.text.size()).trimmed();
      if ((sigil == QLatin1Char('@') || sigil == QLatin1Char('%') ||
           sigil == QLatin1Char('!')) &&
          after.startsWith(QLatin1Char('='))) {
        define(first.text);
      } else if (first.text == QLatin1String("attributes") &&
                 tokens.size() > 1 &&
                 tokens[1].text.startsWith(QLatin1Char('#'))) {
        define(tokens[1].text);
      } else if (first.text == QLatin1String("declare") ||
                 first.text == QLatin1String("define")) {
        // The function, then a definition's parameters.
        bool named = false;
        for (const auto &token : tokens) {
          if (token.comment)
            break;
          if (!named && token.text.startsWith(QLatin1Char('@'))) {
            define(token.text);
            named = true;
          } else if (named && first.text == QLatin1String("define") &&
                     token.text.startsWith(QLatin1Char('%'))) {
            define(token.text);
          }
        }
      } else if (first.begin == 0 && llvmNamePart(sigil) &&
                 after.startsWith(QLatin1Char(':'))) {
        // A block's label, which branches name as `%label`.
        define(QLatin1Char('%') + first.text);
      }
    }
    ++line;
  }
  return found;
}
} // namespace

CodeText::CodeText(Session &session, QWidget *parent)
    : QAbstractScrollArea(parent), session_(session) {
  setFocusPolicy(Qt::StrongFocus);
  setFrameShape(QFrame::NoFrame);
  viewport()->setAttribute(Qt::WA_OpaquePaintEvent);
  viewport()->setCursor(Qt::IBeamCursor);
  viewport()->setMouseTracking(true);
  updateMetrics();
  connect(&Theme::instance(), &Theme::changed, this, [this] {
    ++styleStamp_;
    updateMetrics();
    viewport()->update();
  });
  connect(&session_, &Session::revisionChanged, this, [this] {
    if (!function_)
      return;
    if (isVisible())
      load(*function_, representation_);
    else
      interrupted_ = true;
  });
  connect(&session_, &Session::unloaded, this, &CodeText::clear);
  // A drag held beyond an edge scrolls toward it, the faster the farther
  // out, and the selection follows, as in an editor.
  dragScroll_.setInterval(DragScrollMs);
  connect(&dragScroll_, &QTimer::timeout, this, [this] {
    const QRect area = viewport()->rect();
    const auto speed = [](int distance, qreal unit) {
      return std::clamp(1 + int(distance / std::max<qreal>(1, unit)), 1, 20);
    };
    auto *rows = verticalScrollBar();
    if (dragPoint_.y() < 0)
      rows->setValue(rows->value() - speed(-dragPoint_.y(), lineHeight_));
    else if (dragPoint_.y() >= area.height())
      rows->setValue(rows->value() +
                     speed(dragPoint_.y() - area.height(), lineHeight_));
    auto *columns = horizontalScrollBar();
    if (dragPoint_.x() < 0)
      columns->setValue(columns->value() -
                        int(speed(-dragPoint_.x(), charWidth_) * charWidth_));
    else if (dragPoint_.x() >= area.width())
      columns->setValue(
          columns->value() +
          int(speed(dragPoint_.x() - area.width(), charWidth_) * charWidth_));
    dragTo(dragPoint_);
  });
}

void CodeText::updateMetrics() {
  const QFontMetricsF metrics(Theme::instance().codeFont());
  charWidth_ = metrics.horizontalAdvance(QLatin1Char('M'));
  lineHeight_ = int(std::ceil(metrics.lineSpacing()));
  ascent_ = int(std::ceil(metrics.ascent()));
  updateRange();
}

int CodeText::visibleLines() const {
  return std::max(1, viewport()->height() / std::max(1, lineHeight_));
}

void CodeText::updateRange() {
  verticalScrollBar()->setRange(
      0, std::max(0, int(lines_.size()) - visibleLines() + 1));
  verticalScrollBar()->setPageStep(visibleLines());
  horizontalScrollBar()->setRange(0, std::max(0, int(widestLine_ * charWidth_) +
                                                     textLeft() -
                                                     viewport()->width()));
  horizontalScrollBar()->setPageStep(viewport()->width());
}

void CodeText::resizeEvent(QResizeEvent *event) {
  QAbstractScrollArea::resizeEvent(event);
  updateRange();
  updateMouseCursor(viewport()->mapFromGlobal(QCursor::pos()));
}

void CodeText::scrollContentsBy(int, int) {
  updateMouseCursor(viewport()->mapFromGlobal(QCursor::pos()));
  viewport()->update();
}

void CodeText::cancel() {
  ++serial_;
  pendingAddress_.reset();
  selectedAddress_.reset();
  session_.cancelAnalysisReads(this);
  if (loading_) {
    sourceOptions_ = renderedOptions_;
    interrupted_ = true;
    loading_ = false;
    status_ = tr("Cancelled");
    emit statusChanged();
  }
}

void CodeText::clear() {
  cancel();
  placeAfterLoad_.reset();
  source_.clear();
  sourceBytes_ = renderedChars_ = renderedRows_ = widestLine_ = 0;
  interrupted_ = false;
  sourceRows_.clear();
  lineStarts_.clear();
  regions_ = {};
  regionsValid_ = false;
  prelude_ = {};
  epilogue_ = {};
  references_.clear();
  unresolvedDeclaration_.clear();
  declarations_.reset();
  library_.reset({}, {}, {});
  blocksAfterLoad_.clear();
  blockStructureAfterLoad_.clear();
  lines_.clear();
  function_.reset();
  sourceOptions_ = {};
  renderedOptions_ = {};
  status_.clear();
  cursorLine_ = cursorColumn_ = 0;
  anchor_.reset();
  selecting_ = false;
  viewport()->setCursor(Qt::IBeamCursor);
  marked_.clear();
  sourceNames_.clear();
  editNames_.clear();
  editMetadata_ = false;
  sourceNameIndex_.clear();
  unread_ = 0;
  updateRange();
  viewport()->update();
  if (!pageLanguage_.isEmpty()) {
    pageLanguage_.clear();
    emit languageChanged();
  }
  emit statusChanged();
}

void CodeText::load(Address function, const QString &representation,
                    std::optional<QJsonObject> options) {
  const bool same = function_ == function && representation_ == representation;
  const auto nextOptions =
      options.value_or(same ? sourceOptions_ : QJsonObject());
  // A reload of complete text keeps its place; one a newer reload cuts short
  // keeps the place the first one saved.
  if (!same) {
    placeAfterLoad_.reset();
    selectAllAfterLoad_ = false;
  } else if (!placeAfterLoad_ && !loading_ && !interrupted_ &&
             cursorLine_ < lines_.size())
    placeAfterLoad_ =
        SourcePlace{lines_[cursorLine_].sourceLine, cursorColumn_,
                    lines_[std::clamp(verticalScrollBar()->value(), 0,
                                      int(lines_.size()) - 1)]
                        .sourceLine};
  cancel();
  function_ = function;
  representation_ = representation;
  sourceOptions_ = nextOptions;
  unresolvedDeclaration_.clear();
  const quint64 serial = ++serial_;
  if (!same) {
    renderedOptions_ = {};
    lines_.clear();
    widestLine_ = 0;
    cursorLine_ = cursorColumn_ = 0;
    anchor_.reset();
    verticalScrollBar()->setValue(0);
  }
  inComment_ = false;
  loading_ = true;
  viewport()->setCursor(Qt::IBeamCursor);
  interrupted_ = false;
  // Folding state survives a refresh of the same function as "fold all";
  // a new function opens at its definition, its prelude folded.
  foldAfterLoad_ = same && library_.libraryFolded();
  foldPreludeAfterLoad_ = !same || library_.isFolded(PreludeRegion);
  foldEpilogueAfterLoad_ = !same || library_.isFolded(EpilogueRegion);
  blocksAfterLoad_ = same ? library_.foldedBlocks() : QSet<QString>();
  blockStructureAfterLoad_ = same ? library_.blockStructure() : QString();
  if (!same)
    library_.reset({}, {}, {});
  emit foldingChanged();
  status_ = tr("Decompiling…");
  emit statusChanged();
  request(0, serial);
}

void CodeText::setSourceOptions(const QJsonObject &options) {
  if (function_ && CodeView::isSource(representation_))
    load(*function_, representation_, options);
}

void CodeText::request(int offset, quint64 serial) {
  if (!function_)
    return;
  session_.read(
      QStringLiteral("decompile"),
      {{"address", hexAddress(*function_)},
       {"representation", representation_},
       {"source_options", sourceOptions_},
       {"offset", offset},
       {"limit", PageLines}},
      this,
      [this, offset, serial](const QJsonObject &payload) {
        if (serial != serial_)
          return;
        appendPage(payload, offset);
        renderedOptions_ = sourceOptions_;
        const auto next = payload.value("next_offset");
        if (!next.isNull() && next.toInt() > offset) {
          rebuildLines(true);
          request(next.toInt(), serial);
          return;
        }
        loading_ = false;
        // Folding needs the whole function: regions span pages.
        library_.reset(source_, sourceRows_,
                       regionsValid_ ? foldRegions() : QJsonArray(),
                       byteOffset_, false, CodeView::isSource(representation_));
        if (foldAfterLoad_)
          library_.setFolded(true);
        if (foldPreludeAfterLoad_)
          library_.setRegionFolded(PreludeRegion, true);
        if (foldEpilogueAfterLoad_)
          library_.setRegionFolded(EpilogueRegion, true);
        if (!blocksAfterLoad_.isEmpty() &&
            library_.blockStructure() == blockStructureAfterLoad_)
          library_.setFoldedBlocks(blocksAfterLoad_);
        // Unfolded pages are already colored and mapped. Only a changed
        // projection requires rebuilding earlier lines.
        rebuildLines(!library_.anyFolded());
        emit foldingChanged();
        if (const auto place = std::exchange(placeAfterLoad_, {});
            place && !lines_.isEmpty()) {
          // The first shown line at or after a source line.
          const auto shown = [this](int source) {
            for (int i = 0; i < lines_.size(); ++i)
              if (lines_[i].sourceLine >= source)
                return i;
            return int(lines_.size()) - 1;
          };
          cursorLine_ = shown(place->cursorLine);
          cursorColumn_ =
              std::clamp(place->cursorColumn, 0,
                         int(lines_[cursorLine_].styled.text.size()));
          verticalScrollBar()->setValue(shown(place->topLine));
        }
        const auto mapping = payload.value("mapping_status").toString();
        status_ = session_.metadata().value("analyzed").toBool()
                      ? QString()
                      : tr("Function-level analysis");
        if (mapping == QLatin1String("instruction_anchors"))
          status_ += (status_.isEmpty() ? QString() : QStringLiteral(" · ")) +
                     tr("rows linked to instructions");
        if (unread_ > 0)
          status_ += (status_.isEmpty() ? QString() : QStringLiteral(" · ")) +
                     tr("%n declarations shown as C", nullptr, unread_);
        if (payload.value("code_edits_stale").toBool())
          status_ +=
              QStringLiteral(" · ") +
              tr("Saved variable names belong to a different source rendering");
        if (sourceOptions_.value("symbolic").toBool())
          status_ += QStringLiteral(" · ") + tr("Symbolic simplification");
        if (representation_ == QLatin1String("llvmc"))
          status_ +=
              QStringLiteral(" · ") + sourceOptions_.value("llvm_optimization")
                                          .toString(QStringLiteral("O2"));
        updateRange();
        viewport()->update();
        if (const auto address = std::exchange(pendingAddress_, {}))
          selectAddress(*address);
        if (selectAllAfterLoad_)
          selectAll();
        emit statusChanged();
      },
      [this, serial](const QString &, const QString &message) {
        if (serial != serial_)
          return;
        loading_ = false;
        pendingAddress_.reset();
        placeAfterLoad_.reset();
        selectAllAfterLoad_ = false;
        if (!lines_.isEmpty() && sourceOptions_ != renderedOptions_) {
          sourceOptions_ = renderedOptions_;
          status_ = tr("Transformation failed: %1").arg(message);
          emit statusChanged();
          return;
        }
        // A refused function reads in no language and names nothing.
        editNames_.clear();
        editMetadata_ = true;
        sourceNames_.clear();
        sourceNameIndex_.clear();
        unread_ = 0;
        if (!pageLanguage_.isEmpty()) {
          pageLanguage_.clear();
          emit languageChanged();
        }
        lines_.clear();
        Line line;
        line.styled.text = message;
        line.styled.spans = {{0, int(message.size()), Comment, std::nullopt}};
        lines_.append(std::move(line));
        widestLine_ = message.size();
        status_ = message;
        updateRange();
        viewport()->update();
        emit statusChanged();
      });
}

void CodeText::appendPage(const QJsonObject &payload, int offset) {
  if (offset == 0) {
    library_.reset({}, {}, {});
    editNames_.clear();
    editMetadata_ = payload.contains("code_names");
  }
  const QString text = payload.value("text").toString();
  const qint64 pageOffset = payload.value("byte_offset").toInteger(-1);
  if (offset == 0) {
    source_.clear();
    sourceRows_.clear();
    regions_ = payload.value("library_regions").toArray();
    regionsValid_ =
        payload.value("library_regions").isArray() && pageOffset >= 0;
    prelude_ = payload.value("prelude").toObject();
    epilogue_ = payload.value("epilogue").toObject();
    references_.clear();
    declarations_.reset();
    byteOffset_ = std::max<qint64>(0, pageOffset);
    sourceNames_.clear();
    sourceNameIndex_.clear();
    unread_ = int(payload.value("unread").toArray().size());
    const QString language = payload.value("dialect").toString();
    if (language != pageLanguage_) {
      pageLanguage_ = language;
      emit languageChanged();
    }
  } else if (regionsValid_ && pageOffset != byteOffset_ + sourceBytes_) {
    // Pages that do not continue each other cannot share byte offsets.
    regionsValid_ = false;
  }
  if (offset == 0) {
    sourceBytes_ = renderedChars_ = renderedRows_ = widestLine_ = 0;
    lines_.clear();
    lineStarts_.clear();
  }
  const int pageCharBase = source_.size();
  source_ += text;
  sourceBytes_ += text.toUtf8().size();
  if (!text.isEmpty() && !text.endsWith(QLatin1Char('\n'))) {
    source_ += QLatin1Char('\n');
    ++sourceBytes_;
  }
  for (const auto &row : payload.value("rows").toArray())
    sourceRows_.append(row.toObject().toVariantMap());
  for (const auto &value : payload.value("references").toArray()) {
    const auto reference = value.toObject();
    const auto line = reference.value("line").toInteger(-1);
    const auto address = addressValue(reference.value("address"));
    const QString name = reference.value("name").toString();
    if (line >= 0 && line < std::numeric_limits<int>::max() && address &&
        !name.isEmpty())
      references_[int(line)].append({name, *address});
  }
  // A source name's bytes are in the page that holds its line.
  const QByteArray bytes = text.toUtf8();
  for (const auto &value : payload.value("code_names").toArray()) {
    const auto name = value.toObject();
    const auto begin = name.value("begin_byte").toInteger(-1) - pageOffset;
    const auto end = name.value("end_byte").toInteger(-1) - pageOffset;
    if (begin >= 0 && end > begin && end <= bytes.size()) {
      auto target = name;
      const auto spelling = QString::fromUtf8(bytes.mid(begin, end - begin));
      target["name"] = spelling;
      editNames_.push_back(
          {pageCharBase + int(QString::fromUtf8(bytes.left(begin)).size()),
           pageCharBase + int(QString::fromUtf8(bytes.left(end)).size()),
           std::move(target)});
    }
  }
  bool namesAdded = false;
  for (const auto &value : payload.value("source_names").toArray()) {
    const auto name = value.toObject();
    const qint64 begin = name.value("begin_byte").toInteger(-1) - pageOffset;
    const qint64 end = name.value("end_byte").toInteger(-1) - pageOffset;
    if (pageOffset < 0 || begin < 0 || end <= begin || end > bytes.size())
      continue;
    const QString spelled = QString::fromUtf8(bytes.mid(begin, end - begin));
    if (spelled.isEmpty() || sourceNames_.contains(spelled))
      continue;
    sourceNames_.insert(spelled, {name.value("identifier").toString(),
                                  name.value("symbol").toString(),
                                  addressValue(name.value("address"))});
    auto &bucket = sourceNameIndex_[spelled.at(0)];
    bucket.append(spelled);
    namesAdded = true;
  }
  if (namesAdded)
    for (auto &bucket : sourceNameIndex_)
      std::sort(bucket.begin(), bucket.end(),
                [](const QString &a, const QString &b) {
                  return a.size() > b.size();
                });
}

QString CodeText::language() const {
  switch (dialectOf(representation_, pageLanguage_)) {
  case Dialect::C:
    return QStringLiteral("c");
  case Dialect::Cpp:
    return QStringLiteral("cpp");
  case Dialect::Rust:
    return QStringLiteral("rust");
  case Dialect::Go:
    return QStringLiteral("go");
  default:
    return {};
  }
}

int CodeText::sourceNameLength(const QString &text, int position) const {
  const auto bucket = sourceNameIndex_.constFind(text.at(position));
  if (bucket == sourceNameIndex_.cend())
    return 0;
  // A name starts and ends where identifiers do, so `core::fmt::write` is
  // not found inside `xcore::fmt::writer`.
  const auto joins = [](QChar c) {
    return identifierPart(c) || c == QLatin1Char(':') ||
           c == QLatin1Char('.') || c == QLatin1Char('/');
  };
  if (position > 0 && joins(text.at(position - 1)))
    return 0;
  for (const QString &name : *bucket) {
    const int end = position + int(name.size());
    if (end <= text.size() &&
        QStringView(text).mid(position, name.size()) == name &&
        (end == text.size() || !identifierPart(text.at(end))))
      return int(name.size());
  }
  return 0;
}

std::optional<std::pair<QString, CodeText::SourceName>>
CodeText::sourceNameAt(int line, int column) const {
  if (line < 0 || line >= lines_.size() || sourceNames_.isEmpty())
    return std::nullopt;
  const auto &styled = lines_[line].styled;
  for (const auto &span : styled.spans) {
    if (span.role != Function || column < span.start ||
        column >= span.start + span.length)
      continue;
    const QString spelled = styled.text.mid(span.start, span.length);
    if (const auto found = sourceNames_.constFind(spelled);
        found != sourceNames_.cend())
      return std::pair{spelled, *found};
  }
  return std::nullopt;
}

QJsonArray CodeText::foldRegions() const {
  QJsonArray regions = regions_;
  // The prelude folds to its summary line, the newline before the
  // definition kept.
  const qint64 lines = prelude_.value("lines").toInteger(-1);
  const qint64 end = prelude_.value("end_byte").toInteger(-1);
  const bool llvm = dialectOf(representation_, pageLanguage_) == Dialect::LLVM;
  if (lines > 0 && end > byteOffset_ + 1)
    regions.append(QJsonObject{
        {"id", PreludeRegion},
        {"kind", PreludeRegion},
        {"display_name",
         llvm ? tr("%n lines of declarations", nullptr, int(lines))
              : tr("%n lines of includes and declarations", nullptr,
                   int(lines))},
        {"foldable", true},
        {"mapping_status", "mapped"},
        {"spans", QJsonArray{QJsonObject{{"begin_byte", byteOffset_},
                                         {"end_byte", end - 1}}}}});
  // The epilogue folds to its summary line too, the final newline kept.
  const qint64 tail = epilogue_.value("lines").toInteger(-1);
  const qint64 begin = epilogue_.value("begin_byte").toInteger(-1);
  const qint64 last = byteOffset_ + sourceBytes_ - 1;
  if (tail > 0 && begin >= byteOffset_ && begin < last)
    regions.append(QJsonObject{
        {"id", EpilogueRegion},
        {"kind", EpilogueRegion},
        {"display_name",
         tr("%n lines of attributes and metadata", nullptr, int(tail))},
        {"foldable", true},
        {"mapping_status", "mapped"},
        {"spans",
         QJsonArray{QJsonObject{{"begin_byte", begin}, {"end_byte", last}}}}});
  return regions;
}

void CodeText::rebuildLines(bool append) {
  const bool folding = !loading_ && library_.canFold();
  const QString display = folding ? library_.text() : source_;
  const QVariantList &rows = folding ? library_.mappings() : sourceRows_;
  const int previousLine = cursorLine_;
  if (!append) {
    lines_.clear();
    lineStarts_.clear();
    inComment_ = false;
    renderedChars_ = renderedRows_ = widestLine_ = 0;
  }
  int start = renderedChars_;
  const bool addressLabels = labelsNameAddresses();
  while (start < display.size()) {
    int end = int(display.indexOf(QLatin1Char('\n'), start));
    if (end < 0)
      end = int(display.size());
    Line line;
    line.styled.text = display.mid(start, end - start);
    // A label's row is the block it labels, though the rows the engine maps
    // may give it the instruction of a statement around it.
    if (addressLabels)
      if (const auto label = labelDefinition(line.styled.text)) {
        line.label = true;
        line.addresses = {label->second};
      }
    widestLine_ = std::max(widestLine_, int(line.styled.text.size()));
    highlightLine(line, inComment_);
    lines_.append(std::move(line));
    lineStarts_.append(start);
    start = end + 1;
  }
  for (int i = renderedRows_; i < rows.size(); ++i) {
    const auto row = rows[i].toMap();
    const int index = row.value(QStringLiteral("line"), -1).toInt();
    if (index < 0 || index >= lines_.size() || lines_[index].label)
      continue;
    for (const auto &address : row.value(QStringLiteral("addresses")).toList())
      if (const auto parsed = parseAddress(address.toString()))
        lines_[index].addresses.append(*parsed);
  }
  renderedChars_ = display.size();
  renderedRows_ = rows.size();
  for (int i = 0; i < lines_.size(); ++i) {
    lines_[i].sourceLine = folding ? library_.sourceLineAt(lineStarts_[i]) : i;
    lines_[i].block.clear();
    lines_[i].blockFolded = false;
  }
  if (folding)
    for (const auto &block : library_.blocks())
      if (block.line >= 0 && block.line < lines_.size()) {
        lines_[block.line].block = block.id;
        lines_[block.line].blockFolded = block.folded;
      }
  if (folding)
    for (const auto &[begin, end] : library_.foldedRanges()) {
      auto it =
          std::upper_bound(lineStarts_.cbegin(), lineStarts_.cend(), begin);
      const int index = int(it - lineStarts_.cbegin()) - 1;
      if (index < 0 || index >= lines_.size())
        continue;
      auto &line = lines_[index];
      line.folded = true;
      const int column = begin - lineStarts_[index];
      const int length =
          std::min(end, int(lineStarts_[index] + line.styled.text.size())) -
          begin;
      // The summary reads as a comment over the fold tint.
      QVector<StyledSpan> spans;
      for (const auto &span : line.styled.spans)
        if (span.start + span.length <= column || span.start >= column + length)
          spans.append(span);
      spans.append({column, length, Comment, std::nullopt});
      std::sort(spans.begin(), spans.end(),
                [](const StyledSpan &a, const StyledSpan &b) {
                  return a.start < b.start;
                });
      line.styled.spans = spans;
    }
  const int lastSourceLine =
      lines_.isEmpty() ? 1 : lines_.back().sourceLine + 1;
  gutterChars_ = std::max(3, int(QString::number(lastSourceLine).size()));
  cursorLine_ =
      std::clamp(previousLine, 0, std::max(0, int(lines_.size()) - 1));
  updateRange();
  updateMouseCursor(viewport()->mapFromGlobal(QCursor::pos()));
  viewport()->update();
}

int CodeText::displayPosition(int line, int column) const {
  if (line < 0 || line >= lineStarts_.size())
    return 0;
  return lineStarts_[line] + column;
}

QString CodeText::regionAt(const QPoint &position) const {
  if (!library_.anyFolded())
    return {};
  const int line = lineAt(position.y());
  if (line < 0 || line >= lines_.size() || !lines_[line].folded)
    return {};
  return library_.regionAt(displayPosition(line, columnAt(line, position.x())));
}

QString CodeText::blockAt(const QPoint &position) const {
  const int line = lineAt(position.y());
  const int left = Margin + int((gutterChars_ + 0.5) * charWidth_);
  const int right = Margin + int((gutterChars_ + 2) * charWidth_);
  return line >= 0 && line < lines_.size() && position.x() >= left &&
                 position.x() < right
             ? lines_[line].block
             : QString();
}

void CodeText::updateMouseCursor(const QPoint &position) {
  const bool clickable =
      !loading_ && !selecting_ && viewport()->rect().contains(position) &&
      (!blockAt(position).isEmpty() || !regionAt(position).isEmpty());
  const auto shape = clickable ? Qt::PointingHandCursor : Qt::IBeamCursor;
  if (viewport()->cursor().shape() != shape)
    viewport()->setCursor(shape);
}

bool CodeText::viewportEvent(QEvent *event) {
  if (event->type() == QEvent::Enter)
    updateMouseCursor(viewport()->mapFromGlobal(QCursor::pos()));
  else if (event->type() == QEvent::Leave)
    viewport()->setCursor(Qt::IBeamCursor);
  return QAbstractScrollArea::viewportEvent(event);
}

void CodeText::setRegionFolded(const QString &id, bool folded) {
  if (!library_.hasRegion(id) || library_.isFolded(id) == folded)
    return;
  const int cursor =
      library_.originalPosition(displayPosition(cursorLine_, cursorColumn_));
  const auto anchor =
      anchor_ ? std::optional<int>(library_.originalPosition(
                    displayPosition(anchor_->first, anchor_->second)))
              : std::nullopt;
  const int top = library_.originalPosition(
      displayPosition(verticalScrollBar()->value(), 0));
  library_.setRegionFolded(id, folded);
  rebuildLines();
  auto projected = [this](int original) {
    const int position = library_.projectedPosition(original);
    const auto next =
        std::upper_bound(lineStarts_.cbegin(), lineStarts_.cend(), position);
    const int line = std::max(0, int(next - lineStarts_.cbegin()) - 1);
    return std::pair{line, std::clamp(position - lineStarts_[line], 0,
                                      int(lines_[line].styled.text.size()))};
  };
  if (!lines_.isEmpty()) {
    const auto at = projected(cursor);
    cursorLine_ = at.first;
    cursorColumn_ = at.second;
    anchor_ = anchor ? std::optional(projected(*anchor)) : std::nullopt;
    verticalScrollBar()->setValue(projected(top).first);
  }
  selectedAddress_.reset();
  marked_.clear();
  viewport()->update();
  emit foldingChanged();
}

bool CodeText::setCurrentBlockFolded(bool folded) {
  if (loading_ || cursorLine_ < 0 || cursorLine_ >= lines_.size())
    return false;
  const auto id =
      !lines_[cursorLine_].block.isEmpty()
          ? lines_[cursorLine_].block
          : library_.blockAt(displayPosition(cursorLine_, cursorColumn_));
  if (id.isEmpty())
    return false;
  setRegionFolded(id, folded);
  return true;
}

void CodeText::setFolded(bool folded) {
  library_.setFolded(folded);
  rebuildLines();
  emit foldingChanged();
}

bool CodeText::event(QEvent *event) {
  // The text keeps Ctrl+A for itself, as IDA's text panes do, whatever a
  // window-wide command takes the key for.
  if (event->type() == QEvent::ShortcutOverride &&
      static_cast<QKeyEvent *>(event)->matches(QKeySequence::SelectAll)) {
    event->accept();
    return true;
  }
  // A window switch or popup during a drag takes the release with it.
  if (event->type() == QEvent::FocusOut || event->type() == QEvent::Hide) {
    dragScroll_.stop();
    selecting_ = false;
  }
  if (event->type() == QEvent::ToolTip) {
    auto *help = static_cast<QHelpEvent *>(event);
    const QPoint local = viewport()->mapFrom(this, help->pos());
    if (const QString id = blockAt(local); !id.isEmpty()) {
      QToolTip::showText(help->globalPos(),
                         library_.isFolded(id)
                             ? tr("Expand block (Ctrl+Shift+])")
                             : tr("Collapse block (Ctrl+Shift+[)"),
                         this);
      return true;
    }
    if (const QString id = regionAt(local); !id.isEmpty()) {
      for (const auto &value : library_.regions()) {
        const auto region = value.toMap();
        if (region.value(QStringLiteral("id")).toString() != id)
          continue;
        const bool llvm =
            dialectOf(representation_, pageLanguage_) == Dialect::LLVM;
        QToolTip::showText(
            help->globalPos(),
            id == QLatin1String(PreludeRegion) && llvm
                ? tr("The target, types, globals and functions this function "
                     "refers to; click to show them, Keypad - to fold them "
                     "again.")
            : id == QLatin1String(PreludeRegion)
                ? tr("The includes, support types and declarations this code "
                     "compiles with; click to show them, Keypad - to fold "
                     "them again.")
            : id == QLatin1String(EpilogueRegion)
                ? tr("The attribute groups and metadata this function uses; "
                     "click to show them, Keypad - to fold them again.")
            : region.value(QStringLiteral("kind")) == "source-block"
                ? tr("Expand block (Ctrl+Shift+])")
                : tr("%1\nRecognized library operation; click to show its "
                     "code.")
                      .arg(region.value(QStringLiteral("display_name"))
                               .toString()),
            this);
        return true;
      }
    }
    // A source name shows the C identifier and the symbol it reads.
    if (const int line = lineAt(local.y()); line >= 0 && line < lines_.size())
      if (const auto name = sourceNameAt(line, columnAt(line, local.x()))) {
        QString text = tr("C: %1").arg(name->second.identifier);
        if (!name->second.symbol.isEmpty() &&
            name->second.symbol != name->first)
          text += QLatin1Char('\n') + name->second.symbol;
        QToolTip::showText(help->globalPos(), text, this);
        return true;
      }
    // A global of LLVM IR says what it is and where it goes.
    if (const int line = lineAt(local.y()); line >= 0 && line < lines_.size())
      if (const QString text = globalHoverText(line, columnAt(line, local.x()));
          !text.isEmpty()) {
        QToolTip::showText(help->globalPos(), text, this);
        return true;
      }
    // A type or macro the code declares shows its declaration.
    if (const int line = lineAt(local.y()); line >= 0 && line < lines_.size())
      if (const QString token = tokenAt(line, columnAt(line, local.x()));
          !token.isEmpty())
        if (const auto declared = declarationLine(token)) {
          const auto text = QStringView(source_)
                                .split(QLatin1Char('\n'))
                                .at(*declared)
                                .trimmed();
          // A global of LLVM IR goes first to what the binary names so.
          const bool global =
              dialectOf(representation_, pageLanguage_) == Dialect::LLVM &&
              token.startsWith(QLatin1Char('@'));
          QToolTip::showText(
              help->globalPos(),
              global ? text.toString()
                     : tr("%1\nDouble-click to go to the declaration.")
                           .arg(text.toString()),
              this);
          return true;
        } else if (const auto linked = declarations().linked.constFind(token);
                   linked != declarations().linked.cend()) {
          // Its declaration names the symbol and the demangled signature.
          QToolTip::showText(help->globalPos(),
                             QStringView(source_)
                                 .split(QLatin1Char('\n'))
                                 .at(linked->first)
                                 .trimmed()
                                 .toString(),
                             this);
          return true;
        }
    QToolTip::hideText();
    event->ignore();
    return true;
  }
  return QAbstractScrollArea::event(event);
}

void CodeText::highlightLine(Line &line, bool &inComment) const {
  const Dialect dialect = dialectOf(representation_, pageLanguage_);
  const bool source = sourceDialect(dialect);
  const QString &s = line.styled.text;
  auto &spans = line.styled.spans;
  spans.clear();
  const auto add = [&](int start, int end, int role) {
    if (end > start)
      spans.append({start, end - start, role, std::nullopt});
  };
  int i = 0;
  const int n = int(s.size());
  if (source && !inComment) {
    int first = 0;
    while (first < n && s.at(first).isSpace())
      ++first;
    if (first < n && s.at(first) == QLatin1Char('#')) {
      add(first, n, Preprocessor);
      return;
    }
  }
  while (i < n) {
    const QChar c = s.at(i);
    if (inComment) {
      const int end = int(s.indexOf(QStringLiteral("*/"), i));
      if (end < 0) {
        add(i, n, Comment);
        return;
      }
      add(i, end + 2, Comment);
      i = end + 2;
      inComment = false;
      continue;
    }
    if (source && c == QLatin1Char('/') && i + 1 < n &&
        s.at(i + 1) == QLatin1Char('/')) {
      add(i, n, Comment);
      return;
    }
    if (source && c == QLatin1Char('/') && i + 1 < n &&
        s.at(i + 1) == QLatin1Char('*')) {
      inComment = true;
      continue;
    }
    if (!source && c == QLatin1Char(';')) {
      add(i, n, Comment);
      return;
    }
    // A Rust label, `'switch1`, is not a character literal.
    if (dialect == Dialect::Rust && c == QLatin1Char('\'') && i + 1 < n &&
        identifierStart(s.at(i + 1))) {
      int j = i + 2;
      while (j < n && identifierPart(s.at(j)))
        ++j;
      if (j >= n || s.at(j) != QLatin1Char('\'')) {
        add(i, j, Constant);
        i = j;
        continue;
      }
    }
    // A source name, `core::fmt::write`, is one name.
    if (!sourceNames_.isEmpty() && source) {
      if (const int length = sourceNameLength(s, i)) {
        const auto name = sourceNames_.value(s.mid(i, length));
        spans.append({i, length, Function, name.address});
        i += length;
        continue;
      }
    }
    if (c == QLatin1Char('"') || (source && c == QLatin1Char('\''))) {
      int j = i + 1;
      while (j < n && s.at(j) != c) {
        if (s.at(j) == QLatin1Char('\\'))
          ++j;
        ++j;
      }
      add(i, std::min(n, j + 1), String);
      i = std::min(n, j + 1);
      continue;
    }
    if (c.isDigit()) {
      int j = i + 1;
      while (j < n &&
             (s.at(j).isLetterOrNumber() || s.at(j) == QLatin1Char('.') ||
              s.at(j) == QLatin1Char('_')))
        ++j;
      add(i, j, Number);
      i = j;
      continue;
    }
    const bool sigil = dialect == Dialect::LLVM &&
                       (c == QLatin1Char('%') || c == QLatin1Char('@'));
    if (identifierStart(c) || sigil) {
      int j = i + 1;
      while (j < n && (identifierPart(s.at(j)) || s.at(j) == QLatin1Char('.')))
        ++j;
      const QString word = s.mid(i, j - i);
      int k = j;
      while (k < n && s.at(k) == QLatin1Char(' '))
        ++k;
      const bool call = k < n && s.at(k) == QLatin1Char('(');
      int role = Variable;
      if (source) {
        const auto &words = vocabularyOf(dialect);
        if (words.control.contains(word))
          role = Control;
        else if (words.types.contains(word) || widthType(dialect, word) ||
                 ((dialect == Dialect::C || dialect == Dialect::Cpp) &&
                  word.endsWith(QStringLiteral("_t"))))
          role = Type;
        else if (words.keywords.contains(word))
          role = Keyword;
        else if (call)
          role = Function;
        else if (word.size() > 1 && word == word.toUpper() &&
                 word.at(0).isLetter())
          role = Constant;
      } else if (dialect == Dialect::LLVM) {
        if (c == QLatin1Char('@'))
          role = Function;
        else if (c == QLatin1Char('%'))
          role = Variable;
        else if (llvmWords().contains(word))
          role = Keyword;
        else if (word.size() > 1 && word.at(0) == QLatin1Char('i') &&
                 word.mid(1).toInt() > 0)
          role = Type;
        else
          role = Default;
      } else {
        // Textual IR: OPCODES in capitals, labels end with a colon.
        if (word == word.toUpper() && word.size() > 1)
          role = Keyword;
        else if (k < n && s.at(k) == QLatin1Char(':'))
          role = Function;
        else
          role = Variable;
      }
      add(i, j, role);
      i = j;
      continue;
    }
    if (!c.isSpace())
      add(i, i + 1, Punctuation);
    ++i;
  }
}

int CodeText::textLeft() const {
  return Margin + int((gutterChars_ + 2) * charWidth_) -
         horizontalScrollBar()->value();
}

int CodeText::lineAt(int y) const {
  return verticalScrollBar()->value() + y / std::max(1, lineHeight_);
}

int CodeText::columnAt(int line, int x) const {
  if (line < 0 || line >= lines_.size())
    return 0;
  auto &layout = lines_[line].styled.layout(Theme::instance().codeFont(),
                                            styleStamp_, codeColor);
  return layout.lineCount() ? layout.lineAt(0).xToCursor(x - textLeft()) : 0;
}

void CodeText::paintEvent(QPaintEvent *) {
  QPainter painter(viewport());
  const auto &theme = Theme::instance();
  const QRect area = viewport()->rect();
  painter.fillRect(area, theme.color(ColorRole::CodeBackground));
  if (lines_.isEmpty())
    return;
  const QFont font = theme.codeFont();
  painter.setFont(font);
  const int first = verticalScrollBar()->value();
  const int last = std::min(int(lines_.size()) - 1, first + visibleLines());
  const int left = textLeft();
  const auto cursor = std::pair{cursorLine_, cursorColumn_};
  const auto selectionFirst = anchor_ ? std::min(*anchor_, cursor) : cursor;
  const auto selectionLast = anchor_ ? std::max(*anchor_, cursor) : cursor;
  for (int i = first; i <= last; ++i) {
    const int y = (i - first) * lineHeight_;
    const QRect row(0, y, area.width(), lineHeight_);
    if (i == cursorLine_)
      painter.fillRect(row, theme.color(ColorRole::ListingCurrentLine));
    else if (marked_.contains(i) || lines_[i].folded)
      painter.fillRect(row, theme.color(ColorRole::CodeFold));
    const int gutterRight = Margin + int((gutterChars_ + 2) * charWidth_);
    painter.save();
    painter.setClipRect(QRect(
        gutterRight, y, std::max(0, area.width() - gutterRight), lineHeight_));
    auto &layout = lines_[i].styled.layout(font, styleStamp_, codeColor);
    if (!highlight_.isEmpty() && layout.lineCount()) {
      const QTextLine textLine = layout.lineAt(0);
      for (const int at : tokenOccurrences(lines_[i].styled.text, highlight_)) {
        const qreal x0 = textLine.cursorToX(at);
        const qreal x1 = textLine.cursorToX(at + int(highlight_.size()));
        painter.fillRect(QRectF(left + x0, y, x1 - x0, lineHeight_),
                         theme.color(ColorRole::ListingHighlight));
      }
    }
    QList<QTextLayout::FormatRange> selection;
    if (anchor_ && selectionFirst != selectionLast &&
        i >= selectionFirst.first && i <= selectionLast.first) {
      QTextLayout::FormatRange range;
      range.start = i == selectionFirst.first ? selectionFirst.second : 0;
      const int end = i == selectionLast.first
                          ? selectionLast.second
                          : int(lines_[i].styled.text.size());
      range.length = std::max(0, end - range.start);
      range.format.setBackground(theme.color(ColorRole::ListingSelection));
      selection.append(range);
    }
    layout.draw(&painter, QPointF(left, y), selection);
    if (i == cursorLine_ && hasFocus() && layout.lineCount()) {
      const qreal x = left + layout.lineAt(0).cursorToX(cursorColumn_);
      painter.fillRect(QRectF(x, y + 1, 2, lineHeight_ - 2),
                       theme.color(ColorRole::ListingCursor));
    }
    painter.restore();
    painter.setPen(theme.color(ColorRole::CodeLineNumber));
    painter.drawText(
        QRect(Margin, y, int(gutterChars_ * charWidth_), lineHeight_),
        Qt::AlignRight | Qt::AlignVCenter,
        QString::number(lines_[i].sourceLine + 1));
    if (!lines_[i].block.isEmpty()) {
      const qreal x = Margin + (gutterChars_ + 1.25) * charWidth_;
      const qreal middle = y + lineHeight_ / 2.0;
      const qreal radius = std::min(charWidth_ * 0.35, lineHeight_ * 0.22);
      painter.save();
      painter.setRenderHint(QPainter::Antialiasing);
      painter.setPen(QPen(theme.color(ColorRole::CodeLineNumber), 1.3));
      if (lines_[i].blockFolded)
        painter.drawPolyline(QPolygonF{{x - radius / 2, middle - radius},
                                       {x + radius / 2, middle},
                                       {x - radius / 2, middle + radius}});
      else
        painter.drawPolyline(QPolygonF{{x - radius, middle - radius / 2},
                                       {x, middle + radius / 2},
                                       {x + radius, middle - radius / 2}});
      painter.restore();
    }
  }
}

void CodeText::moveCursor(int line, int column, bool extend) {
  selectAllAfterLoad_ = false;
  pendingAddress_.reset();
  selectedAddress_.reset();
  if (lines_.isEmpty())
    return;
  if (extend && !anchor_)
    anchor_ = std::pair{cursorLine_, cursorColumn_};
  else if (!extend)
    anchor_.reset();
  cursorLine_ = std::clamp(line, 0, int(lines_.size()) - 1);
  cursorColumn_ =
      std::clamp(column, 0, int(lines_[cursorLine_].styled.text.size()));
  auto *bar = verticalScrollBar();
  if (cursorLine_ < bar->value())
    bar->setValue(cursorLine_);
  else if (cursorLine_ >= bar->value() + visibleLines())
    bar->setValue(cursorLine_ - visibleLines() + 1);
  // A move of the code's own leaves the rows the disassembly marked, and
  // takes the disassembly along.
  if (!following_) {
    marked_.clear();
    if (const auto address = currentAddress())
      emit locationChanged(*address);
  }
  viewport()->update();
}

std::optional<Address> CodeText::currentAddress() const {
  if (cursorLine_ < 0 || cursorLine_ >= lines_.size() ||
      lines_[cursorLine_].addresses.isEmpty())
    return std::nullopt;
  if (selectedAddress_ &&
      lines_[cursorLine_].addresses.contains(*selectedAddress_))
    return selectedAddress_;
  return lines_[cursorLine_].addresses.front();
}

void CodeText::selectAddress(Address address) {
  if (loading_) {
    pendingAddress_ = address;
    return;
  }
  // Unfold the mapped original row, rather than selecting a summary whose
  // instruction addresses belong to several hidden source rows.
  for (const auto &value : sourceRows_) {
    const auto row = value.toMap();
    const auto addresses = row.value(QStringLiteral("addresses")).toList();
    if (std::none_of(addresses.cbegin(), addresses.cend(),
                     [address](const QVariant &value) {
                       return parseAddress(value.toString()) == address;
                     }))
      continue;
    const int sourceLine = row.value(QStringLiteral("line"), -1).toInt();
    if (library_.unfoldSourceLine(sourceLine)) {
      rebuildLines();
      emit foldingChanged();
    }
    break;
  }
  QVector<int> rows;
  for (int i = 0; i < lines_.size(); ++i)
    if (lines_[i].addresses.contains(address))
      rows.append(i);
  if (!rows.isEmpty()) {
    // The disassembly stays on the instruction the user selected there,
    // though the row's first mapping may be another one.
    const QScopedValueRollback<bool> following(following_, true);
    moveCursor(rows.front(), 0, false);
    // Preserve an assembly address that is one of several on this row, so
    // a Tab round trip returns to the instruction the user selected.
    selectedAddress_ = address;
  }
  marked_ = rows;
  viewport()->update();
  emit addressSelected(address, !marked_.isEmpty());
}

void CodeText::revealAddress(Address address) {
  if (loading_ || lines_.isEmpty())
    return;
  QVector<int> rows;
  for (int i = 0; i < lines_.size(); ++i)
    if (lines_[i].addresses.contains(address))
      rows.append(i);
  // A row already under the cursor keeps it where the user put it.
  if (!rows.isEmpty() && !rows.contains(cursorLine_)) {
    const QScopedValueRollback<bool> following(following_, true);
    moveCursor(rows.front(), 0, false);
  }
  if (!rows.isEmpty())
    selectedAddress_ = address;
  marked_ = rows;
  viewport()->update();
}

QString CodeText::currentToken() const {
  if (cursorLine_ < 0 || cursorLine_ >= lines_.size())
    return {};
  return lines_[cursorLine_].styled.tokenAt(cursorColumn_);
}

std::optional<QJsonObject> CodeText::renameTarget() const {
  if (!function_ || loading_)
    return std::nullopt;
  const auto display = displayPosition(cursorLine_, cursorColumn_);
  if (!library_.regionAt(display).isEmpty())
    return std::nullopt;
  const auto position =
      library_.canFold() ? library_.originalPosition(display) : display;
  for (const auto &name : editNames_)
    if (position >= name.begin && position < name.end)
      return name.target;
  if (editMetadata_)
    return std::nullopt;
  const auto token = currentToken();
  if (const auto at = objectAddress(token))
    return QJsonObject{{"address", hexAddress(*at)}, {"name", token}};
  if (const auto name = sourceNameAt(cursorLine_, cursorColumn_)) {
    if (name->second.address)
      return QJsonObject{{"address", hexAddress(*name->second.address)},
                         {"name", token}};
    return QJsonObject{{"query", name->second.symbol}, {"name", token}};
  }
  if (const auto linked = linkedSymbol(token))
    return QJsonObject{{"query", *linked}, {"name", token}};
  return std::nullopt;
}

namespace {
bool sameLocalTarget(const QJsonObject &a, const QJsonObject &b) {
  return a.value("kind").toString() == QLatin1String("local") &&
         b.value("kind").toString() == QLatin1String("local") &&
         !b.value("original").toString().isEmpty() &&
         !b.value("identity").toString().isEmpty() &&
         a.value("original") == b.value("original") &&
         a.value("identity") == b.value("identity");
}
} // namespace

QVector<CodeText::LocalReference>
CodeText::localReferences(const QJsonObject &target) const {
  QVector<LocalReference> references;
  if (!function_ || loading_ || !sameLocalTarget(target, target))
    return references;
  QVector<int> starts{0};
  for (int i = 0; i + 1 < source_.size(); ++i)
    if (source_.at(i) == QLatin1Char('\n'))
      starts.append(i + 1);
  for (const auto &name : editNames_) {
    if (!sameLocalTarget(name.target, target))
      continue;
    const int line =
        int(std::upper_bound(starts.cbegin(), starts.cend(), name.begin) -
            starts.cbegin()) -
        1;
    const int end =
        line + 1 < starts.size() ? starts[line + 1] - 1 : int(source_.size());
    references.append({line, name.begin - starts[line], name.begin, name.end,
                       source_.mid(starts[line], end - starts[line])});
  }
  std::sort(references.begin(), references.end(),
            [](const LocalReference &a, const LocalReference &b) {
              return a.begin < b.begin;
            });
  return references;
}

bool CodeText::selectLocalReference(const QJsonObject &target,
                                    const LocalReference &reference) {
  if (!function_ || loading_ ||
      std::none_of(
          editNames_.cbegin(), editNames_.cend(), [&](const EditName &name) {
            return name.begin == reference.begin && name.end == reference.end &&
                   sameLocalTarget(name.target, target);
          }))
    return false;
  if (library_.unfoldSourceLine(reference.line)) {
    rebuildLines();
    emit foldingChanged();
  }
  const int line = library_.canFold() ? library_.displayLine(reference.line)
                                      : reference.line;
  if (line < 0 || line >= lines_.size())
    return false;
  const int endColumn = reference.column + reference.end - reference.begin;
  marked_.clear();
  highlight_ = source_.mid(reference.begin, reference.end - reference.begin);
  // Leave the cursor inside the name so X/N can act on it again. The anchor
  // at its end still selects exactly the referenced identifier for copying.
  moveCursor(line, endColumn, false);
  moveCursor(line, reference.column, true);
  auto &layout = lines_[line].styled.layout(Theme::instance().codeFont(),
                                            styleStamp_, codeColor);
  if (layout.lineCount()) {
    const qreal left =
        textLeft() + layout.lineAt(0).cursorToX(reference.column);
    const qreal right = textLeft() + layout.lineAt(0).cursorToX(endColumn);
    const qreal margin = textLeft() + horizontalScrollBar()->value();
    auto *across = horizontalScrollBar();
    if (left < margin)
      across->setValue(across->value() - int(std::ceil(margin - left)));
    else if (right > viewport()->width())
      across->setValue(across->value() +
                       int(std::ceil(right - viewport()->width())));
  }
  viewport()->update();
  return true;
}

std::optional<QJsonObject> CodeText::commentTarget() const {
  if (!function_ || loading_ || lines_.isEmpty())
    return std::nullopt;
  const auto position = displayPosition(cursorLine_, 0);
  if (library_.regionAt(position) == QLatin1String(PreludeRegion))
    return QJsonObject{{"mapped_address", hexAddress(*function_)}};
  if (!library_.regionAt(position).isEmpty())
    return std::nullopt;
  const int line =
      library_.canFold() ? library_.sourceLineAt(position) : cursorLine_;
  for (const auto &value : sourceRows_) {
    const auto row = value.toMap();
    if (row.value("line").toInt() != line || !row.contains("code_anchor"))
      continue;
    QJsonObject target{{"line", line},
                       {"anchor", row.value("code_anchor").toString()},
                       {"text", row.value("code_comment").toString()}};
    // A note saved before the engine could map this row retains its source
    // identity; adding an address mapping must not hide subsequent edits. A
    // label's row maps to its block only for navigation: the worker shows an
    // instruction's comment on the rows the engine maps, so a note on the
    // label stays the line's.
    if (!row.value("code_comment_is_source").toBool() &&
        !lines_[cursorLine_].label)
      if (const auto at = currentAddress())
        target["mapped_address"] = hexAddress(*at);
    return target;
  }
  return std::nullopt;
}

std::optional<CodeText::NumberTarget> CodeText::numberTarget() const {
  // The worker edits the source views it decorates.
  if (!function_ || loading_ || lines_.isEmpty() || !editMetadata_ ||
      !CodeView::isSource(representation_) || cursorLine_ >= lines_.size())
    return std::nullopt;
  const auto display = displayPosition(cursorLine_, cursorColumn_);
  if (!library_.regionAt(display).isEmpty())
    return std::nullopt;
  const int position =
      library_.canFold() ? library_.originalPosition(display) : display;
  const int start =
      position > 0
          ? int(source_.lastIndexOf(QLatin1Char('\n'), position - 1)) + 1
          : 0;
  const int line = int(QStringView(source_).left(start).count(u'\n'));
  QVariantMap row;
  for (const auto &value : sourceRows_)
    if (auto candidate = value.toMap();
        candidate.value("line").toInt() == line &&
        candidate.contains("code_anchor")) {
      row = std::move(candidate);
      break;
    }
  if (row.isEmpty())
    return std::nullopt;
  NumberTarget target;
  target.line = line;
  target.anchor = row.value("code_anchor").toString();
  const QString language = this->language();
  target.syntax = language == QLatin1String("rust") ? LiteralSyntax::Rust
                  : language == QLatin1String("go") ? LiteralSyntax::Go
                                                    : LiteralSyntax::C;
  // A block comment carries over lines; the lines before say whether this
  // one starts in one.
  bool inComment = false;
  for (int at = 0; at < start;) {
    const int end = int(source_.indexOf(QLatin1Char('\n'), at));
    if (end < 0 || end >= start)
      break;
    (void)integerLiterals(source_.mid(at, end - at).toStdString(),
                          target.syntax, inComment);
    at = end + 1;
  }
  bool anchorInComment = inComment;
  int end = int(source_.indexOf(QLatin1Char('\n'), start));
  if (end < 0)
    end = int(source_.size());
  const auto shown = integerLiterals(
      source_.mid(start, end - start).toStdString(), target.syntax, inComment);
  // The engine's own spelling, which the worker spells the other bases from;
  // the shown line counts the same literals.
  const auto written = integerLiterals(target.anchor.toStdString(),
                                       target.syntax, anchorInComment);
  // The literal under the cursor, or the one it just passed.
  const auto column =
      std::size_t(source_.mid(start, position - start).toUtf8().size());
  for (std::size_t i = 0; i < shown.size() && i < written.size(); ++i) {
    if (column < shown[i].Begin || column > shown[i].End ||
        (column == shown[i].End && i + 1 < shown.size() &&
         shown[i + 1].Begin == column))
      continue;
    target.index = int(i);
    target.literal = written[i];
    target.shown = shown[i].Radix;
    const auto radix = row.value("code_number_radixes")
                           .toMap()
                           .value(QString::number(i))
                           .toString();
    target.chosen =
        literalRadixNamed(radix.toStdString()).value_or(LiteralRadix::Number);
    return target;
  }
  return std::nullopt;
}

QString CodeText::selectedText() const {
  if (lines_.isEmpty())
    return {};
  const auto cursor = std::pair{cursorLine_, cursorColumn_};
  const bool selected = anchor_ && *anchor_ != cursor;
  const auto first =
      selected ? std::min(*anchor_, cursor) : std::pair{cursorLine_, 0};
  const auto last =
      selected
          ? std::max(*anchor_, cursor)
          : std::pair{cursorLine_, int(lines_[cursorLine_].styled.text.size())};
  // Folded summaries copy as the code they stand for.
  if (library_.anyFolded()) {
    const int begin = displayPosition(first.first, first.second);
    const int end = displayPosition(last.first, last.second);
    return library_.originalSelection(begin, end);
  }
  QStringList text;
  for (int i = first.first; i <= last.first && i < lines_.size(); ++i) {
    const int begin = i == first.first ? first.second : 0;
    const int end =
        i == last.first ? last.second : int(lines_[i].styled.text.size());
    text.append(lines_[i].styled.text.mid(begin, end - begin));
  }
  return text.join(QLatin1Char('\n'));
}

QString CodeText::allText() const {
  QString text = source_;
  if (text.endsWith(QLatin1Char('\n')))
    text.chop(1);
  return text;
}

bool CodeText::findText(const QString &text, bool forward) {
  if (text.isEmpty())
    return false;
  QVector<int> starts{0};
  for (int i = 0; i + 1 < source_.size(); ++i)
    if (source_[i] == '\n')
      starts.append(i + 1);
  // Search from the row after the cursor's. A summary standing for whole
  // rows, such as the prelude or a library operation, is the cursor's row;
  // the hidden body of a block follows its header.
  int current = cursorLine_;
  if (!loading_ && library_.canFold() && cursorLine_ < lines_.size())
    current = library_.sourceLineAt(displayPosition(
        cursorLine_,
        forward ? int(lines_[cursorLine_].styled.text.size()) : 0));
  for (int step = 1; step <= starts.size(); ++step) {
    const int index = forward ? current + step : current - step;
    if (index < 0 || index >= starts.size())
      break;
    const int end =
        index + 1 < starts.size() ? starts[index + 1] : int(source_.size());
    const int column = int(source_.mid(starts[index], end - starts[index])
                               .indexOf(text, 0, Qt::CaseInsensitive));
    if (column >= 0) {
      if (!loading_ && library_.unfoldSourceLine(index)) {
        rebuildLines();
        emit foldingChanged();
      }
      const int position =
          !loading_ && library_.canFold()
              ? library_.projectedPosition(starts[index] + column)
              : starts[index] + column;
      const auto next =
          std::upper_bound(lineStarts_.cbegin(), lineStarts_.cend(), position);
      const int line = std::max(0, int(next - lineStarts_.cbegin()) - 1);
      if (line >= lineStarts_.size())
        return false;
      highlight_ = text;
      moveCursor(line, position - lineStarts_[line], false);
      return true;
    }
  }
  return false;
}

void CodeText::keyPressEvent(QKeyEvent *event) {
  const bool shift = event->modifiers() & Qt::ShiftModifier;
  switch (event->key()) {
  case Qt::Key_Up:
    moveCursor(cursorLine_ - 1, cursorColumn_, shift);
    return;
  case Qt::Key_Down:
    moveCursor(cursorLine_ + 1, cursorColumn_, shift);
    return;
  case Qt::Key_PageUp:
    moveCursor(cursorLine_ - visibleLines() + 1, cursorColumn_, shift);
    return;
  case Qt::Key_PageDown:
    moveCursor(cursorLine_ + visibleLines() - 1, cursorColumn_, shift);
    return;
  case Qt::Key_Left:
    moveCursor(cursorLine_, cursorColumn_ - 1, shift);
    return;
  case Qt::Key_Right:
    moveCursor(cursorLine_, cursorColumn_ + 1, shift);
    return;
  case Qt::Key_Home:
    moveCursor((event->modifiers() & Qt::ControlModifier) ? 0 : cursorLine_, 0,
               shift);
    return;
  case Qt::Key_End:
    if ((event->modifiers() & Qt::ControlModifier) && !lines_.isEmpty())
      moveCursor(int(lines_.size()) - 1, int(lines_.back().styled.text.size()),
                 shift);
    else if (cursorLine_ < lines_.size())
      moveCursor(cursorLine_, int(lines_[cursorLine_].styled.text.size()),
                 shift);
    return;
  default:
    break;
  }
  if (event->matches(QKeySequence::Copy)) {
    QApplication::clipboard()->setText(selectedText());
    return;
  }
  if (event->matches(QKeySequence::SelectAll)) {
    selectAll();
    return;
  }
  const auto foldModifiers = Qt::ControlModifier | Qt::ShiftModifier;
  if ((event->modifiers() & foldModifiers) == foldModifiers) {
    if (event->key() == Qt::Key_BracketLeft ||
        event->key() == Qt::Key_BraceLeft) {
      setCurrentBlockFolded(true);
      return;
    }
    if (event->key() == Qt::Key_BracketRight ||
        event->key() == Qt::Key_BraceRight) {
      setCurrentBlockFolded(false);
      return;
    }
  }
  // Keypad +/- also fold source blocks, while retaining the prelude gestures.
  if (event->modifiers() & Qt::KeypadModifier) {
    if (event->key() == Qt::Key_Plus) {
      if (const QString id =
              library_.regionAt(displayPosition(cursorLine_, cursorColumn_));
          !id.isEmpty()) {
        setRegionFolded(id, false);
      } else
        setCurrentBlockFolded(false);
      return;
    }
    if (event->key() == Qt::Key_Minus) {
      if (setCurrentBlockFolded(true))
        return;
      if (hasPrelude() && !preludeFolded() &&
          cursorLine_ < prelude_.value("lines").toInteger(0))
        setPreludeFolded(true);
      else if (hasEpilogue() && !epilogueFolded() && cursorLine_ >= 0 &&
               cursorLine_ < lines_.size() &&
               lines_[cursorLine_].sourceLine >= epilogueLine())
        setEpilogueFolded(true);
      return;
    }
  }
  QAbstractScrollArea::keyPressEvent(event);
}

void CodeText::selectAll() {
  selectAllAfterLoad_ = loading_;
  if (lines_.isEmpty())
    return;
  pendingAddress_.reset();
  selectedAddress_.reset();
  anchor_ = std::pair{0, 0};
  cursorLine_ = int(lines_.size()) - 1;
  cursorColumn_ = int(lines_.back().styled.text.size());
  viewport()->update();
}

void CodeText::dragTo(const QPoint &position) {
  if (lines_.isEmpty())
    return;
  selectAllAfterLoad_ = false;
  pendingAddress_.reset();
  selectedAddress_.reset();
  const QRect area = viewport()->rect();
  const int y = std::clamp(position.y(), 0, std::max(0, area.height() - 1));
  const int line = std::clamp(lineAt(y), 0, int(lines_.size()) - 1);
  if (!anchor_)
    anchor_ = std::pair{cursorLine_, cursorColumn_};
  cursorLine_ = line;
  cursorColumn_ = columnAt(line, position.x());
  viewport()->update();
}

void CodeText::mousePressEvent(QMouseEvent *event) {
  setFocus(Qt::MouseFocusReason);
  selecting_ = false;
  dragScroll_.stop();
  if (event->button() != Qt::LeftButton && event->button() != Qt::RightButton)
    return;
  if (event->button() == Qt::LeftButton) {
    if (const QString id = blockAt(event->position().toPoint());
        !id.isEmpty()) {
      setRegionFolded(id, !library_.isFolded(id));
      return;
    } else if (const QString id = regionAt(event->position().toPoint());
               !id.isEmpty()) {
      setRegionFolded(id, false);
      return;
    }
  }
  const int line =
      std::min(lineAt(int(event->position().y())), int(lines_.size()) - 1);
  if (line < 0)
    return;
  const int column = columnAt(line, int(event->position().x()));
  // A right click inside the selection keeps it for the menu's Copy.
  if (event->button() == Qt::RightButton && anchor_) {
    const auto at = std::pair{line, column};
    const auto cursor = std::pair{cursorLine_, cursorColumn_};
    if (std::min(*anchor_, cursor) <= at && at <= std::max(*anchor_, cursor))
      return;
  }
  moveCursor(line, column, event->modifiers() & Qt::ShiftModifier);
  if (event->button() == Qt::LeftButton) {
    selecting_ = true;
    dragPoint_ = event->position().toPoint();
    const auto name = sourceNameAt(line, column);
    highlight_ = name ? name->first : tokenAt(line, column);
  }
  updateMouseCursor(event->position().toPoint());
  viewport()->update();
}

void CodeText::mouseMoveEvent(QMouseEvent *event) {
  updateMouseCursor(event->position().toPoint());
  if (!selecting_ || !(event->buttons() & Qt::LeftButton) || lines_.isEmpty())
    return;
  dragPoint_ = event->position().toPoint();
  dragTo(dragPoint_);
  if (viewport()->rect().contains(dragPoint_))
    dragScroll_.stop();
  else if (!dragScroll_.isActive())
    dragScroll_.start();
}

void CodeText::mouseReleaseEvent(QMouseEvent *event) {
  selecting_ = false;
  dragScroll_.stop();
  updateMouseCursor(event->position().toPoint());
  if (anchor_ && *anchor_ == std::pair{cursorLine_, cursorColumn_})
    anchor_.reset();
}

void CodeText::mouseDoubleClickEvent(QMouseEvent *event) {
  if (event->button() != Qt::LeftButton ||
      !blockAt(event->position().toPoint()).isEmpty())
    return;
  // A source name resolves by its symbol, as the C view's linked names do.
  if (const auto name = sourceNameAt(cursorLine_, cursorColumn_)) {
    emit nameActivated(name->second.symbol.isEmpty() ? name->second.identifier
                                                     : name->second.symbol);
    return;
  }
  if (dialectOf(representation_, pageLanguage_) == Dialect::LLVM) {
    activateLLVMToken();
    return;
  }
  const QString token = currentToken();
  // A goto label is a place in this function: the code goes to the label,
  // the disassembly to the block it labels.
  if (const auto label = addressLabel(token)) {
    emit labelActivated(label->first);
    goToLabel(token, label->second, label->first);
    return;
  }
  // A type or macro this code declares opens at its declaration; the
  // language's own words name nothing in the binary.
  const auto &words = vocabularyOf(dialectOf(representation_, pageLanguage_));
  if (token.isEmpty() || goToDeclaration(token) ||
      words.control.contains(token) || words.keywords.contains(token) ||
      words.types.contains(token))
    return;
  // A global shows at its address, whatever C called it there.
  if (const auto address = objectAddress(token)) {
    emit objectActivated(*address);
    return;
  }
  // A C++ function links by its mangled symbol, which the binary names.
  emit nameActivated(linkedSymbol(token).value_or(token));
}

bool CodeText::hasPrelude() const {
  return library_.hasRegion(QLatin1String(PreludeRegion));
}

bool CodeText::preludeFolded() const {
  return library_.isFolded(QLatin1String(PreludeRegion));
}

void CodeText::setPreludeFolded(bool folded) {
  if (!hasPrelude() || preludeFolded() == folded)
    return;
  library_.setRegionFolded(QLatin1String(PreludeRegion), folded);
  rebuildLines();
  if (folded)
    moveCursor(0, 0, false);
  emit foldingChanged();
}

bool CodeText::hasEpilogue() const {
  return library_.hasRegion(QLatin1String(EpilogueRegion));
}

bool CodeText::epilogueFolded() const {
  return library_.isFolded(QLatin1String(EpilogueRegion));
}

void CodeText::setEpilogueFolded(bool folded) {
  if (!hasEpilogue() || epilogueFolded() == folded)
    return;
  library_.setRegionFolded(QLatin1String(EpilogueRegion), folded);
  rebuildLines();
  if (folded && cursorLine_ >= lines_.size())
    moveCursor(int(lines_.size()) - 1, 0, false);
  emit foldingChanged();
}

int CodeText::epilogueLine() const {
  return int(source_.count(QLatin1Char('\n')) -
             epilogue_.value("lines").toInteger(0));
}

QString CodeText::tokenAt(int line, int column) const {
  if (line < 0 || line >= lines_.size())
    return {};
  if (dialectOf(representation_, pageLanguage_) != Dialect::LLVM)
    return lines_[line].styled.tokenAt(column);
  const auto token = llvmTokenAt(lines_[line].styled.text, column);
  return token ? token->text : QString();
}

std::optional<Address> CodeText::globalReferenceAt(int line, int column) const {
  if (line < 0 || line >= lines_.size() || lines_[line].folded ||
      dialectOf(representation_, pageLanguage_) != Dialect::LLVM)
    return std::nullopt;
  const QString &text = lines_[line].styled.text;
  const auto token = llvmTokenAt(text, column);
  if (!token || token->comment || !token->text.startsWith(QLatin1Char('@')))
    return std::nullopt;
  const auto found = references_.constFind(lines_[line].sourceLine);
  if (found == references_.cend())
    return std::nullopt;
  // A mirror the line reaches at two offsets spells twice: the occurrence
  // picks its reference.
  int occurrence = 0;
  for (const auto &other : llvmTokens(text))
    if (other.begin < token->begin && other.text == token->text &&
        !other.comment)
      ++occurrence;
  std::optional<Address> last;
  for (const auto &reference : *found)
    if (reference.name == token->text) {
      if (occurrence-- == 0)
        return reference.address;
      last = reference.address;
    }
  return last;
}

QString CodeText::globalHoverText(int line, int column) const {
  const auto address = globalReferenceAt(line, column);
  if (!address)
    return {};
  QString text;
  if (const auto declared = declarationLine(tokenAt(line, column))) {
    const auto source = QStringView(source_).split(QLatin1Char('\n'));
    // The comments over the declaration say what the global is: a mirror's
    // image bytes and the offsets this function uses, a function's address.
    int first = *declared;
    while (first > 0 && source.at(first - 1).trimmed().startsWith(u';'))
      --first;
    for (int i = first; i < *declared; ++i) {
      QStringView comment = source.at(i).trimmed().mid(1);
      if (comment.startsWith(u' '))
        comment = comment.mid(1);
      text += comment;
      text += QLatin1Char('\n');
    }
    // A mirror's declaration spells its whole layout.
    constexpr int Shown = 160;
    QString declaration = source.at(*declared).trimmed().toString();
    if (declaration.size() > Shown)
      declaration = declaration.left(Shown - 1) + QChar(0x2026);
    text += declaration + QLatin1Char('\n');
  }
  return text + tr("Double-click to go to %1.")
                    .arg(QStringLiteral("0x") + displayAddress(*address));
}

bool CodeText::showUnresolvedDeclaration(const QString &name) {
  const QString token = std::exchange(unresolvedDeclaration_, QString());
  return !token.isEmpty() && llvmGlobalName(token) == name &&
         goToDeclaration(token);
}

void CodeText::activateLLVMToken() {
  unresolvedDeclaration_.clear();
  if (cursorLine_ < 0 || cursorLine_ >= lines_.size())
    return;
  const auto token =
      llvmTokenAt(lines_[cursorLine_].styled.text, cursorColumn_);
  if (!token)
    return;
  // A comment names image addresses and what the listing calls them there.
  if (token->comment) {
    emit nameActivated(token->text);
    return;
  }
  if (token->text.startsWith(QLatin1Char('@'))) {
    if (const auto address = globalReferenceAt(cursorLine_, cursorColumn_)) {
      emit nameActivated(hexAddress(*address));
      return;
    }
    // The binary may name a global the engine gives no address, such as an
    // import; LLVM's own and NeverD's declarations show in the prelude.
    unresolvedDeclaration_ = token->text;
    emit nameActivated(llvmGlobalName(token->text));
    return;
  }
  goToDeclaration(token->text);
}

const CodeText::Declarations &CodeText::declarations() const {
  if (declarations_)
    return *declarations_;
  // The declared name ends a typedef, before its attributes; a record or
  // macro names itself first; a labeled function names itself before its
  // parameters.
  static const QRegularExpression Typedef(QStringLiteral(
      R"(^\s*typedef\b.*?\b([A-Za-z_]\w*)\s*(?:__attribute__\s*\(\(.*\)\)\s*)?;\s*$)"));
  static const QRegularExpression Record(QStringLiteral(
      R"(^\s*(?:typedef\s+)?(?:struct|union|enum)\s+([A-Za-z_]\w*)\s*\{)"));
  static const QRegularExpression Define(
      QStringLiteral(R"(^\s*#\s*define\s+([A-Za-z_]\w*))"));
  static const QRegularExpression Labeled(QStringLiteral(
      R"re(\b([A-Za-z_]\w*)\s*\([^;]*\)\s*__asm__\s*\(\s*"([^"\\]+)"\s*\))re"));
  // A global's address: in the comment above its declaration (decompiled C)
  // or after it (C through LLVM).  The declared name precedes its array
  // bounds and its initializer or the end of the declaration.
  static const QRegularExpression ImageAddress(
      QStringLiteral(R"(^\s*/\*\s*neverd\.image:\s*0x([0-9A-Fa-f]+)\b)"));
  static const QRegularExpression TrailingAddress(
      QStringLiteral(R"(;\s*/\*\s*0x([0-9A-Fa-f]+)\s*\*/\s*$)"));
  static const QRegularExpression Declared(
      QStringLiteral(R"(\b([A-Za-z_]\w*)\s*(?:\[[^\]]*\])?\s*[=;])"));
  auto &found = declarations_.emplace();
  if (dialectOf(representation_, pageLanguage_) == Dialect::LLVM) {
    found.types = llvmDefinitions(source_);
    return found;
  }
  int line = 0;
  std::optional<Address> declaredAt;
  for (const QStringView text : QStringView(source_).split(QLatin1Char('\n'))) {
    const QStringView head = text.trimmed();
    if (const auto marker = ImageAddress.matchView(text); marker.hasMatch()) {
      declaredAt = marker.captured(1).toULongLong(nullptr, 16);
      ++line;
      continue;
    }
    std::optional<Address> address = std::exchange(declaredAt, std::nullopt);
    if (const auto trailing = TrailingAddress.matchView(text);
        !address && trailing.hasMatch() && head.startsWith(u"extern"))
      address = trailing.captured(1).toULongLong(nullptr, 16);
    if (address)
      if (const auto name = Declared.matchView(text);
          name.hasMatch() && !found.objects.contains(name.captured(1)))
        found.objects.insert(name.captured(1), *address);
    if (head.startsWith(u"typedef") || head.startsWith(u"struct") ||
        head.startsWith(u"union") || head.startsWith(u"enum") ||
        head.startsWith(u'#')) {
      for (const auto *pattern : {&Typedef, &Record, &Define})
        if (const auto match = pattern->matchView(text); match.hasMatch()) {
          if (!found.types.contains(match.captured(1)))
            found.types.insert(match.captured(1), line);
          break;
        }
    } else if (text.contains(u"__asm__")) {
      if (const auto match = Labeled.matchView(text);
          match.hasMatch() && !found.linked.contains(match.captured(1)))
        found.linked.insert(match.captured(1), {line, match.captured(2)});
    }
    ++line;
  }
  return found;
}

std::optional<int> CodeText::declarationLine(const QString &name) const {
  const auto &types = declarations().types;
  if (const auto found = types.constFind(name); found != types.cend())
    return *found;
  return std::nullopt;
}

std::optional<Address> CodeText::objectAddress(const QString &name) const {
  const auto &objects = declarations().objects;
  if (const auto found = objects.constFind(name); found != objects.cend())
    return *found;
  return std::nullopt;
}

std::optional<QString> CodeText::linkedSymbol(const QString &name) const {
  const auto &linked = declarations().linked;
  if (const auto found = linked.constFind(name); found != linked.cend())
    return found->second;
  return std::nullopt;
}

bool CodeText::labelsNameAddresses() const {
  // LLVM C numbers its labels instead.
  return representation_ == QLatin1String("source") ||
         representation_ == QLatin1String("c");
}

std::optional<std::pair<Address, int>>
CodeText::addressLabel(const QString &name) const {
  if (!labelsNameAddresses() || !name.startsWith(QLatin1String("L_")))
    return std::nullopt;
  int line = 0;
  for (qsizetype start = 0; start < source_.size(); ++line) {
    qsizetype end = source_.indexOf(QLatin1Char('\n'), start);
    if (end < 0)
      end = source_.size();
    if (const auto label =
            labelDefinition(QStringView(source_).mid(start, end - start));
        label && label->first == name)
      return std::pair{label->second, line};
    start = end + 1;
  }
  return std::nullopt;
}

void CodeText::goToLabel(const QString &name, int sourceLine, Address address) {
  if (library_.unfoldSourceLine(sourceLine)) {
    rebuildLines();
    emit foldingChanged();
  }
  const int line =
      library_.canFold() ? library_.displayLine(sourceLine) : sourceLine;
  if (line < 0 || line >= lines_.size())
    return;
  highlight_ = name;
  // The disassembly already shows the block.
  const QScopedValueRollback<bool> following(following_, true);
  moveCursor(line, std::max(0, int(lines_[line].styled.text.indexOf(name))),
             false);
  selectedAddress_ = address;
}

bool CodeText::goToDeclaration(const QString &name) {
  const auto source = declarationLine(name);
  if (!source || loading_)
    return false;
  if (hasPrelude() && preludeFolded() &&
      *source < prelude_.value("lines").toInteger(0))
    setPreludeFolded(false);
  if (hasEpilogue() && epilogueFolded() && *source >= epilogueLine())
    setEpilogueFolded(false);
  const int line = library_.canFold() ? library_.displayLine(*source) : *source;
  if (line < 0 || line >= lines_.size())
    return false;
  highlight_ = name;
  moveCursor(line, std::max(0, int(lines_[line].styled.text.indexOf(name))),
             false);
  return true;
}

void CodeText::wheelEvent(QWheelEvent *event) {
  if (event->modifiers() & Qt::ControlModifier) {
    Theme::instance().zoomCodeFont(event->angleDelta().y() > 0 ? 1 : -1);
    return;
  }
  const int steps = event->angleDelta().y() / 120;
  verticalScrollBar()->setValue(verticalScrollBar()->value() -
                                steps * WheelLines);
  // The selection a drag makes follows the text the wheel scrolls.
  if (selecting_ && (event->buttons() & Qt::LeftButton))
    dragTo(dragPoint_);
}

void CodeText::contextMenuEvent(QContextMenuEvent *event) {
  emit contextMenuRequested(event->globalPos());
}

//===----------------------------------------------------------------------===//
// CodeView
//===----------------------------------------------------------------------===//

QString CodeView::titleOf(const QString &representation) {
  if (const auto *entry = representationOf(representation))
    return QCoreApplication::translate("Representations", entry->title);
  return representation;
}

bool CodeView::isSource(const QString &representation) {
  const auto *entry = representationOf(representation);
  return entry &&
         (sourceDialect(entry->dialect) || entry->dialect == Dialect::Source);
}

QString CodeView::chosenLanguage() const {
  const auto *entry = representationOf(representation());
  if (!entry || entry->dialect != Dialect::Source ||
      text_->representation() != representation())
    return {};
  // Language names are the same in every translation; C, the common case,
  // goes without saying.
  const QString language = text_->language();
  if (language == QLatin1String("cpp"))
    return QStringLiteral("C++");
  if (language == QLatin1String("rust"))
    return QStringLiteral("Rust");
  if (language == QLatin1String("go"))
    return QStringLiteral("Go");
  return {};
}

CodeView::CodeView(Session &session, const QString &representation,
                   QWidget *parent)
    : QWidget(parent), session_(session), selector_(new QComboBox(this)),
      fold_(new QToolButton(this)), lock_(new QToolButton(this)),
      optimize_(new QToolButton(this)), status_(new QLabel(this)),
      text_(new CodeText(session, this)) {
  auto *layout = new QVBoxLayout(this);
  layout->setContentsMargins(0, 0, 0, 0);
  layout->setSpacing(0);
  auto *barRow = new QWidget(this);
  auto *bar = new QHBoxLayout(barRow);
  bar->setContentsMargins(4, 2, 4, 2);
  bar->setSpacing(0);
  selector_->addItem(QString(), representation);
  updateRepresentations();
  bar->addWidget(selector_);
  fold_->setCheckable(true);
  fold_->setAutoRaise(true);
  fold_->setIcon(icon(QStringLiteral("function_library")));
  fold_->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);
  fold_->setText(tr("Fold library code"));
  fold_->setToolTip(tr("Show each recognized library operation as a one-line "
                       "summary; copy and export keep the full code."));
  fold_->setVisible(false);
  bar->addWidget(fold_);
  lock_->setCheckable(true);
  lock_->setAutoRaise(true);
  lock_->setIcon(icon(QStringLiteral("lock")));
  lock_->setToolTip(tr("Keep this function while the disassembly moves on"));
  bar->addWidget(lock_);
  optimize_->setObjectName(QStringLiteral("sourceOptimization"));
  optimize_->setAutoRaise(true);
  optimize_->setIcon(icon(QStringLiteral("source_optimize")));
  optimize_->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);
  optimize_->setText(tr("Optimization"));
  optimize_->setToolTip(tr("Transform the current function"));
  optimize_->setPopupMode(QToolButton::InstantPopup);
  auto *optimizationMenu = new QMenu(optimize_);
  optimize_->setMenu(optimizationMenu);
  connect(optimizationMenu, &QMenu::aboutToShow, this,
          [this, optimizationMenu] {
            optimizationMenu->clear();
            addSourceActions(*optimizationMenu);
          });
  bar->addWidget(optimize_);
  bar->addStretch(1);
  makeRowShrinkable(*barRow);
  layout->addWidget(barRow);
  layout->addWidget(text_, 1);
  makeStatusLine(*status_);
  layout->addWidget(status_);
  connect(selector_, &QComboBox::currentIndexChanged, this, [this] {
    const auto name = selector_->currentData().toString();
    if (text_->function())
      text_->load(*text_->function(), name);
    emit representationChanged(name);
    updateStatus();
  });
  connect(&session_, &Session::opened, this, &CodeView::updateRepresentations);
  connect(&session_, &Session::unloaded, this,
          &CodeView::updateRepresentations);
  connect(text_, &CodeText::statusChanged, this, &CodeView::updateStatus);
  connect(text_, &CodeText::languageChanged, this, &CodeView::languageChanged);
  connect(fold_, &QToolButton::toggled, text_, &CodeText::setFolded);
  connect(text_, &CodeText::foldingChanged, this, [this] {
    fold_->setVisible(text_->foldableCount() > 0);
    const QSignalBlocker blocker(fold_);
    fold_->setChecked(text_->libraryFolded());
  });
  updateStatus();
}

void CodeView::addSourceActions(QMenu &menu) {
  if (!isSource(representation()) || !text_->function())
    return;
  auto *symbolic = menu.addAction(icon(QStringLiteral("source_symbolic")),
                                  tr("Symbolic simplification"));
  symbolic->setObjectName(QStringLiteral("sourceSymbolic"));
  symbolic->setCheckable(true);
  symbolic->setChecked(text_->sourceOptions().value("symbolic").toBool());
  connect(symbolic, &QAction::triggered, this, [this](bool enabled) {
    auto options = text_->sourceOptions();
    if (enabled)
      options["symbolic"] = true;
    else
      options.remove("symbolic");
    text_->setSourceOptions(options);
  });
  if (representation() == QLatin1String("llvmc")) {
    auto *recovery = menu.addAction(icon(QStringLiteral("source_control_flow")),
                                    tr("Recover LLVM control flow"));
    recovery->setObjectName(QStringLiteral("sourceLLVMControlFlow"));
    recovery->setCheckable(true);
    recovery->setChecked(
        text_->sourceOptions().value("llvm_control_flow").toBool(true));
    connect(recovery, &QAction::triggered, this, [this](bool enabled) {
      auto options = text_->sourceOptions();
      if (enabled)
        options.remove("llvm_control_flow");
      else
        options["llvm_control_flow"] = false;
      text_->setSourceOptions(options);
    });
    auto *levels = menu.addMenu(icon(QStringLiteral("source_optimize")),
                                tr("LLVM optimization"));
    auto *group = new QActionGroup(levels);
    const auto selected = text_->sourceOptions()
                              .value("llvm_optimization")
                              .toString(QStringLiteral("O2"));
    for (const auto &level : {QStringLiteral("basic"), QStringLiteral("O0"),
                              QStringLiteral("O1"), QStringLiteral("O2"),
                              QStringLiteral("O3"), QStringLiteral("custom")}) {
      auto *action = levels->addAction(
          level == QLatin1String("basic")    ? tr("Basic cleanup")
          : level == QLatin1String("O2")     ? tr("O2 (default)")
          : level == QLatin1String("custom") ? tr("Choose passes…")
                                             : level);
      action->setObjectName(QStringLiteral("sourceLLVM_") + level);
      action->setCheckable(true);
      action->setChecked(selected == level);
      group->addAction(action);
      connect(action, &QAction::triggered, this, [this, level] {
        auto options = text_->sourceOptions();
        if (level == QLatin1String("custom")) {
          QDialog dialog(this);
          dialog.setWindowTitle(tr("LLVM passes for this function"));
          auto *layout = new QVBoxLayout(&dialog);
          auto *note =
              new QLabel(tr("Choose scalar cleanup passes. Symbolic "
                            "simplification is controlled separately."),
                         &dialog);
          note->setWordWrap(true);
          layout->addWidget(note);
          const auto selectedPasses = options.value("llvm_passes").toArray();
          QVector<QCheckBox *> boxes;
          for (const auto &pass :
               {QStringLiteral("instcombine"), QStringLiteral("early-cse"),
                QStringLiteral("simplifycfg"), QStringLiteral("dce")}) {
            auto *box = new QCheckBox(pass, &dialog);
            box->setChecked(!options.contains("llvm_passes") ||
                            selectedPasses.contains(pass));
            boxes.push_back(box);
            layout->addWidget(box);
          }
          auto *buttons = new QDialogButtonBox(
              QDialogButtonBox::Ok | QDialogButtonBox::Cancel, &dialog);
          connect(buttons, &QDialogButtonBox::accepted, &dialog,
                  &QDialog::accept);
          connect(buttons, &QDialogButtonBox::rejected, &dialog,
                  &QDialog::reject);
          layout->addWidget(buttons);
          const auto function = text_->function();
          const auto epoch = session_.epoch();
          if (dialog.exec() != QDialog::Accepted ||
              text_->function() != function || session_.epoch() != epoch ||
              representation() != QLatin1String("llvmc"))
            return;
          QJsonArray passes;
          for (auto *box : boxes)
            if (box->isChecked())
              passes.append(box->text());
          options["llvm_passes"] = passes;
        } else {
          options.remove("llvm_passes");
        }
        if (level == QLatin1String("O2"))
          options.remove("llvm_optimization");
        else
          options["llvm_optimization"] = level;
        text_->setSourceOptions(options);
      });
    }
  }
  auto *reset = menu.addAction(icon(QStringLiteral("undo")),
                               tr("Restore default source"));
  reset->setObjectName(QStringLiteral("sourceDefault"));
  reset->setEnabled(!text_->sourceOptions().isEmpty());
  connect(reset, &QAction::triggered, this,
          [this] { text_->setSourceOptions({}); });
  auto *cancel = menu.addAction(icon(QStringLiteral("cancel")),
                                tr("Cancel transformation"));
  cancel->setObjectName(QStringLiteral("sourceCancel"));
  cancel->setEnabled(text_->loading());
  connect(cancel, &QAction::triggered, text_, &CodeText::cancel);
}

void CodeView::updateRepresentations() {
  // Pseudocode reads each function in its program's languages: C, and the
  // C++, Rust or Go a function was written in. C beside it is the one other
  // choice, for a program with another language; until a program says, it
  // stays.
  const auto pseudocode =
      session_.metadata().value("language").toObject().value("pseudocode");
  const bool otherLanguage =
      !pseudocode.isArray() || pseudocode.toArray().size() > 1;
  const auto offered = [&](const Representation &entry) {
    return otherLanguage || QLatin1String(entry.name) != QLatin1String("c");
  };
  const QString current = selector_->currentData().toString();
  {
    const QSignalBlocker blocker(selector_);
    selector_->clear();
    for (const auto &entry : Representations)
      if (offered(entry))
        selector_->addItem(
            QCoreApplication::translate("Representations", entry.title),
            QString::fromLatin1(entry.name));
  }
  const int index = selector_->findData(current);
  {
    const QSignalBlocker blocker(selector_);
    selector_->setCurrentIndex(index);
  }
  // A view the program does not offer shows Pseudocode instead.
  if (index < 0)
    selector_->setCurrentIndex(selector_->findData(pseudocodeRepresentation()));
}

void CodeView::showFunction(Address function) {
  text_->load(function, selector_->currentData().toString());
}

void CodeView::setRepresentation(const QString &representation) {
  const int index = selector_->findData(representation);
  if (index >= 0)
    selector_->setCurrentIndex(index);
}

QString CodeView::representation() const {
  return selector_->currentData().toString();
}

void CodeView::updateStatus() {
  status_->setText(text_->status());
  optimize_->setVisible(isSource(representation()));
  optimize_->setEnabled(text_->function().has_value());
}

bool CodeView::locked() const { return lock_->isChecked(); }

} // namespace neverd::gui
