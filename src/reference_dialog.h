#pragma once

#include "database.h"
#include "inspire_client.h"

#include <QDialog>
#include <QNetworkAccessManager>

class QComboBox;
class QLabel;
class QNetworkReply;
class QProgressBar;
class QPushButton;
class QSaveFile;
class QTableWidget;

class ReferenceDialog : public QDialog {
public:
    explicit ReferenceDialog(Database *database,
                             const QString &sourceArxivId,
                             QWidget *parent = nullptr);
    ~ReferenceDialog() override;

    bool libraryChanged() const { return libraryChanged_; }
    bool metricsChanged() const { return metricsChanged_; }

private:
    void buildUi();
    void reload();
    void refreshFromInspire();
    void updateSummary();

    QList<int> selectedReferenceIndexes() const;
    void openSelected();
    void openSelectedArxiv();
    void chooseDownloadFolder();
    QString downloadFolder() const;
    void downloadSelected();
    void downloadNext();
    void finishCurrentDownload(bool success, const QString &message = {});
    void exportReferencesBibTeX();

    static QString safeFilenameId(QString arxivId);
    static QString bibEscape(QString text);

    Database *db_ = nullptr;
    QString sourceArxivId_;
    InspireClient inspire_;
    QNetworkAccessManager network_;
    QList<ReferenceRecord> references_;
    QList<int> pendingDownloads_;
    int currentDownloadRef_ = -1;
    bool libraryChanged_ = false;
    bool metricsChanged_ = false;
    bool refreshing_ = false;
    bool downloading_ = false;

    QSaveFile *saveFile_ = nullptr;
    QNetworkReply *downloadReply_ = nullptr;
    QString currentTargetPath_;
    QByteArray currentPrefix_;

    QLabel *summary_ = nullptr;
    QTableWidget *table_ = nullptr;
    QComboBox *folder_ = nullptr;
    QPushButton *refreshButton_ = nullptr;
    QPushButton *openButton_ = nullptr;
    QPushButton *arxivButton_ = nullptr;
    QPushButton *downloadButton_ = nullptr;
    QPushButton *exportButton_ = nullptr;
    QProgressBar *progress_ = nullptr;
    QLabel *status_ = nullptr;
};
