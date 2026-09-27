#include "citation_graph_dialog.h"

#include <algorithm>
#include <cmath>
#include <utility>

#include <QAction>
#include <QApplication>
#include <QCheckBox>
#include <QDesktopServices>
#include <QDialogButtonBox>
#include <QFont>
#include <QGraphicsItem>
#include <QGraphicsScene>
#include <QGraphicsSceneContextMenuEvent>
#include <QGraphicsSceneMouseEvent>
#include <QGraphicsView>
#include <QHBoxLayout>
#include <QLabel>
#include <QInputDialog>
#include <QLineEdit>
#include <QLineF>
#include <QMap>
#include <QMenu>
#include <QMessageBox>
#include <QPainter>
#include <QPalette>
#include <QPolygonF>
#include <QPushButton>
#include <QRegularExpression>
#include <QSettings>
#include <QSpinBox>
#include <QTimer>
#include <QUrl>
#include <QVBoxLayout>
#include <QWheelEvent>
#include <QXmlStreamReader>

namespace {
constexpr int MaxGraphNodes = 400;
constexpr int InitialCitingFetch = 250;
constexpr int CitingFetchStep = 250;
constexpr int MaxCitingFetch = 1000;
constexpr qreal NodeHalfWidth = 132.0;
constexpr qreal NodeHalfHeight = 44.0;

class GraphView final : public QGraphicsView {
public:
    using QGraphicsView::QGraphicsView;

protected:
    void wheelEvent(QWheelEvent *event) override
    {
        const qreal factor = event->angleDelta().y() > 0 ? 1.15 : (1.0 / 1.15);
        scale(factor, factor);
        event->accept();
    }
};

class GraphNodeItem final : public QGraphicsItem {
public:
    GraphNodeItem(QString key,
                  QString title,
                  QString subtitle,
                  bool local,
                  bool center,
                  std::function<void()> activate,
                  std::function<void(const QPoint &)> contextMenu)
        : key_(std::move(key)),
          title_(std::move(title)),
          subtitle_(std::move(subtitle)),
          local_(local),
          center_(center),
          activate_(std::move(activate)),
          contextMenu_(std::move(contextMenu))
    {
        setFlag(QGraphicsItem::ItemIsSelectable, true);
        setAcceptHoverEvents(true);
        setToolTip(title_ + (subtitle_.isEmpty() ? QString() : QStringLiteral("\n") + subtitle_));
    }

    const QString &key() const { return key_; }
    void setTitle(const QString &displayTitle, const QString &fullTitle)
    {
        title_ = displayTitle;
        setToolTip(fullTitle + (subtitle_.isEmpty() ? QString() : QStringLiteral("\n") + subtitle_));
        update();
    }
    QRectF boundingRect() const override
    {
        return QRectF(-NodeHalfWidth, -NodeHalfHeight, NodeHalfWidth * 2.0, NodeHalfHeight * 2.0);
    }

    void paint(QPainter *painter, const QStyleOptionGraphicsItem *, QWidget *) override
    {
        const QPalette palette = QApplication::palette();
        QColor fill = center_ ? palette.button().color() : palette.base().color();
        if (isSelected())
            fill = palette.highlight().color().lighter(175);

        QColor border = local_ ? palette.highlight().color() : palette.mid().color();
        if (center_)
            border = palette.text().color();

        painter->setRenderHint(QPainter::Antialiasing, true);
        painter->setPen(QPen(border, center_ ? 2.4 : (local_ ? 2.0 : 1.2)));
        painter->setBrush(fill);
        painter->drawRoundedRect(boundingRect(), 9, 9);

        QRectF titleRect = boundingRect().adjusted(11, 7, -11, -29);
        QFont titleFont = painter->font();
        titleFont.setBold(center_ || local_);
        if (center_)
            titleFont.setPointSizeF(titleFont.pointSizeF() + 0.8);
        painter->setFont(titleFont);
        painter->setPen(palette.text().color());
        painter->drawText(titleRect, Qt::AlignLeft | Qt::AlignVCenter | Qt::TextWordWrap, title_);

        QFont subFont = painter->font();
        subFont.setBold(false);
        subFont.setPointSizeF(qMax(7.0, subFont.pointSizeF() - 1.1));
        painter->setFont(subFont);
        painter->setPen(palette.mid().color());
        painter->drawText(boundingRect().adjusted(11, 57, -11, -7),
                          Qt::AlignLeft | Qt::AlignVCenter,
                          subtitle_);
    }

protected:
    void mouseDoubleClickEvent(QGraphicsSceneMouseEvent *event) override
    {
        if (activate_)
            activate_();
        event->accept();
    }

