#include "arxiv_discovery.h"
#include "arxiv_id.h"
#include "arxiv_feed.h"

#include <QAbstractItemView>
#include <QComboBox>
#include <QDate>
#include <QDesktopServices>
#include <QDateTime>
#include <QDialogButtonBox>
#include <QDir>
#include <QFileDialog>
#include <QFileInfo>
#include <QFormLayout>
#include <QHeaderView>
#include <QHBoxLayout>
#include <QItemSelectionModel>
#include <QIODevice>
#include <QLabel>
#include <QLineEdit>
#include <QMessageBox>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QProgressBar>
#include <QPushButton>
#include <QRegularExpression>
#include <QSaveFile>
#include <QSettings>
#include <QSplitter>
#include <QTableWidget>
#include <QTextBrowser>
#include <QTimer>
#include <QUrl>
#include <QUrlQuery>
#include <QVBoxLayout>

#include <algorithm>

namespace {
constexpr int RoleResultIndex = Qt::UserRole;
constexpr int RoleArxivId = Qt::UserRole + 1;

QString quoteQuery(QString text)
{
    text = text.trimmed();
    text.replace(QLatin1Char('\\'), QStringLiteral("\\\\"));
    text.replace(QLatin1Char('"'), QStringLiteral("\\\""));
    return QStringLiteral("\"%1\"").arg(text);
}

QString yearMonthDay(const QString &iso)
{
    if (iso.size() >= 10)
        return iso.left(10);
    return iso;
}
}

ArxivDiscoveryDialog::ArxivDiscoveryDialog(Database *database, QWidget *parent)
    : QDialog(parent), db_(database)
{
    setWindowTitle(QStringLiteral("Discover papers on arXiv"));
    resize(1180, 760);
    setMinimumSize(900, 600);
    setModal(true);
    buildUi();
    restoreSettings();
}

void ArxivDiscoveryDialog::setAuthorSearch(const QString &author, bool startImmediately)
{
    const int index = fieldCombo_->findData(static_cast<int>(SearchField::Author));
    if (index >= 0)
        fieldCombo_->setCurrentIndex(index);
    queryEdit_->setText(author.trimmed());
    if (startImmediately && !author.trimmed().isEmpty())
        QTimer::singleShot(0, this, [this]() { startSearch(); });
}

ArxivDiscoveryDialog::~ArxivDiscoveryDialog()
{
    if (currentDownloadReply_) {
        currentDownloadReply_->abort();
        currentDownloadReply_->deleteLater();
        currentDownloadReply_ = nullptr;
    }
    if (currentSaveFile_) {
        currentSaveFile_->cancelWriting();
        delete currentSaveFile_;
        currentSaveFile_ = nullptr;
    }
    saveSettings();
}

