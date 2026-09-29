#include "cited_by_dialog.h"

#include <QAbstractItemView>
#include <QComboBox>
#include <QDateTime>
#include <QDesktopServices>
#include <QDialogButtonBox>
#include <QDir>
#include <QFileDialog>
#include <QFileInfo>
#include <QHeaderView>
#include <QHBoxLayout>
#include <QItemSelectionModel>
#include <QLabel>
#include <QMessageBox>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QProgressBar>
#include <QPushButton>
#include <QRegularExpression>
#include <QSaveFile>
#include <QSettings>
#include <QTableWidget>
#include <QTimer>
#include <QUrl>
#include <QVBoxLayout>

namespace {
constexpr int RolePaperIndex = Qt::UserRole;

QString compact(QString text, int max = 105)
{
    text = text.simplified();
    if (text.size() <= max)
        return text;
    return text.left(max - 1) + QChar(0x2026);
}

QString displayTitle(const RelatedPaperRecord &paper)
{
    if (!paper.title.trimmed().isEmpty())
        return paper.title.trimmed();
    if (!paper.arxivId.isEmpty())
        return QStringLiteral("arXiv:%1").arg(paper.arxivId);
    if (paper.inspireRecid > 0)
        return QStringLiteral("INSPIRE %1").arg(paper.inspireRecid);
    return QStringLiteral("Untitled citing paper");
}
}

CitedByDialog::CitedByDialog(Database *database,
                             const QString &sourceArxivId,
                             QWidget *parent)
    : QDialog(parent), db_(database), sourceArxivId_(sourceArxivId), inspire_(this)
{
    setWindowTitle(QStringLiteral("Cited by — %1").arg(sourceArxivId_));
    resize(1240, 760);
    setMinimumSize(900, 560);
    buildUi();
    reload();

    const CitingCacheInfo cache = db_ ? db_->citingCacheInfo(sourceArxivId_, nullptr) : CitingCacheInfo{};
    if (cache.fetchedAt.isEmpty())
        refreshFromInspire();
}

CitedByDialog::~CitedByDialog()
{
    if (downloadReply_) {
        downloadReply_->abort();
        downloadReply_->deleteLater();
    }
    if (saveFile_) {
        saveFile_->cancelWriting();
        delete saveFile_;
    }
    QSettings settings;
    settings.setValue(QStringLiteral("citedBy/geometry"), saveGeometry());
    settings.setValue(QStringLiteral("citedBy/downloadFolder"), downloadFolder());
    if (limit_)
        settings.setValue(QStringLiteral("citedBy/limit"), limit_->currentData());
}

