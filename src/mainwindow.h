#pragma once

#include "arxiv_client.h"
#include "arxiv_watches.h"
#include "database.h"
#include "inspire_client.h"
#include "scanner.h"

#include <QFutureWatcher>
#include <QMainWindow>
#include <QStringList>

class QAction;
class QCloseEvent;
class QLabel;
class QLineEdit;
class QGroupBox;
class QListWidget;
class QMenu;
class QPdfDocument;
class QPdfLink;
class QPdfView;
class QPlainTextEdit;
class QProgressBar;
class QPoint;
class QPushButton;
class QSpinBox;
class QSplitter;
class QTabWidget;
class QTableWidget;
class QSystemTrayIcon;
class QTextBrowser;
class QToolButton;
class QTimer;
class QTreeWidget;
class QTreeWidgetItem;
class QWidget;

class MainWindow : public QMainWindow {
public:
    explicit MainWindow(QWidget *parent = nullptr);
    ~MainWindow() override;

protected:
    void closeEvent(QCloseEvent *event) override;

private:
    void buildUi();
    void buildMenusAndToolbar();
    QWidget *buildLibraryPage();
    QWidget *buildReaderPage();
    QWidget *buildDetailsPanel();
    void applyProfessionalStyle();
    void restoreUiState();
    void saveUiState();

    void showArxivDiscovery(const QString &author = {});
    void showArxivWatches();
    void addFolder();
    void manageFolders();
    void rescan();
    void applyScanResult(const ScanResult &result);

    void fetchMissingMetadata();
    void fetchMetadataForIds(const QStringList &ids, const QString &label);
    void refreshSelectedMetadata();
    void refreshSelectedInspire();
    void showSelectedReferences();
    void showSelectedCitedBy();
    void watchSelectedCitations();
    void showSelectedCitationGraph();
    void showLiteratureTrails(int selectTrailId = 0);

    void refreshTable();
    void updateCounts();
    void rebuildNavigation();
    void setLibraryFilter(LibraryFilter filter, const QString &facetValue = {});
    void showNavigationContextMenu(const QPoint &pos);

    QString selectedArxivId() const;
    QString selectedLocalPath() const;
    void showSelectedDetails();
    void showPaperDetails(const QString &arxivId);
    void clearDetails();
    void toggleSelectedFavorite();

    void addTagToSelected();
    void removeTagFromSelected();
    void addSelectedToCollection();
    void removeSelectedFromCollection();
    void createCollection();
    void addSelectedToLiteratureTrail();
    void saveCurrentPaperNote(bool quiet = false);

    void organizeSelectedFile(bool renameOnly = false);
    void movePapersToFolder(bool allLibrary);
    QString expandOrganizationTemplate(const QString &pattern,
                                       const PaperDetails &details,
                                       const QString &sourcePath) const;

    void openSelectedInReader();
    void openPaperInReader(const QString &arxivId, const QString &path, bool pushHistory = false, int pageOverride = -1);
    void openSelectedExternally();
    void openContainingFolder();
    void openArxivPage();
    void openInspirePage();
    void showLibraryContextMenu(const QPoint &pos);

    void exportBibTeX();
    void exportSelectedInspireBibTeX();
    void copySelectedInspireBibTeX();
    void copySelectedArxivId();
    void copySelectedTitle();

    void updateReaderForDocumentStatus();
    void updateReaderPage(int zeroBasedPage);
    void readerPreviousPage();
    void readerNextPage();
    void readerZoomIn();
    void readerZoomOut();
    void readerFitWidth();
    void readerFitPage();
    void readerGoBack();
    void readerGoForward();
    void updateReaderHistoryButtons();
    void updateReaderCitationPanel();
    void updateReaderCitationPreview();
    void ensureReaderReferences();
    void refreshReaderReferencesFromInspire();
    void openSelectedReaderCitation();
    void openReaderReferenceBrowser();
    ReferenceRecord readerReferenceForPosition(int position) const;
    void handlePdfJumped(const QPdfLink &link);

    void setBusyUi(bool busy, const QString &message = {});
    void showDatabaseError(const QString &context, const QString &error);

    Database db_;
    ArxivClient arxiv_;
    InspireClient inspire_;
    ArxivWatchManager *watches_ = nullptr;
    QSystemTrayIcon *citationTray_ = nullptr;
    QFutureWatcher<ScanResult> scanWatcher_;

    LibraryFilter currentFilter_ = LibraryFilter::All;
    QString currentFacetValue_;
    QStringList metadataErrors_;
    QString currentReaderArxivId_;
    QString currentReaderPath_;
    int pendingReaderPage_ = 0;
    bool databaseReady_ = false;

