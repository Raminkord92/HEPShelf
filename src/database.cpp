#include "database.h"

#include <QDir>
#include <QFileInfo>
#include <QRegularExpression>
#include <QSqlError>
#include <QSqlQuery>
#include <QStandardPaths>
#include <QUuid>
#include <QVariant>

namespace {
bool setError(QString *out, const QString &message)
{
    if (out)
        *out = message;
    return false;
}

QString normalizedFolder(const QString &path)
{
    return QDir::cleanPath(QDir(path).absolutePath());
}

QString normalizedLabel(QString value)
{
    value = value.simplified();
    return value.left(120);
}

QString trailIdentity(const TrailItemRecord &item)
{
    if (!item.arxivId.trimmed().isEmpty())
        return QStringLiteral("arxiv:") + item.arxivId.trimmed().toLower();
    if (item.inspireRecid > 0)
        return QStringLiteral("inspire:%1").arg(item.inspireRecid);
    if (!item.doi.trimmed().isEmpty())
        return QStringLiteral("doi:") + item.doi.trimmed().toLower();

    QString title = item.title.toLower().simplified();
    title.remove(QRegularExpression(QStringLiteral("[^\\p{L}\\p{N} ]+")));
    title = title.simplified();
    if (!title.isEmpty())
        return QStringLiteral("title:") + title.left(220);
    return {};
}

QString versionFromPath(const QString &path)
{
    const QString base = QFileInfo(path).completeBaseName();
    static const QRegularExpression rx(QStringLiteral("v(\\d+)$"),
                                       QRegularExpression::CaseInsensitiveOption);
    const auto match = rx.match(base);
    return match.hasMatch() ? QStringLiteral("v%1").arg(match.captured(1)) : QString();
}

QStringList conservativeAuthorSplit(const QString &authors)
{
    QStringList result;
    const QString simplified = authors.simplified();
    if (simplified.isEmpty())
        return result;

    // arXiv's Atom feed gives display names and HEPShelf stores them comma-separated.
    // This fallback is only used for databases created before structured author indexing.
    const QStringList raw = simplified.split(QStringLiteral(", "), Qt::SkipEmptyParts);
    for (const QString &item : raw) {
        const QString name = item.simplified();
        if (name.size() >= 2 && !result.contains(name, Qt::CaseInsensitive))
            result << name;
    }
    return result;
}

QList<FacetCount> readFacetQuery(QSqlDatabase db, const QString &sql, QString *error)
{
    QList<FacetCount> result;
    QSqlQuery q(db);
    if (!q.exec(sql)) {
        setError(error, q.lastError().text());
        return result;
    }
    while (q.next()) {
        FacetCount facet;
        facet.value = q.value(0).toString();
        facet.count = q.value(1).toInt();
        if (!facet.value.trimmed().isEmpty())
            result << facet;
    }
    return result;
}
}

Database::Database()
    : connectionName_(QStringLiteral("hepshelf-%1").arg(QUuid::createUuid().toString(QUuid::WithoutBraces)))
{
}

Database::~Database()
{
    if (db_.isValid()) {
        db_.close();
        const QString name = connectionName_;
        db_ = QSqlDatabase();
        QSqlDatabase::removeDatabase(name);
    }
}

bool Database::open(QString *error)
{
    const QString dataDir = QStandardPaths::writableLocation(QStandardPaths::AppDataLocation);
    if (dataDir.isEmpty())
        return setError(error, QStringLiteral("Qt could not determine an application data directory."));

    if (!QDir().mkpath(dataDir))
        return setError(error, QStringLiteral("Could not create %1").arg(dataDir));

    db_ = QSqlDatabase::addDatabase(QStringLiteral("QSQLITE"), connectionName_);
    db_.setDatabaseName(QDir(dataDir).filePath(QStringLiteral("library.sqlite")));
    if (!db_.open())
        return setError(error, db_.lastError().text());

    QSqlQuery pragma(db_);
    pragma.exec(QStringLiteral("PRAGMA foreign_keys = ON"));
    pragma.exec(QStringLiteral("PRAGMA journal_mode = WAL"));
    pragma.exec(QStringLiteral("PRAGMA synchronous = NORMAL"));
    pragma.exec(QStringLiteral("PRAGMA busy_timeout = 5000"));

    if (!execSchema(error))
        return false;
    return rebuildAuthorIndex(error);
}

