#include "reference_dialog.h"

#include <QAbstractItemView>
#include <QComboBox>
#include <QDesktopServices>
#include <QDateTime>
#include <QDialogButtonBox>
#include <QDir>
#include <QFile>
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
#include <QTextStream>
#include <QTimer>
#include <QUrl>
#include <QVBoxLayout>

namespace {
constexpr int RoleReferenceIndex = Qt::UserRole;

QString compact(QString text, int max = 110)
{
    text = text.simplified();
    if (text.size() <= max)
        return text;
    return text.left(max - 1) + QChar(0x2026);
}

QString referenceLabel(const ReferenceRecord &ref)
{
    if (!ref.title.trimmed().isEmpty())
        return ref.title.trimmed();
    if (!ref.rawText.trimmed().isEmpty())
        return ref.rawText.trimmed();
    if (!ref.arxivId.isEmpty())
        return QStringLiteral("arXiv:%1").arg(ref.arxivId);
    return QStringLiteral("Unresolved reference");
}
}

ReferenceDialog::ReferenceDialog(Database *database,
                                 const QString &sourceArxivId,
                                 QWidget *parent)
    : QDialog(parent), db_(database), sourceArxivId_(sourceArxivId), inspire_(this)
{
    setWindowTitle(QStringLiteral("References — %1").arg(sourceArxivId_));
    resize(1240, 760);
    setMinimumSize(900, 560);
    buildUi();
    reload();

    if (references_.isEmpty())
        refreshFromInspire();
}

ReferenceDialog::~ReferenceDialog()
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
    settings.setValue(QStringLiteral("references/geometry"), saveGeometry());
    settings.setValue(QStringLiteral("references/downloadFolder"), downloadFolder());
}

void ReferenceDialog::buildUi()
{
    auto *root = new QVBoxLayout(this);
    root->setContentsMargins(14, 14, 14, 14);
    root->setSpacing(9);

    auto *intro = new QLabel(
        QStringLiteral("HEPShelf resolves the paper's structured INSPIRE bibliography against your local library. "
                       "Local papers open immediately; missing references with an arXiv identifier can be downloaded without duplicating papers you already own."),
        this);
    intro->setWordWrap(true);
    root->addWidget(intro);

    auto *top = new QHBoxLayout;
    summary_ = new QLabel(this);
    summary_->setTextInteractionFlags(Qt::TextSelectableByMouse);
    refreshButton_ = new QPushButton(QStringLiteral("Refresh from INSPIRE"), this);
    exportButton_ = new QPushButton(QStringLiteral("Export references .bib"), this);
    top->addWidget(summary_, 1);
    top->addWidget(exportButton_);
    top->addWidget(refreshButton_);
    root->addLayout(top);

    table_ = new QTableWidget(this);
    table_->setColumnCount(7);
    table_->setHorizontalHeaderLabels({QStringLiteral("#"), QStringLiteral("Title / reference"),
                                       QStringLiteral("Authors"), QStringLiteral("arXiv"),
                                       QStringLiteral("DOI"), QStringLiteral("INSPIRE"),
                                       QStringLiteral("Status")});
    table_->setSelectionBehavior(QAbstractItemView::SelectRows);
    table_->setSelectionMode(QAbstractItemView::ExtendedSelection);
    table_->setEditTriggers(QAbstractItemView::NoEditTriggers);
    table_->setAlternatingRowColors(true);
    table_->setShowGrid(false);
    table_->verticalHeader()->setVisible(false);
    table_->horizontalHeader()->setSectionResizeMode(0, QHeaderView::ResizeToContents);
    table_->horizontalHeader()->setSectionResizeMode(1, QHeaderView::Stretch);
    table_->horizontalHeader()->setSectionResizeMode(2, QHeaderView::Interactive);
    table_->horizontalHeader()->setSectionResizeMode(3, QHeaderView::ResizeToContents);
    table_->horizontalHeader()->setSectionResizeMode(4, QHeaderView::ResizeToContents);
    table_->horizontalHeader()->setSectionResizeMode(5, QHeaderView::ResizeToContents);
    table_->horizontalHeader()->setSectionResizeMode(6, QHeaderView::ResizeToContents);
    table_->setColumnWidth(2, 250);
    root->addWidget(table_, 1);

    auto *actions = new QHBoxLayout;
    openButton_ = new QPushButton(QStringLiteral("Open local"), this);
    arxivButton_ = new QPushButton(QStringLiteral("Open arXiv"), this);
    actions->addWidget(openButton_);
    actions->addWidget(arxivButton_);
    actions->addStretch();
    actions->addWidget(new QLabel(QStringLiteral("Download missing to"), this));
    folder_ = new QComboBox(this);
    folder_->setEditable(true);
    folder_->setMinimumWidth(350);
    if (db_)
        folder_->addItems(db_->folders(nullptr));
    QSettings settings;
    const QString remembered = settings.value(QStringLiteral("references/downloadFolder")).toString();
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
    connect(exportButton_, &QPushButton::clicked, this, [this]() { exportReferencesBibTeX(); });
    connect(openButton_, &QPushButton::clicked, this, [this]() { openSelected(); });
    connect(arxivButton_, &QPushButton::clicked, this, [this]() { openSelectedArxiv(); });
    connect(browse, &QPushButton::clicked, this, [this]() { chooseDownloadFolder(); });
    connect(selectMissing, &QPushButton::clicked, this, [this]() {
        table_->clearSelection();
        for (int row = 0; row < table_->rowCount(); ++row) {
            QTableWidgetItem *item = table_->item(row, 0);
            if (!item)
                continue;
            const int index = item->data(RoleReferenceIndex).toInt();
            if (index >= 0 && index < references_.size()
                && !references_[index].local && !references_[index].arxivId.isEmpty()) {
                table_->selectionModel()->select(table_->model()->index(row, 0),
                                                  QItemSelectionModel::Select | QItemSelectionModel::Rows);
            }
        }
    });
    connect(downloadButton_, &QPushButton::clicked, this, [this]() { downloadSelected(); });
    connect(table_, &QTableWidget::cellDoubleClicked, this, [this](int, int) { openSelected(); });
    connect(table_, &QTableWidget::itemSelectionChanged, this, [this]() {
        const QList<int> selected = selectedReferenceIndexes();
        bool anyLocal = false;
        bool anyArxiv = false;
        bool anyMissingArxiv = false;
        for (int index : selected) {
            if (index < 0 || index >= references_.size())
                continue;
            anyLocal = anyLocal || references_[index].local;
            anyArxiv = anyArxiv || !references_[index].arxivId.isEmpty();
            anyMissingArxiv = anyMissingArxiv || (!references_[index].local && !references_[index].arxivId.isEmpty());
        }
        openButton_->setEnabled(selected.size() == 1 && anyLocal);
        arxivButton_->setEnabled(selected.size() == 1 && anyArxiv);
        downloadButton_->setEnabled(anyMissingArxiv && !downloading_);
    });

    const QByteArray geometry = settings.value(QStringLiteral("references/geometry")).toByteArray();
    if (!geometry.isEmpty())
        restoreGeometry(geometry);
}