void CitedByDialog::buildUi()
{
    auto *root = new QVBoxLayout(this);
    root->setContentsMargins(14, 14, 14, 14);
    root->setSpacing(9);

    auto *intro = new QLabel(
        QStringLiteral("Cited-by results come from INSPIRE's citation graph. HEPShelf resolves every returned paper against your local library, so papers you already own open immediately and are never offered as duplicate downloads."),
        this);
    intro->setWordWrap(true);
    root->addWidget(intro);

    auto *top = new QHBoxLayout;
    summary_ = new QLabel(this);
    summary_->setTextInteractionFlags(Qt::TextSelectableByMouse);
    top->addWidget(summary_, 1);
    top->addWidget(new QLabel(QStringLiteral("Fetch up to"), this));
    limit_ = new QComboBox(this);
    for (int value : {50, 100, 250, 500})
        limit_->addItem(QString::number(value), value);
    QSettings settings;
    const int rememberedLimit = settings.value(QStringLiteral("citedBy/limit"), 250).toInt();
    const int limitIndex = limit_->findData(rememberedLimit);
    limit_->setCurrentIndex(limitIndex >= 0 ? limitIndex : 2);
    top->addWidget(limit_);
    refreshButton_ = new QPushButton(QStringLiteral("Refresh from INSPIRE"), this);
    top->addWidget(refreshButton_);
    root->addLayout(top);

    table_ = new QTableWidget(this);
    table_->setColumnCount(7);
    table_->setHorizontalHeaderLabels({QStringLiteral("Title"), QStringLiteral("Authors"),
                                       QStringLiteral("Year"), QStringLiteral("arXiv"),
                                       QStringLiteral("Citations"), QStringLiteral("INSPIRE"),
                                       QStringLiteral("Status")});
    table_->setSelectionBehavior(QAbstractItemView::SelectRows);
    table_->setSelectionMode(QAbstractItemView::ExtendedSelection);
    table_->setEditTriggers(QAbstractItemView::NoEditTriggers);
    table_->setAlternatingRowColors(true);
    table_->setShowGrid(false);
    table_->verticalHeader()->setVisible(false);
    table_->horizontalHeader()->setSectionResizeMode(0, QHeaderView::Stretch);
    table_->horizontalHeader()->setSectionResizeMode(1, QHeaderView::Interactive);
    table_->horizontalHeader()->setSectionResizeMode(2, QHeaderView::ResizeToContents);
    table_->horizontalHeader()->setSectionResizeMode(3, QHeaderView::ResizeToContents);
    table_->horizontalHeader()->setSectionResizeMode(4, QHeaderView::ResizeToContents);
    table_->horizontalHeader()->setSectionResizeMode(5, QHeaderView::ResizeToContents);
    table_->horizontalHeader()->setSectionResizeMode(6, QHeaderView::ResizeToContents);
    table_->setColumnWidth(1, 260);
    table_->setSortingEnabled(true);
    root->addWidget(table_, 1);

    auto *actions = new QHBoxLayout;
    openButton_ = new QPushButton(QStringLiteral("Open local"), this);
    arxivButton_ = new QPushButton(QStringLiteral("Open arXiv"), this);
    inspireButton_ = new QPushButton(QStringLiteral("Open INSPIRE"), this);
    actions->addWidget(openButton_);
    actions->addWidget(arxivButton_);
    actions->addWidget(inspireButton_);
    actions->addStretch();
    actions->addWidget(new QLabel(QStringLiteral("Download missing to"), this));
    folder_ = new QComboBox(this);
    folder_->setEditable(true);
    folder_->setMinimumWidth(330);
    if (db_)
        folder_->addItems(db_->folders(nullptr));
    const QString remembered = settings.value(QStringLiteral("citedBy/downloadFolder")).toString();
    if (!remembered.isEmpty()) {
        if (folder_->findText(remembered) < 0)
            folder_->insertItem(0, remembered);
        folder_->setCurrentText(remembered);
    }
    auto *browse = new QPushButton(QStringLiteral("Browse…"), this);
    auto *selectMissing = new QPushButton(QStringLiteral("Select missing"), this);
    downloadButton_ = new QPushButton(QStringLiteral("Download selected"), this);
    actions->addWidget(folder_, 1);
    actions->addWidget(browse);
    actions->addWidget(selectMissing);
    actions->addWidget(downloadButton_);
    root->addLayout(actions);

    auto *progressRow = new QHBoxLayout;
    status_ = new QLabel(QStringLiteral("Ready"), this);
    progress_ = new QProgressBar(this);
    progress_->hide();
    progress_->setMinimumWidth(260);
    progressRow->addWidget(status_, 1);
    progressRow->addWidget(progress_);
    root->addLayout(progressRow);

    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Close, this);
    root->addWidget(buttons);

    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
    connect(refreshButton_, &QPushButton::clicked, this, [this]() { refreshFromInspire(); });
    connect(openButton_, &QPushButton::clicked, this, [this]() { openSelected(); });
    connect(arxivButton_, &QPushButton::clicked, this, [this]() { openSelectedArxiv(); });
    connect(inspireButton_, &QPushButton::clicked, this, [this]() { openSelectedInspire(); });
    connect(browse, &QPushButton::clicked, this, [this]() { chooseDownloadFolder(); });
    connect(selectMissing, &QPushButton::clicked, this, [this]() {
        table_->clearSelection();
        for (int row = 0; row < table_->rowCount(); ++row) {
            QTableWidgetItem *item = table_->item(row, 0);
            if (!item)
                continue;
            const int index = item->data(RolePaperIndex).toInt();
            if (index >= 0 && index < papers_.size()
                && !papers_[index].local && !papers_[index].arxivId.isEmpty()) {
                table_->selectionModel()->select(table_->model()->index(row, 0),
                                                  QItemSelectionModel::Select | QItemSelectionModel::Rows);
            }
        }
    });
    connect(downloadButton_, &QPushButton::clicked, this, [this]() { downloadSelected(); });
    connect(table_, &QTableWidget::cellDoubleClicked, this, [this](int, int) { openSelected(); });
    connect(table_, &QTableWidget::itemSelectionChanged, this, [this]() {
        const QList<int> selected = selectedIndexes();
        bool local = false;
        bool arxiv = false;
        bool inspire = false;
        bool downloadable = false;
        for (int index : selected) {
            if (index < 0 || index >= papers_.size())
                continue;
            const auto &paper = papers_.at(index);
            local |= paper.local;
            arxiv |= !paper.arxivId.isEmpty();
            inspire |= paper.inspireRecid > 0;
            downloadable |= !paper.local && !paper.arxivId.isEmpty();
        }
        openButton_->setEnabled(selected.size() == 1 && local);
        arxivButton_->setEnabled(selected.size() == 1 && arxiv);
        inspireButton_->setEnabled(selected.size() == 1 && inspire);
        downloadButton_->setEnabled(!downloading_ && downloadable);
    });

    const QByteArray geometry = settings.value(QStringLiteral("citedBy/geometry")).toByteArray();
    if (!geometry.isEmpty())
        restoreGeometry(geometry);
}