bool Database::execSchema(QString *error)
{
    static const QStringList schema = {
        QStringLiteral(R"SQL(
            CREATE TABLE IF NOT EXISTS folders (
                path TEXT PRIMARY KEY,
                added_at TEXT NOT NULL DEFAULT CURRENT_TIMESTAMP
            )
        )SQL"),
        QStringLiteral(R"SQL(
            CREATE TABLE IF NOT EXISTS papers (
                arxiv_id TEXT PRIMARY KEY,
                title TEXT,
                authors TEXT,
                abstract TEXT,
                published TEXT,
                updated TEXT,
                primary_category TEXT,
                fetched_at TEXT
            )
        )SQL"),
        QStringLiteral(R"SQL(
            CREATE TABLE IF NOT EXISTS files (
                id INTEGER PRIMARY KEY AUTOINCREMENT,
                path TEXT NOT NULL UNIQUE,
                arxiv_id TEXT NOT NULL,
                size INTEGER NOT NULL,
                mtime INTEGER NOT NULL,
                seen INTEGER NOT NULL DEFAULT 1,
                FOREIGN KEY(arxiv_id) REFERENCES papers(arxiv_id)
                    ON UPDATE CASCADE ON DELETE CASCADE
            )
        )SQL"),
        QStringLiteral(R"SQL(
            CREATE TABLE IF NOT EXISTS tags (
                id INTEGER PRIMARY KEY AUTOINCREMENT,
                name TEXT NOT NULL UNIQUE COLLATE NOCASE,
                created_at TEXT NOT NULL DEFAULT CURRENT_TIMESTAMP
            )
        )SQL"),
        QStringLiteral(R"SQL(
            CREATE TABLE IF NOT EXISTS paper_tags (
                arxiv_id TEXT NOT NULL,
                tag_id INTEGER NOT NULL,
                PRIMARY KEY(arxiv_id, tag_id),
                FOREIGN KEY(arxiv_id) REFERENCES papers(arxiv_id) ON DELETE CASCADE,
                FOREIGN KEY(tag_id) REFERENCES tags(id) ON DELETE CASCADE
            )
        )SQL"),
        QStringLiteral(R"SQL(
            CREATE TABLE IF NOT EXISTS collections (
                id INTEGER PRIMARY KEY AUTOINCREMENT,
                name TEXT NOT NULL UNIQUE COLLATE NOCASE,
                created_at TEXT NOT NULL DEFAULT CURRENT_TIMESTAMP
            )
        )SQL"),
        QStringLiteral(R"SQL(
            CREATE TABLE IF NOT EXISTS collection_papers (
                collection_id INTEGER NOT NULL,
                arxiv_id TEXT NOT NULL,
                PRIMARY KEY(collection_id, arxiv_id),
                FOREIGN KEY(collection_id) REFERENCES collections(id) ON DELETE CASCADE,
                FOREIGN KEY(arxiv_id) REFERENCES papers(arxiv_id) ON DELETE CASCADE
            )
        )SQL"),
        QStringLiteral(R"SQL(
            CREATE TABLE IF NOT EXISTS authors (
                id INTEGER PRIMARY KEY AUTOINCREMENT,
                canonical_name TEXT NOT NULL UNIQUE COLLATE NOCASE
            )
        )SQL"),
        QStringLiteral(R"SQL(
            CREATE TABLE IF NOT EXISTS paper_authors (
                arxiv_id TEXT NOT NULL,
                author_id INTEGER NOT NULL,
                position INTEGER NOT NULL DEFAULT 0,
                PRIMARY KEY(arxiv_id, author_id),
                FOREIGN KEY(arxiv_id) REFERENCES papers(arxiv_id) ON DELETE CASCADE,
                FOREIGN KEY(author_id) REFERENCES authors(id) ON DELETE CASCADE
            )
        )SQL"),
        QStringLiteral(R"SQL(
            CREATE TABLE IF NOT EXISTS paper_references (
                source_arxiv_id TEXT NOT NULL,
                position INTEGER NOT NULL,
                inspire_recid INTEGER,
                target_arxiv_id TEXT,
                title TEXT,
                authors TEXT,
                doi TEXT,
                raw_text TEXT,
                PRIMARY KEY(source_arxiv_id, position),
                FOREIGN KEY(source_arxiv_id) REFERENCES papers(arxiv_id) ON DELETE CASCADE
            )
        )SQL"),
        QStringLiteral(R"SQL(
            CREATE TABLE IF NOT EXISTS paper_citing_cache (
                source_arxiv_id TEXT PRIMARY KEY,
                total_count INTEGER NOT NULL DEFAULT 0,
                fetched_at TEXT NOT NULL DEFAULT CURRENT_TIMESTAMP,
                FOREIGN KEY(source_arxiv_id) REFERENCES papers(arxiv_id) ON DELETE CASCADE
            )
        )SQL"),
        QStringLiteral(R"SQL(
            CREATE TABLE IF NOT EXISTS paper_citing_papers (
                source_arxiv_id TEXT NOT NULL,
                position INTEGER NOT NULL,
                inspire_recid INTEGER,
                target_arxiv_id TEXT,
                title TEXT,
                authors TEXT,
                doi TEXT,
                year TEXT,
                citation_count INTEGER,
                PRIMARY KEY(source_arxiv_id, position),
                FOREIGN KEY(source_arxiv_id) REFERENCES papers(arxiv_id) ON DELETE CASCADE
            )
        )SQL"),
        QStringLiteral(R"SQL(
            CREATE TABLE IF NOT EXISTS paper_notes (
                arxiv_id TEXT PRIMARY KEY,
                note TEXT NOT NULL DEFAULT '',
                updated_at TEXT NOT NULL DEFAULT CURRENT_TIMESTAMP,
                FOREIGN KEY(arxiv_id) REFERENCES papers(arxiv_id) ON DELETE CASCADE
            )
        )SQL"),
        QStringLiteral(R"SQL(
            CREATE TABLE IF NOT EXISTS literature_trails (
                id INTEGER PRIMARY KEY AUTOINCREMENT,
                name TEXT NOT NULL UNIQUE COLLATE NOCASE,
                description TEXT NOT NULL DEFAULT '',
                created_at TEXT NOT NULL DEFAULT CURRENT_TIMESTAMP,
                updated_at TEXT NOT NULL DEFAULT CURRENT_TIMESTAMP
            )
        )SQL"),
        QStringLiteral(R"SQL(
            CREATE TABLE IF NOT EXISTS literature_trail_items (
                id INTEGER PRIMARY KEY AUTOINCREMENT,
                trail_id INTEGER NOT NULL,
                position INTEGER NOT NULL DEFAULT 0,
                identity_key TEXT NOT NULL,
                arxiv_id TEXT,
                inspire_recid INTEGER,
                doi TEXT,
                title TEXT,
                authors TEXT,
                year TEXT,
                note TEXT NOT NULL DEFAULT '',
                added_at TEXT NOT NULL DEFAULT CURRENT_TIMESTAMP,
                UNIQUE(trail_id, identity_key),
                FOREIGN KEY(trail_id) REFERENCES literature_trails(id) ON DELETE CASCADE
            )
        )SQL"),
        QStringLiteral("CREATE INDEX IF NOT EXISTS idx_files_arxiv_id ON files(arxiv_id)"),
        QStringLiteral("CREATE INDEX IF NOT EXISTS idx_papers_title ON papers(title)"),
        QStringLiteral("CREATE INDEX IF NOT EXISTS idx_papers_authors ON papers(authors)"),
        QStringLiteral("CREATE INDEX IF NOT EXISTS idx_paper_tags_arxiv ON paper_tags(arxiv_id)"),
        QStringLiteral("CREATE INDEX IF NOT EXISTS idx_collection_papers_arxiv ON collection_papers(arxiv_id)"),
        QStringLiteral("CREATE INDEX IF NOT EXISTS idx_paper_authors_arxiv ON paper_authors(arxiv_id)"),
        QStringLiteral("CREATE INDEX IF NOT EXISTS idx_paper_references_source ON paper_references(source_arxiv_id)"),
        QStringLiteral("CREATE INDEX IF NOT EXISTS idx_paper_references_target_arxiv ON paper_references(target_arxiv_id)"),
        QStringLiteral("CREATE INDEX IF NOT EXISTS idx_paper_citing_source ON paper_citing_papers(source_arxiv_id)"),
        QStringLiteral("CREATE INDEX IF NOT EXISTS idx_paper_citing_target_arxiv ON paper_citing_papers(target_arxiv_id)"),
        QStringLiteral("CREATE INDEX IF NOT EXISTS idx_paper_notes_arxiv ON paper_notes(arxiv_id)"),
        QStringLiteral("CREATE INDEX IF NOT EXISTS idx_trail_items_trail ON literature_trail_items(trail_id, position)"),
        QStringLiteral("CREATE INDEX IF NOT EXISTS idx_trail_items_arxiv ON literature_trail_items(arxiv_id)")
    };

    QSqlQuery q(db_);
    for (const QString &statement : schema) {
        if (!q.exec(statement))
            return setError(error, q.lastError().text());
    }

    struct ColumnMigration { const char *table; const char *name; const char *definition; };
    static const ColumnMigration migrations[] = {
        {"papers", "categories", "TEXT"},
        {"papers", "doi", "TEXT"},
        {"papers", "journal_ref", "TEXT"},
        {"papers", "comments", "TEXT"},
        {"papers", "favorite", "INTEGER NOT NULL DEFAULT 0"},
        {"papers", "last_opened", "TEXT"},
        {"papers", "open_count", "INTEGER NOT NULL DEFAULT 0"},
        {"papers", "last_page", "INTEGER NOT NULL DEFAULT 0"},
        {"papers", "page_count", "INTEGER NOT NULL DEFAULT 0"},
        {"papers", "inspire_recid", "INTEGER"},
        {"papers", "citation_count", "INTEGER"},
        {"papers", "citation_count_no_self", "INTEGER"},
        {"papers", "reference_count", "INTEGER"},
        {"papers", "inspire_fetched_at", "TEXT"},
        {"files", "arxiv_version", "TEXT"}
    };

    for (const auto &migration : migrations) {
        if (!ensureColumn(QString::fromLatin1(migration.table),
                          QString::fromLatin1(migration.name),
                          QString::fromLatin1(migration.definition),
                          error))
            return false;
    }

    QSqlQuery indexQuery(db_);
    if (!indexQuery.exec(QStringLiteral("CREATE INDEX IF NOT EXISTS idx_papers_last_opened ON papers(last_opened)")))
        return setError(error, indexQuery.lastError().text());

    // Populate version labels for databases created by earlier HEPShelf releases.
    QSqlQuery files(db_);
    if (files.exec(QStringLiteral("SELECT id, path FROM files WHERE arxiv_version IS NULL OR trim(arxiv_version) = ''"))) {
        QSqlQuery update(db_);
        update.prepare(QStringLiteral("UPDATE files SET arxiv_version = ? WHERE id = ?"));
        while (files.next()) {
            const QString version = versionFromPath(files.value(1).toString());
            if (version.isEmpty())
                continue;
            update.bindValue(0, version);
            update.bindValue(1, files.value(0));
            update.exec();
        }
    }

    return true;
}

bool Database::ensureColumn(const QString &table,
                            const QString &column,
                            const QString &definition,
                            QString *error)
{
    QSqlQuery q(db_);
    if (!q.exec(QStringLiteral("PRAGMA table_info(%1)").arg(table)))
        return setError(error, q.lastError().text());

    while (q.next()) {
        if (q.value(1).toString() == column)
            return true;
    }

    QSqlQuery alter(db_);
    if (!alter.exec(QStringLiteral("ALTER TABLE %1 ADD COLUMN %2 %3")
                        .arg(table, column, definition)))
        return setError(error, alter.lastError().text());
    return true;
}

bool Database::addFolder(const QString &path, QString *error)
{
    QSqlQuery q(db_);
    q.prepare(QStringLiteral("INSERT OR IGNORE INTO folders(path) VALUES (?)"));
    q.addBindValue(normalizedFolder(path));
    if (!q.exec())
        return setError(error, q.lastError().text());
    return true;
}

