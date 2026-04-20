#pragma once
#include <QJsonObject>
#include <QString>

class VizRenderer
{
public:
    static bool render(const QString     &vizJsonPath,
                       const QJsonObject &state,
                       const QString     &svgOutputPath,
                       QString           &err);
};
