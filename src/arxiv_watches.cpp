#include "arxiv_watches.h"
#include "arxiv_feed.h"
#include "arxiv_id.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QRegularExpression>
#include <QSaveFile>
#include <QSettings>
#include <QUuid>
#include <QUrlQuery>

#include <algorithm>

namespace {
constexpr int PageSize = 100;
constexpr qint64 ApiIntervalMs = 3100;

QString utcString(const QDateTime &date)
{
    return date.toUTC().toString(Qt::ISODate);
}

QDateTime readUtc(const QJsonValue &value)
{
    return QDateTime::fromString(value.toString(), Qt::ISODate).toUTC();
}

QJsonObject paperJson(const PaperRecord &paper)
{
    QJsonArray authorNames;
    for (const QString &name : paper.authorNames)
        authorNames.append(name);
    return {{QStringLiteral("id"), paper.arxivId},
            {QStringLiteral("title"), paper.title},
            {QStringLiteral("authors"), paper.authors},
            {QStringLiteral("authorNames"), authorNames},
            {QStringLiteral("abstract"), paper.abstractText},
            {QStringLiteral("published"), paper.published},
            {QStringLiteral("updated"), paper.updated},
            {QStringLiteral("category"), paper.primaryCategory},
            {QStringLiteral("categories"), paper.categories},
            {QStringLiteral("doi"), paper.doi},
            {QStringLiteral("journal"), paper.journalRef},
            {QStringLiteral("comments"), paper.comments}};
}

PaperRecord readPaper(const QJsonObject &object)
{
    PaperRecord paper;
    paper.arxivId = object.value(QStringLiteral("id")).toString();
    paper.title = object.value(QStringLiteral("title")).toString();
    paper.authors = object.value(QStringLiteral("authors")).toString();
    for (const QJsonValue &name : object.value(QStringLiteral("authorNames")).toArray())
        paper.authorNames << name.toString();
    if (paper.authorNames.isEmpty())
        paper.authorNames = paper.authors.split(QStringLiteral(", "), Qt::SkipEmptyParts);
    paper.abstractText = object.value(QStringLiteral("abstract")).toString();
    paper.published = object.value(QStringLiteral("published")).toString();
    paper.updated = object.value(QStringLiteral("updated")).toString();
    paper.primaryCategory = object.value(QStringLiteral("category")).toString();
    paper.categories = object.value(QStringLiteral("categories")).toString();
    paper.doi = object.value(QStringLiteral("doi")).toString();
    paper.journalRef = object.value(QStringLiteral("journal")).toString();
    paper.comments = object.value(QStringLiteral("comments")).toString();
    return paper;
}

QString escapedAuthor(QString author)
{
    author.replace(QLatin1Char('\\'), QStringLiteral("\\\\"));
    author.replace(QLatin1Char('"'), QStringLiteral("\\\""));
    return author;
}

QString filenameId(QString id)
{
    id.replace(QLatin1Char('/'), QLatin1Char('_'));
    return id;
}

QString citationIdentity(const RelatedPaperRecord &paper)
{
    if (paper.inspireRecid > 0)
        return QStringLiteral("recid:%1").arg(paper.inspireRecid);
    if (!paper.arxivId.isEmpty())
        return QStringLiteral("arxiv:%1").arg(paper.arxivId);
    if (!paper.doi.isEmpty())
        return QStringLiteral("doi:%1").arg(paper.doi.toLower());
    return {};
}

bool validCitationTerm(const QString &term)
{
    static const QRegularExpression id(QStringLiteral("^(?:[A-Za-z.-]+/[0-9]{7}|[0-9]{4}\\.[0-9]{4,5})(?:v[0-9]+)?$"));
    return id.match(term).hasMatch();
}

bool localFileExists(Database *db, const QString &arxivId)
{
    QString path;
    if (!db || !db->hasLocalFile(arxivId, &path))
        return false;
    if (QFileInfo::exists(path))
        return true;
    PaperDetails details;
    if (!db->paperDetails(arxivId, &details))
        return false;
    for (const FileRecord &file : details.files) {
        if (QFileInfo::exists(file.path))
            return true;
    }
    return false;
}

PaperRecord preserveKnownMetadata(Database *db, PaperRecord paper)
{
    PaperDetails details;
    if (!db || !db->paperDetails(paper.arxivId, &details))
        return paper;
    const PaperRecord &known = details.paper;
    auto fill = [](QString &field, const QString &value) {
        if (field.isEmpty())
            field = value;
    };
    fill(paper.title, known.title);
    fill(paper.authors, known.authors);
    fill(paper.abstractText, known.abstractText);
    fill(paper.published, known.published);
    fill(paper.updated, known.updated);
    fill(paper.primaryCategory, known.primaryCategory);
    fill(paper.categories, known.categories);
    fill(paper.doi, known.doi);
    fill(paper.journalRef, known.journalRef);
    fill(paper.comments, known.comments);
    if (paper.authorNames.isEmpty())
        paper.authorNames = known.authorNames;
    return paper;
}
}

