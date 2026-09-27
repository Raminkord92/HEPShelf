#include "literature_trails_dialog.h"

#include <utility>

#include <QAbstractItemView>
#include <QDesktopServices>
#include <QDialogButtonBox>
#include <QFile>
#include <QFileDialog>
#include <QFont>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QInputDialog>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QMessageBox>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QRegularExpression>
#include <QTableWidget>
#include <QTextStream>
#include <QUrl>
#include <QVBoxLayout>

namespace {
constexpr int RoleTrailId = Qt::UserRole;
constexpr int RoleItemId = Qt::UserRole + 1;

QString safeBibKey(QString value)
{
    value.replace(QRegularExpression(QStringLiteral("[^A-Za-z0-9]+")), QStringLiteral("_"));
    value = value.trimmed();
    return value.isEmpty() ? QStringLiteral("hepshelf_paper") : value;
}

QString safeFileName(QString value)
{
    value = value.simplified();
    value.replace(QRegularExpression(QStringLiteral("[\\\\/:*?\"<>|]+")), QStringLiteral("_"));
    value.replace(QRegularExpression(QStringLiteral("\\s+")), QStringLiteral("_"));
    return value.isEmpty() ? QStringLiteral("literature_trail") : value.left(100);
}

QString bibEscape(QString value)
{
    value.replace(QLatin1Char('{'), QStringLiteral("\\{"));
    value.replace(QLatin1Char('}'), QStringLiteral("\\}"));
    return value;
}

QString bibAuthors(QString authors)
{
    // arXiv metadata is stored as a display-name list separated by comma+space.
    // This is a deterministic offline fallback; INSPIRE BibTeX remains available elsewhere.
    return authors.split(QStringLiteral(", "), Qt::SkipEmptyParts).join(QStringLiteral(" and "));
}

QString itemDisplayTitle(const TrailItemRecord &item)
{
    if (!item.title.trimmed().isEmpty())
        return item.title.simplified();
    if (!item.arxivId.isEmpty())
        return QStringLiteral("arXiv:%1").arg(item.arxivId);
    if (item.inspireRecid > 0)
        return QStringLiteral("INSPIRE %1").arg(item.inspireRecid);
    return QStringLiteral("Untitled paper");
}
}

LiteratureTrailsDialog::LiteratureTrailsDialog(Database *database,
                                               OpenLocalCallback openLocal,
                                               QWidget *parent)
    : QDialog(parent), db_(database), openLocal_(std::move(openLocal))
{
    setWindowTitle(QStringLiteral("Literature trails"));
    resize(1220, 760);
    setMinimumSize(900, 560);
    buildUi();
    refreshTrails();
}