    void contextMenuEvent(QGraphicsSceneContextMenuEvent *event) override
    {
        setSelected(true);
        if (contextMenu_)
            contextMenu_(event->screenPos());
        event->accept();
    }

private:
    QString key_;
    QString title_;
    QString subtitle_;
    bool local_ = false;
    bool center_ = false;
    std::function<void()> activate_;
    std::function<void(const QPoint &)> contextMenu_;
};

QString shortTitle(QString value, int max = 72)
{
    value = value.simplified();
    if (value.isEmpty())
        return QStringLiteral("Untitled paper");
    if (value.size() <= max)
        return value;
    return value.left(max - 1) + QChar(0x2026);
}

QString referenceTitle(const ReferenceRecord &ref)
{
    if (!ref.title.trimmed().isEmpty())
        return ref.title;
    if (!ref.rawText.trimmed().isEmpty()) {
        const QString raw = ref.rawText.trimmed();
        if (raw.startsWith(QLatin1Char('<'))) {
            QXmlStreamReader xml(raw);
            QStringList words;
            while (!xml.atEnd()) {
                xml.readNext();
                if (xml.isCharacters() && !xml.isWhitespace())
                    words << xml.text().toString().trimmed();
            }
            const QString plain = words.join(QLatin1Char(' ')).simplified();
            if (!plain.isEmpty())
                return plain;
            QString stripped = raw;
            stripped.replace(QRegularExpression(QStringLiteral("<[^>]+>")), QStringLiteral(" "));
            return stripped.simplified();
        }
        return raw;
    }
    if (!ref.arxivId.isEmpty())
        return QStringLiteral("arXiv:%1").arg(ref.arxivId);
    return QStringLiteral("Reference %1").arg(ref.position);
}

QString normalizedTitle(QString value)
{
    value = value.toLower().simplified();
    value.remove(QRegularExpression(QStringLiteral("[^\\p{L}\\p{N} ]+")));
    return value.simplified();
}

void addSectionLabel(QGraphicsScene *scene, const QString &text, const QPointF &pos)
{
    auto *label = scene->addText(text);
    QFont font = label->font();
    font.setBold(true);
    font.setPointSizeF(font.pointSizeF() + 0.8);
    label->setFont(font);
    label->setDefaultTextColor(QApplication::palette().text().color());
    label->setPos(pos);
}

void addArrow(QGraphicsScene *scene, const QPointF &fromCenter, const QPointF &toCenter)
{
    QPointF start = fromCenter;
    QPointF end = toCenter;
    const qreal dx = toCenter.x() - fromCenter.x();
    const qreal dy = toCenter.y() - fromCenter.y();

    if (qAbs(dx) >= qAbs(dy)) {
        if (dx >= 0) {
            start.rx() += NodeHalfWidth;
            end.rx() -= NodeHalfWidth;
        } else {
            start.rx() -= NodeHalfWidth;
            end.rx() += NodeHalfWidth;
        }
    } else {
        if (dy >= 0) {
            start.ry() += NodeHalfHeight;
            end.ry() -= NodeHalfHeight;
        } else {
            start.ry() -= NodeHalfHeight;
            end.ry() += NodeHalfHeight;
        }
    }

    QPen pen(QApplication::palette().mid().color(), 1.2);
    pen.setCosmetic(true);
    auto *lineItem = scene->addLine(QLineF(start, end), pen);
    lineItem->setZValue(0);

    QLineF line(start, end);
    if (line.length() < 1.0)
        return;
    const double angle = std::atan2(line.dy(), line.dx());
    const double wing = 9.0;
    const QPointF p1 = end - QPointF(std::cos(angle - 0.45) * wing,
                                     std::sin(angle - 0.45) * wing);
    const QPointF p2 = end - QPointF(std::cos(angle + 0.45) * wing,
                                     std::sin(angle + 0.45) * wing);
    QPolygonF triangle;
    triangle << end << p1 << p2;
    auto *arrow = scene->addPolygon(triangle, pen, QBrush(QApplication::palette().mid().color()));
    arrow->setZValue(0);
}

QString depthLabel(int depth)
{
    if (depth == 0)
        return QStringLiteral("ROOT");
    if (depth < 0)
        return QStringLiteral("%1 step%2 earlier / references")
            .arg(-depth)
            .arg(depth == -1 ? QString() : QStringLiteral("s"));
    return QStringLiteral("%1 step%2 later / cited by")
        .arg(depth)
        .arg(depth == 1 ? QString() : QStringLiteral("s"));
}
}

CitationGraphDialog::CitationGraphDialog(Database *database,
                                         const QString &sourceArxivId,
                                         OpenLocalCallback openLocal,
                                         QWidget *parent)
    : QDialog(parent),
      db_(database),
      sourceArxivId_(sourceArxivId),
      openLocal_(std::move(openLocal)),
      inspire_(this)
{
    setWindowTitle(QStringLiteral("Citation network — %1").arg(sourceArxivId_));
    resize(1450, 900);
    setMinimumSize(940, 620);
    buildUi();

    QSettings settings;
    const QByteArray geometry = settings.value(QStringLiteral("citationGraph/geometry")).toByteArray();
    if (!geometry.isEmpty())
        restoreGeometry(geometry);
    neighborLimit_->setValue(settings.value(QStringLiteral("citationGraph/expansionLimit"), 10).toInt());
    localOnly_->setChecked(settings.value(QStringLiteral("citationGraph/localOnly"), false).toBool());

    resetGraph();
    if (!rootKey_.isEmpty()) {
        const GraphPaper root = nodes_.value(rootKey_);
        if (!root.referencesFetched || !root.citingFetched)
            refreshRootFromInspire();
    }
}

void CitationGraphDialog::buildUi()
{
    auto *root = new QVBoxLayout(this);
    root->setContentsMargins(12, 12, 12, 12);
    root->setSpacing(8);

    auto *top = new QHBoxLayout;
    summary_ = new QLabel(this);
    summary_->setTextInteractionFlags(Qt::TextSelectableByMouse);
    top->addWidget(summary_, 1);

    localOnly_ = new QCheckBox(QStringLiteral("Local papers only"), this);
    top->addWidget(localOnly_);
    top->addWidget(new QLabel(QStringLiteral("Per expansion"), this));
    neighborLimit_ = new QSpinBox(this);
    neighborLimit_->setRange(3, 30);
    neighborLimit_->setValue(10);
    neighborLimit_->setToolTip(QStringLiteral("How many references/citing papers to reveal each time a node is expanded."));
    top->addWidget(neighborLimit_);

    expandButton_ = new QPushButton(QStringLiteral("Expand selected"), this);
    saveTrailButton_ = new QPushButton(QStringLiteral("Save exploration as trail…"), this);
    resetButton_ = new QPushButton(QStringLiteral("Reset"), this);
    auto *zoomOut = new QPushButton(QStringLiteral("−"), this);
    auto *zoomIn = new QPushButton(QStringLiteral("+"), this);
    auto *fit = new QPushButton(QStringLiteral("Fit"), this);
    refreshButton_ = new QPushButton(QStringLiteral("Refresh root"), this);
    top->addWidget(expandButton_);
    top->addWidget(saveTrailButton_);
    top->addWidget(resetButton_);
    top->addWidget(zoomOut);
    top->addWidget(zoomIn);
    top->addWidget(fit);
    top->addWidget(refreshButton_);
    root->addLayout(top);

    scene_ = new QGraphicsScene(this);
    view_ = new GraphView(scene_, this);
    view_->setRenderHint(QPainter::Antialiasing, true);
    view_->setDragMode(QGraphicsView::ScrollHandDrag);
    view_->setTransformationAnchor(QGraphicsView::AnchorUnderMouse);
    view_->setResizeAnchor(QGraphicsView::AnchorViewCenter);
    view_->setBackgroundBrush(QApplication::palette().window());
    root->addWidget(view_, 1);

    auto *bottom = new QHBoxLayout;
    status_ = new QLabel(QStringLiteral("Right-click a node to expand references or cited-by papers. Double-click to open."), this);
    status_->setWordWrap(true);
    bottom->addWidget(status_, 1);
    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Close, this);
    bottom->addWidget(buttons);
    root->addLayout(bottom);

    connect(buttons, &QDialogButtonBox::rejected, this, [this]() {
        QSettings settings;
        settings.setValue(QStringLiteral("citationGraph/geometry"), saveGeometry());
        settings.setValue(QStringLiteral("citationGraph/expansionLimit"), neighborLimit_->value());
        settings.setValue(QStringLiteral("citationGraph/localOnly"), localOnly_->isChecked());
        reject();
    });
    connect(localOnly_, &QCheckBox::toggled, this, [this](bool) { rebuildGraph(true); });
    connect(expandButton_, &QPushButton::clicked, this, [this]() {
        const QString key = selectedNodeKey();
        if (key.isEmpty()) {
            status_->setText(QStringLiteral("Select a graph node first."));
            return;
        }
        expandNode(key, true, true);
    });
    connect(saveTrailButton_, &QPushButton::clicked, this, [this]() { saveExplorationAsTrail(); });
    connect(resetButton_, &QPushButton::clicked, this, [this]() { resetGraph(); });
    connect(refreshButton_, &QPushButton::clicked, this, [this]() { refreshRootFromInspire(); });
    connect(zoomIn, &QPushButton::clicked, this, [this]() { view_->scale(1.2, 1.2); });
    connect(zoomOut, &QPushButton::clicked, this, [this]() { view_->scale(1.0 / 1.2, 1.0 / 1.2); });
    connect(fit, &QPushButton::clicked, this, [this]() {
        if (!scene_->items().isEmpty())
            view_->fitInView(scene_->itemsBoundingRect().adjusted(-50, -50, 50, 50), Qt::KeepAspectRatio);
    });
}

