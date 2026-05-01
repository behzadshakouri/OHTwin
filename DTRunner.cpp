#include "DTRunner.h"

#include "System.h"
#include "Script.h"
#include "DTAssimilation.h"

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonObject>
#include <cmath>
#include <iostream>
#include <memory>
#include <random>
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
    m_intervalDays  = static_cast<double>(config.intervalMs) / (86400.0 * 1000.0);
    m_forecastDays  = static_cast<double>(config.forecastHorizonMs) / (86400.0 * 1000.0);

    // Pre-compute observation save cadence in OHQ day units. If the
    // observations block was omitted, DTConfig::load() already defaulted
    // saveIntervalMs to intervalMs, so this falls through to the model
    // interval naturally. The defensive >0 guard is just belt-and-suspenders
    // against a future code path that might leave it zero.
    m_obsSaveIntervalDays =
        static_cast<double>(config.observations.saveIntervalMs) / (86400.0 * 1000.0);
    if (m_obsSaveIntervalDays <= 0.0)
        m_obsSaveIntervalDays = m_intervalDays;

    std::cout << "[Runner] obs save interval : "
              << m_obsSaveIntervalDays << " day(s)\n";
    if (config.observations.noiseSigma > 0.0)
    {
        const double tauDays =
            static_cast<double>(config.observations.noiseCorrelationTimeMs)
            / (86400.0 * 1000.0);
        std::cout << "[Runner] obs noise sigma   : "
                  << config.observations.noiseSigma << "\n"
                  << "[Runner] obs noise tau     : " << tauDays
                  << " day(s)";
        if (tauDays <= 0.0) std::cout << " (white-noise limit)";
        std::cout << "\n";
    }
    else
    {
        std::cout << "[Runner] obs noise         : disabled (sigma=0)\n";
    }
}

// Out-of-line destructor: required so that unique_ptr<DTAssimilation>'s
// destruction can see the full DTAssimilation type (declared via the
// #include at the top of this file).
DTRunner::~DTRunner() = default;

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
        m_nextIntervalStart.setTimeSpec(Qt::UTC);   // <-- ADD THIS
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

    // ------------------------------------------------------------------
    // Data assimilation (optional). Constructed only if the assimilation
    // block is present in config.json. Failure here is fatal: we don't
    // want to silently run a forward-only twin when the operator
    // explicitly configured assimilation.
    // ------------------------------------------------------------------
    if (m_config.assimilation.enabled)
    {
        m_assimilation.reset(new DTAssimilation(m_config, this));

        // Diagnostic logging on poll outcomes.
        QObject::connect(m_assimilation.get(), &DTAssimilation::buffered,
                         this, [](qint64 n) {
            std::cout << "[Assim] poll OK — " << n << " points buffered\n";
        });
        QObject::connect(m_assimilation.get(), &DTAssimilation::pollFailed,
                         this, [](const QString &err) {
            std::cerr << "[Assim] poll failed: " << err.toStdString() << "\n";
        });

        QString assimErr;
        if (!m_assimilation->start(assimErr))
        {
            errorMessage = "DTAssimilation::start() failed: " + assimErr;
            return false;
        }
    }
    else
    {
        std::cout << "[Runner] Assimilation disabled (no 'assimilation' block in config).\n";
    }

    return true;
}

