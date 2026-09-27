# Citation-aware reader design

HEPShelf 0.6 resolves citations in two layers.

## 1. Reliable side-panel resolution

For each currently visible/current PDF page:

1. `QPdfDocument::getAllText(page)` extracts the page text locally.
2. HEPShelf recognizes common square-bracket numeric citation forms.
3. Ranges such as `[12–15]` are expanded with a safety cap.
4. When a structured INSPIRE bibliography is available, candidate numbers are bounded by the bibliography size to reduce false positives.
5. The numeric position is mapped to `paper_references.position` in SQLite.
6. The existing local-reference resolver checks arXiv ID, DOI, then INSPIRE record ID.
7. The reader presents local/arXiv/metadata status and a context excerpt.

This path does not depend on PDF hyperlink annotations.

## 2. Best-effort direct hyperlink bridge

Many TeX papers built with `hyperref` make `[37]` an internal PDF link to bibliography item 37.

Qt PDF emits a `QPdfPageNavigator::jumped(QPdfLink)` event when such a link is followed. When the link retains its source rectangle, HEPShelf:

1. remembers the source page before the internal jump;
2. reads the text under/around the source rectangle;
3. verifies that it looks like a square-bracket citation;
4. extracts the linked reference number;
5. resolves that number through the cached structured bibliography;
6. if the target has a local PDF, opens it in HEPShelf and pushes the original paper/page onto the cross-paper history stack.

If any step is ambiguous, or the cited paper is not local, HEPShelf does not intercept the click. The PDF's original jump-to-bibliography behavior is preserved.

## Why both layers exist

PDF generators and publishers encode links differently. A side-panel resolver based on page text is predictable; click interception is convenient but cannot be guaranteed for every PDF. The direct-click feature is therefore deliberately conservative rather than guessing.