void CitationGraphDialog::resetGraph()
{
    if (!db_)
        return;

    nodes_.clear();
    edges_.clear();
    edgeKeys_.clear();
    referenceCache_.clear();
    citingCache_.clear();
    rootKey_.clear();
    explorationOrder_.clear();

    QString error;
    if (!db_->paperDetails(sourceArxivId_, &sourceDetails_, &error)) {
        status_->setText(error);
        scene_->clear();
        return;
    }

    GraphPaper root;
    root.arxivId = sourceDetails_.paper.arxivId;
    root.title = sourceDetails_.paper.title;
    root.authors = sourceDetails_.paper.authors;
    root.doi = sourceDetails_.paper.doi;
    root.year = sourceDetails_.paper.published.left(4);
    root.inspireRecid = sourceDetails_.citations.inspireRecid;
    root.citationCount = sourceDetails_.citations.citationCount;
    root.local = true;
    root.localArxivId = sourceArxivId_;
    if (!sourceDetails_.files.isEmpty())
        root.localPath = sourceDetails_.files.first().path;
    rootKey_ = addOrMergeNode(root, 0);

    if (rootKey_.isEmpty()) {
        status_->setText(QStringLiteral("Could not create the root graph node."));
        return;
    }

    if (sourceDetails_.citations.referenceCount >= 0) {
        const QList<ReferenceRecord> refs = db_->referencesForPaper(sourceArxivId_, &error);
        if (error.isEmpty()) {
            referenceCache_.insert(rootKey_, refs);
            nodes_[rootKey_].referencesFetched = true;
        }
    }

    const CitingCacheInfo citingInfo = db_->citingCacheInfo(sourceArxivId_, &error);
    if (error.isEmpty() && !citingInfo.fetchedAt.isEmpty()) {
        const QList<RelatedPaperRecord> citing = db_->citingPapersForPaper(sourceArxivId_, &error);
        if (error.isEmpty()) {
            citingCache_.insert(rootKey_, citing);
            nodes_[rootKey_].citingFetched = true;
            nodes_[rootKey_].totalCiting = citingInfo.totalCount;
        }
    }

    if (nodes_.value(rootKey_).referencesFetched)
        addReferenceChunk(rootKey_);
    if (nodes_.value(rootKey_).citingFetched)
        addCitingChunk(rootKey_);

    rebuildGraph(true);
    status_->setText(QStringLiteral("Graph reset to the selected paper. Right-click any node to expand it."));
}

QString CitationGraphDialog::makeKey(const GraphPaper &paper) const
{
    if (!paper.arxivId.trimmed().isEmpty())
        return QStringLiteral("arxiv:") + paper.arxivId.trimmed().toLower();
    if (paper.inspireRecid > 0)
        return QStringLiteral("inspire:%1").arg(paper.inspireRecid);
    if (!paper.doi.trimmed().isEmpty())
        return QStringLiteral("doi:") + paper.doi.trimmed().toLower();
    const QString title = normalizedTitle(paper.title);
    if (!title.isEmpty())
        return QStringLiteral("title:") + title;
    return QStringLiteral("anonymous:%1").arg(nodes_.size() + 1);
}

QString CitationGraphDialog::findMatchingNode(const GraphPaper &paper) const
{
    const QString normTitle = normalizedTitle(paper.title);
    for (auto it = nodes_.cbegin(); it != nodes_.cend(); ++it) {
        const GraphPaper &existing = it.value();
        if (!paper.arxivId.isEmpty() && !existing.arxivId.isEmpty()
            && paper.arxivId.compare(existing.arxivId, Qt::CaseInsensitive) == 0)
            return it.key();
        if (paper.inspireRecid > 0 && existing.inspireRecid == paper.inspireRecid)
            return it.key();
        if (!paper.doi.isEmpty() && !existing.doi.isEmpty()
            && paper.doi.trimmed().compare(existing.doi.trimmed(), Qt::CaseInsensitive) == 0)
            return it.key();
        if (normTitle.size() >= 12 && normTitle == normalizedTitle(existing.title))
            return it.key();
    }
    return {};
}

void CitationGraphDialog::resolveLocal(GraphPaper *paper) const
{
    if (!paper || !db_)
        return;
    QString localId;
    QString path;
    if (db_->resolveLocalPaper(paper->arxivId, paper->doi, paper->inspireRecid, &localId, &path)) {
        paper->local = true;
        paper->localArxivId = localId;
        paper->localPath = path;
        if (paper->arxivId.isEmpty())
            paper->arxivId = localId;
    }
}

