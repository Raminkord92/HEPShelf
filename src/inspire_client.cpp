#include "inspire_client.h"

#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QRegularExpression>
#include <QUrl>
#include <QUrlQuery>

namespace {
QString collapseWhitespace(QString value)
{
    value.replace(QRegularExpression(QStringLiteral("\\s+")), QStringLiteral(" "));
    return value.trimmed();
}

QString firstValue(const QJsonArray &array, const QString &key)
{
    for (const QJsonValue &value : array) {
        const QString text = collapseWhitespace(value.toObject().value(key).toString());
        if (!text.isEmpty())
            return text;
    }
    return {};
}

int recidFromRecordObject(const QJsonObject &record)
{
    QString ref = record.value(QStringLiteral("$ref")).toString();
    if (ref.isEmpty())
        ref = record.value(QStringLiteral("ref")).toString();
    static const QRegularExpression rx(QStringLiteral("/literature/(\\d+)(?:$|[/?#])"));
    const auto match = rx.match(ref);
    if (match.hasMatch())
        return match.captured(1).toInt();
    return 0;
}

QString referenceAuthors(const QJsonObject &reference)
{
    QStringList names;
    for (const QJsonValue &value : reference.value(QStringLiteral("authors")).toArray()) {
        const QString name = collapseWhitespace(value.toObject().value(QStringLiteral("full_name")).toString());
        if (!name.isEmpty())
            names << name;
    }
    return names.join(QStringLiteral(", "));
}

QString referenceDoi(const QJsonObject &reference)
{
    const QJsonValue value = reference.value(QStringLiteral("dois"));
    if (value.isArray()) {
        const QJsonArray dois = value.toArray();
        if (!dois.isEmpty()) {
            const QJsonValue first = dois.first();
            if (first.isString())
                return first.toString().trimmed();
            return first.toObject().value(QStringLiteral("value")).toString().trimmed();
        }
    } else if (value.isString()) {
        return value.toString().trimmed();
    }
    return {};
}

QString rawReferenceText(const QJsonObject &item)
{
    for (const QJsonValue &value : item.value(QStringLiteral("raw_refs")).toArray()) {
        const QString raw = collapseWhitespace(value.toObject().value(QStringLiteral("value")).toString());
        if (!raw.isEmpty())
            return raw;
    }
    return {};
}

QString referenceMisc(const QJsonObject &reference)
{
    QStringList parts;
    for (const QJsonValue &value : reference.value(QStringLiteral("misc")).toArray()) {
        const QString text = collapseWhitespace(value.toString());
        if (!text.isEmpty())
            parts << text;
    }
    return parts.join(QStringLiteral(" "));
}

QString searchTitle(const QJsonObject &metadata)
{
    const QJsonArray titles = metadata.value(QStringLiteral("titles")).toArray();
    return firstValue(titles, QStringLiteral("title"));
}

QString searchAuthors(const QJsonObject &metadata)
{
    QStringList names;
    for (const QJsonValue &value : metadata.value(QStringLiteral("authors")).toArray()) {
        const QString name = collapseWhitespace(value.toObject().value(QStringLiteral("full_name")).toString());
        if (!name.isEmpty())
            names << name;
    }
    return names.join(QStringLiteral(", "));
}

QString firstObjectValue(const QJsonValue &value, const QString &key)
{
    if (value.isArray()) {
        const QJsonArray values = value.toArray();
        if (!values.isEmpty()) {
            const QJsonValue first = values.first();
            if (first.isString())
                return first.toString().trimmed();
            return first.toObject().value(key).toString().trimmed();
        }
    } else if (value.isString()) {
        return value.toString().trimmed();
    }
    return {};
}
}

InspireClient::InspireClient(QObject *parent)
    : QObject(parent)
{
}

