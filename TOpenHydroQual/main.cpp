/*
 * OpenHydroQual - Environmental Modeling Platform
 * Copyright (C) 2025 Arash Massoudieh
 * 
 * This file is part of OpenHydroQual.
 * 
 * OpenHydroQual is free software: you can redistribute it and/or modify it
 * under the terms of the GNU Affero General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or (at your
 * option) any later version.
 * 
 * If you use this file in a commercial product, you must purchase a
 * commercial license. Contact arash.massoudieh@cua.edu for details.
 */

#include "System.h"
#include "Script.h"
#include <QCoreApplication>
#include <QDate>
#include <QDateTime>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QTimer>
#include <iostream>
#include <memory>
#include <string>
#include <vector>

using namespace std;

namespace {

struct StateVarExport
{
    string variable;
    string outputPath;
};

struct AppOptions
{
    string scriptFile;
    string weatherFile;
    string runnerStateFile;
    string loadModelJson;
    string saveModelJson;
    int runDays = 1;
    bool watchDaily = false;
    vector<StateVarExport> stateVarExports;
};

bool SaveJsonToFile(const QJsonObject &obj, const QString &path)
{
    QFile file(path);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate))
    {
        cerr << "Could not write state file: " << path.toStdString() << endl;
        return false;
    }
    file.write(QJsonDocument(obj).toJson(QJsonDocument::Indented));
    return true;
}

QJsonObject LoadJsonFromFile(const QString &path)
{
    QFile file(path);
    if (!file.exists() || !file.open(QIODevice::ReadOnly))
    {
        return QJsonObject();
    }

    const auto doc = QJsonDocument::fromJson(file.readAll());
    return doc.isObject() ? doc.object() : QJsonObject();
}

QJsonArray LoadWeatherArray(const QString &weatherPath)
{
    if (weatherPath.isEmpty())
        return QJsonArray();

    const auto weatherObj = LoadJsonFromFile(weatherPath);
    if (weatherObj.contains("days") && weatherObj["days"].isArray())
    {
        return weatherObj["days"].toArray();
    }

    return QJsonArray();
}

QJsonObject SelectWeatherForDate(const QJsonArray &weatherDays, const QString &isoDate)
{
    for (const auto &entry : weatherDays)
    {
        if (!entry.isObject())
            continue;
        const auto obj = entry.toObject();
        if (obj.value("date").toString() == isoDate)
        {
            return obj;
        }
    }

    return QJsonObject();
}

void PrintUsage()
{
    cout << "Usage:\n"
         << "  OpenHydroQual-Console <script_file> [options]\n\n"
         << "Options:\n"
         << "  --weather <file.json>                 Weather inputs with a 'days' array\n"
         << "  --state <file.json>                   Save/load runner state\n"
         << "  --load-model-json <model.json>        Load model from json instead of script\n"
         << "  --save-model-json <model.json>        Save model json after build/load\n"
         << "  --save-state-variable <var=path.json> Save state variable to json (repeatable)\n"
         << "  --days <n>                            Number of day-solves to run immediately\n"
         << "  --watch-daily                         Keep process running and solve every 24h\n"
         << endl;
}

bool ParsePositiveInt(const string &input, int &value)
{
    try
    {
        const int parsed = stoi(input);
        if (parsed < 1)
            return false;
        value = parsed;
        return true;
    }
    catch (...)
    {
        return false;
    }
}

bool ParseStateVarExport(const string &raw, StateVarExport &entry)
{
    const size_t splitPos = raw.find('=');
    if (splitPos == string::npos || splitPos == 0 || splitPos + 1 >= raw.size())
    {
        return false;
    }

    entry.variable = raw.substr(0, splitPos);
    entry.outputPath = raw.substr(splitPos + 1);
    return true;
}

bool ParseArgs(int argc, char *argv[], AppOptions &options)
{
    if (argc < 2)
    {
        PrintUsage();
        return false;
    }

    options.scriptFile = argv[1];
    for (int i = 2; i < argc; ++i)
    {
        const string arg = argv[i];

        if (arg == "--weather" && i + 1 < argc)
        {
            options.weatherFile = argv[++i];
        }
        else if (arg == "--state" && i + 1 < argc)
        {
            options.runnerStateFile = argv[++i];
        }
        else if (arg == "--load-model-json" && i + 1 < argc)
        {
            options.loadModelJson = argv[++i];
        }
        else if (arg == "--save-model-json" && i + 1 < argc)
        {
            options.saveModelJson = argv[++i];
        }
        else if (arg == "--save-state-variable" && i + 1 < argc)
        {
            StateVarExport entry;
            if (!ParseStateVarExport(argv[++i], entry))
            {
                cerr << "--save-state-variable requires format var=path.json\n";
                return false;
            }
            options.stateVarExports.push_back(entry);
        }
        else if (arg == "--days" && i + 1 < argc)
        {
            if (!ParsePositiveInt(argv[++i], options.runDays))
            {
                cerr << "--days requires an integer >= 1\n";
                return false;
            }
        }
        else if (arg == "--watch-daily")
        {
            options.watchDaily = true;
        }
        else
        {
            cerr << "Unknown or incomplete argument: " << arg << "\n";
            PrintUsage();
            return false;
        }
    }

    if (options.scriptFile.empty())
    {
        PrintUsage();
        return false;
    }

    return true;
}

