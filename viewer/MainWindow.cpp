#ifdef __EMSCRIPTEN__
#include <emscripten/val.h>
#endif

#include "MainWindow.h"
#include <QBrush>
#include <QColor>
#include <QCursor>
#include <QDateTime>
#include <QFont>
#include <QFrame>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QPainter>
#include <QPen>
#include <QPushButton>
#include <QScrollArea>
#include <QSpinBox>
#include <QStringList>
#include <QToolTip>
#include <QVBoxLayout>
#include <QWidget>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSplitter>
#include <QSvgWidget>
#include <limits>

// ------------------------------------------------------------------
// Per-series line colors. Cycles for series 6..N.
// ------------------------------------------------------------------
static const QStringList kSeriesColors = {
    "#2D7FF9", "#10B981", "#F59E0B",
    "#EF4444", "#8B5CF6", "#EC4899",
};

// Status-label helpers: colored dot + message.
static QString statusHtml(const QString &color, const QString &text)
{
    return QString("<span style='color:%1; font-size:14px;'>&#9679;</span>"
                   " <span>%2</span>").arg(color, text.toHtmlEscaped());
}

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
    root->setContentsMargins(20, 18, 20, 18);
    root->setSpacing(14);

    // ---- page header ----
    auto *title = new QLabel("DrywellDT Viewer");
    title->setObjectName("HeaderTitle");
    auto *subtitle = new QLabel("Observed outputs — live from the OpenHydroQual digital twin");
    subtitle->setObjectName("HeaderSubtitle");
    root->addWidget(title);
    root->addWidget(subtitle);

    // ---- top bar card: URL | refresh (s) | Last N | Refresh button ----
    auto *topBarCard = new QFrame();
    topBarCard->setObjectName("TopBarCard");
    auto *topBar = new QHBoxLayout(topBarCard);
    topBar->setContentsMargins(14, 12, 14, 12);
    topBar->setSpacing(10);

    topBar->addWidget(new QLabel("Refresh (s)"));
    m_intervalSpin = new QSpinBox();
    m_intervalSpin->setRange(5, 24 * 3600);
    m_intervalSpin->setValue(m_refreshSeconds);
    m_intervalSpin->setFixedWidth(90);
    topBar->addWidget(m_intervalSpin);

    topBar->addSpacing(4);
    topBar->addWidget(new QLabel("Last N (0=all)"));
    m_lastNSpin = new QSpinBox();
    m_lastNSpin->setRange(0, 10'000'000);
    m_lastNSpin->setValue(m_lastN);
    m_lastNSpin->setFixedWidth(100);
    topBar->addWidget(m_lastNSpin);

    m_refreshBtn = new QPushButton("Refresh");
    topBar->addWidget(m_refreshBtn);

    root->addWidget(topBarCard);

    m_statusLabel = new QLabel();
    m_statusLabel->setObjectName("StatusLabel");
    m_statusLabel->setTextFormat(Qt::RichText);
    m_statusLabel->setText(statusHtml("#9CA3AF", "Idle"));
    root->addWidget(m_statusLabel);

    // ---- splitter: charts (left) | SVG viz (right) ----
    // ---- splitter: charts (left) | SVG viz (right) ----
    auto *splitter = new QSplitter(Qt::Horizontal);
    splitter->setHandleWidth(6);
    splitter->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);

    // left: scrollable charts
    auto *scroll     = new QScrollArea();
    scroll->setWidgetResizable(true);
    scroll->setFrameShape(QFrame::NoFrame);
    scroll->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
    auto *chartsHost = new QWidget();
    m_chartsLayout   = new QVBoxLayout(chartsHost);
    m_chartsLayout->setContentsMargins(0, 0, 0, 0);
    m_chartsLayout->setSpacing(14);
    scroll->setWidget(chartsHost);

    // right: SVG panel
    m_svgWidget = new QSvgWidget();
    m_svgWidget->setMinimumWidth(280);
    m_svgWidget->setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Expanding);

    splitter->addWidget(scroll);
    splitter->addWidget(m_svgWidget);

    // Set initial sizes explicitly: 2/3 charts, 1/3 svg
    splitter->setSizes({780, 390});

    root->addWidget(splitter, 1);

    setCentralWidget(central);
    resize(1180, 820);

    // ---- signals ----
    connect(m_refreshBtn,   &QPushButton::clicked,
            this, &MainWindow::onRefreshClicked);
    connect(m_intervalSpin, QOverload<int>::of(&QSpinBox::valueChanged),
            this, &MainWindow::onIntervalChanged);
    connect(m_lastNSpin,    QOverload<int>::of(&QSpinBox::valueChanged),
            this, &MainWindow::onLastNChanged);
    connect(&m_loader, &CsvLoader::loaded, this, &MainWindow::onLoaded);
    connect(&m_loader, &CsvLoader::failed, this, &MainWindow::onFailed);
    connect(&m_timer,  &QTimer::timeout,   this, &MainWindow::onRefreshClicked);

    m_timer.setInterval(m_refreshSeconds * 1000);
    m_timer.start();

    m_timer.setInterval(m_refreshSeconds * 1000);
    // Don't start timer or fetch yet — wait for config
    loadConfig();  // <-- replaces onRefreshClicked()
}