void ArxivDiscoveryDialog::buildUi()
{
    auto *root = new QVBoxLayout(this);
    root->setContentsMargins(14, 14, 14, 14);
    root->setSpacing(10);

    auto *intro = new QLabel(
        QStringLiteral("Search arXiv directly, inspect the abstract, and download papers into your HEPShelf library. "
                       "HEPShelf checks your local index first, so papers you already have are marked as Local."),
        this);
    intro->setWordWrap(true);
    root->addWidget(intro);

    auto *searchRow = new QHBoxLayout;
    fieldCombo_ = new QComboBox(this);
    fieldCombo_->addItem(QStringLiteral("All fields"), static_cast<int>(SearchField::All));
    fieldCombo_->addItem(QStringLiteral("Title"), static_cast<int>(SearchField::Title));
    fieldCombo_->addItem(QStringLiteral("Author"), static_cast<int>(SearchField::Author));
    fieldCombo_->addItem(QStringLiteral("Abstract"), static_cast<int>(SearchField::Abstract));
    fieldCombo_->addItem(QStringLiteral("Category"), static_cast<int>(SearchField::Category));
    fieldCombo_->addItem(QStringLiteral("Exact arXiv ID"), static_cast<int>(SearchField::ExactId));
    fieldCombo_->addItem(QStringLiteral("Advanced arXiv query"), static_cast<int>(SearchField::Advanced));
    fieldCombo_->setMinimumWidth(165);

    queryEdit_ = new QLineEdit(this);
    queryEdit_->setClearButtonEnabled(true);
    queryEdit_->setPlaceholderText(QStringLiteral("e.g. an author, paper title, category, or arXiv ID"));

    sortCombo_ = new QComboBox(this);
    sortCombo_->addItem(QStringLiteral("Relevance"), QStringLiteral("relevance|descending"));
    sortCombo_->addItem(QStringLiteral("Newest submitted"), QStringLiteral("submittedDate|descending"));
    sortCombo_->addItem(QStringLiteral("Oldest submitted"), QStringLiteral("submittedDate|ascending"));
    sortCombo_->addItem(QStringLiteral("Recently updated"), QStringLiteral("lastUpdatedDate|descending"));

    limitCombo_ = new QComboBox(this);
    limitCombo_->addItems({QStringLiteral("25"), QStringLiteral("50"), QStringLiteral("100")});
    limitCombo_->setCurrentText(QStringLiteral("25"));
    limitCombo_->setToolTip(QStringLiteral("Maximum number of results returned by arXiv"));

    searchButton_ = new QPushButton(QStringLiteral("Search arXiv"), this);
    searchButton_->setDefault(true);

    searchRow->addWidget(fieldCombo_);
    searchRow->addWidget(queryEdit_, 1);
    searchRow->addWidget(sortCombo_);
    searchRow->addWidget(limitCombo_);
    searchRow->addWidget(searchButton_);
    root->addLayout(searchRow);

    auto *example = new QLabel(
        QStringLiteral("Examples: Author = “Smith” · Title = “particle physics” · "
                       "Category = “hep-ph” · Advanced = au:\"Smith\" AND cat:hep-ph"),
        this);
    example->setObjectName(QStringLiteral("discoverHint"));
    example->setWordWrap(true);
    root->addWidget(example);

    auto *splitter = new QSplitter(Qt::Vertical, this);

    resultsTable_ = new QTableWidget(splitter);
    resultsTable_->setColumnCount(6);
    resultsTable_->setHorizontalHeaderLabels({QStringLiteral("Title"),
                                               QStringLiteral("Authors"),
                                               QStringLiteral("arXiv"),
                                               QStringLiteral("Submitted"),
                                               QStringLiteral("Category"),
                                               QStringLiteral("Status")});
    resultsTable_->setSelectionBehavior(QAbstractItemView::SelectRows);
    resultsTable_->setSelectionMode(QAbstractItemView::ExtendedSelection);
    resultsTable_->setEditTriggers(QAbstractItemView::NoEditTriggers);
    resultsTable_->setAlternatingRowColors(true);
    resultsTable_->setShowGrid(false);
    resultsTable_->verticalHeader()->setVisible(false);
    resultsTable_->verticalHeader()->setDefaultSectionSize(34);
    resultsTable_->horizontalHeader()->setHighlightSections(false);
    resultsTable_->horizontalHeader()->setSectionResizeMode(0, QHeaderView::Stretch);
    resultsTable_->horizontalHeader()->setSectionResizeMode(1, QHeaderView::Interactive);
    resultsTable_->horizontalHeader()->setSectionResizeMode(2, QHeaderView::ResizeToContents);
    resultsTable_->horizontalHeader()->setSectionResizeMode(3, QHeaderView::ResizeToContents);
    resultsTable_->horizontalHeader()->setSectionResizeMode(4, QHeaderView::ResizeToContents);
    resultsTable_->horizontalHeader()->setSectionResizeMode(5, QHeaderView::ResizeToContents);
    resultsTable_->setColumnWidth(1, 270);

    auto *abstractPane = new QWidget(splitter);
    auto *abstractLayout = new QVBoxLayout(abstractPane);
    abstractLayout->setContentsMargins(0, 8, 0, 0);
    auto *abstractLabel = new QLabel(QStringLiteral("Abstract"), abstractPane);
    abstractLabel->setObjectName(QStringLiteral("discoverSectionTitle"));
    abstractView_ = new QTextBrowser(abstractPane);
    abstractView_->setOpenExternalLinks(true);
    abstractView_->setPlaceholderText(QStringLiteral("Select a search result to read its abstract."));
    abstractLayout->addWidget(abstractLabel);
    abstractLayout->addWidget(abstractView_, 1);

    splitter->addWidget(resultsTable_);
    splitter->addWidget(abstractPane);
    splitter->setStretchFactor(0, 4);
    splitter->setStretchFactor(1, 2);
    splitter->setSizes({470, 210});
    root->addWidget(splitter, 1);

    resultSummary_ = new QLabel(QStringLiteral("No search yet."), this);
    root->addWidget(resultSummary_);

    auto *downloadBox = new QHBoxLayout;
    auto *folderLabel = new QLabel(QStringLiteral("Download to"), this);
    downloadFolderCombo_ = new QComboBox(this);
    downloadFolderCombo_->setEditable(true);
    downloadFolderCombo_->setMinimumWidth(360);
    browseFolderButton_ = new QPushButton(QStringLiteral("Browse…"), this);
    openArxivButton_ = new QPushButton(QStringLiteral("Open arXiv page"), this);
    downloadButton_ = new QPushButton(QStringLiteral("Download selected"), this);
    downloadButton_->setEnabled(false);
    openArxivButton_->setEnabled(false);

    downloadBox->addWidget(folderLabel);
    downloadBox->addWidget(downloadFolderCombo_, 1);
    downloadBox->addWidget(browseFolderButton_);
    downloadBox->addSpacing(10);
    downloadBox->addWidget(openArxivButton_);
    downloadBox->addWidget(downloadButton_);
    root->addLayout(downloadBox);

    auto *progressRow = new QHBoxLayout;
    downloadStatus_ = new QLabel(QStringLiteral("Ready"), this);
    downloadProgress_ = new QProgressBar(this);
    downloadProgress_->setMinimumWidth(260);
    downloadProgress_->setTextVisible(true);
    downloadProgress_->hide();
    progressRow->addWidget(downloadStatus_, 1);
    progressRow->addWidget(downloadProgress_);
    root->addLayout(progressRow);

    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Close, this);
    root->addWidget(buttons);

    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
    connect(searchButton_, &QPushButton::clicked, this, [this]() { startSearch(); });
    connect(queryEdit_, &QLineEdit::returnPressed, this, [this]() { startSearch(); });
    connect(fieldCombo_, &QComboBox::currentIndexChanged, this, [this]() {
        const auto field = static_cast<SearchField>(fieldCombo_->currentData().toInt());
        if (field == SearchField::Category)
            queryEdit_->setPlaceholderText(QStringLiteral("e.g. hep-ph, hep-th, nucl-th"));
        else if (field == SearchField::ExactId)
            queryEdit_->setPlaceholderText(QStringLiteral("e.g. 2609.14048 or hep-ph/9901234"));
        else if (field == SearchField::Advanced)
            queryEdit_->setPlaceholderText(QStringLiteral("e.g. au:\"Smith\" AND cat:hep-ph"));
        else
            queryEdit_->setPlaceholderText(QStringLiteral("Search arXiv…"));
    });
    connect(resultsTable_, &QTableWidget::itemSelectionChanged, this, [this]() {
        showSelectedAbstract();
        const bool selected = !resultsTable_->selectionModel()->selectedRows().isEmpty();
        downloadButton_->setEnabled(selected && !downloadBusy_);
        openArxivButton_->setEnabled(resultsTable_->selectionModel()->selectedRows().size() == 1);
    });
    connect(resultsTable_, &QTableWidget::cellDoubleClicked, this, [this](int, int) {
        openSelectedArxivPage();
    });
    connect(openArxivButton_, &QPushButton::clicked, this, [this]() { openSelectedArxivPage(); });
    connect(browseFolderButton_, &QPushButton::clicked, this, [this]() { chooseDownloadFolder(); });
    connect(downloadButton_, &QPushButton::clicked, this, [this]() { downloadSelected(); });

    setStyleSheet(QStringLiteral(R"QSS(
        QLabel#discoverHint { color: palette(mid); }
        QLabel#discoverSectionTitle { font-weight: 600; }
        QTableWidget { border: 1px solid palette(mid); }
        QTextBrowser { border: 1px solid palette(mid); }
    )QSS"));
}

