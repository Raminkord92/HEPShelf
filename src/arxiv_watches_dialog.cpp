#include "arxiv_watches_dialog.h"

#include <QAbstractItemView>
#include <QCheckBox>
#include <QComboBox>
#include <QDesktopServices>
#include <QDialogButtonBox>
#include <QDir>
#include <QFileDialog>
#include <QFormLayout>
#include <QHeaderView>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QMessageBox>
#include <QPushButton>
#include <QSettings>
#include <QSplitter>
#include <QStandardPaths>
#include <QTableWidget>
#include <QUrl>
#include <QVBoxLayout>

namespace {
constexpr int RoleId = Qt::UserRole;
constexpr int RoleArxivId = Qt::UserRole + 1;
constexpr int RoleInspireRecid = Qt::UserRole + 2;
}

ArxivWatchesDialog::ArxivWatchesDialog(ArxivWatchManager *manager, Database *database, QWidget *parent)
    : QDialog(parent), manager_(manager), db_(database)
{
    setWindowTitle(QStringLiteral("Research watches"));
    resize(1050, 700);
    setMinimumSize(800, 530);
    auto *outer = new QVBoxLayout(this);
    auto *intro = new QLabel(QStringLiteral("Watch author names or arXiv categories. Checks run daily while HEPShelf is open and catch up on the next launch. "
                                            "A new watch checks submissions from the previous UTC date onward. Automatic PDF downloads are optional for each watch."), this);
    intro->setWordWrap(true);
    outer->addWidget(intro);

    auto *splitter = new QSplitter(Qt::Vertical, this);
    auto *rulesPane = new QWidget(splitter);
    auto *rulesLayout = new QVBoxLayout(rulesPane);
    rulesLayout->setContentsMargins(0, 0, 0, 0);
    rulesLayout->addWidget(new QLabel(QStringLiteral("Watches"), rulesPane));
    rulesTable_ = new QTableWidget(rulesPane);
    rulesTable_->setColumnCount(7);
    rulesTable_->setHorizontalHeaderLabels({QStringLiteral("Type"), QStringLiteral("Author / category / paper"),
                                             QStringLiteral("Action"), QStringLiteral("Download folder"),
                                             QStringLiteral("Last checked"), QStringLiteral("Enabled"),
                                             QStringLiteral("Last issue")});
    rulesTable_->setSelectionBehavior(QAbstractItemView::SelectRows);
    rulesTable_->setSelectionMode(QAbstractItemView::SingleSelection);
    rulesTable_->setEditTriggers(QAbstractItemView::NoEditTriggers);
    rulesTable_->horizontalHeader()->setSectionResizeMode(1, QHeaderView::Stretch);
    rulesTable_->horizontalHeader()->setSectionResizeMode(3, QHeaderView::Stretch);
    rulesLayout->addWidget(rulesTable_, 1);

    auto *ruleButtons = new QHBoxLayout;
    auto *addAuthor = new QPushButton(QStringLiteral("Add author…"), rulesPane);
    auto *addCategory = new QPushButton(QStringLiteral("Add category…"), rulesPane);
    auto *addCitations = new QPushButton(QStringLiteral("Add paper citations…"), rulesPane);
    auto *edit = new QPushButton(QStringLiteral("Edit…"), rulesPane);
    auto *remove = new QPushButton(QStringLiteral("Remove"), rulesPane);
    auto *check = new QPushButton(QStringLiteral("Check now"), rulesPane);
    ruleButtons->addWidget(addAuthor);
    ruleButtons->addWidget(addCategory);
    ruleButtons->addWidget(addCitations);
    ruleButtons->addWidget(edit);
    ruleButtons->addWidget(remove);
    ruleButtons->addStretch();
    ruleButtons->addWidget(check);
    rulesLayout->addLayout(ruleButtons);

    auto *hitsPane = new QWidget(splitter);
    auto *hitsLayout = new QVBoxLayout(hitsPane);
    hitsLayout->setContentsMargins(0, 0, 0, 0);
    hitsLayout->addWidget(new QLabel(QStringLiteral("Recent matches"), hitsPane));
    hitsTable_ = new QTableWidget(hitsPane);
    hitsTable_->setColumnCount(6);
    hitsTable_->setHorizontalHeaderLabels({QStringLiteral("Found"), QStringLiteral("Watch"),
                                            QStringLiteral("Title"), QStringLiteral("arXiv"),
                                            QStringLiteral("Submitted"), QStringLiteral("Status")});
    hitsTable_->setSelectionBehavior(QAbstractItemView::SelectRows);
    hitsTable_->setSelectionMode(QAbstractItemView::SingleSelection);
    hitsTable_->setEditTriggers(QAbstractItemView::NoEditTriggers);
    hitsTable_->horizontalHeader()->setSectionResizeMode(2, QHeaderView::Stretch);
    hitsLayout->addWidget(hitsTable_, 1);
    auto *hitButtons = new QHBoxLayout;
    auto *download = new QPushButton(QStringLiteral("Download selected PDF…"), hitsPane);
    auto *open = new QPushButton(QStringLiteral("Open paper page"), hitsPane);
    hitButtons->addWidget(download);
    hitButtons->addWidget(open);
    hitButtons->addStretch();
    hitsLayout->addLayout(hitButtons);

    splitter->addWidget(rulesPane);
    splitter->addWidget(hitsPane);
    splitter->setSizes({290, 350});
    outer->addWidget(splitter, 1);
    status_ = new QLabel(QStringLiteral("Ready"), this);
    status_->setWordWrap(true);
    outer->addWidget(status_);
    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Close, this);
    outer->addWidget(buttons);

    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
    connect(this, &QDialog::finished, manager_, [manager_ = manager_](int) { manager_->markHitsSeen(); });
    connect(addAuthor, &QPushButton::clicked, this, [this]() { editRule({}, QStringLiteral("author")); });
    connect(addCategory, &QPushButton::clicked, this, [this]() { editRule({}, QStringLiteral("category")); });
    connect(addCitations, &QPushButton::clicked, this, [this]() { editRule({}, QStringLiteral("citation")); });
    connect(edit, &QPushButton::clicked, this, [this]() { editRule(selectedRuleId()); });
    connect(remove, &QPushButton::clicked, this, [this]() { removeRule(); });
    connect(check, &QPushButton::clicked, manager_, &ArxivWatchManager::checkNow);
    connect(download, &QPushButton::clicked, this, [this]() { downloadSelected(); });
    auto openSelected = [this]() {
        const int row = hitsTable_->currentRow();
        if (row < 0 || !hitsTable_->item(row, 0))
            return;
        const QString id = hitsTable_->item(row, 0)->data(RoleArxivId).toString();
        if (!id.isEmpty())
            QDesktopServices::openUrl(QUrl(QStringLiteral("https://arxiv.org/abs/%1").arg(id)));
        else {
            const int recid = hitsTable_->item(row, 0)->data(RoleInspireRecid).toInt();
            if (recid > 0)
                QDesktopServices::openUrl(QUrl(QStringLiteral("https://inspirehep.net/literature/%1").arg(recid)));
        }
    };
    connect(open, &QPushButton::clicked, this, openSelected);
    connect(hitsTable_, &QTableWidget::cellDoubleClicked, this, [openSelected](int, int) { openSelected(); });
    connect(rulesTable_, &QTableWidget::cellDoubleClicked, this, [this](int, int) { editRule(selectedRuleId()); });
    connect(manager_, &ArxivWatchManager::changed, this, [this]() { refresh(); });
    connect(manager_, &ArxivWatchManager::statusChanged, this, [this](const QString &message) { status_->setText(message); });
    refresh();
}

