#pragma once

#include <QString>
#include <QStringList>

// Thin wrapper around the yt-dlp command-line tool, in the same shape as
// ffmpeg.h: the argument lists and the output parsing live here, the process
// itself is run from the backend.
namespace ytdlp {

// True when text looks like something worth handing to yt-dlp. Which sites
// actually work is yt-dlp's business, not ours — this only keeps stray
// clipboard text from starting a download.
bool isLink(const QString &text);

// Build the argument list that downloads `url` into `dir` as a single mp4,
// capped at maxHeight so trimming a phone clip doesn't pull down 4K. The final
// file's path is written to pathFile, which is the only reliable way to learn
// the name yt-dlp settled on after merging — and it's written for an already
// downloaded file too, which is what makes reopening a link instant.
QStringList downloadArgs(const QString &url, const QString &dir,
                         const QString &pathFile, int maxHeight);

// The percentage from one machine-readable progress line, or -1 for any other
// line. Each file reports its own 0-100, so video and audio each run the bar
// once.
double percentFromProgress(const QString &line);

// What actually went wrong, out of everything yt-dlp wrote to stderr.
QString errorFrom(const QString &stderrText);

// The path to yt-dlp, or an empty string when it isn't installed.
QString toolPath();

}  // namespace ytdlp
