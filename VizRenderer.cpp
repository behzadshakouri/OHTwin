#include "VizRenderer.h"

#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QTextStream>
#include <QVector>
#include <cmath>
#include <limits>

// ============================================================
// Internal data structures
// ============================================================
struct Binding {
    enum class Source { Block, Link, VizSource, Constant };
    Source  source   = Source::Constant;
    QString object;
    QString property;
    double  constant = 0.0;
};

struct Threshold {
    double  value   = 1.0;
    bool    isAbove = false;
    QString color;
};

struct Component {
    QString id, type, label, blockRef;
    double  x = 0, y = 0, w = 100, h = 100;
    bool    layoutOverride = false;
    double  fillMax = 1.0;
    Binding fillBinding;
    Binding intensityBinding;
    QVector<Threshold> thresholds;
    QJsonArray drawCommands;
    // runtime
    double  fillValue    = 0.0;
    double  fillFraction = 0.0;
    QString waterColor   = "#3B82F6";
};

struct Connector {
    QString linkName;
    QString fromBlock, toBlock;   // read from viz_state
    Binding flowBinding;
    double  flowValue = 0.0;
    QJsonArray drawCommands;
};

// ============================================================
// State accessors
// ============================================================
static double stateVal(const QJsonObject &state,
                       const QString &category,
                       const QString &name,
                       const QString &prop)
{
    return state[category].toObject()[name].toObject()
    ["variables"].toObject()["variables"].toObject()
        [prop].toObject()["_val"].toDouble();
}

static double readBinding(const QJsonObject &state, const Binding &b)
{
    switch (b.source) {
    case Binding::Source::Block:
        return stateVal(state, "blocks",  b.object, b.property);
    case Binding::Source::Link:
        return stateVal(state, "links",   b.object, b.property);
    case Binding::Source::VizSource:
        return stateVal(state, "sources", b.object, b.property);
    case Binding::Source::Constant:
        return b.constant;
    }
    return 0.0;
}

static Binding parseBinding(const QJsonValue &v)
{
    Binding b;
    if (v.isDouble()) {
        b.source   = Binding::Source::Constant;
        b.constant = v.toDouble();
        return b;
    }
    const QJsonObject o = v.toObject();
    if (o.contains("block")) {
        b.source   = Binding::Source::Block;
        b.object   = o["block"].toString();
        b.property = o["property"].toString();
    } else if (o.contains("link")) {
        b.source   = Binding::Source::Link;
        b.object   = o["link"].toString();
        b.property = o["property"].toString();
    } else if (o.contains("source")) {
        b.source   = Binding::Source::VizSource;
        b.object   = o["source"].toString();
        b.property = o["property"].toString();
    }
    return b;
}

// ============================================================
// Color resolution
// ============================================================
static QString resolveColor(const Component &c)
{
    if (c.thresholds.isEmpty()) return "#3B82F6";
    for (const auto &t : c.thresholds) {
        if (!t.isAbove && c.fillFraction <  t.value) return t.color;
        if ( t.isAbove && c.fillFraction >= t.value) return t.color;
    }
    return c.thresholds.last().color;
}

// ============================================================
// Dynamic value resolution
// ============================================================

// Resolves a numeric attribute that may be literal or {"bind":...}/{"expr":...}
static double resolveNumber(const QJsonValue &v, const Component &c)
{
    if (v.isDouble()) return v.toDouble();
    if (!v.isObject()) return 0.0;
    const QJsonObject o = v.toObject();

    if (o.contains("bind")) {
        const QString name = o["bind"].toString();
        if (name == "fill_fraction") return c.fillFraction;
        if (name == "fill_value")    return c.fillValue;
        if (name == "intensity")     return c.fillFraction; // mapped same
        return 0.0;
    }

    if (o.contains("expr")) {
        // Minimal expression evaluator: supports +, -, *, /
        // and the tokens: fill_fraction, fill_value, intensity
        QString expr = o["expr"].toString();
        expr.replace("fill_fraction", QString::number(c.fillFraction));
        expr.replace("fill_value",    QString::number(c.fillValue));
        expr.replace("intensity",     QString::number(c.fillFraction));

        // Tokenise and evaluate left-to-right (no precedence needed for simple exprs)
        // Split on operators while keeping them
        // Simple recursive descent for +/- at top level, */ below
        // For now: handle "A op B" single-operation expressions
        for (const QChar op : {'+', '-', '*', '/'}) {
            const int idx = expr.indexOf(op);
            if (idx > 0) {
                const double lhs = expr.left(idx).trimmed().toDouble();
                const double rhs = expr.mid(idx+1).trimmed().toDouble();
                if (op == '+') return lhs + rhs;
                if (op == '-') return lhs - rhs;
                if (op == '*') return lhs * rhs;
                if (op == '/') return rhs != 0 ? lhs / rhs : 0.0;
            }
        }
        return expr.trimmed().toDouble();
    }
    return 0.0;
}