void ArxivDiscoveryDialog::restoreSettings()
{
    QSettings settings;
    const QByteArray geometry = settings.value(QStringLiteral("discover/geometry")).toByteArray();
    if (!geometry.isEmpty())
        restoreGeometry(geometry);

    fieldCombo_->setCurrentIndex(settings.value(QStringLiteral("discover/field"), 0).toInt());
    sortCombo_->setCurrentIndex(settings.value(QStringLiteral("discover/sort"), 1).toInt());
    limitCombo_->setCurrentText(settings.value(QStringLiteral("discover/limit"), QStringLiteral("25")).toString());

    QStringList folders;
    if (db_)
        folders = db_->folders(nullptr);
    const QString remembered = settings.value(QStringLiteral("discover/downloadFolder")).toString();
    if (!remembered.isEmpty() && !folders.contains(remembered))
        folders.prepend(remembered);
    downloadFolderCombo_->addItems(folders);
    if (!remembered.isEmpty())
        downloadFolderCombo_->setCurrentText(remembered);
}

void ArxivDiscoveryDialog::saveSettings()
{
    QSettings settings;
    settings.setValue(QStringLiteral("discover/geometry"), saveGeometry());
    settings.setValue(QStringLiteral("discover/field"), fieldCombo_->currentIndex());
    settings.setValue(QStringLiteral("discover/sort"), sortCombo_->currentIndex());
    settings.setValue(QStringLiteral("discover/limit"), limitCombo_->currentText());
    settings.setValue(QStringLiteral("discover/downloadFolder"), currentDownloadFolder());
}

