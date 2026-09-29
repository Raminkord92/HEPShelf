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

ArxivOaiResult parseArxivOai(const QByteArray &xml)
{
    ArxivOaiResult result;
    QXmlStreamReader reader(xml);
    bool sawResponse = false;
    while (!reader.atEnd()) {
        reader.readNext();
        if (!reader.isStartElement())
            continue;
        const QString name = reader.name().toString();
        if (name == QLatin1String("OAI-PMH")) {
            sawResponse = true;
        } else if (name == QLatin1String("error")) {
            const QString code = reader.attributes().value(QStringLiteral("code")).toString();
            const QString message = collapsed(reader.readElementText());
            if (code != QLatin1String("noRecordsMatch"))
                result.error = message.isEmpty() ? QStringLiteral("arXiv metadata error: %1").arg(code) : message;
        } else if (name == QLatin1String("resumptionToken")) {
            result.nextToken = reader.readElementText().trimmed();
        } else if (name == QLatin1String("record")) {
            PaperRecord paper;
            while (!(reader.isEndElement() && reader.name() == QLatin1String("record")) && !reader.atEnd()) {
                reader.readNext();
                if (!reader.isStartElement() || reader.name() != QLatin1String("arXiv"))
                    continue;
                while (!(reader.isEndElement() && reader.name() == QLatin1String("arXiv")) && !reader.atEnd()) {
                    reader.readNext();
                    if (!reader.isStartElement())
                        continue;
                    const QString field = reader.name().toString();
                    if (field == QLatin1String("id")) {
                        paper.arxivId = ArxivId::stripVersion(reader.readElementText().trimmed());
                    } else if (field == QLatin1String("title")) {
                        paper.title = collapsed(reader.readElementText());
                    } else if (field == QLatin1String("abstract")) {
                        paper.abstractText = collapsed(reader.readElementText());
                    } else if (field == QLatin1String("created")) {
                        paper.published = reader.readElementText().trimmed();
                    } else if (field == QLatin1String("updated")) {
                        paper.updated = reader.readElementText().trimmed();
                    } else if (field == QLatin1String("categories")) {
                        paper.categories = collapsed(reader.readElementText());
                        paper.primaryCategory = paper.categories.section(QLatin1Char(' '), 0, 0);
                    } else if (field == QLatin1String("doi")) {
                        paper.doi = collapsed(reader.readElementText());
                    } else if (field == QLatin1String("journal-ref")) {
                        paper.journalRef = collapsed(reader.readElementText());
                    } else if (field == QLatin1String("comments")) {
                        paper.comments = collapsed(reader.readElementText());
                    } else if (field == QLatin1String("author")) {
                        QString given;
                        QString family;
                        while (!(reader.isEndElement() && reader.name() == QLatin1String("author")) && !reader.atEnd()) {
                            reader.readNext();
                            if (!reader.isStartElement())
                                continue;
                            if (reader.name() == QLatin1String("forenames"))
                                given = collapsed(reader.readElementText());
                            else if (reader.name() == QLatin1String("keyname"))
                                family = collapsed(reader.readElementText());
                        }
                        const QString fullName = (given + QLatin1Char(' ') + family).trimmed();
                        if (!fullName.isEmpty())
                            paper.authorNames << fullName;
                    }
                }
            }
            paper.authors = paper.authorNames.join(QStringLiteral(", "));
            if (!paper.arxivId.isEmpty() && !paper.title.isEmpty())
                result.papers << paper;
        }
    }
    if (reader.hasError())
        result.error = reader.errorString();
    else if (!sawResponse)
        result.error = QStringLiteral("arXiv returned an unexpected metadata response.");
    if (!result.error.isEmpty()) {
        result.papers.clear();
        result.nextToken.clear();
    }
    return result;
}