void CitedByDialog::reload()
{
    QString error;
    papers_ = db_ ? db_->citingPapersForPaper(sourceArxivId_, &error) : QList<RelatedPaperRecord>{};
    if (!error.isEmpty()) {
        status_->setText(error);
        return;
    }

    table_->setSortingEnabled(false);
    table_->setRowCount(papers_.size());
    for (int row = 0; row < papers_.size(); ++row) {
        const RelatedPaperRecord &paper = papers_.at(row);
        auto *title = new QTableWidgetItem(compact(displayTitle(paper)));
        title->setData(RolePaperIndex, row);
        title->setToolTip(displayTitle(paper));
        table_->setItem(row, 0, title);
        table_->setItem(row, 1, new QTableWidgetItem(compact(paper.authors, 80)));
        table_->setItem(row, 2, new QTableWidgetItem(paper.year));
        table_->setItem(row, 3, new QTableWidgetItem(paper.arxivId));
        auto *citations = new QTableWidgetItem;
        if (paper.citationCount >= 0) {
            citations->setData(Qt::DisplayRole, paper.citationCount);
            citations->setTextAlignment(Qt::AlignRight | Qt::AlignVCenter);
        }
        table_->setItem(row, 4, citations);
        table_->setItem(row, 5, new QTableWidgetItem(paper.inspireRecid > 0 ? QString::number(paper.inspireRecid) : QString()));
        auto *status = new QTableWidgetItem;
        if (paper.local) {
            status->setText(QStringLiteral("Local"));
            status->setToolTip(paper.localPath);
        } else if (!paper.arxivId.isEmpty()) {
            status->setText(QStringLiteral("Available on arXiv"));
        } else {
            status->setText(QStringLiteral("INSPIRE only"));
        }
        table_->setItem(row, 6, status);
    }
    table_->setSortingEnabled(true);
    updateSummary();
    if (table_->rowCount() > 0)
        table_->selectRow(0);
}