QString ArxivDiscoveryDialog::buildSearchExpression(QString *error) const
{
    const QString raw = queryEdit_->text().trimmed();
    if (raw.isEmpty()) {
        if (error)
            *error = QStringLiteral("Enter a title, author, category, arXiv ID, or search expression.");
        return {};
    }

    const auto field = static_cast<SearchField>(fieldCombo_->currentData().toInt());
    switch (field) {
    case SearchField::All:
        return QStringLiteral("all:%1").arg(quoteQuery(raw));
    case SearchField::Title:
        return QStringLiteral("ti:%1").arg(quoteQuery(raw));
    case SearchField::Author:
        return QStringLiteral("au:%1").arg(quoteQuery(raw));
    case SearchField::Abstract:
        return QStringLiteral("abs:%1").arg(quoteQuery(raw));
    case SearchField::Category: {
        QString category = raw;
        category.remove(QRegularExpression(QStringLiteral("\\s+")));
        return QStringLiteral("cat:%1").arg(category);
    }
    case SearchField::Advanced:
        return raw;
    case SearchField::ExactId:
        return {};
    }
    return {};
}

void ArxivDiscoveryDialog::startSearch()
{
    if (searchBusy_ || downloadBusy_)
        return;

    QString expressionError;
    const QString expression = buildSearchExpression(&expressionError);
    const auto field = static_cast<SearchField>(fieldCombo_->currentData().toInt());
    if (field != SearchField::ExactId && expression.isEmpty()) {
        QMessageBox::information(this, QStringLiteral("Search arXiv"), expressionError);
        return;
    }

    const QString raw = queryEdit_->text().trimmed();
    if (field == SearchField::ExactId && raw.isEmpty()) {
        QMessageBox::information(this, QStringLiteral("Search arXiv"),
                                 QStringLiteral("Enter an arXiv identifier."));
        return;
    }

    const qint64 nowMs = QDateTime::currentMSecsSinceEpoch();
    const qint64 elapsedMs = lastApiQueryMs_ > 0 ? nowMs - lastApiQueryMs_ : 3000;
    if (elapsedMs < 3000) {
        const int waitMs = static_cast<int>(3000 - elapsedMs);
        searchBusy_ = true;
        searchButton_->setEnabled(false);
        resultSummary_->setText(QStringLiteral("Waiting briefly before the next arXiv API request…"));
        QTimer::singleShot(waitMs, this, [this]() {
            searchBusy_ = false;
            searchButton_->setEnabled(true);
            startSearch();
        });
        return;
    }

    QUrl url(QStringLiteral("https://export.arxiv.org/api/query"));
    QUrlQuery query;
    if (field == SearchField::ExactId) {
        QString id = raw.trimmed();
        id.remove(QRegularExpression(QStringLiteral("^arXiv:\\s*"), QRegularExpression::CaseInsensitiveOption));
        if (id.startsWith(QStringLiteral("http://"), Qt::CaseInsensitive)
            || id.startsWith(QStringLiteral("https://"), Qt::CaseInsensitive)) {
            const QUrl pastedUrl(id);
            id = pastedUrl.path();
            if (id.startsWith(QStringLiteral("/abs/")))
                id.remove(0, 5);
            else if (id.startsWith(QStringLiteral("/pdf/")))
                id.remove(0, 5);
            while (id.startsWith(QLatin1Char('/')))
                id.remove(0, 1);
            if (id.endsWith(QStringLiteral(".pdf"), Qt::CaseInsensitive))
                id.chop(4);
        }
        id = ArxivId::stripVersion(id.trimmed());
        query.addQueryItem(QStringLiteral("id_list"), id);
        query.addQueryItem(QStringLiteral("max_results"), QStringLiteral("1"));
    } else {
        query.addQueryItem(QStringLiteral("search_query"), expression);
        query.addQueryItem(QStringLiteral("start"), QStringLiteral("0"));
        query.addQueryItem(QStringLiteral("max_results"), limitCombo_->currentText());

        const QStringList sortParts = sortCombo_->currentData().toString().split(QLatin1Char('|'));
        if (sortParts.size() == 2) {
            query.addQueryItem(QStringLiteral("sortBy"), sortParts.at(0));
            query.addQueryItem(QStringLiteral("sortOrder"), sortParts.at(1));
        }
    }
    url.setQuery(query);

    QNetworkRequest request(url);
    request.setHeader(QNetworkRequest::UserAgentHeader,
                      QStringLiteral("HEPShelf/0.9.0 local-literature-library"));
    request.setAttribute(QNetworkRequest::RedirectPolicyAttribute,
                         QNetworkRequest::NoLessSafeRedirectPolicy);
    request.setTransferTimeout(30000);

    searchBusy_ = true;
    searchButton_->setEnabled(false);
    queryEdit_->setEnabled(false);
    resultSummary_->setText(QStringLiteral("Searching arXiv…"));
    downloadStatus_->setText(QStringLiteral("Querying arXiv metadata…"));

    lastApiQueryMs_ = QDateTime::currentMSecsSinceEpoch();
    QNetworkReply *reply = network_.get(request);
    connect(reply, &QNetworkReply::finished, this, [this, reply]() {
        searchBusy_ = false;
        searchButton_->setEnabled(true);
        queryEdit_->setEnabled(true);

        const int status = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
        if (reply->error() != QNetworkReply::NoError) {
            const QString message = QStringLiteral("arXiv search failed: %1 (HTTP %2)")
                                        .arg(reply->errorString())
                                        .arg(status > 0 ? QString::number(status) : QStringLiteral("n/a"));
            reply->deleteLater();
            resultSummary_->setText(message);
            downloadStatus_->setText(message);
            QMessageBox::warning(this, QStringLiteral("Search arXiv"), message);
            return;
        }

        QString parseError;
        results_ = parseFeed(reply->readAll(), &parseError);
        reply->deleteLater();
        if (!parseError.isEmpty()) {
            results_.clear();
            resultSummary_->setText(QStringLiteral("Could not parse the arXiv response."));
            downloadStatus_->setText(parseError);
            QMessageBox::warning(this, QStringLiteral("Search arXiv"), parseError);
            return;
        }

        populateResults();
        downloadStatus_->setText(results_.isEmpty()
                                     ? QStringLiteral("No matching papers found.")
                                     : QStringLiteral("Select one or more papers to download."));
    });
}