void ReferenceDialog::reload()
{
    QString error;
    references_ = db_ ? db_->referencesForPaper(sourceArxivId_, &error) : QList<ReferenceRecord>();
    if (!error.isEmpty()) {
        status_->setText(error);
        return;
    }

    table_->setSortingEnabled(false);
    table_->setRowCount(references_.size());
    for (int row = 0; row < references_.size(); ++row) {
        const ReferenceRecord &ref = references_[row];
        auto *number = new QTableWidgetItem(QString::number(ref.position));
        number->setData(RoleReferenceIndex, row);
        table_->setItem(row, 0, number);
        table_->setItem(row, 1, new QTableWidgetItem(compact(referenceLabel(ref), 160)));
        table_->setItem(row, 2, new QTableWidgetItem(compact(ref.authors)));
        table_->setItem(row, 3, new QTableWidgetItem(ref.arxivId));
        table_->setItem(row, 4, new QTableWidgetItem(ref.doi));
        table_->setItem(row, 5, new QTableWidgetItem(ref.inspireRecid > 0 ? QString::number(ref.inspireRecid) : QString()));

        QString state;
        if (ref.local)
            state = QStringLiteral("Local");
        else if (!ref.arxivId.isEmpty())
            state = QStringLiteral("Available on arXiv");
        else if (ref.inspireRecid > 0)
            state = QStringLiteral("INSPIRE only / unresolved arXiv");
        else
            state = QStringLiteral("Unresolved");
        auto *statusItem = new QTableWidgetItem(state);
        if (ref.local)
            statusItem->setToolTip(ref.localPath);
        table_->setItem(row, 6, statusItem);
    }
    table_->setSortingEnabled(true);
    if (table_->rowCount() > 0)
        table_->selectRow(0);
    updateSummary();
}

void ReferenceDialog::updateSummary()
{
    int local = 0;
    int downloadable = 0;
    int unresolved = 0;
    for (const ReferenceRecord &ref : references_) {
        if (ref.local)
            ++local;
        else if (!ref.arxivId.isEmpty())
            ++downloadable;
        else
            ++unresolved;
    }
    summary_->setText(QStringLiteral("%1 references  ·  %2 local  ·  %3 downloadable from arXiv  ·  %4 unresolved")
                          .arg(references_.size()).arg(local).arg(downloadable).arg(unresolved));
    exportButton_->setEnabled(!references_.isEmpty());
}

