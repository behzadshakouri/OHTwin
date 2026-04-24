#ifndef VIZRENDERER_H
#define VIZRENDERER_H

#include <QJsonObject>
#include <QString>

// ============================================================
// VizRenderer
//
// Renders an SVG visualization from a viz-spec JSON file and a
// state snapshot. All rendering logic is self-contained; this
// class exposes a single static entry point.
//
// Usage:
//     QString err;
//     if (!VizRenderer::render("viz.json", stateObj, "out.svg", err)) {
//         qWarning() << "Render failed:" << err;
//     }
// ============================================================
class VizRenderer
{
public:
    // Reads a viz spec from `vizJsonPath`, binds it against the
    // provided `state` object, and writes an SVG to `svgOutputPath`.
    //
    // Returns true on success. On failure, returns false and sets
    // `err` to a human-readable description.
    static bool render(const QString     &vizJsonPath,
                       const QJsonObject &state,
                       const QString     &svgOutputPath,
                       QString           &err);
};

#endif // VIZRENDERER_H