ArxivWatchManager::ArxivWatchManager(Database *database, QObject *parent)
    : QObject(parent), db_(database)
{
    load();
    for (const ArxivWatchHit &hit : hits_) {
        if (hit.status != QStringLiteral("New"))
            continue;
        for (const ArxivWatchRule &rule : rules_) {
            if (rule.id == hit.ruleId && rule.enabled && rule.autoDownload) {
                enqueueDownload(hit.paper, rule.folder);
                break;
            }
        }
    }
    dueTimer_.setInterval(60 * 60 * 1000);
    connect(&dueTimer_, &QTimer::timeout, this, &ArxivWatchManager::checkDue);
    dueTimer_.start();
    QTimer::singleShot(5000, this, &ArxivWatchManager::checkDue);
    QTimer::singleShot(6000, this, &ArxivWatchManager::nextDownload);
}

ArxivWatchManager::~ArxivWatchManager()
{
    if (searchReply_) {
        searchReply_->disconnect(this);
        searchReply_->abort();
    }
    if (downloadReply_)
        downloadReply_->disconnect(this);
    if (downloadReply_)
        downloadReply_->abort();
    if (saveFile_) {
        saveFile_->cancelWriting();
        delete saveFile_;
    }
}

void ArxivWatchManager::load()
{
    QSettings settings;
    const QJsonArray ruleArray = QJsonDocument::fromJson(settings.value(QStringLiteral("watches/rules")).toByteArray()).array();
    for (const QJsonValue &value : ruleArray) {
        const QJsonObject object = value.toObject();
        ArxivWatchRule rule;
        rule.id = object.value(QStringLiteral("id")).toString();
        rule.kind = object.value(QStringLiteral("kind")).toString();
        rule.term = object.value(QStringLiteral("term")).toString();
        rule.paperTitle = object.value(QStringLiteral("paperTitle")).toString();
        rule.inspireRecid = object.value(QStringLiteral("inspireRecid")).toInt();
        rule.citationBaselineReady = object.value(QStringLiteral("citationBaselineReady")).toBool();
        for (const QJsonValue &value : object.value(QStringLiteral("seenCitationIds")).toArray())
            rule.seenCitationIds << value.toString();
        rule.enabled = object.value(QStringLiteral("enabled")).toBool(true);
        rule.autoDownload = object.value(QStringLiteral("autoDownload")).toBool();
        rule.folder = object.value(QStringLiteral("folder")).toString();
        rule.lastCheckedUtc = readUtc(object.value(QStringLiteral("lastCheckedUtc")));
        rule.lastError = object.value(QStringLiteral("lastError")).toString();
        if (!rule.id.isEmpty() && (rule.kind == QStringLiteral("author") || rule.kind == QStringLiteral("category")
                                   || rule.kind == QStringLiteral("citation")))
            rules_ << rule;
    }
    const QJsonArray hitArray = QJsonDocument::fromJson(settings.value(QStringLiteral("watches/hits")).toByteArray()).array();
    for (const QJsonValue &value : hitArray) {
        const QJsonObject object = value.toObject();
        ArxivWatchHit hit;
        hit.ruleId = object.value(QStringLiteral("ruleId")).toString();
        hit.paper = readPaper(object.value(QStringLiteral("paper")).toObject());
        hit.foundUtc = readUtc(object.value(QStringLiteral("foundUtc")));
        hit.status = object.value(QStringLiteral("status")).toString();
        hit.inspireRecid = object.value(QStringLiteral("inspireRecid")).toInt();
        if (!hit.ruleId.isEmpty() && (!hit.paper.arxivId.isEmpty() || hit.inspireRecid > 0))
            hits_ << hit;
    }
}