void CitedByDialog::updateSummary()
{
    int local = 0;
    int downloadable = 0;
    for (const auto &paper : papers_) {
        if (paper.local)
            ++local;
        else if (!paper.arxivId.isEmpty())
            ++downloadable;
    }
    const CitingCacheInfo cache = db_ ? db_->citingCacheInfo(sourceArxivId_, nullptr) : CitingCacheInfo{};
    const int total = cache.totalCount >= 0 ? cache.totalCount : papers_.size();
    const QString truncated = total > papers_.size()
                                  ? QStringLiteral(" · showing newest %1").arg(papers_.size())
                                  : QString();
    summary_->setText(QStringLiteral("%1 citing papers · %2 local · %3 downloadable%4")
                          .arg(total).arg(local).arg(downloadable).arg(truncated));
}

void CitedByDialog::refreshFromInspire()
{
    if (refreshing_ || !db_)
        return;
    PaperDetails details;
    QString error;
    if (!db_->paperDetails(sourceArxivId_, &details, &error)) {
        QMessageBox::warning(this, QStringLiteral("HEPShelf"), error);
        return;
    }

    refreshing_ = true;
    refreshButton_->setEnabled(false);
    status_->setText(QStringLiteral("Resolving INSPIRE record…"));

    if (details.citations.inspireRecid > 0) {
        fetchWithRecid(details.citations.inspireRecid);
        return;
    }

    inspire_.fetchPaper(sourceArxivId_, [this](const InspirePaperData &data, const QString &fetchError) {
        if (!fetchError.isEmpty()) {
            refreshing_ = false;
            refreshButton_->setEnabled(true);
            status_->setText(fetchError);
            QMessageBox::warning(this, QStringLiteral("INSPIRE"), fetchError);
            return;
        }
        QString dbError;
        if (!db_->updateCitationMetrics(sourceArxivId_, data.metrics, &dbError)
            || !db_->replaceReferences(sourceArxivId_, data.references, &dbError)) {
            refreshing_ = false;
            refreshButton_->setEnabled(true);
            status_->setText(dbError);
            QMessageBox::warning(this, QStringLiteral("HEPShelf"), dbError);
            return;
        }
        metricsChanged_ = true;
        fetchWithRecid(data.metrics.inspireRecid);
    });
}

void CitedByDialog::fetchWithRecid(int recid)
{
    const int requested = limit_ ? limit_->currentData().toInt() : 250;
    status_->setText(QStringLiteral("Fetching papers that cite this work from INSPIRE…"));
    inspire_.fetchCitingPapers(recid, requested,
                              [this](const InspireCitingData &data, const QString &error) {
        refreshing_ = false;
        refreshButton_->setEnabled(true);
        if (!error.isEmpty()) {
            status_->setText(error);
            QMessageBox::warning(this, QStringLiteral("INSPIRE"), error);
            return;
        }
        QString dbError;
        if (!db_->replaceCitingPapers(sourceArxivId_, data.papers, data.totalCount, &dbError)) {
            status_->setText(dbError);
            QMessageBox::warning(this, QStringLiteral("HEPShelf"), dbError);
            return;
        }
        metricsChanged_ = true;
        reload();
        status_->setText(QStringLiteral("Cited-by cache refreshed from INSPIRE."));
    });
}

QList<int> CitedByDialog::selectedIndexes() const
{
    QList<int> result;
    if (!table_ || !table_->selectionModel())
        return result;
    for (const QModelIndex &row : table_->selectionModel()->selectedRows()) {
        QTableWidgetItem *item = table_->item(row.row(), 0);
        if (!item)
            continue;
        const int index = item->data(RolePaperIndex).toInt();
        if (!result.contains(index))
            result << index;
    }
    return result;
}

void CitedByDialog::openSelected()
{
    const QList<int> selected = selectedIndexes();
    if (selected.size() != 1)
        return;
    const auto &paper = papers_.at(selected.first());
    if (paper.local && !paper.localPath.isEmpty())
        QDesktopServices::openUrl(QUrl::fromLocalFile(paper.localPath));
    else if (!paper.arxivId.isEmpty())
        openSelectedArxiv();
    else
        openSelectedInspire();
}