    struct ReaderLocation {
        QString arxivId;
        QString path;
        int page = 0;
    };
    QList<ReaderLocation> readerBackHistory_;
    QList<ReaderLocation> readerForwardHistory_;
    QList<ReferenceRecord> readerReferences_;
    bool readerReferencesLoading_ = false;
    int readerPreviousPage_ = -1;
    bool suppressPdfCitationJump_ = false;

    QString currentDetailsArxivId_;
    bool loadingPaperNote_ = false;
    bool paperNoteDirty_ = false;
    QTimer *noteSaveTimer_ = nullptr;

    QAction *discoverAction_ = nullptr;
    QAction *watchesAction_ = nullptr;
    QAction *addFolderAction_ = nullptr;
    QAction *manageFoldersAction_ = nullptr;
    QAction *scanAction_ = nullptr;
    QAction *metadataAction_ = nullptr;
    QAction *exportBibAction_ = nullptr;
    QAction *refreshInspireAction_ = nullptr;
    QAction *referencesAction_ = nullptr;
    QAction *citedByAction_ = nullptr;
    QAction *citationGraphAction_ = nullptr;
    QAction *trailsAction_ = nullptr;
    QAction *saveNoteAction_ = nullptr;
    QAction *exportSelectedBibAction_ = nullptr;
    QAction *favoriteAction_ = nullptr;
    QAction *openReaderAction_ = nullptr;
    QAction *openExternalAction_ = nullptr;
    QAction *organizeAction_ = nullptr;
    QAction *renameAction_ = nullptr;
    QAction *moveSelectedPapersAction_ = nullptr;
    QAction *moveAllPapersAction_ = nullptr;

    QLineEdit *search_ = nullptr;
    QTreeWidget *navigation_ = nullptr;
    QTableWidget *table_ = nullptr;
    QTabWidget *tabs_ = nullptr;
    QSplitter *librarySplitter_ = nullptr;

    QLabel *detailsTitle_ = nullptr;
    QLabel *detailsAuthors_ = nullptr;
    QLabel *detailsMeta_ = nullptr;
    QLabel *detailsJournal_ = nullptr;
    QLabel *detailsCitationMetrics_ = nullptr;
    QTextBrowser *detailsAbstract_ = nullptr;
    QListWidget *detailsFiles_ = nullptr;
    QListWidget *detailsTags_ = nullptr;
    QListWidget *detailsCollections_ = nullptr;
    QListWidget *detailsTrails_ = nullptr;
    QPlainTextEdit *detailsNotes_ = nullptr;
    QLabel *detailsNotesStatus_ = nullptr;
    QToolButton *favoriteButton_ = nullptr;
    QPushButton *readerButton_ = nullptr;
    QPushButton *externalButton_ = nullptr;
    QPushButton *arxivButton_ = nullptr;
    QPushButton *inspireButton_ = nullptr;
    QPushButton *referencesButton_ = nullptr;
    QPushButton *citedByButton_ = nullptr;
    QPushButton *citationGraphButton_ = nullptr;
    QPushButton *refreshInspireButton_ = nullptr;
    QPushButton *addTagButton_ = nullptr;
    QPushButton *removeTagButton_ = nullptr;
    QPushButton *addCollectionButton_ = nullptr;
    QPushButton *removeCollectionButton_ = nullptr;
    QPushButton *addTrailButton_ = nullptr;
    QPushButton *manageTrailsButton_ = nullptr;

    QLabel *summaryLabel_ = nullptr;
    QProgressBar *progress_ = nullptr;

    QPdfDocument *pdfDocument_ = nullptr;
    QPdfView *pdfView_ = nullptr;
    QLabel *readerTitle_ = nullptr;
    QSpinBox *pageSpin_ = nullptr;
    QLabel *pageCountLabel_ = nullptr;
    QPushButton *readerPrevButton_ = nullptr;
    QPushButton *readerNextButton_ = nullptr;
    QPushButton *readerExternalButton_ = nullptr;
    QPushButton *readerHistoryBackButton_ = nullptr;
    QPushButton *readerHistoryForwardButton_ = nullptr;
    QPushButton *readerCitationToggleButton_ = nullptr;
    QWidget *readerCitationPanel_ = nullptr;
    QLabel *readerCitationHeading_ = nullptr;
    QLabel *readerCitationStatus_ = nullptr;
    QListWidget *readerCitationList_ = nullptr;
    QLabel *readerCitationPreview_ = nullptr;
    QPushButton *readerCitationOpenButton_ = nullptr;
    QPushButton *readerCitationArxivButton_ = nullptr;
    QPushButton *readerCitationBrowseButton_ = nullptr;
    QPushButton *readerCitationRefreshButton_ = nullptr;
};
