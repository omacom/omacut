#include "ytdlp.h"

#include <QStandardPaths>
#include <QUrl>

namespace ytdlp {

namespace {
// Marks our own progress lines so they can be told apart from the ordinary
// chatter yt-dlp writes to stdout.
const QString kProgressMarker = QStringLiteral("omacut-progress");
const QString kErrorPrefix = QStringLiteral("ERROR:");
}

QString toolPath() {
    return QStandardPaths::findExecutable(QStringLiteral("yt-dlp"));
}

bool isLink(const QString &text) {
    const QString candidate = text.trimmed();
    if (candidate.isEmpty())
        return false;
    // Anything with whitespace inside it is prose that happens to hold a URL,
    // not a link the user meant to paste.
    for (const QChar c : candidate) {
        if (c.isSpace())
            return false;
    }

    const QUrl url(candidate, QUrl::StrictMode);
    return url.isValid() && !url.host().isEmpty()
        && (url.scheme() == QLatin1String("http") || url.scheme() == QLatin1String("https"));
}

QStringList downloadArgs(const QString &url, const QString &dir,
                         const QString &pathFile, int maxHeight) {
    // A "watch?v=...&list=..." link is a video the user is watching, not a
    // request for the whole playlist behind it.
    QStringList args = {"--no-playlist", "--no-warnings", "--newline"};

    // Machine-readable progress on stdout, mirroring ffmpeg's -progress, so the
    // percentage doesn't depend on scraping yt-dlp's human-facing output.
    args << "--progress-template"
         << QStringLiteral("download:") + kProgressMarker
                + QStringLiteral(" %(progress.downloaded_bytes)s"
                                 " %(progress.total_bytes)s"
                                 " %(progress.total_bytes_estimate)s");

    args << "--print-to-file" << "after_move:filepath" << pathFile;
    args << "-P" << dir << "-o" << "%(title).80B [%(id)s].%(ext)s";

    // Prefer a separate video and audio stream and let yt-dlp mux them, since
    // the pre-muxed formats sites still offer are the low-quality ones.
    args << "-f" << (maxHeight > 0
                         ? QStringLiteral("bv*[height<=%1]+ba/b[height<=%1]/b").arg(maxHeight)
                         : QStringLiteral("bv*+ba/b"))
         << "--merge-output-format" << "mp4";

    // Sites increasingly serve AV1, which plenty of machines can only decode in
    // software — a scrub-and-preview tool wants the codec that plays smoothly
    // everywhere, and it's the one exports are re-encoded to anyway. A sort
    // rather than a filter, so a source without h264 still downloads.
    args << "-S" << "vcodec:h264,acodec:aac,ext:mp4";

    // Fragmented (DASH/HLS) sources arrive far faster in parallel.
    args << "-N" << "4";

    args << url;
    return args;
}

double percentFromProgress(const QString &line) {
    const QStringList parts = line.trimmed().split(QLatin1Char(' '), Qt::SkipEmptyParts);
    if (parts.size() != 4 || parts.first() != kProgressMarker)
        return -1.0;

    bool ok = false;
    const double done = parts.at(1).toDouble(&ok);
    if (!ok)
        return -1.0;

    // total_bytes is "NA" on streams that only ever report an estimate.
    double total = parts.at(2).toDouble(&ok);
    if (!ok || total <= 0.0) {
        total = parts.at(3).toDouble(&ok);
        if (!ok || total <= 0.0)
            return -1.0;
    }

    return qBound(0.0, done / total * 100.0, 100.0);
}

QString errorFrom(const QString &stderrText) {
    const QStringList lines = stderrText.split(QLatin1Char('\n'), Qt::SkipEmptyParts);
    // The last ERROR: line is the one that stopped the download; anything above
    // it is warnings and progress noise not worth putting in front of the user.
    for (auto line = lines.crbegin(); line != lines.crend(); ++line) {
        const QString trimmed = line->trimmed();
        if (trimmed.startsWith(kErrorPrefix))
            return trimmed.mid(kErrorPrefix.size()).trimmed();
    }
    return lines.isEmpty() ? QStringLiteral("yt-dlp could not download that link.")
                           : lines.last().trimmed();
}

}  // namespace ytdlp