// Resolves a color attribute (string literal or {"bind":"water_color"})
static QString resolveColorAttr(const QJsonValue &v, const Component &c)
{
    if (v.isString()) return v.toString();
    if (v.isObject()) {
        const QString name = v.toObject()["bind"].toString();
        if (name == "water_color") return c.waterColor;
    }
    return "#000000";
}

// Resolves a text value ({"bind":"label"} or {"expr":..., "format":...})
static QString resolveText(const QJsonValue &v, const Component &c)
{
    if (v.isString()) return v.toString();
    if (!v.isObject()) return {};
    const QJsonObject o = v.toObject();

    if (o.contains("bind")) {
        const QString name = o["bind"].toString();
        if (name == "label")      return c.label;
        if (name == "fill_value") return QString::number(c.fillValue);
        if (name == "fill_fraction") return QString::number(c.fillFraction);
    }

    if (o.contains("expr")) {
        const double val = resolveNumber(v, c);
        const QString fmt = o.value("format").toString("%.2f");
        // Apply format string
        char buf[64];
        std::snprintf(buf, sizeof(buf), fmt.toStdString().c_str(), val);
        return QString::fromLocal8Bit(buf);
    }
    return {};
}

// ============================================================
// Coordinate helpers: normalize [0..1] → canvas pixels
// ============================================================
static double cx(double nx, const Component &c) { return c.x + nx * c.w; }
static double cy(double ny, const Component &c) { return c.y + ny * c.h; }

// ============================================================
// SVG attribute helpers
// ============================================================
static QString svgFill  (const QString &v) { return QString("fill='%1'").arg(v); }
static QString svgStroke(const QString &v, double w) {
    return QString("stroke='%1' stroke-width='%2'").arg(v).arg(w);
}