QString CitationGraphDialog::addOrMergeNode(GraphPaper paper, int depth)
{
    paper.depth = depth;
    resolveLocal(&paper);

    QString key = findMatchingNode(paper);
    if (key.isEmpty()) {
        if (nodes_.size() >= MaxGraphNodes)
            return {};
        key = makeKey(paper);
        QString base = key;
        int suffix = 2;
        while (nodes_.contains(key))
            key = base + QStringLiteral("#%1").arg(suffix++);
        paper.key = key;
        nodes_.insert(key, paper);
        explorationOrder_.append(key);
        return key;
    }

    GraphPaper &existing = nodes_[key];
    if (existing.arxivId.isEmpty())
        existing.arxivId = paper.arxivId;
    if (existing.inspireRecid <= 0)
        existing.inspireRecid = paper.inspireRecid;
    if ((!paper.titleNeedsResolution && existing.titleNeedsResolution)
        || existing.title.trimmed().isEmpty()
        || existing.title.startsWith(QStringLiteral("Untitled"))) {
        existing.title = paper.title;
        existing.titleNeedsResolution = paper.titleNeedsResolution;
    }
    if (existing.authors.isEmpty())
        existing.authors = paper.authors;
    if (existing.doi.isEmpty())
        existing.doi = paper.doi;
    if (existing.year.isEmpty())
        existing.year = paper.year;
    if (existing.citationCount < 0)
        existing.citationCount = paper.citationCount;
    if (paper.local) {
        existing.local = true;
        existing.localArxivId = paper.localArxivId;
        existing.localPath = paper.localPath;
    }
    if (key != rootKey_ && qAbs(depth) < qAbs(existing.depth))
        existing.depth = depth;
    return key;
}

void CitationGraphDialog::addEdge(const QString &from, const QString &to)
{
    if (from.isEmpty() || to.isEmpty() || from == to)
        return;
    const QString edgeKey = from + QChar(0x2192) + to;
    if (edgeKeys_.contains(edgeKey))
        return;
    edgeKeys_.insert(edgeKey);
    edges_.append({from, to});
}

void CitationGraphDialog::hydrateLocalCaches(const QString &key)
{
    if (!db_ || !nodes_.contains(key))
        return;
    GraphPaper &paper = nodes_[key];
    if (!paper.local || paper.localArxivId.isEmpty())
        return;

    PaperDetails details;
    QString error;
    if (!db_->paperDetails(paper.localArxivId, &details, &error))
        return;

    if (paper.inspireRecid <= 0)
        paper.inspireRecid = details.citations.inspireRecid;
    if (paper.citationCount < 0)
        paper.citationCount = details.citations.citationCount;
    if (paper.title.isEmpty())
        paper.title = details.paper.title;
    if (paper.authors.isEmpty())
        paper.authors = details.paper.authors;
    if (paper.doi.isEmpty())
        paper.doi = details.paper.doi;
    if (paper.year.isEmpty())
        paper.year = details.paper.published.left(4);

    if (!paper.referencesFetched && details.citations.referenceCount >= 0) {
        const QList<ReferenceRecord> refs = db_->referencesForPaper(paper.localArxivId, &error);
        if (error.isEmpty()) {
            referenceCache_.insert(key, refs);
            paper.referencesFetched = true;
        }
    }

    if (!paper.citingFetched) {
        const CitingCacheInfo info = db_->citingCacheInfo(paper.localArxivId, &error);
        if (error.isEmpty() && !info.fetchedAt.isEmpty()) {
            const QList<RelatedPaperRecord> papers = db_->citingPapersForPaper(paper.localArxivId, &error);
            if (error.isEmpty()) {
                citingCache_.insert(key, papers);
                paper.citingFetched = true;
                paper.totalCiting = info.totalCount;
            }
        }
    }
}

void CitationGraphDialog::addReferenceChunk(const QString &sourceKey)
{
    if (!nodes_.contains(sourceKey))
        return;
    const GraphPaper parent = nodes_.value(sourceKey);
    const QList<ReferenceRecord> refs = referenceCache_.value(sourceKey);
    const int start = qBound(0, parent.referenceCursor, refs.size());
    const int end = qMin(start + neighborLimit_->value(), refs.size());

    for (int i = start; i < end; ++i) {
        const ReferenceRecord &ref = refs.at(i);
        GraphPaper child;
        child.inspireRecid = ref.inspireRecid;
        child.arxivId = ref.arxivId;
        child.titleNeedsResolution = ref.title.trimmed().isEmpty() && ref.inspireRecid > 0
                                     && !resolvedReferenceTitles_.contains(ref.inspireRecid);
        child.title = resolvedReferenceTitles_.value(ref.inspireRecid, referenceTitle(ref));
        child.authors = ref.authors;
        child.doi = ref.doi;
        child.local = ref.local;
        child.localPath = ref.localPath;
        if (child.local && !child.arxivId.isEmpty())
            child.localArxivId = child.arxivId;
        const QString childKey = addOrMergeNode(child, parent.depth - 1);
        if (!childKey.isEmpty()) {
            addEdge(childKey, sourceKey);
            if (child.titleNeedsResolution && nodes_.value(childKey).titleNeedsResolution)
                queueReferenceTitle(ref.inspireRecid);
        }
    }
    nodes_[sourceKey].referenceCursor = end;
}

void CitationGraphDialog::queueReferenceTitle(int inspireRecid)
{
    if (inspireRecid <= 0 || requestedReferenceTitles_.contains(inspireRecid))
        return;
    requestedReferenceTitles_.insert(inspireRecid);
    pendingReferenceTitles_ << inspireRecid;
    QTimer::singleShot(0, this, [this]() { fetchPendingReferenceTitles(); });
}

void CitationGraphDialog::fetchPendingReferenceTitles()
{
    if (fetchingReferenceTitles_ || pendingReferenceTitles_.isEmpty())
        return;
    fetchingReferenceTitles_ = true;
    QList<int> batch;
    while (!pendingReferenceTitles_.isEmpty() && batch.size() < 20)
        batch << pendingReferenceTitles_.takeFirst();

    inspire_.fetchTitlesByRecids(batch, [this, batch](const QHash<int, QString> &titles, const QString &error) {
        fetchingReferenceTitles_ = false;
        if (!error.isEmpty()) {
            for (int recid : batch)
                requestedReferenceTitles_.remove(recid);
            status_->setText(error);
        } else {
            for (auto it = titles.cbegin(); it != titles.cend(); ++it)
                resolvedReferenceTitles_.insert(it.key(), it.value());
            for (auto it = nodes_.begin(); it != nodes_.end(); ++it) {
                GraphPaper &paper = it.value();
                if (!paper.titleNeedsResolution || !titles.contains(paper.inspireRecid))
                    continue;
                paper.title = titles.value(paper.inspireRecid);
                paper.titleNeedsResolution = false;
                for (QGraphicsItem *item : scene_->items()) {
                    if (auto *node = dynamic_cast<GraphNodeItem *>(item); node && node->key() == paper.key)
                        node->setTitle(shortTitle(paper.title), paper.title);
                }
            }
        }
        fetchPendingReferenceTitles();
    });
}

