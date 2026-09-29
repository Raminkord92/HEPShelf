# Changelog

## 0.9.2 — toolbar polish

- Group the main library actions into a compact toolbar and move metadata refresh options into an Update data menu.
- Give the toolbar and table headers a lighter gradient, clearer hover states, and a flexible search field.

## 0.9.1 — Research Watch update

- Check new arXiv papers through arXiv's OAI metadata service, including author and category watches. This replaces the legacy search API request that could remain blocked by HTTP 429 rate limits.
- Keep a failed watch due for a later check and show the server error in the watch dialog.
- Label the arXiv discovery toolbar action so it is distinct from Add paper folder.

## 0.9.0 — first public release

This is the first published HEPShelf build. Earlier version numbers described development milestones; they were not public releases.

### Library and reading

- Index PDFs from selected folders, recognize arXiv IDs, cache metadata locally, and find duplicate copies.
- Read PDFs inside HEPShelf with zoom, page navigation, saved reading position, and citation callout navigation.
- Search the library and organize papers with tags, collections, favorites, and metadata-based file move templates, including bulk moves.

### Discovery and citations

- Search arXiv and download selected papers into watched folders.
- Browse INSPIRE references and citing papers, resolve local copies, download missing arXiv papers, and export BibTeX.
- Explore a citation graph with controlled expansion, local-paper highlighting, and a cap on graph size.
- Watch authors, arXiv categories, and new citations to selected papers while HEPShelf is running.

### Research workflow

- Keep searchable notes on local papers.
- Create ordered literature trails containing local or remote papers, add trail-specific notes, and export reading lists or BibTeX.

### Packages

- GitHub Actions builds and tests packages for Ubuntu 24.04 / Linux Mint 22, Windows x86-64, and macOS on Intel and Apple Silicon.
- Windows was tested by the project owner. The macOS package is an unsigned test build that has not yet been tested on a user's Mac.
