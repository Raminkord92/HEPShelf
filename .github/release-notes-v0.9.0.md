# HEPShelf 0.9.0

Download the package for your system from the assets below. All three packages were built and passed their automated tests in GitHub Actions.

| System | Asset | How to use it |
| --- | --- | --- |
| Ubuntu 24.04 / Linux Mint 22, x86-64 | `hepshelf_0.9.0_amd64.deb` | Download, then run `sudo apt install ./hepshelf_0.9.0_amd64.deb`. |
| Windows, x86-64 | `HEPShelf-windows-x64.zip` | Extract the ZIP and run `HEPShelf/hepshelf.exe`; keep the DLLs and plugin folders with it. |
| macOS, Intel or Apple Silicon | `HEPShelf-macOS-universal.dmg` | Open the disk image and copy HEPShelf to Applications. |

## Platform notes

- The Windows package was tested by the project owner and worked. If a different Windows PC reports a missing Microsoft C++ runtime, install the current [Microsoft Visual C++ Redistributable for x64](https://learn.microsoft.com/en-us/cpp/windows/latest-supported-vc-redist).
- The macOS package passed the GitHub Actions build and test, but has not yet been tried on a user's Mac. It is not signed or notarized, so macOS may show a security warning when opening it.
- The Windows and macOS packages do not include `pdftotext`. Detecting arXiv IDs from the *contents* of renamed PDFs requires that tool to be installed separately; recognition from PDF filenames still works.
- No other platform-specific runtime bugs are confirmed yet. Please report any reproducible issue through the repository's Issues page and include your operating system version.