void InspireClient::fetchPaper(const QString &arxivId, PaperCallback callback)
{
    const QString encoded = QString::fromLatin1(QUrl::toPercentEncoding(arxivId));
    QUrl url(QStringLiteral("https://inspirehep.net/api/arxiv/") + encoded);

    QNetworkRequest request(url);
    request.setHeader(QNetworkRequest::UserAgentHeader,
                      QStringLiteral("HEPShelf/0.9.2 local-literature-library"));
    request.setRawHeader("Accept", "application/json");
    request.setAttribute(QNetworkRequest::RedirectPolicyAttribute,
                         QNetworkRequest::NoLessSafeRedirectPolicy);
    request.setTransferTimeout(30000);

    QNetworkReply *reply = network_.get(request);
    connect(reply, &QNetworkReply::finished, this, [this, reply, callback = std::move(callback)]() mutable {
        InspirePaperData data;
        QString error;
        const int status = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
        if (reply->error() != QNetworkReply::NoError) {
            if (status == 404)
                error = QStringLiteral("This arXiv paper does not have an INSPIRE literature record.");
            else if (status == 429)
                error = QStringLiteral("INSPIRE rate limit reached. Wait a few seconds and try again.");
            else
                error = QStringLiteral("INSPIRE request failed: %1 (HTTP %2)")
                            .arg(reply->errorString())
                            .arg(status > 0 ? QString::number(status) : QStringLiteral("n/a"));
        } else {
            parsePaper(reply->readAll(), &data, &error);
        }
        reply->deleteLater();
        if (callback)
            callback(data, error);
    });
}

void InspireClient::fetchPaperByRecid(int inspireRecid, PaperCallback callback)
{
    if (inspireRecid <= 0) {
        if (callback)
            callback({}, QStringLiteral("A valid INSPIRE record ID is required."));
        return;
    }

    QUrl url(QStringLiteral("https://inspirehep.net/api/literature/%1").arg(inspireRecid));
    QNetworkRequest request(url);
    request.setHeader(QNetworkRequest::UserAgentHeader,
                      QStringLiteral("HEPShelf/0.9.2 local-literature-library"));
    request.setRawHeader("Accept", "application/json");
    request.setAttribute(QNetworkRequest::RedirectPolicyAttribute,
                         QNetworkRequest::NoLessSafeRedirectPolicy);
    request.setTransferTimeout(30000);

    QNetworkReply *reply = network_.get(request);
    connect(reply, &QNetworkReply::finished, this, [this, reply, callback = std::move(callback)]() mutable {
        InspirePaperData data;
        QString error;
        const int status = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
        if (reply->error() != QNetworkReply::NoError) {
            if (status == 404)
                error = QStringLiteral("This INSPIRE literature record no longer exists.");
            else if (status == 429)
                error = QStringLiteral("INSPIRE rate limit reached. Wait a few seconds and try again.");
            else
                error = QStringLiteral("INSPIRE request failed: %1 (HTTP %2)")
                            .arg(reply->errorString())
                            .arg(status > 0 ? QString::number(status) : QStringLiteral("n/a"));
        } else {
            parsePaper(reply->readAll(), &data, &error);
        }
        reply->deleteLater();
        if (callback)
            callback(data, error);
    });
}

void InspireClient::fetchBibTeX(const QString &arxivId, TextCallback callback)
{
    const QString encoded = QString::fromLatin1(QUrl::toPercentEncoding(arxivId));
    QUrl url(QStringLiteral("https://inspirehep.net/api/arxiv/") + encoded);
    QUrlQuery query;
    query.addQueryItem(QStringLiteral("format"), QStringLiteral("bibtex"));
    url.setQuery(query);

    QNetworkRequest request(url);
    request.setHeader(QNetworkRequest::UserAgentHeader,
                      QStringLiteral("HEPShelf/0.9.2 local-literature-library"));
    request.setRawHeader("Accept", "application/x-bibtex");
    request.setAttribute(QNetworkRequest::RedirectPolicyAttribute,
                         QNetworkRequest::NoLessSafeRedirectPolicy);
    request.setTransferTimeout(30000);

    QNetworkReply *reply = network_.get(request);
    connect(reply, &QNetworkReply::finished, this, [reply, callback = std::move(callback)]() mutable {
        QString text;
        QString error;
        const int status = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
        if (reply->error() != QNetworkReply::NoError) {
            if (status == 404)
                error = QStringLiteral("No INSPIRE BibTeX record was found for this paper.");
            else
                error = QStringLiteral("Could not obtain BibTeX from INSPIRE: %1 (HTTP %2)")
                            .arg(reply->errorString())
                            .arg(status > 0 ? QString::number(status) : QStringLiteral("n/a"));
        } else {
            text = QString::fromUtf8(reply->readAll()).trimmed();
            if (text.isEmpty())
                error = QStringLiteral("INSPIRE returned an empty BibTeX response.");
        }
        reply->deleteLater();
        if (callback)
            callback(text, error);
    });
}

