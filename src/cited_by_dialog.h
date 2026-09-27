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

class CitedByDialog : public QDialog {
public:
    explicit CitedByDialog(Database *database,
                           const QString &sourceArxivId,
                           QWidget *parent = nullptr);
    ~CitedByDialog() override;

    bool libraryChanged() const { return libraryChanged_; }
    bool metricsChanged() const { return metricsChanged_; }

private:
    void buildUi();
    void reload();
    void refreshFromInspire();
    void fetchWithRecid(int recid);
    void updateSummary();

    QList<int> selectedIndexes() const;
    void openSelected();
    void openSelectedArxiv();
    void openSelectedInspire();
    void chooseDownloadFolder();
    QString downloadFolder() const;
    void downloadSelected();
    void downloadNext();
    void finishCurrentDownload(bool success, const QString &message = {});

    static QString safeFilenameId(QString arxivId);

    Database *db_ = nullptr;
    QString sourceArxivId_;
    InspireClient inspire_;
    QNetworkAccessManager network_;
    QList<RelatedPaperRecord> papers_;
    QList<int> pendingDownloads_;
    int currentDownloadIndex_ = -1;
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
    QComboBox *limit_ = nullptr;
    QComboBox *folder_ = nullptr;
    QPushButton *refreshButton_ = nullptr;
    QPushButton *openButton_ = nullptr;
    QPushButton *arxivButton_ = nullptr;
    QPushButton *inspireButton_ = nullptr;
    QPushButton *downloadButton_ = nullptr;
    QProgressBar *progress_ = nullptr;
    QLabel *status_ = nullptr;
};