void MainWindow::loadConfig()
{
    QUrl configUrl;

#ifdef __EMSCRIPTEN__
    std::string href = emscripten::val::global("window")["location"]["href"].as<std::string>();
    QUrl pageUrl(QString::fromStdString(href));
    configUrl = pageUrl.resolved(QUrl("config.json"));
#else
    configUrl = QUrl::fromLocalFile(
        QCoreApplication::applicationDirPath() + "/config.json");
#endif

    QNetworkReply *reply = m_configNam.get(QNetworkRequest(configUrl));
    connect(reply, &QNetworkReply::finished, this, [this, reply]() {
        onConfigReply(reply);
    });
}

void MainWindow::onConfigReply(QNetworkReply *reply)
{
    reply->deleteLater();
    if (reply->error() == QNetworkReply::NoError) {
        const QByteArray data = reply->readAll();
        qDebug() << "Config raw:" << data;
        const QJsonDocument doc = QJsonDocument::fromJson(data);
        qDebug() << "Config parsed:" << doc.object();
        const QString url = doc.object().value("csv_url").toString();
        qDebug() << "CSV URL from config:" << url;
        if (!url.isEmpty()) {
            m_url = QUrl(url);
        }
        const QString svgUrl = doc.object().value("viz_state_url").toString();
        if (!svgUrl.isEmpty())
            m_svgUrl = QUrl(svgUrl);
    } else {
        qDebug() << "Config load error:" << reply->error() << reply->errorString();
    }
    m_timer.start();
    onRefreshClicked();
}