void InspireClient::fetchCitingPapers(int inspireRecid, int limit, CitingCallback callback)
{
    if (inspireRecid <= 0) {
        if (callback)
            callback({}, QStringLiteral("An INSPIRE record ID is required before cited-by papers can be queried."));
        return;
    }

    const int safeLimit = qBound(1, limit, 1000);
    QUrl url(QStringLiteral("https://inspirehep.net/api/literature"));
    QUrlQuery query;
    query.addQueryItem(QStringLiteral("q"), QStringLiteral("refersto:recid:%1").arg(inspireRecid));
    query.addQueryItem(QStringLiteral("sort"), QStringLiteral("mostrecent"));
    query.addQueryItem(QStringLiteral("size"), QString::number(safeLimit));
    query.addQueryItem(QStringLiteral("fields"),
                       QStringLiteral("titles,authors.full_name,arxiv_eprints,dois,control_number,citation_count,earliest_date"));
    url.setQuery(query);

    QNetworkRequest request(url);
    request.setHeader(QNetworkRequest::UserAgentHeader,
                      QStringLiteral("HEPShelf/0.9.2 local-literature-library"));
    request.setRawHeader("Accept", "application/json");
    request.setAttribute(QNetworkRequest::RedirectPolicyAttribute,
                         QNetworkRequest::NoLessSafeRedirectPolicy);
    request.setTransferTimeout(30000);

    QNetworkReply *reply = network_.get(request);
    connect(reply, &QNetworkReply::finished, this, [this, reply, callback = std::move(callback)]() mutable {
        InspireCitingData data;
        QString error;
        const int status = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
        if (reply->error() != QNetworkReply::NoError) {
            if (status == 429)
                error = QStringLiteral("INSPIRE rate limit reached. Wait a few seconds and try again.");
            else
                error = QStringLiteral("INSPIRE cited-by request failed: %1 (HTTP %2)")
                            .arg(reply->errorString())
                            .arg(status > 0 ? QString::number(status) : QStringLiteral("n/a"));
        } else {
            parseCitingSearch(reply->readAll(), &data, &error);
        }
        reply->deleteLater();
        if (callback)
            callback(data, error);
    });
}

