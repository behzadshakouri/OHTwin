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
//
// Loads and validates a deployment's config.json.
//
// Each deployment is a self-contained directory:
//
//   <deployment_root>/
//     config.json           (Option-A: { deployment{...}, runtime{...} })
//     viz_<name>.json       (path is given by deployment.viz_file)
//     model/<name>.ohq      (path is given by deployment.model_file)
//     state/                (auto-created: state snapshots)
//     outputs/              (auto-created: simulation outputs)
//     snapshots/            (auto-created: per-interval model JSON snapshots)
//
// Relative paths inside config.json (model_file, viz_file, output_path) are
// resolved against deployment_root. The auto-derived working directories
// (state/, outputs/, snapshots/) are created if missing.
//
// All final paths are stored as absolute std::string for direct OHQ use.
// ---------------------------------------------------------------------------
class DTConfig
{
public:
    // Load <deploymentRoot>/config.json.
    // Returns true on success; populates errorMessage on failure.
    bool load(const QString &deploymentRoot, QString &errorMessage);

    // --- deployment metadata ---
    std::string deploymentRoot;    // absolute path to the deployment directory
    std::string deploymentName;    // canonical name from config (deployment.name)
    int         port = 0;          // nginx/HTTP port for this deployment

    // --- paths (all absolute after load()) ---
    std::string scriptFile;        // .ohq script used on cold start
    std::string vizFile;           // visualization JSON spec for VizRenderer
    std::string stateDir;          // <deployment_root>/state
    std::string outputDir;         // <deployment_root>/outputs
    std::string modelSnapshotDir;  // <deployment_root>/snapshots
    std::string weatherFile;       // optional weather JSON (absolute path)
    std::string loadModelJson;     // optional cold-start model JSON (absolute path)

    // --- weather / Open-Meteo ---
    std::string weatherSource = "openmeteo";
    double      latitude      = 0.0;
    double      longitude     = 0.0;

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

private:
    // Parse "300s", "15min", "4hr", "1day" -> milliseconds.
    // Returns -1 on parse error and writes details into err.
    static qint64 parseIntervalMs(const std::string &s, QString &err);

    // Resolve a path string against deploymentRoot. Absolute paths are
    // returned as-is; relative paths are joined with deploymentRoot.
    // Empty input returns empty.
    QString resolvePath(const QString &p) const;
};
