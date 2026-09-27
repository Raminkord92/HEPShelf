#include "arxiv_feed.h"
#include "arxiv_id.h"

#include <QRegularExpression>
#include <QUrl>
#include <QXmlStreamReader>

namespace {
QString collapsed(QString value)
{
    value.replace(QRegularExpression(QStringLiteral("\\s+")), QStringLiteral(" "));
    return value.trimmed();
}

QString idFromEntryUrl(const QString &urlText)
{
    QString id = QUrl(urlText).path();
    if (id.startsWith(QStringLiteral("/abs/")))
        id.remove(0, 5);
    while (id.startsWith(QLatin1Char('/')))
        id.remove(0, 1);
    return ArxivId::stripVersion(id);
}
}

ArxivFeedResult parseArxivFeed(const QByteArray &xml)
{
    ArxivFeedResult result;
    QXmlStreamReader reader(xml);
    while (!reader.atEnd()) {
        reader.readNext();
        if (!reader.isStartElement())
            continue;
        if (reader.name() == QLatin1String("totalResults")) {
            result.totalResults = reader.readElementText().toInt();
            continue;
        }
        if (reader.name() != QLatin1String("entry"))
            continue;

        ++result.entryCount;
        PaperRecord paper;
        QStringList authors;
        QStringList categories;
        while (!(reader.isEndElement() && reader.name() == QLatin1String("entry")) && !reader.atEnd()) {
            reader.readNext();
            if (!reader.isStartElement())
                continue;
            const QString name = reader.name().toString();
            if (name == QLatin1String("id")) {
                paper.arxivId = idFromEntryUrl(reader.readElementText());
            } else if (name == QLatin1String("title")) {
                paper.title = collapsed(reader.readElementText());
            } else if (name == QLatin1String("summary")) {
                paper.abstractText = collapsed(reader.readElementText());
            } else if (name == QLatin1String("published")) {
                paper.published = reader.readElementText().trimmed();
            } else if (name == QLatin1String("updated")) {
                paper.updated = reader.readElementText().trimmed();
            } else if (name == QLatin1String("author")) {
                QString authorName;
                while (!(reader.isEndElement() && reader.name() == QLatin1String("author")) && !reader.atEnd()) {
                    reader.readNext();
                    if (reader.isStartElement() && reader.name() == QLatin1String("name"))
                        authorName = collapsed(reader.readElementText());
                }
                if (!authorName.isEmpty())
                    authors << authorName;
            } else if (name == QLatin1String("primary_category")) {
                paper.primaryCategory = reader.attributes().value(QStringLiteral("term")).toString().trimmed();
                reader.skipCurrentElement();
            } else if (name == QLatin1String("category")) {
                const QString term = reader.attributes().value(QStringLiteral("term")).toString().trimmed();
                if (!term.isEmpty() && !categories.contains(term))
                    categories << term;
                reader.skipCurrentElement();
            } else if (name == QLatin1String("doi")) {
                paper.doi = collapsed(reader.readElementText());
            } else if (name == QLatin1String("journal_ref")) {
                paper.journalRef = collapsed(reader.readElementText());
            } else if (name == QLatin1String("comment")) {
                paper.comments = collapsed(reader.readElementText());
            }
        }
        paper.authorNames = authors;
        paper.authors = authors.join(QStringLiteral(", "));
        paper.categories = categories.join(QStringLiteral(", "));
        if (paper.primaryCategory.isEmpty() && !categories.isEmpty())
            paper.primaryCategory = categories.first();
        if (paper.title.compare(QStringLiteral("Error"), Qt::CaseInsensitive) == 0
            || paper.arxivId.startsWith(QStringLiteral("api/errors"), Qt::CaseInsensitive)) {
            result.error = paper.abstractText.isEmpty() ? QStringLiteral("arXiv returned an error for this query.")
                                                        : paper.abstractText;
            result.papers.clear();
            return result;
        }
        if (!paper.arxivId.isEmpty() && !paper.title.isEmpty())
            result.papers << paper;
    }
    if (reader.hasError()) {
        result.error = reader.errorString();
        result.papers.clear();
    }
    return result;
}
