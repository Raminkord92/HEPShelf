# HEPShelf 0.9.3

Download the package for your system from the assets below. The packages were built and passed automated tests in GitHub Actions.

| System | Asset | How to use it |
| --- | --- | --- |
| Ubuntu 24.04 / Linux Mint 22, x86-64 | `hepshelf_0.9.3_amd64.deb` | Download, then run `sudo apt install ./hepshelf_0.9.3_amd64.deb`. |
| Windows, x86-64 | `HEPShelf-windows-x64.zip` | Extract the ZIP and run `HEPShelf/hepshelf.exe`; keep the DLLs and plugin folders with it. |
| macOS, Intel or Apple Silicon | `HEPShelf-macOS-universal.dmg` | Open the disk image and copy HEPShelf to Applications. |

## Changes since 0.9.0

- Research Watches now use arXiv's OAI metadata service for author and category checks. This avoids the legacy search API request that could be blocked by HTTP 429.
- The library UI has a clearer toolbar, navigation, paper table, and details panel. The details panel can be toggled and starts hidden at compact window sizes so paper titles have room.
- Empty library and search views now explain what to do next.

## Platform notes

- These packages were built with GitHub Actions. The new Windows and macOS packages have not yet been tested on a user's machine. The previous Windows release was tested by the project owner.
- The macOS app is unsigned and not notarized, so macOS may show a security warning when opening it.
- The Windows and macOS packages do not include `pdftotext`. Recognizing arXiv IDs from the contents of renamed PDFs requires that tool separately; recognition from filenames still works.
- If Windows reports a missing Microsoft C++ runtime, install the current [Microsoft Visual C++ Redistributable for x64](https://learn.microsoft.com/en-us/cpp/windows/latest-supported-vc-redist).