void ArxivWatchManager::save() const
{
    QSettings settings;
    QJsonArray ruleArray;
    for (const ArxivWatchRule &rule : rules_) {
        QJsonArray seen;
        for (const QString &identity : rule.seenCitationIds)
            seen.append(identity);
        ruleArray.append(QJsonObject{{QStringLiteral("id"), rule.id},
                                     {QStringLiteral("kind"), rule.kind},
                                     {QStringLiteral("term"), rule.term},
                                     {QStringLiteral("paperTitle"), rule.paperTitle},
                                     {QStringLiteral("inspireRecid"), rule.inspireRecid},
                                     {QStringLiteral("citationBaselineReady"), rule.citationBaselineReady},
                                     {QStringLiteral("seenCitationIds"), seen},
                                     {QStringLiteral("enabled"), rule.enabled},
                                     {QStringLiteral("autoDownload"), rule.autoDownload},
                                     {QStringLiteral("folder"), rule.folder},
                                     {QStringLiteral("lastCheckedUtc"), utcString(rule.lastCheckedUtc)},
                                     {QStringLiteral("lastError"), rule.lastError}});
    }
    QJsonArray hitArray;
    for (const ArxivWatchHit &hit : hits_) {
        hitArray.append(QJsonObject{{QStringLiteral("ruleId"), hit.ruleId},
                                    {QStringLiteral("paper"), paperJson(hit.paper)},
                                    {QStringLiteral("foundUtc"), utcString(hit.foundUtc)},
                                    {QStringLiteral("status"), hit.status},
                                    {QStringLiteral("inspireRecid"), hit.inspireRecid}});
    }
    settings.setValue(QStringLiteral("watches/rules"), QJsonDocument(ruleArray).toJson(QJsonDocument::Compact));
    settings.setValue(QStringLiteral("watches/hits"), QJsonDocument(hitArray).toJson(QJsonDocument::Compact));
}

bool ArxivWatchManager::addRule(ArxivWatchRule rule, QString *error)
{
    rule.term = rule.term.trimmed();
    if (rule.kind == QStringLiteral("citation"))
        rule.term = ArxivId::stripVersion(rule.term);
    if (rule.term.isEmpty() || (rule.kind != QStringLiteral("author") && rule.kind != QStringLiteral("category")
                                && rule.kind != QStringLiteral("citation"))) {
        if (error) *error = QStringLiteral("Choose an author, category, or paper.");
        return false;
    }
    if (rule.kind == QStringLiteral("citation") && !validCitationTerm(rule.term)) {
        if (error) *error = QStringLiteral("Enter the watched paper's arXiv ID, such as 2609.12345 or hep-ph/0603175.");
        return false;
    }
    static const QRegularExpression categoryCode(QStringLiteral("^[A-Za-z0-9.\\-]+$"));
    if (rule.kind == QStringLiteral("category") && !categoryCode.match(rule.term).hasMatch()) {
        if (error) *error = QStringLiteral("Enter an arXiv category code such as hep-ph or astro-ph.CO.");
        return false;
    }
    if (rule.autoDownload && rule.folder.trimmed().isEmpty()) {
        if (error) *error = QStringLiteral("Choose a download folder for automatic downloads.");
        return false;
    }
    for (const ArxivWatchRule &existing : rules_) {
        if (existing.kind == rule.kind && existing.term.compare(rule.term, Qt::CaseInsensitive) == 0) {
            if (error) *error = QStringLiteral("This watch already exists.");
            return false;
        }
    }
    rule.id = QUuid::createUuid().toString(QUuid::WithoutBraces);
    rule.lastCheckedUtc = QDateTime::currentDateTimeUtc().addDays(-1);
    rules_ << rule;
    save();
    emit changed();
    QTimer::singleShot(0, this, &ArxivWatchManager::checkDue);
    return true;
}