void LiteratureTrailsDialog::buildUi()
{
    auto *root = new QVBoxLayout(this);
    root->setContentsMargins(12, 12, 12, 12);
    root->setSpacing(8);

    auto *intro = new QLabel(
        QStringLiteral("Save ordered reading paths through the literature. Trails may contain local papers and remote arXiv/INSPIRE records; item notes are specific to the trail."),
        this);
    intro->setWordWrap(true);
    root->addWidget(intro);

    auto *body = new QHBoxLayout;
    body->setSpacing(10);

    auto *left = new QVBoxLayout;
    auto *leftHeading = new QLabel(QStringLiteral("Saved trails"), this);
    QFont headingFont = leftHeading->font();
    headingFont.setBold(true);
    leftHeading->setFont(headingFont);
    left->addWidget(leftHeading);

    trailList_ = new QListWidget(this);
    trailList_->setMinimumWidth(245);
    trailList_->setMaximumWidth(340);
    left->addWidget(trailList_, 1);

    auto *trailButtons = new QHBoxLayout;
    auto *newTrail = new QPushButton(QStringLiteral("New"), this);
    auto *renameTrail = new QPushButton(QStringLiteral("Rename"), this);
    auto *deleteTrail = new QPushButton(QStringLiteral("Delete"), this);
    trailButtons->addWidget(newTrail);
    trailButtons->addWidget(renameTrail);
    trailButtons->addWidget(deleteTrail);
    left->addLayout(trailButtons);
    body->addLayout(left);

    auto *right = new QVBoxLayout;
    trailSummary_ = new QLabel(QStringLiteral("Select a trail"), this);
    trailSummary_->setWordWrap(true);
    trailSummary_->setTextInteractionFlags(Qt::TextSelectableByMouse);
    QFont summaryFont = trailSummary_->font();
    summaryFont.setBold(true);
    trailSummary_->setFont(summaryFont);
    right->addWidget(trailSummary_);

    auto *descriptionLabel = new QLabel(QStringLiteral("Trail description / purpose"), this);
    right->addWidget(descriptionLabel);
    description_ = new QPlainTextEdit(this);
    description_->setPlaceholderText(QStringLiteral("Why this reading path matters, what question it answers, or what you want to remember…"));
    description_->setMaximumHeight(100);
    right->addWidget(description_);
    saveTrailButton_ = new QPushButton(QStringLiteral("Save trail description"), this);
    right->addWidget(saveTrailButton_, 0, Qt::AlignRight);

    itemsTable_ = new QTableWidget(this);
    itemsTable_->setColumnCount(6);
    itemsTable_->setHorizontalHeaderLabels({QStringLiteral("#"), QStringLiteral("Title"),
                                           QStringLiteral("Authors"), QStringLiteral("Year"),
                                           QStringLiteral("arXiv"), QStringLiteral("Status")});
    itemsTable_->setSelectionBehavior(QAbstractItemView::SelectRows);
    itemsTable_->setSelectionMode(QAbstractItemView::SingleSelection);
    itemsTable_->setEditTriggers(QAbstractItemView::NoEditTriggers);
    itemsTable_->verticalHeader()->setVisible(false);
    itemsTable_->horizontalHeader()->setStretchLastSection(false);
    itemsTable_->horizontalHeader()->setSectionResizeMode(0, QHeaderView::ResizeToContents);
    itemsTable_->horizontalHeader()->setSectionResizeMode(1, QHeaderView::Stretch);
    itemsTable_->horizontalHeader()->setSectionResizeMode(2, QHeaderView::ResizeToContents);
    itemsTable_->horizontalHeader()->setSectionResizeMode(3, QHeaderView::ResizeToContents);
    itemsTable_->horizontalHeader()->setSectionResizeMode(4, QHeaderView::ResizeToContents);
    itemsTable_->horizontalHeader()->setSectionResizeMode(5, QHeaderView::ResizeToContents);
    right->addWidget(itemsTable_, 1);

    auto *rowButtons = new QHBoxLayout;
    upButton_ = new QPushButton(QStringLiteral("Move up"), this);
    downButton_ = new QPushButton(QStringLiteral("Move down"), this);
    removeItemButton_ = new QPushButton(QStringLiteral("Remove"), this);
    openItemButton_ = new QPushButton(QStringLiteral("Open paper"), this);
    exportBibButton_ = new QPushButton(QStringLiteral("Export BibTeX…"), this);
    exportReadingButton_ = new QPushButton(QStringLiteral("Export reading list…"), this);
    rowButtons->addWidget(upButton_);
    rowButtons->addWidget(downButton_);
    rowButtons->addWidget(removeItemButton_);
    rowButtons->addWidget(openItemButton_);
    rowButtons->addStretch();
    rowButtons->addWidget(exportBibButton_);
    rowButtons->addWidget(exportReadingButton_);
    right->addLayout(rowButtons);

    auto *noteLabel = new QLabel(QStringLiteral("Note for selected trail item"), this);
    right->addWidget(noteLabel);
    itemNote_ = new QPlainTextEdit(this);
    itemNote_->setPlaceholderText(QStringLiteral("What this paper contributes to this particular trail…"));
    itemNote_->setMaximumHeight(110);
    right->addWidget(itemNote_);

    auto *noteBottom = new QHBoxLayout;
    itemStatus_ = new QLabel(this);
    itemStatus_->setWordWrap(true);
    noteBottom->addWidget(itemStatus_, 1);
    saveItemNoteButton_ = new QPushButton(QStringLiteral("Save item note"), this);
    noteBottom->addWidget(saveItemNoteButton_);
    right->addLayout(noteBottom);

    body->addLayout(right, 1);
    root->addLayout(body, 1);

    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Close, this);
    root->addWidget(buttons);

    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::accept);
    connect(newTrail, &QPushButton::clicked, this, [this]() { createTrail(); });
    connect(renameTrail, &QPushButton::clicked, this, [this]() { this->renameTrail(); });
    connect(deleteTrail, &QPushButton::clicked, this, [this]() { this->deleteTrail(); });
    connect(saveTrailButton_, &QPushButton::clicked, this, [this]() { saveTrailDetails(); });
    connect(trailList_, &QListWidget::currentRowChanged, this, [this](int) { loadSelectedTrail(); });
    connect(itemsTable_, &QTableWidget::itemSelectionChanged, this, [this]() {
        loadingItemNote_ = true;
        const TrailItemRecord item = selectedItem();
        itemNote_->setPlainText(item.id > 0 ? item.note : QString());
        itemStatus_->setText(item.id > 0
                                 ? (item.local ? QStringLiteral("Local PDF available")
                                               : QStringLiteral("Remote record — local PDF not currently indexed"))
                                 : QString());
        loadingItemNote_ = false;
    });
    connect(itemsTable_, &QTableWidget::cellDoubleClicked, this, [this](int, int) { openSelectedItem(); });
    connect(upButton_, &QPushButton::clicked, this, [this]() { moveSelectedItem(-1); });
    connect(downButton_, &QPushButton::clicked, this, [this]() { moveSelectedItem(1); });
    connect(removeItemButton_, &QPushButton::clicked, this, [this]() { removeSelectedItem(); });
    connect(openItemButton_, &QPushButton::clicked, this, [this]() { openSelectedItem(); });
    connect(saveItemNoteButton_, &QPushButton::clicked, this, [this]() { saveSelectedItemNote(); });
    connect(exportBibButton_, &QPushButton::clicked, this, [this]() { exportBibTeX(); });
    connect(exportReadingButton_, &QPushButton::clicked, this, [this]() { exportReadingList(); });
}

