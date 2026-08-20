#include "SrtParser.h"

#include <QFile>
#include <QRegularExpression>
#include <QStringList>
#include <QTextStream>

namespace dvs {

namespace {

// Matches HH:MM:SS,mmm / HH:MM:SS.mmm / MM:SS,mmm / SS.mmm. Hours and minutes
// are both optional so partial forms still parse.
const QRegularExpression& timestampRe() {
    static const QRegularExpression re(
        QStringLiteral(R"((?:(\d+):)?(?:(\d+):)?(\d+)[,.](\d{1,3}))"));
    return re;
}

const QRegularExpression& tagRe() {
    static const QRegularExpression re(QStringLiteral(R"(<[^>]*>|\{\\[^}]*\})"));
    return re;
}

// Turns the 2-4 captured groups into milliseconds. With all three time groups
// present the order is H:M:S; with two it is M:S; with one it is S.
qint64 toMs(const QRegularExpressionMatch& m) {
    const QString g1 = m.captured(1);
    const QString g2 = m.captured(2);
    const qint64 seconds = m.captured(3).toLongLong();
    qint64 minutes = 0;
    qint64 hours = 0;
    if (!g1.isEmpty() && !g2.isEmpty()) {
        hours = g1.toLongLong();
        minutes = g2.toLongLong();
    } else if (!g1.isEmpty()) {
        minutes = g1.toLongLong();
    }

    QString frac = m.captured(4);
    while (frac.size() < 3) frac.append(QLatin1Char('0'));
    const qint64 millis = frac.left(3).toLongLong();

    return ((hours * 60 + minutes) * 60 + seconds) * 1000 + millis;
}

} // namespace

QString formatTimestamp(qint64 ms) {
    if (ms < 0) ms = 0;
    const qint64 h = ms / 3600000;
    const qint64 m = (ms / 60000) % 60;
    const qint64 s = (ms / 1000) % 60;
    const qint64 f = ms % 1000;
    return QStringLiteral("%1:%2:%3,%4")
        .arg(h, 2, 10, QLatin1Char('0'))
        .arg(m, 2, 10, QLatin1Char('0'))
        .arg(s, 2, 10, QLatin1Char('0'))
        .arg(f, 3, 10, QLatin1Char('0'));
}

SrtParseResult parseSrt(const QString& content) {
    SrtParseResult result;

    QString text = content;
    if (!text.isEmpty() && text.at(0) == QChar(0xFEFF)) {
        text.remove(0, 1);
    }
    text.replace(QLatin1String("\r\n"), QLatin1String("\n"));
    text.replace(QLatin1Char('\r'), QLatin1Char('\n'));

    // Blocks are separated by one or more blank lines.
    const QStringList blocks =
        text.split(QRegularExpression(QStringLiteral(R"(\n[ \t]*\n)")), Qt::SkipEmptyParts);

    for (const QString& block : blocks) {
        const QStringList lines = block.split(QLatin1Char('\n'));

        int timingLine = -1;
        for (int i = 0; i < lines.size(); ++i) {
            if (lines.at(i).contains(QLatin1String("-->"))) {
                timingLine = i;
                break;
            }
        }
        if (timingLine < 0) {
            continue; // no timing -> not a cue (stray header, comment, ...)
        }

        const QStringList halves =
            lines.at(timingLine).split(QLatin1String("-->"));
        if (halves.size() < 2) continue;

        const QRegularExpressionMatch startM = timestampRe().match(halves.at(0));
        const QRegularExpressionMatch endM = timestampRe().match(halves.at(1));
        if (!startM.hasMatch() || !endM.hasMatch()) continue;

        SrtCue cue;
        cue.startMs = toMs(startM);
        cue.endMs = toMs(endM);

        QStringList body;
        for (int i = timingLine + 1; i < lines.size(); ++i) {
            body << lines.at(i);
        }
        cue.text = body.join(QLatin1Char(' '));
        cue.text.remove(tagRe());
        cue.text = cue.text.simplified();

        if (cue.text.isEmpty()) continue;
        if (cue.endMs < cue.startMs) cue.endMs = cue.startMs;

        result.cues.append(cue);
    }

    if (result.cues.isEmpty()) {
        result.error = QStringLiteral("No subtitle cues found - is this a SubRip (.srt) file?");
    }
    return result;
}

SrtParseResult parseSrtFile(const QString& path) {
    SrtParseResult result;
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly)) {
        result.error = QStringLiteral("Cannot open %1: %2").arg(path, f.errorString());
        return result;
    }
    QTextStream in(&f);
    in.setEncoding(QStringConverter::Utf8);
    return parseSrt(in.readAll());
}

} // namespace dvs
