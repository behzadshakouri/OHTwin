#pragma once

#include <QDateTime>
#include <QObject>
#include <QVector>

class QNetworkAccessManager;

enum class datatype {
    ProbabilityofPrecipitation,
    Temperature,
    PrecipitationAmount,
    RelativeHumidity
};

struct WeatherData {
    QDateTime startTime;   // bin start (was: timestamp)
    QDateTime endTime;     // bin end (startTime + duration)
    double    value;
};

class NOAAWeatherFetcher : public QObject {
    Q_OBJECT
public:
    explicit NOAAWeatherFetcher(QObject *parent = nullptr);

    // office: e.g. "LWX",  gridX/gridY: integer grid coords
    QVector<WeatherData> getWeatherPrediction(
        const QString &office, int gridX, int gridY, datatype type);

private:
    // Parse ISO 8601 duration string → seconds
    // Handles: PT1H, PT6H, P1D, P7DT14H, PT30M etc.
    static qint64 parseDurationSecs(const QString &duration);

    QNetworkAccessManager *manager;
};
