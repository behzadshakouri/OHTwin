#ifdef __EMSCRIPTEN__
#include <emscripten/val.h>
#endif

#include "MainWindow.h"
#include "OhqTime.h"

#include <QBrush>
#include <QButtonGroup>
#include <QColor>
#include <QCoreApplication>
#include <QDebug>
#include <QCursor>
#include <QDateTime>
#include <QFont>
#include <QFrame>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QPainter>
#include <QNetworkRequest>
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
#include <QSizePolicy>
#include <QSvgWidget>
#include <limits>
#include <string>

#include <QtCharts/QChart>
#include <QtCharts/QChartView>
#include <QtCharts/QDateTimeAxis>
#include <QtCharts/QLegend>
#include <QtCharts/QLegendMarker>
#include <QtCharts/QLineSeries>
#include <QtCharts/QValueAxis>
#include <QUrlQuery>
// Qt 6 QtCharts: classes are in the global namespace — no `using namespace` needed.

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


// Build a no-cache request for all live viewer assets (CSV, SVG, JSON).
// This avoids stale nginx/browser/WASM cache results during repeated refreshes.
static QNetworkRequest noCacheRequest(QUrl url)
{
    QUrlQuery q(url);
    q.addQueryItem(QStringLiteral("_t"),
                   QString::number(QDateTime::currentMSecsSinceEpoch()));
    url.setQuery(q);

    QNetworkRequest req(url);
    req.setAttribute(QNetworkRequest::CacheLoadControlAttribute,
                     QNetworkRequest::AlwaysNetwork);
    return req;
}

// Resolve a URL string against config.json's URL. Absolute URLs remain
// unchanged; relative paths such as "viz.svg" become siblings of config.json.
static QUrl resolvedConfigUrl(const QUrl &configUrl, const QString &value)
{
    if (value.trimmed().isEmpty())
        return {};
    return configUrl.resolved(QUrl(value.trimmed()));
}

// Default CSV location when served by nginx locally, alongside the wasm.
// The user can edit this field in the UI.
static constexpr const char *kDefaultCsvUrl =
    "http://localhost/selected_output.csv";

MainWindow::MainWindow(QWidget *parent) : QMainWindow(parent)
{
    setWindowTitle("OHTwin Viewer");

    m_url = QUrl(QString::fromLatin1(kDefaultCsvUrl));

    auto *central = new QWidget(this);
    auto *root    = new QVBoxLayout(central);
    root->setContentsMargins(20, 18, 20, 18);
    root->setSpacing(14);

    // ---- page header ----
    auto *title = new QLabel("OHTwin Viewer");
    title->setObjectName("HeaderTitle");
    auto *subtitle = new QLabel("Observed outputs — live from the OpenHydroQual Digital Twin");
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

    // right: SVG panel with Current/Forecast toggle on top
    auto *svgPanel  = new QWidget();
    auto *svgLayout = new QVBoxLayout(svgPanel);
    svgLayout->setContentsMargins(0, 0, 0, 0);
    svgLayout->setSpacing(8);

    // Segmented pill toggle: two buttons inside a rounded frame, styled
    // so that the checked half is filled and the unchecked half is text-only.
    auto *toggleRow = new QHBoxLayout();
    toggleRow->setContentsMargins(0, 0, 0, 0);
    toggleRow->setSpacing(0);

    auto *pill = new QFrame();
    pill->setObjectName("ModePill");
    auto *pillLayout = new QHBoxLayout(pill);
    pillLayout->setContentsMargins(2, 2, 2, 2);
    pillLayout->setSpacing(0);

    m_currentBtn  = new QPushButton("Current");
    m_forecastBtn = new QPushButton("Forecast");
    m_currentBtn ->setObjectName("ModePillBtn");
    m_forecastBtn->setObjectName("ModePillBtn");
    m_currentBtn ->setCheckable(true);
    m_forecastBtn->setCheckable(true);
    m_currentBtn ->setChecked(true);
    m_currentBtn ->setCursor(Qt::PointingHandCursor);
    m_forecastBtn->setCursor(Qt::PointingHandCursor);

    auto *modeGroup = new QButtonGroup(this);
    modeGroup->setExclusive(true);
    modeGroup->addButton(m_currentBtn);
    modeGroup->addButton(m_forecastBtn);

    pillLayout->addWidget(m_currentBtn);
    pillLayout->addWidget(m_forecastBtn);

    pill->setStyleSheet(R"(
        QFrame#ModePill {
            background-color: #F1F3F5;
            border: 1px solid #E1E5EB;
            border-radius: 14px;
        }
        QPushButton#ModePillBtn {
            background-color: transparent;
            border: none;
            border-radius: 12px;
            color: #6B7684;
            font-weight: 500;
            padding: 5px 18px;
            min-height: 18px;
        }
        QPushButton#ModePillBtn:hover:!checked {
            color: #1E2A38;
        }
        QPushButton#ModePillBtn:checked {
            background-color: #FFFFFF;
            color: #1E2A38;
            font-weight: 600;
        }
    )");

    toggleRow->addWidget(pill);
    toggleRow->addStretch(1);
    svgLayout->addLayout(toggleRow);

    m_svgWidget = new QSvgWidget();
    m_svgWidget->setMinimumWidth(280);
    m_svgWidget->setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Expanding);
    svgLayout->addWidget(m_svgWidget, 1);

    splitter->addWidget(scroll);
    splitter->addWidget(svgPanel);

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

    connect(m_currentBtn,  &QPushButton::clicked, this, &MainWindow::onModeToggled);
    connect(m_forecastBtn, &QPushButton::clicked, this, &MainWindow::onModeToggled);

    m_timer.setInterval(m_refreshSeconds * 1000);
    // Don't start timer or fetch yet — wait for config
    loadConfig();
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

    // Cache-bust config.json so deployment edits are picked up immediately.
    QNetworkReply *reply = m_configNam.get(noCacheRequest(configUrl));
    connect(reply, &QNetworkReply::finished, this, [this, reply]() {
        onConfigReply(reply);
    });
}