// ============================================================
// Draw command executor — emits SVG for one draw entry
// ============================================================
static QString executeDraw(const QJsonObject &cmd, const Component &c,
                           int clipId)
{
    const QString shape = cmd["shape"].toString();

    // -- shared optional clip reference --
    const bool useClip = cmd["clip"].toBool(false);
    const QString clipAttr = useClip
                                 ? QString("clip-path='url(#comp_clip_%1)'").arg(clipId) : "";

    if (shape == "rect") {
        const double x  = cx(resolveNumber(cmd["x"], c), c);
        const double y  = cy(resolveNumber(cmd["y"], c), c);
        const double w  = resolveNumber(cmd["w"], c) * c.w;
        const double h  = resolveNumber(cmd["h"], c) * c.h;
        const double rx = cmd["rx"].toDouble(0);
        const QString fill   = resolveColorAttr(cmd["fill"],   c);
        const QString stroke = resolveColorAttr(cmd.value("stroke"), c);
        const double  sw     = cmd["stroke_width"].toDouble(1.0);

        if (w <= 0 || h <= 0) return {};

        return QString("<rect x='%1' y='%2' width='%3' height='%4' "
                       "rx='%5' %6 %7 %8/>\n")
            .arg(x).arg(y).arg(w).arg(h).arg(rx)
            .arg(svgFill(fill))
            .arg(stroke.isEmpty() ? "stroke='none'" : svgStroke(stroke, sw))
            .arg(clipAttr);
    }

    if (shape == "ellipse") {
        const double ecx = cx(resolveNumber(cmd["cx"], c), c);
        const double ecy = cy(resolveNumber(cmd["cy"], c), c);
        const double rx  = resolveNumber(cmd["rx"], c) * c.w;
        const double ry  = resolveNumber(cmd["ry"], c) * c.h;
        const QString fill   = resolveColorAttr(cmd["fill"],   c);
        const QString stroke = resolveColorAttr(cmd.value("stroke"), c);
        const double  sw     = cmd["stroke_width"].toDouble(1.0);

        return QString("<ellipse cx='%1' cy='%2' rx='%3' ry='%4' "
                       "%5 %6 %7/>\n")
            .arg(ecx).arg(ecy).arg(rx).arg(ry)
            .arg(svgFill(fill))
            .arg(stroke.isEmpty() ? "stroke='none'" : svgStroke(stroke, sw))
            .arg(clipAttr);
    }

    if (shape == "polygon") {
        const QJsonArray pts = cmd["points"].toArray();
        QString pointsStr;
        for (const auto &pv : pts) {
            const QJsonArray pt = pv.toArray();
            if (pt.size() < 2) continue;
            pointsStr += QString("%1,%2 ")
                             .arg(cx(pt[0].toDouble(), c))
                             .arg(cy(pt[1].toDouble(), c));
        }
        const QString fill   = resolveColorAttr(cmd["fill"],   c);
        const QString stroke = resolveColorAttr(cmd.value("stroke"), c);
        const double  sw     = cmd["stroke_width"].toDouble(1.0);

        return QString("<polygon points='%1' %2 %3 %4/>\n")
            .arg(pointsStr.trimmed())
            .arg(svgFill(fill))
            .arg(stroke.isEmpty() ? "stroke='none'" : svgStroke(stroke, sw))
            .arg(clipAttr);
    }

    if (shape == "line") {
        const double x1 = cx(resolveNumber(cmd["x1"], c), c);
        const double y1 = cy(resolveNumber(cmd["y1"], c), c);
        const double x2 = cx(resolveNumber(cmd["x2"], c), c);
        const double y2 = cy(resolveNumber(cmd["y2"], c), c);
        const QString stroke = resolveColorAttr(cmd["stroke"], c);
        const double  sw     = cmd["stroke_width"].toDouble(1.0);
        const QString dash   = cmd["dash"].toString();
        const QString dashAttr = dash.isEmpty() ? ""
                                                : QString("stroke-dasharray='%1'").arg(dash);

        return QString("<line x1='%1' y1='%2' x2='%3' y2='%4' "
                       "%5 %6/>\n")
            .arg(x1).arg(y1).arg(x2).arg(y2)
            .arg(svgStroke(stroke, sw))
            .arg(dashAttr);
    }

    if (shape == "path") {
        // Path d-string: replace normalized coord tokens if needed
        // For now treat as literal SVG path (author uses canvas coords)
        QString d = cmd["d"].toString();
        // Replace normalized point tokens: "nx,ny" → actual pixels
        // Simple approach: author writes literal canvas coords in path
        const QString fill   = resolveColorAttr(cmd["fill"],   c);
        const QString stroke = resolveColorAttr(cmd.value("stroke"), c);
        const double  sw     = cmd["stroke_width"].toDouble(1.0);

        return QString("<path d='%1' %2 %3 %4/>\n")
            .arg(d)
            .arg(svgFill(fill))
            .arg(stroke.isEmpty() ? "stroke='none'" : svgStroke(stroke, sw))
            .arg(clipAttr);
    }

    if (shape == "text") {
        const double  tx      = cx(resolveNumber(cmd["x"], c), c);
        const double  ty      = cy(resolveNumber(cmd["y"], c), c);
        const QString anchor  = cmd["anchor"].toString("middle");
        const QString val     = resolveText(cmd["value"], c);
        const int     fs      = cmd["font_size"].toInt(11);
        const int     fw      = cmd["font_weight"].toInt(400);
        const QString fill    = resolveColorAttr(cmd["fill"], c);

        return QString("<text x='%1' y='%2' text-anchor='%3' "
                       "font-family='sans-serif' font-size='%4' "
                       "font-weight='%5' fill='%6'>%7</text>\n")
            .arg(tx).arg(ty).arg(anchor)
            .arg(fs).arg(fw).arg(fill).arg(val);
    }

    return {};  // unknown shape — skip
}

