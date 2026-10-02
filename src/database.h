#pragma once

#include <QList>
#include <QSqlDatabase>
#include <QString>
#include <QStringList>

struct PaperRecord {
    QString arxivId;
    QString title;
    QString authors;
    QStringList authorNames;
    QString abstractText;
    QString published;
    QString updated;
    QString primaryCategory;
    QString categories;
    QString doi;
    QString journalRef;
    QString comments;
};

enum class LibraryFilter {
    All = 0,
    Favorites,
    Recent,
    MissingMetadata,
    Unread,
    Duplicates,
    Tag,
    Collection,
    Author,
    Category,
    Trail
};

struct LibraryRow {
    QString arxivId;
    QString title;
    QString authors;
    QString year;
    QString category;
    int fileCount = 0;
    QString path;
    bool favorite = false;
    QString lastOpened;
    int lastPage = 0;
    int pageCount = 0;
    int citationCount = -1;
    int referenceCount = -1;
};

struct CitationMetrics {
    int inspireRecid = 0;
    int citationCount = -1;
    int citationCountWithoutSelf = -1;
    int referenceCount = -1;
    QString fetchedAt;
};

struct ReferenceRecord {
    int position = 0;
    int inspireRecid = 0;
    QString arxivId;
    QString title;
    QString authors;
    QString doi;
    QString rawText;
    bool local = false;
    QString localPath;
};

struct RelatedPaperRecord {
    int inspireRecid = 0;
    QString arxivId;
    QString title;
    QString authors;
    QStringList authorNames;
    QString doi;
    QString year;
    int citationCount = -1;
    bool local = false;
    QString localPath;
};

struct CitingCacheInfo {
    int totalCount = -1;
    QString fetchedAt;
};


struct PaperNoteRecord {
    QString text;
    QString updatedAt;
};

struct PaperHighlightRecord {
    int id = 0;
    int page = 0;
    int startIndex = 0;
    int length = 0;
    QString text;
};

struct LiteratureTrailRecord {
    int id = 0;
    QString name;
    QString description;
    int itemCount = 0;
    QString updatedAt;
};

struct TrailItemRecord {
    int id = 0;
    int position = 0;
    QString arxivId;
    int inspireRecid = 0;
    QString doi;
    QString title;
    QString authors;
    QString year;
    QString note;
    bool local = false;
    QString localArxivId;
    QString localPath;
};

struct FileRecord {
    QString path;
    QString version;
    qint64 size = 0;
    qint64 mtime = 0;
};

struct PaperDetails {
    PaperRecord paper;
    QStringList paths;
    QList<FileRecord> files;
    QStringList tags;
    QStringList collections;
    QStringList indexedAuthors;
    bool favorite = false;
    QString lastOpened;
    int openCount = 0;
    int lastPage = 0;
    int pageCount = 0;
    QString fetchedAt;
    CitationMetrics citations;
};

struct FacetCount {
    QString value;
    int count = 0;
};

class Database {
public:
    Database();
    ~Database();

    bool open(QString *error = nullptr);

    bool addFolder(const QString &path, QString *error = nullptr);
    bool removeFolder(const QString &path, bool removeIndexedFiles, QString *error = nullptr);
    QStringList folders(QString *error = nullptr) const;

    bool markFolderUnseen(const QString &folder, QString *error = nullptr);
    bool removeUnseenInFolder(const QString &folder, QString *error = nullptr);

    bool upsertFile(const QString &path,
                    const QString &arxivId,
                    qint64 size,
                    qint64 mtime,
                    QString *error = nullptr);
    bool removeFile(const QString &path, QString *error = nullptr);
    bool updateFilePath(const QString &oldPath,
                        const QString &newPath,
                        qint64 size,
                        qint64 mtime,
                        QString *error = nullptr);

    QStringList idsMissingMetadata(QString *error = nullptr) const;
    bool upsertPaper(const PaperRecord &paper, QString *error = nullptr);
    bool updateCitationMetrics(const QString &arxivId,
                               const CitationMetrics &metrics,
                               QString *error = nullptr);
    bool replaceReferences(const QString &sourceArxivId,
                           const QList<ReferenceRecord> &references,
                           QString *error = nullptr);
    QList<ReferenceRecord> referencesForPaper(const QString &sourceArxivId,
                                              QString *error = nullptr) const;
    bool replaceCitingPapers(const QString &sourceArxivId,
                             const QList<RelatedPaperRecord> &papers,
                             int totalCount,
                             QString *error = nullptr);
    QList<RelatedPaperRecord> citingPapersForPaper(const QString &sourceArxivId,
                                                   QString *error = nullptr) const;
    CitingCacheInfo citingCacheInfo(const QString &sourceArxivId,
                                    QString *error = nullptr) const;
    bool hasLocalFile(const QString &arxivId, QString *path = nullptr) const;
    bool resolveLocalPaper(const QString &arxivId,
                           const QString &doi,
                           int inspireRecid,
                           QString *resolvedArxivId = nullptr,
                           QString *path = nullptr) const;
    QString arxivIdForPath(const QString &path) const;

