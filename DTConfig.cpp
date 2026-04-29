#include "DTConfig.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonValue>

#include <cctype>
#include <iostream>

// ---------------------------------------------------------------------------
// DTConfig::parseIntervalMs
// ---------------------------------------------------------------------------
qint64 DTConfig::parseIntervalMs(const std::string &s, QString &err)
{
    const QString raw = QString::fromStdString(s).trimmed();
    const std::string trimmed = raw.toStdString();

    if (trimmed.empty())
    {
        err = "interval string is empty";
        return -1;
    }

    size_t i = 0;
    while (i < trimmed.size() &&
           (std::isdigit(static_cast<unsigned char>(trimmed[i])) || trimmed[i] == '.'))
        ++i;

    if (i == 0)
    {
        err = QString("interval '%1' has no leading number").arg(raw);
        return -1;
    }

    double value = 0.0;
    try
    {
        value = std::stod(trimmed.substr(0, i));
    }
    catch (...)
    {
        err = QString("invalid numeric value in interval '%1'").arg(raw);
        return -1;
    }

    const std::string unit = trimmed.substr(i);

    qint64 multiplierMs = 0;
    if      (unit == "s")   multiplierMs = 1000LL;
    else if (unit == "min") multiplierMs = 60LL   * 1000LL;
    else if (unit == "hr")  multiplierMs = 3600LL * 1000LL;
    else if (unit == "day") multiplierMs = 86400LL * 1000LL;
    else
    {
        err = QString("unknown interval unit '%1' (use s, min, hr, day)")
        .arg(QString::fromStdString(unit));
        return -1;
    }

    const qint64 result = static_cast<qint64>(value * static_cast<double>(multiplierMs));
    if (result <= 0)
    {
        err = QString("interval must be > 0, got '%1'").arg(raw);
        return -1;
    }

    return result;
}

// ---------------------------------------------------------------------------
// DTConfig::resolvePath
// ---------------------------------------------------------------------------
QString DTConfig::resolvePath(const QString &p) const
{
    if (p.isEmpty()) return {};
    if (QDir::isAbsolutePath(p)) return QDir::cleanPath(p);
    return QDir::cleanPath(QString::fromStdString(deploymentRoot) + "/" + p);
}