QJsonObject BuildRunnerState(const AppOptions &options, const QString &lastRunDate, int totalRuns, const QString &lastOutputFile)
{
    QJsonObject state;
    state["script"] = QString::fromStdString(options.scriptFile);
    state["weather"] = QString::fromStdString(options.weatherFile);
    state["last_run_date"] = lastRunDate;
    state["next_run_date"] = QDate::fromString(lastRunDate, Qt::ISODate).addDays(1).toString(Qt::ISODate);
    state["runs_completed"] = totalRuns;
    state["last_output_file"] = lastOutputFile;
    state["updated_at_utc"] = QDateTime::currentDateTimeUtc().toString(Qt::ISODate);
    return state;
}

void PersistModelArtifacts(System *system, const AppOptions &options)
{
    if (!options.saveModelJson.empty())
    {
        system->SavetoJson(options.saveModelJson, system->addedtemplates, false, true);
    }

    for (const auto &entry : options.stateVarExports)
    {
        system->SaveStateVariableToJson(entry.variable, entry.outputPath);
    }
}

bool BuildAndSolveSystem(const AppOptions &options, const QString &solveDateIso, const QJsonObject &weatherForDay, QString &outputFilePath)
{
    unique_ptr<System> system(new System());
    cout << "Input file: " << options.scriptFile << endl;

    const string defaulttemppath = qApp->applicationDirPath().toStdString() + "/../../resources/";
    const string settingfilename = defaulttemppath + "settings.json";
    cout << "Default Template path = " + defaulttemppath + "\n";

    const QString scriptPath = QString::fromStdString(options.scriptFile);
    const QFileInfo scriptFile(scriptPath);
    if (!scriptFile.exists())
    {
        cerr << "Script file not found: " << options.scriptFile << endl;
        return false;
    }

    system->SetDefaultTemplatePath(defaulttemppath);
    system->SetWorkingFolder(scriptFile.canonicalPath().toStdString() + "/");

    cout << "Preparing model ..." << endl;
    if (!options.loadModelJson.empty())
    {
        system->ReadSystemSettingsTemplate(settingfilename);
        system->LoadfromJson(QString::fromStdString(options.loadModelJson));
    }
    else
    {
        Script scr(options.scriptFile, system.get());
        system->CreateFromScript(scr, settingfilename);
    }

    system->SetSilent(false);
    system->CalcAllInitialValues();

    cout << "Solving daily period for " << solveDateIso.toStdString() << " ..." << endl;
    if (!weatherForDay.isEmpty())
    {
        cout << "Weather data loaded for " << solveDateIso.toStdString() << " (keys: ";
        const auto keys = weatherForDay.keys();
        for (int i = 0; i < keys.size(); ++i)
        {
            cout << keys[i].toStdString();
            if (i + 1 < keys.size())
                cout << ", ";
        }
        cout << ")\n";
    }

    system->Solve();

    outputFilePath = QString::fromStdString(system->GetWorkingFolder() + system->OutputFileName());
    cout << "Writing outputs in '" << outputFilePath.toStdString() << "'" << endl;
    system->GetOutputs().write(outputFilePath.toStdString());

    PersistModelArtifacts(system.get(), options);
    return true;
}

} // namespace

int main(int argc, char *argv[])
{
    QCoreApplication a(argc, argv);

    AppOptions options;
    if (!ParseArgs(argc, argv, options))
    {
        return 1;
    }

    const QString weatherPath = QString::fromStdString(options.weatherFile);
    const QJsonArray weatherDays = LoadWeatherArray(weatherPath);

    const QString runnerStatePath = QString::fromStdString(options.runnerStateFile);
    const QJsonObject loadedState = LoadJsonFromFile(runnerStatePath);

    QDate solveDate = QDate::currentDate();
    if (loadedState.contains("next_run_date"))
    {
        const QDate loadedDate = QDate::fromString(loadedState.value("next_run_date").toString(), Qt::ISODate);
        if (loadedDate.isValid())
        {
            solveDate = loadedDate;
        }
    }

    int totalRuns = loadedState.value("runs_completed").toInt(0);

    auto runForDay = [&]() {
        const QString dayIso = solveDate.toString(Qt::ISODate);
        const QJsonObject weatherForDay = SelectWeatherForDate(weatherDays, dayIso);

        QString outputFilePath;
        const bool solved = BuildAndSolveSystem(options, dayIso, weatherForDay, outputFilePath);
        if (!solved)
        {
            return false;
        }

        ++totalRuns;
        const QJsonObject updatedState = BuildRunnerState(options, dayIso, totalRuns, outputFilePath);
        if (!runnerStatePath.isEmpty())
        {
            SaveJsonToFile(updatedState, runnerStatePath);
        }

        solveDate = solveDate.addDays(1);
        return true;
    };

    for (int i = 0; i < options.runDays; ++i)
    {
        if (!runForDay())
            return 2;
    }

    if (options.watchDaily)
    {
        QTimer timer;
        QObject::connect(&timer, &QTimer::timeout, &a, [&]() {
            if (!runForDay())
            {
                cerr << "Daily solve failed. Waiting for next cycle." << endl;
            }
        });
        timer.start(24 * 60 * 60 * 1000);
        cout << "watch-daily enabled. Next run date: " << solveDate.toString(Qt::ISODate).toStdString() << endl;
        return a.exec();
    }

    return 0;
}