bool ArxivWatchManager::updateRule(const ArxivWatchRule &rule, QString *error)
{
    for (ArxivWatchRule &existing : rules_) {
        if (existing.id != rule.id)
            continue;
        ArxivWatchRule updated = rule;
        updated.term = updated.term.trimmed();
        if (updated.kind == QStringLiteral("citation"))
            updated.term = ArxivId::stripVersion(updated.term);
        if (updated.term.isEmpty() || (updated.kind != QStringLiteral("author") && updated.kind != QStringLiteral("category")
                                       && updated.kind != QStringLiteral("citation"))) {
            if (error) *error = QStringLiteral("Choose an author, category, or paper.");
            return false;
        }
        if (updated.kind == QStringLiteral("citation") && !validCitationTerm(updated.term)) {
            if (error) *error = QStringLiteral("Enter the watched paper's arXiv ID.");
            return false;
        }
        static const QRegularExpression categoryCode(QStringLiteral("^[A-Za-z0-9.\\-]+$"));
        if (updated.kind == QStringLiteral("category") && !categoryCode.match(updated.term).hasMatch()) {
            if (error) *error = QStringLiteral("Enter an arXiv category code such as hep-ph or astro-ph.CO.");
            return false;
        }
        if (updated.autoDownload && updated.folder.trimmed().isEmpty()) {
            if (error) *error = QStringLiteral("Choose a download folder for automatic downloads.");
            return false;
        }
        for (const ArxivWatchRule &other : rules_) {
            if (other.id != updated.id && other.kind == updated.kind
                && other.term.compare(updated.term, Qt::CaseInsensitive) == 0) {
                if (error) *error = QStringLiteral("This watch already exists.");
                return false;
            }
        }
        if (updated.kind != existing.kind || updated.term.compare(existing.term, Qt::CaseInsensitive) != 0) {
            updated.lastCheckedUtc = QDateTime::currentDateTimeUtc().addDays(-1);
            updated.lastError.clear();
            updated.inspireRecid = 0;
            updated.paperTitle.clear();
            updated.citationBaselineReady = false;
            updated.seenCitationIds.clear();
            for (qsizetype i = hits_.size() - 1; i >= 0; --i) {
                if (hits_.at(i).ruleId == updated.id)
                    hits_.removeAt(i);
            }
        }
        existing = updated;
        if (updated.enabled && updated.autoDownload) {
            for (const ArxivWatchHit &hit : hits_) {
                if (hit.ruleId == updated.id && hit.status == QStringLiteral("New"))
                    enqueueDownload(hit.paper, updated.folder);
            }
        }
        save();
        emit changed();
        QTimer::singleShot(0, this, &ArxivWatchManager::checkDue);
        if (!checking_)
            nextDownload();
        return true;
    }
    if (error) *error = QStringLiteral("The selected watch no longer exists.");
    return false;
}

void ArxivWatchManager::removeRule(const QString &id)
{
    for (qsizetype i = rules_.size() - 1; i >= 0; --i) {
        if (rules_.at(i).id == id)
            rules_.removeAt(i);
    }
    for (qsizetype i = hits_.size() - 1; i >= 0; --i) {
        if (hits_.at(i).ruleId == id)
            hits_.removeAt(i);
    }
    ruleQueue_.removeAll(id);
    save();
    emit changed();
}

void ArxivWatchManager::markHitsSeen()
{
    bool updated = false;
    for (ArxivWatchHit &hit : hits_) {
        if (hit.status == QStringLiteral("New")) {
            hit.status = QStringLiteral("Seen");
            updated = true;
        }
    }
    if (updated) {
        save();
        emit changed();
    }
}

void ArxivWatchManager::checkDue()
{
    if (busy())
        return;
    const QDateTime now = QDateTime::currentDateTimeUtc();
    QStringList due;
    for (const ArxivWatchRule &rule : rules_) {
        if (rule.enabled && (!rule.lastCheckedUtc.isValid() || rule.lastCheckedUtc.secsTo(now) >= 24 * 60 * 60))
            due << rule.id;
    }
    beginChecks(due);
}

void ArxivWatchManager::checkNow()
{
    if (busy()) {
        emit statusChanged(QStringLiteral("A watch check or download is already running."));
        return;
    }
    QStringList ids;
    for (const ArxivWatchRule &rule : rules_) {
        if (rule.enabled)
            ids << rule.id;
    }
    if (ids.isEmpty()) {
        emit statusChanged(QStringLiteral("Add or enable a watch first."));
        return;
    }
    beginChecks(ids);
}

void ArxivWatchManager::beginChecks(const QStringList &ruleIds)
{
    if (ruleIds.isEmpty())
        return;
    ruleQueue_ = ruleIds;
    newHitCount_ = 0;
    newCitationCount_ = 0;
    failedRuleCount_ = 0;
    checking_ = true;
    emit statusChanged(QStringLiteral("Checking %1 watch(es)…").arg(ruleIds.size()));
    nextRule();
}

