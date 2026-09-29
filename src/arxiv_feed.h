#pragma once

#include "database.h"

#include <QByteArray>

struct ArxivFeedResult {
    QList<PaperRecord> papers;
    int totalResults = 0;
    int entryCount = 0;
    QString error;
};

ArxivFeedResult parseArxivFeed(const QByteArray &xml);

struct ArxivOaiResult {
    QList<PaperRecord> papers;
    QString nextToken;
    QString error;
};

ArxivOaiResult parseArxivOai(const QByteArray &xml);