// ---------------------------------------------------------------------------
// renderOnly
// Regenerates SVG files without running OHQ, fetching weather, updating state
// snapshots, or touching the selected-output stream.
//
// Preferred path:
//   outputs/viz_state.json          + deployment viz_*.json -> outputs/viz.svg
//   outputs/forecast_viz_state.json + deployment viz_*.json -> outputs/forecast_viz.svg
//
// Static fallback:
//   If a viz-state JSON file does not exist (or is empty/invalid), render from
//   an empty QJsonObject. This lets a deployment produce a static layout SVG
//   directly from config.json + viz_*.json before any simulation has run.
// ---------------------------------------------------------------------------
bool DTRunner::renderOnly()
{
    const QString vizJsonPath = QString::fromStdString(m_config.vizFile);
    const QString outputDir   = QString::fromStdString(m_config.outputDir);

    QDir().mkpath(outputDir);

    const QFileInfo vizInfo(vizJsonPath);
    if (!vizInfo.exists() || !vizInfo.isFile())
    {
        std::cerr << "[Runner] render-only: viz spec not found: "
                  << vizJsonPath.toStdString() << "\n";
        return false;
    }

    bool okAny = false;

    auto renderOne = [&](const QString &statePath,
                         const QString &svgPath,
                         const char *label) -> bool
    {
        QJsonObject fullState;
        const QFileInfo stateInfo(statePath);

        if (stateInfo.exists() && stateInfo.isFile())
        {
            fullState = readJson(statePath);
            if (fullState.isEmpty())
            {
                std::cout << "[Runner] render-only: " << label
                          << " state is empty/invalid; rendering static layout from "
                          << vizJsonPath.toStdString() << "\n";
            }
            else
            {
                std::cout << "[Runner] render-only: using "
                          << statePath.toStdString() << "\n";
            }
        }
        else
        {
            std::cout << "[Runner] render-only: " << label
                      << " state not found; rendering static layout from "
                      << vizJsonPath.toStdString() << "\n";
        }

        QString err;
        if (!VizRenderer::render(vizJsonPath, fullState, svgPath, err))
        {
            std::cerr << "[Runner] render-only " << label << " warning: "
                      << err.toStdString() << "\n";
            return false;
        }

        std::cout << "[Runner] render-only wrote: "
                  << svgPath.toStdString() << "\n";
        return true;
    };

    okAny = renderOne(outputDir + "/viz_state.json",
                      outputDir + "/viz.svg",
                      "advance") || okAny;

    okAny = renderOne(outputDir + "/forecast_viz_state.json",
                      outputDir + "/forecast_viz.svg",
                      "forecast") || okAny;

    return okAny;
}