void ArxivWatchManager::nextRule()
{
    while (!ruleQueue_.isEmpty()) {
        const QString id = ruleQueue_.takeFirst();
        auto it = std::find_if(rules_.cbegin(), rules_.cend(), [&id](const ArxivWatchRule &rule) { return rule.id == id && rule.enabled; });
        if (it == rules_.cend())
            continue;
        activeRule_ = *it;
        activeEndUtc_ = QDateTime::currentDateTimeUtc();
        pageStart_ = 0;
        if (activeRule_.kind == QStringLiteral("citation"))
            fetchCitations();
        else
            fetchPage();
        return;
    }
    checking_ = false;
    save();
    emit changed();
    emit statusChanged(QStringLiteral("Watches checked: %1 new paper(s), %2 failed watch(es).")
                           .arg(newHitCount_).arg(failedRuleCount_));
    if (newCitationCount_ > 0)
        emit newCitationsFound(newCitationCount_);
    nextDownload();
}

void ArxivWatchManager::failActiveRule(const QString &error)
{
    ++failedRuleCount_;
    for (ArxivWatchRule &rule : rules_) {
        if (rule.id == activeRule_.id) {
            rule.lastError = error;
            break;
        }
    }
    save();
    emit changed();
    emit statusChanged(QStringLiteral("Watch %1 failed: %2").arg(activeRule_.term, error));
    nextRule();
}

void ArxivWatchManager::fetchCitations()
{
    if (activeRule_.inspireRecid <= 0) {
        inspire_.fetchPaper(activeRule_.term, [this](const InspirePaperData &paper, const QString &error) {
            if (!checking_)
                return;
            if (!error.isEmpty() || paper.metrics.inspireRecid <= 0) {
                failActiveRule(error.isEmpty() ? QStringLiteral("The paper has no INSPIRE record.") : error);
                return;
            }
            activeRule_.inspireRecid = paper.metrics.inspireRecid;
            for (ArxivWatchRule &rule : rules_) {
                if (rule.id == activeRule_.id) {
                    rule.inspireRecid = activeRule_.inspireRecid;
                    break;
                }
            }
            save();
            inspire_.fetchCitingPapers(activeRule_.inspireRecid, 1000,
                                      [this](const InspireCitingData &data, const QString &searchError) {
                                          if (!checking_)
                                              return;
                                          if (!searchError.isEmpty())
                                              failActiveRule(searchError);
                                          else
                                              processCitations(data);
                                      });
        });
        return;
    }
    inspire_.fetchCitingPapers(activeRule_.inspireRecid, 1000,
                              [this](const InspireCitingData &data, const QString &error) {
                                  if (!checking_)
                                      return;
                                  if (!error.isEmpty())
                                      failActiveRule(error);
                                  else
                                      processCitations(data);
                              });
}

void ArxivWatchManager::processCitations(const InspireCitingData &data)
{
    for (ArxivWatchRule &rule : rules_) {
        if (rule.id != activeRule_.id)
            continue;
        QSet<QString> seen(rule.seenCitationIds.cbegin(), rule.seenCitationIds.cend());
        for (const RelatedPaperRecord &citing : data.papers) {
            const QString identity = citationIdentity(citing);
            if (identity.isEmpty() || seen.contains(identity))
                continue;
            seen.insert(identity);
            rule.seenCitationIds << identity;
            if (!rule.citationBaselineReady || !rule.enabled)
                continue;
            ArxivWatchHit hit;
            hit.ruleId = rule.id;
            hit.inspireRecid = citing.inspireRecid;
            hit.paper.arxivId = citing.arxivId;
            hit.paper.title = citing.title;
            hit.paper.authors = citing.authors;
            hit.paper.authorNames = citing.authorNames;
            hit.paper.published = citing.year;
            hit.paper.doi = citing.doi;
            hit.foundUtc = QDateTime::currentDateTimeUtc();
            hit.status = !citing.arxivId.isEmpty() && localFileExists(db_, citing.arxivId)
                             ? QStringLiteral("Local") : QStringLiteral("New");
            hits_.prepend(hit);
            ++newHitCount_;
            ++newCitationCount_;
            if (rule.autoDownload && hit.status == QStringLiteral("New") && !citing.arxivId.isEmpty())
                enqueueDownload(hit.paper, rule.folder);
        }
        rule.citationBaselineReady = true;
        rule.lastCheckedUtc = activeEndUtc_;
        rule.lastError = data.totalCount > data.papers.size()
                             ? QStringLiteral("INSPIRE returned only the first %1 of %2 citations; older additions may be missed.")
                                   .arg(data.papers.size()).arg(data.totalCount)
                             : QString();
        break;
    }
    while (hits_.size() > 500)
        hits_.removeLast();
    save();
    emit changed();
    nextRule();
}