QString ArxivWatchesDialog::selectedRuleId() const
{
    const int row = rulesTable_->currentRow();
    const QTableWidgetItem *item = row >= 0 ? rulesTable_->item(row, 0) : nullptr;
    return item ? item->data(RoleId).toString() : QString();
}

void ArxivWatchesDialog::refresh()
{
    const QString selectedId = selectedRuleId();
    rulesTable_->setRowCount(manager_->rules().size());
    int row = 0;
    for (const ArxivWatchRule &rule : manager_->rules()) {
        auto *type = new QTableWidgetItem(rule.kind == QStringLiteral("author") ? QStringLiteral("Author")
                                          : rule.kind == QStringLiteral("citation") ? QStringLiteral("Citations")
                                                                                       : QStringLiteral("Category"));
        type->setData(RoleId, rule.id);
        rulesTable_->setItem(row, 0, type);
        auto *term = new QTableWidgetItem(rule.kind == QStringLiteral("citation") && !rule.paperTitle.isEmpty()
                                              ? QStringLiteral("%1 (arXiv:%2)").arg(rule.paperTitle, rule.term)
                                              : rule.term);
        term->setToolTip(term->text());
        rulesTable_->setItem(row, 1, term);
        rulesTable_->setItem(row, 2, new QTableWidgetItem(rule.autoDownload ? QStringLiteral("Auto-download") : QStringLiteral("Notify")));
        rulesTable_->setItem(row, 3, new QTableWidgetItem(rule.autoDownload ? rule.folder : QString()));
        rulesTable_->setItem(row, 4, new QTableWidgetItem(rule.lastCheckedUtc.isValid()
                                                            ? rule.lastCheckedUtc.toLocalTime().toString(QStringLiteral("yyyy-MM-dd hh:mm"))
                                                            : QStringLiteral("Never")));
        rulesTable_->setItem(row, 5, new QTableWidgetItem(rule.enabled ? QStringLiteral("Yes") : QStringLiteral("No")));
        auto *lastError = new QTableWidgetItem(rule.lastError);
        lastError->setToolTip(rule.lastError);
        rulesTable_->setItem(row, 6, lastError);
        if (rule.id == selectedId)
            rulesTable_->selectRow(row);
        ++row;
    }
    hitsTable_->setRowCount(manager_->hits().size());
    row = 0;
    for (const ArxivWatchHit &hit : manager_->hits()) {
        QString label;
        for (const ArxivWatchRule &rule : manager_->rules()) {
            if (rule.id == hit.ruleId) {
                label = rule.kind == QStringLiteral("citation") && !rule.paperTitle.isEmpty()
                            ? rule.paperTitle : rule.term;
                break;
            }
        }
        auto *found = new QTableWidgetItem(hit.foundUtc.toLocalTime().toString(QStringLiteral("yyyy-MM-dd hh:mm")));
        found->setData(RoleId, hit.ruleId);
        found->setData(RoleArxivId, hit.paper.arxivId);
        found->setData(RoleInspireRecid, hit.inspireRecid);
        hitsTable_->setItem(row, 0, found);
        hitsTable_->setItem(row, 1, new QTableWidgetItem(label));
        auto *title = new QTableWidgetItem(hit.paper.title);
        title->setToolTip(hit.paper.title);
        hitsTable_->setItem(row, 2, title);
        hitsTable_->setItem(row, 3, new QTableWidgetItem(hit.paper.arxivId));
        hitsTable_->setItem(row, 4, new QTableWidgetItem(hit.paper.published.left(10)));
        hitsTable_->setItem(row, 5, new QTableWidgetItem(hit.status));
        ++row;
    }
}