// ---------------------------------------------------------------------------
// runOnce  (orchestrator: Advance, then optional Forecast)
// ---------------------------------------------------------------------------
bool DTRunner::runOnce()
{
    const QDateTime advanceStart = m_nextIntervalStart;
    const QDateTime advanceEnd   = advanceStart.addMSecs(m_config.intervalMs);

    std::cout << "\n[Runner] ======== Cycle " << (m_runsCompleted + 1) << " ========\n";

    // ------------------------------------------------------------------
    // Build / locate the initial-condition JSON used by BOTH stages.
    // (Stages run independently from the same starting state.)
    // ------------------------------------------------------------------
    const QString latestSnapshot = findLatestStateSnapshot();
    QString initialModelJsonPath;

    if (!latestSnapshot.isEmpty())
    {
        QJsonObject prevState = readJson(latestSnapshot);
        if (prevState.isEmpty())
        {
            std::cerr << "[Runner] Failed to read previous state: "
                      << latestSnapshot.toStdString() << "\n";
            return false;
        }

        // We will re-patch the simulation window inside runStage() per stage,
        // but writing the prev state once gives both stages a stable base file.
        initialModelJsonPath =
            QString::fromStdString(m_config.stateDir) + "/_current_input.json";
        if (!writeJson(prevState, initialModelJsonPath))
        {
            std::cerr << "[Runner] Failed to write base initial-condition state\n";
            return false;
        }
        std::cout << "[Runner] Initial condition: " << latestSnapshot.toStdString() << "\n";
    }
    else
    {
        std::cout << "[Runner] Initial condition: cold start from script "
                  << m_config.scriptFile << "\n";
        // initialModelJsonPath stays empty → runStage() cold-starts from script
    }

    // ------------------------------------------------------------------
    // Stage A — Advance [t, t+Δ]
    // ------------------------------------------------------------------
    const StageResult advance = runStage(StageKind::Advance,
                                         advanceStart, advanceEnd,
                                         initialModelJsonPath);
    if (!advance.ok)
    {
        std::cerr << "[Runner] Advance stage failed.\n";
        return false;
    }

    // ------------------------------------------------------------------
    // Stage B — Forecast [t, t+Δ+H]   (optional)
    // ------------------------------------------------------------------
    StageResult forecast;
    forecast.ok = false;

    if (m_forecastDays > 0.0)
    {
        const QDateTime forecastEnd =
            advanceStart.addMSecs(m_config.intervalMs + m_config.forecastHorizonMs);

        forecast = runStage(StageKind::Forecast,
                            advanceStart, forecastEnd,
                            initialModelJsonPath);
        if (!forecast.ok)
        {
            std::cerr << "[Runner] Forecast stage failed (continuing).\n";
            // Non-fatal: Advance already updated state for next cycle.
        }
    }
    else
    {
        std::cout << "[Runner] Forecast stage disabled (forecast_horizon = 0).\n";
    }

    // ------------------------------------------------------------------
    // Merge Advance + Forecast observed outputs into selected_output.csv
    //   - actuals: Advance series  [t, t+Δ]
    //   - forecast tail: Forecast series with t > t+Δ
    //   - knockout at (t - ε) drops the previous cycle's forecast tail
    // If Forecast is disabled or failed, only the Advance rows are merged
    // (and the file behaves as a pure actuals stream).
    // ------------------------------------------------------------------
    {
        const double cutoffSerial = toOHQDaySerial(advanceStart);

        // ------------------------------------------------------------
        // Step 0: OHQ's observed-output series record the integration's
        //         t=0 sample as 0 (initial-state placeholder before the
        //         first solver step). Left in place this produces a
        //         visible "half-value" artifact at every cycle boundary
        //         after make_uniform interpolates from 0 up to the first
        //         real value. Replace each series' first sample with its
        //         second sample so make_uniform interpolates between two
        //         comparable values. No-op if a series has fewer than
        //         two points.
        // ------------------------------------------------------------
        TimeSeriesSet<double> advanceClean = advance.observed;
        for (size_t s = 0; s < advanceClean.size(); ++s)
        {
            TimeSeries<double> &ts = advanceClean[s];
            if (ts.size() >= 2)
                ts.setValue(0, ts.getValue(1));
        }

        // ------------------------------------------------------------
        // Step 1: resample Advance observed outputs to the configured
        //         observation cadence (m_obsSaveIntervalDays).
        // ------------------------------------------------------------
        TimeSeriesSet<double> advanceProcessed =
            advanceClean.make_uniform(m_obsSaveIntervalDays);

        // ------------------------------------------------------------
        // Step 2: apply correlated multiplicative log-normal noise
        //         using an OU process whose state persists across
        //         cycles (m_ouState). No-op if sigma == 0.
        // ------------------------------------------------------------
        if (m_config.observations.noiseSigma > 0.0)
        {
            const double tauDays =
                static_cast<double>(m_config.observations.noiseCorrelationTimeMs)
                / (86400.0 * 1000.0);
            applyOUNoiseStateful(advanceProcessed,
                                 m_config.observations.noiseSigma,
                                 tauDays);
        }

        // ------------------------------------------------------------
        // Step 3: forecast tail left clean. The Truth Twin doesn't run
        //         a forecast stage anyway; if a future config does and
        //         needs noise, snapshot/restore m_ouState around a
        //         noised forecast block here.
        // ------------------------------------------------------------
        TimeSeriesSet<double> emptyForecast;
        const TimeSeriesSet<double> &forecastObs =
            forecast.ok ? forecast.observed : emptyForecast;

        if (!mergeIntoSelectedOutput(advanceProcessed, forecastObs, cutoffSerial))
            std::cerr << "[Runner] selected_output.csv merge failed (continuing).\n";

        // ------------------------------------------------------------
        // Step 4: refresh the metadata sidecar (cheap; describes the
        //         noise model that produced selected_output.csv).
        // ------------------------------------------------------------
        if (!writeMetadataSidecar())
            std::cerr << "[Runner] selected_output_meta.json write failed (continuing).\n";
    }

    // ------------------------------------------------------------------
    // Advance timing for next cycle
    // ------------------------------------------------------------------
    ++m_runsCompleted;
    m_nextIntervalStart = advanceEnd;

    std::cout << "[Runner] Cycle complete. Next cycle start: "
              << m_nextIntervalStart.toString(Qt::ISODate).toStdString() << "\n";

    // ------------------------------------------------------------------
    // Stop condition: configured stop_datetime reached
    // ------------------------------------------------------------------
    if (!m_config.stopDatetime.empty())
    {
        const QDateTime stopAt = QDateTime::fromString(
            QString::fromStdString(m_config.stopDatetime), Qt::ISODate);
        if (stopAt.isValid() && m_nextIntervalStart >= stopAt)
        {
            std::cout << "[Runner] stop_datetime reached ("
                      << m_config.stopDatetime
                      << "). Requesting clean shutdown.\n";
            QCoreApplication::quit();
        }
    }

    return true;
}

