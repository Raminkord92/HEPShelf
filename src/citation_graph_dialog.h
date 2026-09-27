#pragma once

#include "database.h"
#include "inspire_client.h"

#include <QDialog>
#include <QHash>
#include <QList>
#include <QPoint>
#include <QSet>
#include <QString>
#include <functional>

class QCheckBox;
class QGraphicsScene;
class QGraphicsView;
class QLabel;
class QPushButton;
class QSpinBox;

class CitationGraphDialog : public QDialog {
public:
    using OpenLocalCallback = std::function<void(const QString &arxivId, const QString &path)>;

    CitationGraphDialog(Database *database,
                        const QString &sourceArxivId,
                        OpenLocalCallback openLocal,
                        QWidget *parent = nullptr);

    bool dataChanged() const { return dataChanged_; }

private:
    struct GraphPaper {
        QString key;
        int inspireRecid = 0;
        QString arxivId;
        QString title;
        bool titleNeedsResolution = false;
        QString authors;
        QString doi;
        QString year;
        int citationCount = -1;
        bool local = false;
        QString localArxivId;
        QString localPath;
        int depth = 0;

        bool referencesFetched = false;
        bool citingFetched = false;
        int referenceCursor = 0;
        int citingCursor = 0;
        int totalCiting = -1;
    };

    struct GraphEdge {
        QString from;
        QString to;
    };

    void buildUi();
    void resetGraph();
    void rebuildGraph(bool fitView = false, const QString &focusKey = {});
    void refreshRootFromInspire();
    void fetchRootCiting(int recid);

    QString makeKey(const GraphPaper &paper) const;
    QString findMatchingNode(const GraphPaper &paper) const;
    QString addOrMergeNode(GraphPaper paper, int depth);
    void addEdge(const QString &from, const QString &to);
    void resolveLocal(GraphPaper *paper) const;
    void hydrateLocalCaches(const QString &key);

    void addReferenceChunk(const QString &sourceKey);
    void queueReferenceTitle(int inspireRecid);
    void fetchPendingReferenceTitles();
    void addCitingChunk(const QString &sourceKey);
    void expandNode(const QString &key, bool references, bool citedBy);
    void fetchPaperForExpansion(const QString &key, bool references, bool citedBy);
    void continueExpansionAfterPaper(const QString &key, bool references, bool citedBy);
    void fetchCitingForExpansion(const QString &key, bool referencesAlreadyHandled);
    void finishExpansion(const QString &message, const QString &focusKey = {});

    void showNodeMenu(const QString &key, const QPoint &globalPos);
    void openNode(const QString &key);
    void openExternalNode(const QString &key);
    void saveExplorationAsTrail();
    void addNodeToTrail(const QString &key);
    TrailItemRecord trailItemForNode(const QString &key) const;
    QString selectedNodeKey() const;
    QString nodeSubtitle(const GraphPaper &paper) const;

    Database *db_ = nullptr;
    QString sourceArxivId_;
    OpenLocalCallback openLocal_;
    InspireClient inspire_;
    PaperDetails sourceDetails_;

    QHash<QString, GraphPaper> nodes_;
    QList<GraphEdge> edges_;
    QSet<QString> edgeKeys_;
    QHash<QString, QList<ReferenceRecord>> referenceCache_;
    QHash<QString, QList<RelatedPaperRecord>> citingCache_;
    QString rootKey_;
    QList<QString> explorationOrder_;
    QHash<int, QString> resolvedReferenceTitles_;
    QSet<int> requestedReferenceTitles_;
    QList<int> pendingReferenceTitles_;
    bool fetchingReferenceTitles_ = false;

    bool refreshingRoot_ = false;
    bool expanding_ = false;
    bool dataChanged_ = false;

    QLabel *summary_ = nullptr;
    QLabel *status_ = nullptr;
    QCheckBox *localOnly_ = nullptr;
    QSpinBox *neighborLimit_ = nullptr;
    QPushButton *expandButton_ = nullptr;
    QPushButton *refreshButton_ = nullptr;
    QPushButton *saveTrailButton_ = nullptr;
    QPushButton *resetButton_ = nullptr;
    QGraphicsScene *scene_ = nullptr;
    QGraphicsView *view_ = nullptr;
};