void ReferenceDialog::refreshFromInspire()
{
    if (refreshing_ || sourceArxivId_.isEmpty())
        return;
    refreshing_ = true;
    refreshButton_->setEnabled(false);
    status_->setText(QStringLiteral("Fetching references and citation metrics from INSPIRE…"));

    inspire_.fetchPaper(sourceArxivId_, [this](const InspirePaperData &data, const QString &error) {
        refreshing_ = false;
        refreshButton_->setEnabled(true);
        if (!error.isEmpty()) {
            status_->setText(error);
            QMessageBox::warning(this, QStringLiteral("INSPIRE"), error);
            return;
        }
        QString dbError;
        if (!db_->updateCitationMetrics(sourceArxivId_, data.metrics, &dbError)
            || !db_->replaceReferences(sourceArxivId_, data.references, &dbError)) {
            status_->setText(dbError);
            QMessageBox::warning(this, QStringLiteral("HEPShelf"), dbError);
            return;
        }
        metricsChanged_ = true;
        status_->setText(QStringLiteral("INSPIRE data refreshed."));
        reload();
    });
}

QList<int> ReferenceDialog::selectedReferenceIndexes() const
{
    QList<int> indexes;
    const auto rows = table_->selectionModel()->selectedRows();
    for (const QModelIndex &rowIndex : rows) {
        QTableWidgetItem *item = table_->item(rowIndex.row(), 0);
        if (!item)
            continue;
        const int index = item->data(RoleReferenceIndex).toInt();
        if (!indexes.contains(index))
            indexes << index;
    }
    return indexes;
}

void ReferenceDialog::openSelected()
{
    const QList<int> selected = selectedReferenceIndexes();
    if (selected.size() != 1)
        return;
    const ReferenceRecord &ref = references_.at(selected.first());
    if (!ref.local || ref.localPath.isEmpty())
        return;
    if (!QDesktopServices::openUrl(QUrl::fromLocalFile(ref.localPath)))
        QMessageBox::warning(this, QStringLiteral("HEPShelf"), QStringLiteral("Could not open %1").arg(ref.localPath));
}

void ReferenceDialog::openSelectedArxiv()
{
    const QList<int> selected = selectedReferenceIndexes();
    if (selected.size() != 1)
        return;
    const QString id = references_.at(selected.first()).arxivId;
    if (!id.isEmpty())
        QDesktopServices::openUrl(QUrl(QStringLiteral("https://arxiv.org/abs/%1").arg(id)));
}

void ReferenceDialog::chooseDownloadFolder()
{
    const QString selected = QFileDialog::getExistingDirectory(this,
                                                                QStringLiteral("Download references to folder"),
                                                                downloadFolder());
    if (selected.isEmpty())
        return;
    if (folder_->findText(selected) < 0)
        folder_->addItem(selected);
    folder_->setCurrentText(selected);
}

QString ReferenceDialog::downloadFolder() const
{
    return folder_ ? QDir::cleanPath(folder_->currentText().trimmed()) : QString();
}

void ReferenceDialog::downloadSelected()
{
    if (downloading_)
        return;
    const QString root = downloadFolder();
    if (root.isEmpty()) {
        QMessageBox::information(this, QStringLiteral("Download references"), QStringLiteral("Choose a destination folder first."));
        return;
    }
    if (!QDir().mkpath(root)) {
        QMessageBox::warning(this, QStringLiteral("Download references"), QStringLiteral("Could not create %1").arg(root));
        return;
    }

    pendingDownloads_.clear();
    for (int index : selectedReferenceIndexes()) {
        if (index < 0 || index >= references_.size())
            continue;
        const ReferenceRecord &ref = references_.at(index);
        if (!ref.local && !ref.arxivId.isEmpty())
            pendingDownloads_ << index;
    }
    if (pendingDownloads_.isEmpty()) {
        status_->setText(QStringLiteral("No selected missing references have an arXiv identifier."));
        return;
    }

    QString error;
    if (db_ && !db_->addFolder(root, &error)) {
        QMessageBox::warning(this, QStringLiteral("HEPShelf"), error);
        return;
    }

    downloading_ = true;
    downloadButton_->setEnabled(false);
    refreshButton_->setEnabled(false);
    progress_->setRange(0, pendingDownloads_.size());
    progress_->setValue(0);
    progress_->show();
    status_->setText(QStringLiteral("Downloading %1 missing reference(s)…").arg(pendingDownloads_.size()));
    downloadNext();
}