// ============================================================
// Connector SVG — line from source block bottom to dest block top
// ============================================================
static QString executeConnectorDraw(const QJsonObject &cmd,
                                    double x1, double y1,
                                    double x2, double y2,
                                    double flowValue)
{
    const QString shape = cmd["shape"].toString();

    if (shape == "line") {
        const QString stroke = cmd["stroke"].toString("#3B82F6");
        const double  sw     = cmd["stroke_width"].toDouble(2.0);
        const QString dash   = cmd["dash"].toString();
        const QString dashAttr = dash.isEmpty() ? ""
                                                : QString("stroke-dasharray='%1'").arg(dash);

        return QString("<line x1='%1' y1='%2' x2='%3' y2='%4' "
                       "stroke='%5' stroke-width='%6' %7 "
                       "marker-end='url(#arrowhead)'/>\n")
            .arg(x1).arg(y1).arg(x2).arg(y2)
            .arg(stroke).arg(sw).arg(dashAttr);
    }

    if (shape == "text") {
        const double  tx  = (x1 + x2) / 2 + 6;
        const double  ty  = (y1 + y2) / 2;
        const QString fmt = cmd.value("value").toObject()
                                .value("format").toString("%.1f");
        char buf[64];
        std::snprintf(buf, sizeof(buf), fmt.toStdString().c_str(), flowValue);
        const int    fs   = cmd["font_size"].toInt(9);
        const QString fill = cmd["fill"].toString("#475569");

        return QString("<text x='%1' y='%2' text-anchor='middle' "
                       "font-family='sans-serif' font-size='%3' fill='%4'>"
                       "%5</text>\n")
            .arg(tx).arg(ty).arg(fs).arg(fill)
            .arg(QString::fromLocal8Bit(buf));
    }

    return {};
}