// ---------------------------------------------------------------------------
// runStage
// Performs a single solve over [stageStart, stageEnd] starting from
// `modelJsonPath` (empty → cold start from script).
//
// Stage-dependent side effects:
//   Advance  : saves *_output.txt, viz_state.json, viz.svg, model snapshot,
//              state snapshot in stateDir, and (for now) appends the observed
//              outputs to selected_output.csv.
//   Forecast : writes only *_forecast_output.txt and forecast_viz.svg.
// ---------------------------------------------------------------------------
StageResult DTRunner::runStage(StageKind kind,
                               const QDateTime &stageStart,
                               const QDateTime &stageEnd,
                               const QString &modelJsonPath)
{
    StageResult result;
    result.kind = kind;

    const bool isAdvance = (kind == StageKind::Advance);
    const char *stageLabel = isAdvance ? "Advance" : "Forecast";

    const double ohqStart = toOHQDaySerial(stageStart);
    const double ohqEnd   = toOHQDaySerial(stageEnd);

    std::cout << "\n[Runner] ---- Stage " << stageLabel << " ----\n"
              << "[Runner] Wall clock : "
              << stageStart.toString(Qt::ISODate).toStdString() << "  →  "
              << stageEnd.toString(Qt::ISODate).toStdString() << "\n"
              << "[Runner] OHQ serial : " << ohqStart << "  →  " << ohqEnd << "\n";

    // ------------------------------------------------------------------
    // Patch the simulation window in the initial-condition file (if any).
    // Each stage writes its own patched copy so the two solves don't
    // race over the same _current_input.json.
    // ------------------------------------------------------------------
    QString patchedPath;
    if (!modelJsonPath.isEmpty())
    {
        const QJsonObject base    = readJson(modelJsonPath);
        const QJsonObject patched = patchSimulationWindow(base, ohqStart, ohqEnd);

        const QString suffix = isAdvance ? "_advance.json" : "_forecast.json";
        patchedPath = QString::fromStdString(m_config.stateDir) +
                      "/_current_input" + suffix;
        if (!writeJson(patched, patchedPath))
        {
            std::cerr << "[Runner] Failed to write patched state for "
                      << stageLabel << "\n";
            return result;
        }
    }

    // ------------------------------------------------------------------
    // Build and solve
    // ------------------------------------------------------------------
    const std::string defaultTemplatePath =
        QCoreApplication::applicationDirPath().toStdString() + "/../../resources/";
    const std::string settingsFile = defaultTemplatePath + "settings.json";

    std::unique_ptr<System> ohqSystem(new System());
    ohqSystem->SetDefaultTemplatePath(defaultTemplatePath);
    ohqSystem->SetWorkingFolder(
        QString::fromStdString(m_config.outputDir).toStdString() + "/");

    std::cout << "[Runner] Preparing model...\n";
    if (!patchedPath.isEmpty())
    {
        ohqSystem->ReadSystemSettingsTemplate(settingsFile);
        ohqSystem->LoadfromJson(patchedPath);
    }
    else
    {
        // Cold start
        Script scr(m_config.scriptFile, ohqSystem.get());
        ohqSystem->CreateFromScript(scr, settingsFile);
        ohqSystem->SetProp("simulation_start_time", ohqStart);
        ohqSystem->SetProp("simulation_end_time",   ohqEnd);
    }

    ohqSystem->SetSilent(false);


    // Precipitation (clamped to this stage's window inside fetchPrecipitation)
    CPrecipitation precip = fetchPrecipitation(stageStart, stageEnd);
    {
        const QString precipFile =
            QString::fromStdString(m_config.outputDir) + "/" +
            stageStart.toString("yyyyMMdd_HHmmss") + "_" +
            (isAdvance ? "advance" : "forecast") + "_precipitation.txt";
        precip.writefile(precipFile.toStdString());
    }
    injectPrecipitation(ohqSystem.get(), precip);
    ohqSystem->CalcAllInitialValues();
    std::cout << "[Runner] Solving...\n";
    ohqSystem->Solve();

    // ------------------------------------------------------------------
    // Outputs
    // ------------------------------------------------------------------
    const QString stageTag = isAdvance ? "_output.txt" : "_forecast_output.txt";
    const QString outputFile =
        QString::fromStdString(m_config.outputDir) + "/" +
        stageStart.toString("yyyyMMdd_HHmmss") + stageTag;

    std::cout << "[Runner] Writing output to: " << outputFile.toStdString() << "\n";
    ohqSystem->GetObservedOutputs().write(outputFile.toStdString());
    result.outputFilePath = outputFile;

    // Capture the observed outputs for the (future) merge step.
    result.observed = ohqSystem->GetObservedOutputs();

    // ------------------------------------------------------------------
    // Stage-specific side effects
    // ------------------------------------------------------------------
    if (isAdvance)
    {
        // Export configured state variables
        for (const auto &exp : m_config.stateVarExports)
            ohqSystem->SaveStateVariableToJson(exp.variable, exp.outputPath);

        // Save model snapshot (drives next cycle)
        const QString modelSnapshotPath =
            QString::fromStdString(m_config.modelSnapshotDir) + "/" +
            stageStart.toString("yyyyMMdd_HHmmss") + "_model.json";
        ohqSystem->SavetoJson(modelSnapshotPath.toStdString(),
                              ohqSystem->addedtemplates, false, true);

        // Full state for visualization
        const QString vizStatePath =
            QString::fromStdString(m_config.outputDir) + "/viz_state.json";
        ohqSystem->SaveFullStateTo(vizStatePath);
        std::cout << "[Runner] Viz state written to: "
                  << vizStatePath.toStdString() << "\n";

        const QString vizJsonPath = QString::fromStdString(m_config.vizFile);
        const QString vizSvgPath  =
            QString::fromStdString(m_config.outputDir) + "/viz.svg";
        const QJsonObject fullState = readJson(vizStatePath);
        QString vizErr;
        if (!VizRenderer::render(vizJsonPath, fullState, vizSvgPath, vizErr))
            std::cerr << "[Runner] VizRenderer warning: " << vizErr.toStdString() << "\n";
        else
            std::cout << "[Runner] viz.svg written to: " << vizSvgPath.toStdString() << "\n";
        result.vizSvgPath = vizSvgPath;

        // State snapshot in stateDir (this is what next cycle picks up)
        QJsonObject savedState = readJson(modelSnapshotPath);
        savedState["_dt_interval_start_utc"] = stageStart.toString(Qt::ISODate);
        savedState["_dt_interval_end_utc"]   = stageEnd.toString(Qt::ISODate);
        savedState["_dt_next_start_utc"]     = stageEnd.toString(Qt::ISODate);
        savedState["_dt_runs_completed"]     = m_runsCompleted + 1;
        savedState["_dt_output_file"]        = outputFile;

        const QString snapshotPath =
            QString::fromStdString(m_config.stateDir) + "/" +
            makeSnapshotFilename(stageEnd);
        if (!writeJson(savedState, snapshotPath))
            std::cerr << "[Runner] Warning: failed to write state snapshot\n";
        else
            std::cout << "[Runner] State snapshot: " << snapshotPath.toStdString() << "\n";
        result.stateSnapshotPath = snapshotPath;
    }
    else // Forecast
    {
        // Forecast viz: independent SVG, generated from forecast end-state.
        // No model snapshot, no state snapshot, no state-variable exports.
        const QString forecastVizStatePath =
            QString::fromStdString(m_config.outputDir) + "/forecast_viz_state.json";
        ohqSystem->SaveFullStateTo(forecastVizStatePath);

        const QString vizJsonPath = QString::fromStdString(m_config.vizFile);
        const QString forecastVizSvgPath =
            QString::fromStdString(m_config.outputDir) + "/forecast_viz.svg";
        const QJsonObject fullState = readJson(forecastVizStatePath);
        QString vizErr;
        if (!VizRenderer::render(vizJsonPath, fullState, forecastVizSvgPath, vizErr))
            std::cerr << "[Runner] Forecast VizRenderer warning: "
                      << vizErr.toStdString() << "\n";
        else
            std::cout << "[Runner] forecast_viz.svg written to: "
                      << forecastVizSvgPath.toStdString() << "\n";
        result.vizSvgPath = forecastVizSvgPath;
    }

    result.ok = true;
    return result;
}