void ArxivWatchManager::fetchPage()
{
    if (!checking_)
        return;
    const qint64 wait = ApiIntervalMs - (QDateTime::currentMSecsSinceEpoch() - lastApiQueryMs_);
    if (lastApiQueryMs_ > 0 && wait > 0) {
        QTimer::singleShot(wait, this, [this]() { fetchPage(); });
        return;
    }

    const QDateTime from = activeRule_.lastCheckedUtc.isValid()
                               ? activeRule_.lastCheckedUtc.addDays(-1)
                               : activeEndUtc_.addDays(-1);
    const QString dateRange = QStringLiteral("submittedDate:[%1 TO %2]")
                                  .arg(from.toUTC().toString(QStringLiteral("yyyyMMddhhmm")),
                                       activeEndUtc_.toUTC().toString(QStringLiteral("yyyyMMddhhmm")));
    const QString field = activeRule_.kind == QStringLiteral("author")
                              ? QStringLiteral("au:\"%1\"").arg(escapedAuthor(activeRule_.term))
                              : QStringLiteral("cat:%1").arg(activeRule_.term);
    QUrl url(QStringLiteral("https://export.arxiv.org/api/query"));
    QUrlQuery query;
    query.addQueryItem(QStringLiteral("search_query"), field + QStringLiteral(" AND ") + dateRange);
    query.addQueryItem(QStringLiteral("start"), QString::number(pageStart_));
    query.addQueryItem(QStringLiteral("max_results"), QString::number(PageSize));
    query.addQueryItem(QStringLiteral("sortBy"), QStringLiteral("submittedDate"));
    query.addQueryItem(QStringLiteral("sortOrder"), QStringLiteral("ascending"));
    url.setQuery(query);
    QNetworkRequest request(url);
    request.setHeader(QNetworkRequest::UserAgentHeader, QStringLiteral("HEPShelf/0.9.0 local-literature-library"));
    request.setAttribute(QNetworkRequest::RedirectPolicyAttribute,
                         QNetworkRequest::NoLessSafeRedirectPolicy);
    request.setTransferTimeout(30000);
    lastApiQueryMs_ = QDateTime::currentMSecsSinceEpoch();
    QNetworkReply *reply = network_.get(request);
    searchReply_ = reply;
    connect(reply, &QNetworkReply::finished, this, [this, reply]() {
        searchReply_ = nullptr;
        const QString failure = reply->errorString();
        const bool ok = reply->error() == QNetworkReply::NoError;
        const QByteArray body = ok ? reply->readAll() : QByteArray();
        reply->deleteLater();
        if (!ok) {
            ++failedRuleCount_;
            for (ArxivWatchRule &rule : rules_) {
                if (rule.id == activeRule_.id && rule.kind == activeRule_.kind
                    && rule.term.compare(activeRule_.term, Qt::CaseInsensitive) == 0)
                    rule.lastError = failure;
            }
            save();
            emit changed();
            emit statusChanged(QStringLiteral("arXiv watch %1 failed: %2").arg(activeRule_.term, failure));
            nextRule();
            return;
        }
        const ArxivFeedResult feed = parseArxivFeed(body);
        if (!feed.error.isEmpty()) {
            ++failedRuleCount_;
            for (ArxivWatchRule &rule : rules_) {
                if (rule.id == activeRule_.id && rule.kind == activeRule_.kind
                    && rule.term.compare(activeRule_.term, Qt::CaseInsensitive) == 0)
                    rule.lastError = feed.error;
            }
            save();
            emit changed();
            emit statusChanged(QStringLiteral("arXiv watch %1 failed: %2").arg(activeRule_.term, feed.error));
            nextRule();
            return;
        }
        processPapers(feed.papers);
        pageStart_ += feed.entryCount;
        if (feed.entryCount == PageSize && (feed.totalResults == 0 || pageStart_ < feed.totalResults)) {
            fetchPage();
            return;
        }
        for (ArxivWatchRule &rule : rules_) {
            if (rule.id == activeRule_.id && rule.kind == activeRule_.kind
                && rule.term.compare(activeRule_.term, Qt::CaseInsensitive) == 0) {
                rule.lastCheckedUtc = activeEndUtc_;
                rule.lastError.clear();
                break;
            }
        }
        save();
        emit changed();
        nextRule();
    });
}

