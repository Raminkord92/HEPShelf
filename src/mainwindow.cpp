#include "mainwindow.h"
#include "arxiv_discovery.h"
#include "arxiv_watches_dialog.h"
#include "reference_dialog.h"
#include "cited_by_dialog.h"
#include "citation_graph_dialog.h"
#include "literature_trails_dialog.h"

#include <QAbstractItemView>
#include <QAction>
#include <QApplication>
#include <QCheckBox>
#include <QClipboard>
#include <QCloseEvent>
#include <QComboBox>
#include <QDesktopServices>
#include <QDialog>
#include <QDialogButtonBox>
#include <QDir>
#include <QFile>
#include <QFileDialog>
#include <QFileInfo>
#include <QFrame>
#include <QFont>
#include <QFormLayout>
#include <QFuture>
#include <QHeaderView>
#include <QHBoxLayout>
#include <QIcon>
#include <QLabel>
#include <QItemSelectionModel>
#include <QInputDialog>
#include <QKeySequence>
#include <QLineEdit>
#include <QListView>
#include <QListWidget>
#include <QMap>
#include <QMenu>
#include <QMenuBar>
#include <QMessageBox>
#include <QPdfDocument>
#include <QPdfLink>
#include <QPdfPageNavigator>
#include <QPdfSelection>
#include <QPdfView>
#include <QPointF>
#include <QProgressBar>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QRegularExpression>
#include <QScrollArea>
#include <QSet>
#include <QSignalBlocker>
#include <QSettings>
#include <QSpinBox>
#include <QSizePolicy>
#include <QSplitter>
#include <QStandardPaths>
#include <QStatusBar>
#include <QStyle>
#include <QSystemTrayIcon>
#include <QTabWidget>
#include <QTableWidget>
#include <QTextBrowser>
#include <QTextStream>
#include <QTimer>
#include <QToolBar>
#include <QToolButton>
#include <QTreeWidget>
#include <QTreeWidgetItemIterator>
#include <QUrl>
#include <QUrlQuery>
#include <QVBoxLayout>
#include <QtConcurrent/QtConcurrentRun>

namespace {
constexpr int RoleArxivId = Qt::UserRole;
constexpr int RolePath = Qt::UserRole + 1;
constexpr int RoleFavorite = Qt::UserRole + 2;
constexpr int RoleNavFilter = Qt::UserRole + 20;
constexpr int RoleNavValue = Qt::UserRole + 21;

QString displayTitle(const PaperDetails &details)
{
    return details.paper.title.trimmed().isEmpty() ? details.paper.arxivId : details.paper.title.trimmed();
}

QString yearFromDate(const QString &date)
{
    return date.size() >= 4 ? date.left(4) : QString();
}

QString bibKey(QString arxivId)
{
    arxivId.replace(QRegularExpression(QStringLiteral("[^A-Za-z0-9]+")), QStringLiteral("_"));
    return QStringLiteral("arxiv_%1").arg(arxivId);
}

QString bibAuthors(const PaperDetails &details)
{
    if (!details.indexedAuthors.isEmpty())
        return details.indexedAuthors.join(QStringLiteral(" and "));
    if (!details.paper.authorNames.isEmpty())
        return details.paper.authorNames.join(QStringLiteral(" and "));
    return details.paper.authors;
}

QString formatProgress(int page, int count)
{
    if (count <= 0)
        return {};
    const int percent = qBound(0, qRound((page + 1) * 100.0 / count), 100);
    return QStringLiteral("%1 / %2 pages · %3%").arg(page + 1).arg(count).arg(percent);
}

QString safePathPart(QString value)
{
    value = value.simplified();
    value.replace(QRegularExpression(QStringLiteral("[\\\\/:*?\"<>|]+")), QStringLiteral("_"));
    value.replace(QRegularExpression(QStringLiteral("\\s+")), QStringLiteral("_"));
    value.replace(QRegularExpression(QStringLiteral("_+")), QStringLiteral("_"));
    while (value.startsWith(QLatin1Char('.')) || value.startsWith(QLatin1Char('_')))
        value.remove(0, 1);
    while (value.endsWith(QLatin1Char('.')) || value.endsWith(QLatin1Char('_')))
        value.chop(1);
    return value.left(110);
}

bool safeOrganizationRelative(const QString &relative)
{
    return !relative.isEmpty() && relative != QStringLiteral(".")
           && relative != QStringLiteral("..")
           && !relative.startsWith(QStringLiteral("../"))
           && !QDir::isAbsolutePath(relative);
}

QString firstAuthorKey(const PaperDetails &details)
{
    QString name;
    if (!details.indexedAuthors.isEmpty())
        name = details.indexedAuthors.first();
    else if (!details.paper.authors.isEmpty())
        name = details.paper.authors.section(QStringLiteral(", "), 0, 0);
    if (name.contains(QLatin1Char(',')))
        name = name.section(QLatin1Char(','), 0, 0);
    else {
        const QStringList words = name.simplified().split(QLatin1Char(' '), Qt::SkipEmptyParts);
        if (!words.isEmpty())
            name = words.last();
    }
    const QString safe = safePathPart(name);
    return safe.isEmpty() ? QStringLiteral("UnknownAuthor") : safe;
}

QString humanBytes(qint64 bytes)
{
    if (bytes < 1024)
        return QStringLiteral("%1 B").arg(bytes);
    if (bytes < 1024 * 1024)
        return QStringLiteral("%1 KiB").arg(bytes / 1024.0, 0, 'f', 1);
    return QStringLiteral("%1 MiB").arg(bytes / (1024.0 * 1024.0), 0, 'f', 1);
}
}

MainWindow::MainWindow(QWidget *parent)
    : QMainWindow(parent), arxiv_(this), inspire_(this)
{
    buildUi();

    QString error;
    if (!db_.open(&error)) {
        QMessageBox::critical(this, QStringLiteral("HEPShelf"),
                              QStringLiteral("Could not open the local library database.\n\n%1").arg(error));
        setBusyUi(true, QStringLiteral("Database unavailable"));
        return;
    }
    databaseReady_ = true;
    watches_ = new ArxivWatchManager(&db_, this);
    connect(watches_, &ArxivWatchManager::libraryChanged, this, [this]() {
        refreshTable();
        rebuildNavigation();
        updateCounts();
    });
    connect(watches_, &ArxivWatchManager::statusChanged, this, [this](const QString &message) {
        statusBar()->showMessage(message, 12000);
    });
    connect(watches_, &ArxivWatchManager::newCitationsFound, this, [this](int count) {
        const QString message = QStringLiteral("%1 new citing paper(s) found. Open Research watches to review them.").arg(count);
        statusBar()->showMessage(message, 20000);
        QApplication::alert(this);
        if (QSystemTrayIcon::isSystemTrayAvailable()) {
            if (!citationTray_) {
                citationTray_ = new QSystemTrayIcon(style()->standardIcon(QStyle::SP_FileDialogInfoView), this);
                citationTray_->setToolTip(QStringLiteral("HEPShelf citation watches"));
                citationTray_->show();
            }
            citationTray_->showMessage(QStringLiteral("New citations"), message,
                                       QSystemTrayIcon::Information, 15000);
        }
    });
    auto updateWatchAction = [this]() {
        int newCount = 0;
        int issueCount = 0;
        for (const ArxivWatchHit &hit : watches_->hits()) {
            if (hit.status == QStringLiteral("New"))
                ++newCount;
            if (hit.status == QStringLiteral("Failed"))
                ++issueCount;
        }
        for (const ArxivWatchRule &rule : watches_->rules()) {
            if (!rule.lastError.isEmpty())
                ++issueCount;
        }
        QStringList counts;
        if (newCount > 0)
            counts << QStringLiteral("%1 new").arg(newCount);
        if (issueCount > 0)
            counts << QStringLiteral("%1 issue(s)").arg(issueCount);
        watchesAction_->setText(counts.isEmpty()
                                    ? QStringLiteral("Research watches…")
                                    : QStringLiteral("Research watches… (%1)").arg(counts.join(QStringLiteral(", "))));
    };
    connect(watches_, &ArxivWatchManager::changed, this, updateWatchAction);
    updateWatchAction();

    refreshTable();
    updateCounts();
    rebuildNavigation();
    restoreUiState();

    QTimer::singleShot(0, this, [this]() { fetchMissingMetadata(); });
}

MainWindow::~MainWindow()
{
    delete watches_;
}

void MainWindow::buildUi()
{
    setWindowTitle(QStringLiteral("HEPShelf"));
    resize(1480, 860);
    setMinimumSize(980, 620);
    setWindowIcon(style()->standardIcon(QStyle::SP_FileDialogDetailedView));

    buildMenusAndToolbar();

    tabs_ = new QTabWidget(this);
    tabs_->setDocumentMode(true);
    tabs_->addTab(buildLibraryPage(), QStringLiteral("Library"));
    tabs_->addTab(buildReaderPage(), QStringLiteral("Reader"));
    tabs_->setTabEnabled(1, false);
    setCentralWidget(tabs_);

    summaryLabel_ = new QLabel(this);
    summaryLabel_->setTextInteractionFlags(Qt::TextSelectableByMouse);
    progress_ = new QProgressBar(this);
    progress_->setMaximumWidth(220);
    progress_->setTextVisible(true);
    progress_->hide();
    statusBar()->addPermanentWidget(summaryLabel_, 1);
    statusBar()->addPermanentWidget(progress_);
    statusBar()->showMessage(QStringLiteral("Ready"));

    connect(&scanWatcher_, &QFutureWatcher<ScanResult>::finished, this, [this]() {
        const ScanResult result = scanWatcher_.result();
        applyScanResult(result);
        scanAction_->setEnabled(true);
        addFolderAction_->setEnabled(true);
        manageFoldersAction_->setEnabled(true);
        progress_->hide();
        fetchMissingMetadata();
    });

    applyProfessionalStyle();
}

void MainWindow::buildMenusAndToolbar()
{
    discoverAction_ = new QAction(QStringLiteral("Discover arXiv…"), this);
    discoverAction_->setShortcut(QKeySequence(QStringLiteral("Ctrl+Shift+A")));
    watchesAction_ = new QAction(QStringLiteral("Research watches…"), this);

    addFolderAction_ = new QAction(style()->standardIcon(QStyle::SP_DirOpenIcon),
                                   QStringLiteral("Add paper folder…"), this);
    addFolderAction_->setShortcut(QKeySequence(QStringLiteral("Ctrl+O")));

    manageFoldersAction_ = new QAction(QStringLiteral("Manage paper folders…"), this);
    scanAction_ = new QAction(style()->standardIcon(QStyle::SP_BrowserReload),
                              QStringLiteral("Rescan library"), this);
    scanAction_->setShortcut(QKeySequence(QStringLiteral("Ctrl+R")));

    metadataAction_ = new QAction(QStringLiteral("Refresh missing metadata"), this);
    metadataAction_->setShortcut(QKeySequence(QStringLiteral("Ctrl+Shift+R")));

    refreshInspireAction_ = new QAction(QStringLiteral("Refresh INSPIRE data for selected paper"), this);
    refreshInspireAction_->setShortcut(QKeySequence(QStringLiteral("Ctrl+I")));
    referencesAction_ = new QAction(QStringLiteral("Browse references…"), this);
    referencesAction_->setShortcut(QKeySequence(QStringLiteral("Ctrl+Shift+B")));
    citedByAction_ = new QAction(QStringLiteral("Browse cited-by papers…"), this);
    citedByAction_->setShortcut(QKeySequence(QStringLiteral("Ctrl+Shift+Y")));
    citationGraphAction_ = new QAction(QStringLiteral("Open citation network…"), this);
    citationGraphAction_->setShortcut(QKeySequence(QStringLiteral("Ctrl+G")));
    trailsAction_ = new QAction(QStringLiteral("Literature trails…"), this);
    trailsAction_->setShortcut(QKeySequence(QStringLiteral("Ctrl+Shift+T")));
    saveNoteAction_ = new QAction(QStringLiteral("Save paper note"), this);
    saveNoteAction_->setShortcut(QKeySequence::Save);

    exportBibAction_ = new QAction(QStringLiteral("Export current view as BibTeX…"), this);
    exportSelectedBibAction_ = new QAction(QStringLiteral("Export selected paper using INSPIRE BibTeX…"), this);
    favoriteAction_ = new QAction(QStringLiteral("Toggle favorite"), this);
    favoriteAction_->setShortcut(QKeySequence(QStringLiteral("Ctrl+D")));
    openReaderAction_ = new QAction(QStringLiteral("Open in HEPShelf Reader"), this);
    openReaderAction_->setShortcut(QKeySequence(QStringLiteral("Return")));
    openExternalAction_ = new QAction(QStringLiteral("Open in system PDF viewer"), this);
    organizeAction_ = new QAction(QStringLiteral("Organize / move selected PDF…"), this);
    organizeAction_->setShortcut(QKeySequence(QStringLiteral("Ctrl+Shift+M")));
    renameAction_ = new QAction(QStringLiteral("Rename selected PDF using metadata…"), this);
    moveSelectedPapersAction_ = new QAction(QStringLiteral("Move selected papers to folder…"), this);
    moveAllPapersAction_ = new QAction(QStringLiteral("Move all library papers to folder…"), this);
    connect(moveSelectedPapersAction_, &QAction::triggered, this, [this]() { movePapersToFolder(false); });
    connect(moveAllPapersAction_, &QAction::triggered, this, [this]() { movePapersToFolder(true); });

    auto *fileMenu = menuBar()->addMenu(QStringLiteral("&File"));
    fileMenu->addAction(discoverAction_);
    fileMenu->addSeparator();
    fileMenu->addAction(addFolderAction_);
    fileMenu->addAction(manageFoldersAction_);
    fileMenu->addSeparator();
    fileMenu->addAction(exportBibAction_);
    fileMenu->addAction(exportSelectedBibAction_);
    fileMenu->addSeparator();
    auto *quitAction = fileMenu->addAction(QStringLiteral("Quit"));
    quitAction->setShortcut(QKeySequence::Quit);
    connect(quitAction, &QAction::triggered, qApp, &QApplication::quit);

    auto *libraryMenu = menuBar()->addMenu(QStringLiteral("&Library"));
    libraryMenu->addAction(discoverAction_);
    libraryMenu->addAction(watchesAction_);
    libraryMenu->addSeparator();
    libraryMenu->addAction(scanAction_);
    libraryMenu->addAction(metadataAction_);
    libraryMenu->addAction(refreshInspireAction_);
    libraryMenu->addAction(referencesAction_);
    libraryMenu->addAction(citedByAction_);
    libraryMenu->addAction(citationGraphAction_);
    libraryMenu->addAction(trailsAction_);
    libraryMenu->addSeparator();
    libraryMenu->addAction(openReaderAction_);
    libraryMenu->addAction(openExternalAction_);
    libraryMenu->addAction(favoriteAction_);

    auto *organizeMenu = menuBar()->addMenu(QStringLiteral("&Organize"));
    organizeMenu->addAction(trailsAction_);
    organizeMenu->addAction(saveNoteAction_);
    organizeMenu->addSeparator();
    organizeMenu->addAction(organizeAction_);
    organizeMenu->addAction(renameAction_);
    organizeMenu->addAction(moveSelectedPapersAction_);
    organizeMenu->addAction(moveAllPapersAction_);
    organizeMenu->addSeparator();
    organizeMenu->addAction(QStringLiteral("Add tag to selected paper…"), this,
                            [this]() { addTagToSelected(); });
    organizeMenu->addAction(QStringLiteral("Add selected paper to collection…"), this,
                            [this]() { addSelectedToCollection(); });
    organizeMenu->addAction(QStringLiteral("Create collection…"), this,
                            [this]() { createCollection(); });

    auto *helpMenu = menuBar()->addMenu(QStringLiteral("&Help"));
    helpMenu->addAction(QStringLiteral("About HEPShelf"), this, [this]() {
        QMessageBox::about(
            this, QStringLiteral("About HEPShelf"),
            QStringLiteral("<b>HEPShelf 0.9.1</b><br><br>"
                           "A local, citation-oriented paper library designed for HEP workflows.<br><br>"
                           "This version adds persistent research notes and named literature trails that can be built from the library or citation graph, reordered, annotated, and exported."));
    });

    auto *toolbar = addToolBar(QStringLiteral("Library"));
    toolbar->setObjectName(QStringLiteral("mainToolbar"));
    toolbar->setMovable(false);
    toolbar->addAction(discoverAction_);
    toolbar->addAction(watchesAction_);
    toolbar->addSeparator();
    toolbar->addAction(addFolderAction_);
    toolbar->addAction(scanAction_);
    toolbar->addAction(metadataAction_);
    toolbar->addAction(refreshInspireAction_);
    toolbar->addSeparator();

    auto *searchLabel = new QLabel(QStringLiteral("Search"), toolbar);
    toolbar->addWidget(searchLabel);
    search_ = new QLineEdit(toolbar);
    search_->setObjectName(QStringLiteral("librarySearch"));
    search_->setPlaceholderText(QStringLiteral("Title, author, arXiv ID, note, tag, collection, trail, DOI, or path…"));
    search_->setClearButtonEnabled(true);
    search_->setMinimumWidth(340);
    search_->setMaximumWidth(680);
    toolbar->addWidget(search_);

    auto *focusSearch = new QAction(this);
    focusSearch->setShortcut(QKeySequence::Find);
    addAction(focusSearch);
    connect(focusSearch, &QAction::triggered, this, [this]() {
        search_->setFocus();
        search_->selectAll();
    });

    connect(discoverAction_, &QAction::triggered, this, [this]() { showArxivDiscovery(); });
    connect(watchesAction_, &QAction::triggered, this, [this]() { showArxivWatches(); });
    connect(addFolderAction_, &QAction::triggered, this, [this]() { addFolder(); });
    connect(manageFoldersAction_, &QAction::triggered, this, [this]() { manageFolders(); });
    connect(scanAction_, &QAction::triggered, this, [this]() { rescan(); });
    connect(metadataAction_, &QAction::triggered, this, [this]() { fetchMissingMetadata(); });
    connect(refreshInspireAction_, &QAction::triggered, this, [this]() { refreshSelectedInspire(); });
    connect(referencesAction_, &QAction::triggered, this, [this]() { showSelectedReferences(); });
    connect(citedByAction_, &QAction::triggered, this, [this]() { showSelectedCitedBy(); });
    connect(citationGraphAction_, &QAction::triggered, this, [this]() { showSelectedCitationGraph(); });
    connect(trailsAction_, &QAction::triggered, this, [this]() { showLiteratureTrails(); });
    connect(saveNoteAction_, &QAction::triggered, this, [this]() { saveCurrentPaperNote(false); });
    connect(exportBibAction_, &QAction::triggered, this, [this]() { exportBibTeX(); });
    connect(exportSelectedBibAction_, &QAction::triggered, this, [this]() { exportSelectedInspireBibTeX(); });
    connect(favoriteAction_, &QAction::triggered, this, [this]() { toggleSelectedFavorite(); });
    connect(openReaderAction_, &QAction::triggered, this, [this]() { openSelectedInReader(); });
    connect(openExternalAction_, &QAction::triggered, this, [this]() { openSelectedExternally(); });
    connect(organizeAction_, &QAction::triggered, this, [this]() { organizeSelectedFile(false); });
    connect(renameAction_, &QAction::triggered, this, [this]() { organizeSelectedFile(true); });
    connect(search_, &QLineEdit::textChanged, this, [this]() { refreshTable(); });
}

