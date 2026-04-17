#include "MainWindow.h"

#include <QCursor>
#include <QDateTime>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QPainter>
#include <QPushButton>
#include <QScrollArea>
#include <QSpinBox>
#include <QToolTip>
#include <QVBoxLayout>
#include <QWidget>

#include <limits>

#include <QtCharts/QChart>
#include <QtCharts/QChartView>
#include <QtCharts/QDateTimeAxis>
#include <QtCharts/QLegend>
#include <QtCharts/QLineSeries>
#include <QtCharts/QValueAxis>
// Qt 6 QtCharts: classes are in the global namespace — no `using namespace` needed.

// Default CSV location when served by nginx locally, alongside the wasm.
// The user can edit this field in the UI.
static constexpr const char *kDefaultCsvUrl =
    "http://localhost/selected_output.csv";

MainWindow::MainWindow(QWidget *parent) : QMainWindow(parent)
{
    setWindowTitle("DrywellDT Viewer");

    m_url = QUrl(QString::fromLatin1(kDefaultCsvUrl));

    auto *central = new QWidget(this);
    auto *root    = new QVBoxLayout(central);

    // ---- top bar: URL | refresh (s) | Refresh button ----
    auto *topBar = new QHBoxLayout();

    topBar->addWidget(new QLabel("CSV URL:"));
    m_urlEdit = new QLineEdit(m_url.toString());
    topBar->addWidget(m_urlEdit, 1);

    topBar->addWidget(new QLabel("Refresh (s):"));
    m_intervalSpin = new QSpinBox();
    m_intervalSpin->setRange(5, 24 * 3600);
    m_intervalSpin->setValue(m_refreshSeconds);
    topBar->addWidget(m_intervalSpin);

    topBar->addWidget(new QLabel("Last N (0=all):"));
    m_lastNSpin = new QSpinBox();
    m_lastNSpin->setRange(0, 10'000'000);
    m_lastNSpin->setValue(m_lastN);
    topBar->addWidget(m_lastNSpin);

    m_refreshBtn = new QPushButton("Refresh");
    topBar->addWidget(m_refreshBtn);

    root->addLayout(topBar);

    m_statusLabel = new QLabel("Idle");
    root->addWidget(m_statusLabel);

    // ---- scrollable column of per-series charts ----
    auto *scroll     = new QScrollArea();
    scroll->setWidgetResizable(true);
    auto *chartsHost = new QWidget();
    m_chartsLayout   = new QVBoxLayout(chartsHost);
    m_chartsLayout->setContentsMargins(0, 0, 0, 0);
    scroll->setWidget(chartsHost);
    root->addWidget(scroll, 1);

    setCentralWidget(central);
    resize(1100, 800);

    // ---- signals ----
    connect(m_refreshBtn,   &QPushButton::clicked,
            this, &MainWindow::onRefreshClicked);
    connect(m_intervalSpin, QOverload<int>::of(&QSpinBox::valueChanged),
            this, &MainWindow::onIntervalChanged);
    connect(m_lastNSpin,    QOverload<int>::of(&QSpinBox::valueChanged),
            this, &MainWindow::onLastNChanged);
    connect(m_urlEdit,      &QLineEdit::editingFinished,
            this, &MainWindow::onUrlChanged);
    connect(&m_loader, &CsvLoader::loaded, this, &MainWindow::onLoaded);
    connect(&m_loader, &CsvLoader::failed, this, &MainWindow::onFailed);
    connect(&m_timer,  &QTimer::timeout,   this, &MainWindow::onRefreshClicked);

    m_timer.setInterval(m_refreshSeconds * 1000);
    m_timer.start();

    onRefreshClicked();  // initial fetch
}

void MainWindow::onRefreshClicked()
{
    m_statusLabel->setText(QString("Fetching %1 ...").arg(m_url.toString()));
    m_loader.fetch(m_url);
}

void MainWindow::onIntervalChanged(int seconds)
{
    m_refreshSeconds = seconds;
    m_timer.setInterval(m_refreshSeconds * 1000);
}

void MainWindow::onLastNChanged(int n)
{
    m_lastN = n;
    display();   // re-slice + redraw without re-fetching
}

void MainWindow::onUrlChanged()
{
    const QString text = m_urlEdit->text().trimmed();
    if (text.isEmpty()) return;
    m_url = QUrl(text);
    onRefreshClicked();
}

void MainWindow::onFailed(const QString &err)
{
    m_statusLabel->setText("Error: " + err);
}