QList<PaperRecord> ArxivDiscoveryDialog::parseFeed(const QByteArray &xml, QString *error) const
{
    const ArxivFeedResult result = parseArxivFeed(xml);
    if (error)
        *error = result.error;
    return result.papers;
}

void ArxivDiscoveryDialog::populateResults()
{
    resultsTable_->setSortingEnabled(false);
    resultsTable_->clearContents();
    resultsTable_->setRowCount(results_.size());

    int localCount = 0;
    for (int row = 0; row < results_.size(); ++row) {
        const PaperRecord &paper = results_.at(row);
        const bool local = resultIsLocal(paper.arxivId);
        if (local)
            ++localCount;

        auto *title = new QTableWidgetItem(paper.title);
        title->setData(RoleResultIndex, row);
        title->setData(RoleArxivId, paper.arxivId);
        title->setToolTip(paper.title);

        auto *authors = new QTableWidgetItem(compactAuthors(paper.authors));
        authors->setToolTip(paper.authors);
        auto *id = new QTableWidgetItem(paper.arxivId);
        auto *submitted = new QTableWidgetItem(yearMonthDay(paper.published));
        auto *category = new QTableWidgetItem(paper.primaryCategory);
        auto *status = new QTableWidgetItem(local ? QStringLiteral("Local") : QStringLiteral("Available"));
        if (local)
            status->setToolTip(QStringLiteral("A local PDF for this arXiv record is already indexed by HEPShelf."));

        resultsTable_->setItem(row, 0, title);
        resultsTable_->setItem(row, 1, authors);
        resultsTable_->setItem(row, 2, id);
        resultsTable_->setItem(row, 3, submitted);
        resultsTable_->setItem(row, 4, category);
        resultsTable_->setItem(row, 5, status);
    }

    // Preserve the ordering requested from arXiv (relevance/newest/etc.).
    resultsTable_->setSortingEnabled(false);
    resultSummary_->setText(
        QStringLiteral("%1 result(s) · %2 already in your local library")
            .arg(results_.size())
            .arg(localCount));

    if (resultsTable_->rowCount() > 0)
        resultsTable_->selectRow(0);
    else
        abstractView_->clear();
}

void ArxivDiscoveryDialog::showSelectedAbstract()
{
    const QModelIndexList rows = resultsTable_->selectionModel()->selectedRows();
    if (rows.size() != 1) {
        abstractView_->setPlainText(rows.isEmpty()
                                        ? QStringLiteral("Select a result to read its abstract.")
                                        : QStringLiteral("%1 papers selected.").arg(rows.size()));
        return;
    }

    QTableWidgetItem *item = resultsTable_->item(rows.first().row(), 0);
    if (!item)
        return;
    const int index = item->data(RoleResultIndex).toInt();
    if (index < 0 || index >= results_.size())
        return;

    const PaperRecord &paper = results_.at(index);
    QString text;
    text += QStringLiteral("%1\n\n").arg(paper.title);
    text += QStringLiteral("%1\n").arg(paper.authors);
    text += QStringLiteral("arXiv:%1 · %2 · %3\n\n")
                .arg(paper.arxivId, yearMonthDay(paper.published), paper.primaryCategory);
    text += paper.abstractText;
    if (!paper.comments.isEmpty())
        text += QStringLiteral("\n\nComments: %1").arg(paper.comments);
    if (!paper.journalRef.isEmpty())
        text += QStringLiteral("\nJournal reference: %1").arg(paper.journalRef);
    if (!paper.doi.isEmpty())
        text += QStringLiteral("\nDOI: %1").arg(paper.doi);
    abstractView_->setPlainText(text);
}

