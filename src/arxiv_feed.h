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