void LiteratureTrailsDialog::refreshTrails(int selectId)
{
    if (!db_)
        return;
    if (selectId <= 0)
        selectId = selectedTrailId();

    QString error;
    const QList<LiteratureTrailRecord> trails = db_->literatureTrails(&error);
    if (!error.isEmpty()) {
        QMessageBox::warning(this, QStringLiteral("Literature trails"), error);
        return;
    }

    trailList_->blockSignals(true);
    trailList_->clear();
    int rowToSelect = -1;
    for (int i = 0; i < trails.size(); ++i) {
        const LiteratureTrailRecord &trail = trails.at(i);
        auto *item = new QListWidgetItem(QStringLiteral("%1  (%2)").arg(trail.name).arg(trail.itemCount), trailList_);
        item->setData(RoleTrailId, trail.id);
        item->setToolTip(trail.description);
        if (trail.id == selectId)
            rowToSelect = i;
    }
    trailList_->blockSignals(false);

    if (rowToSelect < 0 && trailList_->count() > 0)
        rowToSelect = 0;
    if (rowToSelect >= 0)
        trailList_->setCurrentRow(rowToSelect);
    else
        loadSelectedTrail();
}

void LiteratureTrailsDialog::selectTrail(int trailId)
{
    for (int i = 0; i < trailList_->count(); ++i) {
        if (trailList_->item(i)->data(RoleTrailId).toInt() == trailId) {
            trailList_->setCurrentRow(i);
            return;
        }
    }
}

int LiteratureTrailsDialog::selectedTrailId() const
{
    const QListWidgetItem *item = trailList_ ? trailList_->currentItem() : nullptr;
    return item ? item->data(RoleTrailId).toInt() : 0;
}

int LiteratureTrailsDialog::selectedItemId() const
{
    if (!itemsTable_ || itemsTable_->currentRow() < 0)
        return 0;
    QTableWidgetItem *item = itemsTable_->item(itemsTable_->currentRow(), 0);
    return item ? item->data(RoleItemId).toInt() : 0;
}