void InspireClient::fetchTitlesByRecids(const QList<int> &recids, TitlesCallback callback)
{
    QStringList terms;
    for (int recid : recids) {
        if (recid > 0)
            terms << QStringLiteral("recid:%1").arg(recid);
    }
    if (terms.isEmpty()) {
        if (callback)
            callback({}, {});
        return;
    }

    QUrl url(QStringLiteral("https://inspirehep.net/api/literature"));
    QUrlQuery query;
    query.addQueryItem(QStringLiteral("q"), terms.join(QStringLiteral(" OR ")));
    query.addQueryItem(QStringLiteral("size"), QString::number(terms.size()));
    query.addQueryItem(QStringLiteral("fields"), QStringLiteral("titles,control_number"));
    url.setQuery(query);

    QNetworkRequest request(url);
    request.setHeader(QNetworkRequest::UserAgentHeader,
                      QStringLiteral("HEPShelf/0.9.2 local-literature-library"));
    request.setRawHeader("Accept", "application/json");
    request.setAttribute(QNetworkRequest::RedirectPolicyAttribute,
                         QNetworkRequest::NoLessSafeRedirectPolicy);
    request.setTransferTimeout(30000);

    QNetworkReply *reply = network_.get(request);
    connect(reply, &QNetworkReply::finished, this, [reply, callback = std::move(callback)]() mutable {
        QHash<int, QString> titles;
        QString error;
        if (reply->error() != QNetworkReply::NoError) {
            error = QStringLiteral("INSPIRE title lookup failed: %1").arg(reply->errorString());
        } else {
            QJsonParseError parseError;
            const QJsonDocument doc = QJsonDocument::fromJson(reply->readAll(), &parseError);
            if (parseError.error != QJsonParseError::NoError || !doc.isObject()) {
                error = QStringLiteral("INSPIRE returned invalid title data.");
            } else {
                const QJsonArray hits = doc.object().value(QStringLiteral("hits"))
                                            .toObject().value(QStringLiteral("hits")).toArray();
                for (const QJsonValue &value : hits) {
                    const QJsonObject hit = value.toObject();
                    const QJsonObject metadata = hit.value(QStringLiteral("metadata")).toObject();
                    int recid = metadata.value(QStringLiteral("control_number")).toInt();
                    if (recid <= 0)
                        recid = hit.value(QStringLiteral("id")).toVariant().toInt();
                    QString title;
                    const QJsonArray candidates = metadata.value(QStringLiteral("titles")).toArray();
                    for (const QJsonValue &candidate : candidates) {
                        const QJsonObject item = candidate.toObject();
                        const QString text = collapseWhitespace(item.value(QStringLiteral("title")).toString());
                        if (text.isEmpty())
                            continue;
                        if (title.isEmpty())
                            title = text;
                        if (item.value(QStringLiteral("source")).toString().compare(QStringLiteral("arXiv"), Qt::CaseInsensitive) == 0) {
                            title = text;
                            break;
                        }
                    }
                    if (recid > 0 && !title.isEmpty())
                        titles.insert(recid, title);
                }
            }
        }
        reply->deleteLater();
        if (callback)
            callback(titles, error);
    });
}

bool InspireClient::parsePaper(const QByteArray &json, InspirePaperData *data, QString *error) const
{
    if (!data)
        return false;

    QJsonParseError parseError;
    const QJsonDocument doc = QJsonDocument::fromJson(json, &parseError);
    if (parseError.error != QJsonParseError::NoError || !doc.isObject()) {
        if (error)
            *error = QStringLiteral("INSPIRE returned invalid JSON: %1").arg(parseError.errorString());
        return false;
    }

    const QJsonObject root = doc.object();
    const QJsonObject metadata = root.value(QStringLiteral("metadata")).toObject();
    if (metadata.isEmpty()) {
        if (error)
            *error = QStringLiteral("INSPIRE record contains no metadata.");
        return false;
    }

    InspirePaperData result;
    result.metrics.inspireRecid = metadata.value(QStringLiteral("control_number")).toInt();
    result.metrics.citationCount = metadata.contains(QStringLiteral("citation_count"))
                                       ? metadata.value(QStringLiteral("citation_count")).toInt()
                                       : -1;
    result.metrics.citationCountWithoutSelf = metadata.contains(QStringLiteral("citation_count_without_self_citations"))
                                                  ? metadata.value(QStringLiteral("citation_count_without_self_citations")).toInt()
                                                  : -1;

    const QJsonArray references = metadata.value(QStringLiteral("references")).toArray();
    result.metrics.referenceCount = references.size();

    int position = 1;
    for (const QJsonValue &value : references) {
        const QJsonObject item = value.toObject();
        const QJsonObject reference = item.value(QStringLiteral("reference")).toObject();

        ReferenceRecord ref;
        ref.position = position++;
        ref.inspireRecid = recidFromRecordObject(item.value(QStringLiteral("record")).toObject());
        if (ref.inspireRecid == 0)
            ref.inspireRecid = recidFromRecordObject(reference.value(QStringLiteral("record")).toObject());

        ref.arxivId = reference.value(QStringLiteral("arxiv_eprint")).toString().trimmed();
        ref.title = collapseWhitespace(reference.value(QStringLiteral("title")).toObject().value(QStringLiteral("title")).toString());
        if (ref.title.isEmpty()) {
            const QJsonValue titleValue = reference.value(QStringLiteral("title"));
            if (titleValue.isString())
                ref.title = collapseWhitespace(titleValue.toString());
        }
        ref.authors = referenceAuthors(reference);
        ref.doi = referenceDoi(reference);
        ref.rawText = rawReferenceText(item);

        // Some INSPIRE records keep recognizable fields next to, rather than inside,
        // the compact reference object. Preserve anything useful we can identify.
        if (ref.arxivId.isEmpty())
            ref.arxivId = item.value(QStringLiteral("arxiv_eprint")).toString().trimmed();
        if (ref.rawText.isEmpty())
            ref.rawText = referenceMisc(reference);

        result.references << ref;
    }

    *data = result;
    return true;
}

