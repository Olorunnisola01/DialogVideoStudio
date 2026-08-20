#include "TranslationService.h"

#include <QEventLoop>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QRegularExpression>
#include <QTimer>
#include <QUrl>
#include <QUrlQuery>

#include <algorithm>

#include "Paths.h"

namespace dvs {

// --- provider table ---------------------------------------------------------

QList<ProviderInfo> translationProviders() {
    return {
        {TranslationProvider::GoogleFree, QStringLiteral("google"),
         QStringLiteral("Google Translate (no key)"), false, {}, {}},
        {TranslationProvider::MyMemory, QStringLiteral("mymemory"),
         QStringLiteral("MyMemory (no key)"), false, {}, {}},
        {TranslationProvider::OpenAI, QStringLiteral("openai"),
         QStringLiteral("OpenAI (ChatGPT)"), true, QStringLiteral("gpt-4o-mini"),
         QStringLiteral("https://platform.openai.com/api-keys")},
        {TranslationProvider::Anthropic, QStringLiteral("anthropic"),
         QStringLiteral("Anthropic (Claude)"), true, QStringLiteral("claude-opus-5"),
         QStringLiteral("https://platform.claude.com/settings/keys")},
        {TranslationProvider::Groq, QStringLiteral("groq"), QStringLiteral("Groq"), true,
         QStringLiteral("llama-3.3-70b-versatile"),
         QStringLiteral("https://console.groq.com/keys")},
        {TranslationProvider::OpenRouter, QStringLiteral("openrouter"),
         QStringLiteral("OpenRouter"), true, QStringLiteral("openai/gpt-4o-mini"),
         QStringLiteral("https://openrouter.ai/keys")},
    };
}

ProviderInfo providerInfo(TranslationProvider provider) {
    for (const ProviderInfo& info : translationProviders()) {
        if (info.provider == provider) return info;
    }
    return translationProviders().first();
}

bool providerFromId(const QString& id, TranslationProvider* out) {
    for (const ProviderInfo& info : translationProviders()) {
        if (info.id.compare(id, Qt::CaseInsensitive) == 0) {
            if (out) *out = info.provider;
            return true;
        }
    }
    return false;
}

namespace {

constexpr int kTimeoutMs = 60000;

QString languageName(const QString& code) {
    static const QHash<QString, QString> names = {
        {QStringLiteral("en"), QStringLiteral("English")},
        {QStringLiteral("de"), QStringLiteral("German")},
        {QStringLiteral("fr"), QStringLiteral("French")},
        {QStringLiteral("es"), QStringLiteral("Spanish")},
        {QStringLiteral("it"), QStringLiteral("Italian")},
        {QStringLiteral("pt"), QStringLiteral("Portuguese")},
        {QStringLiteral("nl"), QStringLiteral("Dutch")},
        {QStringLiteral("pl"), QStringLiteral("Polish")},
        {QStringLiteral("tr"), QStringLiteral("Turkish")},
        {QStringLiteral("ru"), QStringLiteral("Russian")},
        {QStringLiteral("ar"), QStringLiteral("Arabic")},
        {QStringLiteral("yo"), QStringLiteral("Yoruba")},
        {QStringLiteral("ha"), QStringLiteral("Hausa")},
        {QStringLiteral("ig"), QStringLiteral("Igbo")},
        {QStringLiteral("zh"), QStringLiteral("Chinese")},
        {QStringLiteral("ja"), QStringLiteral("Japanese")},
        {QStringLiteral("ko"), QStringLiteral("Korean")},
    };
    return names.value(code.left(2).toLower(), code);
}

// One blocking HTTP request with its own event loop, so the whole service can
// run on a worker thread. `body` null means GET.
QByteArray performRequest(QNetworkAccessManager& nam, const QNetworkRequest& request,
                          const QByteArray& body, QString* error, int* httpStatus = nullptr) {
    QEventLoop loop;
    QNetworkReply* reply = body.isNull() ? nam.get(request) : nam.post(request, body);

    QTimer timer;
    timer.setSingleShot(true);
    QObject::connect(&timer, &QTimer::timeout, reply, [reply] { reply->abort(); });
    QObject::connect(reply, &QNetworkReply::finished, &loop, &QEventLoop::quit);
    timer.start(kTimeoutMs);
    loop.exec();

    const QByteArray payload = reply->readAll();
    const int status =
        reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
    if (httpStatus) *httpStatus = status;

    if (reply->error() != QNetworkReply::NoError) {
        // Providers put the useful part in the body, not the Qt error string.
        QString detail = QString::fromUtf8(payload).trimmed();
        const QJsonDocument doc = QJsonDocument::fromJson(payload);
        if (doc.isObject()) {
            const QJsonObject err = doc.object().value(QStringLiteral("error")).toObject();
            if (err.contains(QStringLiteral("message"))) {
                detail = err.value(QStringLiteral("message")).toString();
            }
        }
        if (detail.size() > 400) detail = detail.left(400) + QStringLiteral("...");
        *error = status > 0
                     ? QStringLiteral("HTTP %1: %2").arg(status).arg(
                           detail.isEmpty() ? reply->errorString() : detail)
                     : reply->errorString();
    }

    reply->deleteLater();
    return payload;
}

// --- keyless providers ------------------------------------------------------

// The endpoint the Google Translate web page calls. No key and no quota to
// manage, but it is undocumented: it can rate-limit or change without notice,
// which is why MyMemory is offered as a documented keyless alternative.
QString googleFreeTranslate(QNetworkAccessManager& nam, const QString& text,
                            const TranslationSettings& settings, QString* error) {
    QUrl url(QStringLiteral("https://translate.googleapis.com/translate_a/single"));
    QUrlQuery query;
    query.addQueryItem(QStringLiteral("client"), QStringLiteral("gtx"));
    query.addQueryItem(QStringLiteral("sl"), settings.sourceLang);
    query.addQueryItem(QStringLiteral("tl"), settings.targetLang);
    query.addQueryItem(QStringLiteral("dt"), QStringLiteral("t"));
    query.addQueryItem(QStringLiteral("q"), text);
    url.setQuery(query);

    QNetworkRequest request(url);
    request.setHeader(QNetworkRequest::UserAgentHeader,
                      QStringLiteral("Mozilla/5.0 DialogVideoStudio"));

    const QByteArray payload = performRequest(nam, request, QByteArray(), error);
    if (!error->isEmpty()) return {};

    // [[["translated","source",null,...],...],null,"de",...]
    const QJsonDocument doc = QJsonDocument::fromJson(payload);
    if (!doc.isArray()) {
        *error = QStringLiteral("Unexpected reply from Google Translate.");
        return {};
    }
    QString out;
    for (const QJsonValue& chunk : doc.array().first().toArray()) {
        out += chunk.toArray().first().toString();
    }
    return out.trimmed();
}

QString myMemoryTranslate(QNetworkAccessManager& nam, const QString& text,
                          const TranslationSettings& settings, QString* error) {
    QUrl url(QStringLiteral("https://api.mymemory.translated.net/get"));
    QUrlQuery query;
    query.addQueryItem(QStringLiteral("q"), text);
    query.addQueryItem(QStringLiteral("langpair"),
                       QStringLiteral("%1|%2").arg(settings.sourceLang, settings.targetLang));
    url.setQuery(query);

    const QByteArray payload = performRequest(nam, QNetworkRequest(url), QByteArray(), error);
    if (!error->isEmpty()) return {};

    const QJsonObject root = QJsonDocument::fromJson(payload).object();
    const QString out = root.value(QStringLiteral("responseData"))
                            .toObject()
                            .value(QStringLiteral("translatedText"))
                            .toString();
    if (out.isEmpty()) {
        *error = root.value(QStringLiteral("responseDetails")).toString(
            QStringLiteral("MyMemory returned no translation."));
    }
    return out.trimmed();
}

// --- LLM providers ----------------------------------------------------------

QString batchPrompt(const QStringList& lines, const TranslationSettings& settings) {
    QJsonArray array;
    for (const QString& line : lines) array.append(line);
    return QStringLiteral(
               "Translate each of these %1 subtitle lines into %2.\n"
               "Return a JSON object of the form {\"translations\": [...]} whose array holds "
               "exactly %1 strings, in the same order as the input.\n\n%3")
        .arg(lines.size())
        .arg(languageName(settings.targetLang),
             QString::fromUtf8(QJsonDocument(array).toJson(QJsonDocument::Compact)));
}

QString systemPrompt(const TranslationSettings& settings) {
    return QStringLiteral(
               "You translate subtitles for a %1-to-%2 language-learning video. "
               "Produce natural spoken %2 that a learner can read at subtitle speed - "
               "not a word-for-word gloss. Keep each line's meaning, register and "
               "punctuation. Translate every line, even fragments; never merge, split, "
               "reorder or drop a line, and never add commentary.")
        .arg(languageName(settings.sourceLang), languageName(settings.targetLang));
}

QStringList parseTranslationArray(const QByteArray& text, int expected, QString* error) {
    QJsonDocument doc = QJsonDocument::fromJson(text);

    // Models sometimes wrap JSON in a ```json fence despite being told not to.
    if (doc.isNull()) {
        const QString asText = QString::fromUtf8(text);
        const int begin = asText.indexOf(QLatin1Char('{'));
        const int end = asText.lastIndexOf(QLatin1Char('}'));
        if (begin >= 0 && end > begin) {
            doc = QJsonDocument::fromJson(asText.mid(begin, end - begin + 1).toUtf8());
        }
    }

    QJsonArray array;
    if (doc.isObject()) {
        array = doc.object().value(QStringLiteral("translations")).toArray();
    } else if (doc.isArray()) {
        array = doc.array();
    }

    if (array.isEmpty()) {
        *error = QStringLiteral("The model did not return a usable list of translations.");
        return {};
    }
    if (array.size() != expected) {
        *error = QStringLiteral("The model returned %1 translations for %2 lines.")
                     .arg(array.size())
                     .arg(expected);
        return {};
    }

    QStringList out;
    out.reserve(array.size());
    for (const QJsonValue& v : array) out << v.toString().trimmed();
    return out;
}

// OpenAI, Groq and OpenRouter all speak the OpenAI chat-completions shape, so
// only the endpoint and the model differ.
QStringList openAiCompatibleBatch(QNetworkAccessManager& nam, const QStringList& lines,
                                  const TranslationSettings& settings, QString* error) {
    QString endpoint;
    switch (settings.provider) {
    case TranslationProvider::OpenAI:
        endpoint = QStringLiteral("https://api.openai.com/v1/chat/completions");
        break;
    case TranslationProvider::Groq:
        endpoint = QStringLiteral("https://api.groq.com/openai/v1/chat/completions");
        break;
    default:
        endpoint = QStringLiteral("https://openrouter.ai/api/v1/chat/completions");
        break;
    }

    QJsonObject body{
        {QStringLiteral("model"), settings.model},
        {QStringLiteral("messages"),
         QJsonArray{
             QJsonObject{{QStringLiteral("role"), QStringLiteral("system")},
                         {QStringLiteral("content"), systemPrompt(settings)}},
             QJsonObject{{QStringLiteral("role"), QStringLiteral("user")},
                         {QStringLiteral("content"), batchPrompt(lines, settings)}},
         }},
        {QStringLiteral("response_format"),
         QJsonObject{{QStringLiteral("type"), QStringLiteral("json_object")}}},
        {QStringLiteral("temperature"), 0.2},
    };

    QNetworkRequest request{QUrl(endpoint)};
    request.setHeader(QNetworkRequest::ContentTypeHeader, QStringLiteral("application/json"));
    request.setRawHeader("Authorization", QStringLiteral("Bearer %1").arg(settings.apiKey).toUtf8());
    if (settings.provider == TranslationProvider::OpenRouter) {
        request.setRawHeader("X-Title", "Dialog Video Studio");
    }

    const QByteArray payload =
        performRequest(nam, request, QJsonDocument(body).toJson(QJsonDocument::Compact), error);
    if (!error->isEmpty()) return {};

    const QJsonArray choices =
        QJsonDocument::fromJson(payload).object().value(QStringLiteral("choices")).toArray();
    if (choices.isEmpty()) {
        *error = QStringLiteral("The model returned no answer.");
        return {};
    }
    const QString content = choices.first()
                                .toObject()
                                .value(QStringLiteral("message"))
                                .toObject()
                                .value(QStringLiteral("content"))
                                .toString();
    return parseTranslationArray(content.toUtf8(), lines.size(), error);
}

QStringList anthropicBatch(QNetworkAccessManager& nam, const QStringList& lines,
                           const TranslationSettings& settings, QString* error) {
    // Structured outputs pin the reply to this schema, so there is no prose to
    // strip and no fenced-JSON guesswork.
    const QJsonObject schema{
        {QStringLiteral("type"), QStringLiteral("object")},
        {QStringLiteral("properties"),
         QJsonObject{{QStringLiteral("translations"),
                      QJsonObject{{QStringLiteral("type"), QStringLiteral("array")},
                                  {QStringLiteral("items"),
                                   QJsonObject{{QStringLiteral("type"),
                                                QStringLiteral("string")}}}}}}},
        {QStringLiteral("required"), QJsonArray{QStringLiteral("translations")}},
        {QStringLiteral("additionalProperties"), false},
    };

    QJsonObject body{
        {QStringLiteral("model"), settings.model},
        // Thinking counts against max_tokens on Claude Opus 5, where it is on by
        // default - leave headroom above the translations themselves.
        {QStringLiteral("max_tokens"), 8192},
        {QStringLiteral("system"), systemPrompt(settings)},
        {QStringLiteral("output_config"),
         QJsonObject{
             // Translation is not a reasoning task; low effort keeps it cheap.
             {QStringLiteral("effort"), QStringLiteral("low")},
             {QStringLiteral("format"),
              QJsonObject{{QStringLiteral("type"), QStringLiteral("json_schema")},
                          {QStringLiteral("schema"), schema}}},
         }},
        // A safety classifier can decline a request outright; this re-runs it on
        // Anthropic's recommended fallback model inside the same call.
        {QStringLiteral("fallbacks"), QStringLiteral("default")},
        {QStringLiteral("messages"),
         QJsonArray{QJsonObject{{QStringLiteral("role"), QStringLiteral("user")},
                                {QStringLiteral("content"), batchPrompt(lines, settings)}}}},
    };

    QNetworkRequest request{QUrl(QStringLiteral("https://api.anthropic.com/v1/messages"))};
    request.setHeader(QNetworkRequest::ContentTypeHeader, QStringLiteral("application/json"));
    request.setRawHeader("x-api-key", settings.apiKey.toUtf8());
    request.setRawHeader("anthropic-version", "2023-06-01");
    request.setRawHeader("anthropic-beta", "server-side-fallback-2026-07-01");

    const QByteArray payload =
        performRequest(nam, request, QJsonDocument(body).toJson(QJsonDocument::Compact), error);
    if (!error->isEmpty()) return {};

    const QJsonObject root = QJsonDocument::fromJson(payload).object();

    // A declined request is a normal 200 with an empty or partial body - check
    // before reading the content.
    if (root.value(QStringLiteral("stop_reason")).toString() == QLatin1String("refusal")) {
        *error = QStringLiteral(
            "Claude declined to translate this batch. Edit the wording of the lines "
            "involved, or translate them by hand.");
        return {};
    }

    // Thinking blocks come first and carry no text; take the text block, never
    // content[0].
    QString text;
    for (const QJsonValue& block : root.value(QStringLiteral("content")).toArray()) {
        const QJsonObject obj = block.toObject();
        if (obj.value(QStringLiteral("type")).toString() == QLatin1String("text")) {
            text = obj.value(QStringLiteral("text")).toString();
            break;
        }
    }
    if (text.isEmpty()) {
        *error = QStringLiteral("Claude returned an empty answer (stop reason: %1).")
                     .arg(root.value(QStringLiteral("stop_reason")).toString());
        return {};
    }

    return parseTranslationArray(text.toUtf8(), lines.size(), error);
}

bool isLlmProvider(TranslationProvider provider) {
    return providerInfo(provider).needsKey;
}

} // namespace

QString translateOne(const QString& text, const TranslationSettings& settings, QString* error) {
    QString localError;
    QString* err = error ? error : &localError;
    err->clear();

    QNetworkAccessManager nam;
    switch (settings.provider) {
    case TranslationProvider::GoogleFree:
        return googleFreeTranslate(nam, text, settings, err);
    case TranslationProvider::MyMemory:
        return myMemoryTranslate(nam, text, settings, err);
    default: {
        const QStringList out = settings.provider == TranslationProvider::Anthropic
                                    ? anthropicBatch(nam, {text}, settings, err)
                                    : openAiCompatibleBatch(nam, {text}, settings, err);
        return out.isEmpty() ? QString() : out.first();
    }
    }
}

TranslationReport translateSegments(QList<Segment>& segments,
                                    const TranslationSettings& settings,
                                    const std::atomic<bool>* cancelled,
                                    const std::function<void(int)>& progress) {
    TranslationReport report;

    const ProviderInfo info = providerInfo(settings.provider);
    if (info.needsKey && settings.apiKey.isEmpty()) {
        report.error = QStringLiteral("%1 needs an API key. Add one on the English tab.")
                           .arg(info.displayName);
        return report;
    }

    // Which lines actually need work.
    QList<int> pending;
    for (int i = 0; i < segments.size(); ++i) {
        const Segment& s = segments.at(i);
        if (s.text.trimmed().isEmpty()) continue;
        if (!s.translation.isEmpty() && !settings.overwriteExisting) {
            report.skipped++;
            continue;
        }
        pending.append(i);
    }
    if (pending.isEmpty()) {
        report.warning = QStringLiteral(
            "Every line already had a translation. Tick \"Replace existing\" to redo them.");
        return report;
    }

    QNetworkAccessManager nam;
    const auto report_ = [&](int done) {
        if (progress) progress(static_cast<int>(100.0 * done / pending.size()));
    };

    if (!isLlmProvider(settings.provider)) {
        // Keyless endpoints take one line per request.
        for (int done = 0; done < pending.size(); ++done) {
            if (cancelled && cancelled->load()) break;

            const int index = pending.at(done);
            QString error;
            const QString out = settings.provider == TranslationProvider::GoogleFree
                                    ? googleFreeTranslate(nam, segments.at(index).text, settings, &error)
                                    : myMemoryTranslate(nam, segments.at(index).text, settings, &error);
            if (error.isEmpty() && !out.isEmpty()) {
                segments[index].translation = out;
                report.translated++;
            } else {
                report.failed++;
                if (report.warning.isEmpty()) report.warning = error;
            }
            report_(done + 1);
        }
    } else {
        // One request per batch of lines, which is both faster and gives the
        // model the surrounding lines as context.
        const int batchSize = std::max(1, settings.batchSize);
        int done = 0;
        for (int start = 0; start < pending.size(); start += batchSize) {
            if (cancelled && cancelled->load()) break;

            const int count = std::min(batchSize, static_cast<int>(pending.size()) - start);
            QStringList lines;
            for (int k = 0; k < count; ++k) lines << segments.at(pending.at(start + k)).text;

            QString error;
            QStringList out = settings.provider == TranslationProvider::Anthropic
                                  ? anthropicBatch(nam, lines, settings, &error)
                                  : openAiCompatibleBatch(nam, lines, settings, &error);

            // A count mismatch loses the line-to-line mapping for the whole
            // batch, so retry those lines one at a time rather than guessing.
            if (out.size() != lines.size() && count > 1) {
                out.clear();
                for (const QString& line : lines) {
                    QString lineError;
                    const QStringList single =
                        settings.provider == TranslationProvider::Anthropic
                            ? anthropicBatch(nam, {line}, settings, &lineError)
                            : openAiCompatibleBatch(nam, {line}, settings, &lineError);
                    out << (single.size() == 1 ? single.first() : QString());
                    if (!lineError.isEmpty() && error.isEmpty()) error = lineError;
                }
            }

            for (int k = 0; k < count; ++k) {
                const QString value = k < out.size() ? out.at(k) : QString();
                if (value.isEmpty()) {
                    report.failed++;
                } else {
                    segments[pending.at(start + k)].translation = value;
                    report.translated++;
                }
            }
            if (!error.isEmpty() && report.warning.isEmpty()) report.warning = error;

            done += count;
            report_(done);
        }
    }

    if (report.translated == 0 && report.failed > 0) {
        report.error = report.warning.isEmpty()
                           ? QStringLiteral("No lines could be translated.")
                           : QStringLiteral("No lines could be translated. %1").arg(report.warning);
    } else if (report.failed > 0 && report.warning.isEmpty()) {
        report.warning = QStringLiteral("%1 line(s) could not be translated.").arg(report.failed);
    }
    return report;
}

} // namespace dvs