void ArxivWatchesDialog::editRule(const QString &id, const QString &initialKind)
{
    if (!id.isEmpty() && selectedRuleId().isEmpty())
        return;
    ArxivWatchRule rule;
    rule.id = id;
    rule.kind = initialKind;
    for (const ArxivWatchRule &existing : manager_->rules()) {
        if (existing.id == id) {
            rule = existing;
            break;
        }
    }
    if (!id.isEmpty() && rule.term.isEmpty())
        return;

    QDialog dialog(this);
    dialog.setWindowTitle(id.isEmpty() ? QStringLiteral("Add research watch") : QStringLiteral("Edit research watch"));
    auto *outer = new QVBoxLayout(&dialog);
    auto *form = new QFormLayout;
    auto *kind = new QComboBox(&dialog);
    kind->addItem(QStringLiteral("Author name"), QStringLiteral("author"));
    kind->addItem(QStringLiteral("Category code"), QStringLiteral("category"));
    kind->addItem(QStringLiteral("Citations to a paper"), QStringLiteral("citation"));
    kind->setCurrentIndex(rule.kind == QStringLiteral("category") ? 1 : rule.kind == QStringLiteral("citation") ? 2 : 0);
    auto *term = new QLineEdit(rule.term, &dialog);
    term->setPlaceholderText(rule.kind == QStringLiteral("category") ? QStringLiteral("hep-ph")
                              : rule.kind == QStringLiteral("citation") ? QStringLiteral("2609.12345")
                                                                           : QStringLiteral("Author's name"));
    auto *enabled = new QCheckBox(QStringLiteral("Check this watch daily"), &dialog);
    enabled->setChecked(rule.enabled);
    auto *automatic = new QCheckBox(QStringLiteral("Automatically download matching PDFs"), &dialog);
    automatic->setChecked(rule.autoDownload);
    auto *folderWidget = new QWidget(&dialog);
    auto *folderRow = new QHBoxLayout(folderWidget);
    folderRow->setContentsMargins(0, 0, 0, 0);
    QString defaultFolder = rule.folder;
    if (defaultFolder.isEmpty() && db_) {
        const QStringList watched = db_->folders();
        if (!watched.isEmpty())
            defaultFolder = watched.first();
    }
    if (defaultFolder.isEmpty())
        defaultFolder = QStandardPaths::writableLocation(QStandardPaths::DocumentsLocation);
    auto *folder = new QLineEdit(defaultFolder, folderWidget);
    auto *browse = new QPushButton(QStringLiteral("Browse…"), folderWidget);
    folderRow->addWidget(folder, 1);
    folderRow->addWidget(browse);
    folderWidget->setEnabled(automatic->isChecked());
    form->addRow(QStringLiteral("Watch:"), kind);
    form->addRow(QStringLiteral("Name / code / arXiv ID:"), term);
    form->addRow(QString(), enabled);
    form->addRow(QString(), automatic);
    form->addRow(QStringLiteral("Download folder:"), folderWidget);
    outer->addLayout(form);
    auto *hint = new QLabel(QStringLiteral("Author watches match names in arXiv metadata; namesakes can match. Category watches use codes such as hep-ph. Citation watches use INSPIRE; the first check records existing citations as a baseline. Automatic PDF downloads apply when a citing paper has an arXiv ID."), &dialog);
    hint->setWordWrap(true);
    outer->addWidget(hint);
    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Cancel | QDialogButtonBox::Save, &dialog);
    outer->addWidget(buttons);
    connect(buttons, &QDialogButtonBox::accepted, &dialog, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
    connect(automatic, &QCheckBox::toggled, folderWidget, &QWidget::setEnabled);
    connect(kind, &QComboBox::currentIndexChanged, &dialog, [kind, term]() {
        const QString selected = kind->currentData().toString();
        term->setPlaceholderText(selected == QStringLiteral("category") ? QStringLiteral("hep-ph")
                                 : selected == QStringLiteral("citation") ? QStringLiteral("2609.12345")
                                                                            : QStringLiteral("Author's name"));
    });
    connect(browse, &QPushButton::clicked, &dialog, [&dialog, folder]() {
        const QString choice = QFileDialog::getExistingDirectory(&dialog, QStringLiteral("Choose PDF download folder"), folder->text());
        if (!choice.isEmpty())
            folder->setText(choice);
    });

    while (dialog.exec() == QDialog::Accepted) {
        rule.kind = kind->currentData().toString();
        rule.term = term->text();
        rule.enabled = enabled->isChecked();
        rule.autoDownload = automatic->isChecked();
        rule.folder = folder->text().trimmed();
        QString error;
        const bool saved = id.isEmpty() ? manager_->addRule(rule, &error) : manager_->updateRule(rule, &error);
        if (saved)
            break;
        QMessageBox::warning(&dialog, QStringLiteral("Research watch"), error);
    }
}

