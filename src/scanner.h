#pragma once

#include <QList>
#include <QString>
#include <QStringList>

struct ScannedFile {
    QString path;
    QString arxivId;
    qint64 size = 0;
    qint64 mtime = 0;
    bool detectedFromContent = false;
};

struct ScanResult {
    QList<ScannedFile> files;
    QStringList scannedFolders;
    QStringList unavailableFolders;
    int pdfCount = 0;
    int recognizedCount = 0;
    int contentRecognizedCount = 0;
    bool contentDetectionAvailable = false;
};

namespace Scanner {
ScanResult scanFolders(const QStringList &folders);
}
