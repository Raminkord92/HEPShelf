# HEPShelf

HEPShelf is a native local research-paper library aimed at high-energy-physics workflows. It indexes only folders selected by the user, recognizes arXiv papers, caches metadata in SQLite, provides an integrated PDF reader, organizes papers with tags and collections, searches and downloads from arXiv, and uses INSPIRE for HEP citation metadata and BibTeX.

## Install on Linux

Download the `.deb` installer from this repository's **Releases** page. It is built for Ubuntu 24.04 and compatible Linux Mint 22 systems on x86-64. Install it with:

```bash
sudo apt install ./hepshelf_*.deb
```

Launch **HEPShelf** from the application menu or run `hepshelf`. Package dependencies, including Qt, are installed by APT. The optional `poppler-utils` package helps recognize arXiv IDs in renamed PDFs. Library data stays in the user's home directory and is preserved when the package is upgraded or removed.

The release workflow builds, tests, and packages the application. It attaches the installer to version tags and keeps packages from manual runs as GitHub Actions artifacts.

## Try the Windows build

The **Windows build** GitHub Actions workflow creates a 64-bit ZIP. Download it from this repository's **Releases** page, extract it, and launch `HEPShelf/hepshelf.exe`. Keep the bundled DLLs and plugin folders beside the executable. If Windows reports a missing Microsoft C++ runtime, install the current [Microsoft Visual C++ Redistributable (x64)](https://learn.microsoft.com/en-us/cpp/windows/latest-supported-vc-redist) and try again. 

## Try the macOS build

The **macOS build** GitHub Actions workflow creates a universal disk image for Intel and Apple Silicon Macs. Download the `.dmg` from this repository's **Releases** page, open it, and copy HEPShelf to Applications. This is an unsigned test build and has not yet been notarized or tested on a user's Mac.

HEPShelf includes **local paper notes** and **named literature trails**. A trail is an ordered reading/research path that may contain papers already on your laptop as well as remote arXiv or INSPIRE records discovered in the citation graph.

## Research notes

Each local paper now has a **Research notes** editor in the details pane. Notes are stored only in the local HEPShelf SQLite database and autosave after a short pause while typing. `Ctrl+S` forces an immediate save.

Notes are included in library search, so a remembered phrase such as:

```text
key result to revisit
compare with related work
compare the methods in section 3
```

can find the paper even when those words are not present in its title or metadata.

## Named literature trails

Open:

```text
Library → Literature trails…
Ctrl+Shift+T
```

A trail is an ordered sequence such as:

```text
Foundational paper
   ↓
Follow-up method
   ↓
Comparison study
   ↓
Recent application
```

Unlike a normal collection, order matters. Trails can also contain remote papers that are not downloaded yet.

The trail manager supports:

- create, rename, describe, and delete trails;
- reorder papers with **Move up / Move down**;
- remove an item without touching any PDF file;
- per-item notes describing why a paper matters in that specific trail;
- open local PDFs directly in HEPShelf;
- open remote records on arXiv or INSPIRE;
- export the ordered trail as BibTeX;
- export a Markdown reading list including trail/item notes.

The library sidebar contains a **LITERATURE TRAILS** section. Clicking a trail filters the main library to its locally available papers. Trail names and trail-item notes are also searchable.

## Build trails directly from the citation graph

The citation network now has a **Save exploration as trail…** button. HEPShelf remembers the order in which papers were discovered during the current graph exploration and can persist that sequence as a named trail.

Right-click any graph node and choose:

```text
Add paper to literature trail…
```

This works for both local nodes and remote arXiv/INSPIRE nodes. You can therefore build a reading path while exploring the citation network, then return to it later from the main library.

A selected local paper can also be added from the details pane or table context menu using **Add to literature trail…**.

## Multi-hop citation explorer

Use:

```text
Library → Open citation network…
Ctrl+G
```

The graph still starts from the selected paper, but every node is now interactive. Right-click a node to choose:

```text
Open local PDF / external record
Expand references
Expand cited-by papers
Expand both directions
Show the next cached batch
Fetch more cited-by records when needed
Fit whole graph
Reset graph to root
```