// ============================================================
// VizRenderer::render
// ============================================================
bool VizRenderer::render(const QString     &vizJsonPath,
                         const QJsonObject &state,
                         const QString     &svgOutputPath,
                         QString           &err)
{
    // --- load viz.json ---
    QFile vf(vizJsonPath);
    if (!vf.open(QIODevice::ReadOnly)) {
        err = "Cannot open viz.json: " + vizJsonPath;
        return false;
    }
    QJsonParseError pe;
    const QJsonDocument vdoc = QJsonDocument::fromJson(vf.readAll(), &pe);
    if (vdoc.isNull()) {
        err = "viz.json parse error: " + pe.errorString();
        return false;
    }
    const QJsonObject vroot = vdoc.object();

    const QJsonObject canvas = vroot["canvas"].toObject();
    const double cw = canvas["width"].toDouble(420);
    const double ch = canvas["height"].toDouble(560);

    // --- parse components ---
    QVector<Component> components;
    for (const auto &cv : vroot["components"].toArray()) {
        const QJsonObject co = cv.toObject();
        Component c;
        c.id           = co["id"].toString();
        c.type         = co["type"].toString();
        c.label        = co["label"].toString(c.id);
        c.blockRef     = co["block"].toString();
        c.fillMax      = co["fill_max"].toDouble(1.0);
        c.drawCommands = co["draw"].toArray();

        if (co.contains("x") && co.contains("y")) {
            c.x = co["x"].toDouble();
            c.y = co["y"].toDouble();
            c.w = co["width"].toDouble(100);
            c.h = co["height"].toDouble(100);
            c.layoutOverride = true;
        }

        const QJsonObject bind = co["bind"].toObject();
        if (bind.contains("fill_value"))
            c.fillBinding = parseBinding(bind["fill_value"]);
        if (bind.contains("intensity"))
            c.intensityBinding = parseBinding(bind["intensity"]);

        for (const auto &tv : co["thresholds"].toArray()) {
            const QJsonObject to = tv.toObject();
            Threshold t;
            t.isAbove = to.contains("above");
            t.value   = t.isAbove ? to["above"].toDouble()
                                : to["below"].toDouble(1.0);
            t.color   = to["color"].toString("#3B82F6");
            c.thresholds.append(t);
        }
        components.append(c);
    }

    // --- parse connectors ---
    QVector<Connector> connectors;
    const QJsonObject stateLinks = state["links"].toObject();
    for (const auto &cv : vroot["connectors"].toArray()) {
        const QJsonObject co = cv.toObject();
        Connector cn;
        cn.linkName    = co["link"].toString();
        cn.drawCommands = co["draw"].toArray();

        // read from/to from viz_state
        const QJsonObject linkObj = stateLinks[cn.linkName].toObject();
        cn.fromBlock = linkObj["s_Block_name"].toString();
        cn.toBlock   = linkObj["e_Block_name"].toString();

        const QJsonObject bind = co["bind"].toObject();
        if (bind.contains("flow_value"))
            cn.flowBinding = parseBinding(bind["flow_value"]);
        connectors.append(cn);
    }

    // --- Option A: resolve layout from state ---
    {
        double xMin =  std::numeric_limits<double>::infinity();
        double yMin =  std::numeric_limits<double>::infinity();
        double xMax = -std::numeric_limits<double>::infinity();
        double yMax = -std::numeric_limits<double>::infinity();

        for (auto &c : components) {
            if (c.layoutOverride || c.blockRef.isEmpty()) continue;
            c.x = stateVal(state, "blocks", c.blockRef, "x");
            c.y = stateVal(state, "blocks", c.blockRef, "y");
            c.w = stateVal(state, "blocks", c.blockRef, "_width");
            c.h = stateVal(state, "blocks", c.blockRef, "_height");
            if (c.w <= 0) c.w = 100;
            if (c.h <= 0) c.h = 100;
            xMin = qMin(xMin, c.x);
            yMin = qMin(yMin, c.y);
            xMax = qMax(xMax, c.x + c.w);
            yMax = qMax(yMax, c.y + c.h);
        }

        if (xMax > xMin && yMax > yMin) {
            const double pad    = 30.0;
            const double scaleX = (cw - 2*pad) / (xMax - xMin);
            const double scaleY = (ch - 2*pad) / (yMax - yMin);
            const double scale  = qMin(scaleX, scaleY);
            for (auto &c : components) {
                if (c.layoutOverride) continue;
                c.x = pad + (c.x - xMin) * scale;
                c.y = pad + (c.y - yMin) * scale;
                c.w = c.w * scale;
                c.h = c.h * scale;
            }
        }
    }

    // --- resolve bindings ---
    for (auto &c : components) {
        c.fillValue    = readBinding(state, c.fillBinding);
        c.fillFraction = (c.fillMax > 0)
                             ? qBound(0.0, c.fillValue / c.fillMax, 1.0)
                             : 0.0;
        c.waterColor   = resolveColor(c);
    }
    for (auto &cn : connectors)
        cn.flowValue = readBinding(state, cn.flowBinding);

    // --- generate SVG ---
    QString svg;
    svg += QString("<?xml version='1.0' encoding='UTF-8'?>\n"
                   "<svg xmlns='http://www.w3.org/2000/svg' "
                   "width='%1' height='%2' viewBox='0 0 %1 %2'>\n")
               .arg(cw).arg(ch);

    // defs: arrowhead + per-component clip paths
    svg += "<defs>\n";
    svg += "  <marker id='arrowhead' markerWidth='8' markerHeight='6' "
           "refX='8' refY='3' orient='auto'>\n"
           "    <polygon points='0 0, 8 3, 0 6' fill='#3B82F6'/>\n"
           "  </marker>\n";
    for (int i = 0; i < components.size(); ++i) {
        const auto &c = components[i];
        svg += QString("  <clipPath id='comp_clip_%1'>"
                       "<rect x='%2' y='%3' width='%4' height='%5' rx='8'/>"
                       "</clipPath>\n")
                   .arg(i).arg(c.x).arg(c.y).arg(c.w).arg(c.h);
    }
    svg += "</defs>\n";

    // background
    svg += QString("<rect width='%1' height='%2' fill='#F8FAFC'/>\n")
               .arg(cw).arg(ch);

    // connectors first (drawn behind components)
    for (const auto &cn : connectors) {
        // find from/to component positions by matching blockRef
        double x1 = 0, y1 = 0, x2 = 0, y2 = 0;
        for (const auto &c : components) {
            if (c.blockRef == cn.fromBlock)
            { x1 = c.x + c.w/2; y1 = c.y + c.h; }
            if (c.blockRef == cn.toBlock)
            { x2 = c.x + c.w/2; y2 = c.y; }
        }
        for (const auto &cmd : cn.drawCommands)
            svg += executeConnectorDraw(cmd.toObject(),
                                        x1, y1, x2, y2, cn.flowValue);
    }

    // components
    for (int i = 0; i < components.size(); ++i) {
        const auto &c = components[i];
        for (const auto &cmd : c.drawCommands)
            svg += executeDraw(cmd.toObject(), c, i);
    }

    svg += "</svg>\n";

    // --- write output ---
    QFile out(svgOutputPath);
    if (!out.open(QIODevice::WriteOnly | QIODevice::Text)) {
        err = "Cannot write SVG: " + svgOutputPath;
        return false;
    }
    QTextStream ts(&out);
    ts << svg;
    return true;
}