// ---------------------------------------------------------------------------
// DTConfig::load
// ---------------------------------------------------------------------------
bool DTConfig::load(const QString &deploymentRootIn, QString &errorMessage)
{
    // ------------------------------------------------------------------
    // Resolve deployment root
    // ------------------------------------------------------------------
    const QFileInfo rootInfo(deploymentRootIn);
    if (!rootInfo.exists() || !rootInfo.isDir())
    {
        errorMessage = "deployment path is not a directory: " + deploymentRootIn;
        return false;
    }
    deploymentRoot = rootInfo.absoluteFilePath().toStdString();

    const QString configPath =
        QDir(QString::fromStdString(deploymentRoot)).absoluteFilePath("config.json");

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

    const QJsonObject root = doc.object();
    stateVarExports.clear();

    // ------------------------------------------------------------------
    // deployment{}
    // ------------------------------------------------------------------
    if (!root.contains("deployment") || !root.value("deployment").isObject())
    {
        errorMessage = "config.json is missing required 'deployment' object";
        return false;
    }
    const QJsonObject dep = root.value("deployment").toObject();

    deploymentName = dep.value("name").toString().trimmed().toStdString();
    if (deploymentName.empty())
    {
        // Fall back to the directory name if the config omits it.
        deploymentName = rootInfo.fileName().toStdString();
    }

    port = dep.value("port").toInt(0);
    if (port <= 0)
    {
        errorMessage = "config.json deployment.port must be a positive integer";
        return false;
    }

    const QString modelFileQ = dep.value("model_file").toString().trimmed();
    if (modelFileQ.isEmpty())
    {
        errorMessage = "config.json deployment.model_file is required";
        return false;
    }
    scriptFile = resolvePath(modelFileQ).toStdString();

    const QString vizFileQ = dep.value("viz_file").toString().trimmed();
    if (vizFileQ.isEmpty())
    {
        errorMessage = "config.json deployment.viz_file is required";
        return false;
    }
    vizFile = resolvePath(vizFileQ).toStdString();

    // ------------------------------------------------------------------
    // runtime{}
    // ------------------------------------------------------------------
    if (!root.contains("runtime") || !root.value("runtime").isObject())
    {
        errorMessage = "config.json is missing required 'runtime' object";
        return false;
    }
    const QJsonObject rt = root.value("runtime").toObject();

    weatherSource = rt.value("weather_source").toString("openmeteo").trimmed().toStdString();
    latitude      = rt.value("latitude").toDouble(0.0);
    longitude     = rt.value("longitude").toDouble(0.0);

    startDatetime      = rt.value("start_datetime").toString().trimmed().toStdString();
    intervalStr        = rt.value("interval").toString("1day").trimmed().toStdString();
    forecastHorizonStr = rt.value("forecast_horizon").toString().trimmed().toStdString();

    QString intervalErr;
    intervalMs = parseIntervalMs(intervalStr, intervalErr);
    if (intervalMs < 0)
    {
        errorMessage = "config.json runtime.interval error: " + intervalErr;
        return false;
    }

    if (!forecastHorizonStr.empty())
    {
        QString horizonErr;
        forecastHorizonMs = parseIntervalMs(forecastHorizonStr, horizonErr);
        if (forecastHorizonMs < 0)
        {
            errorMessage = "config.json runtime.forecast_horizon error: " + horizonErr;
            return false;
        }
    }
    else
    {
        forecastHorizonMs = 0;
    }

    // Optional cold-start / weather paths (relative to deployment root).
    loadModelJson =
        resolvePath(rt.value("load_model_json").toString().trimmed()).toStdString();
    weatherFile =
        resolvePath(rt.value("weather_file").toString().trimmed()).toStdString();

    // ------------------------------------------------------------------
    // state_variables (relative output_paths -> deployment root)
    // ------------------------------------------------------------------
    const QJsonArray stateVars = rt.value("state_variables").toArray();
    for (const auto &entry : stateVars)
    {
        if (!entry.isObject()) continue;

        const QJsonObject obj = entry.toObject();
        StateVarExport exp;
        exp.variable   = obj.value("variable").toString().toStdString();
        exp.outputPath = resolvePath(obj.value("output_path").toString()).toStdString();

        if (!exp.variable.empty() && !exp.outputPath.empty())
            stateVarExports.push_back(exp);
    }

    // ------------------------------------------------------------------
    // Auto-derived working directories under the deployment root
    // ------------------------------------------------------------------
    const QString rootQ = QString::fromStdString(deploymentRoot);
    stateDir         = QDir(rootQ).absoluteFilePath("state").toStdString();
    outputDir        = QDir(rootQ).absoluteFilePath("outputs").toStdString();
    modelSnapshotDir = QDir(rootQ).absoluteFilePath("snapshots").toStdString();

    for (const auto &dir : { stateDir, outputDir, modelSnapshotDir })
        QDir().mkpath(QString::fromStdString(dir));

    // ------------------------------------------------------------------
    // Sanity-check that the model and viz files exist
    // ------------------------------------------------------------------
    if (!QFileInfo::exists(QString::fromStdString(scriptFile)))
    {
        errorMessage = "model_file does not exist: " + QString::fromStdString(scriptFile);
        return false;
    }
    if (!QFileInfo::exists(QString::fromStdString(vizFile)))
    {
        errorMessage = "viz_file does not exist: " + QString::fromStdString(vizFile);
        return false;
    }

    // ------------------------------------------------------------------
    // Log resolved configuration
    // ------------------------------------------------------------------
    std::cout << "[Config] config.json       : " << configPath.toStdString() << "\n"
              << "[Config] deployment_root   : " << deploymentRoot << "\n"
              << "[Config] deployment_name   : " << deploymentName << "\n"
              << "[Config] port              : " << port << "\n"
              << "[Config] script_file       : " << scriptFile << "\n"
              << "[Config] viz_file          : " << vizFile << "\n"
              << "[Config] state_dir         : " << stateDir << "\n"
              << "[Config] output_dir        : " << outputDir << "\n"
              << "[Config] model_snapshot_dir: " << modelSnapshotDir << "\n"
              << "[Config] weather_source    : " << weatherSource << "\n"
              << "[Config] latitude          : " << latitude << "\n"
              << "[Config] longitude         : " << longitude << "\n"
              << "[Config] interval          : " << intervalStr
              << " (" << intervalMs << " ms)\n";

    if (forecastHorizonMs > 0)
        std::cout << "[Config] forecast_horizon  : " << forecastHorizonStr
                  << " (" << forecastHorizonMs << " ms)\n";
    else
        std::cout << "[Config] forecast_horizon  : disabled\n";

    if (!startDatetime.empty())
        std::cout << "[Config] start_datetime    : " << startDatetime << "\n";

    if (!loadModelJson.empty())
        std::cout << "[Config] load_model_json   : " << loadModelJson << "\n";

    if (!weatherFile.empty())
        std::cout << "[Config] weather_file      : " << weatherFile << "\n";

    return true;
}