void ArxivWatchesDialog::removeRule()
{
    const QString id = selectedRuleId();
    if (id.isEmpty())
        return;
    if (QMessageBox::question(this, QStringLiteral("Remove watch"),
                              QStringLiteral("Remove this watch and its saved matches?")) == QMessageBox::Yes)
        manager_->removeRule(id);
}

void ArxivWatchesDialog::downloadSelected()
{
    const int row = hitsTable_->currentRow();
    const QTableWidgetItem *item = row >= 0 ? hitsTable_->item(row, 0) : nullptr;
    if (!item)
        return;
    const QString ruleId = item->data(RoleId).toString();
    const QString arxivId = item->data(RoleArxivId).toString();
    if (arxivId.isEmpty()) {
        QMessageBox::information(this, QStringLiteral("No arXiv PDF"),
                                 QStringLiteral("This citing paper has no arXiv ID in INSPIRE, so there is no arXiv PDF to download."));
        return;
    }
    QString folder;
    for (const ArxivWatchRule &rule : manager_->rules()) {
        if (rule.id == ruleId) {
            folder = rule.folder;
            break;
        }
    }
    if (folder.isEmpty() && db_) {
        const QStringList watched = db_->folders();
        if (!watched.isEmpty())
            folder = watched.first();
    }
    const QString chosen = QFileDialog::getExistingDirectory(this, QStringLiteral("Download PDF to folder"), folder);
    if (!chosen.isEmpty())
        manager_->downloadHit(ruleId, arxivId, chosen);
}