void CitedByDialog::openSelectedArxiv()
{
    const QList<int> selected = selectedIndexes();
    if (selected.size() != 1)
        return;
    const QString id = papers_.at(selected.first()).arxivId;
    if (!id.isEmpty())
        QDesktopServices::openUrl(QUrl(QStringLiteral("https://arxiv.org/abs/%1").arg(id)));
}

void CitedByDialog::openSelectedInspire()
{
    const QList<int> selected = selectedIndexes();
    if (selected.size() != 1)
        return;
    const int recid = papers_.at(selected.first()).inspireRecid;
    if (recid > 0)
        QDesktopServices::openUrl(QUrl(QStringLiteral("https://inspirehep.net/literature/%1").arg(recid)));
}

void CitedByDialog::chooseDownloadFolder()
{
    const QString selected = QFileDialog::getExistingDirectory(this,
                                                                QStringLiteral("Download citing papers to folder"),
                                                                downloadFolder());
    if (selected.isEmpty())
        return;
    if (folder_->findText(selected) < 0)
        folder_->addItem(selected);
    folder_->setCurrentText(selected);
}

QString CitedByDialog::downloadFolder() const
{
    return folder_ ? QDir::cleanPath(folder_->currentText().trimmed()) : QString();
}

void CitedByDialog::downloadSelected()
{
    if (downloading_ || !db_)
        return;
    const QString root = downloadFolder();
    if (root.isEmpty()) {
        QMessageBox::information(this, QStringLiteral("Download citing papers"), QStringLiteral("Choose a destination folder first."));
        return;
    }
    if (!QDir().mkpath(root)) {
        QMessageBox::warning(this, QStringLiteral("Download citing papers"), QStringLiteral("Could not create %1").arg(root));
        return;
    }

    pendingDownloads_.clear();
    for (int index : selectedIndexes()) {
        if (index < 0 || index >= papers_.size())
            continue;
        const auto &paper = papers_.at(index);
        if (!paper.local && !paper.arxivId.isEmpty())
            pendingDownloads_ << index;
    }
    if (pendingDownloads_.isEmpty()) {
        status_->setText(QStringLiteral("No selected missing citing papers have an arXiv identifier."));
        return;
    }

    QString error;
    if (!db_->addFolder(root, &error)) {
        QMessageBox::warning(this, QStringLiteral("HEPShelf"), error);
        return;
    }

    downloading_ = true;
    downloadButton_->setEnabled(false);
    refreshButton_->setEnabled(false);
    progress_->setRange(0, pendingDownloads_.size());
    progress_->setValue(0);
    progress_->show();
    status_->setText(QStringLiteral("Downloading %1 citing paper(s)…").arg(pendingDownloads_.size()));
    downloadNext();
}

