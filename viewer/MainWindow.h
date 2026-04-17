#pragma once

#include <QMainWindow>
#include <QTimer>
#include <QUrl>
#include <QVector>
#include "CsvLoader.h"

class QLineEdit;
class QSpinBox;
class QPushButton;
class QLabel;
class QVBoxLayout;

// Qt 6 removed the QtCharts namespace — classes live in the global namespace.
class QChart;
class QChartView;
class QLineSeries;
class QDateTimeAxis;
class QValueAxis;

struct ChartPanel
{
    QChart        *chart  = nullptr;
    QLineSeries   *series = nullptr;
    QDateTimeAxis *axisX  = nullptr;
    QValueAxis    *axisY  = nullptr;
    QChartView    *view   = nullptr;
    QString        name;
};

class MainWindow : public QMainWindow
{
    Q_OBJECT
public:
    explicit MainWindow(QWidget *parent = nullptr);

private slots:
    void onRefreshClicked();
    void onIntervalChanged(int seconds);
    void onLastNChanged(int n);
    void onUrlChanged();
    void onLoaded(const QVector<CsvSeries> &series);
    void onFailed(const QString &err);
    void onSeriesHovered(const QPointF &point, bool state);

private:
    void display();                                          // slice m_lastData by m_lastN and render
    void rebuildPanels(const QVector<CsvSeries> &series);
    void updatePanels (const QVector<CsvSeries> &series);
    static void recomputeBounds(CsvSeries &s);

    QLineEdit   *m_urlEdit       = nullptr;
    QSpinBox    *m_intervalSpin  = nullptr;
    QSpinBox    *m_lastNSpin     = nullptr;
    QPushButton *m_refreshBtn    = nullptr;
    QLabel      *m_statusLabel   = nullptr;
    QVBoxLayout *m_chartsLayout  = nullptr;

    QTimer    m_timer;
    CsvLoader m_loader;

    QVector<ChartPanel> m_panels;
    QVector<CsvSeries>  m_lastData;   // full dataset from most recent fetch

    QUrl m_url;
    int  m_refreshSeconds = 300;   // default 5 min, editable from UI
    int  m_lastN          = 0;     // 0 = show all points
};
