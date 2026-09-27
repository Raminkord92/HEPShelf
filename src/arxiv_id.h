#pragma once

#include <QString>

namespace ArxivId {

QString fromPdfPath(const QString &path);
QString fromPdfContent(const QString &path);
QString stripVersion(QString id);
bool contentDetectionAvailable();

} // namespace ArxivId