void CitedByDialog::downloadNext()
{
    if (pendingDownloads_.isEmpty()) {
        downloading_ = false;
        refreshButton_->setEnabled(true);
        downloadButton_->setEnabled(true);
        progress_->hide();
        status_->setText(QStringLiteral("Citing-paper downloads finished."));
        reload();
        return;
    }

    currentDownloadIndex_ = pendingDownloads_.takeFirst();
    if (currentDownloadIndex_ < 0 || currentDownloadIndex_ >= papers_.size()) {
        downloadNext();
        return;
    }
    const auto &paper = papers_.at(currentDownloadIndex_);
    const QString root = downloadFolder();
    currentTargetPath_ = QDir(root).filePath(QStringLiteral("%1.pdf").arg(safeFilenameId(paper.arxivId)));

    if (QFileInfo::exists(currentTargetPath_)) {
        QFileInfo info(currentTargetPath_);
        QString error;
        if (!db_->upsertFile(info.absoluteFilePath(), paper.arxivId, info.size(),
                             info.lastModified().toSecsSinceEpoch(), &error)) {
            finishCurrentDownload(false, error);
            return;
        }
        libraryChanged_ = true;
        finishCurrentDownload(true, QStringLiteral("Already present on disk: %1").arg(info.fileName()));
        return;
    }

    saveFile_ = new QSaveFile(currentTargetPath_);
    if (!saveFile_->open(QIODevice::WriteOnly)) {
        finishCurrentDownload(false, QStringLiteral("Could not create %1").arg(currentTargetPath_));
        return;
    }
    currentPrefix_.clear();

    QNetworkRequest request(QUrl(QStringLiteral("https://arxiv.org/pdf/%1").arg(paper.arxivId)));
    request.setHeader(QNetworkRequest::UserAgentHeader,
                      QStringLiteral("HEPShelf/0.9.3 local-literature-library"));
    request.setAttribute(QNetworkRequest::RedirectPolicyAttribute,
                         QNetworkRequest::NoLessSafeRedirectPolicy);
    request.setTransferTimeout(60000);
    downloadReply_ = network_.get(request);

    connect(downloadReply_, &QNetworkReply::readyRead, this, [this]() {
        if (!downloadReply_ || !saveFile_)
            return;
        const QByteArray chunk = downloadReply_->readAll();
        if (currentPrefix_.size() < 8)
            currentPrefix_.append(chunk.left(8 - currentPrefix_.size()));
        saveFile_->write(chunk);
    });
    connect(downloadReply_, &QNetworkReply::finished, this, [this]() {
        if (!downloadReply_)
            return;
        if (downloadReply_->bytesAvailable() > 0) {
            const QByteArray chunk = downloadReply_->readAll();
            if (currentPrefix_.size() < 8)
                currentPrefix_.append(chunk.left(8 - currentPrefix_.size()));
            if (saveFile_)
                saveFile_->write(chunk);
        }
        const int http = downloadReply_->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
        const QString networkError = downloadReply_->errorString();
        const bool networkOk = downloadReply_->error() == QNetworkReply::NoError && http >= 200 && http < 300;
        downloadReply_->deleteLater();
        downloadReply_ = nullptr;

        if (!networkOk) {
            finishCurrentDownload(false, QStringLiteral("Download failed (HTTP %1): %2").arg(http).arg(networkError));
            return;
        }
        if (!currentPrefix_.startsWith("%PDF-")) {
            finishCurrentDownload(false, QStringLiteral("arXiv did not return a PDF for this paper."));
            return;
        }
        if (!saveFile_ || !saveFile_->commit()) {
            finishCurrentDownload(false, QStringLiteral("Could not finish writing the PDF."));
            return;
        }
        delete saveFile_;
        saveFile_ = nullptr;

        const auto paper = papers_.at(currentDownloadIndex_);
        QFileInfo info(currentTargetPath_);
        QString error;
        if (!db_->upsertFile(info.absoluteFilePath(), paper.arxivId, info.size(),
                             info.lastModified().toSecsSinceEpoch(), &error)) {
            finishCurrentDownload(false, error);
            return;
        }
        libraryChanged_ = true;
        finishCurrentDownload(true, QStringLiteral("Downloaded %1").arg(info.fileName()));
    });
}

void CitedByDialog::finishCurrentDownload(bool success, const QString &message)
{
    if (saveFile_) {
        if (!success)
            saveFile_->cancelWriting();
        delete saveFile_;
        saveFile_ = nullptr;
    }
    if (!message.isEmpty())
        status_->setText(message);
    const int total = progress_->maximum();
    progress_->setValue(qMin(total, total - pendingDownloads_.size()));
    currentDownloadIndex_ = -1;
    QTimer::singleShot(350, this, [this]() { downloadNext(); });
}

QString CitedByDialog::safeFilenameId(QString arxivId)
{
    arxivId.replace(QLatin1Char('/'), QLatin1Char('_'));
    arxivId.replace(QRegularExpression(QStringLiteral("[^A-Za-z0-9._-]+")), QStringLiteral("_"));
    return arxivId;
}
