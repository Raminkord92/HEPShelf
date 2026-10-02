#include "selectable_pdf_view.h"

#include <QApplication>
#include <QClipboard>
#include <QContextMenuEvent>
#include <QGuiApplication>
#include <QKeyEvent>
#include <QMenu>
#include <QMouseEvent>
#include <QPainter>
#include <QPdfDocument>
#include <QPdfPageNavigator>
#include <QScreen>
#include <QScrollBar>
#include <QToolTip>

SelectablePdfView::SelectablePdfView(QWidget *parent) : QPdfView(parent)
{
    setFocusPolicy(Qt::StrongFocus);
    viewport()->setMouseTracking(true);
}

void SelectablePdfView::setHighlights(const QList<PaperHighlightRecord> &highlights)
{
    highlights_ = highlights;
    viewport()->update();
}

void SelectablePdfView::setSearchResults(const QList<QPdfLink> &results, int currentIndex)
{
    searchResults_ = results;
    currentSearchIndex_ = currentIndex;
    viewport()->update();
}

void SelectablePdfView::scrollToSearchResult(const QPdfLink &result)
{
    if (!result.isValid() || result.page() < 0 || result.rectangles().isEmpty()) return;
    pageNavigator()->jump(result.page(), {}, 0);
    qreal scale = 1;
    const QRectF page = pageRect(result.page(), &scale);
    const QPointF target = page.topLeft() + result.rectangles().first().topLeft() * scale;
    verticalScrollBar()->setValue(qMax(0, int(target.y() - viewport()->height() / 3)));
    horizontalScrollBar()->setValue(qMax(0, int(target.x() - viewport()->width() / 3)));
    viewport()->update();
}

void SelectablePdfView::setHighlightActions(std::function<void()> add, std::function<void(int)> remove)
{
    addHighlight_ = std::move(add);
    removeHighlight_ = std::move(remove);
}

QRectF SelectablePdfView::pageRect(int page, qreal *scale) const
{
    if (!document() || document()->status() != QPdfDocument::Status::Ready || page < 0 || page >= document()->pageCount())
        return {};
    const qreal dpi = QGuiApplication::primaryScreen()->logicalDotsPerInch() / 72.0;
    const auto sizeFor = [this, dpi](int index) {
        QSize base = QSizeF(document()->pagePointSize(index) * dpi).toSize();
        if (base.isEmpty()) return QSize();
        if (zoomMode() == ZoomMode::Custom) return QSizeF(document()->pagePointSize(index) * dpi * zoomFactor()).toSize();
        if (zoomMode() == ZoomMode::FitToWidth)
            return QSizeF(base).toSize() * (qreal(viewport()->width() - documentMargins().left() - documentMargins().right()) / base.width());
        return base.scaled(viewport()->size() - QSize(documentMargins().left() + documentMargins().right(), pageSpacing()), Qt::KeepAspectRatio);
    };
    int width = 0;
    for (int i = 0; i < document()->pageCount(); ++i)
        width = qMax(width, sizeFor(i).width());
    const QSize size = sizeFor(page);
    int y = documentMargins().top();
    for (int i = 0; i < page; ++i)
        y += sizeFor(i).height() + pageSpacing();
    const int totalWidth = width + documentMargins().left() + documentMargins().right();
    const int x = (qMax(totalWidth, viewport()->width()) - size.width()) / 2;
    if (scale) *scale = qreal(size.width()) / document()->pagePointSize(page).width();
    return QRectF(x, y, size.width(), size.height());
}

