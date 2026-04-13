#pragma once

#include <QString>
#include <QJsonObject>
#include <vector>
#include <string>

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
// All paths are stored as std::string so they can be passed straight to OHQ.
// ---------------------------------------------------------------------------
class DTConfig
{
public:
    // Load config.json from the same directory as the running binary.
    // Returns true on success; populates errorMessage on failure.
    bool load(QString &errorMessage);

    // --- paths ---
    std::string scriptFile;        // .ohq script (used on cold start if loadModelJson is empty)
    std::string loadModelJson;     // optional: force cold-start from this model JSON instead of script
    std::string stateDir;          // directory for timestamped state snapshots
    std::string outputDir;         // directory for simulation output files (nginx-served)
    std::string modelSnapshotDir;  // directory for per-interval model JSON snapshots
    std::string weatherFile;       // optional weather JSON

    // --- weather / NOAA ---
    std::string noaaOffice;        // e.g. "LWX"
    int         noaaGridX = 0;     // NOAA grid X coordinate
    int         noaaGridY = 0;     // NOAA grid Y coordinate

    // --- timing ---
    // "300s" | "4hr" | "2day"  — parsed into intervalMs for QTimer
    std::string intervalStr;
    qint64      intervalMs  = 86400000; // default: 1 day
    // Optional fixed start datetime (ISO 8601). Empty = use current time.
    std::string startDatetime;

    // --- state variable exports ---
    std::vector<StateVarExport> stateVarExports;

    // Raw JSON object (kept for forward-compatibility; callers may inspect it)
    QJsonObject raw;

private:
    // Parse "300s", "4hr", "2day" → milliseconds.
    // Returns -1 on parse error.
    static qint64 parseIntervalMs(const std::string &s, QString &err); // "300s","4hr","2day" → ms
};
