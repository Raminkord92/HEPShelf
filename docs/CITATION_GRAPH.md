# Citation graph explorer

HEPShelf 0.8 uses a controlled multi-hop graph rather than an automatically recursive citation crawl.

## Interaction

Open the graph with `Ctrl+G`. Right-click any node to expand its references, cited-by papers, or both. The **Per expansion** control determines how many neighbors are revealed at one time. Repeating the expansion reveals the next cached batch.

Double-clicking a local node opens its PDF in the built-in reader. Double-clicking a remote node opens arXiv when an arXiv ID is available, otherwise INSPIRE.

## Layout semantics

The root paper is depth 0. References are assigned one depth level earlier; citing papers are assigned one level later. Arrows point from cited work to later citing work. Nodes reached by several routes are merged when they share a known arXiv ID, INSPIRE record ID, DOI, or normalized title.

## Network policy

Local SQLite caches are preferred for local papers. INSPIRE is queried only when the requested direction is not cached or when more cited-by results are needed. Cited-by results grow in 250-record requests up to 1,000 records for a node. The visual graph itself grows only by the selected per-expansion batch.

## Safety against runaway graphs

A graph session is capped at 400 nodes. Expansion is explicit and user-driven. This prevents popular reviews or classic papers from accidentally producing an unreadable thousands-node graph.

## Local matching

Every discovered node is checked against the HEPShelf library by arXiv ID, DOI, and INSPIRE record ID. The **Local papers only** toggle can therefore filter an expanded graph at any depth.
