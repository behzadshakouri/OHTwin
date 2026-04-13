#include "noaaweatherfetcher.h"

#include <QEventLoop>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QRegularExpression>
#include <iostream>

NOAAWeatherFetcher::NOAAWeatherFetcher(QObject *parent)
    : QObject(parent)
    , manager(new QNetworkAccessManager(this))
{}

// ---------------------------------------------------------------------------
// parseDurationSecs
// Parses ISO 8601 duration: P[nD][T[nH][nM][nS]]
// Examples: PT1H → 3600   P1D → 86400   P7DT14H → 655200   PT30M → 1800
// ---------------------------------------------------------------------------
qint64 NOAAWeatherFetcher::parseDurationSecs(const QString &dur)
{
    qint64 secs = 0;

    // Split into date part and time part at 'T'
    // e.g. "P7DT14H" → datePart="P7D", timePart="14H"
    //      "PT1H"    → datePart="P",   timePart="1H"
    //      "P1D"     → datePart="P1D", timePart=""
    const int tIdx = dur.indexOf('T');
    const QString datePart = (tIdx == -1) ? dur : dur.left(tIdx);
    const QString timePart = (tIdx == -1) ? QString() : dur.mid(tIdx + 1);

    QRegularExpression dayRx("(\\d+)D");
    QRegularExpression hourRx("(\\d+)H");
    QRegularExpression minRx("(\\d+)M");
    QRegularExpression secRx("(\\d+)S");

    auto match = dayRx.match(datePart);
    if (match.hasMatch())
        secs += match.captured(1).toLongLong() * 86400;

    match = hourRx.match(timePart);
    if (match.hasMatch())
        secs += match.captured(1).toLongLong() * 3600;

    match = minRx.match(timePart);
    if (match.hasMatch())
        secs += match.captured(1).toLongLong() * 60;

    match = secRx.match(timePart);
    if (match.hasMatch())
        secs += match.captured(1).toLongLong();

    return secs;
}

// ---------------------------------------------------------------------------
// getWeatherPrediction
// ---------------------------------------------------------------------------
QVector<WeatherData> NOAAWeatherFetcher::getWeatherPrediction(
    const QString &office, int gridX, int gridY, datatype type)
{
    QVector<WeatherData> result;

    const QString url = QString("https://api.weather.gov/gridpoints/%1/%2,%3")
                            .arg(office).arg(gridX).arg(gridY);

    QNetworkRequest request((QUrl(url)));
    request.setHeader(QNetworkRequest::ContentTypeHeader, "application/json");
    // NOAA requires a User-Agent header or it returns 403
    request.setRawHeader("User-Agent", "DTRunner/1.0 (openhydroqual@example.com)");

    QString dataTypeKey;
    switch (type) {
    case datatype::PrecipitationAmount:        dataTypeKey = "quantitativePrecipitation"; break;
    case datatype::ProbabilityofPrecipitation: dataTypeKey = "probabilityOfPrecipitation"; break;
    case datatype::RelativeHumidity:           dataTypeKey = "relativeHumidity"; break;
    case datatype::Temperature:                dataTypeKey = "temperature"; break;
    }

    QNetworkReply *reply = manager->get(request);
    QEventLoop loop;
    connect(reply, &QNetworkReply::finished, &loop, &QEventLoop::quit);
    loop.exec();

    if (reply->error() != QNetworkReply::NoError) {
        std::cerr << "[NOAA] Network error: "
                  << reply->errorString().toStdString() << "\n";
        reply->deleteLater();
        return result;
    }

    const QJsonObject root =
        QJsonDocument::fromJson(reply->readAll()).object();
    reply->deleteLater();

    const QJsonArray values =
        root["properties"].toObject()[dataTypeKey].toObject()["values"].toArray();

    if (values.isEmpty()) {
        std::cerr << "[NOAA] No values found for key: "
                  << dataTypeKey.toStdString() << "\n";
        return result;
    }

    for (const auto &entry : values) {
        const QJsonObject obj = entry.toObject();
        const QString validTime = obj["validTime"].toString();

        // Split "2026-04-13T14:00:00+00:00/PT1H" → datetime + duration
        const QStringList parts = validTime.split('/');
        if (parts.size() != 2) continue;

        // Parse start time — Qt handles the +00:00 offset natively
        const QDateTime start =
            QDateTime::fromString(parts[0], Qt::ISODate).toUTC();
        if (!start.isValid()) continue;

        const qint64 durSecs = parseDurationSecs(parts[1]);
        if (durSecs <= 0) continue;

        const QDateTime end = start.addSecs(durSecs);
        const double value  = obj["value"].toDouble();

        result.push_back({ start, end, value });
    }

    std::cout << "[NOAA] Fetched " << result.size()
              << " " << dataTypeKey.toStdString() << " records\n";
    return result;
}