void MainWindow::onConfigReply(QNetworkReply *reply)
{
    const QUrl configUrl = reply->url();

    if (reply->error() == QNetworkReply::NoError) {
        QJsonParseError parseError;
        const QByteArray data = reply->readAll();
        const QJsonDocument doc = QJsonDocument::fromJson(data, &parseError);

        if (!doc.isObject()) {
            qDebug() << "Config parse error:" << parseError.errorString();
            m_statusLabel->setText(statusHtml("#EF4444",
                                               "Config parse error: " + parseError.errorString()));
        } else {
            const QJsonObject obj = doc.object();

            const QString csv = obj.value("csv_url").toString();
            if (!csv.isEmpty())
                m_url = resolvedConfigUrl(configUrl, csv);

            const QString viz = obj.value("viz_url").toString();
            if (!viz.isEmpty())
                m_vizUrl = resolvedConfigUrl(configUrl, viz);

            const QString vizS = obj.value("viz_state_url").toString();
            if (!vizS.isEmpty())
                m_vizStateUrl = resolvedConfigUrl(configUrl, vizS);

            const QString fviz = obj.value("forecast_viz_url").toString();
            if (!fviz.isEmpty())
                m_forecastVizUrl = resolvedConfigUrl(configUrl, fviz);

            const QString fvizS = obj.value("forecast_viz_state_url").toString();
            if (!fvizS.isEmpty())
                m_forecastVizStateUrl = resolvedConfigUrl(configUrl, fvizS);

            qDebug() << "Config: csv="          << m_url
                     << "viz="                  << m_vizUrl
                     << "viz_state="            << m_vizStateUrl
                     << "forecast_viz="         << m_forecastVizUrl
                     << "forecast_viz_state="   << m_forecastVizStateUrl;
        }
    } else {
        qDebug() << "Config load error:" << reply->error() << reply->errorString();
        m_statusLabel->setText(statusHtml("#EF4444",
                                           "Config load error: " + reply->errorString()));
    }

    reply->deleteLater();

    m_timer.start();
    onRefreshClicked();
}

