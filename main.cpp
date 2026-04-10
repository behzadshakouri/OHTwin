/*
 * OpenHydroQual Digital Twin — generic OHQ model runner
 *
 * main.cpp is intentionally thin:
 *   - Load config
 *   - Initialise runner
 *   - Fire an immediate first run, then arm QTimer for subsequent intervals
 *   - (Future) Start Crow HTTP API
 */

#include "DTConfig.h"
#include "DTRunner.h"

#include <QCoreApplication>
#include <QTimer>
#include <iostream>

int main(int argc, char *argv[])
{
    QCoreApplication app(argc, argv);

    // ------------------------------------------------------------------
    // 1. Load config.json from next to the binary
    // ------------------------------------------------------------------
    DTConfig config;
    QString configError;
    if (!config.load(configError))
    {
        std::cerr << "[Main] Failed to load config: "
                  << configError.toStdString() << "\n";
        return 1;
    }

    // ------------------------------------------------------------------
    // 2. Initialise runner
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
    // 3. Run the first interval immediately (at start-up)
    // ------------------------------------------------------------------
    // Use a single-shot zero-delay timer so the event loop is running
    // before the first solve begins (keeps the door open for signals/slots
    // and the future Crow API to be wired in before any blocking work).
    QTimer::singleShot(0, &runner, [&runner]() {
        if (!runner.runOnce())
        {
            std::cerr << "[Main] Initial run failed.\n";
            QCoreApplication::exit(3);
        }
    });

    // ------------------------------------------------------------------
    // 4. Arm the recurring timer for subsequent intervals
    // ------------------------------------------------------------------
    QTimer intervalTimer;
    intervalTimer.setInterval(static_cast<int>(
        std::min(config.intervalMs, static_cast<qint64>(INT_MAX))));

    QObject::connect(&intervalTimer, &QTimer::timeout,
                     &runner, [&runner]() {
                         if (!runner.runOnce())
                         {
                             std::cerr << "[Main] Periodic run failed. Continuing to next interval.\n";
                             // Non-fatal: log and wait for the next tick
                         }
                     });
    intervalTimer.start();

    std::cout << "[Main] OHQ Digital Twin running. Interval: "
              << config.intervalStr << "  (" << config.intervalMs << " ms)\n"
              << "[Main] Press Ctrl+C to stop.\n";

    // ------------------------------------------------------------------
    // 5. TODO: wire in Crow HTTP API here before app.exec()
    // ------------------------------------------------------------------

    return app.exec();
}