SelectablePdfView::PageHit SelectablePdfView::hitAt(const QPoint &position, bool clampToPage) const
{
    PageHit hit;
    if (!document() || document()->status() != QPdfDocument::Status::Ready) return hit;
    const QPointF documentPos = QPointF(position) + QPointF(horizontalScrollBar()->value(), verticalScrollBar()->value());
    const qreal dpi = QGuiApplication::primaryScreen()->logicalDotsPerInch() / 72.0;
    QVector<QSize> sizes;
    sizes.reserve(document()->pageCount());
    int maxWidth = 0;
    for (int page = 0; page < document()->pageCount(); ++page) {
        const QSize base = QSizeF(document()->pagePointSize(page) * dpi).toSize();
        QSize size;
        if (zoomMode() == ZoomMode::Custom)
            size = QSizeF(document()->pagePointSize(page) * dpi * zoomFactor()).toSize();
        else if (zoomMode() == ZoomMode::FitToWidth && base.width() > 0)
            size = QSizeF(base * (qreal(viewport()->width() - documentMargins().left() - documentMargins().right()) / base.width())).toSize();
        else
            size = base.scaled(viewport()->size() - QSize(documentMargins().left() + documentMargins().right(), pageSpacing()), Qt::KeepAspectRatio);
        sizes << size;
        maxWidth = qMax(maxWidth, size.width());
    }
    int y = documentMargins().top();
    const int totalWidth = maxWidth + documentMargins().left() + documentMargins().right();
    for (int page = 0; page < sizes.size(); ++page) {
        const QSize size = sizes.at(page);
        const QRectF rect((qMax(totalWidth, viewport()->width()) - size.width()) / 2, y, size.width(), size.height());
        const qreal scale = qreal(size.width()) / document()->pagePointSize(page).width();
        if (!rect.contains(documentPos)) {
            y += size.height() + pageSpacing();
            continue;
        }
        hit.page = page;
        hit.rect = rect;
        hit.scale = scale;
        hit.point = (documentPos - rect.topLeft()) / scale;
        return hit;
    }
    if (clampToPage && anchor_.page >= 0) {
        hit.page = anchor_.page;
        hit.rect = pageRect(hit.page, &hit.scale);
        QPointF p = documentPos - hit.rect.topLeft();
        p.setX(qBound<qreal>(0, p.x(), hit.rect.width()));
        p.setY(qBound<qreal>(0, p.y(), hit.rect.height()));
        hit.point = p / hit.scale;
    }
    return hit;
}

void SelectablePdfView::updateSelection(const QPoint &position)
{
    const PageHit end = hitAt(position, true);
    if (anchor_.page < 0 || end.page < 0) return;
    selections_.clear();
    if (end.page == anchor_.page) {
        const QPdfSelection selection = document()->getSelection(end.page, anchor_.point, end.point);
        if (!selection.text().isEmpty()) selections_.append({end.page, selection});
    } else {
        const bool forward = end.page > anchor_.page;
        const int first = qMin(end.page, anchor_.page);
        const int last = qMax(end.page, anchor_.page);
        for (int page = first; page <= last; ++page) {
            const QSizeF size = document()->pagePointSize(page);
            const QPointF start = page == anchor_.page ? anchor_.point : (forward ? QPointF(0, 0) : QPointF(size.width(), size.height()));
            const QPointF finish = page == end.page ? end.point : (forward ? QPointF(size.width(), size.height()) : QPointF(0, 0));
            const QPdfSelection selection = document()->getSelection(page, start, finish);
            if (!selection.text().isEmpty()) selections_.append({page, selection});
        }
    }
    emit selectionChanged(!selections_.isEmpty());
    viewport()->update();
}

QList<PaperHighlightRecord> SelectablePdfView::selectedRanges() const
{
    QList<PaperHighlightRecord> ranges;
    for (const auto &entry : selections_) {
        const QPdfSelection &selection = entry.second;
        ranges << PaperHighlightRecord{0, entry.first, selection.startIndex(), selection.endIndex() - selection.startIndex(), selection.text()};
    }
    return ranges;
}

QString SelectablePdfView::selectedText() const
{
    QStringList pieces;
    for (const auto &entry : selections_) pieces << entry.second.text();
    return pieces.join(QLatin1Char('\n'));
}

void SelectablePdfView::clearSelection()
{
    selections_.clear();
    emit selectionChanged(false);
    viewport()->update();
}

void SelectablePdfView::copySelection()
{
    if (!selections_.isEmpty()) QApplication::clipboard()->setText(selectedText());
}

void SelectablePdfView::drawSelection(QPainter &painter, int page, const QPdfSelection &selection, const QColor &color)
{
    if (selection.text().isEmpty()) return;
    qreal scale = 1;
    const QRectF rect = pageRect(page, &scale);
    const QPointF origin = rect.topLeft() - QPointF(horizontalScrollBar()->value(), verticalScrollBar()->value());
    painter.setPen(Qt::NoPen);
    painter.setBrush(color);
    for (const QPolygonF &polygon : selection.bounds()) {
        QPolygonF mapped;
        for (const QPointF &point : polygon) mapped << origin + point * scale;
        painter.drawPolygon(mapped);
    }
}