void MainWindow::onRefreshClicked()
{
    m_statusLabel->setText(statusHtml("#F59E0B",
                                      QString("Fetching …")));
    m_loader.fetch(m_url);
    fetchSvg();   // <-- add this
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

void MainWindow::onFailed(const QString &err)
{
    m_statusLabel->setText(statusHtml("#EF4444", "Error: " + err));
}

void MainWindow::onLoaded(const QVector<CsvSeries> &series)
{
    m_lastData = series;
    m_statusLabel->setText(statusHtml("#10B981",
        QString("Loaded %1 series at %2")
            .arg(series.size())
            .arg(QDateTime::currentDateTime().toString("yyyy-MM-dd HH:mm:ss"))));
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

    const QColor gridColor("#EDF0F4");
    const QColor labelColor("#6B7684");
    const QColor titleColor("#1E2A38");
    const QFont  titleFont("Inter", 11, QFont::DemiBold);
    const QFont  labelFont("Inter", 10);

    for (int i = 0; i < series.size(); ++i) {
        const auto &s = series[i];

        ChartPanel panel;
        panel.name  = s.name;
        panel.chart = new QChart();
        panel.chart->setTitle(s.name);
        panel.chart->setTitleFont(QFont("Inter", 12, QFont::DemiBold));
        panel.chart->setTitleBrush(QBrush(titleColor));
        panel.chart->legend()->setVisible(true);
        panel.chart->legend()->setAlignment(Qt::AlignBottom);
        panel.chart->legend()->setFont(labelFont);
        panel.chart->legend()->setLabelColor(labelColor);
        panel.chart->setAnimationOptions(QChart::SeriesAnimations);
        panel.chart->setAnimationDuration(250);
        panel.chart->setMargins(QMargins(14, 10, 14, 10));
        panel.chart->setBackgroundBrush(QBrush(QColor("#FFFFFF")));
        panel.chart->setBackgroundPen(QPen(QColor("#E1E5EB")));
        panel.chart->setBackgroundRoundness(12);
        panel.chart->setPlotAreaBackgroundVisible(false);

        panel.series = new QLineSeries();
        panel.series->setName(s.name);
        panel.series->setPointsVisible(true);

        QPen pen{QColor(kSeriesColors[i % kSeriesColors.size()])};
        pen.setWidthF(2.2);
        pen.setCapStyle(Qt::RoundCap);
        pen.setJoinStyle(Qt::RoundJoin);
        panel.series->setPen(pen);

        panel.series->replace(s.points);
        panel.chart->addSeries(panel.series);

        connect(panel.series, &QLineSeries::hovered,
                this, &MainWindow::onSeriesHovered);

        panel.axisX = new QDateTimeAxis();
        panel.axisX->setFormat("yyyy-MM-dd HH:mm");
        panel.axisX->setTitleText("Time (UTC)");
        panel.axisX->setTitleFont(titleFont);
        panel.axisX->setLabelsFont(labelFont);
        panel.axisX->setTitleBrush(QBrush(labelColor));
        panel.axisX->setLabelsColor(labelColor);
        panel.axisX->setGridLineColor(gridColor);
        panel.axisX->setLinePenColor(gridColor);
        panel.chart->addAxis(panel.axisX, Qt::AlignBottom);
        panel.series->attachAxis(panel.axisX);

        panel.axisY = new QValueAxis();
        panel.axisY->setTitleText(s.name);
        panel.axisY->setTitleFont(titleFont);
        panel.axisY->setLabelsFont(labelFont);
        panel.axisY->setTitleBrush(QBrush(labelColor));
        panel.axisY->setLabelsColor(labelColor);
        panel.axisY->setGridLineColor(gridColor);
        panel.axisY->setLinePenColor(gridColor);
        panel.chart->addAxis(panel.axisY, Qt::AlignLeft);
        panel.series->attachAxis(panel.axisY);

        if (!s.points.isEmpty()) {
            panel.axisX->setRange(QDateTime::fromMSecsSinceEpoch(s.xMin),
                                  QDateTime::fromMSecsSinceEpoch(s.xMax));
            panel.axisY->setRange(s.yMin, s.yMax);
        }

        panel.view = new QChartView(panel.chart);
        panel.view->setRenderHint(QPainter::Antialiasing);
        panel.view->setFrameShape(QFrame::NoFrame);
        panel.view->setStyleSheet("background: transparent;");
        panel.view->setMinimumHeight(280);

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

void MainWindow::fetchSvg()
{
    if (m_svgUrl.isEmpty()) return;
    QNetworkReply *reply = m_svgNam.get(QNetworkRequest(m_svgUrl));
    connect(reply, &QNetworkReply::finished, this, [this, reply]() {
        onSvgFetched(reply);
    });
}

void MainWindow::onSvgFetched(QNetworkReply *reply)
{
    reply->deleteLater();
    if (reply->error() == QNetworkReply::NoError)
        m_svgWidget->load(reply->readAll());
}
