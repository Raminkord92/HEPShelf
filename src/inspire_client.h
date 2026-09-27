#pragma once

#include "database.h"

#include <QHash>
#include <QNetworkAccessManager>
#include <QObject>

#include <functional>

struct InspirePaperData {
    CitationMetrics metrics;
    QList<ReferenceRecord> references;
};

struct InspireCitingData {
    QList<RelatedPaperRecord> papers;
    int totalCount = -1;
};

class InspireClient : public QObject {
public:
    using PaperCallback = std::function<void(const InspirePaperData &data, const QString &error)>;
    using TextCallback = std::function<void(const QString &text, const QString &error)>;
    using CitingCallback = std::function<void(const InspireCitingData &data, const QString &error)>;
    using TitlesCallback = std::function<void(const QHash<int, QString> &titles, const QString &error)>;

    explicit InspireClient(QObject *parent = nullptr);

    void fetchPaper(const QString &arxivId, PaperCallback callback);
    void fetchPaperByRecid(int inspireRecid, PaperCallback callback);
    void fetchBibTeX(const QString &arxivId, TextCallback callback);
    void fetchCitingPapers(int inspireRecid, int limit, CitingCallback callback);
    void fetchTitlesByRecids(const QList<int> &recids, TitlesCallback callback);

private:
    bool parsePaper(const QByteArray &json, InspirePaperData *data, QString *error) const;
    bool parseCitingSearch(const QByteArray &json, InspireCitingData *data, QString *error) const;

    QNetworkAccessManager network_;
};