QWidget *MainWindow::buildLibraryPage()
{
    auto *page = new QWidget(this);
    auto *layout = new QVBoxLayout(page);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(0);

    librarySplitter_ = new QSplitter(Qt::Horizontal, page);
    librarySplitter_->setChildrenCollapsible(false);

    navigation_ = new QTreeWidget(librarySplitter_);
    navigation_->setObjectName(QStringLiteral("navigation"));
    navigation_->setHeaderHidden(true);
    navigation_->setRootIsDecorated(true);
    navigation_->setIndentation(14);
    navigation_->setSelectionMode(QAbstractItemView::SingleSelection);
    navigation_->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    navigation_->setMinimumWidth(190);
    navigation_->setMaximumWidth(310);
    navigation_->setContextMenuPolicy(Qt::CustomContextMenu);

    table_ = new QTableWidget(librarySplitter_);
    table_->setObjectName(QStringLiteral("libraryTable"));
    table_->setColumnCount(8);
    table_->setHorizontalHeaderLabels({QStringLiteral("Title"),
                                       QStringLiteral("Authors"),
                                       QStringLiteral("arXiv"),
                                       QStringLiteral("Year"),
                                       QStringLiteral("Category"),
                                       QStringLiteral("Citations"),
                                       QStringLiteral("Refs"),
                                       QStringLiteral("Copies")});
    table_->setSelectionBehavior(QAbstractItemView::SelectRows);
    table_->setSelectionMode(QAbstractItemView::ExtendedSelection);
    table_->setEditTriggers(QAbstractItemView::NoEditTriggers);
    table_->setAlternatingRowColors(true);
    table_->setShowGrid(false);
    table_->setSortingEnabled(true);
    table_->verticalHeader()->setVisible(false);
    table_->verticalHeader()->setDefaultSectionSize(34);
    table_->horizontalHeader()->setHighlightSections(false);
    table_->horizontalHeader()->setStretchLastSection(false);
    table_->horizontalHeader()->setSectionResizeMode(QHeaderView::Interactive);
    table_->horizontalHeader()->setMinimumSectionSize(48);
    table_->setColumnWidth(0, 430);
    table_->setColumnWidth(1, 280);
    table_->setColumnWidth(2, 125);
    table_->setColumnWidth(3, 65);
    table_->setColumnWidth(4, 110);
    table_->setColumnWidth(5, 80);
    table_->setColumnWidth(6, 65);
    table_->setColumnWidth(7, 70);
    table_->setContextMenuPolicy(Qt::CustomContextMenu);

    QWidget *details = buildDetailsPanel();
    details->setMinimumWidth(320);
    details->setMaximumWidth(520);
    librarySplitter_->addWidget(navigation_);
    librarySplitter_->addWidget(table_);
    librarySplitter_->addWidget(details);
    librarySplitter_->setStretchFactor(0, 0);
    librarySplitter_->setStretchFactor(1, 1);
    librarySplitter_->setStretchFactor(2, 0);
    librarySplitter_->setSizes({225, 900, 390});

    layout->addWidget(librarySplitter_);

    connect(navigation_, &QTreeWidget::currentItemChanged, this,
            [this](QTreeWidgetItem *current, QTreeWidgetItem *) {
                if (!current || !current->data(0, RoleNavFilter).isValid())
                    return;
                setLibraryFilter(static_cast<LibraryFilter>(current->data(0, RoleNavFilter).toInt()),
                                 current->data(0, RoleNavValue).toString());
            });
    connect(navigation_, &QTreeWidget::customContextMenuRequested,
            this, [this](const QPoint &pos) { showNavigationContextMenu(pos); });
    connect(table_->selectionModel(), &QItemSelectionModel::selectionChanged,
            this, [this]() { showSelectedDetails(); });
    connect(table_, &QTableWidget::cellDoubleClicked,
            this, [this](int, int) { openSelectedInReader(); });
    connect(table_, &QTableWidget::customContextMenuRequested,
            this, [this](const QPoint &pos) { showLibraryContextMenu(pos); });

    return page;
}

QWidget *MainWindow::buildDetailsPanel()
{
    auto *scroll = new QScrollArea(this);
    scroll->setObjectName(QStringLiteral("detailsScroll"));
    scroll->setWidgetResizable(true);
    scroll->setFrameShape(QFrame::NoFrame);

    auto *panel = new QWidget(scroll);
    auto *layout = new QVBoxLayout(panel);
    layout->setContentsMargins(18, 18, 18, 18);
    layout->setSpacing(10);

    auto *titleRow = new QHBoxLayout();
    detailsTitle_ = new QLabel(QStringLiteral("Select a paper"), panel);
    detailsTitle_->setObjectName(QStringLiteral("detailsTitle"));
    detailsTitle_->setWordWrap(true);
    detailsTitle_->setTextInteractionFlags(Qt::TextSelectableByMouse);
    favoriteButton_ = new QToolButton(panel);
    favoriteButton_->setCheckable(true);
    favoriteButton_->setText(QStringLiteral("☆"));
    favoriteButton_->setToolTip(QStringLiteral("Add/remove favorite"));
    favoriteButton_->setEnabled(false);
    titleRow->addWidget(detailsTitle_, 1);
    titleRow->addWidget(favoriteButton_, 0, Qt::AlignTop);
    layout->addLayout(titleRow);

    detailsAuthors_ = new QLabel(panel);
    detailsAuthors_->setObjectName(QStringLiteral("detailsAuthors"));
    detailsAuthors_->setWordWrap(true);
    detailsAuthors_->setTextInteractionFlags(Qt::TextSelectableByMouse);
    layout->addWidget(detailsAuthors_);

    detailsMeta_ = new QLabel(panel);
    detailsMeta_->setObjectName(QStringLiteral("detailsMeta"));
    detailsMeta_->setWordWrap(true);
    detailsMeta_->setTextInteractionFlags(Qt::TextSelectableByMouse);
    layout->addWidget(detailsMeta_);

    detailsJournal_ = new QLabel(panel);
    detailsJournal_->setWordWrap(true);
    detailsJournal_->setTextInteractionFlags(Qt::TextSelectableByMouse);
    layout->addWidget(detailsJournal_);

    detailsCitationMetrics_ = new QLabel(panel);
    detailsCitationMetrics_->setObjectName(QStringLiteral("citationMetrics"));
    detailsCitationMetrics_->setWordWrap(true);
    detailsCitationMetrics_->setTextInteractionFlags(Qt::TextSelectableByMouse);
    layout->addWidget(detailsCitationMetrics_);

    auto *citationButtons = new QHBoxLayout();
    referencesButton_ = new QPushButton(QStringLiteral("References…"), panel);
    citedByButton_ = new QPushButton(QStringLiteral("Cited by…"), panel);
    citationGraphButton_ = new QPushButton(QStringLiteral("Network…"), panel);
    refreshInspireButton_ = new QPushButton(QStringLiteral("Refresh INSPIRE"), panel);
    citationButtons->addWidget(referencesButton_);
    citationButtons->addWidget(citedByButton_);
    citationButtons->addWidget(citationGraphButton_);
    citationButtons->addWidget(refreshInspireButton_);
    layout->addLayout(citationButtons);

    auto *abstractHeading = new QLabel(QStringLiteral("Abstract"), panel);
    abstractHeading->setObjectName(QStringLiteral("sectionHeading"));
    layout->addWidget(abstractHeading);

    detailsAbstract_ = new QTextBrowser(panel);
    detailsAbstract_->setObjectName(QStringLiteral("abstractView"));
    detailsAbstract_->setOpenExternalLinks(false);
    detailsAbstract_->setMinimumHeight(190);
    layout->addWidget(detailsAbstract_, 1);

    auto *notesHeading = new QHBoxLayout();
    auto *notesLabel = new QLabel(QStringLiteral("Research notes"), panel);
    notesLabel->setObjectName(QStringLiteral("sectionHeading"));
    detailsNotesStatus_ = new QLabel(panel);
    detailsNotesStatus_->setObjectName(QStringLiteral("detailsMeta"));
    detailsNotesStatus_->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
    notesHeading->addWidget(notesLabel);
    notesHeading->addStretch();
    notesHeading->addWidget(detailsNotesStatus_);
    layout->addLayout(notesHeading);

    detailsNotes_ = new QPlainTextEdit(panel);
    detailsNotes_->setPlaceholderText(QStringLiteral("Your local research note for this paper — ideas, equations to revisit, presentation comments, links to your own work…"));
    detailsNotes_->setMinimumHeight(125);
    detailsNotes_->setMaximumHeight(210);
    layout->addWidget(detailsNotes_);

    noteSaveTimer_ = new QTimer(this);
    noteSaveTimer_->setSingleShot(true);
    noteSaveTimer_->setInterval(900);
    connect(noteSaveTimer_, &QTimer::timeout, this, [this]() { saveCurrentPaperNote(true); });
    connect(detailsNotes_, &QPlainTextEdit::textChanged, this, [this]() {
        if (loadingPaperNote_ || currentDetailsArxivId_.isEmpty())
            return;
        paperNoteDirty_ = true;
        detailsNotesStatus_->setText(QStringLiteral("Saving…"));
        noteSaveTimer_->start();
    });

    auto *tagsHeading = new QHBoxLayout();
    auto *tagsLabel = new QLabel(QStringLiteral("Tags"), panel);
    tagsLabel->setObjectName(QStringLiteral("sectionHeading"));
    addTagButton_ = new QPushButton(QStringLiteral("+"), panel);
    removeTagButton_ = new QPushButton(QStringLiteral("−"), panel);
    addTagButton_->setMaximumWidth(34);
    removeTagButton_->setMaximumWidth(34);
    tagsHeading->addWidget(tagsLabel);
    tagsHeading->addStretch();
    tagsHeading->addWidget(addTagButton_);
    tagsHeading->addWidget(removeTagButton_);
    layout->addLayout(tagsHeading);

    detailsTags_ = new QListWidget(panel);
    detailsTags_->setFlow(QListView::LeftToRight);
    detailsTags_->setWrapping(true);
    detailsTags_->setResizeMode(QListView::Adjust);
    detailsTags_->setMaximumHeight(82);
    detailsTags_->setSelectionMode(QAbstractItemView::SingleSelection);
    layout->addWidget(detailsTags_);

    auto *collectionsHeading = new QHBoxLayout();
    auto *collectionsLabel = new QLabel(QStringLiteral("Collections"), panel);
    collectionsLabel->setObjectName(QStringLiteral("sectionHeading"));
    addCollectionButton_ = new QPushButton(QStringLiteral("+"), panel);
    removeCollectionButton_ = new QPushButton(QStringLiteral("−"), panel);
    addCollectionButton_->setMaximumWidth(34);
    removeCollectionButton_->setMaximumWidth(34);
    collectionsHeading->addWidget(collectionsLabel);
    collectionsHeading->addStretch();
    collectionsHeading->addWidget(addCollectionButton_);
    collectionsHeading->addWidget(removeCollectionButton_);
    layout->addLayout(collectionsHeading);

    detailsCollections_ = new QListWidget(panel);
    detailsCollections_->setMaximumHeight(86);
    detailsCollections_->setSelectionMode(QAbstractItemView::SingleSelection);
    layout->addWidget(detailsCollections_);

    auto *trailsHeading = new QHBoxLayout();
    auto *trailsLabel = new QLabel(QStringLiteral("Literature trails"), panel);
    trailsLabel->setObjectName(QStringLiteral("sectionHeading"));
    addTrailButton_ = new QPushButton(QStringLiteral("Add…"), panel);
    manageTrailsButton_ = new QPushButton(QStringLiteral("Manage…"), panel);
    trailsHeading->addWidget(trailsLabel);
    trailsHeading->addStretch();
    trailsHeading->addWidget(addTrailButton_);
    trailsHeading->addWidget(manageTrailsButton_);
    layout->addLayout(trailsHeading);

    detailsTrails_ = new QListWidget(panel);
    detailsTrails_->setMaximumHeight(92);
    detailsTrails_->setSelectionMode(QAbstractItemView::SingleSelection);
    layout->addWidget(detailsTrails_);

    auto *filesHeading = new QLabel(QStringLiteral("Local copies / arXiv versions"), panel);
    filesHeading->setObjectName(QStringLiteral("sectionHeading"));
    layout->addWidget(filesHeading);

    detailsFiles_ = new QListWidget(panel);
    detailsFiles_->setSelectionMode(QAbstractItemView::SingleSelection);
    detailsFiles_->setMinimumHeight(85);
    detailsFiles_->setMaximumHeight(150);
    layout->addWidget(detailsFiles_);

    auto *primaryButtons = new QHBoxLayout();
    readerButton_ = new QPushButton(QStringLiteral("Read"), panel);
    readerButton_->setDefault(true);
    externalButton_ = new QPushButton(QStringLiteral("Open externally"), panel);
    primaryButtons->addWidget(readerButton_);
    primaryButtons->addWidget(externalButton_);
    layout->addLayout(primaryButtons);

    auto *onlineButtons = new QHBoxLayout();
    arxivButton_ = new QPushButton(QStringLiteral("arXiv"), panel);
    inspireButton_ = new QPushButton(QStringLiteral("INSPIRE"), panel);
    onlineButtons->addWidget(arxivButton_);
    onlineButtons->addWidget(inspireButton_);
    layout->addLayout(onlineButtons);
    layout->addStretch();

    connect(favoriteButton_, &QToolButton::clicked, this, [this]() { toggleSelectedFavorite(); });
    connect(readerButton_, &QPushButton::clicked, this, [this]() { openSelectedInReader(); });
    connect(externalButton_, &QPushButton::clicked, this, [this]() { openSelectedExternally(); });
    connect(arxivButton_, &QPushButton::clicked, this, [this]() { openArxivPage(); });
    connect(inspireButton_, &QPushButton::clicked, this, [this]() { openInspirePage(); });
    connect(referencesButton_, &QPushButton::clicked, this, [this]() { showSelectedReferences(); });
    connect(citedByButton_, &QPushButton::clicked, this, [this]() { showSelectedCitedBy(); });
    connect(citationGraphButton_, &QPushButton::clicked, this, [this]() { showSelectedCitationGraph(); });
    connect(refreshInspireButton_, &QPushButton::clicked, this, [this]() { refreshSelectedInspire(); });
    connect(addTagButton_, &QPushButton::clicked, this, [this]() { addTagToSelected(); });
    connect(removeTagButton_, &QPushButton::clicked, this, [this]() { removeTagFromSelected(); });
    connect(addCollectionButton_, &QPushButton::clicked, this, [this]() { addSelectedToCollection(); });
    connect(removeCollectionButton_, &QPushButton::clicked, this, [this]() { removeSelectedFromCollection(); });
    connect(addTrailButton_, &QPushButton::clicked, this, [this]() { addSelectedToLiteratureTrail(); });
    connect(manageTrailsButton_, &QPushButton::clicked, this, [this]() { showLiteratureTrails(); });
    connect(detailsTags_, &QListWidget::itemDoubleClicked, this, [this](QListWidgetItem *item) {
        if (item)
            setLibraryFilter(LibraryFilter::Tag, item->text());
    });
    connect(detailsCollections_, &QListWidget::itemDoubleClicked, this, [this](QListWidgetItem *item) {
        if (item)
            setLibraryFilter(LibraryFilter::Collection, item->text());
    });
    connect(detailsTrails_, &QListWidget::itemDoubleClicked, this, [this](QListWidgetItem *item) {
        if (item)
            setLibraryFilter(LibraryFilter::Trail, item->text());
    });
    connect(detailsFiles_, &QListWidget::itemDoubleClicked, this,
            [this](QListWidgetItem *) { openSelectedInReader(); });

    scroll->setWidget(panel);
    clearDetails();
    return scroll;
}