void ArxivDiscoveryDialog::openSelectedArxivPage()
{
    const QModelIndexList rows = resultsTable_->selectionModel()->selectedRows();
    if (rows.size() != 1)
        return;
    QTableWidgetItem *item = resultsTable_->item(rows.first().row(), 0);
    if (!item)
        return;
    const int index = item->data(RoleResultIndex).toInt();
    if (index < 0 || index >= results_.size())
        return;
    const QString id = results_.at(index).arxivId;
    QDesktopServices::openUrl(QUrl(QStringLiteral("https://arxiv.org/abs/%1").arg(id)));
}

void ArxivDiscoveryDialog::chooseDownloadFolder()
{
    const QString initial = currentDownloadFolder();
    const QString folder = QFileDialog::getExistingDirectory(
        this, QStringLiteral("Choose where arXiv PDFs should be saved"), initial);
    if (folder.isEmpty())
        return;

    const int existing = downloadFolderCombo_->findText(folder);
    if (existing < 0)
        downloadFolderCombo_->insertItem(0, folder);
    downloadFolderCombo_->setCurrentText(folder);
}

QString ArxivDiscoveryDialog::currentDownloadFolder() const
{
    const QString path = downloadFolderCombo_->currentText().trimmed();
    return path.isEmpty() ? QString() : QDir::cleanPath(path);
}

void ArxivDiscoveryDialog::downloadSelected()
{
    if (downloadBusy_ || searchBusy_ || !db_)
        return;

    const QModelIndexList rows = resultsTable_->selectionModel()->selectedRows();
    if (rows.isEmpty())
        return;

    const QString folder = currentDownloadFolder();
    if (folder.isEmpty()) {
        chooseDownloadFolder();
        if (currentDownloadFolder().isEmpty())
            return;
    }

    QDir dir(currentDownloadFolder());
    if (!dir.exists() && !QDir().mkpath(dir.absolutePath())) {
        QMessageBox::warning(this, QStringLiteral("Download papers"),
                             QStringLiteral("Could not create the destination folder:\n%1")
                                 .arg(dir.absolutePath()));
        return;
    }

    QString dbError;
    if (!db_->addFolder(dir.absolutePath(), &dbError)) {
        QMessageBox::warning(this, QStringLiteral("Download papers"),
                             QStringLiteral("Could not add the destination to watched folders:\n%1")
                                 .arg(dbError));
        return;
    }

    pendingDownloads_.clear();
    for (const QModelIndex &rowIndex : rows) {
        QTableWidgetItem *item = resultsTable_->item(rowIndex.row(), 0);
        if (!item)
            continue;
        const int index = item->data(RoleResultIndex).toInt();
        if (index < 0 || index >= results_.size())
            continue;
        if (!pendingDownloads_.contains(index))
            pendingDownloads_ << index;
    }

    std::sort(pendingDownloads_.begin(), pendingDownloads_.end());
    downloadBusy_ = true;
    downloadButton_->setEnabled(false);
    searchButton_->setEnabled(false);
    browseFolderButton_->setEnabled(false);
    downloadFolderCombo_->setEnabled(false);
    downloadProgress_->setRange(0, pendingDownloads_.size());
    downloadProgress_->setValue(0);
    downloadProgress_->setFormat(QStringLiteral("Papers %v/%m"));
    downloadProgress_->show();
    downloadStatus_->setText(QStringLiteral("Preparing downloads…"));
    saveSettings();
    downloadNext();
}

