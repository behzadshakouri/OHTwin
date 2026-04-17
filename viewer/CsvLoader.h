#pragma once

#include <QObject>
#include <QNetworkAccessManager>
#include <QPointF>
#include <QString>
#include <QUrl>
#include <QVector>
#include <QList>
#include <limits>

// One (t, value) pair from the CSV, converted to wall-clock.
//   x  : QDateTime::toMSecsSinceEpoch  (from OHQ day-serial)
//   y  : value as read from CSV
struct CsvSeries
{
    QString        name;
    QList<QPointF> points;
    double yMin =  std::numeric_limits<double>::infinity();
    double yMax = -std::numeric_limits<double>::infinity();
    qint64 xMin =  std::numeric_limits<qint64>::max();
    qint64 xMax =  std::numeric_limits<qint64>::min();
};

// Fetches the selected_output.csv from a URL and parses interleaved
// (t, value) column pairs into CsvSeries objects.
class CsvLoader : public QObject
{
    Q_OBJECT
public:
    explicit CsvLoader(QObject *parent = nullptr);

    // Fires off an async GET. A cache-busting query param is appended
    // so repeated fetches do not return a stale browser cache entry.
    void fetch(const QUrl &url);

signals:
    void loaded(const QVector<CsvSeries> &series);
    void failed(const QString &error);

private:
    void parse(const QByteArray &data);

    QNetworkAccessManager m_nam;
};