// ---------------------------------------------------------------------------
// mergeIntoSelectedOutput
// Maintains selected_output.csv as:
//     [actuals up to advanceEnd] + [forecast tail (advanceEnd, advanceEnd+H]]
//
// Each cycle:
//   1. Load existing CSV (if any) into a TimeSeriesSet.
//   2. Knockout at (advanceStart - epsilon) to drop the previous cycle's
//      forecast tail (which started at the previous advanceStart and now
//      should be replaced by today's authoritative actuals + new forecast).
//   3. Append the Advance observed rows ([advanceStart, advanceEnd]).
//   4. Append the Forecast observed rows whose t > advanceEnd
//      (i.e. the forecast tail beyond the just-added Advance window).
//   5. Write back with full header.
//
// Series are matched by name across loaded / advance / forecast sets.
// ---------------------------------------------------------------------------
bool DTRunner::mergeIntoSelectedOutput(const TimeSeriesSet<double> &advanceObs,
                                       const TimeSeriesSet<double> &forecastObs,
                                       double cutoffTime)
{
    const QString selectedOutputFile =
        QString::fromStdString(m_config.outputDir) + "/selected_output.csv";

    // Tiny epsilon in day units to drop the boundary point from the
    // previous cycle so today's Advance can re-supply it without duplication.
    // ~0.5 ms in days = 5.787e-9. We use 1e-6 (≈86 ms) to be safely above
    // any float round-trip in CSV write/read cycles.
    constexpr double kBoundaryEpsilonDays = 1e-6;

    // ------------------------------------------------------------------
    // 1. Load existing file (or start fresh)
    // ------------------------------------------------------------------
    TimeSeriesSet<double> merged;
    QFileInfo fi(selectedOutputFile);
    if (fi.exists() && fi.size() > 0)
    {
        TimeSeriesSet<double> loaded(selectedOutputFile.toStdString(), true);
        if (loaded.file_not_found)
        {
            std::cerr << "[Runner] mergeIntoSelectedOutput: failed to read "
                      << selectedOutputFile.toStdString()
                      << " — starting fresh.\n";
        }
        else
        {
            merged = loaded;
            // 2. Drop the previous cycle's forecast tail.
            merged.knockout(cutoffTime - kBoundaryEpsilonDays);
        }
    }
    else
    {
        // Bootstrap: take the structure (series + names) from advanceObs.
        // No rows yet; they get added in step 3.
        for (size_t i = 0; i < advanceObs.size(); ++i)
        {
            TimeSeries<double> empty;
            empty.setName(advanceObs[i].name());
            merged.push_back(empty);
        }
    }

    // Helper: find a series in `merged` by name; create it if missing.
    auto findOrCreate = [&merged](const std::string &name) -> TimeSeries<double>& {
        for (size_t i = 0; i < merged.size(); ++i)
            if (merged[i].name() == name)
                return merged[i];
        TimeSeries<double> empty;
        empty.setName(name);
        merged.push_back(empty);
        return merged.back();
    };

    // ------------------------------------------------------------------
    // 3. Append Advance observed rows (the new actuals).
    // ------------------------------------------------------------------
    for (size_t i = 0; i < advanceObs.size(); ++i)
    {
        const TimeSeries<double> &src = advanceObs[i];
        TimeSeries<double> &dst = findOrCreate(src.name());
        for (size_t j = 0; j < src.size(); ++j)
        {
            const auto &pt = src[j];
            dst.append(pt.t, pt.c);
        }
    }

    // ------------------------------------------------------------------
    // 4. Append Forecast observed rows beyond advanceEnd (the forecast tail).
    //    cutoffTime here is advanceStart, not advanceEnd — we need the
    //    Advance window's *end* to slice the forecast.
    //    The orchestrator passes advanceEnd as a separate argument… except
    //    we only have cutoffTime in this signature. We derive advanceEnd
    //    by taking the last Advance timestamp seen above.
    // ------------------------------------------------------------------
    double advanceLastT = cutoffTime;  // fallback if Advance is empty
    if (!advanceObs.empty())
    {
        for (size_t i = 0; i < advanceObs.size(); ++i)
        {
            const TimeSeries<double> &src = advanceObs[i];
            if (!src.empty())
                advanceLastT = std::max(advanceLastT, src[src.size() - 1].t);
        }
    }

    for (size_t i = 0; i < forecastObs.size(); ++i)
    {
        const TimeSeries<double> &src = forecastObs[i];
        TimeSeries<double> &dst = findOrCreate(src.name());
        for (size_t j = 0; j < src.size(); ++j)
        {
            const auto &pt = src[j];
            if (pt.t > advanceLastT)
                dst.append(pt.t, pt.c);
        }
    }

    // ------------------------------------------------------------------
    // 5. Write back (full file with header)
    // ------------------------------------------------------------------
    merged.write(selectedOutputFile.toStdString());
    std::cout << "[Runner] selected_output.csv merged: "
              << merged.size() << " series, "
              << merged.maxnumpoints() << " max rows\n";

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
// Fetches precipitation for [intervalStart, intervalEnd] from the source
// configured in config.weather_source:
//   "openmeteo"            -> 7-day forecast (real-time / forward mode)
//   "openmeteo_historical" -> ERA5 archive   (Truth-Twin / replay mode)
// Returns CPrecipitation with bins in OHQ day-serial units, in metres.
// ---------------------------------------------------------------------------
CPrecipitation DTRunner::fetchPrecipitation(const QDateTime &intervalStart,
                                            const QDateTime &intervalEnd)
{
    NOAAWeatherFetcher fetcher;

    if (m_config.weatherSource == "openmeteo") {
        CPrecipitation precip = fetcher.getOpenMeteoPrecipitation(
            m_config.latitude, m_config.longitude,
            intervalStart, intervalEnd);
        if (precip.n == 0)
            std::cerr << "[Runner] Warning: " << fetcher.lastError().toStdString() << "\n";
        return precip;
    }

    if (m_config.weatherSource == "openmeteo_historical") {
        CPrecipitation precip = fetcher.getOpenMeteoHistoricalPrecipitation(
            m_config.latitude, m_config.longitude,
            intervalStart, intervalEnd);
        if (precip.n == 0)
            std::cerr << "[Runner] Warning: " << fetcher.lastError().toStdString() << "\n";
        return precip;
    }

    std::cerr << "[Runner] Unknown weather_source: '"
              << m_config.weatherSource << "' (expected 'openmeteo' or "
              << "'openmeteo_historical')\n";
    return CPrecipitation();
}

void DTRunner::injectPrecipitation(System *system, const CPrecipitation &precip)
{
    if (precip.n == 0) {
        std::cerr << "[Runner] injectPrecipitation: empty precipitation, skipping.\n";
        return;
    }

    // The block name in the .ohq is "Rain" (capitalized) — match that exactly.
    Source *src = system->source("Rain");
    if (!src) {
        std::cerr << "[Runner] injectPrecipitation: source 'Rain' not found in system.\n";
        return;
    }

    auto *var = src->Variable("timeseries");
    if (!var) {
        std::cerr << "[Runner] injectPrecipitation: variable 'timeseries' not found on source 'Rain'.\n";
        return;
    }

    var->SetTimeSeries(precip);
    std::cout << "[Runner] Injected " << precip.n
              << " precipitation bins into 'Rain' source.\n";

    auto *intensityVar = src->Variable("timeseries");
    if (intensityVar) {
        std::cout << "[Inject DIAG] After SetTimeSeries on Rain.timeseries:\n";
        std::cout << "  Rain.timeseries._timeseries.size() = "
                  << var->GetTimeSeries()->size() << "\n";   // or whatever the getter is
        std::cout << "  Rain.Intensity._val = "
                  << intensityVar->GetVal() << "\n";  // should be 0 here, that's fine
        std::cout << "  Rain.Intensity expression = "
                  << intensityVar->GetExpression()->ToString() << "\n";
    }
    std::cout << "[Runner] Injected " << precip.n << " precipitation bins...\n";
}


// ---------------------------------------------------------------------------
// applyOUNoiseStateful
//
// Apply multiplicative log-normal noise driven by a continuous Ornstein-
// Uhlenbeck process with unit stationary variance:
//
//     x_obs(t) = x_model(t) * exp(sigma * epsilon(t))
//
// where epsilon evolves according to dε = -ε/τ dt + √(2/τ) dW, integrated
// with the *exact* Gaussian transition (not Euler-Maruyama):
//
//     epsilon_{n+1} = phi * epsilon_n + sqrt(1 - phi^2) * eta_n
//     phi           = exp(-Δt / τ)
//     eta_n         ~ N(0, 1)
//
// The OU state is persisted in m_ouState across runOnce() cycles, keyed by
// series name, so correlation continues smoothly between cycles. First time
// a series name is seen, epsilon is drawn from N(0,1) (the stationary
// distribution).
//
// τ <= 0 is treated as the white-noise limit (each point gets an independent
// N(0,1) draw). σ <= 0 is a no-op.
// ---------------------------------------------------------------------------
void DTRunner::applyOUNoiseStateful(TimeSeriesSet<double> &set,
                                    double sigma,
                                    double tauDays)
{
    if (sigma <= 0.0) return;
    if (set.empty()) return;

    // One shared RNG for the whole process. random_device seeds it once.
    // For reproducible Truth Twins, swap in a fixed seed (e.g. from
    // m_config.observations.seed if you add such a field) here.
    static thread_local std::mt19937 gen{ std::random_device{}() };
    static thread_local std::normal_distribution<double> N01{ 0.0, 1.0 };

    for (size_t s = 0; s < set.size(); ++s)
    {
        TimeSeries<double> &ts = set[s];
        if (ts.empty()) continue;

        // Name-indexed: robust against OHQ reordering observed outputs.
        const std::string seriesKey = ts.name();

        // Initialize or fetch the persisted OU state for this series.
        double eps;
        auto it = m_ouState.find(seriesKey);
        if (it == m_ouState.end())
        {
            eps = N01(gen);                  // stationary draw
            m_ouState[seriesKey] = eps;
        }
        else
        {
            eps = it->second;
        }

        // Walk the series, advancing OU exactly between samples.
        for (size_t i = 0; i < ts.size(); ++i)
        {
            if (i > 0)
            {
                const double dt = ts.getTime(i) - ts.getTime(i - 1);

                if (tauDays <= 0.0 || dt <= 0.0)
                {
                    // White-noise limit / degenerate dt: independent draw.
                    eps = N01(gen);
                }
                else
                {
                    const double phi = std::exp(-dt / tauDays);
                    const double sqrt_one_minus_phi2 =
                        std::sqrt(std::max(0.0, 1.0 - phi * phi));
                    eps = phi * eps + sqrt_one_minus_phi2 * N01(gen);
                }
            }

            const double clean = ts.getValue(i);
            const double noised = clean * std::exp(sigma * eps);
            ts.setValue(i, noised);
        }

        // Persist epsilon for the next cycle.
        m_ouState[seriesKey] = eps;
    }
}

// ---------------------------------------------------------------------------
// writeMetadataSidecar
//
// Writes outputs/selected_output_meta.json describing the noise model
// applied to selected_output.csv. Idempotent: re-written every cycle so
// `last_updated_utc` reflects the most recent merge.
// ---------------------------------------------------------------------------
bool DTRunner::writeMetadataSidecar() const
{
    QJsonObject obsBlock;
    obsBlock["save_interval_ms"] =
        static_cast<qint64>(m_config.observations.saveIntervalMs);
    obsBlock["save_interval_days"] = m_obsSaveIntervalDays;
    obsBlock["noise_sigma"] = m_config.observations.noiseSigma;
    obsBlock["noise_correlation_time_ms"] =
        static_cast<qint64>(m_config.observations.noiseCorrelationTimeMs);
    obsBlock["noise_correlation_time_days"] =
        static_cast<double>(m_config.observations.noiseCorrelationTimeMs)
        / (86400.0 * 1000.0);
    obsBlock["noise_model"] =
        QStringLiteral("multiplicative_log_normal_OU "
                       "x_obs = x_model * exp(sigma * epsilon)");
    obsBlock["ou_stationary_variance"] = 1.0;

    QJsonObject root;
    root["observations"]    = obsBlock;
    root["deployment_name"] = QString::fromStdString(m_config.deploymentName);
    root["runs_completed"]  = m_runsCompleted;
    root["last_updated_utc"] =
        QDateTime::currentDateTimeUtc().toString(Qt::ISODate);

    const QString path =
        QString::fromStdString(m_config.outputDir) + "/selected_output_meta.json";

    return writeJson(root, path);
}