void ArxivDiscoveryDialog::downloadNext()
{
    if (pendingDownloads_.isEmpty()) {
        downloadBusy_ = false;
        currentDownloadIndex_ = -1;
        downloadButton_->setEnabled(!resultsTable_->selectionModel()->selectedRows().isEmpty());
        searchButton_->setEnabled(true);
        browseFolderButton_->setEnabled(true);
        downloadFolderCombo_->setEnabled(true);
        downloadProgress_->hide();
        downloadStatus_->setText(QStringLiteral("Download queue finished."));
        populateResults();
        return;
    }

    currentDownloadIndex_ = pendingDownloads_.takeFirst();
    if (currentDownloadIndex_ < 0 || currentDownloadIndex_ >= results_.size()) {
        QTimer::singleShot(0, this, [this]() { downloadNext(); });
        return;
    }

    const PaperRecord &paper = results_.at(currentDownloadIndex_);
    const int completed = downloadProgress_->maximum() - pendingDownloads_.size() - 1;
    downloadProgress_->setValue(completed);

    if (resultIsLocal(paper.arxivId)) {
        downloadStatus_->setText(QStringLiteral("Skipping arXiv:%1 — already local.").arg(paper.arxivId));
        downloadProgress_->setValue(completed + 1);
        QTimer::singleShot(250, this, [this]() { downloadNext(); });
        return;
    }

    startDownloadForIndex(currentDownloadIndex_);
}

void ArxivDiscoveryDialog::startDownloadForIndex(int resultIndex)
{
    const PaperRecord &paper = results_.at(resultIndex);
    currentDownloadPath_ = targetPathFor(paper);

    QFileInfo existing(currentDownloadPath_);
    if (existing.exists() && existing.isFile()) {
        QString error;
        if (!db_->upsertPaper(paper, &error)
            || !db_->upsertFile(existing.absoluteFilePath(), paper.arxivId,
                                existing.size(), existing.lastModified().toSecsSinceEpoch(), &error)) {
            finishDownload(false, QStringLiteral("Could not index existing file: %1").arg(error));
            return;
        }
        libraryChanged_ = true;
        markResultLocal(paper.arxivId, existing.absoluteFilePath());
        finishDownload(true, QStringLiteral("Indexed existing file for arXiv:%1").arg(paper.arxivId));
        return;
    }

    currentDownloadPrefix_.clear();
    currentSaveFile_ = new QSaveFile(currentDownloadPath_);
    if (!currentSaveFile_->open(QIODevice::WriteOnly)) {
        const QString error = currentSaveFile_->errorString();
        delete currentSaveFile_;
        currentSaveFile_ = nullptr;
        finishDownload(false, QStringLiteral("Could not create %1: %2").arg(currentDownloadPath_, error));
        return;
    }

    QUrl url(QStringLiteral("https://arxiv.org/pdf/%1.pdf").arg(paper.arxivId));
    QNetworkRequest request(url);
    request.setHeader(QNetworkRequest::UserAgentHeader,
                      QStringLiteral("HEPShelf/0.9.0 local-literature-library"));
    request.setAttribute(QNetworkRequest::RedirectPolicyAttribute,
                         QNetworkRequest::NoLessSafeRedirectPolicy);
    request.setTransferTimeout(90000);

    downloadStatus_->setText(QStringLiteral("Downloading arXiv:%1 — %2")
                                 .arg(paper.arxivId, paper.title));

    currentDownloadReply_ = network_.get(request);
    connect(currentDownloadReply_, &QNetworkReply::downloadProgress, this,
            [this](qint64 received, qint64 total) {
                if (total > 0) {
                    const int percent = static_cast<int>((received * 100) / total);
                    downloadStatus_->setText(QStringLiteral("Downloading PDF… %1% (%2 / %3 MiB)")
                                                 .arg(percent)
                                                 .arg(received / (1024.0 * 1024.0), 0, 'f', 1)
                                                 .arg(total / (1024.0 * 1024.0), 0, 'f', 1));
                }
            });
    connect(currentDownloadReply_, &QIODevice::readyRead, this, [this]() {
        if (!currentSaveFile_ || !currentDownloadReply_)
            return;
        const QByteArray chunk = currentDownloadReply_->readAll();
        if (currentDownloadPrefix_.size() < 8)
            currentDownloadPrefix_.append(chunk.left(8 - currentDownloadPrefix_.size()));
        currentSaveFile_->write(chunk);
    });
    connect(currentDownloadReply_, &QNetworkReply::finished, this, [this, paper]() {
        QNetworkReply *reply = currentDownloadReply_;
        currentDownloadReply_ = nullptr;
        if (!reply)
            return;

        if (currentSaveFile_) {
            const QByteArray tail = reply->readAll();
            if (currentDownloadPrefix_.size() < 8)
                currentDownloadPrefix_.append(tail.left(8 - currentDownloadPrefix_.size()));
            currentSaveFile_->write(tail);
        }

        const int status = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
        const QString contentType = reply->header(QNetworkRequest::ContentTypeHeader).toString();
        const bool networkOk = reply->error() == QNetworkReply::NoError && status >= 200 && status < 300;
        const bool looksLikePdf = currentDownloadPrefix_.startsWith("%PDF-")
                                  || contentType.contains(QStringLiteral("application/pdf"), Qt::CaseInsensitive);
        const QString networkError = reply->errorString();
        reply->deleteLater();

        if (!networkOk || !looksLikePdf) {
            if (currentSaveFile_)
                currentSaveFile_->cancelWriting();
            delete currentSaveFile_;
            currentSaveFile_ = nullptr;
            const QString reason = !networkOk
                                       ? QStringLiteral("%1 (HTTP %2)")
                                             .arg(networkError)
                                             .arg(status > 0 ? QString::number(status) : QStringLiteral("n/a"))
                                       : QStringLiteral("the server response was not a PDF");
            finishDownload(false,
                           QStringLiteral("Download failed for arXiv:%1: %2")
                               .arg(paper.arxivId, reason));
            return;
        }

        if (QFileInfo::exists(currentDownloadPath_)) {
            currentSaveFile_->cancelWriting();
            delete currentSaveFile_;
            currentSaveFile_ = nullptr;
            finishDownload(false,
                           QStringLiteral("The target file appeared while downloading, so HEPShelf did not overwrite it:\n%1")
                               .arg(currentDownloadPath_));
            return;
        }

        if (!currentSaveFile_ || !currentSaveFile_->commit()) {
            const QString saveError = currentSaveFile_ ? currentSaveFile_->errorString()
                                                       : QStringLiteral("unknown file error");
            delete currentSaveFile_;
            currentSaveFile_ = nullptr;
            finishDownload(false,
                           QStringLiteral("Could not save arXiv:%1: %2").arg(paper.arxivId, saveError));
            return;
        }
        delete currentSaveFile_;
        currentSaveFile_ = nullptr;

        QFileInfo info(currentDownloadPath_);
        QString error;
        if (!db_->upsertPaper(paper, &error)
            || !db_->upsertFile(info.absoluteFilePath(), paper.arxivId,
                                info.size(), info.lastModified().toSecsSinceEpoch(), &error)) {
            finishDownload(false,
                           QStringLiteral("PDF was downloaded but could not be indexed: %1").arg(error));
            return;
        }

        libraryChanged_ = true;
        markResultLocal(paper.arxivId, info.absoluteFilePath());
        finishDownload(true, QStringLiteral("Added arXiv:%1 to your library.").arg(paper.arxivId));
    });
}