    QList<LibraryRow> search(const QString &query,
                             LibraryFilter filter = LibraryFilter::All,
                             const QString &facetValue = {},
                             QString *error = nullptr) const;
    bool paperDetails(const QString &arxivId, PaperDetails *details, QString *error = nullptr) const;

    bool setFavorite(const QString &arxivId, bool favorite, QString *error = nullptr);
    bool recordOpened(const QString &arxivId, QString *error = nullptr);
    bool setReadingProgress(const QString &arxivId,
                            int lastPage,
                            int pageCount,
                            QString *error = nullptr);

    PaperNoteRecord paperNote(const QString &arxivId, QString *error = nullptr) const;
    bool setPaperNote(const QString &arxivId, const QString &text, QString *error = nullptr);
    QList<PaperHighlightRecord> paperHighlights(const QString &arxivId, QString *error = nullptr) const;
    bool addPaperHighlight(const QString &arxivId, const PaperHighlightRecord &highlight, QString *error = nullptr);
    bool removePaperHighlight(int id, QString *error = nullptr);
    QString paperPageNote(const QString &arxivId, int page, QString *error = nullptr) const;
    bool setPaperPageNote(const QString &arxivId, int page, const QString &text, QString *error = nullptr);

    QList<LiteratureTrailRecord> literatureTrails(QString *error = nullptr) const;
    QList<LiteratureTrailRecord> literatureTrailsForPaper(const QString &arxivId, QString *error = nullptr) const;
    int createLiteratureTrail(const QString &name, const QString &description = {}, QString *error = nullptr);
    bool updateLiteratureTrail(int trailId, const QString &name, const QString &description, QString *error = nullptr);
    bool deleteLiteratureTrail(int trailId, QString *error = nullptr);
    bool addTrailItem(int trailId, const TrailItemRecord &item, QString *error = nullptr);
    QList<TrailItemRecord> trailItems(int trailId, QString *error = nullptr) const;
    bool removeTrailItem(int itemId, QString *error = nullptr);
    bool moveTrailItem(int trailId, int itemId, int delta, QString *error = nullptr);
    bool setTrailItemNote(int itemId, const QString &note, QString *error = nullptr);

    bool createTag(const QString &name, QString *error = nullptr);
    bool deleteTag(const QString &name, QString *error = nullptr);
    bool addTagToPaper(const QString &arxivId, const QString &name, QString *error = nullptr);
    bool removeTagFromPaper(const QString &arxivId, const QString &name, QString *error = nullptr);
    QStringList tagsForPaper(const QString &arxivId, QString *error = nullptr) const;

    bool createCollection(const QString &name, QString *error = nullptr);
    bool deleteCollection(const QString &name, QString *error = nullptr);
    bool addPaperToCollection(const QString &arxivId, const QString &name, QString *error = nullptr);
    bool removePaperFromCollection(const QString &arxivId, const QString &name, QString *error = nullptr);
    QStringList collectionsForPaper(const QString &arxivId, QString *error = nullptr) const;

    QList<FacetCount> tagCounts(QString *error = nullptr) const;
    QList<FacetCount> collectionCounts(QString *error = nullptr) const;
    QList<FacetCount> authorCounts(QString *error = nullptr) const;
    QList<FacetCount> categoryCounts(QString *error = nullptr) const;
    QList<FacetCount> trailCounts(QString *error = nullptr) const;

    int paperCount(QString *error = nullptr) const;
    int fileCount(QString *error = nullptr) const;
    int favoriteCount(QString *error = nullptr) const;
    int missingMetadataCount(QString *error = nullptr) const;
    int unreadCount(QString *error = nullptr) const;
    int duplicatePaperCount(QString *error = nullptr) const;

private:
    bool execSchema(QString *error);
    bool ensureColumn(const QString &table,
                      const QString &column,
                      const QString &definition,
                      QString *error);
    bool cleanupOrphanPapers(QString *error);
    bool rebuildAuthorIndex(QString *error);

    QSqlDatabase db_;
    QString connectionName_;
};