void CitationGraphDialog::addCitingChunk(const QString &sourceKey)
{
    if (!nodes_.contains(sourceKey))
        return;
    const GraphPaper parent = nodes_.value(sourceKey);
    const QList<RelatedPaperRecord> papers = citingCache_.value(sourceKey);
    const int start = qBound(0, parent.citingCursor, papers.size());
    const int end = qMin(start + neighborLimit_->value(), papers.size());

    for (int i = start; i < end; ++i) {
        const RelatedPaperRecord &related = papers.at(i);
        GraphPaper child;
        child.inspireRecid = related.inspireRecid;
        child.arxivId = related.arxivId;
        child.title = related.title;
        child.authors = related.authors;
        child.doi = related.doi;
        child.year = related.year;
        child.citationCount = related.citationCount;
        child.local = related.local;
        child.localPath = related.localPath;
        if (child.local && !child.arxivId.isEmpty())
            child.localArxivId = child.arxivId;
        const QString childKey = addOrMergeNode(child, parent.depth + 1);
        if (!childKey.isEmpty())
            addEdge(sourceKey, childKey);
    }
    nodes_[sourceKey].citingCursor = end;
}

QString CitationGraphDialog::nodeSubtitle(const GraphPaper &paper) const
{
    QStringList parts;
    if (!paper.year.isEmpty())
        parts << paper.year;
    if (paper.local)
        parts << QStringLiteral("LOCAL");
    else if (!paper.arxivId.isEmpty())
        parts << QStringLiteral("arXiv:%1").arg(paper.arxivId);
    else if (paper.inspireRecid > 0)
        parts << QStringLiteral("INSPIRE:%1").arg(paper.inspireRecid);

    if (paper.referencesFetched) {
        const int total = referenceCache_.value(paper.key).size();
        parts << QStringLiteral("refs %1/%2").arg(paper.referenceCursor).arg(total);
    }
    if (paper.citingFetched) {
        const int fetched = citingCache_.value(paper.key).size();
        const int total = paper.totalCiting >= 0 ? paper.totalCiting : fetched;
        parts << QStringLiteral("cited %1/%2").arg(paper.citingCursor).arg(total);
    }
    return parts.join(QStringLiteral(" · "));
}

void CitationGraphDialog::rebuildGraph(bool fitView, const QString &focusKey)
{
    scene_->clear();
    if (nodes_.isEmpty() || rootKey_.isEmpty()) {
        summary_->setText(QStringLiteral("No graph data"));
        return;
    }

    QSet<QString> visible;
    int localCount = 0;
    int minDepth = 0;
    int maxDepth = 0;
    for (auto it = nodes_.cbegin(); it != nodes_.cend(); ++it) {
        if (it.value().local)
            ++localCount;
        if (!localOnly_->isChecked() || it.value().local || it.key() == rootKey_) {
            visible.insert(it.key());
            minDepth = qMin(minDepth, it.value().depth);
            maxDepth = qMax(maxDepth, it.value().depth);
        }
    }

    QMap<int, QList<QString>> columns;
    for (const QString &key : visible)
        columns[nodes_.value(key).depth].append(key);
    for (auto it = columns.begin(); it != columns.end(); ++it) {
        std::sort(it.value().begin(), it.value().end(), [this](const QString &a, const QString &b) {
            const GraphPaper pa = nodes_.value(a);
            const GraphPaper pb = nodes_.value(b);
            if (a == rootKey_)
                return true;
            if (b == rootKey_)
                return false;
            if (pa.year != pb.year)
                return pa.year < pb.year;
            return pa.title.toLower() < pb.title.toLower();
        });
    }

    const qreal xSpacing = 385.0;
    const qreal ySpacing = 112.0;
    QHash<QString, QPointF> positions;
    qreal topY = 0.0;
    bool haveTop = false;

    for (auto it = columns.cbegin(); it != columns.cend(); ++it) {
        const int depth = it.key();
        const QList<QString> keys = it.value();
        const qreal startY = keys.isEmpty() ? 0.0 : -((keys.size() - 1) * ySpacing) / 2.0;
        for (int i = 0; i < keys.size(); ++i) {
            const QPointF pos(depth * xSpacing, startY + i * ySpacing);
            positions.insert(keys.at(i), pos);
            if (!haveTop || pos.y() < topY) {
                topY = pos.y();
                haveTop = true;
            }
        }
    }

    for (const GraphEdge &edge : edges_) {
        if (!visible.contains(edge.from) || !visible.contains(edge.to))
            continue;
        addArrow(scene_, positions.value(edge.from), positions.value(edge.to));
    }

    for (auto it = columns.cbegin(); it != columns.cend(); ++it) {
        const qreal x = it.key() * xSpacing - NodeHalfWidth;
        addSectionLabel(scene_, depthLabel(it.key()), QPointF(x, topY - 92.0));
    }

    for (const QString &key : visible) {
        const GraphPaper paper = nodes_.value(key);
        auto *node = new GraphNodeItem(
            key,
            shortTitle(paper.title),
            nodeSubtitle(paper),
            paper.local,
            key == rootKey_,
            [this, key]() { openNode(key); },
            [this, key](const QPoint &pos) { showNodeMenu(key, pos); });
        node->setToolTip(paper.title + (nodeSubtitle(paper).isEmpty()
                                              ? QString() : QStringLiteral("\n") + nodeSubtitle(paper)));
        node->setPos(positions.value(key));
        node->setZValue(2);
        scene_->addItem(node);
    }

    const int hidden = nodes_.size() - visible.size();
    summary_->setText(QStringLiteral("%1 papers in graph · %2 local%3 · depth %4…+%5 · arrows point from cited work to later citing work")
                          .arg(nodes_.size())
                          .arg(localCount)
                          .arg(hidden > 0 ? QStringLiteral(" · %1 hidden").arg(hidden) : QString())
                          .arg(minDepth)
                          .arg(maxDepth));

    auto *legend = scene_->addText(QStringLiteral("Right-click = expand · double-click = open · thicker outline = local PDF · manual expansion prevents graph explosion"));
    legend->setDefaultTextColor(QApplication::palette().mid().color());
    legend->setPos((minDepth * xSpacing),
                   scene_->itemsBoundingRect().bottom() + 55.0);

    scene_->setSceneRect(scene_->itemsBoundingRect().adjusted(-90, -90, 90, 90));
    if (fitView && !scene_->items().isEmpty()) {
        view_->fitInView(scene_->sceneRect(), Qt::KeepAspectRatio);
    } else if (!focusKey.isEmpty() && positions.contains(focusKey)) {
        view_->centerOn(positions.value(focusKey));
    }
}

