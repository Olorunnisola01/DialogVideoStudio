#include "SecretStore.h"

#include <QByteArray>
#include <QSettings>

#include <windows.h>

#include <dpapi.h>

namespace dvs::secrets {

namespace {

QSettings settings() {
    return QSettings(QStringLiteral("DialogVideoStudio"), QStringLiteral("DialogVideoStudio"));
}

QString settingsKey(const QString& name) {
    return QStringLiteral("secret/%1").arg(name);
}

QByteArray protect(const QByteArray& plain) {
    DATA_BLOB in{static_cast<DWORD>(plain.size()),
                 reinterpret_cast<BYTE*>(const_cast<char*>(plain.constData()))};
    DATA_BLOB out{};
    if (!CryptProtectData(&in, L"DialogVideoStudio", nullptr, nullptr, nullptr,
                          CRYPTPROTECT_UI_FORBIDDEN, &out)) {
        return {};
    }
    const QByteArray blob(reinterpret_cast<const char*>(out.pbData), static_cast<int>(out.cbData));
    LocalFree(out.pbData);
    return blob;
}

QByteArray unprotect(const QByteArray& blob) {
    if (blob.isEmpty()) return {};
    DATA_BLOB in{static_cast<DWORD>(blob.size()),
                 reinterpret_cast<BYTE*>(const_cast<char*>(blob.constData()))};
    DATA_BLOB out{};
    if (!CryptUnprotectData(&in, nullptr, nullptr, nullptr, nullptr, CRYPTPROTECT_UI_FORBIDDEN,
                            &out)) {
        return {};
    }
    const QByteArray plain(reinterpret_cast<const char*>(out.pbData), static_cast<int>(out.cbData));
    SecureZeroMemory(out.pbData, out.cbData);
    LocalFree(out.pbData);
    return plain;
}

} // namespace

void setApiKey(const QString& name, const QString& key) {
    QSettings s = settings();
    if (key.isEmpty()) {
        s.remove(settingsKey(name));
        return;
    }
    const QByteArray blob = protect(key.toUtf8());
    if (blob.isEmpty()) return; // encryption failed - better to store nothing
    s.setValue(settingsKey(name), blob.toBase64());
}

QString apiKey(const QString& name) {
    const QByteArray stored =
        QByteArray::fromBase64(settings().value(settingsKey(name)).toByteArray());
    return QString::fromUtf8(unprotect(stored));
}

bool hasApiKey(const QString& name) {
    return !settings().value(settingsKey(name)).toByteArray().isEmpty();
}

void clearApiKey(const QString& name) { settings().remove(settingsKey(name)); }

} // namespace dvs::secrets