void ArxivWatchManager::processPapers(const QList<PaperRecord> &papers)
{
    const bool ruleExists = std::any_of(rules_.cbegin(), rules_.cend(), [this](const ArxivWatchRule &rule) {
        return rule.id == activeRule_.id && rule.enabled;
    });
    if (!ruleExists)
        return;
    for (const PaperRecord &paper : papers) {
        bool known = false;
        for (const ArxivWatchHit &hit : hits_) {
            if (hit.ruleId == activeRule_.id && hit.paper.arxivId == paper.arxivId) {
                known = true;
                break;
            }
        }
        if (known)
            continue;
        ArxivWatchHit hit;
        hit.ruleId = activeRule_.id;
        hit.paper = paper;
        hit.foundUtc = QDateTime::currentDateTimeUtc();
        hit.status = localFileExists(db_, paper.arxivId) ? QStringLiteral("Local") : QStringLiteral("New");
        hits_.prepend(hit);
        ++newHitCount_;
        if (activeRule_.autoDownload && hit.status == QStringLiteral("New"))
            enqueueDownload(paper, activeRule_.folder);
    }
    while (hits_.size() > 500)
        hits_.removeLast();
    save();
    emit changed();
}

void ArxivWatchManager::enqueueDownload(const PaperRecord &paper, const QString &folder)
{
    if (folder.trimmed().isEmpty() || paper.arxivId.isEmpty())
        return;
    for (const DownloadJob &job : downloadQueue_) {
        if (job.paper.arxivId == paper.arxivId)
            return;
    }
    if (downloading_ && currentDownload_.paper.arxivId == paper.arxivId)
        return;
    downloadQueue_ << DownloadJob{paper, folder};
}

void ArxivWatchManager::downloadHit(const QString &ruleId, const QString &arxivId, const QString &folder)
{
    for (const ArxivWatchHit &hit : hits_) {
        if (hit.ruleId == ruleId && hit.paper.arxivId == arxivId) {
            enqueueDownload(hit.paper, folder);
            if (!checking_)
                nextDownload();
            return;
        }
    }
}