void CitationGraphDialog::expandNode(const QString &key, bool references, bool citedBy)
{
    if (expanding_ || refreshingRoot_ || !nodes_.contains(key))
        return;
    if (!references && !citedBy)
        return;

    expanding_ = true;
    expandButton_->setEnabled(false);
    resetButton_->setEnabled(false);
    status_->setText(QStringLiteral("Checking cached citation data…"));

    hydrateLocalCaches(key);
    const GraphPaper paper = nodes_.value(key);
    const bool needsPaper = (references && !paper.referencesFetched)
                         || (citedBy && paper.inspireRecid <= 0);
    if (needsPaper) {
        fetchPaperForExpansion(key, references, citedBy);
        return;
    }
    continueExpansionAfterPaper(key, references, citedBy);
}

void CitationGraphDialog::fetchPaperForExpansion(const QString &key, bool references, bool citedBy)
{
    if (!nodes_.contains(key)) {
        finishExpansion(QStringLiteral("The selected graph node no longer exists."));
        return;
    }
    const GraphPaper paper = nodes_.value(key);
    status_->setText(QStringLiteral("Fetching references/record data from INSPIRE…"));

    auto callback = [this, key, references, citedBy](const InspirePaperData &data, const QString &error) {
        if (!error.isEmpty()) {
            finishExpansion(error, key);
            return;
        }
        if (!nodes_.contains(key)) {
            finishExpansion(QStringLiteral("The expanded node disappeared from the graph."));
            return;
        }

        GraphPaper &node = nodes_[key];
        if (data.metrics.inspireRecid > 0)
            node.inspireRecid = data.metrics.inspireRecid;
        node.citationCount = data.metrics.citationCount;
        referenceCache_.insert(key, data.references);
        node.referencesFetched = true;

        if (node.local && !node.localArxivId.isEmpty()) {
            QString dbError;
            if (db_->updateCitationMetrics(node.localArxivId, data.metrics, &dbError)
                && db_->replaceReferences(node.localArxivId, data.references, &dbError)) {
                dataChanged_ = true;
            } else if (!dbError.isEmpty()) {
                status_->setText(dbError);
            }
        }
        continueExpansionAfterPaper(key, references, citedBy);
    };

    if (!paper.arxivId.isEmpty())
        inspire_.fetchPaper(paper.arxivId, std::move(callback));
    else if (paper.inspireRecid > 0)
        inspire_.fetchPaperByRecid(paper.inspireRecid, std::move(callback));
    else
        finishExpansion(QStringLiteral("This node has neither an arXiv ID nor an INSPIRE record ID, so it cannot be expanded automatically."), key);
}

void CitationGraphDialog::continueExpansionAfterPaper(const QString &key, bool references, bool citedBy)
{
    if (!nodes_.contains(key)) {
        finishExpansion(QStringLiteral("The selected graph node no longer exists."));
        return;
    }

    const GraphPaper paper = nodes_.value(key);
    if (citedBy) {
        const int cached = citingCache_.value(key).size();
        const bool needFirstFetch = !paper.citingFetched;
        const bool exhaustedCached = paper.citingFetched && paper.citingCursor >= cached;
        const bool moreExist = paper.totalCiting > cached;
        const bool canFetchMore = cached < MaxCitingFetch;
        if (needFirstFetch || (exhaustedCached && moreExist && canFetchMore)) {
            if (paper.inspireRecid <= 0) {
                if (references)
                    addReferenceChunk(key);
                finishExpansion(QStringLiteral("References expanded, but this paper has no INSPIRE record ID for a cited-by query."), key);
                return;
            }
            fetchCitingForExpansion(key, references);
            return;
        }
    }

    if (references)
        addReferenceChunk(key);
    if (citedBy)
        addCitingChunk(key);

    finishExpansion(QStringLiteral("Expanded citation neighborhood. Right-click the node again to reveal the next batch."), key);
}

void CitationGraphDialog::fetchCitingForExpansion(const QString &key, bool referencesAlreadyHandled)
{
    if (!nodes_.contains(key)) {
        finishExpansion(QStringLiteral("The selected graph node no longer exists."));
        return;
    }
    const GraphPaper paper = nodes_.value(key);
    const int cached = citingCache_.value(key).size();
    const int requested = cached <= 0
                            ? InitialCitingFetch
                            : qMin(MaxCitingFetch, cached + CitingFetchStep);
    status_->setText(QStringLiteral("Fetching up to %1 cited-by papers from INSPIRE…").arg(requested));

    inspire_.fetchCitingPapers(paper.inspireRecid, requested,
        [this, key, referencesAlreadyHandled](const InspireCitingData &data, const QString &error) {
            if (!error.isEmpty()) {
                if (referencesAlreadyHandled)
                    addReferenceChunk(key);
                finishExpansion(error, key);
                return;
            }
            if (!nodes_.contains(key)) {
                finishExpansion(QStringLiteral("The expanded node disappeared from the graph."));
                return;
            }

            GraphPaper &node = nodes_[key];
            citingCache_.insert(key, data.papers);
            node.citingFetched = true;
            node.totalCiting = data.totalCount;
            node.citingCursor = qMin(node.citingCursor, data.papers.size());

            if (node.local && !node.localArxivId.isEmpty()) {
                QString dbError;
                if (db_->replaceCitingPapers(node.localArxivId, data.papers, data.totalCount, &dbError))
                    dataChanged_ = true;
                else if (!dbError.isEmpty())
                    status_->setText(dbError);
            }

            if (referencesAlreadyHandled)
                addReferenceChunk(key);
            addCitingChunk(key);
            finishExpansion(QStringLiteral("Expanded citation neighborhood. Right-click again to reveal more."), key);
        });
}