TrailItemRecord LiteratureTrailsDialog::selectedItem() const
{
    TrailItemRecord result;
    const int itemId = selectedItemId();
    if (itemId <= 0 || !db_)
        return result;
    const QList<TrailItemRecord> items = db_->trailItems(selectedTrailId(), nullptr);
    for (const TrailItemRecord &item : items) {
        if (item.id == itemId)
            return item;
    }
    return result;
}

void LiteratureTrailsDialog::loadSelectedTrail()
{
    const int trailId = selectedTrailId();
    itemsTable_->setRowCount(0);
    itemNote_->clear();
    itemStatus_->clear();

    if (trailId <= 0 || !db_) {
        trailSummary_->setText(QStringLiteral("No saved trails yet"));
        description_->clear();
        description_->setEnabled(false);
        saveTrailButton_->setEnabled(false);
        exportBibButton_->setEnabled(false);
        exportReadingButton_->setEnabled(false);
        return;
    }

    QString error;
    const QList<LiteratureTrailRecord> trails = db_->literatureTrails(&error);
    LiteratureTrailRecord current;
    for (const LiteratureTrailRecord &trail : trails) {
        if (trail.id == trailId) {
            current = trail;
            break;
        }
    }
    if (!error.isEmpty()) {
        QMessageBox::warning(this, QStringLiteral("Literature trails"), error);
        return;
    }

    trailSummary_->setText(QStringLiteral("%1 — %2 paper%3 · updated %4")
                               .arg(current.name)
                               .arg(current.itemCount)
                               .arg(current.itemCount == 1 ? QString() : QStringLiteral("s"))
                               .arg(current.updatedAt.isEmpty() ? QStringLiteral("now") : current.updatedAt));
    description_->setEnabled(true);
    description_->setPlainText(current.description);
    saveTrailButton_->setEnabled(true);

    const QList<TrailItemRecord> items = db_->trailItems(trailId, &error);
    if (!error.isEmpty()) {
        QMessageBox::warning(this, QStringLiteral("Literature trails"), error);
        return;
    }
    itemsTable_->setRowCount(items.size());
    for (int row = 0; row < items.size(); ++row) {
        const TrailItemRecord &item = items.at(row);
        auto *number = new QTableWidgetItem(QString::number(row + 1));
        number->setData(RoleItemId, item.id);
        itemsTable_->setItem(row, 0, number);
        itemsTable_->setItem(row, 1, new QTableWidgetItem(itemDisplayTitle(item)));
        itemsTable_->setItem(row, 2, new QTableWidgetItem(item.authors));
        itemsTable_->setItem(row, 3, new QTableWidgetItem(item.year));
        itemsTable_->setItem(row, 4, new QTableWidgetItem(item.arxivId));
        auto *status = new QTableWidgetItem(item.local ? QStringLiteral("Local")
                                                       : (!item.arxivId.isEmpty() ? QStringLiteral("arXiv")
                                                                                 : QStringLiteral("Metadata only")));
        if (!item.note.trimmed().isEmpty())
            status->setToolTip(QStringLiteral("Trail note saved"));
        itemsTable_->setItem(row, 5, status);
    }
    if (itemsTable_->rowCount() > 0)
        itemsTable_->selectRow(0);

    const bool hasItems = !items.isEmpty();
    exportBibButton_->setEnabled(hasItems);
    exportReadingButton_->setEnabled(hasItems);
}

void LiteratureTrailsDialog::createTrail()
{
    bool ok = false;
    const QString name = QInputDialog::getText(this, QStringLiteral("New literature trail"),
                                               QStringLiteral("Trail name"), QLineEdit::Normal,
                                               QString(), &ok).trimmed();
    if (!ok || name.isEmpty())
        return;
    QString error;
    const int id = db_->createLiteratureTrail(name, {}, &error);
    if (id <= 0) {
        QMessageBox::warning(this, QStringLiteral("Literature trails"), error);
        return;
    }
    dataChanged_ = true;
    refreshTrails(id);
}