bool Database::removeFolder(const QString &path, bool removeIndexedFiles, QString *error)
{
    const QString root = normalizedFolder(path);
    if (!db_.transaction())
        return setError(error, db_.lastError().text());

    QSqlQuery removeFolderQuery(db_);
    removeFolderQuery.prepare(QStringLiteral("DELETE FROM folders WHERE path = ?"));
    removeFolderQuery.addBindValue(root);
    if (!removeFolderQuery.exec()) {
        db_.rollback();
        return setError(error, removeFolderQuery.lastError().text());
    }

    if (removeIndexedFiles) {
        QSqlQuery removeFiles(db_);
        removeFiles.prepare(QStringLiteral(R"SQL(
            DELETE FROM files
            WHERE (path = ? OR substr(path, 1, length(?) + 1) = ? || '/')
              AND NOT EXISTS (
                  SELECT 1 FROM folders w
                  WHERE files.path = w.path
                     OR substr(files.path, 1, length(w.path) + 1) = w.path || '/'
              )
        )SQL"));
        removeFiles.addBindValue(root);
        removeFiles.addBindValue(root);
        removeFiles.addBindValue(root);
        if (!removeFiles.exec()) {
            db_.rollback();
            return setError(error, removeFiles.lastError().text());
        }

        if (!cleanupOrphanPapers(error)) {
            db_.rollback();
            return false;
        }
    }

    if (!db_.commit())
        return setError(error, db_.lastError().text());
    return true;
}

QStringList Database::folders(QString *error) const
{
    QStringList result;
    QSqlQuery q(db_);
    if (!q.exec(QStringLiteral("SELECT path FROM folders ORDER BY path COLLATE NOCASE"))) {
        setError(error, q.lastError().text());
        return result;
    }
    while (q.next())
        result << q.value(0).toString();
    return result;
}

bool Database::markFolderUnseen(const QString &folder, QString *error)
{
    const QString root = normalizedFolder(folder);
    QSqlQuery q(db_);
    q.prepare(QStringLiteral(R"SQL(
        UPDATE files SET seen = 0
        WHERE path = ? OR substr(path, 1, length(?) + 1) = ? || '/'
    )SQL"));
    q.addBindValue(root);
    q.addBindValue(root);
    q.addBindValue(root);
    if (!q.exec())
        return setError(error, q.lastError().text());
    return true;
}

bool Database::removeUnseenInFolder(const QString &folder, QString *error)
{
    const QString root = normalizedFolder(folder);
    QSqlQuery q(db_);
    q.prepare(QStringLiteral(R"SQL(
        DELETE FROM files
        WHERE seen = 0
          AND (path = ? OR substr(path, 1, length(?) + 1) = ? || '/')
    )SQL"));
    q.addBindValue(root);
    q.addBindValue(root);
    q.addBindValue(root);
    if (!q.exec())
        return setError(error, q.lastError().text());
    return cleanupOrphanPapers(error);
}

bool Database::upsertFile(const QString &path,
                          const QString &arxivId,
                          qint64 size,
                          qint64 mtime,
                          QString *error)
{
    QSqlQuery paper(db_);
    paper.prepare(QStringLiteral("INSERT OR IGNORE INTO papers(arxiv_id) VALUES (?)"));
    paper.addBindValue(arxivId);
    if (!paper.exec())
        return setError(error, paper.lastError().text());

    QSqlQuery q(db_);
    q.prepare(QStringLiteral(R"SQL(
        INSERT INTO files(path, arxiv_id, size, mtime, seen, arxiv_version)
        VALUES (?, ?, ?, ?, 1, ?)
        ON CONFLICT(path) DO UPDATE SET
            arxiv_id = excluded.arxiv_id,
            size = excluded.size,
            mtime = excluded.mtime,
            seen = 1,
            arxiv_version = excluded.arxiv_version
    )SQL"));
    q.addBindValue(QDir::cleanPath(path));
    q.addBindValue(arxivId);
    q.addBindValue(size);
    q.addBindValue(mtime);
    q.addBindValue(versionFromPath(path));
    if (!q.exec())
        return setError(error, q.lastError().text());
    return true;
}

bool Database::removeFile(const QString &path, QString *error)
{
    QSqlQuery q(db_);
    q.prepare(QStringLiteral("DELETE FROM files WHERE path = ?"));
    q.addBindValue(QDir::cleanPath(path));
    if (!q.exec())
        return setError(error, q.lastError().text());
    return cleanupOrphanPapers(error);
}

bool Database::updateFilePath(const QString &oldPath,
                              const QString &newPath,
                              qint64 size,
                              qint64 mtime,
                              QString *error)
{
    QSqlQuery q(db_);
    q.prepare(QStringLiteral(R"SQL(
        UPDATE files
        SET path = ?, size = ?, mtime = ?, arxiv_version = ?, seen = 1
        WHERE path = ?
    )SQL"));
    q.addBindValue(QDir::cleanPath(newPath));
    q.addBindValue(size);
    q.addBindValue(mtime);
    q.addBindValue(versionFromPath(newPath));
    q.addBindValue(QDir::cleanPath(oldPath));
    if (!q.exec())
        return setError(error, q.lastError().text());
    if (q.numRowsAffected() == 0)
        return setError(error, QStringLiteral("The source file is not present in the HEPShelf index."));
    return true;
}

bool Database::cleanupOrphanPapers(QString *error)
{
    QSqlQuery cleanup(db_);
    if (!cleanup.exec(QStringLiteral(
            "DELETE FROM papers WHERE NOT EXISTS "
            "(SELECT 1 FROM files WHERE files.arxiv_id = papers.arxiv_id)")))
        return setError(error, cleanup.lastError().text());

    QSqlQuery authorCleanup(db_);
    authorCleanup.exec(QStringLiteral(
        "DELETE FROM authors WHERE NOT EXISTS "
        "(SELECT 1 FROM paper_authors WHERE paper_authors.author_id = authors.id)"));
    return true;
}

QStringList Database::idsMissingMetadata(QString *error) const
{
    QStringList ids;
    QSqlQuery q(db_);
    if (!q.exec(QStringLiteral(R"SQL(
        SELECT arxiv_id
        FROM papers
        WHERE title IS NULL OR trim(title) = ''
        ORDER BY arxiv_id
    )SQL"))) {
        setError(error, q.lastError().text());
        return ids;
    }
    while (q.next())
        ids << q.value(0).toString();
    return ids;
}

bool Database::upsertPaper(const PaperRecord &paper, QString *error)
{
    if (!db_.transaction())
        return setError(error, db_.lastError().text());

    QSqlQuery q(db_);
    q.prepare(QStringLiteral(R"SQL(
        INSERT INTO papers(arxiv_id, title, authors, abstract, published, updated,
                           primary_category, categories, doi, journal_ref, comments, fetched_at)
        VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, CURRENT_TIMESTAMP)
        ON CONFLICT(arxiv_id) DO UPDATE SET
            title = excluded.title,
            authors = excluded.authors,
            abstract = excluded.abstract,
            published = excluded.published,
            updated = excluded.updated,
            primary_category = excluded.primary_category,
            categories = excluded.categories,
            doi = excluded.doi,
            journal_ref = excluded.journal_ref,
            comments = excluded.comments,
            fetched_at = CURRENT_TIMESTAMP
    )SQL"));
    q.addBindValue(paper.arxivId);
    q.addBindValue(paper.title);
    q.addBindValue(paper.authors);
    q.addBindValue(paper.abstractText);
    q.addBindValue(paper.published);
    q.addBindValue(paper.updated);
    q.addBindValue(paper.primaryCategory);
    q.addBindValue(paper.categories);
    q.addBindValue(paper.doi);
    q.addBindValue(paper.journalRef);
    q.addBindValue(paper.comments);
    if (!q.exec()) {
        db_.rollback();
        return setError(error, q.lastError().text());
    }

    QStringList authors = paper.authorNames;
    if (authors.isEmpty())
        authors = conservativeAuthorSplit(paper.authors);

    if (!authors.isEmpty()) {
        QSqlQuery clear(db_);
        clear.prepare(QStringLiteral("DELETE FROM paper_authors WHERE arxiv_id = ?"));
        clear.addBindValue(paper.arxivId);
        if (!clear.exec()) {
            db_.rollback();
            return setError(error, clear.lastError().text());
        }

        int position = 0;
        for (QString authorName : authors) {
            authorName = authorName.simplified();
            if (authorName.isEmpty())
                continue;

            QSqlQuery insertAuthor(db_);
            insertAuthor.prepare(QStringLiteral("INSERT OR IGNORE INTO authors(canonical_name) VALUES (?)"));
            insertAuthor.addBindValue(authorName);
            if (!insertAuthor.exec()) {
                db_.rollback();
                return setError(error, insertAuthor.lastError().text());
            }

            QSqlQuery lookup(db_);
            lookup.prepare(QStringLiteral("SELECT id FROM authors WHERE canonical_name = ? COLLATE NOCASE"));
            lookup.addBindValue(authorName);
            if (!lookup.exec() || !lookup.next()) {
                db_.rollback();
                return setError(error, lookup.lastError().text());
            }

            QSqlQuery link(db_);
            link.prepare(QStringLiteral("INSERT OR REPLACE INTO paper_authors(arxiv_id, author_id, position) VALUES (?, ?, ?)"));
            link.addBindValue(paper.arxivId);
            link.addBindValue(lookup.value(0));
            link.addBindValue(position++);
            if (!link.exec()) {
                db_.rollback();
                return setError(error, link.lastError().text());
            }
        }
    }

    if (!db_.commit())
        return setError(error, db_.lastError().text());
    return true;
}

bool Database::updateCitationMetrics(const QString &arxivId,
                                     const CitationMetrics &metrics,
                                     QString *error)
{
    QSqlQuery q(db_);
    q.prepare(QStringLiteral(R"SQL(
        UPDATE papers
        SET inspire_recid = ?, citation_count = ?, citation_count_no_self = ?,
            reference_count = ?, inspire_fetched_at = CURRENT_TIMESTAMP
        WHERE arxiv_id = ?
    )SQL"));
    q.addBindValue(metrics.inspireRecid > 0 ? QVariant(metrics.inspireRecid) : QVariant());
    q.addBindValue(metrics.citationCount >= 0 ? QVariant(metrics.citationCount) : QVariant());
    q.addBindValue(metrics.citationCountWithoutSelf >= 0 ? QVariant(metrics.citationCountWithoutSelf) : QVariant());
    q.addBindValue(metrics.referenceCount >= 0 ? QVariant(metrics.referenceCount) : QVariant());
    q.addBindValue(arxivId);
    if (!q.exec())
        return setError(error, q.lastError().text());
    return true;
}

bool Database::replaceReferences(const QString &sourceArxivId,
                                 const QList<ReferenceRecord> &references,
                                 QString *error)
{
    if (!db_.transaction())
        return setError(error, db_.lastError().text());

    QSqlQuery clear(db_);
    clear.prepare(QStringLiteral("DELETE FROM paper_references WHERE source_arxiv_id = ?"));
    clear.addBindValue(sourceArxivId);
    if (!clear.exec()) {
        db_.rollback();
        return setError(error, clear.lastError().text());
    }

    QSqlQuery insert(db_);
    insert.prepare(QStringLiteral(R"SQL(
        INSERT INTO paper_references(source_arxiv_id, position, inspire_recid,
                                     target_arxiv_id, title, authors, doi, raw_text)
        VALUES (?, ?, ?, ?, ?, ?, ?, ?)
    )SQL"));
    for (const ReferenceRecord &ref : references) {
        insert.bindValue(0, sourceArxivId);
        insert.bindValue(1, ref.position);
        insert.bindValue(2, ref.inspireRecid > 0 ? QVariant(ref.inspireRecid) : QVariant());
        insert.bindValue(3, ref.arxivId);
        insert.bindValue(4, ref.title);
        insert.bindValue(5, ref.authors);
        insert.bindValue(6, ref.doi);
        insert.bindValue(7, ref.rawText);
        if (!insert.exec()) {
            db_.rollback();
            return setError(error, insert.lastError().text());
        }
    }

    if (!db_.commit())
        return setError(error, db_.lastError().text());
    return true;
}

QList<ReferenceRecord> Database::referencesForPaper(const QString &sourceArxivId,
                                                    QString *error) const
{
    QList<ReferenceRecord> result;
    QSqlQuery q(db_);
    q.prepare(QStringLiteral(R"SQL(
        SELECT r.position, COALESCE(r.inspire_recid, 0), COALESCE(r.target_arxiv_id, ''),
               COALESCE(r.title, ''), COALESCE(r.authors, ''), COALESCE(r.doi, ''),
               COALESCE(r.raw_text, ''),
               COALESCE(
                   (SELECT MIN(f.path) FROM files f
                    WHERE trim(COALESCE(r.target_arxiv_id, '')) <> ''
                      AND f.arxiv_id = r.target_arxiv_id),
                   (SELECT MIN(f.path)
                    FROM files f JOIN papers p ON p.arxiv_id = f.arxiv_id
                    WHERE trim(COALESCE(r.doi, '')) <> ''
                      AND lower(trim(COALESCE(p.doi, ''))) = lower(trim(r.doi))),
                   (SELECT MIN(f.path)
                    FROM files f JOIN papers p ON p.arxiv_id = f.arxiv_id
                    WHERE COALESCE(r.inspire_recid, 0) > 0
                      AND p.inspire_recid = r.inspire_recid),
                   '')
        FROM paper_references r
        WHERE r.source_arxiv_id = ?
        ORDER BY r.position
    )SQL"));
    q.addBindValue(sourceArxivId);
    if (!q.exec()) {
        setError(error, q.lastError().text());
        return result;
    }
    while (q.next()) {
        ReferenceRecord ref;
        ref.position = q.value(0).toInt();
        ref.inspireRecid = q.value(1).toInt();
        ref.arxivId = q.value(2).toString();
        ref.title = q.value(3).toString();
        ref.authors = q.value(4).toString();
        ref.doi = q.value(5).toString();
        ref.rawText = q.value(6).toString();
        ref.localPath = q.value(7).toString();
        ref.local = !ref.localPath.isEmpty();
        result << ref;
    }
    return result;
}

bool Database::replaceCitingPapers(const QString &sourceArxivId,
                                   const QList<RelatedPaperRecord> &papers,
                                   int totalCount,
                                   QString *error)
{
    if (!db_.transaction())
        return setError(error, db_.lastError().text());

    QSqlQuery clear(db_);
    clear.prepare(QStringLiteral("DELETE FROM paper_citing_papers WHERE source_arxiv_id = ?"));
    clear.addBindValue(sourceArxivId);
    if (!clear.exec()) {
        db_.rollback();
        return setError(error, clear.lastError().text());
    }

    QSqlQuery insert(db_);
    insert.prepare(QStringLiteral(R"SQL(
        INSERT INTO paper_citing_papers(source_arxiv_id, position, inspire_recid,
                                        target_arxiv_id, title, authors, doi, year,
                                        citation_count)
        VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?)
    )SQL"));
    int position = 1;
    for (const RelatedPaperRecord &paper : papers) {
        insert.bindValue(0, sourceArxivId);
        insert.bindValue(1, position++);
        insert.bindValue(2, paper.inspireRecid > 0 ? QVariant(paper.inspireRecid) : QVariant());
        insert.bindValue(3, paper.arxivId);
        insert.bindValue(4, paper.title);
        insert.bindValue(5, paper.authors);
        insert.bindValue(6, paper.doi);
        insert.bindValue(7, paper.year);
        insert.bindValue(8, paper.citationCount >= 0 ? QVariant(paper.citationCount) : QVariant());
        if (!insert.exec()) {
            db_.rollback();
            return setError(error, insert.lastError().text());
        }
    }

    QSqlQuery cache(db_);
    cache.prepare(QStringLiteral(R"SQL(
        INSERT INTO paper_citing_cache(source_arxiv_id, total_count, fetched_at)
        VALUES (?, ?, CURRENT_TIMESTAMP)
        ON CONFLICT(source_arxiv_id) DO UPDATE SET
            total_count = excluded.total_count,
            fetched_at = CURRENT_TIMESTAMP
    )SQL"));
    cache.addBindValue(sourceArxivId);
    cache.addBindValue(qMax(0, totalCount));
    if (!cache.exec()) {
        db_.rollback();
        return setError(error, cache.lastError().text());
    }

    if (!db_.commit())
        return setError(error, db_.lastError().text());
    return true;
}

QList<RelatedPaperRecord> Database::citingPapersForPaper(const QString &sourceArxivId,
                                                         QString *error) const
{
    QList<RelatedPaperRecord> result;
    QSqlQuery q(db_);
    q.prepare(QStringLiteral(R"SQL(
        SELECT c.inspire_recid, COALESCE(c.target_arxiv_id, ''), COALESCE(c.title, ''),
               COALESCE(c.authors, ''), COALESCE(c.doi, ''), COALESCE(c.year, ''),
               COALESCE(c.citation_count, -1),
               COALESCE(
                   (SELECT MIN(f.path) FROM files f
                    WHERE trim(COALESCE(c.target_arxiv_id, '')) <> ''
                      AND f.arxiv_id = c.target_arxiv_id),
                   (SELECT MIN(f.path)
                    FROM files f JOIN papers p ON p.arxiv_id = f.arxiv_id
                    WHERE trim(COALESCE(c.doi, '')) <> ''
                      AND lower(trim(COALESCE(p.doi, ''))) = lower(trim(c.doi))),
                   (SELECT MIN(f.path)
                    FROM files f JOIN papers p ON p.arxiv_id = f.arxiv_id
                    WHERE COALESCE(c.inspire_recid, 0) > 0
                      AND p.inspire_recid = c.inspire_recid),
                   '')
        FROM paper_citing_papers c
        WHERE c.source_arxiv_id = ?
        ORDER BY c.position
    )SQL"));
    q.addBindValue(sourceArxivId);
    if (!q.exec()) {
        setError(error, q.lastError().text());
        return result;
    }
    while (q.next()) {
        RelatedPaperRecord paper;
        paper.inspireRecid = q.value(0).toInt();
        paper.arxivId = q.value(1).toString();
        paper.title = q.value(2).toString();
        paper.authors = q.value(3).toString();
        paper.doi = q.value(4).toString();
        paper.year = q.value(5).toString();
        paper.citationCount = q.value(6).toInt();
        paper.localPath = q.value(7).toString();
        paper.local = !paper.localPath.isEmpty();
        result << paper;
    }
    return result;
}

CitingCacheInfo Database::citingCacheInfo(const QString &sourceArxivId,
                                          QString *error) const
{
    CitingCacheInfo info;
    QSqlQuery q(db_);
    q.prepare(QStringLiteral("SELECT total_count, fetched_at FROM paper_citing_cache WHERE source_arxiv_id = ?"));
    q.addBindValue(sourceArxivId);
    if (!q.exec()) {
        setError(error, q.lastError().text());
        return info;
    }
    if (q.next()) {
        info.totalCount = q.value(0).toInt();
        info.fetchedAt = q.value(1).toString();
    }
    return info;
}

bool Database::hasLocalFile(const QString &arxivId, QString *path) const
{
    QSqlQuery q(db_);
    q.prepare(QStringLiteral("SELECT path FROM files WHERE arxiv_id = ? ORDER BY path LIMIT 1"));
    q.addBindValue(arxivId);
    if (!q.exec() || !q.next())
        return false;
    if (path)
        *path = q.value(0).toString();
    return true;
}

bool Database::resolveLocalPaper(const QString &arxivId,
                                 const QString &doi,
                                 int inspireRecid,
                                 QString *resolvedArxivId,
                                 QString *path) const
{
    QSqlQuery q(db_);
    q.prepare(QStringLiteral(R"SQL(
        SELECT p.arxiv_id, MIN(f.path),
               CASE
                   WHEN ? <> '' AND p.arxiv_id = ? THEN 0
                   WHEN ? > 0 AND p.inspire_recid = ? THEN 1
                   WHEN ? <> '' AND lower(trim(COALESCE(p.doi, ''))) = lower(trim(?)) THEN 2
                   ELSE 3
               END AS match_rank
        FROM papers p
        JOIN files f ON f.arxiv_id = p.arxiv_id
        WHERE (? <> '' AND p.arxiv_id = ?)
           OR (? > 0 AND p.inspire_recid = ?)
           OR (? <> '' AND lower(trim(COALESCE(p.doi, ''))) = lower(trim(?)))
        GROUP BY p.arxiv_id
        ORDER BY match_rank, p.arxiv_id
        LIMIT 1
    )SQL"));
    q.addBindValue(arxivId.trimmed());
    q.addBindValue(arxivId.trimmed());
    q.addBindValue(inspireRecid);
    q.addBindValue(inspireRecid);
    q.addBindValue(doi.trimmed());
    q.addBindValue(doi.trimmed());
    q.addBindValue(arxivId.trimmed());
    q.addBindValue(arxivId.trimmed());
    q.addBindValue(inspireRecid);
    q.addBindValue(inspireRecid);
    q.addBindValue(doi.trimmed());
    q.addBindValue(doi.trimmed());
    if (!q.exec() || !q.next())
        return false;
    if (resolvedArxivId)
        *resolvedArxivId = q.value(0).toString();
    if (path)
        *path = q.value(1).toString();
    return true;
}

QString Database::arxivIdForPath(const QString &path) const
{
    QSqlQuery q(db_);
    q.prepare(QStringLiteral("SELECT arxiv_id FROM files WHERE path = ? LIMIT 1"));
    q.addBindValue(path);
    if (!q.exec() || !q.next())
        return {};
    return q.value(0).toString();
}

bool Database::rebuildAuthorIndex(QString *error)
{
    QSqlQuery q(db_);
    if (!q.exec(QStringLiteral(R"SQL(
        SELECT p.arxiv_id, COALESCE(p.authors, '')
        FROM papers p
        WHERE trim(COALESCE(p.authors, '')) <> ''
          AND NOT EXISTS (SELECT 1 FROM paper_authors pa WHERE pa.arxiv_id = p.arxiv_id)
    )SQL")))
        return setError(error, q.lastError().text());

    QList<QPair<QString, QStringList>> pending;
    while (q.next())
        pending.append({q.value(0).toString(), conservativeAuthorSplit(q.value(1).toString())});

    for (const auto &entry : pending) {
        int position = 0;
        for (const QString &name : entry.second) {
            QSqlQuery insert(db_);
            insert.prepare(QStringLiteral("INSERT OR IGNORE INTO authors(canonical_name) VALUES (?)"));
            insert.addBindValue(name);
            if (!insert.exec())
                return setError(error, insert.lastError().text());

            QSqlQuery link(db_);
            link.prepare(QStringLiteral(R"SQL(
                INSERT OR IGNORE INTO paper_authors(arxiv_id, author_id, position)
                SELECT ?, id, ? FROM authors WHERE canonical_name = ? COLLATE NOCASE
            )SQL"));
            link.addBindValue(entry.first);
            link.addBindValue(position++);
            link.addBindValue(name);
            if (!link.exec())
                return setError(error, link.lastError().text());
        }
    }
    return true;
}

QList<LibraryRow> Database::search(const QString &query,
                                   LibraryFilter filter,
                                   const QString &facetValue,
                                   QString *error) const
{
    QList<LibraryRow> rows;
    QSqlQuery q(db_);
    const QString trimmed = query.trimmed();
    const QString needle = QStringLiteral("%%1%").arg(trimmed);

    QString filterSql;
    QString orderSql = QStringLiteral(
        "CASE WHEN p.title IS NULL OR trim(p.title) = '' THEN 1 ELSE 0 END, "
        "display_title COLLATE NOCASE");
    QVariantList extraBinds;

    switch (filter) {
    case LibraryFilter::Favorites:
        filterSql = QStringLiteral(" AND p.favorite = 1 ");
        break;
    case LibraryFilter::Recent:
        filterSql = QStringLiteral(" AND p.last_opened IS NOT NULL AND trim(p.last_opened) <> '' ");
        orderSql = QStringLiteral("p.last_opened DESC");
        break;
    case LibraryFilter::MissingMetadata:
        filterSql = QStringLiteral(" AND (p.title IS NULL OR trim(p.title) = '') ");
        break;
    case LibraryFilter::Unread:
        filterSql = QStringLiteral(" AND COALESCE(p.open_count, 0) = 0 ");
        break;
    case LibraryFilter::Duplicates:
        filterSql = QStringLiteral(" AND (SELECT COUNT(*) FROM files ff WHERE ff.arxiv_id = p.arxiv_id) > 1 ");
        break;
    case LibraryFilter::Tag:
        filterSql = QStringLiteral(R"SQL(
            AND EXISTS (
                SELECT 1 FROM paper_tags pt JOIN tags t ON t.id = pt.tag_id
                WHERE pt.arxiv_id = p.arxiv_id AND t.name = ? COLLATE NOCASE
            )
        )SQL");
        extraBinds << facetValue;
        break;
    case LibraryFilter::Collection:
        filterSql = QStringLiteral(R"SQL(
            AND EXISTS (
                SELECT 1 FROM collection_papers cp JOIN collections c ON c.id = cp.collection_id
                WHERE cp.arxiv_id = p.arxiv_id AND c.name = ? COLLATE NOCASE
            )
        )SQL");
        extraBinds << facetValue;
        break;
    case LibraryFilter::Author:
        filterSql = QStringLiteral(R"SQL(
            AND EXISTS (
                SELECT 1 FROM paper_authors pa JOIN authors a ON a.id = pa.author_id
                WHERE pa.arxiv_id = p.arxiv_id AND a.canonical_name = ? COLLATE NOCASE
            )
        )SQL");
        extraBinds << facetValue;
        break;
    case LibraryFilter::Category:
        filterSql = QStringLiteral(" AND p.primary_category = ? COLLATE NOCASE ");
        extraBinds << facetValue;
        break;
    case LibraryFilter::Trail:
        filterSql = QStringLiteral(R"SQL(
            AND EXISTS (
                SELECT 1 FROM literature_trail_items lti
                JOIN literature_trails lt ON lt.id = lti.trail_id
                WHERE lt.name = ? COLLATE NOCASE
                  AND (lower(COALESCE(lti.arxiv_id, '')) = lower(p.arxiv_id)
                       OR (COALESCE(lti.inspire_recid, 0) > 0 AND lti.inspire_recid = COALESCE(p.inspire_recid, 0))
                       OR (trim(COALESCE(lti.doi, '')) <> '' AND lower(trim(lti.doi)) = lower(trim(COALESCE(p.doi, '')))))
            )
        )SQL");
        extraBinds << facetValue;
        break;
    case LibraryFilter::All:
    default:
        break;
    }

    QString sql = QStringLiteral(R"SQL(
        SELECT p.arxiv_id,
               COALESCE(NULLIF(trim(p.title), ''), p.arxiv_id) AS display_title,
               COALESCE(p.authors, ''),
               CASE WHEN length(p.published) >= 4 THEN substr(p.published, 1, 4) ELSE '' END,
               COALESCE(NULLIF(trim(p.primary_category), ''), ''),
               COUNT(f.id),
               MIN(f.path),
               COALESCE(p.favorite, 0),
               COALESCE(p.last_opened, ''),
               COALESCE(p.last_page, 0),
               COALESCE(p.page_count, 0),
               COALESCE(p.citation_count, -1),
               COALESCE(p.reference_count, -1)
        FROM papers p
        JOIN files f ON f.arxiv_id = p.arxiv_id
        WHERE ((? = '')
           OR p.arxiv_id LIKE ? COLLATE NOCASE
           OR p.title LIKE ? COLLATE NOCASE
           OR p.authors LIKE ? COLLATE NOCASE
           OR p.primary_category LIKE ? COLLATE NOCASE
           OR p.doi LIKE ? COLLATE NOCASE
           OR f.path LIKE ? COLLATE NOCASE
           OR EXISTS (SELECT 1 FROM paper_tags pt JOIN tags t ON t.id = pt.tag_id
                      WHERE pt.arxiv_id = p.arxiv_id AND t.name LIKE ? COLLATE NOCASE)
           OR EXISTS (SELECT 1 FROM collection_papers cp JOIN collections c ON c.id = cp.collection_id
                      WHERE cp.arxiv_id = p.arxiv_id AND c.name LIKE ? COLLATE NOCASE)
           OR EXISTS (SELECT 1 FROM paper_authors pa JOIN authors a ON a.id = pa.author_id
                      WHERE pa.arxiv_id = p.arxiv_id AND a.canonical_name LIKE ? COLLATE NOCASE)
           OR EXISTS (SELECT 1 FROM paper_notes pn
                      WHERE pn.arxiv_id = p.arxiv_id AND pn.note LIKE ? COLLATE NOCASE)
           OR EXISTS (SELECT 1 FROM literature_trail_items lti JOIN literature_trails lt ON lt.id = lti.trail_id
                      WHERE lower(COALESCE(lti.arxiv_id, '')) = lower(p.arxiv_id)
                        AND (lt.name LIKE ? COLLATE NOCASE OR lti.note LIKE ? COLLATE NOCASE)))
    )SQL") + filterSql + QStringLiteral(R"SQL(
        GROUP BY p.arxiv_id, p.title, p.authors, p.published, p.primary_category,
                 p.favorite, p.last_opened, p.last_page, p.page_count, p.citation_count, p.reference_count
        ORDER BY )SQL") + orderSql;

    q.prepare(sql);
    q.addBindValue(trimmed);
    for (int i = 0; i < 12; ++i)
        q.addBindValue(needle);
    for (const QVariant &value : extraBinds)
        q.addBindValue(value);

    if (!q.exec()) {
        setError(error, q.lastError().text());
        return rows;
    }

    while (q.next()) {
        LibraryRow row;
        row.arxivId = q.value(0).toString();
        row.title = q.value(1).toString();
        row.authors = q.value(2).toString();
        row.year = q.value(3).toString();
        row.category = q.value(4).toString();
        row.fileCount = q.value(5).toInt();
        row.path = q.value(6).toString();
        row.favorite = q.value(7).toInt() != 0;
        row.lastOpened = q.value(8).toString();
        row.lastPage = q.value(9).toInt();
        row.pageCount = q.value(10).toInt();
        row.citationCount = q.value(11).toInt();
        row.referenceCount = q.value(12).toInt();
        rows.push_back(row);
    }
    return rows;
}

bool Database::paperDetails(const QString &arxivId, PaperDetails *details, QString *error) const
{
    if (!details)
        return setError(error, QStringLiteral("Internal error: null paper-details destination."));

    QSqlQuery q(db_);
    q.prepare(QStringLiteral(R"SQL(
        SELECT arxiv_id, COALESCE(title, ''), COALESCE(authors, ''),
               COALESCE(abstract, ''), COALESCE(published, ''), COALESCE(updated, ''),
               COALESCE(primary_category, ''), COALESCE(categories, ''),
               COALESCE(doi, ''), COALESCE(journal_ref, ''), COALESCE(comments, ''),
               COALESCE(favorite, 0), COALESCE(last_opened, ''), COALESCE(open_count, 0),
               COALESCE(last_page, 0), COALESCE(page_count, 0), COALESCE(fetched_at, ''),
               COALESCE(inspire_recid, 0), COALESCE(citation_count, -1),
               COALESCE(citation_count_no_self, -1), COALESCE(reference_count, -1),
               COALESCE(inspire_fetched_at, '')
        FROM papers WHERE arxiv_id = ?
    )SQL"));
    q.addBindValue(arxivId);
    if (!q.exec())
        return setError(error, q.lastError().text());
    if (!q.next())
        return setError(error, QStringLiteral("Paper %1 is not in the library.").arg(arxivId));

    PaperDetails result;
    result.paper.arxivId = q.value(0).toString();
    result.paper.title = q.value(1).toString();
    result.paper.authors = q.value(2).toString();
    result.paper.abstractText = q.value(3).toString();
    result.paper.published = q.value(4).toString();
    result.paper.updated = q.value(5).toString();
    result.paper.primaryCategory = q.value(6).toString();
    result.paper.categories = q.value(7).toString();
    result.paper.doi = q.value(8).toString();
    result.paper.journalRef = q.value(9).toString();
    result.paper.comments = q.value(10).toString();
    result.favorite = q.value(11).toInt() != 0;
    result.lastOpened = q.value(12).toString();
    result.openCount = q.value(13).toInt();
    result.lastPage = q.value(14).toInt();
    result.pageCount = q.value(15).toInt();
    result.fetchedAt = q.value(16).toString();
    result.citations.inspireRecid = q.value(17).toInt();
    result.citations.citationCount = q.value(18).toInt();
    result.citations.citationCountWithoutSelf = q.value(19).toInt();
    result.citations.referenceCount = q.value(20).toInt();
    result.citations.fetchedAt = q.value(21).toString();

    QSqlQuery files(db_);
    files.prepare(QStringLiteral(R"SQL(
        SELECT path, COALESCE(arxiv_version, ''), size, mtime
        FROM files WHERE arxiv_id = ?
        ORDER BY CASE WHEN arxiv_version GLOB 'v[0-9]*' THEN CAST(substr(arxiv_version, 2) AS INTEGER) ELSE 0 END DESC,
                 path COLLATE NOCASE
    )SQL"));
    files.addBindValue(arxivId);
    if (!files.exec())
        return setError(error, files.lastError().text());
    while (files.next()) {
        FileRecord file;
        file.path = files.value(0).toString();
        file.version = files.value(1).toString();
        file.size = files.value(2).toLongLong();
        file.mtime = files.value(3).toLongLong();
        result.files << file;
        result.paths << file.path;
    }

    result.tags = tagsForPaper(arxivId, error);
    if (error && !error->isEmpty())
        return false;
    result.collections = collectionsForPaper(arxivId, error);
    if (error && !error->isEmpty())
        return false;

    QSqlQuery authors(db_);
    authors.prepare(QStringLiteral(R"SQL(
        SELECT a.canonical_name
        FROM paper_authors pa JOIN authors a ON a.id = pa.author_id
        WHERE pa.arxiv_id = ? ORDER BY pa.position, a.canonical_name COLLATE NOCASE
    )SQL"));
    authors.addBindValue(arxivId);
    if (!authors.exec())
        return setError(error, authors.lastError().text());
    while (authors.next())
        result.indexedAuthors << authors.value(0).toString();
    result.paper.authorNames = result.indexedAuthors;

    *details = result;
    return true;
}

bool Database::setFavorite(const QString &arxivId, bool favorite, QString *error)
{
    QSqlQuery q(db_);
    q.prepare(QStringLiteral("UPDATE papers SET favorite = ? WHERE arxiv_id = ?"));
    q.addBindValue(favorite ? 1 : 0);
    q.addBindValue(arxivId);
    if (!q.exec())
        return setError(error, q.lastError().text());
    return true;
}

bool Database::recordOpened(const QString &arxivId, QString *error)
{
    QSqlQuery q(db_);
    q.prepare(QStringLiteral(R"SQL(
        UPDATE papers
        SET last_opened = CURRENT_TIMESTAMP,
            open_count = COALESCE(open_count, 0) + 1
        WHERE arxiv_id = ?
    )SQL"));
    q.addBindValue(arxivId);
    if (!q.exec())
        return setError(error, q.lastError().text());
    return true;
}

bool Database::setReadingProgress(const QString &arxivId,
                                  int lastPage,
                                  int pageCount,
                                  QString *error)
{
    QSqlQuery q(db_);
    q.prepare(QStringLiteral(R"SQL(
        UPDATE papers SET last_page = ?, page_count = ? WHERE arxiv_id = ?
    )SQL"));
    q.addBindValue(qMax(0, lastPage));
    q.addBindValue(qMax(0, pageCount));
    q.addBindValue(arxivId);
    if (!q.exec())
        return setError(error, q.lastError().text());
    return true;
}

PaperNoteRecord Database::paperNote(const QString &arxivId, QString *error) const
{
    PaperNoteRecord result;
    QSqlQuery q(db_);
    q.prepare(QStringLiteral("SELECT note, updated_at FROM paper_notes WHERE arxiv_id = ?"));
    q.addBindValue(arxivId);
    if (!q.exec()) {
        setError(error, q.lastError().text());
        return result;
    }
    if (q.next()) {
        result.text = q.value(0).toString();
        result.updatedAt = q.value(1).toString();
    }
    return result;
}

bool Database::setPaperNote(const QString &arxivId, const QString &text, QString *error)
{
    if (arxivId.trimmed().isEmpty())
        return setError(error, QStringLiteral("Cannot save a note without a paper identifier."));

    QSqlQuery q(db_);
    if (text.trimmed().isEmpty()) {
        q.prepare(QStringLiteral("DELETE FROM paper_notes WHERE arxiv_id = ?"));
        q.addBindValue(arxivId);
    } else {
        q.prepare(QStringLiteral(R"SQL(
            INSERT INTO paper_notes(arxiv_id, note, updated_at)
            VALUES (?, ?, CURRENT_TIMESTAMP)
            ON CONFLICT(arxiv_id) DO UPDATE SET
                note = excluded.note,
                updated_at = CURRENT_TIMESTAMP
        )SQL"));
        q.addBindValue(arxivId);
        q.addBindValue(text);
    }
    if (!q.exec())
        return setError(error, q.lastError().text());
    return true;
}

QList<LiteratureTrailRecord> Database::literatureTrails(QString *error) const
{
    QList<LiteratureTrailRecord> result;
    QSqlQuery q(db_);
    if (!q.exec(QStringLiteral(R"SQL(
        SELECT t.id, t.name, t.description, COUNT(i.id), t.updated_at
        FROM literature_trails t
        LEFT JOIN literature_trail_items i ON i.trail_id = t.id
        GROUP BY t.id, t.name, t.description, t.updated_at
        ORDER BY t.updated_at DESC, t.name COLLATE NOCASE
    )SQL"))) {
        setError(error, q.lastError().text());
        return result;
    }
    while (q.next()) {
        LiteratureTrailRecord trail;
        trail.id = q.value(0).toInt();
        trail.name = q.value(1).toString();
        trail.description = q.value(2).toString();
        trail.itemCount = q.value(3).toInt();
        trail.updatedAt = q.value(4).toString();
        result << trail;
    }
    return result;
}

QList<LiteratureTrailRecord> Database::literatureTrailsForPaper(const QString &arxivId, QString *error) const
{
    QList<LiteratureTrailRecord> result;
    QSqlQuery q(db_);
    q.prepare(QStringLiteral(R"SQL(
        SELECT DISTINCT t.id, t.name, t.description,
               (SELECT COUNT(*) FROM literature_trail_items x WHERE x.trail_id = t.id),
               t.updated_at
        FROM literature_trails t
        JOIN literature_trail_items i ON i.trail_id = t.id
        JOIN papers p ON p.arxiv_id = ?
        WHERE (lower(COALESCE(i.arxiv_id, '')) = lower(p.arxiv_id))
           OR (COALESCE(i.inspire_recid, 0) > 0 AND i.inspire_recid = COALESCE(p.inspire_recid, 0))
           OR (trim(COALESCE(i.doi, '')) <> '' AND lower(trim(i.doi)) = lower(trim(COALESCE(p.doi, ''))))
        ORDER BY t.name COLLATE NOCASE
    )SQL"));
    q.addBindValue(arxivId);
    if (!q.exec()) {
        setError(error, q.lastError().text());
        return result;
    }
    while (q.next()) {
        LiteratureTrailRecord trail;
        trail.id = q.value(0).toInt();
        trail.name = q.value(1).toString();
        trail.description = q.value(2).toString();
        trail.itemCount = q.value(3).toInt();
        trail.updatedAt = q.value(4).toString();
        result << trail;
    }
    return result;
}

int Database::createLiteratureTrail(const QString &name, const QString &description, QString *error)
{
    const QString clean = normalizedLabel(name);
    if (clean.isEmpty()) {
        setError(error, QStringLiteral("Trail name cannot be empty."));
        return 0;
    }

    QSqlQuery q(db_);
    q.prepare(QStringLiteral(R"SQL(
        INSERT OR IGNORE INTO literature_trails(name, description, updated_at)
        VALUES (?, ?, CURRENT_TIMESTAMP)
    )SQL"));
    q.addBindValue(clean);
    q.addBindValue(description.trimmed());
    if (!q.exec()) {
        setError(error, q.lastError().text());
        return 0;
    }

    QSqlQuery find(db_);
    find.prepare(QStringLiteral("SELECT id FROM literature_trails WHERE name = ? COLLATE NOCASE"));
    find.addBindValue(clean);
    if (!find.exec() || !find.next()) {
        setError(error, find.lastError().text().isEmpty()
                            ? QStringLiteral("Could not read the created literature trail.")
                            : find.lastError().text());
        return 0;
    }
    return find.value(0).toInt();
}

bool Database::updateLiteratureTrail(int trailId,
                                     const QString &name,
                                     const QString &description,
                                     QString *error)
{
    const QString clean = normalizedLabel(name);
    if (trailId <= 0 || clean.isEmpty())
        return setError(error, QStringLiteral("A trail needs a valid name."));
    QSqlQuery q(db_);
    q.prepare(QStringLiteral(R"SQL(
        UPDATE literature_trails
        SET name = ?, description = ?, updated_at = CURRENT_TIMESTAMP
        WHERE id = ?
    )SQL"));
    q.addBindValue(clean);
    q.addBindValue(description.trimmed());
    q.addBindValue(trailId);
    if (!q.exec())
        return setError(error, q.lastError().text());
    return true;
}

bool Database::deleteLiteratureTrail(int trailId, QString *error)
{
    QSqlQuery q(db_);
    q.prepare(QStringLiteral("DELETE FROM literature_trails WHERE id = ?"));
    q.addBindValue(trailId);
    if (!q.exec())
        return setError(error, q.lastError().text());
    return true;
}

bool Database::addTrailItem(int trailId, const TrailItemRecord &item, QString *error)
{
    if (trailId <= 0)
        return setError(error, QStringLiteral("No literature trail was selected."));
    const QString identity = trailIdentity(item);
    if (identity.isEmpty())
        return setError(error, QStringLiteral("This paper does not contain enough metadata to save in a trail."));

    QSqlQuery pos(db_);
    pos.prepare(QStringLiteral("SELECT COALESCE(MAX(position), 0) + 1 FROM literature_trail_items WHERE trail_id = ?"));
    pos.addBindValue(trailId);
    if (!pos.exec() || !pos.next())
        return setError(error, pos.lastError().text());
    const int nextPosition = pos.value(0).toInt();

    QSqlQuery q(db_);
    q.prepare(QStringLiteral(R"SQL(
        INSERT INTO literature_trail_items(
            trail_id, position, identity_key, arxiv_id, inspire_recid, doi,
            title, authors, year, note
        ) VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?)
        ON CONFLICT(trail_id, identity_key) DO UPDATE SET
            arxiv_id = CASE WHEN trim(COALESCE(excluded.arxiv_id, '')) <> '' THEN excluded.arxiv_id ELSE literature_trail_items.arxiv_id END,
            inspire_recid = CASE WHEN COALESCE(excluded.inspire_recid, 0) > 0 THEN excluded.inspire_recid ELSE literature_trail_items.inspire_recid END,
            doi = CASE WHEN trim(COALESCE(excluded.doi, '')) <> '' THEN excluded.doi ELSE literature_trail_items.doi END,
            title = CASE WHEN trim(COALESCE(excluded.title, '')) <> '' THEN excluded.title ELSE literature_trail_items.title END,
            authors = CASE WHEN trim(COALESCE(excluded.authors, '')) <> '' THEN excluded.authors ELSE literature_trail_items.authors END,
            year = CASE WHEN trim(COALESCE(excluded.year, '')) <> '' THEN excluded.year ELSE literature_trail_items.year END
    )SQL"));
    q.addBindValue(trailId);
    q.addBindValue(nextPosition);
    q.addBindValue(identity);
    q.addBindValue(item.arxivId.trimmed());
    q.addBindValue(item.inspireRecid > 0 ? QVariant(item.inspireRecid) : QVariant());
    q.addBindValue(item.doi.trimmed());
    q.addBindValue(item.title.simplified());
    q.addBindValue(item.authors.simplified());
    q.addBindValue(item.year.trimmed());
    q.addBindValue(item.note);
    if (!q.exec())
        return setError(error, q.lastError().text());

    QSqlQuery touch(db_);
    touch.prepare(QStringLiteral("UPDATE literature_trails SET updated_at = CURRENT_TIMESTAMP WHERE id = ?"));
    touch.addBindValue(trailId);
    if (!touch.exec())
        return setError(error, touch.lastError().text());
    return true;
}

QList<TrailItemRecord> Database::trailItems(int trailId, QString *error) const
{
    QList<TrailItemRecord> result;
    QSqlQuery q(db_);
    q.prepare(QStringLiteral(R"SQL(
        SELECT id, position, COALESCE(arxiv_id, ''), COALESCE(inspire_recid, 0),
               COALESCE(doi, ''), COALESCE(title, ''), COALESCE(authors, ''),
               COALESCE(year, ''), COALESCE(note, '')
        FROM literature_trail_items
        WHERE trail_id = ?
        ORDER BY position, id
    )SQL"));
    q.addBindValue(trailId);
    if (!q.exec()) {
        setError(error, q.lastError().text());
        return result;
    }
    while (q.next()) {
        TrailItemRecord item;
        item.id = q.value(0).toInt();
        item.position = q.value(1).toInt();
        item.arxivId = q.value(2).toString();
        item.inspireRecid = q.value(3).toInt();
        item.doi = q.value(4).toString();
        item.title = q.value(5).toString();
        item.authors = q.value(6).toString();
        item.year = q.value(7).toString();
        item.note = q.value(8).toString();
        result << item;
    }

    for (TrailItemRecord &item : result) {
        QString localId;
        QString path;
        if (resolveLocalPaper(item.arxivId, item.doi, item.inspireRecid, &localId, &path)) {
            item.local = true;
            item.localArxivId = localId;
            item.localPath = path;
        }
    }
    return result;
}

bool Database::removeTrailItem(int itemId, QString *error)
{
    QSqlQuery find(db_);
    find.prepare(QStringLiteral("SELECT trail_id FROM literature_trail_items WHERE id = ?"));
    find.addBindValue(itemId);
    if (!find.exec() || !find.next())
        return setError(error, find.lastError().text().isEmpty()
                            ? QStringLiteral("Trail item not found.") : find.lastError().text());
    const int trailId = find.value(0).toInt();

    QSqlQuery q(db_);
    q.prepare(QStringLiteral("DELETE FROM literature_trail_items WHERE id = ?"));
    q.addBindValue(itemId);
    if (!q.exec())
        return setError(error, q.lastError().text());

    QSqlQuery ordered(db_);
    ordered.prepare(QStringLiteral("SELECT id FROM literature_trail_items WHERE trail_id = ? ORDER BY position, id"));
    ordered.addBindValue(trailId);
    if (!ordered.exec())
        return setError(error, ordered.lastError().text());
    QList<int> ids;
    while (ordered.next())
        ids << ordered.value(0).toInt();
    QSqlQuery update(db_);
    update.prepare(QStringLiteral("UPDATE literature_trail_items SET position = ? WHERE id = ?"));
    for (int i = 0; i < ids.size(); ++i) {
        update.bindValue(0, i + 1);
        update.bindValue(1, ids.at(i));
        if (!update.exec())
            return setError(error, update.lastError().text());
    }
    QSqlQuery touch(db_);
    touch.prepare(QStringLiteral("UPDATE literature_trails SET updated_at = CURRENT_TIMESTAMP WHERE id = ?"));
    touch.addBindValue(trailId);
    touch.exec();
    return true;
}

bool Database::moveTrailItem(int trailId, int itemId, int delta, QString *error)
{
    if (delta == 0)
        return true;
    QSqlQuery q(db_);
    q.prepare(QStringLiteral("SELECT id FROM literature_trail_items WHERE trail_id = ? ORDER BY position, id"));
    q.addBindValue(trailId);
    if (!q.exec())
        return setError(error, q.lastError().text());
    QList<int> ids;
    while (q.next())
        ids << q.value(0).toInt();
    const int index = ids.indexOf(itemId);
    if (index < 0)
        return setError(error, QStringLiteral("Trail item not found."));
    const int target = qBound(0, index + delta, ids.size() - 1);
    if (target == index)
        return true;
    ids.move(index, target);

    if (!db_.transaction())
        return setError(error, db_.lastError().text());
    QSqlQuery update(db_);
    update.prepare(QStringLiteral("UPDATE literature_trail_items SET position = ? WHERE id = ?"));
    for (int i = 0; i < ids.size(); ++i) {
        update.bindValue(0, i + 1);
        update.bindValue(1, ids.at(i));
        if (!update.exec()) {
            db_.rollback();
            return setError(error, update.lastError().text());
        }
    }
    QSqlQuery touch(db_);
    touch.prepare(QStringLiteral("UPDATE literature_trails SET updated_at = CURRENT_TIMESTAMP WHERE id = ?"));
    touch.addBindValue(trailId);
    if (!touch.exec()) {
        db_.rollback();
        return setError(error, touch.lastError().text());
    }
    if (!db_.commit())
        return setError(error, db_.lastError().text());
    return true;
}

bool Database::setTrailItemNote(int itemId, const QString &note, QString *error)
{
    QSqlQuery q(db_);
    q.prepare(QStringLiteral(R"SQL(
        UPDATE literature_trail_items
        SET note = ?
        WHERE id = ?
    )SQL"));
    q.addBindValue(note);
    q.addBindValue(itemId);
    if (!q.exec())
        return setError(error, q.lastError().text());

    QSqlQuery touch(db_);
    touch.prepare(QStringLiteral(R"SQL(
        UPDATE literature_trails SET updated_at = CURRENT_TIMESTAMP
        WHERE id = (SELECT trail_id FROM literature_trail_items WHERE id = ?)
    )SQL"));
    touch.addBindValue(itemId);
    touch.exec();
    return true;
}

bool Database::createTag(const QString &name, QString *error)
{
    const QString clean = normalizedLabel(name);
    if (clean.isEmpty())
        return setError(error, QStringLiteral("Tag name cannot be empty."));
    QSqlQuery q(db_);
    q.prepare(QStringLiteral("INSERT OR IGNORE INTO tags(name) VALUES (?)"));
    q.addBindValue(clean);
    if (!q.exec())
        return setError(error, q.lastError().text());
    return true;
}

bool Database::deleteTag(const QString &name, QString *error)
{
    QSqlQuery q(db_);
    q.prepare(QStringLiteral("DELETE FROM tags WHERE name = ? COLLATE NOCASE"));
    q.addBindValue(name);
    if (!q.exec())
        return setError(error, q.lastError().text());
    return true;
}

bool Database::addTagToPaper(const QString &arxivId, const QString &name, QString *error)
{
    if (!createTag(name, error))
        return false;
    QSqlQuery q(db_);
    q.prepare(QStringLiteral(R"SQL(
        INSERT OR IGNORE INTO paper_tags(arxiv_id, tag_id)
        SELECT ?, id FROM tags WHERE name = ? COLLATE NOCASE
    )SQL"));
    q.addBindValue(arxivId);
    q.addBindValue(normalizedLabel(name));
    if (!q.exec())
        return setError(error, q.lastError().text());
    return true;
}

bool Database::removeTagFromPaper(const QString &arxivId, const QString &name, QString *error)
{
    QSqlQuery q(db_);
    q.prepare(QStringLiteral(R"SQL(
        DELETE FROM paper_tags
        WHERE arxiv_id = ? AND tag_id IN (SELECT id FROM tags WHERE name = ? COLLATE NOCASE)
    )SQL"));
    q.addBindValue(arxivId);
    q.addBindValue(name);
    if (!q.exec())
        return setError(error, q.lastError().text());
    return true;
}

QStringList Database::tagsForPaper(const QString &arxivId, QString *error) const
{
    QStringList result;
    QSqlQuery q(db_);
    q.prepare(QStringLiteral(R"SQL(
        SELECT t.name FROM paper_tags pt JOIN tags t ON t.id = pt.tag_id
        WHERE pt.arxiv_id = ? ORDER BY t.name COLLATE NOCASE
    )SQL"));
    q.addBindValue(arxivId);
    if (!q.exec()) {
        setError(error, q.lastError().text());
        return result;
    }
    while (q.next())
        result << q.value(0).toString();
    return result;
}

bool Database::createCollection(const QString &name, QString *error)
{
    const QString clean = normalizedLabel(name);
    if (clean.isEmpty())
        return setError(error, QStringLiteral("Collection name cannot be empty."));
    QSqlQuery q(db_);
    q.prepare(QStringLiteral("INSERT OR IGNORE INTO collections(name) VALUES (?)"));
    q.addBindValue(clean);
    if (!q.exec())
        return setError(error, q.lastError().text());
    return true;
}

bool Database::deleteCollection(const QString &name, QString *error)
{
    QSqlQuery q(db_);
    q.prepare(QStringLiteral("DELETE FROM collections WHERE name = ? COLLATE NOCASE"));
    q.addBindValue(name);
    if (!q.exec())
        return setError(error, q.lastError().text());
    return true;
}

bool Database::addPaperToCollection(const QString &arxivId, const QString &name, QString *error)
{
    if (!createCollection(name, error))
        return false;
    QSqlQuery q(db_);
    q.prepare(QStringLiteral(R"SQL(
        INSERT OR IGNORE INTO collection_papers(collection_id, arxiv_id)
        SELECT id, ? FROM collections WHERE name = ? COLLATE NOCASE
    )SQL"));
    q.addBindValue(arxivId);
    q.addBindValue(normalizedLabel(name));
    if (!q.exec())
        return setError(error, q.lastError().text());
    return true;
}

bool Database::removePaperFromCollection(const QString &arxivId, const QString &name, QString *error)
{
    QSqlQuery q(db_);
    q.prepare(QStringLiteral(R"SQL(
        DELETE FROM collection_papers
        WHERE arxiv_id = ? AND collection_id IN (SELECT id FROM collections WHERE name = ? COLLATE NOCASE)
    )SQL"));
    q.addBindValue(arxivId);
    q.addBindValue(name);
    if (!q.exec())
        return setError(error, q.lastError().text());
    return true;
}

QStringList Database::collectionsForPaper(const QString &arxivId, QString *error) const
{
    QStringList result;
    QSqlQuery q(db_);
    q.prepare(QStringLiteral(R"SQL(
        SELECT c.name FROM collection_papers cp JOIN collections c ON c.id = cp.collection_id
        WHERE cp.arxiv_id = ? ORDER BY c.name COLLATE NOCASE
    )SQL"));
    q.addBindValue(arxivId);
    if (!q.exec()) {
        setError(error, q.lastError().text());
        return result;
    }
    while (q.next())
        result << q.value(0).toString();
    return result;
}

QList<FacetCount> Database::tagCounts(QString *error) const
{
    return readFacetQuery(db_, QStringLiteral(R"SQL(
        SELECT t.name, COUNT(DISTINCT pt.arxiv_id)
        FROM tags t LEFT JOIN paper_tags pt ON pt.tag_id = t.id
        GROUP BY t.id, t.name ORDER BY t.name COLLATE NOCASE
    )SQL"), error);
}

QList<FacetCount> Database::collectionCounts(QString *error) const
{
    return readFacetQuery(db_, QStringLiteral(R"SQL(
        SELECT c.name, COUNT(DISTINCT cp.arxiv_id)
        FROM collections c LEFT JOIN collection_papers cp ON cp.collection_id = c.id
        GROUP BY c.id, c.name ORDER BY c.name COLLATE NOCASE
    )SQL"), error);
}

QList<FacetCount> Database::authorCounts(QString *error) const
{
    return readFacetQuery(db_, QStringLiteral(R"SQL(
        SELECT a.canonical_name, COUNT(DISTINCT pa.arxiv_id)
        FROM authors a JOIN paper_authors pa ON pa.author_id = a.id
        GROUP BY a.id, a.canonical_name
        ORDER BY COUNT(DISTINCT pa.arxiv_id) DESC, a.canonical_name COLLATE NOCASE
    )SQL"), error);
}

QList<FacetCount> Database::categoryCounts(QString *error) const
{
    return readFacetQuery(db_, QStringLiteral(R"SQL(
        SELECT primary_category, COUNT(*)
        FROM papers
        WHERE trim(COALESCE(primary_category, '')) <> ''
          AND EXISTS (SELECT 1 FROM files WHERE files.arxiv_id = papers.arxiv_id)
        GROUP BY primary_category
        ORDER BY COUNT(*) DESC, primary_category COLLATE NOCASE
    )SQL"), error);
}

QList<FacetCount> Database::trailCounts(QString *error) const
{
    return readFacetQuery(db_, QStringLiteral(R"SQL(
        SELECT t.name, COUNT(DISTINCT p.arxiv_id)
        FROM literature_trails t
        LEFT JOIN literature_trail_items i ON i.trail_id = t.id
        LEFT JOIN papers p ON
             lower(COALESCE(i.arxiv_id, '')) = lower(p.arxiv_id)
          OR (COALESCE(i.inspire_recid, 0) > 0 AND i.inspire_recid = COALESCE(p.inspire_recid, 0))
          OR (trim(COALESCE(i.doi, '')) <> '' AND lower(trim(i.doi)) = lower(trim(COALESCE(p.doi, ''))))
        WHERE p.arxiv_id IS NULL OR EXISTS (SELECT 1 FROM files f WHERE f.arxiv_id = p.arxiv_id)
        GROUP BY t.id, t.name
        ORDER BY t.updated_at DESC, t.name COLLATE NOCASE
    )SQL"), error);
}

int Database::paperCount(QString *error) const
{
    QSqlQuery q(db_);
    if (!q.exec(QStringLiteral("SELECT COUNT(*) FROM papers WHERE EXISTS (SELECT 1 FROM files WHERE files.arxiv_id = papers.arxiv_id)"))) {
        setError(error, q.lastError().text());
        return 0;
    }
    return q.next() ? q.value(0).toInt() : 0;
}

int Database::fileCount(QString *error) const
{
    QSqlQuery q(db_);
    if (!q.exec(QStringLiteral("SELECT COUNT(*) FROM files"))) {
        setError(error, q.lastError().text());
        return 0;
    }
    return q.next() ? q.value(0).toInt() : 0;
}

int Database::favoriteCount(QString *error) const
{
    QSqlQuery q(db_);
    if (!q.exec(QStringLiteral("SELECT COUNT(*) FROM papers WHERE favorite = 1 AND EXISTS (SELECT 1 FROM files WHERE files.arxiv_id = papers.arxiv_id)"))) {
        setError(error, q.lastError().text());
        return 0;
    }
    return q.next() ? q.value(0).toInt() : 0;
}

int Database::missingMetadataCount(QString *error) const
{
    QSqlQuery q(db_);
    if (!q.exec(QStringLiteral("SELECT COUNT(*) FROM papers WHERE (title IS NULL OR trim(title) = '') AND EXISTS (SELECT 1 FROM files WHERE files.arxiv_id = papers.arxiv_id)"))) {
        setError(error, q.lastError().text());
        return 0;
    }
    return q.next() ? q.value(0).toInt() : 0;
}

int Database::unreadCount(QString *error) const
{
    QSqlQuery q(db_);
    if (!q.exec(QStringLiteral("SELECT COUNT(*) FROM papers WHERE COALESCE(open_count, 0) = 0 AND EXISTS (SELECT 1 FROM files WHERE files.arxiv_id = papers.arxiv_id)"))) {
        setError(error, q.lastError().text());
        return 0;
    }
    return q.next() ? q.value(0).toInt() : 0;
}

int Database::duplicatePaperCount(QString *error) const
{
    QSqlQuery q(db_);
    if (!q.exec(QStringLiteral(R"SQL(
        SELECT COUNT(*) FROM (
            SELECT arxiv_id FROM files GROUP BY arxiv_id HAVING COUNT(*) > 1
        )
    )SQL"))) {
        setError(error, q.lastError().text());
        return 0;
    }
    return q.next() ? q.value(0).toInt() : 0;
}
