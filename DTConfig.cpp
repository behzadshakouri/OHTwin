#include "DTConfig.h"

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <iostream>

// ---------------------------------------------------------------------------
// helpers
// ---------------------------------------------------------------------------
static QString requireString(const QJsonObject &obj, const char *key, bool &ok)
{
    if (!obj.contains(key) || !obj[key].isString())
    {
        ok = false;
        return {};
    }
    return obj[key].toString();
}

// ---------------------------------------------------------------------------
// DTConfig::parseIntervalMs
// Accepts: <integer><unit>   where unit ∈ { s, min, hr, day }
// ---------------------------------------------------------------------------
qint64 DTConfig::parseIntervalMs(const std::string &s, QString &err)
{
    if (s.empty())
    {
        err = "interval string is empty";
        return -1;
    }

    // find where digits end
    size_t i = 0;
    while (i < s.size() && (std::isdigit(s[i]) || s[i] == '.'))
        ++i;

    if (i == 0)
    {
        err = QString("interval '%1' has no leading number").arg(QString::fromStdString(s));
        return -1;
    }

    const double value = std::stod(s.substr(0, i));
    const std::string unit = s.substr(i);

    qint64 multiplierMs = 0;
    if      (unit == "s")   multiplierMs = 1000LL;
    else if (unit == "min") multiplierMs = 60LL   * 1000;
    else if (unit == "hr")  multiplierMs = 3600LL * 1000;
    else if (unit == "day") multiplierMs = 86400LL * 1000;
    else
    {
        err = QString("unknown interval unit '%1' (use s, min, hr, day)")
        .arg(QString::fromStdString(unit));
        return -1;
    }

    const qint64 result = static_cast<qint64>(value * multiplierMs);
    if (result <= 0)
    {
        err = QString("interval must be > 0, got '%1'").arg(QString::fromStdString(s));
        return -1;
    }
    return result;
}

// ---------------------------------------------------------------------------
// DTConfig::load
// ---------------------------------------------------------------------------
bool DTConfig::load(QString &errorMessage)
{
    const QString configPath =
        QCoreApplication::applicationDirPath() + "/config.json";

    QFile file(configPath);
    if (!file.exists())
    {
        errorMessage = "config.json not found at: " + configPath;
        return false;
    }
    if (!file.open(QIODevice::ReadOnly))
    {
        errorMessage = "Cannot open config.json: " + configPath;
        return false;
    }

    QJsonParseError parseErr;
    const QJsonDocument doc = QJsonDocument::fromJson(file.readAll(), &parseErr);
    if (doc.isNull())
    {
        errorMessage = "config.json parse error: " + parseErr.errorString();
        return false;
    }
    if (!doc.isObject())
    {
        errorMessage = "config.json root must be a JSON object";
        return false;
    }

    raw = doc.object();
    bool ok = true;

    // --- required fields ---
    scriptFile       = requireString(raw, "script_file",       ok).toStdString();
    stateDir         = requireString(raw, "state_dir",         ok).toStdString();
    outputDir        = requireString(raw, "output_dir",        ok).toStdString();
    modelSnapshotDir = requireString(raw, "model_snapshot_dir",ok).toStdString();

    if (!ok)
    {
        errorMessage = "config.json is missing one or more required string fields: "
                       "script_file, state_dir, output_dir, model_snapshot_dir";
        return false;
    }

    // --- optional fields ---
    loadModelJson = raw.value("load_model_json").toString().toStdString();
    weatherFile   = raw.value("weather_file").toString().toStdString();
    startDatetime = raw.value("start_datetime").toString().toStdString();
    intervalStr   = raw.value("interval").toString("1day").toStdString();

    // --- parse interval ---
    QString intervalErr;
    intervalMs = parseIntervalMs(intervalStr, intervalErr);
    if (intervalMs < 0)
    {
        errorMessage = "config.json interval error: " + intervalErr;
        return false;
    }

    // --- state_variables array ---
    if (raw.contains("state_variables") && raw["state_variables"].isArray())
    {
        const QJsonArray arr = raw["state_variables"].toArray();
        for (const auto &entry : arr)
        {
            if (!entry.isObject()) continue;
            const QJsonObject obj = entry.toObject();
            StateVarExport exp;
            exp.variable   = obj.value("variable").toString().toStdString();
            exp.outputPath = obj.value("output_path").toString().toStdString();
            if (!exp.variable.empty() && !exp.outputPath.empty())
                stateVarExports.push_back(exp);
        }
    }

    // --- ensure directories exist ---
    for (const auto &dir : { stateDir, outputDir, modelSnapshotDir })
    {
        if (!dir.empty())
            QDir().mkpath(QString::fromStdString(dir));
    }

    std::cout << "[Config] script_file       : " << scriptFile       << "\n";
    if (!loadModelJson.empty())
        std::cout << "[Config] load_model_json   : " << loadModelJson   << "\n";
    std::cout << "[Config] state_dir         : " << stateDir         << "\n"
              << "[Config] output_dir        : " << outputDir        << "\n"
              << "[Config] model_snapshot_dir: " << modelSnapshotDir << "\n"
              << "[Config] interval          : " << intervalStr
              << " (" << intervalMs << " ms)\n";

    if (!startDatetime.empty())
        std::cout << "[Config] start_datetime    : " << startDatetime << "\n";

    return true;
}
