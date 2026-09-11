#include "text_card.hpp"

#include <QColor>
#include <QFont>
#include <QFontDatabase>
#include <QGuiApplication>
#include <QMimeData>
#include <QPainter>
#include <QPalette>
#include <QStringList>
#include <QTextDocument>
#include <QtMath>

#include <algorithm>

namespace vshot {
namespace {

// Layout constants in logical pixels; the card is rasterized at `pixelRatio`
// device pixels per logical pixel so text stays crisp on HiDPI outputs.
constexpr qreal kPadding = 12.0;
constexpr qreal kBorder = 1.0;
constexpr qreal kMaxContentWidth = 560.0;
constexpr qreal kMaxContentHeight = 1200.0;

// Markdown signals: a code fence is decisive on its own, the rest need
// company, so ordinary prose and snake_case identifiers stay plain text.
bool looksLikeMarkdown(const QString &text)
{
    int score = 0;
    const QStringList lines = text.split(QLatin1Char('\n'));
    for (const QString &line : lines) {
        const QString trimmed = line.trimmed();
        if (trimmed.startsWith(QLatin1String("```"))) {
            score += 3;
            continue;
        }
        int hashes = 0;
        while (hashes < trimmed.size() && trimmed.at(hashes) == QLatin1Char('#')) {
            ++hashes;
        }
        if (hashes >= 1 && hashes <= 6 && hashes < trimmed.size()
            && trimmed.at(hashes) == QLatin1Char(' ')) {
            // Only +1: a lone "# comment" line is far more likely a shell
            // script than a one-line markdown document.
            score += 1;
        }
        const bool listed = trimmed.startsWith(QLatin1String("- "))
            || trimmed.startsWith(QLatin1String("* "))
            || trimmed.startsWith(QLatin1String("+ "))
            || (trimmed.size() >= 3 && trimmed.at(0).isDigit()
                && trimmed.at(1) == QLatin1Char('.') && trimmed.at(2) == QLatin1Char(' '));
        if (listed) {
            score += 1;
        }
        if (trimmed.startsWith(QLatin1String("> "))) {
            score += 1;
        }
        if (trimmed.contains(QLatin1String("|---")) || trimmed.contains(QLatin1String("| ---"))) {
            score += 2;
        }
    }
    if (text.count(QLatin1String("**")) >= 2) {
        score += 1;
    }
    if (text.contains(QLatin1Char('[')) && text.contains(QLatin1String("]("))) {
        score += 1;
    }
    return score >= 2;
}

// Code signals are independent boolean hits over the whole snippet; plain
// prose rarely collects two of them, single lines never render as code.
bool looksLikeCode(const QString &text)
{
    static const char *const kKeywordPrefixes[] = {
        "def ", "class ", "import ", "from ", "fn ", "function ", "const ",
        "let ", "var ", "package ", "public ", "private ", "protected ",
        "static ", "void ", "int ", "#include", "using namespace",
    };
    bool braces = false;
    bool semicolons = false;
    bool colons = false;
    bool keywords = false;
    bool indented = false;
    int nonEmpty = 0;
    for (const QString &raw : text.split(QLatin1Char('\n'))) {
        const QString line = raw.trimmed();
        if (line.isEmpty()) {
            continue;
        }
        ++nonEmpty;
        if (line.endsWith(QLatin1Char(';')) || line.endsWith(QLatin1Char('{'))
            || line.endsWith(QLatin1Char('}'))) {
            semicolons = true;
        }
        if (line.contains(QLatin1Char('{')) || line.contains(QLatin1Char('}'))) {
            braces = true;
        }
        if (line.endsWith(QLatin1Char(':'))) {
            colons = true;
        }
        if (raw.startsWith(QLatin1Char(' ')) || raw.startsWith(QLatin1Char('\t'))) {
            indented = true;
        }
        for (const char *keyword : kKeywordPrefixes) {
            if (line.startsWith(QLatin1String(keyword))) {
                keywords = true;
                break;
            }
        }
    }
    if (nonEmpty < 2) {
        return false;
    }
    const int score = (braces ? 1 : 0) + (semicolons ? 1 : 0) + (colons ? 1 : 0)
        + (keywords ? 1 : 0) + (indented ? 1 : 0);
    return score >= 2;
}

} // namespace

QImage renderTextCard(const QMimeData *mime, const QString &text, int pixelRatio)
{
    if (text.trimmed().isEmpty()) {
        return QImage();
    }
    const int ratio = std::clamp(pixelRatio, 1, 4);

    const bool hasHtml = mime != nullptr && !mime->html().trimmed().isEmpty();
    const bool markdown = !hasHtml && looksLikeMarkdown(text);
    const bool code = !hasHtml && !markdown && looksLikeCode(text);

    QTextDocument document;
    document.setDocumentMargin(0.0);
    const QFont mono = QFontDatabase::systemFont(QFontDatabase::FixedFont);
    if (code) {
        document.setDefaultFont(mono);
        // A fixed dark editor-style card; the escaped snippet keeps its
        // indentation and draws light-on-dark like a terminal pane.
        document.setHtml(QStringLiteral("<pre style=\"color:#e6e6e6;\">%1</pre>")
                             .arg(text.toHtmlEscaped()));
    } else if (hasHtml) {
        document.setDefaultFont(QFontDatabase::systemFont(QFontDatabase::GeneralFont));
        document.setHtml(mime->html());
    } else if (markdown) {
        document.setDefaultFont(QFontDatabase::systemFont(QFontDatabase::GeneralFont));
        document.setMarkdown(text, QTextDocument::MarkdownDialectGitHub);
    } else {
        document.setDefaultFont(QFontDatabase::systemFont(QFontDatabase::GeneralFont));
        document.setPlainText(text);
    }

    // Wrap at the maximum width, then tighten to the actually used width so
    // short snippets get a snug card; re-wrap at that width before reading
    // the final height.
    document.setTextWidth(kMaxContentWidth);
    const qreal contentWidth = std::min<qreal>(document.idealWidth(), kMaxContentWidth);
    document.setTextWidth(contentWidth);
    const qreal contentHeight = std::min<qreal>(document.size().height(), kMaxContentHeight);

    const qreal cardWidth = contentWidth + 2.0 * (kPadding + kBorder);
    const qreal cardHeight = contentHeight + 2.0 * (kPadding + kBorder);
    QImage image(qCeil(cardWidth) * ratio, qCeil(cardHeight) * ratio,
                 QImage::Format_ARGB32_Premultiplied);
    if (image.isNull()) {
        return QImage();
    }
    image.setDevicePixelRatio(ratio);
    image.fill(Qt::transparent);

    QPainter painter(&image);
    painter.setRenderHint(QPainter::Antialiasing, true);
    const QRectF cardRect(0.5, 0.5, qCeil(cardWidth) - 1.0, qCeil(cardHeight) - 1.0);
    if (code) {
        painter.setPen(QPen(QColor(255, 255, 255, 50), kBorder));
        painter.setBrush(QColor(0x1e, 0x1e, 0x1e));
    } else {
        // The card follows the application palette so light and dark themes
        // both yield readable text without inspecting the payload's colors.
        painter.setPen(QPen(QColor(0, 0, 0, 50), kBorder));
        painter.setBrush(QGuiApplication::palette().color(QPalette::Base));
    }
    painter.drawRect(cardRect);
    painter.translate(kPadding + kBorder, kPadding + kBorder);
    // drawContents clips to the exposed rect, which caps absurdly tall
    // payloads at kMaxContentHeight.
    document.drawContents(&painter, QRectF(0, 0, contentWidth, contentHeight));
    painter.end();
    return image;
}

} // namespace vshot