void MainWindow::onLoaded(const QVector<CsvSeries> &series)
{
    m_lastData = series;
    m_statusLabel->setText(QString("Loaded %1 series at %2")
                               .arg(series.size())
                               .arg(QDateTime::currentDateTime()
                                        .toString(Qt::ISODate)));
    display();
}

void MainWindow::display()
{
    QVector<CsvSeries> view = m_lastData;
    if (m_lastN > 0) {
        for (auto &s : view) {
            if (s.points.size() > m_lastN)
                s.points = s.points.mid(s.points.size() - m_lastN);
            recomputeBounds(s);
        }
    }

    bool needRebuild = (m_panels.size() != view.size());
    if (!needRebuild) {
        for (int i = 0; i < view.size(); ++i) {
            if (m_panels[i].name != view[i].name) { needRebuild = true; break; }
        }
    }
    if (needRebuild) rebuildPanels(view);
    else             updatePanels (view);
}

void MainWindow::recomputeBounds(CsvSeries &s)
{
    s.yMin =  std::numeric_limits<double>::infinity();
    s.yMax = -std::numeric_limits<double>::infinity();
    s.xMin =  std::numeric_limits<qint64>::max();
    s.xMax =  std::numeric_limits<qint64>::min();
    for (const auto &p : s.points) {
        s.yMin = qMin(s.yMin, p.y());
        s.yMax = qMax(s.yMax, p.y());
        s.xMin = qMin(s.xMin, qint64(p.x()));
        s.xMax = qMax(s.xMax, qint64(p.x()));
    }
    if (s.yMin == s.yMax) { s.yMin -= 1.0; s.yMax += 1.0; }
}

void MainWindow::onSeriesHovered(const QPointF &point, bool state)
{
    if (!state) { QToolTip::hideText(); return; }
    const QDateTime dt = QDateTime::fromMSecsSinceEpoch(qint64(point.x()));
    QToolTip::showText(QCursor::pos(),
                       QString("%1\n%2")
                           .arg(dt.toString("yyyy-MM-dd HH:mm:ss"))
                           .arg(point.y(), 0, 'g', 6));
}

void MainWindow::rebuildPanels(const QVector<CsvSeries> &series)
{
    for (auto &p : m_panels) {
        if (p.view) {
            m_chartsLayout->removeWidget(p.view);
            p.view->deleteLater();   // owns chart, which owns series + axes
        }
    }
    m_panels.clear();

    for (const auto &s : series) {
        ChartPanel panel;
        panel.name   = s.name;
        panel.chart  = new QChart();
        panel.chart->setTitle(s.name);
        panel.chart->legend()->setVisible(true);
        panel.chart->legend()->setAlignment(Qt::AlignBottom);
        panel.chart->setMargins(QMargins(4, 4, 4, 4));

        panel.series = new QLineSeries();
        panel.series->setName(s.name);
        panel.series->setPointsVisible(true);
        panel.series->replace(s.points);
        panel.chart->addSeries(panel.series);

        connect(panel.series, &QLineSeries::hovered,
                this, &MainWindow::onSeriesHovered);

        panel.axisX = new QDateTimeAxis();
        panel.axisX->setFormat("yyyy-MM-dd HH:mm");
        panel.axisX->setTitleText("Time (UTC)");
        panel.chart->addAxis(panel.axisX, Qt::AlignBottom);
        panel.series->attachAxis(panel.axisX);

        panel.axisY = new QValueAxis();
        panel.axisY->setTitleText(s.name);
        panel.chart->addAxis(panel.axisY, Qt::AlignLeft);
        panel.series->attachAxis(panel.axisY);

        if (!s.points.isEmpty()) {
            panel.axisX->setRange(QDateTime::fromMSecsSinceEpoch(s.xMin),
                                  QDateTime::fromMSecsSinceEpoch(s.xMax));
            panel.axisY->setRange(s.yMin, s.yMax);
        }

        panel.view = new QChartView(panel.chart);
        panel.view->setRenderHint(QPainter::Antialiasing);
        panel.view->setMinimumHeight(260);

        m_chartsLayout->addWidget(panel.view);
        m_panels.push_back(panel);
    }
}

void MainWindow::updatePanels(const QVector<CsvSeries> &series)
{
    for (int i = 0; i < series.size(); ++i) {
        const auto &s = series[i];
        auto       &p = m_panels[i];
        p.series->replace(s.points);
        if (!s.points.isEmpty()) {
            p.axisX->setRange(QDateTime::fromMSecsSinceEpoch(s.xMin),
                              QDateTime::fromMSecsSinceEpoch(s.xMax));
            p.axisY->setRange(s.yMin, s.yMax);
        }
    }
}