QWidget *MainWindow::buildReaderPage()
{
    auto *page = new QWidget(this);
    auto *layout = new QVBoxLayout(page);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(0);

    auto *controls = new QWidget(page);
    controls->setObjectName(QStringLiteral("readerControls"));
    auto *controlLayout = new QHBoxLayout(controls);
    controlLayout->setContentsMargins(8, 7, 8, 7);
    controlLayout->setSpacing(6);

    auto *backToLibrary = new QPushButton(QStringLiteral("← Library"), controls);
    readerHistoryBackButton_ = new QPushButton(QStringLiteral("←"), controls);
    readerHistoryBackButton_->setToolTip(QStringLiteral("Return to the previously opened paper and page"));
    readerHistoryForwardButton_ = new QPushButton(QStringLiteral("→"), controls);
    readerHistoryForwardButton_->setToolTip(QStringLiteral("Go forward in paper history"));
    readerHistoryBackButton_->setEnabled(false);
    readerHistoryForwardButton_->setEnabled(false);

    readerTitle_ = new QLabel(QStringLiteral("No document open"), controls);
    readerTitle_->setObjectName(QStringLiteral("readerTitle"));
    readerTitle_->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Preferred);

    readerPrevButton_ = new QPushButton(QStringLiteral("‹"), controls);
    readerPrevButton_->setToolTip(QStringLiteral("Previous page"));
    readerNextButton_ = new QPushButton(QStringLiteral("›"), controls);
    readerNextButton_->setToolTip(QStringLiteral("Next page"));
    pageSpin_ = new QSpinBox(controls);
    pageSpin_->setRange(1, 1);
    pageSpin_->setMaximumWidth(78);
    pageCountLabel_ = new QLabel(QStringLiteral("/ 0"), controls);

    auto *zoomOut = new QPushButton(QStringLiteral("−"), controls);
    zoomOut->setToolTip(QStringLiteral("Zoom out"));
    auto *zoomIn = new QPushButton(QStringLiteral("+"), controls);
    zoomIn->setToolTip(QStringLiteral("Zoom in"));
    auto *fitWidth = new QPushButton(QStringLiteral("Fit width"), controls);
    auto *fitPage = new QPushButton(QStringLiteral("Fit page"), controls);
    readerCitationToggleButton_ = new QPushButton(QStringLiteral("Citations"), controls);
    readerCitationToggleButton_->setCheckable(true);
    readerCitationToggleButton_->setChecked(true);
    readerCitationToggleButton_->setToolTip(QStringLiteral("Show or hide the citation navigator"));
    readerExternalButton_ = new QPushButton(QStringLiteral("External viewer"), controls);

    controlLayout->addWidget(backToLibrary);
    controlLayout->addWidget(readerHistoryBackButton_);
    controlLayout->addWidget(readerHistoryForwardButton_);
    controlLayout->addWidget(readerTitle_, 1);
    controlLayout->addWidget(readerPrevButton_);
    controlLayout->addWidget(readerNextButton_);
    controlLayout->addWidget(pageSpin_);
    controlLayout->addWidget(pageCountLabel_);
    controlLayout->addSpacing(8);
    controlLayout->addWidget(zoomOut);
    controlLayout->addWidget(zoomIn);
    controlLayout->addWidget(fitWidth);
    controlLayout->addWidget(fitPage);
    controlLayout->addSpacing(8);
    controlLayout->addWidget(readerCitationToggleButton_);
    controlLayout->addWidget(readerExternalButton_);
    layout->addWidget(controls);

    pdfDocument_ = new QPdfDocument(this);
    pdfView_ = new QPdfView(page);
    pdfView_->setDocument(pdfDocument_);
    pdfView_->setPageMode(QPdfView::PageMode::MultiPage);
    pdfView_->setZoomMode(QPdfView::ZoomMode::FitToWidth);
    pdfView_->setPageSpacing(8);

    auto *readerSplitter = new QSplitter(Qt::Horizontal, page);
    readerSplitter->setChildrenCollapsible(false);
    readerSplitter->addWidget(pdfView_);

    readerCitationPanel_ = new QWidget(readerSplitter);
    readerCitationPanel_->setObjectName(QStringLiteral("citationPanel"));
    readerCitationPanel_->setMinimumWidth(270);
    readerCitationPanel_->setMaximumWidth(460);
    auto *citationLayout = new QVBoxLayout(readerCitationPanel_);
    citationLayout->setContentsMargins(14, 14, 14, 14);
    citationLayout->setSpacing(8);

    auto *citationTitle = new QLabel(QStringLiteral("Citation navigator"), readerCitationPanel_);
    QFont citationTitleFont = citationTitle->font();
    citationTitleFont.setBold(true);
    citationTitleFont.setPointSize(citationTitleFont.pointSize() + 1);
    citationTitle->setFont(citationTitleFont);
    citationLayout->addWidget(citationTitle);

    readerCitationHeading_ = new QLabel(QStringLiteral("Open a paper to inspect citations."), readerCitationPanel_);
    readerCitationHeading_->setWordWrap(true);
    citationLayout->addWidget(readerCitationHeading_);

    readerCitationStatus_ = new QLabel(QStringLiteral("HEPShelf detects numeric citations such as [12] directly from the current PDF page."), readerCitationPanel_);
    readerCitationStatus_->setWordWrap(true);
    readerCitationStatus_->setObjectName(QStringLiteral("citationStatus"));
    citationLayout->addWidget(readerCitationStatus_);

    readerCitationList_ = new QListWidget(readerCitationPanel_);
    readerCitationList_->setSelectionMode(QAbstractItemView::SingleSelection);
    readerCitationList_->setAlternatingRowColors(true);
    readerCitationList_->setMinimumHeight(180);
    citationLayout->addWidget(readerCitationList_, 1);

    readerCitationPreview_ = new QLabel(QStringLiteral("Select a citation to see the referenced paper."), readerCitationPanel_);
    readerCitationPreview_->setWordWrap(true);
    readerCitationPreview_->setTextInteractionFlags(Qt::TextSelectableByMouse);
    readerCitationPreview_->setMinimumHeight(80);
    citationLayout->addWidget(readerCitationPreview_);

    auto *citationButtons = new QHBoxLayout();
    readerCitationOpenButton_ = new QPushButton(QStringLiteral("Open"), readerCitationPanel_);
    readerCitationArxivButton_ = new QPushButton(QStringLiteral("arXiv"), readerCitationPanel_);
    citationButtons->addWidget(readerCitationOpenButton_);
    citationButtons->addWidget(readerCitationArxivButton_);
    citationLayout->addLayout(citationButtons);

    readerCitationBrowseButton_ = new QPushButton(QStringLiteral("Browse all references…"), readerCitationPanel_);
    readerCitationRefreshButton_ = new QPushButton(QStringLiteral("Refresh from INSPIRE"), readerCitationPanel_);
    citationLayout->addWidget(readerCitationBrowseButton_);
    citationLayout->addWidget(readerCitationRefreshButton_);

    auto *hint = new QLabel(QStringLiteral("Tip: double-click a local citation to follow it. The reader Back button returns to the exact source page."), readerCitationPanel_);
    hint->setWordWrap(true);
    hint->setObjectName(QStringLiteral("citationHint"));
    citationLayout->addWidget(hint);

    readerSplitter->addWidget(readerCitationPanel_);
    readerSplitter->setStretchFactor(0, 1);
    readerSplitter->setStretchFactor(1, 0);
    readerSplitter->setSizes({1000, 330});
    layout->addWidget(readerSplitter, 1);

    connect(backToLibrary, &QPushButton::clicked, this, [this]() { tabs_->setCurrentIndex(0); });
    connect(readerHistoryBackButton_, &QPushButton::clicked, this, [this]() { readerGoBack(); });
    connect(readerHistoryForwardButton_, &QPushButton::clicked, this, [this]() { readerGoForward(); });
    connect(readerPrevButton_, &QPushButton::clicked, this, [this]() { readerPreviousPage(); });
    connect(readerNextButton_, &QPushButton::clicked, this, [this]() { readerNextPage(); });
    connect(zoomIn, &QPushButton::clicked, this, [this]() { readerZoomIn(); });
    connect(zoomOut, &QPushButton::clicked, this, [this]() { readerZoomOut(); });
    connect(fitWidth, &QPushButton::clicked, this, [this]() { readerFitWidth(); });
    connect(fitPage, &QPushButton::clicked, this, [this]() { readerFitPage(); });
    connect(readerCitationToggleButton_, &QPushButton::toggled, this, [this](bool visible) {
        if (readerCitationPanel_)
            readerCitationPanel_->setVisible(visible);
    });
    connect(readerExternalButton_, &QPushButton::clicked, this, [this]() {
        if (!currentReaderPath_.isEmpty())
            QDesktopServices::openUrl(QUrl::fromLocalFile(currentReaderPath_));
    });
    connect(readerCitationList_, &QListWidget::currentItemChanged, this,
            [this](QListWidgetItem *, QListWidgetItem *) { updateReaderCitationPreview(); });
    connect(readerCitationList_, &QListWidget::itemDoubleClicked, this,
            [this](QListWidgetItem *) { openSelectedReaderCitation(); });
    connect(readerCitationOpenButton_, &QPushButton::clicked, this, [this]() { openSelectedReaderCitation(); });
    connect(readerCitationArxivButton_, &QPushButton::clicked, this, [this]() {
        auto *item = readerCitationList_ ? readerCitationList_->currentItem() : nullptr;
        if (!item)
            return;
        const ReferenceRecord ref = readerReferenceForPosition(item->data(Qt::UserRole).toInt());
        if (!ref.arxivId.isEmpty())
            QDesktopServices::openUrl(QUrl(QStringLiteral("https://arxiv.org/abs/") + ref.arxivId));
    });
    connect(readerCitationBrowseButton_, &QPushButton::clicked, this, [this]() { openReaderReferenceBrowser(); });
    connect(readerCitationRefreshButton_, &QPushButton::clicked, this, [this]() { refreshReaderReferencesFromInspire(); });

    connect(pageSpin_, qOverload<int>(&QSpinBox::valueChanged), this, [this](int oneBasedPage) {
        if (pdfDocument_->status() != QPdfDocument::Status::Ready)
            return;
        const int currentPage = qBound(0, oneBasedPage - 1, qMax(0, pdfDocument_->pageCount() - 1));
        if (pdfView_->pageNavigator()->currentPage() != currentPage)
            pdfView_->pageNavigator()->jump(currentPage, QPointF(0, 0), 0);
    });
    connect(pdfDocument_, &QPdfDocument::statusChanged, this,
            [this](QPdfDocument::Status) { updateReaderForDocumentStatus(); });
    connect(pdfView_->pageNavigator(), &QPdfPageNavigator::currentPageChanged,
            this, [this](int currentPage) { updateReaderPage(currentPage); });

    connect(pdfView_->pageNavigator(), &QPdfPageNavigator::jumped,
            this, [this](const QPdfLink &link) { handlePdfJumped(link); });

    auto *historyBackShortcut = new QAction(page);
    historyBackShortcut->setShortcut(QKeySequence(QStringLiteral("Alt+Left")));
    historyBackShortcut->setShortcutContext(Qt::WidgetWithChildrenShortcut);
    page->addAction(historyBackShortcut);
    connect(historyBackShortcut, &QAction::triggered, this, [this]() { readerGoBack(); });

    auto *historyForwardShortcut = new QAction(page);
    historyForwardShortcut->setShortcut(QKeySequence(QStringLiteral("Alt+Right")));
    historyForwardShortcut->setShortcutContext(Qt::WidgetWithChildrenShortcut);
    page->addAction(historyForwardShortcut);
    connect(historyForwardShortcut, &QAction::triggered, this, [this]() { readerGoForward(); });

    auto *citationPanelShortcut = new QAction(page);
    citationPanelShortcut->setShortcut(QKeySequence(QStringLiteral("Ctrl+Shift+C")));
    citationPanelShortcut->setShortcutContext(Qt::WidgetWithChildrenShortcut);
    page->addAction(citationPanelShortcut);
    connect(citationPanelShortcut, &QAction::triggered, this, [this]() {
        readerCitationToggleButton_->toggle();
    });

    updateReaderHistoryButtons();
    updateReaderCitationPreview();
    return page;
}

void MainWindow::applyProfessionalStyle()
{
    setStyleSheet(QStringLiteral(R"QSS(
        QToolBar {
            spacing: 7px;
            padding: 7px 9px;
            border: 0;
            border-bottom: 1px solid palette(mid);
        }
        QLineEdit, QSpinBox {
            padding: 6px 8px;
            border: 1px solid palette(mid);
            border-radius: 5px;
            background: palette(base);
        }
        QPushButton, QToolButton {
            padding: 6px 10px;
            border: 1px solid palette(mid);
            border-radius: 5px;
            background: palette(button);
        }
        QPushButton:hover, QToolButton:hover {
            background: palette(midlight);
        }
        QPushButton:disabled, QToolButton:disabled {
            color: palette(mid);
        }
        QTreeWidget#navigation {
            border: 0;
            border-right: 1px solid palette(mid);
            padding: 8px 6px;
            background: palette(window);
        }
        QTreeWidget#navigation::item {
            padding: 9px 10px;
            margin: 2px 3px;
            border-radius: 5px;
        }
        QTreeWidget#navigation::item:selected {
            background: palette(highlight);
            color: palette(highlighted-text);
        }
        QTableWidget#libraryTable {
            border: 0;
            selection-background-color: palette(highlight);
            selection-color: palette(highlighted-text);
        }
        QHeaderView::section {
            padding: 8px;
            border: 0;
            border-bottom: 1px solid palette(mid);
            background: palette(button);
            font-weight: 600;
        }
        QScrollArea#detailsScroll {
            border-left: 1px solid palette(mid);
            background: palette(base);
        }
        QLabel#detailsTitle {
            font-size: 18px;
            font-weight: 700;
        }
        QLabel#detailsAuthors {
            color: palette(text);
            font-size: 13px;
        }
        QLabel#detailsMeta {
            color: palette(mid);
        }
        QLabel#sectionHeading {
            font-weight: 700;
            margin-top: 5px;
        }
        QTextBrowser#abstractView {
            border: 1px solid palette(mid);
            border-radius: 5px;
            padding: 5px;
            background: palette(base);
        }
        QWidget#readerControls {
            border-bottom: 1px solid palette(mid);
            background: palette(window);
        }
        QLabel#readerTitle {
            font-weight: 600;
        }
        QWidget#citationPanel {
            border-left: 1px solid palette(mid);
            background: palette(window);
        }
        QLabel#citationStatus, QLabel#citationHint {
            color: palette(mid);
        }
        QStatusBar {
            border-top: 1px solid palette(mid);
        }
    )QSS"));
}

void MainWindow::restoreUiState()
{
    QSettings settings;
    const QByteArray geometry = settings.value(QStringLiteral("ui/geometry")).toByteArray();
    if (!geometry.isEmpty())
        restoreGeometry(geometry);
    const QByteArray splitter = settings.value(QStringLiteral("ui/librarySplitter")).toByteArray();
    if (!splitter.isEmpty())
        librarySplitter_->restoreState(splitter);
    const QByteArray header = settings.value(QStringLiteral("ui/tableHeader")).toByteArray();
    if (!header.isEmpty())
        table_->horizontalHeader()->restoreState(header);
    table_->horizontalHeader()->setSectionResizeMode(QHeaderView::Interactive);
    table_->setColumnHidden(0, false);
    if (table_->horizontalHeader()->visualIndex(0) != 0)
        table_->horizontalHeader()->moveSection(table_->horizontalHeader()->visualIndex(0), 0);
    if (table_->columnWidth(0) < 240)
        table_->setColumnWidth(0, 430);

    currentFilter_ = static_cast<LibraryFilter>(settings.value(QStringLiteral("ui/filter"), 0).toInt());
    currentFacetValue_ = settings.value(QStringLiteral("ui/facetValue")).toString();

    QTreeWidgetItemIterator it(navigation_);
    while (*it) {
        QTreeWidgetItem *item = *it;
        if (item->data(0, RoleNavFilter).isValid()
            && item->data(0, RoleNavFilter).toInt() == static_cast<int>(currentFilter_)
            && item->data(0, RoleNavValue).toString() == currentFacetValue_) {
            navigation_->setCurrentItem(item);
            return;
        }
        ++it;
    }

    currentFilter_ = LibraryFilter::All;
    currentFacetValue_.clear();
    rebuildNavigation();
    refreshTable();
}

void MainWindow::saveUiState()
{
    QSettings settings;
    settings.setValue(QStringLiteral("ui/geometry"), saveGeometry());
    settings.setValue(QStringLiteral("ui/librarySplitter"), librarySplitter_->saveState());
    settings.setValue(QStringLiteral("ui/tableHeader"), table_->horizontalHeader()->saveState());
    settings.setValue(QStringLiteral("ui/filter"), static_cast<int>(currentFilter_));
    settings.setValue(QStringLiteral("ui/facetValue"), currentFacetValue_);
}

void MainWindow::closeEvent(QCloseEvent *event)
{
    if (paperNoteDirty_)
        saveCurrentPaperNote(true);
    saveUiState();
    QMainWindow::closeEvent(event);
}

void MainWindow::showArxivDiscovery(const QString &author)
{
    if (!databaseReady_)
        return;

    ArxivDiscoveryDialog dialog(&db_, this);
    if (!author.trimmed().isEmpty())
        dialog.setAuthorSearch(author, true);
    dialog.exec();

    if (dialog.libraryChanged()) {
        refreshTable();
        updateCounts();
        rebuildNavigation();
        showSelectedDetails();
        statusBar()->showMessage(QStringLiteral("arXiv downloads added to the local library."), 5000);
    }
}

void MainWindow::showArxivWatches()
{
    if (!databaseReady_ || !watches_)
        return;
    ArxivWatchesDialog dialog(watches_, &db_, this);
    dialog.exec();
}

void MainWindow::addFolder()
{
    if (!databaseReady_)
        return;
    const QString folder = QFileDialog::getExistingDirectory(
        this, QStringLiteral("Choose a folder containing papers"));
    if (folder.isEmpty())
        return;

    QString error;
    if (!db_.addFolder(folder, &error)) {
        showDatabaseError(QStringLiteral("Could not save the folder"), error);
        return;
    }
    rescan();
}

void MainWindow::manageFolders()
{
    if (!databaseReady_)
        return;

    QDialog dialog(this);
    dialog.setWindowTitle(QStringLiteral("Paper folders"));
    dialog.resize(650, 360);
    auto *layout = new QVBoxLayout(&dialog);
    auto *description = new QLabel(
        QStringLiteral("HEPShelf scans only these folders. Removing a folder removes its entries from the library but never deletes the PDF files."),
        &dialog);
    description->setWordWrap(true);
    layout->addWidget(description);

    auto *list = new QListWidget(&dialog);
    layout->addWidget(list, 1);

    QString error;
    const QStringList currentFolders = db_.folders(&error);
    if (!error.isEmpty()) {
        showDatabaseError(QStringLiteral("Could not read paper folders"), error);
        return;
    }
    list->addItems(currentFolders);

    auto *buttons = new QHBoxLayout();
    auto *add = new QPushButton(QStringLiteral("Add folder…"), &dialog);
    auto *remove = new QPushButton(QStringLiteral("Remove selected"), &dialog);
    auto *close = new QPushButton(QStringLiteral("Close"), &dialog);
    buttons->addWidget(add);
    buttons->addWidget(remove);
    buttons->addStretch();
    buttons->addWidget(close);
    layout->addLayout(buttons);

    connect(add, &QPushButton::clicked, &dialog, [this, list, &dialog]() {
        const QString folder = QFileDialog::getExistingDirectory(&dialog, QStringLiteral("Choose a paper folder"));
        if (folder.isEmpty())
            return;
        QString error;
        if (!db_.addFolder(folder, &error)) {
            showDatabaseError(QStringLiteral("Could not add paper folder"), error);
            return;
        }
        const QString absolute = QDir(folder).absolutePath();
        if (list->findItems(absolute, Qt::MatchExactly).isEmpty())
            list->addItem(absolute);
    });
    connect(remove, &QPushButton::clicked, &dialog, [this, list]() {
        auto *item = list->currentItem();
        if (!item)
            return;
        const QString folder = item->text();
        const auto answer = QMessageBox::question(
            this, QStringLiteral("Remove paper folder"),
            QStringLiteral("Stop watching this folder and remove its indexed entries from HEPShelf?\n\n%1\n\nThe PDF files themselves will not be deleted.").arg(folder));
        if (answer != QMessageBox::Yes)
            return;
        QString error;
        if (!db_.removeFolder(folder, true, &error)) {
            showDatabaseError(QStringLiteral("Could not remove paper folder"), error);
            return;
        }
        delete item;
        refreshTable();
        updateCounts();
        rebuildNavigation();
    });
    connect(close, &QPushButton::clicked, &dialog, &QDialog::accept);

    dialog.exec();
}