void MainWindow::onRefreshClicked()
{
    m_statusLabel->setText(statusHtml("#F59E0B", QString("Fetching …")));
    m_loader.fetch(m_url);

    // Current SVG
    if (!m_vizUrl.isEmpty()) {
        QNetworkReply *r = m_svgNam.get(noCacheRequest(m_vizUrl));
        connect(r, &QNetworkReply::finished, this, [this, r]() { onSvgFetched(r); });
    }
    // Forecast SVG
    if (!m_forecastVizUrl.isEmpty()) {
        QNetworkReply *r = m_forecastSvgNam.get(noCacheRequest(m_forecastVizUrl));
        connect(r, &QNetworkReply::finished, this, [this, r]() { onForecastSvgFetched(r); });
    }
    // Current viz state JSON (for the boundary timestamp)
    if (!m_vizStateUrl.isEmpty()) {
        QNetworkReply *r = m_vizStateNam.get(noCacheRequest(m_vizStateUrl));
        connect(r, &QNetworkReply::finished, this, [this, r]() { onVizStateFetched(r); });
    }
    // Forecast viz state JSON (for the forecast-end timestamp)
    if (!m_forecastVizStateUrl.isEmpty()) {
        QNetworkReply *r = m_forecastVizStateNam.get(noCacheRequest(m_forecastVizStateUrl));
        connect(r, &QNetworkReply::finished, this, [this, r]() { onForecastVizStateFetched(r); });
    }
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

    if (s.points.isEmpty()) {
        s.yMin = 0.0;
        s.yMax = 1.0;
        s.xMin = 0;
        s.xMax = 1;
        return;
    }

    if (s.yMin == s.yMax) {
        s.yMin -= 1.0;
        s.yMax += 1.0;
    }
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

    applyBoundaryToPanels();
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

    applyBoundaryToPanels();
}

// ---------------------------------------------------------------------------
// SVG / state JSON fetch handlers
// ---------------------------------------------------------------------------
void MainWindow::onSvgFetched(QNetworkReply *reply)
{
    reply->deleteLater();
    if (reply->error() == QNetworkReply::NoError) {
        m_currentSvgBytes = reply->readAll();
        applySvgForCurrentMode();
    }
}

void MainWindow::onForecastSvgFetched(QNetworkReply *reply)
{
    reply->deleteLater();
    if (reply->error() == QNetworkReply::NoError) {
        m_forecastSvgBytes = reply->readAll();
        applySvgForCurrentMode();
    }
}

void MainWindow::onVizStateFetched(QNetworkReply *reply)
{
    reply->deleteLater();
    if (reply->error() != QNetworkReply::NoError) return;
    parseVizStateBoundary(reply->readAll(), m_advanceEndMs);
    applyBoundaryToPanels();
}

void MainWindow::onForecastVizStateFetched(QNetworkReply *reply)
{
    reply->deleteLater();
    if (reply->error() != QNetworkReply::NoError) return;
    parseVizStateBoundary(reply->readAll(), m_forecastEndMs);
    // No chart redraw needed for the forecast-end value alone right now.
}

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------
void MainWindow::parseVizStateBoundary(const QByteArray &data, qint64 &outMs)
{
    const QJsonDocument doc = QJsonDocument::fromJson(data);
    if (!doc.isObject()) return;

    const QJsonObject root   = doc.object();
    const QJsonObject solver = root.value("solver_state").toObject();
    if (!solver.contains("t")) return;

    const double serial = solver.value("t").toDouble();
    outMs = ohqSerialToMsEpoch(serial);
}

void MainWindow::applySvgForCurrentMode()
{
    const QByteArray &bytes = m_showForecast ? m_forecastSvgBytes : m_currentSvgBytes;
    if (!bytes.isEmpty())
        m_svgWidget->load(bytes);
}

void MainWindow::onModeToggled()
{
    m_showForecast = m_forecastBtn->isChecked();
    applySvgForCurrentMode();
}

void MainWindow::applyBoundaryToPanels()
{
    if (m_advanceEndMs < 0) return;

    const QColor boundaryColor("#9CA3AF");  // gray
    QPen pen(boundaryColor);
    pen.setWidthF(1.4);
    pen.setStyle(Qt::DashLine);

    for (auto &p : m_panels) {
        if (!p.chart || !p.axisX || !p.axisY) continue;

        // Lazily create the boundary series
        if (!p.boundary) {
            p.boundary = new QLineSeries();
            p.boundary->setName("now");
            p.boundary->setPen(pen);
            p.chart->addSeries(p.boundary);
            p.boundary->attachAxis(p.axisX);
            p.boundary->attachAxis(p.axisY);
            // Hide from legend (it's metadata, not data)
            const auto markers = p.chart->legend()->markers(p.boundary);
            for (auto *m : markers) m->setVisible(false);
        }

        // Span the current Y range so the line is full-height
        const double yMin = p.axisY->min();
        const double yMax = p.axisY->max();
        QVector<QPointF> pts;
        pts.append(QPointF(static_cast<qreal>(m_advanceEndMs), yMin));
        pts.append(QPointF(static_cast<qreal>(m_advanceEndMs), yMax));
        p.boundary->replace(pts);
    }
}
