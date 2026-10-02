#pragma once

#include "database.h"
#include <QPdfSelection>
#include <QPdfLink>
#include <QPdfView>
#include <functional>

class SelectablePdfView : public QPdfView {
    Q_OBJECT
public:
    explicit SelectablePdfView(QWidget *parent = nullptr);
    void setHighlights(const QList<PaperHighlightRecord> &highlights);
    QList<PaperHighlightRecord> selectedRanges() const;
    QString selectedText() const;
    void clearSelection();
    void copySelection();
    void setHighlightActions(std::function<void()> add, std::function<void(int)> remove);
    void setSearchResults(const QList<QPdfLink> &results, int currentIndex);
    void scrollToSearchResult(const QPdfLink &result);

signals:
    void selectionChanged(bool hasSelection);

protected:
    void paintEvent(QPaintEvent *event) override;
    void mousePressEvent(QMouseEvent *event) override;
    void mouseMoveEvent(QMouseEvent *event) override;
    void mouseReleaseEvent(QMouseEvent *event) override;
    void keyPressEvent(QKeyEvent *event) override;
    void contextMenuEvent(QContextMenuEvent *event) override;

private:
    struct PageHit { int page = -1; QPointF point; QRectF rect; qreal scale = 1; };
    PageHit hitAt(const QPoint &position, bool clampToPage = false) const;
    QRectF pageRect(int page, qreal *scale = nullptr) const;
    void updateSelection(const QPoint &position);
    void drawSelection(QPainter &painter, int page, const QPdfSelection &selection, const QColor &color);
    QList<PaperHighlightRecord> highlights_;
    QList<QPdfLink> searchResults_;
    int currentSearchIndex_ = -1;
    QList<QPair<int, QPdfSelection>> selections_;
    PageHit anchor_;
    QPoint pressPosition_;
    bool dragging_ = false;
    std::function<void()> addHighlight_;
    std::function<void(int)> removeHighlight_;
};
