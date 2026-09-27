#include "scanner.h"
#include "arxiv_id.h"

#include <QDir>
#include <QDirIterator>
#include <QFileInfo>

namespace Scanner {

ScanResult scanFolders(const QStringList &folders)
{
    ScanResult result;
    result.contentDetectionAvailable = ArxivId::contentDetectionAvailable();

    QStringList rootsToScan;
    for (const QString &folder : folders) {
        const QFileInfo folderInfo(folder);
        if (!folderInfo.exists() || !folderInfo.isDir() || !folderInfo.isReadable()) {
            result.unavailableFolders << folder;
            continue;
        }

        const QString root = QDir(folder).absolutePath();
        result.scannedFolders << root;

        bool coveredByExistingRoot = false;
        for (const QString &existing : rootsToScan) {
            if (root == existing || root.startsWith(existing + QLatin1Char('/'))) {
                coveredByExistingRoot = true;
                break;
            }
        }
        if (!coveredByExistingRoot) {
            for (qsizetype i = rootsToScan.size() - 1; i >= 0; --i) {
                if (rootsToScan.at(i).startsWith(root + QLatin1Char('/')))
                    rootsToScan.removeAt(i);
            }
            rootsToScan << root;
        }
    }

    for (const QString &root : rootsToScan) {
        QDirIterator it(root,
                        QStringList() << QStringLiteral("*.pdf") << QStringLiteral("*.PDF"),
                        QDir::Files | QDir::Readable,
                        QDirIterator::Subdirectories);

        while (it.hasNext()) {
            const QString path = it.next();
            ++result.pdfCount;

            QString arxivId = ArxivId::fromPdfPath(path);
            bool fromContent = false;
            if (arxivId.isEmpty() && result.contentDetectionAvailable) {
                arxivId = ArxivId::fromPdfContent(path);
                fromContent = !arxivId.isEmpty();
            }

            if (arxivId.isEmpty())
                continue;

            const QFileInfo info(path);
            ScannedFile file;
            file.path = info.absoluteFilePath();
            file.arxivId = arxivId;
            file.size = info.size();
            file.mtime = info.lastModified().toSecsSinceEpoch();
            file.detectedFromContent = fromContent;
            result.files.push_back(file);
            ++result.recognizedCount;
            if (fromContent)
                ++result.contentRecognizedCount;
        }
    }

    return result;
}

} // namespace Scanner