void MainWindow::rescan()
{
    if (!databaseReady_ || scanWatcher_.isRunning())
        return;

    QString error;
    const QStringList folders = db_.folders(&error);
    if (!error.isEmpty()) {
        showDatabaseError(QStringLiteral("Could not read the folder list"), error);
        return;
    }
    if (folders.isEmpty()) {
        statusBar()->showMessage(QStringLiteral("Add a paper folder first."), 5000);
        return;
    }

    scanAction_->setEnabled(false);
    addFolderAction_->setEnabled(false);
    manageFoldersAction_->setEnabled(false);
    progress_->setRange(0, 0);
    progress_->setFormat(QStringLiteral("Scanning…"));
    progress_->show();
    statusBar()->showMessage(QStringLiteral("Scanning registered folders in the background…"));

    scanWatcher_.setFuture(QtConcurrent::run([folders]() { return Scanner::scanFolders(folders); }));
}

void MainWindow::applyScanResult(const ScanResult &result)
{
    QString error;
    for (const QString &folder : result.scannedFolders) {
        if (!db_.markFolderUnseen(folder, &error)) {
            showDatabaseError(QStringLiteral("Could not prepare folder scan"), error);
            return;
        }
    }

    for (const ScannedFile &file : result.files) {
        if (!db_.upsertFile(file.path, file.arxivId, file.size, file.mtime, &error)) {
            showDatabaseError(QStringLiteral("Could not index %1").arg(file.path), error);
            return;
        }
    }

    for (const QString &folder : result.scannedFolders) {
        if (!db_.removeUnseenInFolder(folder, &error)) {
            showDatabaseError(QStringLiteral("Could not remove stale library entries"), error);
            return;
        }
    }

    refreshTable();
    updateCounts();
    rebuildNavigation();

    QString message = QStringLiteral("Scan complete: %1 PDFs examined, %2 arXiv papers recognized")
                          .arg(result.pdfCount)
                          .arg(result.recognizedCount);
    if (result.contentRecognizedCount > 0)
        message += QStringLiteral(" (%1 recognized from PDF contents)").arg(result.contentRecognizedCount);
    if (!result.contentDetectionAvailable)
        message += QStringLiteral(". Install poppler-utils to recognize renamed PDFs from their contents");
    if (!result.unavailableFolders.isEmpty())
        message += QStringLiteral(". %1 watched folder(s) were unavailable and left untouched")
                       .arg(result.unavailableFolders.size());
    statusBar()->showMessage(message, 12000);
}

void MainWindow::fetchMissingMetadata()
{
    if (!databaseReady_ || arxiv_.busy())
        return;

    QString error;
    const QStringList ids = db_.idsMissingMetadata(&error);
    if (!error.isEmpty()) {
        showDatabaseError(QStringLiteral("Could not determine missing metadata"), error);
        return;
    }
    if (ids.isEmpty()) {
        statusBar()->showMessage(QStringLiteral("Metadata is already up to date."), 3500);
        return;
    }

    fetchMetadataForIds(ids, QStringLiteral("Fetching missing metadata"));
}

void MainWindow::fetchMetadataForIds(const QStringList &ids, const QString &label)
{
    if (ids.isEmpty() || arxiv_.busy())
        return;

    metadataErrors_.clear();
    metadataAction_->setEnabled(false);
    scanAction_->setEnabled(false);
    discoverAction_->setEnabled(false);
    progress_->setRange(0, ids.size());
    progress_->setValue(0);
    progress_->setFormat(QStringLiteral("Metadata %v/%m"));
    progress_->show();
    statusBar()->showMessage(QStringLiteral("%1 for %2 paper(s)…").arg(label).arg(ids.size()));

    arxiv_.fetch(
        ids,
        [this](const QList<PaperRecord> &papers, const QString &networkError) {
            if (!networkError.isEmpty())
                metadataErrors_ << networkError;
            for (const PaperRecord &paper : papers) {
                QString dbError;
                if (!db_.upsertPaper(paper, &dbError))
                    showDatabaseError(QStringLiteral("Could not save metadata"), dbError);
            }
            refreshTable();
            updateCounts();
            rebuildNavigation();
            showSelectedDetails();
        },
        [this](int completed, int total) {
            progress_->setRange(0, total);
            progress_->setValue(completed);
            statusBar()->showMessage(QStringLiteral("Fetching metadata: %1 / %2").arg(completed).arg(total));
        },
        [this]() {
            refreshTable();
            updateCounts();
            rebuildNavigation();
            showSelectedDetails();
            metadataAction_->setEnabled(true);
            discoverAction_->setEnabled(true);
            if (!scanWatcher_.isRunning())
                scanAction_->setEnabled(true);
            progress_->hide();
            if (metadataErrors_.isEmpty()) {
                statusBar()->showMessage(QStringLiteral("Metadata update finished."), 5000);
            } else {
                statusBar()->showMessage(
                    QStringLiteral("Metadata finished with unresolved papers. %1")
                        .arg(metadataErrors_.join(QStringLiteral(" | "))),
                    20000);
            }
        });
}

void MainWindow::refreshSelectedMetadata()
{
    const QString id = selectedArxivId();
    if (!id.isEmpty())
        fetchMetadataForIds({id}, QStringLiteral("Refreshing metadata"));
}

void MainWindow::refreshTable()
{
    if (!databaseReady_ || !table_)
        return;

    QSet<QString> selectedIds;
    for (const QModelIndex &index : table_->selectionModel()->selectedRows()) {
        if (const QTableWidgetItem *item = table_->item(index.row(), 0))
            selectedIds.insert(item->data(RoleArxivId).toString());
    }
    const int sortColumn = table_->horizontalHeader()->sortIndicatorSection();
    const Qt::SortOrder sortOrder = table_->horizontalHeader()->sortIndicatorOrder();

    QString error;
    const QList<LibraryRow> rows = db_.search(search_ ? search_->text() : QString(), currentFilter_, currentFacetValue_, &error);
    if (!error.isEmpty()) {
        showDatabaseError(QStringLiteral("Could not read the library"), error);
        return;
    }

    {
    const QSignalBlocker blockSelection(table_->selectionModel());
    table_->setUpdatesEnabled(false);
    table_->setSortingEnabled(false);
    table_->clearContents();
    table_->setRowCount(rows.size());

    for (int r = 0; r < rows.size(); ++r) {
        const LibraryRow &row = rows.at(r);
        const QString shownTitle = row.favorite ? QStringLiteral("★  %1").arg(row.title) : row.title;

        auto *titleItem = new QTableWidgetItem(shownTitle);
        titleItem->setData(RoleArxivId, row.arxivId);
        titleItem->setData(RolePath, row.path);
        titleItem->setData(RoleFavorite, row.favorite);
        titleItem->setToolTip(row.title);

        auto *authorsItem = new QTableWidgetItem(row.authors);
        auto *idItem = new QTableWidgetItem(row.arxivId);
        auto *yearItem = new QTableWidgetItem(row.year);
        auto *categoryItem = new QTableWidgetItem(row.category);
        auto *citationsItem = new QTableWidgetItem(row.citationCount >= 0 ? QString::number(row.citationCount)
                                                                         : QStringLiteral("—"));
        if (row.citationCount >= 0)
            citationsItem->setData(Qt::DisplayRole, row.citationCount);
        auto *refsItem = new QTableWidgetItem(row.referenceCount >= 0 ? QString::number(row.referenceCount)
                                                                      : QStringLiteral("—"));
        if (row.referenceCount >= 0)
            refsItem->setData(Qt::DisplayRole, row.referenceCount);
        auto *copiesItem = new QTableWidgetItem();
        copiesItem->setData(Qt::DisplayRole, row.fileCount);

        table_->setItem(r, 0, titleItem);
        table_->setItem(r, 1, authorsItem);
        table_->setItem(r, 2, idItem);
        table_->setItem(r, 3, yearItem);
        table_->setItem(r, 4, categoryItem);
        table_->setItem(r, 5, citationsItem);
        table_->setItem(r, 6, refsItem);
        table_->setItem(r, 7, copiesItem);

    }

    if (currentFilter_ == LibraryFilter::Recent) {
        // Preserve the database's last-opened ordering in this view.
        table_->setSortingEnabled(false);
    } else {
        table_->setSortingEnabled(true);
        if (sortColumn >= 0)
            table_->sortItems(sortColumn, sortOrder);
    }
    table_->setUpdatesEnabled(true);

    bool restored = false;
    for (int r = 0; r < table_->rowCount(); ++r) {
        const QTableWidgetItem *item = table_->item(r, 0);
        if (item && selectedIds.contains(item->data(RoleArxivId).toString())) {
            table_->selectionModel()->select(table_->model()->index(r, 0),
                                             QItemSelectionModel::Select | QItemSelectionModel::Rows);
            restored = true;
        }
    }
    if (!restored && table_->rowCount() > 0)
        table_->selectRow(0);
    }
    showSelectedDetails();
}

void MainWindow::updateCounts()
{
    if (!databaseReady_)
        return;
    QString error1, error2;
    const int papers = db_.paperCount(&error1);
    const int files = db_.fileCount(&error2);
    if (!error1.isEmpty() || !error2.isEmpty())
        return;

    const int tags = db_.tagCounts(nullptr).size();
    const int collections = db_.collectionCounts(nullptr).size();
    summaryLabel_->setText(
        QStringLiteral("%1 papers · %2 local PDFs · %3 tags · %4 collections")
            .arg(papers)
            .arg(files)
            .arg(tags)
            .arg(collections));
}

void MainWindow::rebuildNavigation()
{
    if (!databaseReady_ || !navigation_)
        return;

    QString error;
    const int all = db_.paperCount(&error);
    if (!error.isEmpty())
        return;
    const int favorites = db_.favoriteCount(nullptr);
    const int missing = db_.missingMetadataCount(nullptr);
    const int unread = db_.unreadCount(nullptr);
    const int duplicates = db_.duplicatePaperCount(nullptr);
    const QList<FacetCount> collections = db_.collectionCounts(nullptr);
    const QList<FacetCount> trails = db_.trailCounts(nullptr);
    const QList<FacetCount> tags = db_.tagCounts(nullptr);
    const QList<FacetCount> authors = db_.authorCounts(nullptr);
    const QList<FacetCount> categories = db_.categoryCounts(nullptr);

    navigation_->blockSignals(true);
    navigation_->clear();

    auto makeRoot = [this](const QString &name) {
        auto *root = new QTreeWidgetItem(navigation_, QStringList(name));
        QFont font = root->font(0);
        font.setBold(true);
        root->setFont(0, font);
        root->setFlags(root->flags() & ~Qt::ItemIsSelectable);
        root->setExpanded(true);
        return root;
    };
    auto makeItem = [](QTreeWidgetItem *root,
                       const QString &label,
                       LibraryFilter filter,
                       const QString &value = QString()) {
        auto *item = new QTreeWidgetItem(root, QStringList(label));
        item->setData(0, RoleNavFilter, static_cast<int>(filter));
        item->setData(0, RoleNavValue, value);
        return item;
    };

    QTreeWidgetItem *libraryRoot = makeRoot(QStringLiteral("LIBRARY"));
    makeItem(libraryRoot, QStringLiteral("All papers  %1").arg(all), LibraryFilter::All);
    makeItem(libraryRoot, QStringLiteral("Favorites  %1").arg(favorites), LibraryFilter::Favorites);
    makeItem(libraryRoot, QStringLiteral("Recently opened"), LibraryFilter::Recent);
    makeItem(libraryRoot, QStringLiteral("Unread  %1").arg(unread), LibraryFilter::Unread);
    makeItem(libraryRoot, QStringLiteral("Duplicate copies  %1").arg(duplicates), LibraryFilter::Duplicates);
    makeItem(libraryRoot, QStringLiteral("Missing metadata  %1").arg(missing), LibraryFilter::MissingMetadata);

    QTreeWidgetItem *collectionsRoot = makeRoot(QStringLiteral("COLLECTIONS"));
    if (collections.isEmpty()) {
        auto *empty = new QTreeWidgetItem(collectionsRoot, QStringList(QStringLiteral("No collections yet")));
        empty->setFlags(empty->flags() & ~Qt::ItemIsSelectable);
    } else {
        for (const FacetCount &facet : collections)
            makeItem(collectionsRoot,
                     QStringLiteral("%1  %2").arg(facet.value).arg(facet.count),
                     LibraryFilter::Collection, facet.value);
    }

    QTreeWidgetItem *trailsRoot = makeRoot(QStringLiteral("LITERATURE TRAILS"));
    if (trails.isEmpty()) {
        auto *empty = new QTreeWidgetItem(trailsRoot, QStringList(QStringLiteral("No trails yet")));
        empty->setFlags(empty->flags() & ~Qt::ItemIsSelectable);
    } else {
        for (const FacetCount &facet : trails)
            makeItem(trailsRoot,
                     QStringLiteral("%1  %2").arg(facet.value).arg(facet.count),
                     LibraryFilter::Trail, facet.value);
    }

    QTreeWidgetItem *tagsRoot = makeRoot(QStringLiteral("TAGS"));
    if (tags.isEmpty()) {
        auto *empty = new QTreeWidgetItem(tagsRoot, QStringList(QStringLiteral("No tags yet")));
        empty->setFlags(empty->flags() & ~Qt::ItemIsSelectable);
    } else {
        for (const FacetCount &facet : tags)
            makeItem(tagsRoot,
                     QStringLiteral("%1  %2").arg(facet.value).arg(facet.count),
                     LibraryFilter::Tag, facet.value);
    }

    QTreeWidgetItem *authorsRoot = makeRoot(QStringLiteral("AUTHORS"));
    const int authorLimit = qMin(200, authors.size());
    for (int i = 0; i < authorLimit; ++i) {
        const FacetCount &facet = authors.at(i);
        makeItem(authorsRoot,
                 QStringLiteral("%1  %2").arg(facet.value).arg(facet.count),
                 LibraryFilter::Author, facet.value);
    }
    if (authors.size() > authorLimit) {
        auto *more = new QTreeWidgetItem(authorsRoot,
                                         QStringList(QStringLiteral("… %1 more (use Search)")
                                                         .arg(authors.size() - authorLimit)));
        more->setFlags(more->flags() & ~Qt::ItemIsSelectable);
    }

    QTreeWidgetItem *categoriesRoot = makeRoot(QStringLiteral("ARXIV CATEGORIES"));
    for (const FacetCount &facet : categories)
        makeItem(categoriesRoot,
                 QStringLiteral("%1  %2").arg(facet.value).arg(facet.count),
                 LibraryFilter::Category, facet.value);

    QTreeWidgetItem *toSelect = nullptr;
    QTreeWidgetItemIterator it(navigation_);
    while (*it) {
        QTreeWidgetItem *item = *it;
        if (item->data(0, RoleNavFilter).isValid()
            && item->data(0, RoleNavFilter).toInt() == static_cast<int>(currentFilter_)
            && item->data(0, RoleNavValue).toString() == currentFacetValue_) {
            toSelect = item;
            break;
        }
        ++it;
    }
    if (!toSelect && libraryRoot->childCount() > 0) {
        currentFilter_ = LibraryFilter::All;
        currentFacetValue_.clear();
        toSelect = libraryRoot->child(0);
    }
    navigation_->setCurrentItem(toSelect);
    navigation_->blockSignals(false);
}

void MainWindow::setLibraryFilter(LibraryFilter filter, const QString &facetValue)
{
    currentFilter_ = filter;
    currentFacetValue_ = facetValue;

    if (navigation_) {
        QTreeWidgetItemIterator it(navigation_);
        while (*it) {
            QTreeWidgetItem *item = *it;
            if (item->data(0, RoleNavFilter).isValid()
                && item->data(0, RoleNavFilter).toInt() == static_cast<int>(filter)
                && item->data(0, RoleNavValue).toString() == facetValue) {
                if (navigation_->currentItem() != item) {
                    navigation_->blockSignals(true);
                    navigation_->setCurrentItem(item);
                    navigation_->blockSignals(false);
                }
                break;
            }
            ++it;
        }
    }
    refreshTable();
}

void MainWindow::showNavigationContextMenu(const QPoint &pos)
{
    QTreeWidgetItem *item = navigation_->itemAt(pos);
    QMenu menu(this);
    menu.addAction(QStringLiteral("Create collection…"), this, [this]() { createCollection(); });
    menu.addAction(QStringLiteral("Literature trails…"), this, [this]() { showLiteratureTrails(); });

    if (item && item->data(0, RoleNavFilter).isValid()) {
        const auto filter = static_cast<LibraryFilter>(item->data(0, RoleNavFilter).toInt());
        const QString value = item->data(0, RoleNavValue).toString();
        if (filter == LibraryFilter::Author && !value.isEmpty()) {
            menu.addSeparator();
            menu.addAction(QStringLiteral("Find more papers by “%1” on arXiv…").arg(value),
                           this, [this, value]() { showArxivDiscovery(value); });
        } else if (filter == LibraryFilter::Tag && !value.isEmpty()) {
            menu.addSeparator();
            menu.addAction(QStringLiteral("Delete tag “%1”…").arg(value), this, [this, value]() {
                if (QMessageBox::question(this, QStringLiteral("Delete tag"),
                                          QStringLiteral("Delete the tag “%1” from the library?\n\nNo PDF files will be changed.").arg(value))
                    != QMessageBox::Yes)
                    return;
                QString error;
                if (!db_.deleteTag(value, &error)) {
                    showDatabaseError(QStringLiteral("Could not delete tag"), error);
                    return;
                }
                if (currentFilter_ == LibraryFilter::Tag && currentFacetValue_.compare(value, Qt::CaseInsensitive) == 0) {
                    currentFilter_ = LibraryFilter::All;
                    currentFacetValue_.clear();
                }
                rebuildNavigation();
                refreshTable();
                showSelectedDetails();
            });
        } else if (filter == LibraryFilter::Trail && !value.isEmpty()) {
            menu.addSeparator();
            menu.addAction(QStringLiteral("Manage literature trails…"), this, [this]() { showLiteratureTrails(); });
        } else if (filter == LibraryFilter::Collection && !value.isEmpty()) {
            menu.addSeparator();
            menu.addAction(QStringLiteral("Delete collection “%1”…").arg(value), this, [this, value]() {
                if (QMessageBox::question(this, QStringLiteral("Delete collection"),
                                          QStringLiteral("Delete the collection “%1”?\n\nPapers and PDF files will stay in the library.").arg(value))
                    != QMessageBox::Yes)
                    return;
                QString error;
                if (!db_.deleteCollection(value, &error)) {
                    showDatabaseError(QStringLiteral("Could not delete collection"), error);
                    return;
                }
                if (currentFilter_ == LibraryFilter::Collection && currentFacetValue_.compare(value, Qt::CaseInsensitive) == 0) {
                    currentFilter_ = LibraryFilter::All;
                    currentFacetValue_.clear();
                }
                rebuildNavigation();
                refreshTable();
                showSelectedDetails();
            });
        }
    }

    menu.exec(navigation_->viewport()->mapToGlobal(pos));
}

