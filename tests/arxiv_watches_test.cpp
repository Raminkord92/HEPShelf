#include "arxiv_feed.h"
#include "arxiv_watches.h"

#include <QApplication>
#include <QSettings>
#include <QTemporaryDir>

#include <cstdlib>

namespace {
void require(bool condition)
{
    if (!condition)
        std::abort();
}
}

int main(int argc, char **argv)
{
    QApplication app(argc, argv);
    QTemporaryDir settingsDir;
    require(settingsDir.isValid());
    QSettings::setDefaultFormat(QSettings::IniFormat);
    QSettings::setPath(QSettings::IniFormat, QSettings::UserScope, settingsDir.path());
    QCoreApplication::setOrganizationName(QStringLiteral("HEPShelfWatchTests"));
    QCoreApplication::setApplicationName(QStringLiteral("Watches"));

    const QByteArray feed = R"XML(<?xml version="1.0" encoding="UTF-8"?>
<feed xmlns="http://www.w3.org/2005/Atom"
      xmlns:opensearch="http://a9.com/-/spec/opensearch/1.1/"
      xmlns:arxiv="http://arxiv.org/schemas/atom">
  <opensearch:totalResults>1</opensearch:totalResults>
  <entry>
    <id>https://arxiv.org/abs/2609.12345v2</id>
    <title>A new particle paper</title>
    <summary>Short abstract</summary>
    <published>2026-09-26T12:00:00Z</published>
    <author><name>Jane Doe</name></author>
    <arxiv:primary_category term="hep-ph" />
  </entry>
</feed>)XML";
    const ArxivFeedResult parsed = parseArxivFeed(feed);
    require(parsed.error.isEmpty());
    require(parsed.totalResults == 1);
    require(parsed.entryCount == 1);
    require(parsed.papers.size() == 1);
    require(parsed.papers.first().arxivId == QStringLiteral("2609.12345"));
    require(parsed.papers.first().authorNames == QStringList{QStringLiteral("Jane Doe")});
    require(parsed.papers.first().primaryCategory == QStringLiteral("hep-ph"));

    const QByteArray oai = R"XML(<OAI-PMH xmlns="http://www.openarchives.org/OAI/2.0/">
<ListRecords><record><header><identifier>oai:arXiv.org:2609.12345</identifier></header>
<metadata><arXiv xmlns="http://arxiv.org/OAI/arXiv/"><id>2609.12345</id>
<created>2026-09-28</created><authors><author><keyname>Valeshabadi</keyname>
<forenames>Kord</forenames></author></authors><title>New paper</title>
<categories>hep-ph hep-th</categories><abstract>Abstract text</abstract></arXiv></metadata>
</record><resumptionToken>next-page</resumptionToken></ListRecords></OAI-PMH>)XML";
    const ArxivOaiResult oaiParsed = parseArxivOai(oai);
    require(oaiParsed.error.isEmpty());
    require(oaiParsed.nextToken == QStringLiteral("next-page"));
    require(oaiParsed.papers.size() == 1);
    require(oaiParsed.papers.first().arxivId == QStringLiteral("2609.12345"));
    require(oaiParsed.papers.first().authorNames == QStringList{QStringLiteral("Kord Valeshabadi")});
    require(oaiParsed.papers.first().published == QStringLiteral("2026-09-28"));
    require(oaiParsed.papers.first().categories == QStringLiteral("hep-ph hep-th"));
    require(parseArxivOai(QByteArrayLiteral("<OAI-PMH><error code=\"noRecordsMatch\">No records</error></OAI-PMH>")).error.isEmpty());
    require(!parseArxivOai(QByteArrayLiteral("<html>rate limited</html>")).error.isEmpty());

    QString ruleId;
    QString citationRuleId;
    {
        ArxivWatchManager manager(nullptr);
        ArxivWatchRule invalid;
        invalid.kind = QStringLiteral("category");
        invalid.term = QStringLiteral("hep-ph OR au:other");
        QString error;
        require(!manager.addRule(invalid, &error));
        require(!error.isEmpty());

        ArxivWatchRule author;
        author.kind = QStringLiteral("author");
        author.term = QStringLiteral("Jane Doe");
        require(manager.addRule(author, &error));
        require(manager.rules().size() == 1);
        require(!manager.rules().first().lastCheckedUtc.isValid());
        ruleId = manager.rules().first().id;
        require(!manager.addRule(author, &error));

        ArxivWatchRule edited = manager.rules().first();
        edited.autoDownload = true;
        edited.folder = settingsDir.path();
        edited.enabled = false;
        require(manager.updateRule(edited, &error));

        ArxivWatchRule citation;
        citation.kind = QStringLiteral("citation");
        citation.term = QStringLiteral("hep-th/9711200v2");
        citation.paperTitle = QStringLiteral("The Large N limit");
        citation.enabled = false;
        require(manager.addRule(citation, &error));
        citationRuleId = manager.rules().last().id;
        require(manager.rules().last().term == QStringLiteral("hep-th/9711200"));
        require(!manager.addRule(citation, &error));
        ArxivWatchRule established = manager.rules().last();
        established.citationBaselineReady = true;
        established.inspireRecid = 451647;
        established.seenCitationIds = {QStringLiteral("recid:100"), QStringLiteral("recid:200")};
        require(manager.updateRule(established, &error));
    }
    {
        ArxivWatchManager manager(nullptr);
        require(manager.rules().size() == 2);
        require(manager.rules().first().id == ruleId);
        require(manager.rules().first().autoDownload);
        require(!manager.rules().first().enabled);
        const ArxivWatchRule saved = manager.rules().last();
        require(saved.id == citationRuleId);
        require(saved.citationBaselineReady);
        require(saved.inspireRecid == 451647);
        require(saved.seenCitationIds.size() == 2);
        ArxivWatchRule changed = saved;
        changed.term = QStringLiteral("2609.12345");
        QString error;
        require(manager.updateRule(changed, &error));
        require(!manager.rules().last().citationBaselineReady);
        require(!manager.rules().last().lastCheckedUtc.isValid());
        require(manager.rules().last().seenCitationIds.isEmpty());
        require(manager.rules().last().inspireRecid == 0);
        manager.removeRule(ruleId);
        manager.removeRule(citationRuleId);
        require(manager.rules().isEmpty());
    }
    return 0;
}
