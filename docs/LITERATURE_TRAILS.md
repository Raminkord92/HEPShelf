# Literature trails and research notes

HEPShelf 0.9 adds a persistent research layer for reading paths, project planning, and paper-specific notes.

## Research notes

Each local paper has one global research note in the details pane. Notes are stored in `paper_notes` inside the HEPShelf SQLite database and are never written into the PDF itself.

Notes autosave after a short pause while typing. `Ctrl+S` forces an immediate save. Note text participates in the main library search.

Use the global paper note for information that should follow the paper everywhere, for example:

- the main result you care about;
- equations or figures to revisit;
- how the work connects to your own project;
- presentation or referee-report reminders.

## Literature trails

A literature trail is an ordered sequence of papers. Unlike a collection, the sequence is meaningful.

Examples:

```text
original formalism -> refinement -> phenomenology -> recent application
```

or:

```text
paper to read first -> technical background -> comparison paper -> paper to cite
```

A trail is stored in `literature_trails`; its ordered entries live in `literature_trail_items`.

Trail entries are identifier-based and may represent papers that are not yet downloaded. HEPShelf stores arXiv ID, INSPIRE record ID, DOI, title, authors, and year when available. When the trail is opened later, every item is re-resolved against the local library, so a previously remote entry can automatically become `Local` after the PDF is added.

## Trail manager

Open **Library -> Literature trails...** (`Ctrl+Shift+T`). The manager can:

- create, rename, describe, or delete trails;
- reorder entries;
- remove entries without deleting PDFs;
- attach a note to an item in the context of that trail;
- open local items in the HEPShelf reader;
- open remote items on arXiv or INSPIRE;
- export the sequence as BibTeX;
- export a Markdown reading list.

## Global paper notes vs trail-item notes

They are intentionally different.

A **paper note** belongs to the paper globally. A **trail-item note** belongs to that paper's role in one particular trail.

For example:

```text
Global paper note:
"Good derivation of the unequal-scale DPD sum rule; check Eq. 27."

Trail: QCD Extreme talk
Item note:
"Use only Fig. 2 on the motivation slide."

Trail: DPD evolution reading
Item note:
"Read after Gaunt 2010; this is where the boundary condition becomes clearer."
```

## Building a trail from the citation network

The citation graph keeps the order in which new nodes are discovered during the current exploration.

Use **Save exploration as trail...** to persist that sequence. You can then reorder or annotate the result in the trail manager.

Right-clicking an individual graph node also offers **Add paper to literature trail...**. This works for both local and remote nodes.

## Search and sidebar integration

The main search covers:

- global paper notes;
- trail names;
- trail-item notes.

The **LITERATURE TRAILS** sidebar section filters the local library to papers that belong to a chosen trail. Remote-only entries remain visible in the trail manager but do not appear as library rows until a local PDF exists.
