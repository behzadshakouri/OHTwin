#include "DTRunner.h"

#include "System.h"
#include "Script.h"

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonObject>
#include <iostream>
#include <memory>
#include "VizRenderer.h"

// ---------------------------------------------------------------------------
// OHQ epoch: day-serial 0 = 1899-12-30 (Excel convention)
// ---------------------------------------------------------------------------
static const QDate kOHQEpoch(1899, 12, 30);

// ---------------------------------------------------------------------------
// ctor
// ---------------------------------------------------------------------------
DTRunner::DTRunner(const DTConfig &config, QObject *parent)
    : QObject(parent)
    , m_config(config)
{
    // Pre-compute interval length in OHQ day units
    m_intervalDays = static_cast<double>(config.intervalMs) / (86400.0 * 1000.0);
}

// ---------------------------------------------------------------------------
// init
// ---------------------------------------------------------------------------
bool DTRunner::init(QString &errorMessage)
{
    // Validate script file exists
    if (!m_config.loadModelJson.empty())
    {
        // hot-restart path checked at run-time (snapshot may not exist yet)
    }
    else
    {
        const QFileInfo fi(QString::fromStdString(m_config.scriptFile));
        if (!fi.exists())
        {
            errorMessage = "Script file not found: " +
                           QString::fromStdString(m_config.scriptFile);
            return false;
        }
    }

    // Determine the wall-clock start time for the first interval
    if (!m_config.startDatetime.empty())
    {
        m_nextIntervalStart = QDateTime::fromString(
            QString::fromStdString(m_config.startDatetime), Qt::ISODate);
        if (!m_nextIntervalStart.isValid())
        {
            errorMessage = "start_datetime in config is not a valid ISO 8601 string: " +
                           QString::fromStdString(m_config.startDatetime);
            return false;
        }
        std::cout << "[Runner] Using config start_datetime: "
                  << m_config.startDatetime << "\n";
    }
    else
    {
        // Check if a prior state snapshot already encodes a next-start time
        const QString latestSnapshot = findLatestStateSnapshot();
        if (!latestSnapshot.isEmpty())
        {
            const QJsonObject state = readJson(latestSnapshot);
            const QString nextStart = state.value("_dt_next_start_utc").toString();
            if (!nextStart.isEmpty())
            {
                m_nextIntervalStart = QDateTime::fromString(nextStart, Qt::ISODate);
            }
        }

        if (!m_nextIntervalStart.isValid())
        {
            // True fresh start: begin from now
            m_nextIntervalStart = QDateTime::currentDateTimeUtc();
            std::cout << "[Runner] No prior state found; starting from now: "
                      << m_nextIntervalStart.toString(Qt::ISODate).toStdString() << "\n";
        }
        else
        {
            std::cout << "[Runner] Resuming from previous state; next interval start: "
                      << m_nextIntervalStart.toString(Qt::ISODate).toStdString() << "\n";
        }
    }

    return true;
}

