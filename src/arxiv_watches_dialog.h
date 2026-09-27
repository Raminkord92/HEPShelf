#pragma once

#include "arxiv_watches.h"

#include <QDialog>

class QLabel;
class QTableWidget;

class ArxivWatchesDialog : public QDialog {
public:
    explicit ArxivWatchesDialog(ArxivWatchManager *manager, Database *database, QWidget *parent = nullptr);

private:
    void refresh();
    void editRule(const QString &id = {}, const QString &initialKind = QStringLiteral("author"));
    void removeRule();
    void downloadSelected();
    QString selectedRuleId() const;

    ArxivWatchManager *manager_ = nullptr;
    Database *db_ = nullptr;
    QTableWidget *rulesTable_ = nullptr;
    QTableWidget *hitsTable_ = nullptr;
    QLabel *status_ = nullptr;
};