A double-click keeps the fast open behavior. Local nodes open in the HEPShelf reader; remote nodes open on arXiv or INSPIRE.

The graph is laid out in citation depth columns:

```text
2 steps earlier       1 step earlier          ROOT          1 step later       2 steps later

older source ───────► cited paper ───────► selected ───────► citing paper ───────► later work
```

Arrows point from the cited work toward the later paper that cites it. A paper encountered through several routes is deduplicated by arXiv ID, INSPIRE record ID, DOI, and finally normalized title.

## Controlled expansion rather than graph explosion

The **Per expansion** control defaults to 10. Each expansion reveals only that many neighbors in the requested direction. Right-click the same node again to reveal the next batch.

HEPShelf keeps a hard in-memory graph cap of 400 nodes for one explorer session. Cited-by searches are fetched from INSPIRE in progressively larger cached batches, up to 1,000 records for a node, but only the requested small visual batch is added to the graph.

This makes workflows such as the following practical:

```text
Foundational paper
   ↓ expand cited by
Follow-up paper
   ↓ expand cited by
Recent study
   ↓ expand references
related formalism
```

without loading an entire citation universe at once.

## Local-library awareness at every depth

Multi-hop nodes are checked against the local SQLite library using arXiv ID, DOI, and INSPIRE record ID. A paper can therefore become a **LOCAL** graph node even when it was discovered several citation steps away from the root.

For local nodes, existing HEPShelf reference/cited-by caches are reused before making a network request. Fresh reference data can be persisted back to the local paper's citation cache.

The **Local papers only** switch works across the whole expanded graph, not just the first citation layer.

## Citation graph controls

The graph includes:

- right-click expansion of individual nodes;
- **Expand selected** for both directions;
- batch-size control per expansion;
- local-only filtering;
- zoom and drag panning;
- fit-to-window;
- reset-to-root;
- root citation refresh from INSPIRE;
- persistent graph window geometry, local-only setting, and expansion batch size.

## Paper details now include local citation context

After cited-by data has been fetched, the details pane can show information such as:

```text
Citations 128
without self-citations 116
References 63 (41 local)
Cited-by cached 128 (17 local)
INSPIRE 1234567
```

The INSPIRE citation count remains the authoritative total displayed by HEPShelf. The cached cited-by list may contain only the newest N records chosen in the cited-by browser.

## Citation-aware PDF reader

While reading a paper, HEPShelf detects common numeric citation callouts on the current PDF page, including:

```text
[12]
[4, 7, 19]
[31-35]
[31–35]
```

It resolves those callouts against the cached INSPIRE bibliography and indicates whether each cited paper is local. Local references can be followed inside the HEPShelf reader, with browser-style Back/Forward history returning to the exact source page.

Keyboard shortcuts:

```text
Alt+Left        Reader history back
Alt+Right       Reader history forward
Ctrl+C          Copy selected PDF text
Ctrl+F          Find text in the PDF
F3 / Shift+F3  Next / previous match
Ctrl+Shift+C    Toggle reader sidebar
```

## References and missing-paper acquisition

Use **Browse references…** (`Ctrl+Shift+B`) to inspect the full structured INSPIRE bibliography.

It shows:

```text
Local
Available on arXiv
INSPIRE only / unresolved arXiv
Unresolved
```

Selected missing arXiv references can be downloaded directly. HEPShelf does not recursively download references-of-references.

## Citation metrics and BibTeX

Use **Refresh INSPIRE** (`Ctrl+I`) for a selected paper. HEPShelf caches:

- INSPIRE record ID;
- citation count;
- citation count excluding self-citations when provided;
- reference count;
- structured bibliography.

BibTeX workflows include:

- **File → Export current view as BibTeX…**;
- **Copy INSPIRE BibTeX** for one selected paper;
- **Export INSPIRE BibTeX…** for one selected paper;
- **Export references .bib** from the reference browser.

## arXiv discovery

Use **Discover on arXiv…** (`Ctrl+Shift+A`) to search by:

```text
All fields
Title
Author
Abstract
Category
Exact arXiv ID
Advanced arXiv query
```

Remote results are checked against the local SQLite library before download.

## Research organization

HEPShelf supports:

- tags;
- collections;
- author browsing;
- arXiv-category browsing;
- favorites;
- unread and recently opened views;
- duplicate-copy and missing-metadata views;
- search across title, author, arXiv ID, tags, collections, DOI, and path;
- metadata-based safe move/rename templates;
- bulk moves for selected papers or the entire library;
- arXiv-version-aware local copies.

Logical tags and collections do not move PDFs. Physical file organization is always explicit and previewed.
To move several papers, select rows with Ctrl or Shift and choose **Organize → Move selected papers to folder…**. Choose **Move all library papers to folder…** to include papers outside the current search or filter. Both commands use the same metadata templates as the single-PDF organizer, show an example target, and move every local PDF copy for each paper. Existing targets are skipped. Drag column borders to resize them.

## Daily research watches

Open **Library → Research watches…** to watch an author name, an arXiv category code such as `hep-ph` or `astro-ph.CO`, or new citations to a paper. You can also right-click a library paper and choose **Watch new citations to this paper…**. Citation watches use INSPIRE's citing-paper search. The first successful check records the citations already present; later checks notify you about newly listed citing papers. Matches appear in the watch window and menu count, with a desktop notification where the system tray is available. INSPIRE may not cover citations outside high-energy physics. For papers with over 1,000 citations, the watch window warns that only the 1,000 most recent results were checked.

Each watch can record new matches or automatically download matching arXiv PDFs into a folder you choose. Citation matches without an arXiv ID remain visible and link to INSPIRE, but cannot be downloaded from arXiv. The folder is added to HEPShelf's watched folders, and downloaded papers are indexed in the library. You can also download an individual match from the watch window.

HEPShelf checks enabled watches every 24 hours while it runs, and catches up on the next launch if it was closed. A new author or category watch checks submissions from the previous UTC date onward. The watch window has **Check now**, shows recent matches and download status, and the menu action shows the number of unseen matches. Author watches match names in arXiv's OAI metadata, so authors with the same name may both match. Service failures leave a watch due for a later retry. Author and category checks use the original submission date, so a new version of an older paper is not treated as a new submission.

## Integrated PDF reader

The reader provides:

- continuous multi-page reading;
- previous/next page navigation;
- direct page entry;
- zoom in/out;
- fit width and fit page;
- drag selection, copy text, and saved text highlights (use the toolbar or right-click menu);
- in-document search with match navigation;
- automatically saved notes for each PDF page in the reader sidebar;
- system-viewer fallback;
- persistent reading position;
- current-page citation detection;
- local citation following;
- cross-paper Back/Forward navigation.

Highlights and page notes are stored in HEPShelf's local library database and do not alter the PDF file. Search and text selection require a PDF text layer; image-only scans need OCR first.

## Existing local-paper recognition

Direct filename recognition includes:

```text
2609.14048.pdf
2609.14048v1.pdf
arXiv_2609.14048v2.pdf
hep-ph_9901234.pdf
.../hep-ph/9901234.pdf
```

With `poppler-utils` installed, HEPShelf can inspect the first two pages of renamed PDFs and detect a printed arXiv identifier.

## Build on Linux Mint / Ubuntu

Install dependencies:

```bash
sudo apt update
sudo apt install build-essential cmake qt6-base-dev qt6-pdf-dev libqt6sql6-sqlite
```

Recommended for recognizing renamed PDFs:

```bash
sudo apt install poppler-utils
```

Build:

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j"$(nproc)"
```

Run:

```bash
./build/hepshelf
```

Or install system-wide:

```bash
sudo cmake --install build
```

To build a local Debian installer after building, run:

```bash
cpack --config build/CPackConfig.cmake -G DEB -B dist
```

## Data sources

HEPShelf uses arXiv for paper discovery/PDF acquisition and INSPIRE for HEP citation metadata. Cited-by discovery uses INSPIRE's citation search for papers referring to a literature record.

## Scope of the graph

HEPShelf supports manual multi-hop exploration, but it intentionally does **not** recursively crawl the whole citation network. Expansion is explicit, batched, and capped per session. This keeps classic or highly cited HEP papers usable in the graph instead of turning the view into an unreadable mass of nodes.

## License

MIT. See [LICENSE](LICENSE).