QString MainWindow::selectedArxivId() const
{
    if (!table_)
        return {};
    const auto rows = table_->selectionModel()->selectedRows();
    if (rows.isEmpty())
        return {};
    QTableWidgetItem *item = table_->item(rows.first().row(), 0);
    return item ? item->data(RoleArxivId).toString() : QString();
}

QString MainWindow::selectedLocalPath() const
{
    if (detailsFiles_ && detailsFiles_->currentItem()) {
        const QString path = detailsFiles_->currentItem()->data(Qt::UserRole).toString();
        if (!path.isEmpty())
            return path;
    }

    if (!table_)
        return {};
    const auto rows = table_->selectionModel()->selectedRows();
    if (rows.isEmpty())
        return {};
    QTableWidgetItem *item = table_->item(rows.first().row(), 0);
    return item ? item->data(RolePath).toString() : QString();
}

void MainWindow::showSelectedDetails()
{
    const QString id = selectedArxivId();
    if (id.isEmpty()) {
        clearDetails();
        return;
    }
    showPaperDetails(id);
}

void MainWindow::showPaperDetails(const QString &arxivId)
{
    if (paperNoteDirty_ && !currentDetailsArxivId_.isEmpty())
        saveCurrentPaperNote(true);

    PaperDetails details;
    QString error;
    if (!db_.paperDetails(arxivId, &details, &error)) {
        clearDetails();
        return;
    }

    detailsTitle_->setText(displayTitle(details));
    detailsAuthors_->setText(details.paper.authors.isEmpty() ? QStringLiteral("Authors not available") : details.paper.authors);

    QStringList meta;
    meta << QStringLiteral("arXiv %1").arg(details.paper.arxivId);
    const QString year = yearFromDate(details.paper.published);
    if (!year.isEmpty())
        meta << year;
    if (!details.paper.primaryCategory.isEmpty())
        meta << details.paper.primaryCategory;
    if (!details.paper.doi.isEmpty())
        meta << QStringLiteral("DOI %1").arg(details.paper.doi);
    if (details.pageCount > 0)
        meta << formatProgress(details.lastPage, details.pageCount);
    if (details.files.size() > 1)
        meta << QStringLiteral("%1 local copies").arg(details.files.size());
    detailsMeta_->setText(meta.join(QStringLiteral("  ·  ")));

    QStringList journal;
    if (!details.paper.journalRef.isEmpty())
        journal << details.paper.journalRef;
    if (!details.paper.comments.isEmpty())
        journal << details.paper.comments;
    detailsJournal_->setText(journal.join(QStringLiteral("\n")));
    detailsJournal_->setVisible(!journal.isEmpty());

    QStringList citationParts;
    if (details.citations.citationCount >= 0)
        citationParts << QStringLiteral("Citations %1").arg(details.citations.citationCount);
    if (details.citations.citationCountWithoutSelf >= 0)
        citationParts << QStringLiteral("without self-citations %1").arg(details.citations.citationCountWithoutSelf);
    if (details.citations.referenceCount >= 0) {
        const QList<ReferenceRecord> cachedRefs = db_.referencesForPaper(details.paper.arxivId, nullptr);
        int localReferences = 0;
        for (const ReferenceRecord &ref : cachedRefs) {
            if (ref.local)
                ++localReferences;
        }
        if (!cachedRefs.isEmpty())
            citationParts << QStringLiteral("References %1 (%2 local)")
                                 .arg(details.citations.referenceCount)
                                 .arg(localReferences);
        else
            citationParts << QStringLiteral("References %1").arg(details.citations.referenceCount);
    }
    const CitingCacheInfo citedByCache = db_.citingCacheInfo(details.paper.arxivId, nullptr);
    if (!citedByCache.fetchedAt.isEmpty()) {
        const QList<RelatedPaperRecord> cachedCiting = db_.citingPapersForPaper(details.paper.arxivId, nullptr);
        int localCiting = 0;
        for (const RelatedPaperRecord &paper : cachedCiting) {
            if (paper.local)
                ++localCiting;
        }
        citationParts << QStringLiteral("Cited-by cached %1 (%2 local)")
                             .arg(citedByCache.totalCount >= 0 ? citedByCache.totalCount : cachedCiting.size())
                             .arg(localCiting);
    }
    if (details.citations.inspireRecid > 0)
        citationParts << QStringLiteral("INSPIRE %1").arg(details.citations.inspireRecid);
    detailsCitationMetrics_->setText(citationParts.isEmpty()
                                         ? QStringLiteral("INSPIRE metrics not fetched yet")
                                         : citationParts.join(QStringLiteral("  ·  ")));

    detailsAbstract_->setPlainText(details.paper.abstractText.isEmpty()
                                       ? QStringLiteral("No abstract is cached for this paper yet.")
                                       : details.paper.abstractText);

    currentDetailsArxivId_ = arxivId;
    const PaperNoteRecord note = db_.paperNote(arxivId, nullptr);
    loadingPaperNote_ = true;
    detailsNotes_->setPlainText(note.text);
    loadingPaperNote_ = false;
    paperNoteDirty_ = false;
    detailsNotes_->setEnabled(true);
    detailsNotesStatus_->setText(note.updatedAt.isEmpty()
                                     ? QStringLiteral("Autosaved locally")
                                     : QStringLiteral("Saved %1").arg(note.updatedAt));
    saveNoteAction_->setEnabled(true);

    detailsTags_->clear();
    for (const QString &tag : details.tags)
        detailsTags_->addItem(tag);
    if (detailsTags_->count() > 0)
        detailsTags_->setCurrentRow(0);

    detailsCollections_->clear();
    for (const QString &collection : details.collections)
        detailsCollections_->addItem(collection);
    if (detailsCollections_->count() > 0)
        detailsCollections_->setCurrentRow(0);

    detailsTrails_->clear();
    const QList<LiteratureTrailRecord> trails = db_.literatureTrailsForPaper(arxivId, nullptr);
    for (const LiteratureTrailRecord &trail : trails)
        detailsTrails_->addItem(trail.name);
    if (detailsTrails_->count() > 0)
        detailsTrails_->setCurrentRow(0);

    detailsFiles_->clear();
    for (const FileRecord &file : details.files) {
        const QString version = file.version.isEmpty() ? QStringLiteral("local") : file.version;
        const QString label = QStringLiteral("%1  ·  %2  ·  %3")
                                  .arg(version, humanBytes(file.size), file.path);
        auto *item = new QListWidgetItem(label, detailsFiles_);
        item->setData(Qt::UserRole, file.path);
        item->setToolTip(file.path);
        if (!QFileInfo::exists(file.path))
            item->setText(QStringLiteral("Missing  ·  %1").arg(file.path));
    }
    if (detailsFiles_->count() > 0)
        detailsFiles_->setCurrentRow(0);

    favoriteButton_->setEnabled(true);
    favoriteButton_->setChecked(details.favorite);
    favoriteButton_->setText(details.favorite ? QStringLiteral("★") : QStringLiteral("☆"));
    favoriteButton_->setToolTip(details.favorite ? QStringLiteral("Remove from favorites")
                                                  : QStringLiteral("Add to favorites"));

    const bool hasFile = !details.paths.isEmpty();
    readerButton_->setEnabled(hasFile);
    externalButton_->setEnabled(hasFile);
    addTagButton_->setEnabled(true);
    removeTagButton_->setEnabled(!details.tags.isEmpty());
    addCollectionButton_->setEnabled(true);
    removeCollectionButton_->setEnabled(!details.collections.isEmpty());
    addTrailButton_->setEnabled(true);
    manageTrailsButton_->setEnabled(true);
    arxivButton_->setEnabled(!details.paper.arxivId.isEmpty());
    inspireButton_->setEnabled(!details.paper.arxivId.isEmpty());
    referencesButton_->setEnabled(!details.paper.arxivId.isEmpty());
    citedByButton_->setEnabled(!details.paper.arxivId.isEmpty());
    citationGraphButton_->setEnabled(!details.paper.arxivId.isEmpty());
    refreshInspireButton_->setEnabled(!details.paper.arxivId.isEmpty());
}

void MainWindow::clearDetails()
{
    if (!detailsTitle_)
        return;
    if (paperNoteDirty_ && !currentDetailsArxivId_.isEmpty())
        saveCurrentPaperNote(true);
    currentDetailsArxivId_.clear();
    paperNoteDirty_ = false;
    detailsTitle_->setText(QStringLiteral("Select a paper"));
    detailsAuthors_->clear();
    detailsMeta_->clear();
    detailsJournal_->clear();
    detailsJournal_->hide();
    detailsCitationMetrics_->clear();
    detailsAbstract_->clear();
    loadingPaperNote_ = true;
    detailsNotes_->clear();
    loadingPaperNote_ = false;
    detailsNotes_->setEnabled(false);
    detailsNotesStatus_->clear();
    detailsFiles_->clear();
    detailsTags_->clear();
    detailsCollections_->clear();
    detailsTrails_->clear();
    favoriteButton_->setEnabled(false);
    favoriteButton_->setChecked(false);
    favoriteButton_->setText(QStringLiteral("☆"));
    readerButton_->setEnabled(false);
    externalButton_->setEnabled(false);
    addTagButton_->setEnabled(false);
    removeTagButton_->setEnabled(false);
    addCollectionButton_->setEnabled(false);
    removeCollectionButton_->setEnabled(false);
    addTrailButton_->setEnabled(false);
    manageTrailsButton_->setEnabled(databaseReady_);
    saveNoteAction_->setEnabled(false);
    arxivButton_->setEnabled(false);
    inspireButton_->setEnabled(false);
    referencesButton_->setEnabled(false);
    citedByButton_->setEnabled(false);
    citationGraphButton_->setEnabled(false);
    refreshInspireButton_->setEnabled(false);
}

void MainWindow::toggleSelectedFavorite()
{
    const QString id = selectedArxivId();
    if (id.isEmpty())
        return;

    PaperDetails details;
    QString error;
    if (!db_.paperDetails(id, &details, &error)) {
        showDatabaseError(QStringLiteral("Could not read paper"), error);
        return;
    }
    if (!db_.setFavorite(id, !details.favorite, &error)) {
        showDatabaseError(QStringLiteral("Could not update favorite"), error);
        return;
    }

    refreshTable();
    rebuildNavigation();
    showPaperDetails(id);
}

void MainWindow::addTagToSelected()
{
    const QString id = selectedArxivId();
    if (id.isEmpty())
        return;

    bool ok = false;
    const QString tag = QInputDialog::getText(this, QStringLiteral("Add tag"),
                                              QStringLiteral("Tag name:"),
                                              QLineEdit::Normal, QString(), &ok).simplified();
    if (!ok || tag.isEmpty())
        return;

    QString error;
    if (!db_.addTagToPaper(id, tag, &error)) {
        showDatabaseError(QStringLiteral("Could not add tag"), error);
        return;
    }
    rebuildNavigation();
    refreshTable();
    showPaperDetails(id);
}

void MainWindow::removeTagFromSelected()
{
    const QString id = selectedArxivId();
    QListWidgetItem *item = detailsTags_ ? detailsTags_->currentItem() : nullptr;
    if (id.isEmpty() || !item)
        return;

    QString error;
    if (!db_.removeTagFromPaper(id, item->text(), &error)) {
        showDatabaseError(QStringLiteral("Could not remove tag"), error);
        return;
    }
    rebuildNavigation();
    refreshTable();
    showPaperDetails(id);
}

void MainWindow::addSelectedToCollection()
{
    const QString id = selectedArxivId();
    if (id.isEmpty())
        return;

    QStringList names;
    for (const FacetCount &facet : db_.collectionCounts(nullptr))
        names << facet.value;

    bool ok = false;
    const QString collection = QInputDialog::getItem(this,
                                                      QStringLiteral("Add to collection"),
                                                      QStringLiteral("Collection name (type a new name or choose one):"),
                                                      names, 0, true, &ok).simplified();
    if (!ok || collection.isEmpty())
        return;

    QString error;
    if (!db_.addPaperToCollection(id, collection, &error)) {
        showDatabaseError(QStringLiteral("Could not add paper to collection"), error);
        return;
    }
    rebuildNavigation();
    refreshTable();
    showPaperDetails(id);
}

void MainWindow::removeSelectedFromCollection()
{
    const QString id = selectedArxivId();
    QListWidgetItem *item = detailsCollections_ ? detailsCollections_->currentItem() : nullptr;
    if (id.isEmpty() || !item)
        return;

    QString error;
    if (!db_.removePaperFromCollection(id, item->text(), &error)) {
        showDatabaseError(QStringLiteral("Could not remove paper from collection"), error);
        return;
    }
    rebuildNavigation();
    refreshTable();
    showPaperDetails(id);
}

void MainWindow::createCollection()
{
    bool ok = false;
    const QString name = QInputDialog::getText(this, QStringLiteral("New collection"),
                                               QStringLiteral("Collection name:"),
                                               QLineEdit::Normal, QString(), &ok).simplified();
    if (!ok || name.isEmpty())
        return;
    QString error;
    if (!db_.createCollection(name, &error)) {
        showDatabaseError(QStringLiteral("Could not create collection"), error);
        return;
    }
    rebuildNavigation();
    statusBar()->showMessage(QStringLiteral("Created collection “%1”.").arg(name), 4000);
}

void MainWindow::saveCurrentPaperNote(bool quiet)
{
    if (!databaseReady_ || currentDetailsArxivId_.isEmpty() || !detailsNotes_)
        return;
    if (noteSaveTimer_)
        noteSaveTimer_->stop();
    if (!paperNoteDirty_) {
        if (!quiet && detailsNotesStatus_)
            detailsNotesStatus_->setText(QStringLiteral("Already saved"));
        return;
    }

    QString error;
    if (!db_.setPaperNote(currentDetailsArxivId_, detailsNotes_->toPlainText(), &error)) {
        if (detailsNotesStatus_)
            detailsNotesStatus_->setText(QStringLiteral("Save failed"));
        if (!quiet)
            showDatabaseError(QStringLiteral("Could not save paper note"), error);
        return;
    }
    paperNoteDirty_ = false;
    const PaperNoteRecord saved = db_.paperNote(currentDetailsArxivId_, nullptr);
    if (detailsNotesStatus_)
        detailsNotesStatus_->setText(saved.updatedAt.isEmpty()
                                         ? QStringLiteral("Saved")
                                         : QStringLiteral("Saved %1").arg(saved.updatedAt));
}

void MainWindow::showLiteratureTrails(int selectTrailId)
{
    if (!databaseReady_)
        return;
    LiteratureTrailsDialog dialog(
        &db_,
        [this](const QString &arxivId, const QString &path) {
            openPaperInReader(arxivId, path, true);
        },
        this);
    if (selectTrailId > 0)
        dialog.selectTrail(selectTrailId);
    dialog.exec();
    if (dialog.dataChanged()) {
        rebuildNavigation();
        refreshTable();
        showSelectedDetails();
    }
}

void MainWindow::addSelectedToLiteratureTrail()
{
    const QString id = selectedArxivId();
    if (id.isEmpty())
        return;

    PaperDetails details;
    QString error;
    if (!db_.paperDetails(id, &details, &error)) {
        showDatabaseError(QStringLiteral("Could not read paper"), error);
        return;
    }

    const QList<LiteratureTrailRecord> trails = db_.literatureTrails(&error);
    if (!error.isEmpty()) {
        showDatabaseError(QStringLiteral("Could not read literature trails"), error);
        return;
    }

    QStringList choices;
    choices << QStringLiteral("+ New literature trail…");
    for (const LiteratureTrailRecord &trail : trails)
        choices << trail.name;

    bool ok = false;
    const QString choice = QInputDialog::getItem(this, QStringLiteral("Add to literature trail"),
                                                 QStringLiteral("Choose a trail:"), choices, 0, false, &ok);
    if (!ok || choice.isEmpty())
        return;

    int trailId = 0;
    if (choice.startsWith(QLatin1Char('+'))) {
        const QString name = QInputDialog::getText(this, QStringLiteral("New literature trail"),
                                                   QStringLiteral("Trail name:"), QLineEdit::Normal,
                                                   QString(), &ok).trimmed();
        if (!ok || name.isEmpty())
            return;
        trailId = db_.createLiteratureTrail(name, {}, &error);
        if (trailId <= 0) {
            showDatabaseError(QStringLiteral("Could not create literature trail"), error);
            return;
        }
    } else {
        for (const LiteratureTrailRecord &trail : trails) {
            if (trail.name.compare(choice, Qt::CaseInsensitive) == 0) {
                trailId = trail.id;
                break;
            }
        }
    }

    if (trailId <= 0)
        return;

    TrailItemRecord item;
    item.arxivId = details.paper.arxivId;
    item.inspireRecid = details.citations.inspireRecid;
    item.doi = details.paper.doi;
    item.title = displayTitle(details);
    item.authors = details.paper.authors;
    item.year = yearFromDate(details.paper.published);
    if (!db_.addTrailItem(trailId, item, &error)) {
        showDatabaseError(QStringLiteral("Could not add paper to literature trail"), error);
        return;
    }

    rebuildNavigation();
    showPaperDetails(id);
    statusBar()->showMessage(QStringLiteral("Paper added to literature trail."), 4000);
}

QString MainWindow::expandOrganizationTemplate(const QString &pattern,
                                               const PaperDetails &details,
                                               const QString &sourcePath) const
{
    QString result = pattern.trimmed();
    QString arxiv = details.paper.arxivId;
    arxiv.replace(QLatin1Char('/'), QLatin1Char('_'));
    arxiv = safePathPart(arxiv);
    const QString year = yearFromDate(details.paper.published).isEmpty()
                             ? QStringLiteral("UnknownYear")
                             : yearFromDate(details.paper.published);
    const QString author = firstAuthorKey(details);
    const QString title = safePathPart(displayTitle(details));
    const QString category = safePathPart(details.paper.primaryCategory.isEmpty()
                                              ? QStringLiteral("uncategorized")
                                              : details.paper.primaryCategory);

    QString version;
    for (const FileRecord &file : details.files) {
        if (QDir::cleanPath(file.path) == QDir::cleanPath(sourcePath)) {
            version = file.version;
            break;
        }
    }

    result.replace(QStringLiteral("{arxiv}"), arxiv);
    result.replace(QStringLiteral("{year}"), safePathPart(year));
    result.replace(QStringLiteral("{first_author}"), author);
    result.replace(QStringLiteral("{title}"), title);
    result.replace(QStringLiteral("{category}"), category);
    result.replace(QStringLiteral("{version}"), safePathPart(version));

    result.replace(QLatin1Char('\\'), QLatin1Char('/'));
    while (result.startsWith(QLatin1Char('/')))
        result.remove(0, 1);
    result = QDir::cleanPath(result);
    return result;
}

