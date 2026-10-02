# HEPShelf 0.9.4

Download the package for your system from the assets below. These packages were built and passed automated tests in GitHub Actions.

| System | Asset | How to use it |
| --- | --- | --- |
| Ubuntu 24.04 / Linux Mint 22, x86-64 | `hepshelf_0.9.4_amd64.deb` | Download, then run `sudo apt install ./hepshelf_0.9.4_amd64.deb`. |
| Windows, x86-64 | `HEPShelf-setup-x64.exe` | Run the installer to install HEPShelf for your Windows account. |
| Windows, x86-64, portable | `HEPShelf-windows-x64.zip` | Extract the ZIP and run `HEPShelf/hepshelf.exe`; keep the DLLs and plugin folders with it. |
| macOS, Intel or Apple Silicon | `HEPShelf-macOS-universal.dmg` | Open the disk image and copy HEPShelf to Applications. |

## What's new

- Select and copy text in the integrated PDF reader.
- Save, revisit, and remove highlights in your local HEPShelf library.
- Search within a PDF with Ctrl+F, visible matches, and F3 / Shift+F3 navigation.
- Keep automatically saved notes for each PDF page and quote selected text into a note.
- Use the new reader sidebar for citations and page notes.
- Install the Windows build directly from a setup executable, alongside the existing portable ZIP.

## Platform notes

- These packages were built with GitHub Actions. The 0.9.4 Windows installer and macOS disk image have not yet been tested on a user's machine.
- The Windows installer and macOS app are unsigned. Windows or macOS may show a publisher or security warning when opening them.
- Search and text selection require a PDF text layer. Image-only scans need OCR first. Highlights and page notes are saved in HEPShelf's local database; the PDF file itself is unchanged.
- The Windows and macOS packages do not include `pdftotext`. Recognizing arXiv IDs from the contents of renamed PDFs requires that tool separately; filename recognition still works.
- If Windows reports a missing Microsoft C++ runtime, install the current [Microsoft Visual C++ Redistributable for x64](https://learn.microsoft.com/en-us/cpp/windows/latest-supported-vc-redist).
