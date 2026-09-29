#include "arxiv_client.h"
#include "arxiv_id.h"

#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QRegularExpression>
#include <QTimer>
#include <QUrl>
#include <QUrlQuery>
#include <QXmlStreamReader>

namespace {
QString collapseWhitespace(QString value)
{
    value.replace(QRegularExpression(QStringLiteral("\\s+")), QStringLiteral(" "));
    return value.trimmed();
}

QString idFromEntryUrl(const QString &urlText)
{
    const QUrl url(urlText);
    QString id = url.path();
    if (id.startsWith(QStringLiteral("/abs/")))
        id.remove(0, 5);
    while (id.startsWith('/'))
        id.remove(0, 1);
    return ArxivId::stripVersion(id);
}

QString firstString(const QJsonArray &array, const QString &key)
{
    for (const QJsonValue &value : array) {
        const QString result = value.toObject().value(key).toString().trimmed();
        if (!result.isEmpty())
            return result;
    }
    return {};
}

QString inspireJournalReference(const QJsonObject &metadata)
{
    const QJsonArray publications = metadata.value(QStringLiteral("publication_info")).toArray();
    if (publications.isEmpty())
        return {};

    const QJsonObject publication = publications.first().toObject();
    QStringList parts;
    const QString title = publication.value(QStringLiteral("journal_title")).toString().trimmed();
    const QString volume = publication.value(QStringLiteral("journal_volume")).toString().trimmed();
    const QString year = publication.value(QStringLiteral("year")).toVariant().toString().trimmed();
    QString article = publication.value(QStringLiteral("artid")).toString().trimmed();
    if (article.isEmpty())
        article = publication.value(QStringLiteral("page_start")).toString().trimmed();

    if (!title.isEmpty())
        parts << title;
    if (!volume.isEmpty())
        parts << volume;
    if (!year.isEmpty())
        parts << QStringLiteral("(%1)").arg(year);
    if (!article.isEmpty())
        parts << article;
    return parts.join(QLatin1Char(' '));
}
}

ArxivClient::ArxivClient(QObject *parent)
    : QObject(parent)
{
}

void ArxivClient::fetch(const QStringList &ids,
                        BatchCallback onBatch,
                        ProgressCallback onProgress,
                        FinishedCallback onFinished)
{
    if (busy_)
        return;

    pendingIds_ = ids;
    pendingIds_.removeDuplicates();
    totalIds_ = pendingIds_.size();
    completedIds_ = 0;
    onBatch_ = std::move(onBatch);
    onProgress_ = std::move(onProgress);
    onFinished_ = std::move(onFinished);

    if (pendingIds_.isEmpty()) {
        if (onFinished_)
            onFinished_();
        return;
    }

    busy_ = true;
    requestNextBatch();
}

void ArxivClient::requestNextBatch()
{
    if (pendingIds_.isEmpty()) {
        busy_ = false;
        if (onFinished_)
            onFinished_();
        return;
    }

    constexpr int batchSize = 20;
    currentBatch_.clear();
    currentPapers_.clear();
    currentResolved_.clear();
    currentErrors_.clear();
    inspirePending_.clear();

    while (!pendingIds_.isEmpty() && currentBatch_.size() < batchSize)
        currentBatch_ << pendingIds_.takeFirst();

    QUrl url(QStringLiteral("https://export.arxiv.org/api/query"));
    QUrlQuery query;
    query.addQueryItem(QStringLiteral("id_list"), currentBatch_.join(','));
    query.addQueryItem(QStringLiteral("max_results"), QString::number(currentBatch_.size()));
    url.setQuery(query);

    QNetworkRequest request(url);
    request.setHeader(QNetworkRequest::UserAgentHeader,
                      QStringLiteral("HEPShelf/0.9.2 local-literature-library"));
    request.setAttribute(QNetworkRequest::RedirectPolicyAttribute,
                         QNetworkRequest::NoLessSafeRedirectPolicy);
    request.setTransferTimeout(20000);

    QNetworkReply *reply = network_.get(request);
    connect(reply, &QNetworkReply::finished, this, [this, reply]() {
        QString parseError;
        QList<PaperRecord> papers;
        const int status = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();

        if (reply->error() != QNetworkReply::NoError) {
            currentErrors_ << QStringLiteral("arXiv API: %1 (HTTP %2)")
                                  .arg(reply->errorString())
                                  .arg(status > 0 ? QString::number(status) : QStringLiteral("n/a"));
        } else {
            papers = parseFeed(reply->readAll(), &parseError);
            if (!parseError.isEmpty())
                currentErrors_ << QStringLiteral("arXiv XML: %1").arg(parseError);
        }
        reply->deleteLater();

        for (const PaperRecord &paper : papers) {
            if (paper.arxivId.isEmpty())
                continue;
            currentPapers_ << paper;
            currentResolved_.insert(paper.arxivId);
        }

        for (const QString &id : currentBatch_) {
            if (!currentResolved_.contains(id))
                inspirePending_ << id;
        }

        if (inspirePending_.isEmpty()) {
            finishCurrentBatch();
            return;
        }

        requestNextInspireFallback();
    });
}