void MainWindow::organizeSelectedFile(bool renameOnly)
{
    const QString id = selectedArxivId();
    const QString sourcePath = selectedLocalPath();
    if (id.isEmpty() || sourcePath.isEmpty())
        return;
    if (!QFileInfo::exists(sourcePath)) {
        QMessageBox::warning(this, QStringLiteral("HEPShelf"),
                             QStringLiteral("The selected local file no longer exists:\n%1").arg(sourcePath));
        return;
    }

    PaperDetails details;
    QString error;
    if (!db_.paperDetails(id, &details, &error)) {
        showDatabaseError(QStringLiteral("Could not read paper metadata"), error);
        return;
    }

    QDialog dialog(this);
    dialog.setWindowTitle(renameOnly ? QStringLiteral("Rename PDF using metadata")
                                     : QStringLiteral("Organize / move PDF"));
    dialog.resize(720, 330);
    auto *outer = new QVBoxLayout(&dialog);
    auto *intro = new QLabel(renameOnly
        ? QStringLiteral("Preview a metadata-based filename before changing anything. HEPShelf updates its index after the rename.")
        : QStringLiteral("Move this local PDF into an organized folder. Nothing is overwritten, and HEPShelf updates its index only after the move succeeds."),
        &dialog);
    intro->setWordWrap(true);
    outer->addWidget(intro);

    auto *form = new QFormLayout();
    auto *sourceLabel = new QLabel(sourcePath, &dialog);
    sourceLabel->setWordWrap(true);
    sourceLabel->setTextInteractionFlags(Qt::TextSelectableByMouse);
    form->addRow(QStringLiteral("Source:"), sourceLabel);

    QSettings organizerSettings;
    QLineEdit *destination = nullptr;
    QPushButton *browse = nullptr;
    QCheckBox *watchDestination = nullptr;
    if (!renameOnly) {
        auto *destRowWidget = new QWidget(&dialog);
        auto *destRow = new QHBoxLayout(destRowWidget);
        destRow->setContentsMargins(0, 0, 0, 0);
        const QString rememberedRoot = organizerSettings.value(QStringLiteral("organize/root"), QFileInfo(sourcePath).absolutePath()).toString();
        destination = new QLineEdit(rememberedRoot, destRowWidget);
        browse = new QPushButton(QStringLiteral("Browse…"), destRowWidget);
        destRow->addWidget(destination, 1);
        destRow->addWidget(browse);
        form->addRow(QStringLiteral("Library root:"), destRowWidget);

        watchDestination = new QCheckBox(QStringLiteral("Add this destination root to HEPShelf's watched folders"), &dialog);
        watchDestination->setChecked(true);
        form->addRow(QString(), watchDestination);
    }

    auto *pattern = new QComboBox(&dialog);
    pattern->setEditable(true);
    if (renameOnly) {
        pattern->addItems({QStringLiteral("{arxiv}{version}.pdf"),
                           QStringLiteral("{first_author}_{year}_{arxiv}{version}.pdf"),
                           QStringLiteral("{first_author}_{year}_{title}.pdf")});
    } else {
        pattern->addItems({QStringLiteral("{arxiv}{version}.pdf"),
                           QStringLiteral("{first_author}_{year}_{arxiv}{version}.pdf"),
                           QStringLiteral("{year}/{first_author}/{arxiv}{version}.pdf"),
                           QStringLiteral("{category}/{year}/{first_author}_{arxiv}{version}.pdf"),
                           QStringLiteral("{year}/{first_author}/{title}.pdf")});
    }
    const QString settingKey = renameOnly ? QStringLiteral("organize/renameTemplate")
                                           : QStringLiteral("organize/moveTemplate");
    const QString defaultPattern = renameOnly ? QStringLiteral("{first_author}_{year}_{arxiv}{version}.pdf")
                                              : QStringLiteral("{year}/{first_author}/{arxiv}{version}.pdf");
    pattern->setCurrentText(organizerSettings.value(settingKey, defaultPattern).toString());
    form->addRow(QStringLiteral("Template:"), pattern);

    auto *hint = new QLabel(QStringLiteral("Available fields: {arxiv}, {version}, {year}, {first_author}, {category}, {title}"), &dialog);
    hint->setWordWrap(true);
    form->addRow(QString(), hint);

    auto *preview = new QLabel(&dialog);
    preview->setWordWrap(true);
    preview->setTextInteractionFlags(Qt::TextSelectableByMouse);
    form->addRow(QStringLiteral("Target:"), preview);
    outer->addLayout(form);

    auto updatePreview = [this, renameOnly, sourcePath, &details, destination, pattern, preview]() {
        const QString relative = expandOrganizationTemplate(pattern->currentText(), details, sourcePath);
        const QString root = renameOnly ? QFileInfo(sourcePath).absolutePath() : destination->text().trimmed();
        const QString target = QDir(root).filePath(relative);
        preview->setText(QDir::cleanPath(target));
    };
    if (browse) {
        connect(browse, &QPushButton::clicked, &dialog, [this, destination, &dialog, updatePreview]() {
            const QString folder = QFileDialog::getExistingDirectory(&dialog, QStringLiteral("Choose destination folder"), destination->text());
            if (!folder.isEmpty()) {
                destination->setText(folder);
                updatePreview();
            }
        });
        connect(destination, &QLineEdit::textChanged, &dialog, [updatePreview]() { updatePreview(); });
    }
    connect(pattern->lineEdit(), &QLineEdit::textChanged, &dialog, [updatePreview]() { updatePreview(); });
    updatePreview();

    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Cancel | QDialogButtonBox::Ok, &dialog);
    buttons->button(QDialogButtonBox::Ok)->setText(renameOnly ? QStringLiteral("Rename") : QStringLiteral("Move PDF"));
    outer->addWidget(buttons);
    connect(buttons, &QDialogButtonBox::accepted, &dialog, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);

    if (dialog.exec() != QDialog::Accepted)
        return;

    const QString relative = expandOrganizationTemplate(pattern->currentText(), details, sourcePath);
    if (!safeOrganizationRelative(relative)) {
        QMessageBox::warning(this, QStringLiteral("HEPShelf"), QStringLiteral("The organization template produced an unsafe path."));
        return;
    }
    if (renameOnly && relative.contains(QLatin1Char('/'))) {
        QMessageBox::warning(this, QStringLiteral("HEPShelf"),
                             QStringLiteral("Rename templates must produce a filename, not a subdirectory path."));
        return;
    }

    if (!renameOnly && destination->text().trimmed().isEmpty()) {
        QMessageBox::warning(this, QStringLiteral("HEPShelf"), QStringLiteral("Choose a destination folder first."));
        return;
    }
    const QString destinationRoot = renameOnly ? QFileInfo(sourcePath).absolutePath()
                                               : QDir(destination->text().trimmed()).absolutePath();
    if (destinationRoot.isEmpty())
        return;
    const QString targetPath = QDir::cleanPath(QDir(destinationRoot).filePath(relative));
    if (QDir::cleanPath(sourcePath) == targetPath) {
        statusBar()->showMessage(QStringLiteral("The PDF already has that path."), 4000);
        return;
    }
    if (QFileInfo::exists(targetPath)) {
        QMessageBox::warning(this, QStringLiteral("HEPShelf"),
                             QStringLiteral("The target already exists. HEPShelf will not overwrite it:\n\n%1").arg(targetPath));
        return;
    }

    const QString targetDir = QFileInfo(targetPath).absolutePath();
    if (!QDir().mkpath(targetDir)) {
        QMessageBox::warning(this, QStringLiteral("HEPShelf"),
                             QStringLiteral("Could not create destination folder:\n%1").arg(targetDir));
        return;
    }

    const bool readerWasOpen = (currentReaderPath_ == sourcePath);
    if (readerWasOpen && pdfDocument_)
        pdfDocument_->close();

    bool moved = QFile::rename(sourcePath, targetPath);
    if (!moved) {
        // Cross-filesystem moves commonly cannot be represented as a single rename.
        if (QFile::copy(sourcePath, targetPath)) {
            if (QFile::remove(sourcePath)) {
                moved = true;
            } else {
                QFile::remove(targetPath);
            }
        }
    }
    if (!moved) {
        if (readerWasOpen && pdfDocument_)
            pdfDocument_->load(sourcePath);
        QMessageBox::warning(this, QStringLiteral("HEPShelf"),
                             QStringLiteral("Could not move the PDF. The original file was left untouched."));
        return;
    }

    const QFileInfo targetInfo(targetPath);
    if (!db_.updateFilePath(sourcePath, targetPath, targetInfo.size(), targetInfo.lastModified().toSecsSinceEpoch(), &error)) {
        QMessageBox::warning(this, QStringLiteral("HEPShelf"),
                             QStringLiteral("The file was moved, but the HEPShelf index could not be updated. Run Rescan Library.\n\n%1").arg(error));
    }

    organizerSettings.setValue(settingKey, pattern->currentText());
    if (!renameOnly) {
        organizerSettings.setValue(QStringLiteral("organize/root"), destinationRoot);
        if (watchDestination && watchDestination->isChecked())
            db_.addFolder(destinationRoot, nullptr);
    }

    if (readerWasOpen) {
        currentReaderPath_ = targetPath;
        pendingReaderPage_ = details.lastPage;
        readerTitle_->setToolTip(targetPath);
        if (pdfDocument_)
            pdfDocument_->load(targetPath);
    }

    refreshTable();
    rebuildNavigation();
    updateCounts();
    showPaperDetails(id);
    statusBar()->showMessage((renameOnly ? QStringLiteral("Renamed PDF to %1")
                                          : QStringLiteral("Moved PDF to %1")).arg(targetPath), 7000);
}

void MainWindow::movePapersToFolder(bool allLibrary)
{
    QStringList ids;
    if (allLibrary) {
        QString searchError;
        const QList<LibraryRow> papers = db_.search({}, LibraryFilter::All, {}, &searchError);
        if (!searchError.isEmpty()) {
            showDatabaseError(QStringLiteral("Could not read the library"), searchError);
            return;
        }
        for (const LibraryRow &paper : papers)
            ids << paper.arxivId;
    } else {
        for (const QModelIndex &index : table_->selectionModel()->selectedRows()) {
            if (const QTableWidgetItem *item = table_->item(index.row(), 0))
                ids << item->data(RoleArxivId).toString();
        }
    }
    ids.removeAll(QString());
    ids.removeDuplicates();
    if (ids.isEmpty()) {
        QMessageBox::information(this, QStringLiteral("Move papers"),
                                 allLibrary ? QStringLiteral("There are no papers in the library.")
                                            : QStringLiteral("Select one or more papers first."));
        return;
    }

    PaperDetails sample;
    QString error;
    if (!db_.paperDetails(ids.first(), &sample, &error)) {
        showDatabaseError(QStringLiteral("Could not read paper metadata"), error);
        return;
    }

    QSettings settings;
    QDialog dialog(this);
    dialog.setWindowTitle(QStringLiteral("Organize / move papers"));
    dialog.resize(760, 350);
    auto *outer = new QVBoxLayout(&dialog);
    auto *intro = new QLabel(allLibrary
                                ? QStringLiteral("Move every local PDF for all %1 library papers using one template.").arg(ids.size())
                                : QStringLiteral("Move every local PDF for %1 selected papers using one template.").arg(ids.size()),
                            &dialog);
    intro->setWordWrap(true);
    outer->addWidget(intro);

    auto *form = new QFormLayout();
    auto *destRowWidget = new QWidget(&dialog);
    auto *destRow = new QHBoxLayout(destRowWidget);
    destRow->setContentsMargins(0, 0, 0, 0);
    auto *destination = new QLineEdit(settings.value(QStringLiteral("organize/root"),
                                                      QDir::homePath()).toString(), destRowWidget);
    auto *browse = new QPushButton(QStringLiteral("Browse…"), destRowWidget);
    destRow->addWidget(destination, 1);
    destRow->addWidget(browse);
    form->addRow(QStringLiteral("Library root:"), destRowWidget);

    auto *pattern = new QComboBox(&dialog);
    pattern->setEditable(true);
    pattern->addItems({QStringLiteral("{arxiv}{version}.pdf"),
                       QStringLiteral("{first_author}_{year}_{arxiv}{version}.pdf"),
                       QStringLiteral("{year}/{first_author}/{arxiv}{version}.pdf"),
                       QStringLiteral("{category}/{year}/{first_author}_{arxiv}{version}.pdf"),
                       QStringLiteral("{year}/{first_author}/{title}.pdf")});
    pattern->setCurrentText(settings.value(QStringLiteral("organize/moveTemplate"),
                                            QStringLiteral("{year}/{first_author}/{arxiv}{version}.pdf")).toString());
    form->addRow(QStringLiteral("Template:"), pattern);

    auto *hint = new QLabel(QStringLiteral("Available fields: {arxiv}, {version}, {year}, {first_author}, {category}, {title}. Existing targets are skipped."), &dialog);
    hint->setWordWrap(true);
    form->addRow(QString(), hint);
    auto *preview = new QLabel(&dialog);
    preview->setWordWrap(true);
    preview->setTextInteractionFlags(Qt::TextSelectableByMouse);
    form->addRow(QStringLiteral("Example target:"), preview);
    outer->addLayout(form);

    const QString samplePath = sample.files.isEmpty() ? QString() : sample.files.first().path;
    auto updatePreview = [this, destination, pattern, preview, &sample, samplePath]() {
        if (samplePath.isEmpty() || destination->text().trimmed().isEmpty()) {
            preview->setText(QStringLiteral("Choose a destination folder."));
            return;
        }
        const QString relative = expandOrganizationTemplate(pattern->currentText(), sample, samplePath);
        preview->setText(safeOrganizationRelative(relative)
                             ? QDir::cleanPath(QDir(destination->text().trimmed()).filePath(relative))
                             : QStringLiteral("The template produces an unsafe path."));
    };
    connect(browse, &QPushButton::clicked, &dialog, [&dialog, destination, updatePreview]() {
        const QString folder = QFileDialog::getExistingDirectory(&dialog, QStringLiteral("Choose destination folder"), destination->text());
        if (!folder.isEmpty()) {
            destination->setText(folder);
            updatePreview();
        }
    });
    connect(destination, &QLineEdit::textChanged, &dialog, [updatePreview]() { updatePreview(); });
    connect(pattern->lineEdit(), &QLineEdit::textChanged, &dialog, [updatePreview]() { updatePreview(); });
    updatePreview();

    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Cancel | QDialogButtonBox::Ok, &dialog);
    buttons->button(QDialogButtonBox::Ok)->setText(QStringLiteral("Move PDFs"));
    outer->addWidget(buttons);
    connect(buttons, &QDialogButtonBox::accepted, &dialog, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
    if (dialog.exec() != QDialog::Accepted)
        return;
    if (destination->text().trimmed().isEmpty()) {
        QMessageBox::warning(this, QStringLiteral("HEPShelf"), QStringLiteral("Choose a destination folder first."));
        return;
    }
    if (!samplePath.isEmpty()
        && !safeOrganizationRelative(expandOrganizationTemplate(pattern->currentText(), sample, samplePath))) {
        QMessageBox::warning(this, QStringLiteral("HEPShelf"),
                             QStringLiteral("The organization template produced an unsafe path."));
        return;
    }
    const QString destinationRoot = QDir(destination->text().trimmed()).absolutePath();
    if (!QDir().mkpath(destinationRoot)) {
        QMessageBox::warning(this, QStringLiteral("HEPShelf"),
                             QStringLiteral("Could not create destination folder:\n%1").arg(destinationRoot));
        return;
    }
    const QString moveTemplate = pattern->currentText();

    int moved = 0;
    int skipped = 0;
    QStringList problems;
    for (const QString &id : ids) {
        PaperDetails details;
        if (!db_.paperDetails(id, &details, &error)) {
            problems << QStringLiteral("%1: %2").arg(id, error);
            continue;
        }
        for (const FileRecord &file : details.files) {
            const QString source = QDir::cleanPath(file.path);
            const QString relative = expandOrganizationTemplate(moveTemplate, details, source);
            if (!safeOrganizationRelative(relative)) {
                problems << QStringLiteral("Unsafe template for: %1").arg(source);
                ++skipped;
                continue;
            }
            const QString target = QDir::cleanPath(QDir(destinationRoot).filePath(relative));
            if (source == target) {
                ++skipped;
                continue;
            }
            if (!QFileInfo::exists(source)) {
                problems << QStringLiteral("Missing: %1").arg(source);
                continue;
            }
            if (QFileInfo::exists(target)) {
                problems << QStringLiteral("Already exists: %1").arg(target);
                ++skipped;
                continue;
            }
            if (!QDir().mkpath(QFileInfo(target).absolutePath())) {
                problems << QStringLiteral("Could not create target folder: %1").arg(target);
                ++skipped;
                continue;
            }

            const bool readerWasOpen = (currentReaderPath_ == source);
            if (readerWasOpen && pdfDocument_)
                pdfDocument_->close();
            bool fileMoved = QFile::rename(source, target);
            if (!fileMoved && QFile::copy(source, target)) {
                fileMoved = QFile::remove(source);
                if (!fileMoved)
                    QFile::remove(target);
            }
            if (!fileMoved) {
                if (readerWasOpen && pdfDocument_)
                    pdfDocument_->load(source);
                problems << QStringLiteral("Could not move: %1").arg(source);
                continue;
            }

            const QFileInfo targetInfo(target);
            if (!db_.updateFilePath(source, target, targetInfo.size(),
                                    targetInfo.lastModified().toSecsSinceEpoch(), &error)) {
                if (QFile::rename(target, source)) {
                    if (readerWasOpen && pdfDocument_)
                        pdfDocument_->load(source);
                    problems << QStringLiteral("Index update failed; restored %1: %2").arg(source, error);
                } else {
                    problems << QStringLiteral("Index update failed; file is at %1: %2").arg(target, error);
                }
                continue;
            }
            if (readerWasOpen) {
                currentReaderPath_ = target;
                readerTitle_->setToolTip(target);
                if (pdfDocument_)
                    pdfDocument_->load(target);
            }
            ++moved;
        }
    }

    settings.setValue(QStringLiteral("organize/root"), destinationRoot);
    settings.setValue(QStringLiteral("organize/moveTemplate"), moveTemplate);
    if (moved > 0) {
        if (!db_.addFolder(destinationRoot, &error))
            problems << QStringLiteral("Could not watch destination folder: %1").arg(error);
        refreshTable();
        rebuildNavigation();
        updateCounts();
    }
    QString summary = QStringLiteral("Moved %1 PDF(s); skipped %2.").arg(moved).arg(skipped);
    if (!problems.isEmpty())
        summary += QStringLiteral("\n\n%1").arg(problems.mid(0, 12).join(QLatin1Char('\n')));
    if (problems.size() > 12)
        summary += QStringLiteral("\n…and %1 more issue(s).").arg(problems.size() - 12);
    QMessageBox::information(this, QStringLiteral("Move papers"), summary);
}

