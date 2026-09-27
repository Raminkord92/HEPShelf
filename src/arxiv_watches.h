#pragma once

#include "database.h"
#include "inspire_client.h"

#include <QDateTime>
#include <QNetworkAccessManager>
#include <QObject>
#include <QSet>
#include <QTimer>

class QNetworkReply;
class QSaveFile;

struct ArxivWatchRule {
    QString id;
    QString kind; // "author", "category", or "citation"
    QString term;
    QString paperTitle;
    int inspireRecid = 0;
    bool citationBaselineReady = false;
    QStringList seenCitationIds;
    bool enabled = true;
    bool autoDownload = false;
    QString folder;
    QDateTime lastCheckedUtc;
    QString lastError;
};

struct ArxivWatchHit {
    QString ruleId;
    PaperRecord paper;
    QDateTime foundUtc;
    QString status;
    int inspireRecid = 0;
};

class ArxivWatchManager : public QObject {
    Q_OBJECT
public:
    explicit ArxivWatchManager(Database *database, QObject *parent = nullptr);
    ~ArxivWatchManager() override;

    const QList<ArxivWatchRule> &rules() const { return rules_; }
    const QList<ArxivWatchHit> &hits() const { return hits_; }
    bool busy() const { return checking_ || downloading_; }
    bool addRule(ArxivWatchRule rule, QString *error = nullptr);
    bool updateRule(const ArxivWatchRule &rule, QString *error = nullptr);
    void removeRule(const QString &id);
    void checkNow();
    void downloadHit(const QString &ruleId, const QString &arxivId, const QString &folder);
    void markHitsSeen();

signals:
    void changed();
    void statusChanged(const QString &message);
    void libraryChanged();
    void newCitationsFound(int count);

private:
    struct DownloadJob {
        PaperRecord paper;
        QString folder;
    };

    void load();
    void save() const;
    void checkDue();
    void beginChecks(const QStringList &ruleIds);
    void nextRule();
    void fetchPage();
    void fetchCitations();
    void processCitations(const InspireCitingData &data);
    void failActiveRule(const QString &error);
    void processPapers(const QList<PaperRecord> &papers);
    void enqueueDownload(const PaperRecord &paper, const QString &folder);
    void nextDownload();
    void finishDownload(const QString &status, const QString &message);
    void setPaperStatus(const QString &arxivId, const QString &status);

    Database *db_ = nullptr;
    QNetworkAccessManager network_;
    InspireClient inspire_;
    QTimer dueTimer_;
    QList<ArxivWatchRule> rules_;
    QList<ArxivWatchHit> hits_;
    QStringList ruleQueue_;
    ArxivWatchRule activeRule_;
    QDateTime activeEndUtc_;
    int pageStart_ = 0;
    int newHitCount_ = 0;
    int newCitationCount_ = 0;
    int failedRuleCount_ = 0;
    bool checking_ = false;
    qint64 lastApiQueryMs_ = 0;
    QNetworkReply *searchReply_ = nullptr;

    QList<DownloadJob> downloadQueue_;
    DownloadJob currentDownload_;
    bool downloading_ = false;
    QNetworkReply *downloadReply_ = nullptr;
    QSaveFile *saveFile_ = nullptr;
    QString downloadPath_;
    QByteArray downloadPrefix_;
};
