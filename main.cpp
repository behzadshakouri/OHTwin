/*
 * OpenHydroQual Digital Twin — generic OHQ model runner
 *
 * Each invocation targets one self-contained deployment directory:
 *
 *   OHTwin --deployment /path/to/deployments/Bioretention
 *
 * The deployment directory holds config.json, the .ohq model file, the
 * visualization spec, and the runtime state/outputs/snapshots folders.
 *
 * main.cpp is intentionally thin:
 *   - Parse command-line arguments
 *   - Load config from the deployment directory
 *   - Initialise runner
 *   - Fire an immediate first run, then arm QTimer for subsequent intervals
 *   - (Future) Start Crow HTTP API
 */

#include "DTConfig.h"
#include "DTRunner.h"

#include <QCommandLineOption>
#include <QCommandLineParser>
#include <QCoreApplication>
#include <QTimer>

#include <algorithm>
#include <climits>
#include <iostream>

int main(int argc, char *argv[])
{
    QCoreApplication app(argc, argv);
    QCoreApplication::setApplicationName("OHTwin");
    QCoreApplication::setApplicationVersion("1.0");

    // ------------------------------------------------------------------
    // 1. Parse command-line arguments
    // ------------------------------------------------------------------
    QCommandLineParser parser;
    parser.setApplicationDescription(
        "OpenHydroQual Digital Twin — runs one deployment");
    parser.addHelpOption();
    parser.addVersionOption();

    QCommandLineOption deploymentOpt(
        QStringList() << "d" << "deployment",
        "Path to the deployment directory containing config.json.",
        "path");
    parser.addOption(deploymentOpt);

    parser.process(app);

    if (!parser.isSet(deploymentOpt))
    {
        std::cerr << "[Main] --deployment <path> is required.\n"
                  << "       Example: OHTwin --deployment "
                     "/home/arash/Projects/DrywellDT/deployments/Bioretention\n";
        return 1;
    }

    const QString deploymentRoot = parser.value(deploymentOpt);

    // ------------------------------------------------------------------
    // 2. Load the deployment's config.json
    // ------------------------------------------------------------------
    DTConfig config;
    QString configError;
    if (!config.load(deploymentRoot, configError))
    {
        std::cerr << "[Main] Failed to load config: "
                  << configError.toStdString() << "\n";
        return 1;
    }

    // ------------------------------------------------------------------
    // 3. Initialise runner
    // ------------------------------------------------------------------
    DTRunner runner(config);
    QString initError;
    if (!runner.init(initError))
    {
        std::cerr << "[Main] Runner init failed: "
                  << initError.toStdString() << "\n";
        return 2;
    }

    // ------------------------------------------------------------------
    // 4. Run the first interval immediately at start-up
    // ------------------------------------------------------------------
    // Use a single-shot zero-delay timer so the event loop is running before
    // the first solve begins. This keeps the door open for signals/slots and
    // the future Crow API to be wired in before any blocking work.
    QTimer::singleShot(0, &runner, [&runner]() {
        if (!runner.runOnce())
        {
            std::cerr << "[Main] Initial run failed.\n";
            QCoreApplication::exit(3);
        }
    });

    // ------------------------------------------------------------------
    // 5. Arm the recurring timer for subsequent intervals
    // ------------------------------------------------------------------
    QTimer intervalTimer;

    const qint64 safeIntervalMs =
        std::min(config.intervalMs, static_cast<qint64>(INT_MAX));

    intervalTimer.setInterval(static_cast<int>(safeIntervalMs));

    QObject::connect(&intervalTimer, &QTimer::timeout,
                     &runner, [&runner]() {
                         if (!runner.runOnce())
                         {
                             std::cerr << "[Main] Periodic run failed. "
                                          "Continuing to next interval.\n";
                             // Non-fatal: log and wait for the next tick.
                         }
                     });

    intervalTimer.start();

    std::cout << "[Main] OHQ Digital Twin running. Deployment: "
              << config.deploymentName
              << "  Interval: " << config.intervalStr
              << "  (" << config.intervalMs << " ms)\n"
              << "[Main] Press Ctrl+C to stop.\n";

    // ------------------------------------------------------------------
    // 6. TODO: wire in Crow HTTP API here before app.exec()
    // ------------------------------------------------------------------

    return app.exec();
}