// ---------------------------------------------------------------------------
// runOnce  (the main per-interval entry point)
// ---------------------------------------------------------------------------
bool DTRunner::runOnce()
{
    const QDateTime intervalStart = m_nextIntervalStart;
    const QDateTime intervalEnd   = intervalStart.addMSecs(m_config.intervalMs);


    const double ohqStart = toOHQDaySerial(intervalStart);
    const double ohqEnd   = toOHQDaySerial(intervalEnd);

    std::cout << "\n[Runner] ---- Interval " << (m_runsCompleted + 1) << " ----\n"
              << "[Runner] Wall clock : "
              << intervalStart.toString(Qt::ISODate).toStdString()
              << "  →  "
              << intervalEnd.toString(Qt::ISODate).toStdString() << "\n"
              << "[Runner] OHQ serial : " << ohqStart << "  →  " << ohqEnd << "\n";

    // ------------------------------------------------------------------
    // 1.  Locate or build the model state JSON to feed this interval
    // ------------------------------------------------------------------
    const QString latestSnapshot = findLatestStateSnapshot();
    QString modelJsonPath;

    if (!latestSnapshot.isEmpty())
    {
        // Hot-restart: patch the window in the previous snapshot
        QJsonObject prevState = readJson(latestSnapshot);
        if (prevState.isEmpty())
        {
            std::cerr << "[Runner] Failed to read previous state: "
                      << latestSnapshot.toStdString() << "\n";
            return false;
        }

        const QJsonObject patched = patchSimulationWindow(prevState, ohqStart, ohqEnd);

        // Write patched state to a temp file that OHQ will load
        const QString patchedPath =
            QString::fromStdString(m_config.stateDir) + "/_current_input.json";
        if (!writeJson(patched, patchedPath))
        {
            std::cerr << "[Runner] Failed to write patched state\n";
            return false;
        }
        modelJsonPath = patchedPath;
        std::cout << "[Runner] Hot-restart from: " << latestSnapshot.toStdString() << "\n";
    }
    else
    {
        std::cout << "[Runner] Cold start from script: "
                  << m_config.scriptFile << "\n";
        // modelJsonPath stays empty → BuildAndSolve uses the script
    }

    // ------------------------------------------------------------------
    // 2.  Build and solve
    // ------------------------------------------------------------------
    const std::string defaultTemplatePath =
        QCoreApplication::applicationDirPath().toStdString() + "/../../resources/";
    const std::string settingsFile = defaultTemplatePath + "settings.json";

    std::unique_ptr<System> ohqSystem(new System());
    //std::cout << "[Runner] sizeof(System) = " << sizeof(System) << "\n";
    ohqSystem->SetDefaultTemplatePath(defaultTemplatePath);

    const QFileInfo scriptFi(QString::fromStdString(m_config.scriptFile));
    ohqSystem->SetWorkingFolder(
        QString::fromStdString(m_config.outputDir).toStdString() + "/");

    std::cout << "[Runner] Preparing model...\n";
    if (!modelJsonPath.isEmpty())
    {
        ohqSystem->ReadSystemSettingsTemplate(settingsFile);
        ohqSystem->LoadfromJson(modelJsonPath);
    }
    else
    {
        // First-ever run: build from script, then override the time window
        Script scr(m_config.scriptFile, ohqSystem.get());
        ohqSystem->CreateFromScript(scr, settingsFile);

        // Override the simulation window with what we want
        // (script may have hardcoded dates from calibration)
        ohqSystem->SetProp("simulation_start_time",ohqStart);
        ohqSystem->SetProp("simulation_end_time",ohqEnd);

    }

    ohqSystem->SetSilent(false);
    ohqSystem->CalcAllInitialValues();
    // Fetch and inject precipitation for this interval
    CPrecipitation precip = fetchPrecipitation(intervalStart, intervalEnd);
    const QString precipOutputFile =
        QString::fromStdString(m_config.outputDir) + "/" +
        intervalStart.toString("yyyyMMdd_HHmmss") + "_precipitation.txt";

    precip.writefile(precipOutputFile.toStdString());

    injectPrecipitation(ohqSystem.get(), precip);
    std::cout << "[Runner] Solving...\n";
    ohqSystem->Solve();

    // ------------------------------------------------------------------
    // 3.  Write simulation outputs to outputDir
    // ------------------------------------------------------------------
    const QString outputFile =
        QString::fromStdString(m_config.outputDir) + "/" +
        intervalStart.toString("yyyyMMdd_HHmmss") + "_output.txt";

    std::cout << "[Runner] Writing output to: " << outputFile.toStdString() << "\n";
    ohqSystem->GetOutputs().write(outputFile.toStdString());

    const QString selectedOutputFile =
        QString::fromStdString(m_config.outputDir) + "/selected_output.csv";

    if (!m_selectedOutputWritten) {
        ohqSystem->GetObservedOutputs().write(selectedOutputFile.toStdString());
        m_selectedOutputWritten = true;
    } else {
        ohqSystem->GetObservedOutputs().appendtofile(selectedOutputFile.toStdString());
    }
    std::cout << "[Runner] Selected output written to: " << selectedOutputFile.toStdString() << "\n";

    // ------------------------------------------------------------------
    // 4.  Export any requested state variables
    // ------------------------------------------------------------------
    for (const auto &exp : m_config.stateVarExports)
    {
        ohqSystem->SaveStateVariableToJson(exp.variable, exp.outputPath);
    }

    // ------------------------------------------------------------------
    // 5.  Save timestamped model snapshot (the "end state")
    // ------------------------------------------------------------------
    const QString snapshotFilename = makeSnapshotFilename(intervalEnd);
    const QString snapshotPath =
        QString::fromStdString(m_config.stateDir) + "/" + snapshotFilename;

    const QString modelSnapshotPath =
        QString::fromStdString(m_config.modelSnapshotDir) + "/" +
        intervalStart.toString("yyyyMMdd_HHmmss") + "_model.json";

    // Save the OHQ system state to the model snapshot dir
    ohqSystem->SavetoJson(modelSnapshotPath.toStdString(),
                       ohqSystem->addedtemplates, false, true);


    // Save full state (all derived/expression variables) for visualization
    const QString vizStatePath =
        QString::fromStdString(m_config.outputDir) + "/viz_state.json";
    ohqSystem->SaveFullStateTo(vizStatePath);
    std::cout << "[Runner] Viz state written to: " << vizStatePath.toStdString() << "\n";


    // Load it back so we can annotate it with runner metadata
    QJsonObject savedState = readJson(modelSnapshotPath);

    const QString vizJsonPath =
        QCoreApplication::applicationDirPath() + "/viz.json";
    const QString vizSvgPath  =
        QString::fromStdString(m_config.outputDir) + "/viz.svg";
    const QJsonObject fullState = readJson(vizStatePath);
    QString vizErr;
    if (!VizRenderer::render(vizJsonPath, fullState, vizSvgPath, vizErr))
        std::cerr << "[Runner] VizRenderer warning: " << vizErr.toStdString() << "\n";
    else
        std::cout << "[Runner] viz.svg written to: " << vizSvgPath.toStdString() << "\n";

    // Annotate with runner bookkeeping fields (prefixed _runner_ to
    // avoid clashing with OHQ keys)
    savedState["_dt_interval_start_utc"] = intervalStart.toString(Qt::ISODate);
    savedState["_dt_interval_end_utc"]   = intervalEnd.toString(Qt::ISODate);
    savedState["_dt_next_start_utc"]     = intervalEnd.toString(Qt::ISODate);
    savedState["_dt_runs_completed"]     = m_runsCompleted + 1;
    savedState["_dt_output_file"]        = outputFile;

    // Write the annotated snapshot to stateDir
    if (!writeJson(savedState, snapshotPath))
    {
        std::cerr << "[Runner] Warning: failed to write state snapshot\n";
        // Non-fatal: outputs were already written
    }
    else
    {
        std::cout << "[Runner] State snapshot: " << snapshotPath.toStdString() << "\n";
    }

    // ------------------------------------------------------------------
    // 6.  Advance timing for next interval
    // ------------------------------------------------------------------
    ++m_runsCompleted;
    m_nextIntervalStart = intervalEnd;

    std::cout << "[Runner] Interval complete. Next start: "
              << m_nextIntervalStart.toString(Qt::ISODate).toStdString() << "\n";
    return true;
}