void ReferenceDialog::downloadNext()
{
    if (pendingDownloads_.isEmpty()) {
        downloading_ = false;
        refreshButton_->setEnabled(true);
        progress_->hide();
        status_->setText(QStringLiteral("Reference downloads finished."));
        reload();
        return;
    }

    currentDownloadRef_ = pendingDownloads_.takeFirst();
    if (currentDownloadRef_ < 0 || currentDownloadRef_ >= references_.size()) {
        downloadNext();
        return;
    }
    const ReferenceRecord &ref = references_.at(currentDownloadRef_);
    const QString root = downloadFolder();
    currentTargetPath_ = QDir(root).filePath(QStringLiteral("%1.pdf").arg(safeFilenameId(ref.arxivId)));

    if (QFileInfo::exists(currentTargetPath_)) {
        QFileInfo info(currentTargetPath_);
        QString error;
        if (!db_->upsertFile(info.absoluteFilePath(), ref.arxivId, info.size(),
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

    QNetworkRequest request(QUrl(QStringLiteral("https://arxiv.org/pdf/%1").arg(ref.arxivId)));
    request.setHeader(QNetworkRequest::UserAgentHeader,
                      QStringLiteral("HEPShelf/0.9.0 local-literature-library"));
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
            finishCurrentDownload(false, QStringLiteral("arXiv did not return a PDF for this reference."));
            return;
        }
        if (!saveFile_ || !saveFile_->commit()) {
            finishCurrentDownload(false, QStringLiteral("Could not finish writing the PDF."));
            return;
        }
        delete saveFile_;
        saveFile_ = nullptr;

        const ReferenceRecord ref = references_.at(currentDownloadRef_);
        QString error;
        QFileInfo info(currentTargetPath_);
        if (!db_->upsertFile(info.absoluteFilePath(), ref.arxivId, info.size(), info.lastModified().toSecsSinceEpoch(), &error)) {
            finishCurrentDownload(false, error);
            return;
        }
        libraryChanged_ = true;
        finishCurrentDownload(true, QStringLiteral("Downloaded %1").arg(info.fileName()));
    });
}

void ReferenceDialog::finishCurrentDownload(bool success, const QString &message)
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
    currentDownloadRef_ = -1;
    QTimer::singleShot(350, this, [this]() { downloadNext(); });
}

QString ReferenceDialog::safeFilenameId(QString arxivId)
{
    arxivId.replace(QLatin1Char('/'), QLatin1Char('_'));
    arxivId.replace(QRegularExpression(QStringLiteral("[^A-Za-z0-9._-]+")), QStringLiteral("_"));
    return arxivId;
}

QString ReferenceDialog::bibEscape(QString text)
{
    text.replace(QLatin1Char('{'), QStringLiteral("\\{"));
    text.replace(QLatin1Char('}'), QStringLiteral("\\}"));
    return text;
}

void ReferenceDialog::exportReferencesBibTeX()
{
    if (references_.isEmpty())
        return;
    const QString path = QFileDialog::getSaveFileName(this,
                                                       QStringLiteral("Export references as BibTeX"),
                                                       QStringLiteral("%1-references.bib").arg(safeFilenameId(sourceArxivId_)),
                                                       QStringLiteral("BibTeX files (*.bib)"));
    if (path.isEmpty())
        return;

    QFile file(path);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate | QIODevice::Text)) {
        QMessageBox::warning(this, QStringLiteral("HEPShelf"), QStringLiteral("Could not write %1").arg(path));
        return;
    }
    QTextStream out(&file);
    for (const ReferenceRecord &ref : references_) {
        if (ref.title.isEmpty() && ref.arxivId.isEmpty() && ref.doi.isEmpty())
            continue;
        QString key;
        if (!ref.arxivId.isEmpty())
            key = QStringLiteral("arxiv_%1").arg(safeFilenameId(ref.arxivId));
        else if (ref.inspireRecid > 0)
            key = QStringLiteral("inspire_%1").arg(ref.inspireRecid);
        else
            key = QStringLiteral("ref_%1_%2").arg(safeFilenameId(sourceArxivId_)).arg(ref.position);
        out << "@article{" << key << ",\n";
        if (!ref.title.isEmpty())
            out << "  title = {{" << bibEscape(ref.title) << "}},\n";
        if (!ref.authors.isEmpty()) {
            QString authors = ref.authors;
            authors.replace(QStringLiteral(", "), QStringLiteral(" and "));
            out << "  author = {" << bibEscape(authors) << "},\n";
        }
        if (!ref.arxivId.isEmpty()) {
            out << "  eprint = {" << ref.arxivId << "},\n";
            out << "  archivePrefix = {arXiv},\n";
        }
        if (!ref.doi.isEmpty())
            out << "  doi = {" << ref.doi << "},\n";
        out << "}\n\n";
    }
    file.close();
    status_->setText(QStringLiteral("Exported references to %1").arg(path));
}
