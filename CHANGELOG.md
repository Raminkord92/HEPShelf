# Changelog

## 0.9.0 — research notes and literature trails

- Added per-paper **Research notes** stored locally in SQLite.
- Notes autosave after a short typing pause and can be saved immediately with `Ctrl+S`.
- Added note text to the main library search index.
- Added named, ordered **Literature trails** with descriptions.
- Trails may contain both local papers and remote arXiv/INSPIRE records.
- Added a dedicated trail manager with create, rename, delete, reorder, remove, and open actions.
- Added per-trail-item notes for recording why a paper matters in that specific reading path.
- Added offline BibTeX export for a complete ordered trail.
- Added Markdown reading-list export including trail and per-item notes.
- Added a **LITERATURE TRAILS** sidebar facet for filtering locally available trail papers.
- Added trail names and trail-item notes to main-library search.
- Added **Add to literature trail…** to the paper details pane and library context menu.
- Added **Literature trails…** (`Ctrl+Shift+T`) to the main application menus.
- Added **Save exploration as trail…** to the citation graph.
- Added graph-node context action **Add paper to literature trail…** for local and remote citation nodes.
- Citation-graph exploration order is persisted into a trail in discovery order and can then be manually reordered.
- Added non-destructive `paper_notes`, `literature_trails`, and `literature_trail_items` tables.
- Existing PDFs, metadata, tags, collections, reader history, citation caches, and INSPIRE/arXiv data are preserved.
- Updated application version and network user agents to 0.9.0.

## 0.8.0 — controlled multi-hop citation exploration

- Replaced the one-hop citation graph with a controlled multi-hop explorer.
- Any graph node can now be expanded independently through references, cited-by papers, or both directions.
- Added right-click node menus with expansion, opening, fit, and reset actions.
- Added **Expand selected** for keyboard/mouse-friendly exploration.
- Added per-expansion batch sizing (3–30, default 10) instead of globally dumping large neighborhoods into the scene.
- Added progressive cited-by fetching from INSPIRE in 250-record steps, up to 1,000 cached results per expanded node.
- Added citation-depth column layout, with arrows pointing from cited works toward later citing works.
- Added multi-route node deduplication by arXiv ID, INSPIRE record ID, DOI, and normalized title.
- Added local-library resolution for remote graph nodes by arXiv ID, DOI, or INSPIRE record ID.
- Local nodes reuse existing reference and cited-by caches before network access.
- Added a 400-node in-memory session cap to prevent accidental graph explosions.
- Added graph progress badges such as `refs 10/63` and `cited 20/128`.
- Added persistent graph geometry, local-only mode, and expansion-batch preference.
- Added INSPIRE record fetching by literature record ID for nodes that have no arXiv identifier.
- Extended the database API with identifier-aware local-paper resolution; no destructive migration is required.
- Updated application version and network user agents to 0.8.0.

## 0.7.0 — cited-by browser and interactive citation network

- Added **Browse cited-by papers…** (`Ctrl+Shift+Y`).
- Added INSPIRE cited-by queries using the selected paper's INSPIRE record ID.
- Added SQLite caching for citing-paper search results and their total result count.
- Citing papers are resolved against the local library by arXiv ID, DOI, or INSPIRE record ID.
- Added configurable cited-by fetch limits of 50, 100, 250, and 500 newest papers.
- Added local/remote status to cited-by results.
- Added direct opening of local citing papers.
- Added arXiv and INSPIRE opening for remote citing papers.
- Added selective download of missing citing papers from arXiv with atomic writes and PDF validation.
- Added **Open citation network…** (`Ctrl+G`).
- Added an interactive, pannable, zoomable one-hop graph with references on the left, the current paper in the center, and citing papers on the right.
- Added graph filtering to local papers only.
- Added configurable neighbor count per side.
- Added graph refresh of both the structured bibliography and cited-by results.
- Double-clicking a local graph node opens it in the HEPShelf reader.
- Paper details now show the cached cited-by total and how many cached citing papers are already local.
- Added `docs/CITATION_GRAPH.md`.
- Added `paper_citing_cache` and `paper_citing_papers` database tables; migration is automatic and non-destructive.
- Updated application version and network user agents to 0.7.0.

## 0.6.0 — citation-aware reader

- Added a live Citation navigator beside the built-in Qt PDF reader.
- Current-page PDF text is scanned for common numeric citation callouts such as `[12]`, `[4, 7, 19]`, and ranges such as `[31-35]`.
- Detected callout numbers are resolved against cached structured INSPIRE references.
- Added local/arXiv/metadata/unmatched status in the reader.
- Added citation preview with title, authors, identifiers, local path, and the current-page sentence/excerpt containing the citation.
- Double-clicking a local citation opens the cited PDF inside the HEPShelf reader.
- Added browser-style cross-paper Back/Forward navigation that preserves the source page.
- Added `Alt+Left` and `Alt+Right` reader-history shortcuts.
- Added `Ctrl+Shift+C` citation-panel toggle.
- Added best-effort interception of internal PDF citation hyperlinks when Qt exposes a usable source rectangle; local linked references can open directly instead of jumping only to the bibliography.
- Added automatic INSPIRE bibliography fetching the first time an uncached paper is opened in the reader.
- Added reader access to the full reference browser and manual INSPIRE refresh.
- Added `docs/CITATION_READER.md`.
- No new database migration is required for 0.6.

## 0.5.0 — INSPIRE citations, references and BibTeX

- Added INSPIRE citation metrics and structured references.
- Added local reference resolution and missing-reference downloads from arXiv.
- Added INSPIRE BibTeX copy/export support.
- Added reference bibliography export.

## 0.4.0 — arXiv discovery

- Added remote arXiv search by author, title, abstract, category, exact ID, and advanced query.
- Added duplicate-aware paper downloads into watched folders.

## 0.3.0 — organization

- Added tags, collections, authors, categories, file organization, rename templates, and arXiv version handling.

## 0.2.0 — professional reader preview

- Added three-pane library UI, integrated Qt PDF reader, reading-position memory, favorites, recent papers, and richer metadata.

## 0.1.x — initial indexer

- Added watched folders, arXiv ID detection, metadata caching, search, local opening, and duplicate-copy awareness.
