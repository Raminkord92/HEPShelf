#pragma once

#include "database.h"

#include <QByteArray>
#include <QDialog>
#include <QList>
#include <QNetworkAccessManager>
#include <QString>
#include <QStringList>

class QComboBox;
class QLabel;
class QLineEdit;
class QNetworkReply;
class QProgressBar;
class QPushButton;
class QSaveFile;
class QSpinBox;
class QTableWidget;
class QTextBrowser;

class ArxivDiscoveryDialog : public QDialog {
public:
    explicit ArxivDiscoveryDialog(Database *database, QWidget *parent = nullptr);
    ~ArxivDiscoveryDialog() override;

    bool libraryChanged() const { return libraryChanged_; }
    void setAuthorSearch(const QString &author, bool startImmediately = true);

private:
    enum class SearchField {
        All,
        Title,
        Author,
        Abstract,
        Category,
        ExactId,
        Advanced
    };

    void buildUi();
    void restoreSettings();
    void saveSettings();

    void startSearch();
    QString buildSearchExpression(QString *error) const;
    QList<PaperRecord> parseFeed(const QByteArray &xml, QString *error) const;
    void populateResults();
    void showSelectedAbstract();
    void openSelectedArxivPage();

    void chooseDownloadFolder();
    QString currentDownloadFolder() const;
    void downloadSelected();
    void downloadNext();
    void startDownloadForIndex(int resultIndex);
    void finishDownload(bool success, const QString &message = {});
    void markResultLocal(const QString &arxivId, const QString &path);
    bool resultIsLocal(const QString &arxivId) const;
    QString targetPathFor(const PaperRecord &paper) const;

    static QString compactAuthors(const QString &authors, int maxChars = 90);
    static QString filenameId(QString arxivId);

    Database *db_ = nullptr;
    QNetworkAccessManager network_;
    QList<PaperRecord> results_;
    QList<int> pendingDownloads_;
    int currentDownloadIndex_ = -1;
    bool libraryChanged_ = false;
    bool searchBusy_ = false;
    bool downloadBusy_ = false;
    qint64 lastApiQueryMs_ = 0;

    QSaveFile *currentSaveFile_ = nullptr;
    QNetworkReply *currentDownloadReply_ = nullptr;
    QString currentDownloadPath_;
    QByteArray currentDownloadPrefix_;

    QLineEdit *queryEdit_ = nullptr;
    QComboBox *fieldCombo_ = nullptr;
    QComboBox *sortCombo_ = nullptr;
    QComboBox *limitCombo_ = nullptr;
    QPushButton *searchButton_ = nullptr;

    QTableWidget *resultsTable_ = nullptr;
    QTextBrowser *abstractView_ = nullptr;
    QLabel *resultSummary_ = nullptr;

    QComboBox *downloadFolderCombo_ = nullptr;
    QPushButton *browseFolderButton_ = nullptr;
    QPushButton *downloadButton_ = nullptr;
    QPushButton *openArxivButton_ = nullptr;
    QProgressBar *downloadProgress_ = nullptr;
    QLabel *downloadStatus_ = nullptr;
};
