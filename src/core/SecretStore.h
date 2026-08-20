#pragma once

#include <QString>

namespace dvs {

// API keys, kept out of the registry in plain text.
//
// Values are encrypted with the Windows Data Protection API before being
// stored, so the blob is tied to the current user account: another user on the
// same machine cannot read them, and copying the registry value elsewhere is
// useless. This is at-rest protection, not a vault - anything running as this
// user can still decrypt them, as it can for any app that stores credentials.
namespace secrets {

// `name` identifies the provider, e.g. "openai".
void setApiKey(const QString& name, const QString& key);
QString apiKey(const QString& name);
bool hasApiKey(const QString& name);
void clearApiKey(const QString& name);

} // namespace secrets
} // namespace dvs