void LiteratureTrailsDialog::renameTrail()
{
    const int trailId = selectedTrailId();
    if (trailId <= 0)
        return;
    QString error;
    const QList<LiteratureTrailRecord> trails = db_->literatureTrails(&error);
    LiteratureTrailRecord current;
    for (const auto &trail : trails) {
        if (trail.id == trailId) {
            current = trail;
            break;
        }
    }
    bool ok = false;
    const QString name = QInputDialog::getText(this, QStringLiteral("Rename trail"),
                                               QStringLiteral("Trail name"), QLineEdit::Normal,
                                               current.name, &ok).trimmed();
    if (!ok || name.isEmpty())
        return;
    if (!db_->updateLiteratureTrail(trailId, name, description_->toPlainText(), &error)) {
        QMessageBox::warning(this, QStringLiteral("Literature trails"), error);
        return;
    }
    dataChanged_ = true;
    refreshTrails(trailId);
}

void LiteratureTrailsDialog::deleteTrail()
{
    const int trailId = selectedTrailId();
    if (trailId <= 0)
        return;
    if (QMessageBox::question(this, QStringLiteral("Delete literature trail"),
                              QStringLiteral("Delete this trail? The papers and PDFs will not be deleted."))
        != QMessageBox::Yes)
        return;
    QString error;
    if (!db_->deleteLiteratureTrail(trailId, &error)) {
        QMessageBox::warning(this, QStringLiteral("Literature trails"), error);
        return;
    }
    dataChanged_ = true;
    refreshTrails();
}

void LiteratureTrailsDialog::saveTrailDetails()
{
    const int trailId = selectedTrailId();
    if (trailId <= 0)
        return;
    QString error;
    const QList<LiteratureTrailRecord> trails = db_->literatureTrails(&error);
    QString name;
    for (const auto &trail : trails) {
        if (trail.id == trailId) {
            name = trail.name;
            break;
        }
    }
    if (!db_->updateLiteratureTrail(trailId, name, description_->toPlainText(), &error)) {
        QMessageBox::warning(this, QStringLiteral("Literature trails"), error);
        return;
    }
    dataChanged_ = true;
    itemStatus_->setText(QStringLiteral("Trail description saved."));
    refreshTrails(trailId);
}

void LiteratureTrailsDialog::moveSelectedItem(int delta)
{
    const int trailId = selectedTrailId();
    const int itemId = selectedItemId();
    if (trailId <= 0 || itemId <= 0)
        return;
    QString error;
    if (!db_->moveTrailItem(trailId, itemId, delta, &error)) {
        QMessageBox::warning(this, QStringLiteral("Literature trails"), error);
        return;
    }
    dataChanged_ = true;
    loadSelectedTrail();
    for (int row = 0; row < itemsTable_->rowCount(); ++row) {
        if (itemsTable_->item(row, 0)->data(RoleItemId).toInt() == itemId) {
            itemsTable_->selectRow(row);
            break;
        }
    }
}

void LiteratureTrailsDialog::removeSelectedItem()
{
    const int itemId = selectedItemId();
    if (itemId <= 0)
        return;
    QString error;
    if (!db_->removeTrailItem(itemId, &error)) {
        QMessageBox::warning(this, QStringLiteral("Literature trails"), error);
        return;
    }
    dataChanged_ = true;
    refreshTrails(selectedTrailId());
}

void LiteratureTrailsDialog::saveSelectedItemNote()
{
    if (loadingItemNote_)
        return;
    const int itemId = selectedItemId();
    if (itemId <= 0)
        return;
    QString error;
    if (!db_->setTrailItemNote(itemId, itemNote_->toPlainText(), &error)) {
        QMessageBox::warning(this, QStringLiteral("Literature trails"), error);
        return;
    }
    dataChanged_ = true;
    itemStatus_->setText(QStringLiteral("Trail item note saved."));
}

void LiteratureTrailsDialog::openSelectedItem()
{
    const TrailItemRecord item = selectedItem();
    if (item.id <= 0)
        return;
    if (item.local && !item.localPath.isEmpty() && openLocal_) {
        openLocal_(item.localArxivId.isEmpty() ? item.arxivId : item.localArxivId, item.localPath);
        accept();
        return;
    }
    if (!item.arxivId.isEmpty()) {
        QDesktopServices::openUrl(QUrl(QStringLiteral("https://arxiv.org/abs/%1").arg(item.arxivId)));
        return;
    }
    if (item.inspireRecid > 0)
        QDesktopServices::openUrl(QUrl(QStringLiteral("https://inspirehep.net/literature/%1").arg(item.inspireRecid)));
}