void ArxivWatchManager::nextDownload()
{
    if (checking_ || downloading_ || downloadQueue_.isEmpty())
        return;
    currentDownload_ = downloadQueue_.takeFirst();
    const PaperRecord &paper = currentDownload_.paper;
    if (localFileExists(db_, paper.arxivId)) {
        setPaperStatus(paper.arxivId, QStringLiteral("Local"));
        QTimer::singleShot(0, this, &ArxivWatchManager::nextDownload);
        return;
    }
    const QString folder = QDir(currentDownload_.folder).absolutePath();
    if (!QDir().mkpath(folder)) {
        finishDownload(QStringLiteral("Failed"), QStringLiteral("Could not create download folder: %1").arg(folder));
        return;
    }
    QString error;
    if (!db_->addFolder(folder, &error)) {
        finishDownload(QStringLiteral("Failed"), QStringLiteral("Could not watch download folder: %1").arg(error));
        return;
    }
    downloadPath_ = QDir(folder).filePath(filenameId(paper.arxivId) + QStringLiteral(".pdf"));
    if (QFileInfo::exists(downloadPath_)) {
        const QString indexedId = db_->arxivIdForPath(downloadPath_);
        if (!indexedId.isEmpty() && indexedId != paper.arxivId) {
            finishDownload(QStringLiteral("Failed"), QStringLiteral("Target belongs to another paper: %1").arg(downloadPath_));
            return;
        }
        QFile existing(downloadPath_);
        if (!existing.open(QIODevice::ReadOnly) || !existing.read(5).startsWith("%PDF-")) {
            finishDownload(QStringLiteral("Failed"), QStringLiteral("Target exists and is not a PDF: %1").arg(downloadPath_));
            return;
        }
        const QFileInfo info(downloadPath_);
        if (!db_->upsertPaper(preserveKnownMetadata(db_, paper), &error)
            || !db_->upsertFile(downloadPath_, paper.arxivId, info.size(), info.lastModified().toSecsSinceEpoch(), &error)) {
            finishDownload(QStringLiteral("Failed"), QStringLiteral("Could not index existing PDF: %1").arg(error));
            return;
        }
        emit libraryChanged();
        finishDownload(QStringLiteral("Downloaded"), QStringLiteral("Indexed existing PDF for arXiv:%1").arg(paper.arxivId));
        return;
    }

    saveFile_ = new QSaveFile(downloadPath_);
    if (!saveFile_->open(QIODevice::WriteOnly)) {
        const QString message = saveFile_->errorString();
        delete saveFile_;
        saveFile_ = nullptr;
        finishDownload(QStringLiteral("Failed"), QStringLiteral("Could not create PDF: %1").arg(message));
        return;
    }
    downloading_ = true;
    downloadPrefix_.clear();
    QNetworkRequest request(QUrl(QStringLiteral("https://arxiv.org/pdf/%1.pdf").arg(paper.arxivId)));
    request.setHeader(QNetworkRequest::UserAgentHeader, QStringLiteral("HEPShelf/0.9.0 local-literature-library"));
    request.setAttribute(QNetworkRequest::RedirectPolicyAttribute,
                         QNetworkRequest::NoLessSafeRedirectPolicy);
    request.setTransferTimeout(90000);
    emit statusChanged(QStringLiteral("Downloading arXiv:%1…").arg(paper.arxivId));
    downloadReply_ = network_.get(request);
    connect(downloadReply_, &QIODevice::readyRead, this, [this]() {
        if (!downloadReply_ || !saveFile_)
            return;
        const QByteArray chunk = downloadReply_->readAll();
        if (downloadPrefix_.size() < 5)
            downloadPrefix_.append(chunk.left(5 - downloadPrefix_.size()));
        if (saveFile_->write(chunk) != chunk.size())
            downloadReply_->abort();
    });
    connect(downloadReply_, &QNetworkReply::finished, this, [this, paper]() {
        QNetworkReply *reply = downloadReply_;
        downloadReply_ = nullptr;
        if (!reply)
            return;
        const QByteArray tail = reply->readAll();
        if (downloadPrefix_.size() < 5)
            downloadPrefix_.append(tail.left(5 - downloadPrefix_.size()));
        const bool wrote = saveFile_ && saveFile_->write(tail) == tail.size();
        const int status = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
        const bool valid = reply->error() == QNetworkReply::NoError && status >= 200 && status < 300
                           && wrote && downloadPrefix_.startsWith("%PDF-");
        const QString networkError = reply->errorString();
        reply->deleteLater();
        if (!valid || QFileInfo::exists(downloadPath_)) {
            if (saveFile_)
                saveFile_->cancelWriting();
            delete saveFile_;
            saveFile_ = nullptr;
            finishDownload(QStringLiteral("Failed"), QStringLiteral("Download failed for arXiv:%1: %2")
                               .arg(paper.arxivId, valid ? QStringLiteral("target appeared during download") : networkError));
            return;
        }
        if (!saveFile_->commit()) {
            const QString message = saveFile_->errorString();
            delete saveFile_;
            saveFile_ = nullptr;
            finishDownload(QStringLiteral("Failed"), QStringLiteral("Could not save arXiv:%1: %2").arg(paper.arxivId, message));
            return;
        }
        delete saveFile_;
        saveFile_ = nullptr;
        const QFileInfo info(downloadPath_);
        QString dbError;
        if (!db_->upsertPaper(preserveKnownMetadata(db_, paper), &dbError)
            || !db_->upsertFile(info.absoluteFilePath(), paper.arxivId, info.size(), info.lastModified().toSecsSinceEpoch(), &dbError)) {
            finishDownload(QStringLiteral("Failed"), QStringLiteral("PDF saved but indexing failed: %1").arg(dbError));
            return;
        }
        emit libraryChanged();
        finishDownload(QStringLiteral("Downloaded"), QStringLiteral("Downloaded arXiv:%1").arg(paper.arxivId));
    });
}

void ArxivWatchManager::setPaperStatus(const QString &arxivId, const QString &status)
{
    for (ArxivWatchHit &hit : hits_) {
        if (hit.paper.arxivId == arxivId)
            hit.status = status;
    }
    save();
    emit changed();
}

void ArxivWatchManager::finishDownload(const QString &status, const QString &message)
{
    downloading_ = false;
    setPaperStatus(currentDownload_.paper.arxivId, status);
    emit statusChanged(message);
    QTimer::singleShot(0, this, &ArxivWatchManager::nextDownload);
}
