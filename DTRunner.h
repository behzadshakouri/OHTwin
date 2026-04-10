#pragma once

#include "DTConfig.h"

#include <QDateTime>
#include <QJsonObject>
#include <QObject>
#include <QString>
#include <string>

// ---------------------------------------------------------------------------
// DTRunner
//
// Owns the simulation loop.  Each call to runOnce():
//   1. Determines the current simulation window [startTime, endTime] in OHQ
//      day-serial units.
//   2. Builds (first run) or hot-restarts (subsequent runs) the System.
//   3. Advances simulation_start_time / simulation_end_time in the loaded
//      state so the next interval picks up where this one left off.
//   4. Writes outputs to outputDir and a timestamped state snapshot to
//      stateDir.
//
// The runner is intentionally a QObject so it can be connected to QTimer
// and, later, to Crow API callbacks.
// ---------------------------------------------------------------------------
class DTRunner : public QObject
{
    Q_OBJECT

public:
    explicit DTRunner(const DTConfig &config, QObject *parent = nullptr);

    // Call once at start-up; initialises timing state.
    // Returns false if something is fatally wrong (bad config paths, etc.)
    bool init(QString &errorMessage);

public slots:
    // Triggered by QTimer (or called directly for an immediate first run).
    // Returns true on success.
    bool runOnce();

private:
    // -----------------------------------------------------------------------
    // Internal helpers
    // -----------------------------------------------------------------------

    // Find the most-recent state snapshot in stateDir, if any.
    // Returns empty string if none found.
    QString findLatestStateSnapshot() const;

    // Build the timestamped snapshot filename:  state_YYYYMMDD_HHmmss.json
    QString makeSnapshotFilename(const QDateTime &dt) const;

    // Convert a QDateTime to an OHQ day-serial (days since 1899-12-30,
    // which is the Excel epoch used by OHQ).
    static double toOHQDaySerial(const QDateTime &dt);

    // Convert an OHQ day-serial back to QDateTime.
    static QDateTime fromOHQDaySerial(double serial);

    // Write a QJsonObject to a file; returns false on I/O error.
    static bool writeJson(const QJsonObject &obj, const QString &path);

    // Read a QJsonObject from a file; returns empty object on error.
    static QJsonObject readJson(const QString &path);

    // Patch simulation_start_time and simulation_end_time inside a state
    // JSON object and return the modified copy.
    static QJsonObject patchSimulationWindow(const QJsonObject &state,
                                             double startSerial,
                                             double endSerial);

    // -----------------------------------------------------------------------
    // State
    // -----------------------------------------------------------------------
    const DTConfig &m_config;

    // Wall-clock datetime that corresponds to the *start* of the next
    // simulation interval.
    QDateTime m_nextIntervalStart;

    // OHQ interval length expressed in days (derived from config.intervalMs).
    double m_intervalDays = 1.0;

    // How many intervals have completed successfully.
    int m_runsCompleted = 0;
};