// ---------------------------------------------------------------------------
// findLatestStateSnapshot
// Returns the full path of the most-recent state_YYYYMMDD_HHmmss.json file.
// ---------------------------------------------------------------------------
QString DTRunner::findLatestStateSnapshot() const
{
    const QDir dir(QString::fromStdString(m_config.stateDir));
    if (!dir.exists()) return {};

    const QStringList files = dir.entryList(
        QStringList() << "state_????????_??????.json",
        QDir::Files, QDir::Name | QDir::Reversed); // Reversed → newest first

    if (files.isEmpty()) return {};
    return dir.absoluteFilePath(files.first());
}

// ---------------------------------------------------------------------------
// makeSnapshotFilename
// ---------------------------------------------------------------------------
QString DTRunner::makeSnapshotFilename(const QDateTime &dt) const
{
    return "state_" + dt.toUTC().toString("yyyyMMdd_HHmmss") + ".json";
}

// ---------------------------------------------------------------------------
// toOHQDaySerial
// Days since 1899-12-30 (Excel epoch), with fractional day for time-of-day.
// ---------------------------------------------------------------------------
double DTRunner::toOHQDaySerial(const QDateTime &dt)
{
    const qint64 msSinceEpoch =
        QDateTime(kOHQEpoch, QTime(0, 0, 0), Qt::UTC).msecsTo(dt.toUTC());
    return static_cast<double>(msSinceEpoch) / (86400.0 * 1000.0);
}

