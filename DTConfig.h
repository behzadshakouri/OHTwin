#pragma once

#include <QJsonObject>
#include <QString>
#include <QtGlobal>

#include <string>
#include <vector>

// ---------------------------------------------------------------------------
// StateVarExport
// A variable name and the path where its JSON snapshot should be written.
// ---------------------------------------------------------------------------
struct StateVarExport
{
    std::string variable;
    std::string outputPath;
};

// ---------------------------------------------------------------------------
// DTConfig
// Loads and validates config.json from next to the binary.
//
// Paths may be machine-based in config.json using ${project_root}; DTConfig.cpp
// expands those values from the selected machine profile before passing them to
// OHQ. All final paths are stored as std::string for direct OHQ use.
// ---------------------------------------------------------------------------
class DTConfig
{
public:
    // Load config.json from the same directory as the running binary.
    // Returns true on success; populates errorMessage on failure.
    bool load(QString &errorMessage);

    // --- paths ---
    std::string scriptFile;        // .ohq script used on cold start if loadModelJson is empty
    std::string loadModelJson;     // optional: force cold-start from this model JSON instead of script
    std::string stateDir;          // directory for timestamped state snapshots
    std::string outputDir;         // directory for simulation output files / nginx-served outputs
    std::string modelSnapshotDir;  // directory for per-interval model JSON snapshots
    std::string weatherFile;       // optional weather JSON
    std::string vizFile;           // visualization JSON spec for VizRenderer (model-specific)
    std::string modelName;         // active model profile name (VN, Drywell, HQ, R, etc.)

    // --- weather / NOAA / Open-Meteo ---
    std::string weatherSource = "openmeteo"; // "noaa" or "openmeteo"
    double      latitude      = 0.0;
    double      longitude     = 0.0;
    std::string noaaOffice;        // e.g. "LWX"
    int         noaaGridX = 0;
    int         noaaGridY = 0;

    // --- timing ---
    // Accepted syntax: "300s", "15min", "4hr", "1day".
    std::string intervalStr;
    qint64      intervalMs = 86400000; // default: 1 day

    // Optional fixed start datetime (ISO 8601). Empty = use current time / snapshot.
    std::string startDatetime;

    // --- forecast horizon ---
    // Optional duration of Stage B forecast window beyond the advance interval.
    // Same syntax as intervalStr. Empty / 0 = forecast disabled.
    std::string forecastHorizonStr;
    qint64      forecastHorizonMs = 0;

    // --- state variable exports ---
    std::vector<StateVarExport> stateVarExports;

    // Raw JSON object kept for forward compatibility / optional inspection.
    QJsonObject raw;

private:
    // Parse "300s", "15min", "4hr", "1day" -> milliseconds.
    // Returns -1 on parse error and writes details into err.
    static qint64 parseIntervalMs(const std::string &s, QString &err);
};