void CitationGraphDialog::finishExpansion(const QString &message, const QString &focusKey)
{
    expanding_ = false;
    expandButton_->setEnabled(true);
    resetButton_->setEnabled(true);
    status_->setText(message);
    rebuildGraph(false, focusKey);
}

void CitationGraphDialog::showNodeMenu(const QString &key, const QPoint &globalPos)
{
    if (!nodes_.contains(key))
        return;
    hydrateLocalCaches(key);
    const GraphPaper paper = nodes_.value(key);
    const int refsCached = referenceCache_.value(key).size();
    const int citedCached = citingCache_.value(key).size();

    QMenu menu(this);
    QAction *heading = menu.addAction(shortTitle(paper.title, 90));
    heading->setEnabled(false);
    menu.addSeparator();

    QAction *open = menu.addAction(paper.local ? QStringLiteral("Open local PDF") : QStringLiteral("Open on arXiv / INSPIRE"));
    connect(open, &QAction::triggered, this, [this, key]() { openNode(key); });
    QAction *addTrail = menu.addAction(QStringLiteral("Add paper to literature trail…"));
    connect(addTrail, &QAction::triggered, this, [this, key]() { addNodeToTrail(key); });
    menu.addSeparator();

    QString refsText;
    bool refsEnabled = true;
    if (!paper.referencesFetched)
        refsText = QStringLiteral("Expand references");
    else if (paper.referenceCursor < refsCached)
        refsText = QStringLiteral("Show next references (%1 cached remaining)").arg(refsCached - paper.referenceCursor);
    else {
        refsText = QStringLiteral("All %1 references shown").arg(refsCached);
        refsEnabled = false;
    }
    QAction *refsAction = menu.addAction(refsText);
    refsAction->setEnabled(refsEnabled && !expanding_);
    connect(refsAction, &QAction::triggered, this, [this, key]() { expandNode(key, true, false); });

    QString citingText;
    bool citingEnabled = true;
    if (!paper.citingFetched) {
        citingText = QStringLiteral("Expand cited-by papers");
    } else if (paper.citingCursor < citedCached) {
        citingText = QStringLiteral("Show next cited-by (%1 cached remaining)").arg(citedCached - paper.citingCursor);
    } else if (paper.totalCiting > citedCached && citedCached < MaxCitingFetch) {
        citingText = QStringLiteral("Fetch more cited-by from INSPIRE (%1 total)").arg(paper.totalCiting);
    } else {
        const int total = paper.totalCiting >= 0 ? paper.totalCiting : citedCached;
        citingText = QStringLiteral("Cited-by set shown (%1)").arg(total);
        citingEnabled = false;
    }
    QAction *citingAction = menu.addAction(citingText);
    citingAction->setEnabled(citingEnabled && !expanding_);
    connect(citingAction, &QAction::triggered, this, [this, key]() { expandNode(key, false, true); });

    QAction *both = menu.addAction(QStringLiteral("Expand both directions"));
    both->setEnabled((refsEnabled || citingEnabled) && !expanding_);
    connect(both, &QAction::triggered, this, [this, key]() { expandNode(key, true, true); });

    menu.addSeparator();
    QAction *external = menu.addAction(QStringLiteral("Open external record"));
    external->setEnabled(!paper.arxivId.isEmpty() || paper.inspireRecid > 0);
    connect(external, &QAction::triggered, this, [this, key]() { openExternalNode(key); });

    QAction *fit = menu.addAction(QStringLiteral("Fit whole graph"));
    connect(fit, &QAction::triggered, this, [this]() { rebuildGraph(true); });
    if (key != rootKey_) {
        QAction *reset = menu.addAction(QStringLiteral("Reset graph to root"));
        connect(reset, &QAction::triggered, this, [this]() { resetGraph(); });
    }

    menu.exec(globalPos);
}

TrailItemRecord CitationGraphDialog::trailItemForNode(const QString &key) const
{
    TrailItemRecord item;
    if (!nodes_.contains(key))
        return item;
    const GraphPaper paper = nodes_.value(key);
    item.arxivId = paper.arxivId;
    item.inspireRecid = paper.inspireRecid;
    item.doi = paper.doi;
    item.title = paper.title;
    item.authors = paper.authors;
    item.year = paper.year;
    item.local = paper.local;
    item.localArxivId = paper.localArxivId;
    item.localPath = paper.localPath;
    return item;
}

void CitationGraphDialog::addNodeToTrail(const QString &key)
{
    if (!db_ || !nodes_.contains(key))
        return;

    QString error;
    const QList<LiteratureTrailRecord> trails = db_->literatureTrails(&error);
    if (!error.isEmpty()) {
        QMessageBox::warning(this, QStringLiteral("Literature trails"), error);
        return;
    }

    QStringList choices;
    choices << QStringLiteral("+ New literature trail…");
    for (const LiteratureTrailRecord &trail : trails)
        choices << trail.name;

    bool ok = false;
    const QString choice = QInputDialog::getItem(this, QStringLiteral("Add to literature trail"),
                                                 QStringLiteral("Choose a trail:"), choices,
                                                 0, false, &ok);
    if (!ok || choice.isEmpty())
        return;

    int trailId = 0;
    if (choice.startsWith(QLatin1Char('+'))) {
        const QString name = QInputDialog::getText(this, QStringLiteral("New literature trail"),
                                                   QStringLiteral("Trail name:"), QLineEdit::Normal,
                                                   QString(), &ok).trimmed();
        if (!ok || name.isEmpty())
            return;
        trailId = db_->createLiteratureTrail(name, {}, &error);
    } else {
        for (const LiteratureTrailRecord &trail : trails) {
            if (trail.name.compare(choice, Qt::CaseInsensitive) == 0) {
                trailId = trail.id;
                break;
            }
        }
    }

    if (trailId <= 0 || !error.isEmpty()) {
        QMessageBox::warning(this, QStringLiteral("Literature trails"),
                             error.isEmpty() ? QStringLiteral("Could not select a literature trail.") : error);
        return;
    }

    if (!db_->addTrailItem(trailId, trailItemForNode(key), &error)) {
        QMessageBox::warning(this, QStringLiteral("Literature trails"), error);
        return;
    }
    dataChanged_ = true;
    status_->setText(QStringLiteral("Paper added to the literature trail."));
}