// ---------------------------------------------------------------------------
// fromOHQDaySerial
// ---------------------------------------------------------------------------
QDateTime DTRunner::fromOHQDaySerial(double serial)
{
    const qint64 msOffset = static_cast<qint64>(serial * 86400.0 * 1000.0);
    return QDateTime(kOHQEpoch, QTime(0, 0, 0), Qt::UTC).addMSecs(msOffset);
}

// ---------------------------------------------------------------------------
// writeJson
// ---------------------------------------------------------------------------
bool DTRunner::writeJson(const QJsonObject &obj, const QString &path)
{
    QFile file(path);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate))
    {
        std::cerr << "[Runner] Cannot write: " << path.toStdString() << "\n";
        return false;
    }
    file.write(QJsonDocument(obj).toJson(QJsonDocument::Indented));
    return true;
}

// ---------------------------------------------------------------------------
// readJson
// ---------------------------------------------------------------------------
QJsonObject DTRunner::readJson(const QString &path)
{
    QFile file(path);
    if (!file.exists() || !file.open(QIODevice::ReadOnly))
        return {};
    const auto doc = QJsonDocument::fromJson(file.readAll());
    return doc.isObject() ? doc.object() : QJsonObject();
}

// ---------------------------------------------------------------------------
// patchSimulationWindow
// Updates simulation_start_time and simulation_end_time inside the
// Settings sub-object of an OHQ state JSON.
// ---------------------------------------------------------------------------
QJsonObject DTRunner::patchSimulationWindow(const QJsonObject &state,
                                            double startSerial,
                                            double endSerial)
{
    QJsonObject patched = state;

    QJsonObject settings = patched.value("Settings").toObject();
    settings["simulation_start_time"] = QString::number(startSerial, 'f', 6);
    settings["simulation_end_time"]   = QString::number(endSerial,   'f', 6);
    patched["Settings"] = settings;

    return patched;
}

// ---------------------------------------------------------------------------
// fetchPrecipitation
// Fetches NOAA quantitative precipitation for [intervalStart, intervalEnd]
// and returns a CPrecipitation object with bins in OHQ day-serial units.
// NOAA returns mm; bins outside the interval window are excluded.
// ---------------------------------------------------------------------------
CPrecipitation DTRunner::fetchPrecipitation(const QDateTime &intervalStart,
                                            const QDateTime &intervalEnd)
{
    CPrecipitation precip;

    NOAAWeatherFetcher fetcher;

    if (m_config.weatherSource == "openmeteo") {
        CPrecipitation precip = fetcher.getOpenMeteoPrecipitation(
            m_config.latitude, m_config.longitude,
            intervalStart, intervalEnd);
        if (precip.n == 0)
            std::cerr << "[Runner] Warning: " << fetcher.lastError().toStdString() << "\n";
        return precip;
    }

    // NOAA path
    const QVector<WeatherData> raw = fetcher.getWeatherPrediction(
        QString::fromStdString(m_config.noaaOffice),
        m_config.noaaGridX, m_config.noaaGridY,
        datatype::PrecipitationAmount);

    if (raw.isEmpty()) {
        std::cerr << "[Runner] Warning: no precipitation data returned from NOAA\n";
        return precip;
    }

    int skipped = 0;
    for (const WeatherData &wd : raw)
    {
        // Only include bins that overlap with [intervalStart, intervalEnd]
        if (wd.endTime <= intervalStart || wd.startTime >= intervalEnd) {
            ++skipped;
            continue;
        }

        // Clamp bin edges to the interval window
        const QDateTime binStart = qMax(wd.startTime, intervalStart);
        const QDateTime binEnd   = qMin(wd.endTime,   intervalEnd);

        const double s = toOHQDaySerial(binStart);
        const double e = toOHQDaySerial(binEnd);

        // NOAA quantitativePrecipitation is total mm over the bin duration.
        // CPrecipitation stores intensity * bin_width = total depth,
        // so i = value (mm) directly — getval() divides by (e-s) internally.
        precip.append(s, e, wd.value/1000.0);
    }

    std::cout << "[Runner] Precipitation bins loaded: " << precip.n
              << " (skipped " << skipped << " outside window)\n";

    return precip;
}

// ---------------------------------------------------------------------------
// injectPrecipitation  (placeholder)
// ---------------------------------------------------------------------------
void DTRunner::injectPrecipitation(System *system, const CPrecipitation &precip)
{
    system->source("rain")->Variable("timeseries")->SetTimeSeries(precip);
    (void)system;
    (void)precip;
}