void ArxivClient::requestNextInspireFallback()
{
    if (inspirePending_.isEmpty()) {
        finishCurrentBatch();
        return;
    }

    const QString id = inspirePending_.takeFirst();
    const QByteArray encoded = QUrl::toPercentEncoding(id);
    const QUrl url(QStringLiteral("https://inspirehep.net/api/arxiv/") + QString::fromLatin1(encoded));

    QNetworkRequest request(url);
    request.setHeader(QNetworkRequest::UserAgentHeader,
                      QStringLiteral("HEPShelf/0.9.2 local-literature-library"));
    request.setRawHeader("Accept", "application/json");
    request.setAttribute(QNetworkRequest::RedirectPolicyAttribute,
                         QNetworkRequest::NoLessSafeRedirectPolicy);
    request.setTransferTimeout(20000);

    QNetworkReply *reply = network_.get(request);
    connect(reply, &QNetworkReply::finished, this, [this, reply, id]() {
        const int status = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
        QString error;
        PaperRecord paper;

        if (reply->error() == QNetworkReply::NoError) {
            if (parseInspireRecord(reply->readAll(), id, &paper, &error)) {
                currentPapers_ << paper;
                currentResolved_.insert(id);
            }
        } else if (status != 404) {
            error = QStringLiteral("%1 (HTTP %2)")
                        .arg(reply->errorString())
                        .arg(status > 0 ? QString::number(status) : QStringLiteral("n/a"));
        }
        reply->deleteLater();

        if (!error.isEmpty())
            currentErrors_ << QStringLiteral("INSPIRE %1: %2").arg(id, error);

        QTimer::singleShot(400, this, [this]() { requestNextInspireFallback(); });
    });
}

void ArxivClient::finishCurrentBatch()
{
    QStringList unresolved;
    for (const QString &id : currentBatch_) {
        if (!currentResolved_.contains(id))
            unresolved << id;
    }

    QString error;
    if (!unresolved.isEmpty()) {
        error = QStringLiteral("Could not resolve metadata for: %1").arg(unresolved.join(QStringLiteral(", ")));
        if (!currentErrors_.isEmpty())
            error += QStringLiteral(". %1").arg(currentErrors_.join(QStringLiteral(" | ")));
    }

    completedIds_ += currentBatch_.size();
    if (onBatch_)
        onBatch_(currentPapers_, error);
    if (onProgress_)
        onProgress_(completedIds_, totalIds_);

    if (pendingIds_.isEmpty()) {
        busy_ = false;
        if (onFinished_)
            onFinished_();
        return;
    }

    QTimer::singleShot(3000, this, [this]() { requestNextBatch(); });
}