void MainWindow::openSelectedInReader()
{
    const QString id = selectedArxivId();
    const QString path = selectedLocalPath();
    if (id.isEmpty() || path.isEmpty())
        return;
    openPaperInReader(id, path);
}

void MainWindow::openPaperInReader(const QString &arxivId,
                                   const QString &path,
                                   bool pushHistory,
                                   int pageOverride)
{
    if (!QFileInfo::exists(path)) {
        QMessageBox::warning(this, QStringLiteral("HEPShelf"),
                             QStringLiteral("The local file no longer exists:\n%1").arg(path));
        return;
    }

    PaperDetails details;
    QString error;
    if (!db_.paperDetails(arxivId, &details, &error)) {
        showDatabaseError(QStringLiteral("Could not read paper details"), error);
        return;
    }

    if (pushHistory && !currentReaderArxivId_.isEmpty() && !currentReaderPath_.isEmpty()) {
        ReaderLocation previous;
        previous.arxivId = currentReaderArxivId_;
        previous.path = currentReaderPath_;
        previous.page = pendingReaderPage_;
        if (pdfDocument_ && pdfDocument_->status() == QPdfDocument::Status::Ready)
            previous.page = qMax(0, pdfView_->pageNavigator()->currentPage());
        readerBackHistory_.append(previous);
        if (readerBackHistory_.size() > 100)
            readerBackHistory_.removeFirst();
        readerForwardHistory_.clear();
    }

    currentReaderArxivId_ = arxivId;
    currentReaderPath_ = path;
    pendingReaderPage_ = pageOverride >= 0 ? pageOverride : details.lastPage;
    readerPreviousPage_ = -1;
    readerTitle_->setText(displayTitle(details));
    readerTitle_->setToolTip(path);
    readerReferences_ = db_.referencesForPaper(arxivId, nullptr);
    readerReferencesLoading_ = false;

    if (readerCitationList_)
        readerCitationList_->clear();
    if (readerCitationHeading_)
        readerCitationHeading_->setText(QStringLiteral("Loading citations…"));
    if (readerCitationStatus_)
        readerCitationStatus_->setText(QStringLiteral("Preparing citation navigator."));

    pdfDocument_->close();
    pageSpin_->blockSignals(true);
    pageSpin_->setRange(1, 1);
    pageSpin_->setValue(1);
    pageSpin_->blockSignals(false);
    pageCountLabel_->setText(QStringLiteral("/ 0"));

    const QPdfDocument::Error loadError = pdfDocument_->load(path);
    if (loadError != QPdfDocument::Error::None) {
        QMessageBox::warning(this, QStringLiteral("HEPShelf Reader"),
                             QStringLiteral("Could not load the PDF in the integrated reader. You can still open it in the system viewer.\n\n%1").arg(path));
        return;
    }

    db_.recordOpened(arxivId, nullptr);
    tabs_->setTabEnabled(1, true);
    tabs_->setCurrentIndex(1);
    refreshTable();
    rebuildNavigation();
    showPaperDetails(arxivId);
    updateReaderHistoryButtons();
    ensureReaderReferences();
}

void MainWindow::openSelectedExternally()
{
    const QString path = selectedLocalPath();
    if (path.isEmpty())
        return;
    if (!QFileInfo::exists(path)) {
        QMessageBox::warning(this, QStringLiteral("HEPShelf"),
                             QStringLiteral("The local file no longer exists:\n%1").arg(path));
        return;
    }
    if (!QDesktopServices::openUrl(QUrl::fromLocalFile(path))) {
        QMessageBox::warning(this, QStringLiteral("HEPShelf"),
                             QStringLiteral("Could not open:\n%1").arg(path));
    }
}

void MainWindow::openContainingFolder()
{
    const QString path = selectedLocalPath();
    if (path.isEmpty())
        return;
    const QFileInfo info(path);
    QDesktopServices::openUrl(QUrl::fromLocalFile(info.absolutePath()));
}

void MainWindow::openArxivPage()
{
    const QString id = selectedArxivId();
    if (!id.isEmpty())
        QDesktopServices::openUrl(QUrl(QStringLiteral("https://arxiv.org/abs/%1").arg(id)));
}

void MainWindow::openInspirePage()
{
    const QString id = selectedArxivId();
    if (id.isEmpty())
        return;
    QUrl url(QStringLiteral("https://inspirehep.net/literature"));
    QUrlQuery query;
    query.addQueryItem(QStringLiteral("q"), QStringLiteral("arxiv:%1").arg(id));
    url.setQuery(query);
    QDesktopServices::openUrl(url);
}

void MainWindow::refreshSelectedInspire()
{
    const QString id = selectedArxivId();
    if (id.isEmpty())
        return;

    if (refreshInspireButton_)
        refreshInspireButton_->setEnabled(false);
    if (refreshInspireAction_)
        refreshInspireAction_->setEnabled(false);
    statusBar()->showMessage(QStringLiteral("Fetching citation metrics and references from INSPIRE…"));

    inspire_.fetchPaper(id, [this, id](const InspirePaperData &data, const QString &error) {
        if (refreshInspireButton_)
            refreshInspireButton_->setEnabled(true);
        if (refreshInspireAction_)
            refreshInspireAction_->setEnabled(true);

        if (!error.isEmpty()) {
            statusBar()->showMessage(error, 7000);
            QMessageBox::warning(this, QStringLiteral("INSPIRE"), error);
            return;
        }

        QString dbError;
        if (!db_.updateCitationMetrics(id, data.metrics, &dbError)
            || !db_.replaceReferences(id, data.references, &dbError)) {
            showDatabaseError(QStringLiteral("Could not store INSPIRE data"), dbError);
            return;
        }

        refreshTable();
        showPaperDetails(id);
        if (currentReaderArxivId_ == id) {
            readerReferences_ = db_.referencesForPaper(id, nullptr);
            updateReaderCitationPanel();
        }
        statusBar()->showMessage(
            QStringLiteral("INSPIRE updated: %1 citations · %2 references")
                .arg(data.metrics.citationCount >= 0 ? QString::number(data.metrics.citationCount)
                                                     : QStringLiteral("unknown"))
                .arg(data.metrics.referenceCount >= 0 ? QString::number(data.metrics.referenceCount)
                                                      : QStringLiteral("unknown")),
            6000);
    });
}

void MainWindow::showSelectedReferences()
{
    const QString id = selectedArxivId();
    if (id.isEmpty())
        return;

    ReferenceDialog dialog(&db_, id, this);
    dialog.exec();
    if (dialog.libraryChanged() || dialog.metricsChanged()) {
        refreshTable();
        updateCounts();
        rebuildNavigation();
        showPaperDetails(id);
        if (dialog.libraryChanged())
            fetchMissingMetadata();
    }
    if (currentReaderArxivId_ == id) {
        readerReferences_ = db_.referencesForPaper(id, nullptr);
        updateReaderCitationPanel();
    }
}

void MainWindow::showSelectedCitedBy()
{
    const QString id = selectedArxivId();
    if (id.isEmpty())
        return;

    CitedByDialog dialog(&db_, id, this);
    dialog.exec();
    if (dialog.libraryChanged() || dialog.metricsChanged()) {
        refreshTable();
        updateCounts();
        rebuildNavigation();
        showPaperDetails(id);
        if (dialog.libraryChanged())
            fetchMissingMetadata();
    }
}

void MainWindow::watchSelectedCitations()
{
    const QString id = selectedArxivId();
    if (id.isEmpty() || !watches_)
        return;
    ArxivWatchRule rule;
    rule.kind = QStringLiteral("citation");
    rule.term = id;
    PaperDetails details;
    if (db_.paperDetails(id, &details)) {
        rule.paperTitle = details.paper.title;
        rule.inspireRecid = details.citations.inspireRecid;
    }
    QString error;
    if (!watches_->addRule(rule, &error) && error != QStringLiteral("This watch already exists.")) {
        QMessageBox::warning(this, QStringLiteral("Citation watch"), error);
        return;
    }
    showArxivWatches();
}

void MainWindow::showSelectedCitationGraph()
{
    const QString id = selectedArxivId();
    if (id.isEmpty())
        return;

    CitationGraphDialog dialog(
        &db_, id,
        [this](const QString &arxivId, const QString &path) {
            if (!arxivId.isEmpty() && !path.isEmpty())
                openPaperInReader(arxivId, path, true);
        },
        this);
    dialog.exec();
    if (dialog.dataChanged()) {
        rebuildNavigation();
        refreshTable();
        showPaperDetails(id);
        if (currentReaderArxivId_ == id) {
            readerReferences_ = db_.referencesForPaper(id, nullptr);
            updateReaderCitationPanel();
        }
    }
}

void MainWindow::copySelectedInspireBibTeX()
{
    const QString id = selectedArxivId();
    if (id.isEmpty())
        return;
    statusBar()->showMessage(QStringLiteral("Fetching BibTeX from INSPIRE…"));
    inspire_.fetchBibTeX(id, [this](const QString &bibtex, const QString &error) {
        if (!error.isEmpty()) {
            statusBar()->showMessage(error, 7000);
            QMessageBox::warning(this, QStringLiteral("INSPIRE BibTeX"), error);
            return;
        }
        QApplication::clipboard()->setText(bibtex);
        statusBar()->showMessage(QStringLiteral("INSPIRE BibTeX copied to the clipboard."), 5000);
    });
}

void MainWindow::exportSelectedInspireBibTeX()
{
    const QString id = selectedArxivId();
    if (id.isEmpty())
        return;

    QString safeId = id;
    safeId.replace(QLatin1Char('/'), QLatin1Char('_'));

    const QString path = QFileDialog::getSaveFileName(
        this, QStringLiteral("Export INSPIRE BibTeX"),
        QStringLiteral("%1.bib").arg(safeId),
        QStringLiteral("BibTeX files (*.bib)"));
    if (path.isEmpty())
        return;

    statusBar()->showMessage(QStringLiteral("Fetching BibTeX from INSPIRE…"));
    inspire_.fetchBibTeX(id, [this, path](const QString &bibtex, const QString &error) {
        if (!error.isEmpty()) {
            statusBar()->showMessage(error, 7000);
            QMessageBox::warning(this, QStringLiteral("INSPIRE BibTeX"), error);
            return;
        }
        QFile file(path);
        if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate | QIODevice::Text)) {
            QMessageBox::warning(this, QStringLiteral("HEPShelf"),
                                 QStringLiteral("Could not write %1").arg(path));
            return;
        }
        file.write(bibtex.toUtf8());
        if (!bibtex.endsWith(QLatin1Char('\n')))
            file.write("\n");
        file.close();
        statusBar()->showMessage(QStringLiteral("Exported INSPIRE BibTeX to %1").arg(path), 6000);
    });
}

void MainWindow::showLibraryContextMenu(const QPoint &pos)
{
    QTableWidgetItem *clicked = table_->itemAt(pos);
    if (!clicked)
        return;
    if (!clicked->isSelected())
        table_->selectRow(clicked->row());

    QMenu menu(this);
    menu.addAction(QStringLiteral("Open in HEPShelf Reader"), this, [this]() { openSelectedInReader(); });
    menu.addAction(QStringLiteral("Open in system PDF viewer"), this, [this]() { openSelectedExternally(); });
    menu.addAction(QStringLiteral("Show containing folder"), this, [this]() { openContainingFolder(); });
    menu.addSeparator();
    menu.addAction(QStringLiteral("Organize / move selected PDF…"), this, [this]() { organizeSelectedFile(false); });
    menu.addAction(QStringLiteral("Rename selected PDF using metadata…"), this, [this]() { organizeSelectedFile(true); });
    menu.addAction(QStringLiteral("Move selected papers to folder…"), this, [this]() { movePapersToFolder(false); });
    menu.addAction(QStringLiteral("Move all library papers to folder…"), this, [this]() { movePapersToFolder(true); });
    menu.addSeparator();
    menu.addAction(QStringLiteral("Add tag…"), this, [this]() { addTagToSelected(); });
    menu.addAction(QStringLiteral("Add to collection…"), this, [this]() { addSelectedToCollection(); });
    menu.addAction(QStringLiteral("Add to literature trail…"), this, [this]() { addSelectedToLiteratureTrail(); });
    menu.addSeparator();
    menu.addAction(QStringLiteral("Open arXiv page"), this, [this]() { openArxivPage(); });
    menu.addAction(QStringLiteral("Open INSPIRE search"), this, [this]() { openInspirePage(); });
    menu.addAction(QStringLiteral("Browse references…"), this, [this]() { showSelectedReferences(); });
    menu.addAction(QStringLiteral("Browse cited-by papers…"), this, [this]() { showSelectedCitedBy(); });
    menu.addAction(QStringLiteral("Watch new citations to this paper…"), this, [this]() { watchSelectedCitations(); });
    menu.addAction(QStringLiteral("Open citation network…"), this, [this]() { showSelectedCitationGraph(); });
    menu.addSeparator();

    const QTableWidgetItem *current = table_->item(clicked->row(), 0);
    const bool favorite = current && current->data(RoleFavorite).toBool();
    menu.addAction(favorite ? QStringLiteral("Remove from favorites") : QStringLiteral("Add to favorites"),
                   this, [this]() { toggleSelectedFavorite(); });
    menu.addAction(QStringLiteral("Refresh metadata"), this, [this]() { refreshSelectedMetadata(); });
    menu.addAction(QStringLiteral("Refresh INSPIRE citation data"), this, [this]() { refreshSelectedInspire(); });
    menu.addSeparator();
    menu.addAction(QStringLiteral("Copy INSPIRE BibTeX"), this, [this]() { copySelectedInspireBibTeX(); });
    menu.addAction(QStringLiteral("Export INSPIRE BibTeX…"), this, [this]() { exportSelectedInspireBibTeX(); });
    menu.addSeparator();
    menu.addAction(QStringLiteral("Copy arXiv ID"), this, [this]() { copySelectedArxivId(); });
    menu.addAction(QStringLiteral("Copy title"), this, [this]() { copySelectedTitle(); });

    menu.exec(table_->viewport()->mapToGlobal(pos));
}

void MainWindow::exportBibTeX()
{
    if (!databaseReady_)
        return;

    QString error;
    const QList<LibraryRow> rows = db_.search(search_->text(), currentFilter_, currentFacetValue_, &error);
    if (!error.isEmpty()) {
        showDatabaseError(QStringLiteral("Could not read papers for export"), error);
        return;
    }
    if (rows.isEmpty()) {
        statusBar()->showMessage(QStringLiteral("There are no papers in the current view to export."), 4000);
        return;
    }

    const QString path = QFileDialog::getSaveFileName(this,
                                                       QStringLiteral("Export BibTeX"),
                                                       QStringLiteral("hepshelf-library.bib"),
                                                       QStringLiteral("BibTeX files (*.bib)"));
    if (path.isEmpty())
        return;

    QString output;
    for (const LibraryRow &row : rows) {
        PaperDetails details;
        if (!db_.paperDetails(row.arxivId, &details, &error))
            continue;

        output += QStringLiteral("@article{%1,\n").arg(bibKey(row.arxivId));
        output += QStringLiteral("  title = {{%1}},\n").arg(displayTitle(details));
        if (!details.paper.authors.isEmpty())
            output += QStringLiteral("  author = {%1},\n").arg(bibAuthors(details));
        output += QStringLiteral("  eprint = {%1},\n").arg(details.paper.arxivId);
        output += QStringLiteral("  archivePrefix = {arXiv},\n");
        if (!details.paper.primaryCategory.isEmpty())
            output += QStringLiteral("  primaryClass = {%1},\n").arg(details.paper.primaryCategory);
        const QString year = yearFromDate(details.paper.published);
        if (!year.isEmpty())
            output += QStringLiteral("  year = {%1},\n").arg(year);
        if (!details.paper.doi.isEmpty())
            output += QStringLiteral("  doi = {%1},\n").arg(details.paper.doi);
        if (!details.paper.journalRef.isEmpty())
            output += QStringLiteral("  note = {Journal reference: %1},\n").arg(details.paper.journalRef);
        output += QStringLiteral("}\n\n");
    }

    QFile file(path);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        QMessageBox::warning(this, QStringLiteral("HEPShelf"),
                             QStringLiteral("Could not write %1").arg(path));
        return;
    }
    file.write(output.toUtf8());
    file.close();
    statusBar()->showMessage(QStringLiteral("Exported %1 paper(s) to %2").arg(rows.size()).arg(path), 6000);
}

void MainWindow::copySelectedArxivId()
{
    const QString id = selectedArxivId();
    if (!id.isEmpty())
        QApplication::clipboard()->setText(id);
}

void MainWindow::copySelectedTitle()
{
    const QString id = selectedArxivId();
    if (id.isEmpty())
        return;
    PaperDetails details;
    if (db_.paperDetails(id, &details, nullptr))
        QApplication::clipboard()->setText(displayTitle(details));
}

void MainWindow::updateReaderForDocumentStatus()
{
    if (pdfDocument_->status() == QPdfDocument::Status::Ready) {
        const int count = pdfDocument_->pageCount();
        pageSpin_->blockSignals(true);
        pageSpin_->setRange(1, qMax(1, count));
        pageSpin_->blockSignals(false);
        pageCountLabel_->setText(QStringLiteral("/ %1").arg(count));
        readerPrevButton_->setEnabled(count > 1);
        readerNextButton_->setEnabled(count > 1);

        if (count > 0) {
            const int page = qBound(0, pendingReaderPage_, count - 1);
            pendingReaderPage_ = page;
            pdfView_->pageNavigator()->jump(page, QPointF(0, 0), 0);
            db_.setReadingProgress(currentReaderArxivId_, page, count, nullptr);
            QTimer::singleShot(0, this, [this]() { updateReaderCitationPanel(); });
        }
        return;
    }

    if (pdfDocument_->status() == QPdfDocument::Status::Error) {
        statusBar()->showMessage(QStringLiteral("The integrated reader could not load this PDF."), 8000);
    }
}

void MainWindow::updateReaderPage(int zeroBasedPage)
{
    if (zeroBasedPage < 0 || pdfDocument_->pageCount() <= 0)
        return;

    if (pendingReaderPage_ != zeroBasedPage)
        readerPreviousPage_ = pendingReaderPage_;
    pendingReaderPage_ = zeroBasedPage;
    pageSpin_->blockSignals(true);
    pageSpin_->setValue(zeroBasedPage + 1);
    pageSpin_->blockSignals(false);
    readerPrevButton_->setEnabled(zeroBasedPage > 0);
    readerNextButton_->setEnabled(zeroBasedPage + 1 < pdfDocument_->pageCount());

    if (!currentReaderArxivId_.isEmpty()) {
        db_.setReadingProgress(currentReaderArxivId_, zeroBasedPage, pdfDocument_->pageCount(), nullptr);
        if (selectedArxivId() == currentReaderArxivId_)
            showPaperDetails(currentReaderArxivId_);
    }

    updateReaderCitationPanel();
}