void CitationGraphDialog::saveExplorationAsTrail()
{
    if (!db_ || explorationOrder_.isEmpty())
        return;

    bool ok = false;
    const QString name = QInputDialog::getText(this, QStringLiteral("Save exploration as literature trail"),
                                               QStringLiteral("Trail name:"), QLineEdit::Normal,
                                               QString(), &ok).trimmed();
    if (!ok || name.isEmpty())
        return;

    QString error;
    const QList<LiteratureTrailRecord> existing = db_->literatureTrails(&error);
    int trailId = 0;
    for (const LiteratureTrailRecord &trail : existing) {
        if (trail.name.compare(name, Qt::CaseInsensitive) == 0) {
            if (QMessageBox::question(this, QStringLiteral("Existing trail"),
                                      QStringLiteral("A trail named “%1” already exists. Append this exploration to it?").arg(trail.name))
                != QMessageBox::Yes)
                return;
            trailId = trail.id;
            break;
        }
    }
    if (trailId <= 0)
        trailId = db_->createLiteratureTrail(name,
                                             QStringLiteral("Saved from a HEPShelf citation-network exploration."),
                                             &error);
    if (trailId <= 0 || !error.isEmpty()) {
        QMessageBox::warning(this, QStringLiteral("Literature trails"), error);
        return;
    }

    int saved = 0;
    for (const QString &key : explorationOrder_) {
        if (!nodes_.contains(key))
            continue;
        const TrailItemRecord item = trailItemForNode(key);
        if (db_->addTrailItem(trailId, item, &error))
            ++saved;
        else
            break;
    }
    if (!error.isEmpty()) {
        QMessageBox::warning(this, QStringLiteral("Literature trails"), error);
        return;
    }
    dataChanged_ = true;
    status_->setText(QStringLiteral("Saved %1 explored paper%2 to literature trail “%3”.")
                         .arg(saved)
                         .arg(saved == 1 ? QString() : QStringLiteral("s"))
                         .arg(name));
}

QString CitationGraphDialog::selectedNodeKey() const
{
    if (!scene_)
        return {};
    const QList<QGraphicsItem *> selected = scene_->selectedItems();
    for (QGraphicsItem *item : selected) {
        if (auto *node = dynamic_cast<GraphNodeItem *>(item))
            return node->key();
    }
    return {};
}

void CitationGraphDialog::openNode(const QString &key)
{
    if (!nodes_.contains(key))
        return;
    const GraphPaper paper = nodes_.value(key);
    if (paper.local && !paper.localPath.isEmpty() && openLocal_) {
        const QString localId = !paper.localArxivId.isEmpty() ? paper.localArxivId : paper.arxivId;
        if (!localId.isEmpty()) {
            openLocal_(localId, paper.localPath);
            accept();
            return;
        }
    }
    openExternalNode(key);
}

void CitationGraphDialog::openExternalNode(const QString &key)
{
    if (!nodes_.contains(key))
        return;
    const GraphPaper paper = nodes_.value(key);
    if (!paper.arxivId.isEmpty()) {
        QDesktopServices::openUrl(QUrl(QStringLiteral("https://arxiv.org/abs/%1").arg(paper.arxivId)));
        return;
    }
    if (paper.inspireRecid > 0)
        QDesktopServices::openUrl(QUrl(QStringLiteral("https://inspirehep.net/literature/%1").arg(paper.inspireRecid)));
}

void CitationGraphDialog::refreshRootFromInspire()
{
    if (refreshingRoot_ || expanding_ || !db_ || rootKey_.isEmpty())
        return;
    refreshingRoot_ = true;
    refreshButton_->setEnabled(false);
    expandButton_->setEnabled(false);
    resetButton_->setEnabled(false);
    status_->setText(QStringLiteral("Refreshing root references and citation metrics from INSPIRE…"));

    inspire_.fetchPaper(sourceArxivId_, [this](const InspirePaperData &data, const QString &error) {
        if (!error.isEmpty()) {
            refreshingRoot_ = false;
            refreshButton_->setEnabled(true);
            expandButton_->setEnabled(true);
            resetButton_->setEnabled(true);
            status_->setText(error);
            QMessageBox::warning(this, QStringLiteral("INSPIRE"), error);
            return;
        }

        QString dbError;
        if (!db_->updateCitationMetrics(sourceArxivId_, data.metrics, &dbError)
            || !db_->replaceReferences(sourceArxivId_, data.references, &dbError)) {
            refreshingRoot_ = false;
            refreshButton_->setEnabled(true);
            expandButton_->setEnabled(true);
            resetButton_->setEnabled(true);
            status_->setText(dbError);
            QMessageBox::warning(this, QStringLiteral("HEPShelf"), dbError);
            return;
        }
        dataChanged_ = true;
        fetchRootCiting(data.metrics.inspireRecid);
    });
}

void CitationGraphDialog::fetchRootCiting(int recid)
{
    if (recid <= 0) {
        refreshingRoot_ = false;
        refreshButton_->setEnabled(true);
        expandButton_->setEnabled(true);
        resetButton_->setEnabled(true);
        resetGraph();
        status_->setText(QStringLiteral("References refreshed. INSPIRE did not provide a record ID for cited-by lookup."));
        return;
    }

    status_->setText(QStringLiteral("Refreshing root cited-by papers from INSPIRE…"));
    inspire_.fetchCitingPapers(recid, InitialCitingFetch, [this](const InspireCitingData &data, const QString &error) {
        refreshingRoot_ = false;
        refreshButton_->setEnabled(true);
        expandButton_->setEnabled(true);
        resetButton_->setEnabled(true);
        if (!error.isEmpty()) {
            status_->setText(error);
            QMessageBox::warning(this, QStringLiteral("INSPIRE"), error);
            resetGraph();
            return;
        }

        QString dbError;
        if (!db_->replaceCitingPapers(sourceArxivId_, data.papers, data.totalCount, &dbError)) {
            status_->setText(dbError);
            QMessageBox::warning(this, QStringLiteral("HEPShelf"), dbError);
            return;
        }
        dataChanged_ = true;
        resetGraph();
        status_->setText(QStringLiteral("Root citation data refreshed. Right-click any node to continue exploring."));
    });
}
