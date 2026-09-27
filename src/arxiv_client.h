#pragma once

#include "database.h"

#include <QList>
#include <QNetworkAccessManager>
#include <QObject>
#include <QSet>
#include <QStringList>

#include <functional>

class ArxivClient : public QObject {
public:
    using BatchCallback = std::function<void(const QList<PaperRecord> &papers, const QString &error)>;
    using ProgressCallback = std::function<void(int completed, int total)>;
    using FinishedCallback = std::function<void()>;

    explicit ArxivClient(QObject *parent = nullptr);

    void fetch(const QStringList &ids,
               BatchCallback onBatch,
               ProgressCallback onProgress,
               FinishedCallback onFinished);

    bool busy() const { return busy_; }

private:
    void requestNextBatch();
    void requestNextInspireFallback();
    void finishCurrentBatch();

    QList<PaperRecord> parseFeed(const QByteArray &xml, QString *error) const;
    bool parseInspireRecord(const QByteArray &json,
                            const QString &requestedId,
                            PaperRecord *paper,
                            QString *error) const;

    QNetworkAccessManager network_;
    QStringList pendingIds_;
    QStringList currentBatch_;
    QStringList inspirePending_;
    QList<PaperRecord> currentPapers_;
    QSet<QString> currentResolved_;
    QStringList currentErrors_;

    int totalIds_ = 0;
    int completedIds_ = 0;
    bool busy_ = false;

    BatchCallback onBatch_;
    ProgressCallback onProgress_;
    FinishedCallback onFinished_;
};