void MainWindow::readerPreviousPage()
{
    if (pdfDocument_->status() != QPdfDocument::Status::Ready)
        return;
    const int page = qMax(0, pdfView_->pageNavigator()->currentPage() - 1);
    pdfView_->pageNavigator()->jump(page, QPointF(0, 0), 0);
}

void MainWindow::readerNextPage()
{
    if (pdfDocument_->status() != QPdfDocument::Status::Ready)
        return;
    const int page = qMin(pdfDocument_->pageCount() - 1, pdfView_->pageNavigator()->currentPage() + 1);
    pdfView_->pageNavigator()->jump(page, QPointF(0, 0), 0);
}

void MainWindow::readerZoomIn()
{
    pdfView_->setZoomMode(QPdfView::ZoomMode::Custom);
    pdfView_->setZoomFactor(qMin<qreal>(5.0, pdfView_->zoomFactor() * 1.15));
}

void MainWindow::readerZoomOut()
{
    pdfView_->setZoomMode(QPdfView::ZoomMode::Custom);
    pdfView_->setZoomFactor(qMax<qreal>(0.2, pdfView_->zoomFactor() / 1.15));
}

void MainWindow::readerFitWidth()
{
    pdfView_->setZoomMode(QPdfView::ZoomMode::FitToWidth);
}

void MainWindow::readerFitPage()
{
    pdfView_->setZoomMode(QPdfView::ZoomMode::FitInView);
}

void MainWindow::readerGoBack()
{
    if (readerBackHistory_.isEmpty())
        return;

    if (!currentReaderArxivId_.isEmpty() && !currentReaderPath_.isEmpty()) {
        ReaderLocation current;
        current.arxivId = currentReaderArxivId_;
        current.path = currentReaderPath_;
        current.page = pendingReaderPage_;
        if (pdfDocument_ && pdfDocument_->status() == QPdfDocument::Status::Ready)
            current.page = qMax(0, pdfView_->pageNavigator()->currentPage());
        readerForwardHistory_.append(current);
    }

    const ReaderLocation target = readerBackHistory_.takeLast();
    openPaperInReader(target.arxivId, target.path, false, target.page);
    updateReaderHistoryButtons();
}

void MainWindow::readerGoForward()
{
    if (readerForwardHistory_.isEmpty())
        return;

    if (!currentReaderArxivId_.isEmpty() && !currentReaderPath_.isEmpty()) {
        ReaderLocation current;
        current.arxivId = currentReaderArxivId_;
        current.path = currentReaderPath_;
        current.page = pendingReaderPage_;
        if (pdfDocument_ && pdfDocument_->status() == QPdfDocument::Status::Ready)
            current.page = qMax(0, pdfView_->pageNavigator()->currentPage());
        readerBackHistory_.append(current);
    }

    const ReaderLocation target = readerForwardHistory_.takeLast();
    openPaperInReader(target.arxivId, target.path, false, target.page);
    updateReaderHistoryButtons();
}

void MainWindow::updateReaderHistoryButtons()
{
    if (readerHistoryBackButton_)
        readerHistoryBackButton_->setEnabled(!readerBackHistory_.isEmpty());
    if (readerHistoryForwardButton_)
        readerHistoryForwardButton_->setEnabled(!readerForwardHistory_.isEmpty());
}

ReferenceRecord MainWindow::readerReferenceForPosition(int position) const
{
    for (const ReferenceRecord &ref : readerReferences_) {
        if (ref.position == position)
            return ref;
    }
    ReferenceRecord missing;
    missing.position = position;
    return missing;
}

void MainWindow::handlePdfJumped(const QPdfLink &link)
{
    if (suppressPdfCitationJump_ || currentReaderArxivId_.isEmpty()
        || readerReferences_.isEmpty() || !link.url().isEmpty())
        return;

    const int sourcePage = readerPreviousPage_;
    if (sourcePage < 0 || sourcePage >= pdfDocument_->pageCount() || link.rectangles().isEmpty())
        return;

    // LaTeX/hyperref citation links normally carry the rectangle of the clicked
    // reference number.  Use that source rectangle to recover the number from
    // the page text.  If a PDF does not expose link rectangles, the side-panel
    // citation navigator remains the fallback.
    int referenceNumber = 0;
    for (const QRectF &rect : link.rectangles()) {
        const QPdfSelection exact = pdfDocument_->getSelection(sourcePage, rect.topLeft(), rect.bottomRight());
        const QString exactText = exact.text().trimmed();

        QRectF expanded = rect.adjusted(-8.0, -2.0, 8.0, 2.0);
        const QPdfSelection around = pdfDocument_->getSelection(sourcePage, expanded.topLeft(), expanded.bottomRight());
        const QString aroundText = around.text();
        if (!aroundText.contains(QLatin1Char('[')) || !aroundText.contains(QLatin1Char(']')))
            continue;

        const auto numberMatch = QRegularExpression(QStringLiteral("(\\d+)")).match(exactText);
        if (numberMatch.hasMatch()) {
            referenceNumber = numberMatch.captured(1).toInt();
            break;
        }
    }

    if (referenceNumber <= 0)
        return;

    const ReferenceRecord ref = readerReferenceForPosition(referenceNumber);
    if (!ref.local || ref.localPath.isEmpty())
        return; // Preserve the PDF's normal jump-to-bibliography behavior.

    QString targetId = ref.arxivId;
    if (targetId.isEmpty())
        targetId = db_.arxivIdForPath(ref.localPath);
    if (targetId.isEmpty())
        return;

    ReaderLocation source;
    source.arxivId = currentReaderArxivId_;
    source.path = currentReaderPath_;
    source.page = sourcePage;
    const QString targetPath = ref.localPath;

    suppressPdfCitationJump_ = true;
    QTimer::singleShot(0, this, [this, source, targetId, targetPath, referenceNumber]() {
        readerBackHistory_.append(source);
        if (readerBackHistory_.size() > 100)
            readerBackHistory_.removeFirst();
        readerForwardHistory_.clear();
        statusBar()->showMessage(
            QStringLiteral("Following local citation [%1]").arg(referenceNumber), 3500);
        openPaperInReader(targetId, targetPath, false);
        suppressPdfCitationJump_ = false;
        updateReaderHistoryButtons();
    });
}

void MainWindow::updateReaderCitationPanel()
{
    if (!readerCitationList_ || !readerCitationHeading_ || !readerCitationStatus_)
        return;

    readerCitationList_->clear();

    if (!pdfDocument_ || pdfDocument_->status() != QPdfDocument::Status::Ready
        || currentReaderArxivId_.isEmpty()) {
        readerCitationHeading_->setText(QStringLiteral("Open a paper to inspect citations."));
        readerCitationStatus_->setText(QStringLiteral("Citation detection becomes available when the PDF is loaded."));
        updateReaderCitationPreview();
        return;
    }

    const int page = qMax(0, pdfView_->pageNavigator()->currentPage());
    readerCitationHeading_->setText(QStringLiteral("Citations on page %1").arg(page + 1));

    const QPdfSelection selection = pdfDocument_->getAllText(page);
    QString pageText = selection.text();
    if (pageText.trimmed().isEmpty()) {
        readerCitationStatus_->setText(QStringLiteral("No extractable text is available on this page. Scanned/image-only PDFs need OCR before callouts can be detected."));
        updateReaderCitationPreview();
        return;
    }

    int maxReference = 0;
    for (const ReferenceRecord &ref : readerReferences_)
        maxReference = qMax(maxReference, ref.position);

    QMap<int, QString> contexts;
    QMap<int, int> occurrences;
    const QRegularExpression citationRx(
        QStringLiteral(R"(\[\s*([0-9][0-9\s,;\-\x{2013}\x{2014}]*)\s*\])"),
        QRegularExpression::UseUnicodePropertiesOption);
    const QRegularExpression rangeRx(
        QStringLiteral(R"(^\s*(\d+)\s*[\-\x{2013}\x{2014}]\s*(\d+)\s*$)"),
        QRegularExpression::UseUnicodePropertiesOption);
    const QRegularExpression whitespaceRx(QStringLiteral("\\s+"));

    auto matches = citationRx.globalMatch(pageText);
    while (matches.hasNext()) {
        const auto match = matches.next();
        const QString payload = match.captured(1);
        QStringList parts = payload.split(QRegularExpression(QStringLiteral("\\s*[,;]\\s*")), Qt::SkipEmptyParts);
        QList<int> numbers;

        for (const QString &part : parts) {
            const auto rangeMatch = rangeRx.match(part);
            if (rangeMatch.hasMatch()) {
                const int first = rangeMatch.captured(1).toInt();
                const int last = rangeMatch.captured(2).toInt();
                if (first > 0 && last >= first && last - first <= 50) {
                    for (int n = first; n <= last; ++n)
                        numbers << n;
                }
                continue;
            }

            bool ok = false;
            const int number = part.trimmed().toInt(&ok);
            if (ok && number > 0)
                numbers << number;
        }

        const int contextStart = qMax(0, match.capturedStart() - 80);
        const int contextEnd = qMin(pageText.size(), match.capturedEnd() + 100);
        QString context = pageText.mid(contextStart, contextEnd - contextStart);
        context.replace(whitespaceRx, QStringLiteral(" "));
        context = context.trimmed();

        for (const int number : numbers) {
            // Once structured references are known, numbers beyond the bibliography are
            // almost certainly equation/list labels rather than citations.
            if (maxReference > 0 && number > maxReference)
                continue;
            if (!contexts.contains(number))
                contexts.insert(number, context);
            occurrences[number] = occurrences.value(number) + 1;
        }
    }

    int localCount = 0;
    for (auto it = contexts.cbegin(); it != contexts.cend(); ++it) {
        const int number = it.key();
        const ReferenceRecord ref = readerReferenceForPosition(number);
        QString status;
        QString title;
        if (ref.position > 0 && (!ref.title.isEmpty() || !ref.rawText.isEmpty() || !ref.arxivId.isEmpty()
                                 || ref.inspireRecid > 0 || !ref.doi.isEmpty())) {
            if (ref.local) {
                status = QStringLiteral("✓ Local");
                ++localCount;
            } else if (!ref.arxivId.isEmpty()) {
                status = QStringLiteral("○ arXiv");
            } else {
                status = QStringLiteral("○ Metadata");
            }
            title = !ref.title.isEmpty() ? ref.title : (!ref.rawText.isEmpty() ? ref.rawText : ref.arxivId);
        } else {
            status = QStringLiteral("? Unmatched");
            title = QStringLiteral("Reference metadata not loaded");
        }

        if (title.size() > 90)
            title = title.left(87) + QStringLiteral("…");
        const int count = occurrences.value(number);
        const QString suffix = count > 1 ? QStringLiteral("  · %1× on page").arg(count) : QString();
        auto *item = new QListWidgetItem(
            QStringLiteral("[%1]  %2 — %3%4").arg(number).arg(status).arg(title).arg(suffix),
            readerCitationList_);
        item->setData(Qt::UserRole, number);
        item->setData(Qt::UserRole + 1, it.value());
        item->setToolTip(it.value());
    }

    if (contexts.isEmpty()) {
        if (readerReferencesLoading_)
            readerCitationStatus_->setText(QStringLiteral("Loading structured references from INSPIRE…"));
        else
            readerCitationStatus_->setText(QStringLiteral("No numeric [n] citation callouts were detected on this page."));
    } else if (readerReferences_.isEmpty()) {
        readerCitationStatus_->setText(
            QStringLiteral("Detected %1 citation number(s), but structured reference metadata is not available yet.")
                .arg(contexts.size()));
    } else {
        readerCitationStatus_->setText(
            QStringLiteral("%1 referenced paper(s) detected on this page · %2 already local")
                .arg(contexts.size())
                .arg(localCount));
    }

    if (readerCitationList_->count() > 0)
        readerCitationList_->setCurrentRow(0);
    else
        updateReaderCitationPreview();
}

void MainWindow::updateReaderCitationPreview()
{
    if (!readerCitationPreview_ || !readerCitationOpenButton_ || !readerCitationArxivButton_)
        return;

    auto *item = readerCitationList_ ? readerCitationList_->currentItem() : nullptr;
    if (!item) {
        readerCitationPreview_->setText(QStringLiteral("Select a citation to see the referenced paper and local availability."));
        readerCitationOpenButton_->setEnabled(false);
        readerCitationArxivButton_->setEnabled(false);
        return;
    }

    const int position = item->data(Qt::UserRole).toInt();
    const QString context = item->data(Qt::UserRole + 1).toString();
    const ReferenceRecord ref = readerReferenceForPosition(position);

    QStringList lines;
    lines << QStringLiteral("Reference [%1]").arg(position);
    if (!ref.title.isEmpty())
        lines << ref.title;
    else if (!ref.rawText.isEmpty())
        lines << ref.rawText;
    if (!ref.authors.isEmpty())
        lines << ref.authors;
    if (!ref.arxivId.isEmpty())
        lines << QStringLiteral("arXiv:%1").arg(ref.arxivId);
    if (!ref.doi.isEmpty())
        lines << QStringLiteral("DOI: %1").arg(ref.doi);
    if (ref.local)
        lines << QStringLiteral("✓ Local PDF: %1").arg(ref.localPath);
    else if (!ref.arxivId.isEmpty())
        lines << QStringLiteral("○ Not in the local library");
    if (!context.isEmpty())
        lines << QStringLiteral("\nOn this page: …%1…").arg(context);

    readerCitationPreview_->setText(lines.join(QStringLiteral("\n")));
    readerCitationOpenButton_->setEnabled(ref.local || !ref.arxivId.isEmpty());
    readerCitationOpenButton_->setText(ref.local ? QStringLiteral("Open local paper")
                                                  : QStringLiteral("Open arXiv"));
    readerCitationArxivButton_->setEnabled(!ref.arxivId.isEmpty());
}

void MainWindow::ensureReaderReferences()
{
    if (currentReaderArxivId_.isEmpty())
        return;

    readerReferences_ = db_.referencesForPaper(currentReaderArxivId_, nullptr);
    if (!readerReferences_.isEmpty()) {
        updateReaderCitationPanel();
        return;
    }

    PaperDetails details;
    if (db_.paperDetails(currentReaderArxivId_, &details, nullptr)
        && details.citations.fetchedAt.isEmpty()) {
        refreshReaderReferencesFromInspire();
        return;
    }

    updateReaderCitationPanel();
}

void MainWindow::refreshReaderReferencesFromInspire()
{
    if (currentReaderArxivId_.isEmpty() || readerReferencesLoading_)
        return;

    const QString sourceId = currentReaderArxivId_;
    readerReferencesLoading_ = true;
    if (readerCitationRefreshButton_)
        readerCitationRefreshButton_->setEnabled(false);
    if (readerCitationStatus_)
        readerCitationStatus_->setText(QStringLiteral("Fetching structured references from INSPIRE…"));

    inspire_.fetchPaper(sourceId, [this, sourceId](const InspirePaperData &data, const QString &error) {
        readerReferencesLoading_ = false;
        if (readerCitationRefreshButton_)
            readerCitationRefreshButton_->setEnabled(true);

        if (!error.isEmpty()) {
            if (sourceId == currentReaderArxivId_) {
                readerCitationStatus_->setText(error);
                updateReaderCitationPanel();
            }
            statusBar()->showMessage(error, 7000);
            return;
        }

        QString dbError;
        if (!db_.updateCitationMetrics(sourceId, data.metrics, &dbError)
            || !db_.replaceReferences(sourceId, data.references, &dbError)) {
            showDatabaseError(QStringLiteral("Could not store INSPIRE reference data"), dbError);
            return;
        }

        if (sourceId == currentReaderArxivId_) {
            readerReferences_ = db_.referencesForPaper(sourceId, nullptr);
            updateReaderCitationPanel();
            showPaperDetails(sourceId);
        }
        refreshTable();
        statusBar()->showMessage(
            QStringLiteral("Loaded %1 structured references from INSPIRE").arg(data.references.size()), 5000);
    });
}

void MainWindow::openSelectedReaderCitation()
{
    auto *item = readerCitationList_ ? readerCitationList_->currentItem() : nullptr;
    if (!item)
        return;

    const ReferenceRecord ref = readerReferenceForPosition(item->data(Qt::UserRole).toInt());
    if (ref.local && !ref.localPath.isEmpty()) {
        QString targetId = ref.arxivId;
        if (targetId.isEmpty())
            targetId = db_.arxivIdForPath(ref.localPath);
        if (!targetId.isEmpty()) {
            openPaperInReader(targetId, ref.localPath, true);
            return;
        }
    }

    if (!ref.arxivId.isEmpty()) {
        QDesktopServices::openUrl(QUrl(QStringLiteral("https://arxiv.org/abs/") + ref.arxivId));
        return;
    }

    QMessageBox::information(this, QStringLiteral("Citation"),
                             QStringLiteral("This reference could not be mapped to a local PDF or an arXiv identifier yet.\n\nUse “Browse all references…” to inspect its DOI/INSPIRE metadata."));
}

void MainWindow::openReaderReferenceBrowser()
{
    if (currentReaderArxivId_.isEmpty())
        return;

    const QString sourceId = currentReaderArxivId_;
    ReferenceDialog dialog(&db_, sourceId, this);
    dialog.exec();
    if (dialog.libraryChanged() || dialog.metricsChanged()) {
        refreshTable();
        updateCounts();
        showPaperDetails(sourceId);
    }
    if (sourceId == currentReaderArxivId_) {
        readerReferences_ = db_.referencesForPaper(sourceId, nullptr);
        updateReaderCitationPanel();
    }
}

void MainWindow::setBusyUi(bool busy, const QString &message)
{
    if (addFolderAction_)
        addFolderAction_->setEnabled(!busy);
    if (manageFoldersAction_)
        manageFoldersAction_->setEnabled(!busy);
    if (scanAction_)
        scanAction_->setEnabled(!busy);
    if (metadataAction_)
        metadataAction_->setEnabled(!busy);
    if (watchesAction_)
        watchesAction_->setEnabled(!busy);
    if (organizeAction_)
        organizeAction_->setEnabled(!busy);
    if (renameAction_)
        renameAction_->setEnabled(!busy);
    if (moveSelectedPapersAction_)
        moveSelectedPapersAction_->setEnabled(!busy);
    if (moveAllPapersAction_)
        moveAllPapersAction_->setEnabled(!busy);
    if (!message.isEmpty())
        statusBar()->showMessage(message);
}

void MainWindow::showDatabaseError(const QString &context, const QString &error)
{
    QMessageBox::warning(this, QStringLiteral("HEPShelf"),
                         QStringLiteral("%1.\n\n%2").arg(context, error));
}
