#include "CsvLoader.h"

#include <QNetworkReply>
#include <QNetworkRequest>
#include <QDateTime>
#include <QRegularExpression>
#include <QUrlQuery>
#include <QStringList>

// OHQ / Excel epoch: day-serial 0 = 1899-12-30 UTC
static const QDateTime kOHQEpoch = QDateTime(QDate(1899, 12, 30),
                                             QTime(0, 0, 0), Qt::UTC);

static qint64 ohqSerialToMsEpoch(double serial)
{
    const qint64 offsetMs = static_cast<qint64>(serial * 86400.0 * 1000.0);
    return kOHQEpoch.addMSecs(offsetMs).toMSecsSinceEpoch();
}

CsvLoader::CsvLoader(QObject *parent) : QObject(parent) {}

void CsvLoader::fetch(const QUrl &url)
{
    QUrl u = url;
    QUrlQuery q(u);
    q.addQueryItem(QStringLiteral("_t"),
                   QString::number(QDateTime::currentMSecsSinceEpoch()));
    u.setQuery(q);

    QNetworkRequest req(u);
    req.setAttribute(QNetworkRequest::CacheLoadControlAttribute,
                     QNetworkRequest::AlwaysNetwork);

    QNetworkReply *reply = m_nam.get(req);
    connect(reply, &QNetworkReply::finished, this, [this, reply]() {
        if (reply->error() != QNetworkReply::NoError)
            emit failed(reply->errorString());
        else
            parse(reply->readAll());
        reply->deleteLater();
    });
}

void CsvLoader::parse(const QByteArray &data)
{
    const QString text = QString::fromUtf8(data);
    const QStringList lines = text.split(QRegularExpression("[\r\n]+"),
                                         Qt::SkipEmptyParts);
    if (lines.size() < 2) { emit failed("CSV has fewer than 2 lines"); return; }

    const QStringList headerCols = lines[0].split(',');
    if (headerCols.size() < 2 || headerCols.size() % 2 != 0) {
        emit failed(QString("CSV header has %1 columns; expected even count "
                            "(t, value pairs)").arg(headerCols.size()));
        return;
    }
    const int seriesCount = headerCols.size() / 2;

    QVector<CsvSeries> series(seriesCount);
    for (int i = 0; i < seriesCount; ++i)
        series[i].name = headerCols[2 * i + 1].trimmed();

    for (int row = 1; row < lines.size(); ++row) {
        const QStringList cols = lines[row].split(',');
        if (cols.size() < seriesCount * 2) continue;
        for (int i = 0; i < seriesCount; ++i) {
            bool okT = false, okV = false;
            const double t = cols[2 * i].trimmed().toDouble(&okT);
            const double v = cols[2 * i + 1].trimmed().toDouble(&okV);
            if (!okT || !okV) continue;
            const qint64 xMs = ohqSerialToMsEpoch(t);
            series[i].points.append(QPointF(static_cast<qreal>(xMs), v));
            series[i].yMin = qMin(series[i].yMin, v);
            series[i].yMax = qMax(series[i].yMax, v);
            series[i].xMin = qMin(series[i].xMin, xMs);
            series[i].xMax = qMax(series[i].xMax, xMs);
        }
    }

    // Pad flat y-ranges so the axis isn't a hairline
    for (auto &s : series) {
        if (s.yMin == s.yMax) { s.yMin -= 1.0; s.yMax += 1.0; }
    }

    emit loaded(series);
}
