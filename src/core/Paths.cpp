#include "Paths.h"

#include <QCoreApplication>
#include <QDir>
#include <QFileInfo>
#include <QProcessEnvironment>
#include <QSettings>
#include <QStandardPaths>

namespace dvs {

namespace {

QSettings settings() {
    return QSettings(QStringLiteral("DialogVideoStudio"), QStringLiteral("DialogVideoStudio"));
}

bool isExecutable(const QString& path) {
    if (path.isEmpty()) return false;
    const QFileInfo fi(path);
    return fi.exists() && fi.isFile();
}

} // namespace

QString ffmpegPath() {
    const QString env =
        QProcessEnvironment::systemEnvironment().value(QStringLiteral("DVS_FFMPEG"));
    if (isExecutable(env)) return env;

    const QString override_ = settings().value(QStringLiteral("ffmpegPath")).toString();
    if (isExecutable(override_)) return override_;

    const QString bundled =
        QDir(QCoreApplication::applicationDirPath()).filePath(QStringLiteral("tools/ffmpeg.exe"));
    if (isExecutable(bundled)) return bundled;

    const QString onPath = QStandardPaths::findExecutable(QStringLiteral("ffmpeg"));
    if (isExecutable(onPath)) return onPath;

    return {};
}

void setFfmpegOverride(const QString& path) {
    QSettings s = settings();
    if (path.isEmpty()) {
        s.remove(QStringLiteral("ffmpegPath"));
    } else {
        s.setValue(QStringLiteral("ffmpegPath"), path);
    }
}

QString speakerModelPath() {
    const QDir models(QDir(QCoreApplication::applicationDirPath()).filePath(QStringLiteral("models")));
    // Any of the sherpa-onnx speaker-embedding exports works; prefer CAM++.
    const QStringList candidates = {
        QStringLiteral("wespeaker_en_voxceleb_CAM++.onnx"),
        QStringLiteral("3dspeaker_speech_campplus_sv_zh-cn_16k-common.onnx"),
        QStringLiteral("nemo_en_titanet_small.onnx"),
    };
    for (const QString& name : candidates) {
        if (models.exists(name)) return models.filePath(name);
    }
    // Fall back to whatever single .onnx sits in models/.
    const QStringList found = models.entryList({QStringLiteral("*.onnx")}, QDir::Files);
    if (found.size() == 1) return models.filePath(found.first());
    return {};
}

QString cacheDir() {
    const QString dir = QStandardPaths::writableLocation(QStandardPaths::AppLocalDataLocation);
    QDir().mkpath(dir);
    return dir;
}

} // namespace dvs