QList<PaperRecord> ArxivClient::parseFeed(const QByteArray &xml, QString *error) const
{
    QList<PaperRecord> papers;
    QXmlStreamReader reader(xml);

    while (!reader.atEnd()) {
        reader.readNext();
        if (!reader.isStartElement() || reader.name() != QLatin1String("entry"))
            continue;

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
                paper.title = collapseWhitespace(reader.readElementText());
            } else if (name == QLatin1String("summary")) {
                paper.abstractText = collapseWhitespace(reader.readElementText());
            } else if (name == QLatin1String("published")) {
                paper.published = reader.readElementText().trimmed();
            } else if (name == QLatin1String("updated")) {
                paper.updated = reader.readElementText().trimmed();
            } else if (name == QLatin1String("author")) {
                QString authorName;
                while (!(reader.isEndElement() && reader.name() == QLatin1String("author")) && !reader.atEnd()) {
                    reader.readNext();
                    if (reader.isStartElement() && reader.name() == QLatin1String("name"))
                        authorName = collapseWhitespace(reader.readElementText());
                }
                if (!authorName.isEmpty())
                    authors << authorName;
            } else if (name == QLatin1String("primary_category")) {
                paper.primaryCategory = reader.attributes().value(QStringLiteral("term")).toString();
                reader.skipCurrentElement();
            } else if (name == QLatin1String("category")) {
                const QString term = reader.attributes().value(QStringLiteral("term")).toString().trimmed();
                if (!term.isEmpty() && !categories.contains(term))
                    categories << term;
                reader.skipCurrentElement();
            } else if (name == QLatin1String("doi")) {
                paper.doi = collapseWhitespace(reader.readElementText());
            } else if (name == QLatin1String("journal_ref")) {
                paper.journalRef = collapseWhitespace(reader.readElementText());
            } else if (name == QLatin1String("comment")) {
                paper.comments = collapseWhitespace(reader.readElementText());
            }
        }

        paper.authorNames = authors;
        paper.authors = authors.join(QStringLiteral(", "));
        paper.categories = categories.join(QStringLiteral(", "));
        if (paper.primaryCategory.isEmpty() && !categories.isEmpty())
            paper.primaryCategory = categories.first();

        if (!paper.arxivId.isEmpty() && !paper.title.isEmpty())
            papers.push_back(paper);
    }

    if (reader.hasError()) {
        if (error)
            *error = reader.errorString();
        return {};
    }
    return papers;
}

bool ArxivClient::parseInspireRecord(const QByteArray &json,
                                     const QString &requestedId,
                                     PaperRecord *paper,
                                     QString *error) const
{
    QJsonParseError jsonError;
    const QJsonDocument doc = QJsonDocument::fromJson(json, &jsonError);
    if (jsonError.error != QJsonParseError::NoError || !doc.isObject()) {
        if (error)
            *error = QStringLiteral("invalid JSON: %1").arg(jsonError.errorString());
        return false;
    }

    const QJsonObject root = doc.object();
    const QJsonObject metadata = root.value(QStringLiteral("metadata")).toObject();
    if (metadata.isEmpty()) {
        if (error)
            *error = QStringLiteral("record has no metadata object");
        return false;
    }

    PaperRecord result;
    result.arxivId = requestedId;
    result.title = collapseWhitespace(firstString(metadata.value(QStringLiteral("titles")).toArray(),
                                                   QStringLiteral("title")));

    QStringList authors;
    for (const QJsonValue &value : metadata.value(QStringLiteral("authors")).toArray()) {
        const QString name = collapseWhitespace(value.toObject().value(QStringLiteral("full_name")).toString());
        if (!name.isEmpty())
            authors << name;
    }
    result.authorNames = authors;
    result.authors = authors.join(QStringLiteral(", "));

    result.abstractText = collapseWhitespace(
        firstString(metadata.value(QStringLiteral("abstracts")).toArray(), QStringLiteral("value")));

    result.published = metadata.value(QStringLiteral("preprint_date")).toString().trimmed();
    if (result.published.isEmpty())
        result.published = metadata.value(QStringLiteral("earliest_date")).toString().trimmed();

    result.updated = root.value(QStringLiteral("updated")).toString().trimmed();

    const QJsonArray eprints = metadata.value(QStringLiteral("arxiv_eprints")).toArray();
    if (!eprints.isEmpty()) {
        const QJsonArray categoryArray = eprints.at(0).toObject().value(QStringLiteral("categories")).toArray();
        QStringList categories;
        for (const QJsonValue &value : categoryArray) {
            const QString category = value.toString().trimmed();
            if (!category.isEmpty())
                categories << category;
        }
        result.categories = categories.join(QStringLiteral(", "));
        if (!categories.isEmpty())
            result.primaryCategory = categories.first();
    }

    result.doi = firstString(metadata.value(QStringLiteral("dois")).toArray(), QStringLiteral("value"));
    result.journalRef = inspireJournalReference(metadata);

    if (result.title.isEmpty()) {
        if (error)
            *error = QStringLiteral("record has no title");
        return false;
    }

    if (paper)
        *paper = result;
    return true;
}