void SelectablePdfView::paintEvent(QPaintEvent *event)
{
    QPdfView::paintEvent(event);
    if (!document() || document()->status() != QPdfDocument::Status::Ready) return;
    QPainter painter(viewport());
    for (const auto &highlight : highlights_) {
        if (highlight.page < 0 || highlight.page >= document()->pageCount() || highlight.length <= 0) continue;
        const QRectF visible = pageRect(highlight.page).translated(-horizontalScrollBar()->value(), -verticalScrollBar()->value());
        if (!visible.intersects(viewport()->rect())) continue;
        const QPdfSelection selection = document()->getSelectionAtIndex(highlight.page, highlight.startIndex, highlight.length);
        if (selection.text() == highlight.text)
            drawSelection(painter, highlight.page, selection, QColor(255, 207, 55, 105));
    }
    for (int index = 0; index < searchResults_.size(); ++index) {
        const QPdfLink &result = searchResults_.at(index);
        if (result.page() < 0 || result.page() >= document()->pageCount()) continue;
        qreal scale = 1;
        const QRectF page = pageRect(result.page(), &scale);
        const QPointF origin = page.topLeft() - QPointF(horizontalScrollBar()->value(), verticalScrollBar()->value());
        if (!page.translated(-horizontalScrollBar()->value(), -verticalScrollBar()->value()).intersects(viewport()->rect())) continue;
        for (const QRectF &rect : result.rectangles()) {
            const QRectF mapped(origin + rect.topLeft() * scale, rect.size() * scale);
            painter.fillRect(mapped, index == currentSearchIndex_ ? QColor(255, 143, 48, 150) : QColor(255, 229, 89, 105));
            if (index == currentSearchIndex_) {
                painter.setPen(QPen(QColor(214, 93, 20), 1.5));
                painter.setBrush(Qt::NoBrush);
                painter.drawRect(mapped);
            }
        }
    }
    for (const auto &entry : selections_)
        drawSelection(painter, entry.first, entry.second, QColor(47, 131, 231, 110));
}

void SelectablePdfView::mousePressEvent(QMouseEvent *event)
{
    if (event->button() == Qt::LeftButton) {
        setFocus();
        anchor_ = hitAt(event->pos());
        pressPosition_ = event->pos();
        dragging_ = false;
        if (anchor_.page >= 0) {
            clearSelection();
            event->accept();
            return;
        }
    }
    QPdfView::mousePressEvent(event);
}

void SelectablePdfView::mouseMoveEvent(QMouseEvent *event)
{
    if ((event->buttons() & Qt::LeftButton) && anchor_.page >= 0) {
        if (!dragging_ && (event->pos() - pressPosition_).manhattanLength() >= QApplication::startDragDistance()) dragging_ = true;
        if (dragging_) updateSelection(event->pos());
        event->accept();
        return;
    }
    QPdfView::mouseMoveEvent(event);
    if (hitAt(event->pos()).page >= 0 && cursor().shape() != Qt::PointingHandCursor)
        viewport()->setCursor(Qt::IBeamCursor);
}

void SelectablePdfView::mouseReleaseEvent(QMouseEvent *event)
{
    if (event->button() == Qt::LeftButton && anchor_.page >= 0) {
        if (dragging_) {
            updateSelection(event->pos());
            if (selections_.isEmpty() && document()->getAllText(anchor_.page).text().trimmed().isEmpty())
                QToolTip::showText(event->globalPosition().toPoint(), QStringLiteral("No selectable text here. Scanned PDFs need OCR."), viewport());
            event->accept();
        } else {
            QPdfView::mouseReleaseEvent(event);
        }
        anchor_ = {};
        dragging_ = false;
        return;
    }
    QPdfView::mouseReleaseEvent(event);
}

void SelectablePdfView::keyPressEvent(QKeyEvent *event)
{
    if (event->matches(QKeySequence::Copy) && !selections_.isEmpty()) {
        copySelection();
        event->accept();
        return;
    }
    if (event->key() == Qt::Key_Escape && !selections_.isEmpty()) {
        clearSelection();
        event->accept();
        return;
    }
    QPdfView::keyPressEvent(event);
}

void SelectablePdfView::contextMenuEvent(QContextMenuEvent *event)
{
    QMenu menu(this);
    auto *copy = menu.addAction(QStringLiteral("Copy selected text"));
    copy->setEnabled(!selections_.isEmpty());
    auto *highlight = menu.addAction(QStringLiteral("Highlight selection"));
    highlight->setEnabled(!selections_.isEmpty() && bool(addHighlight_));
    int removeId = 0;
    const PageHit hit = hitAt(event->pos());
    if (hit.page >= 0) {
        for (const auto &record : highlights_) {
            if (record.page != hit.page) continue;
            const QPdfSelection selection = document()->getSelectionAtIndex(record.page, record.startIndex, record.length);
            for (const QPolygonF &polygon : selection.bounds())
                if (polygon.containsPoint(hit.point, Qt::OddEvenFill)) { removeId = record.id; break; }
            if (removeId) break;
        }
    }
    auto *remove = menu.addAction(QStringLiteral("Remove highlight"));
    remove->setEnabled(removeId && bool(removeHighlight_));
    QAction *chosen = menu.exec(event->globalPos());
    if (chosen == copy) copySelection();
    else if (chosen == highlight) addHighlight_();
    else if (chosen == remove) removeHighlight_(removeId);
}