bool InspireClient::parseCitingSearch(const QByteArray &json, InspireCitingData *data, QString *error) const
{
    if (!data)
        return false;

    QJsonParseError parseError;
    const QJsonDocument doc = QJsonDocument::fromJson(json, &parseError);
    if (parseError.error != QJsonParseError::NoError || !doc.isObject()) {
        if (error)
            *error = QStringLiteral("INSPIRE returned invalid cited-by JSON: %1").arg(parseError.errorString());
        return false;
    }

    const QJsonObject root = doc.object();
    const QJsonObject hits = root.value(QStringLiteral("hits")).toObject();
    if (hits.isEmpty() && !root.contains(QStringLiteral("hits"))) {
        if (error)
            *error = QStringLiteral("INSPIRE cited-by response contains no search results.");
        return false;
    }

    InspireCitingData result;
    const QJsonValue totalValue = hits.value(QStringLiteral("total"));
    if (totalValue.isDouble())
        result.totalCount = totalValue.toInt();
    else if (totalValue.isObject())
        result.totalCount = totalValue.toObject().value(QStringLiteral("value")).toInt(-1);

    const QJsonArray hitArray = hits.value(QStringLiteral("hits")).toArray();
    for (const QJsonValue &value : hitArray) {
        const QJsonObject hit = value.toObject();
        const QJsonObject metadata = hit.value(QStringLiteral("metadata")).toObject();
        if (metadata.isEmpty())
            continue;

        RelatedPaperRecord paper;
        paper.inspireRecid = metadata.value(QStringLiteral("control_number")).toInt();
        if (paper.inspireRecid <= 0) {
            const QJsonValue hitId = hit.value(QStringLiteral("id"));
            paper.inspireRecid = hitId.isDouble() ? hitId.toInt() : hitId.toString().toInt();
        }
        paper.title = searchTitle(metadata);
        paper.authors = searchAuthors(metadata);
        for (const QJsonValue &author : metadata.value(QStringLiteral("authors")).toArray()) {
            const QString name = author.toObject().value(QStringLiteral("full_name")).toString().simplified();
            if (!name.isEmpty())
                paper.authorNames << name;
        }
        paper.arxivId = firstObjectValue(metadata.value(QStringLiteral("arxiv_eprints")), QStringLiteral("value"));
        paper.doi = firstObjectValue(metadata.value(QStringLiteral("dois")), QStringLiteral("value"));
        paper.year = metadata.value(QStringLiteral("earliest_date")).toString().left(4);
        paper.citationCount = metadata.contains(QStringLiteral("citation_count"))
                                  ? metadata.value(QStringLiteral("citation_count")).toInt()
                                  : -1;
        result.papers << paper;
    }

    if (result.totalCount < 0)
        result.totalCount = result.papers.size();
    *data = result;
    return true;
}
