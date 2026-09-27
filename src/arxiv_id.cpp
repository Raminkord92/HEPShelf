#include "arxiv_id.h"

#include <QFileInfo>
#include <QProcess>
#include <QRegularExpression>
#include <QStandardPaths>

namespace {
QString normalizeSeparators(QString path)
{
    path.replace('\\', '/');
    return path;
}

QString oldArchivePattern()
{
    return QStringLiteral(
        "astro-ph|cond-mat|gr-qc|hep-ex|hep-lat|hep-ph|hep-th|math-ph|"
        "nlin|nucl-ex|nucl-th|physics|quant-ph|alg-geom|chao-dyn|chem-ph|"
        "dg-ga|funct-an|q-alg|solv-int|adap-org|cmp-lg|patt-sol|supr-con");
}

QString detectInText(const QString &text)
{
    static const QRegularExpression explicitModern(
        QStringLiteral("\\barXiv\\s*:\\s*(\\d{4}\\.\\d{4,5})(?:v\\d+)?\\b"),
        QRegularExpression::CaseInsensitiveOption);
    if (const auto match = explicitModern.match(text); match.hasMatch())
        return match.captured(1);

    const QString archives = oldArchivePattern();
    const QRegularExpression explicitOld(
        QStringLiteral("\\barXiv\\s*:\\s*((?:%1)/\\d{7})(?:v\\d+)?\\b").arg(archives),
        QRegularExpression::CaseInsensitiveOption);
    if (const auto match = explicitOld.match(text); match.hasMatch())
        return match.captured(1).toLower();

    // A few older papers print only the archive identifier without the "arXiv:" prefix.
    const QRegularExpression oldStandalone(
        QStringLiteral("\\b((?:%1)/\\d{7})(?:v\\d+)?\\b").arg(archives),
        QRegularExpression::CaseInsensitiveOption);
    if (const auto match = oldStandalone.match(text); match.hasMatch())
        return match.captured(1).toLower();

    return {};
}
}

namespace ArxivId {

QString stripVersion(QString id)
{
    static const QRegularExpression versionRe(QStringLiteral("v\\d+$"),
                                              QRegularExpression::CaseInsensitiveOption);
    id.remove(versionRe);
    return id;
}

QString fromPdfPath(const QString &path)
{
    const QFileInfo info(path);
    const QString base = info.completeBaseName();

    static const QRegularExpression modernRe(
        QStringLiteral("(?:^|[^0-9])(?:arxiv[ _-]*)?(\\d{4}\\.\\d{4,5})(?:v\\d+)?(?:$|[^0-9])"),
        QRegularExpression::CaseInsensitiveOption);

    if (const auto match = modernRe.match(base); match.hasMatch())
        return match.captured(1);

    const QString archives = oldArchivePattern();
    const QString normalizedPath = normalizeSeparators(info.absoluteFilePath());

    const QRegularExpression oldPathRe(
        QStringLiteral("/(%1)/(\\d{7})(?:v\\d+)?\\.pdf$").arg(archives),
        QRegularExpression::CaseInsensitiveOption);
    if (const auto match = oldPathRe.match(normalizedPath); match.hasMatch())
        return match.captured(1).toLower() + QLatin1Char('/') + match.captured(2);

    const QRegularExpression oldFlatRe(
        QStringLiteral("^(%1)[_-](\\d{7})(?:v\\d+)?$").arg(archives),
        QRegularExpression::CaseInsensitiveOption);
    if (const auto match = oldFlatRe.match(base); match.hasMatch())
        return match.captured(1).toLower() + QLatin1Char('/') + match.captured(2);

    return {};
}

bool contentDetectionAvailable()
{
    return !QStandardPaths::findExecutable(QStringLiteral("pdftotext")).isEmpty();
}

QString fromPdfContent(const QString &path)
{
    const QString executable = QStandardPaths::findExecutable(QStringLiteral("pdftotext"));
    if (executable.isEmpty())
        return {};

    QProcess process;
    process.setProcessChannelMode(QProcess::SeparateChannels);
    process.start(executable,
                  {QStringLiteral("-f"), QStringLiteral("1"),
                   QStringLiteral("-l"), QStringLiteral("2"),
                   QStringLiteral("-layout"), path, QStringLiteral("-")});

    if (!process.waitForStarted(1200))
        return {};
    if (!process.waitForFinished(4500)) {
        process.kill();
        process.waitForFinished(500);
        return {};
    }

    QByteArray data = process.readAllStandardOutput();
    if (data.size() > 2 * 1024 * 1024)
        data.truncate(2 * 1024 * 1024);

    return detectInText(QString::fromUtf8(data));
}

} // namespace ArxivId
