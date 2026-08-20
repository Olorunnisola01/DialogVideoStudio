#pragma once

#include <QList>
#include <QString>

#include <atomic>
#include <functional>

#include "Segmenter.h"

namespace dvs {

enum class TranslationProvider {
    GoogleFree,  // no key, the endpoint the Google Translate web page itself calls
    MyMemory,    // no key, documented free tier
    OpenAI,
    Anthropic,
    Groq,
    OpenRouter,
};

struct ProviderInfo {
    TranslationProvider provider;
    QString id;           // stable key for settings, e.g. "openai"
    QString displayName;
    bool needsKey = false;
    QString defaultModel;
    QString keyUrl;       // where to get a key, shown in the UI
};

QList<ProviderInfo> translationProviders();
ProviderInfo providerInfo(TranslationProvider provider);
bool providerFromId(const QString& id, TranslationProvider* out);

struct TranslationSettings {
    TranslationProvider provider = TranslationProvider::GoogleFree;
    QString sourceLang = QStringLiteral("de");
    QString targetLang = QStringLiteral("en");
    QString model;    // LLM providers only; empty uses the provider default
    QString apiKey;   // LLM providers only
    bool overwriteExisting = false; // leave lines that already have a translation
    int batchSize = 20;             // lines per request, LLM providers only
};

struct TranslationReport {
    int translated = 0;
    int skipped = 0;
    int failed = 0;
    QString error;   // set only when nothing could be done at all
    QString warning; // partial failures

    bool ok() const { return error.isEmpty(); }
};

// Fills Segment::translation for every line that needs one.
//
// This sends subtitle text to the chosen provider over the network. Blocking -
// call it from a worker thread. `cancelled` is polled between requests.
TranslationReport translateSegments(QList<Segment>& segments,
                                    const TranslationSettings& settings,
                                    const std::atomic<bool>* cancelled = nullptr,
                                    const std::function<void(int)>& progress = {});

// Translates a single string. Used by the "Test" button so a provider and key
// can be checked without touching the project.
QString translateOne(const QString& text, const TranslationSettings& settings, QString* error);

} // namespace dvs
