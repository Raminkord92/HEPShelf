#pragma once

#include "database.h"

#include <QDialog>
#include <functional>

class QLabel;
class QListWidget;
class QPlainTextEdit;
class QPushButton;
class QTableWidget;

class LiteratureTrailsDialog : public QDialog {
public:
    using OpenLocalCallback = std::function<void(const QString &arxivId, const QString &path)>;

    LiteratureTrailsDialog(Database *database,
                           OpenLocalCallback openLocal,
                           QWidget *parent = nullptr);

    bool dataChanged() const { return dataChanged_; }
    void selectTrail(int trailId);

private:
    void buildUi();
    void refreshTrails(int selectId = 0);
    void loadSelectedTrail();
    int selectedTrailId() const;
    int selectedItemId() const;
    TrailItemRecord selectedItem() const;

    void createTrail();
    void renameTrail();
    void deleteTrail();
    void saveTrailDetails();
    void moveSelectedItem(int delta);
    void removeSelectedItem();
    void saveSelectedItemNote();
    void openSelectedItem();
    void exportBibTeX();
    void exportReadingList();

    Database *db_ = nullptr;
    OpenLocalCallback openLocal_;
    bool dataChanged_ = false;
    bool loadingItemNote_ = false;

    QListWidget *trailList_ = nullptr;
    QLabel *trailSummary_ = nullptr;
    QPlainTextEdit *description_ = nullptr;
    QPushButton *saveTrailButton_ = nullptr;
    QTableWidget *itemsTable_ = nullptr;
    QPlainTextEdit *itemNote_ = nullptr;
    QLabel *itemStatus_ = nullptr;
    QPushButton *saveItemNoteButton_ = nullptr;
    QPushButton *openItemButton_ = nullptr;
    QPushButton *upButton_ = nullptr;
    QPushButton *downButton_ = nullptr;
    QPushButton *removeItemButton_ = nullptr;
    QPushButton *exportBibButton_ = nullptr;
    QPushButton *exportReadingButton_ = nullptr;
};