void LiteratureTrailsDialog::exportBibTeX()
{
    const int trailId = selectedTrailId();
    if (trailId <= 0)
        return;
    const QList<TrailItemRecord> items = db_->trailItems(trailId, nullptr);
    if (items.isEmpty())
        return;
    const QString path = QFileDialog::getSaveFileName(this, QStringLiteral("Export trail as BibTeX"),
                                                      QStringLiteral("literature_trail.bib"),
                                                      QStringLiteral("BibTeX files (*.bib)"));
    if (path.isEmpty())
        return;
    QFile file(path);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Text)) {
        QMessageBox::warning(this, QStringLiteral("Export BibTeX"), file.errorString());
        return;
    }
    QTextStream out(&file);
    for (const TrailItemRecord &item : items) {
        QString keySource = !item.arxivId.isEmpty() ? item.arxivId
                           : (item.inspireRecid > 0 ? QStringLiteral("inspire_%1").arg(item.inspireRecid)
                                                    : item.title.left(40));
        out << "@article{" << safeBibKey(keySource) << ",\n";
        if (!item.title.isEmpty())
            out << "  title = {" << bibEscape(item.title) << "},\n";
        if (!item.authors.isEmpty())
            out << "  author = {" << bibEscape(bibAuthors(item.authors)) << "},\n";
        if (!item.year.isEmpty())
            out << "  year = {" << item.year << "},\n";
        if (!item.arxivId.isEmpty()) {
            out << "  eprint = {" << item.arxivId << "},\n";
            out << "  archivePrefix = {arXiv},\n";
        }
        if (!item.doi.isEmpty())
            out << "  doi = {" << bibEscape(item.doi) << "},\n";
        out << "}\n\n";
    }
    itemStatus_->setText(QStringLiteral("BibTeX exported to %1").arg(path));
}

void LiteratureTrailsDialog::exportReadingList()
{
    const int trailId = selectedTrailId();
    if (trailId <= 0)
        return;
    QString error;
    LiteratureTrailRecord trail;
    for (const auto &candidate : db_->literatureTrails(&error)) {
        if (candidate.id == trailId) {
            trail = candidate;
            break;
        }
    }
    const QList<TrailItemRecord> items = db_->trailItems(trailId, &error);
    if (!error.isEmpty() || items.isEmpty())
        return;

    const QString path = QFileDialog::getSaveFileName(this, QStringLiteral("Export reading list"),
                                                      QStringLiteral("%1.md").arg(safeFileName(trail.name)),
                                                      QStringLiteral("Markdown files (*.md);;Text files (*.txt)"));
    if (path.isEmpty())
        return;
    QFile file(path);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Text)) {
        QMessageBox::warning(this, QStringLiteral("Export reading list"), file.errorString());
        return;
    }
    QTextStream out(&file);
    out << "# " << trail.name << "\n\n";
    if (!trail.description.trimmed().isEmpty())
        out << trail.description.trimmed() << "\n\n";
    for (int i = 0; i < items.size(); ++i) {
        const TrailItemRecord &item = items.at(i);
        out << (i + 1) << ". **" << itemDisplayTitle(item) << "**";
        if (!item.year.isEmpty())
            out << " (" << item.year << ")";
        out << "\n";
        if (!item.authors.isEmpty())
            out << "   - " << item.authors << "\n";
        if (!item.arxivId.isEmpty())
            out << "   - arXiv:" << item.arxivId << "\n";
        else if (item.inspireRecid > 0)
            out << "   - INSPIRE:" << item.inspireRecid << "\n";
        if (!item.doi.isEmpty())
            out << "   - DOI: " << item.doi << "\n";
        if (!item.note.trimmed().isEmpty())
            out << "   - Note: " << item.note.simplified() << "\n";
        out << "\n";
    }
    itemStatus_->setText(QStringLiteral("Reading list exported to %1").arg(path));
}