void ArxivDiscoveryDialog::finishDownload(bool success, const QString &message)
{
    if (!message.isEmpty())
        downloadStatus_->setText(message);

    if (!success && !message.isEmpty()) {
        QMessageBox::warning(this, QStringLiteral("Download paper"), message);
    }

    const int completed = downloadProgress_->maximum() - pendingDownloads_.size();
    downloadProgress_->setValue(qMin(completed, downloadProgress_->maximum()));

    QTimer::singleShot(success ? 900 : 250, this, [this]() { downloadNext(); });
}

void ArxivDiscoveryDialog::markResultLocal(const QString &arxivId, const QString &path)
{
    Q_UNUSED(path);
    for (int row = 0; row < resultsTable_->rowCount(); ++row) {
        QTableWidgetItem *title = resultsTable_->item(row, 0);
        if (!title || title->data(RoleArxivId).toString() != arxivId)
            continue;
        if (QTableWidgetItem *status = resultsTable_->item(row, 5)) {
            status->setText(QStringLiteral("Local"));
            status->setToolTip(QStringLiteral("Downloaded and indexed by HEPShelf."));
        }
    }
}

bool ArxivDiscoveryDialog::resultIsLocal(const QString &arxivId) const
{
    if (!db_)
        return false;
    PaperDetails details;
    QString error;
    if (!db_->paperDetails(arxivId, &details, &error) || !error.isEmpty())
        return false;
    for (const FileRecord &file : details.files) {
        if (QFileInfo::exists(file.path))
            return true;
    }
    return false;
}

QString ArxivDiscoveryDialog::targetPathFor(const PaperRecord &paper) const
{
    QDir dir(currentDownloadFolder());
    return dir.filePath(filenameId(paper.arxivId) + QStringLiteral(".pdf"));
}

QString ArxivDiscoveryDialog::compactAuthors(const QString &authors, int maxChars)
{
    if (authors.size() <= maxChars)
        return authors;
    const QString clipped = authors.left(maxChars - 1);
    const int comma = clipped.lastIndexOf(QStringLiteral(", "));
    return (comma > maxChars / 2 ? clipped.left(comma) : clipped) + QStringLiteral("…");
}

QString ArxivDiscoveryDialog::filenameId(QString arxivId)
{
    arxivId.replace(QLatin1Char('/'), QLatin1Char('_'));
    arxivId.replace(QRegularExpression(QStringLiteral("[^A-Za-z0-9._-]+")), QStringLiteral("_"));
    return arxivId;
}
