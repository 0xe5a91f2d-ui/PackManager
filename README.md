# PackManager

PackManager is a Windows desktop application for building, inspecting, and
editing video collections packaged as a single `.safetensors` file. A package
can contain the original video bytes, cover images, optional subtitles, and a
JSON manifest with searchable metadata. It is intended both for managing
collections locally and for publishing an embedded, single-file package to
Hugging Face Hub so that another program can read the tensors and manifest.

The desktop application and package-format implementation are written in
C++17 with Qt 6. Python is used for interoperability tests and the reference
extractor; it is not needed to run the Windows application.

> **Important:** PackManager's media tensors are application data, not model
> weights. The file follows the SafeTensors container format, so standard
> SafeTensors tools can read its tensors and metadata. Hugging Face Hub can
> store and serve the file, but the standard model viewer does not display
> these custom tensors as playable video.

## Contents

- [Highlights](#highlights)
- [Requirements](#requirements)
- [Build from source](#build-from-source)
- [Run the tests](#run-the-tests)
- [Use PackManager](#use-packmanager)
- [Package format](#package-format)
- [Read a package from Python](#read-a-package-from-python)
- [Hugging Face Hub](#hugging-face-hub)
- [Portable and installer builds](#portable-and-installer-builds)
- [Repository layout](#repository-layout)
- [Known limitations](#known-limitations)
- [Roadmap](#roadmap)
- [Contributing](#contributing)

## Highlights

### Package and media management

- Create a new video collection or open an existing PackManager `.safetensors`
  package.
- Add videos individually, in batches, or by dragging files into the list.
  When names match, video and cover files are paired by their base filename.
- Edit titles, descriptions, categories, tags, collections, subtitles, and
  package-level title, description, version, and creation date.
- Add or replace a video or cover, remove a cover, and capture a video frame
  for use as its cover.
- Reorder entries by dragging them in the list. Search metadata, filter by
  category, sort by primary and secondary criteria, and switch between
  thumbnail and detailed list views.
- Select multiple entries to change their category, apply a cover, delete
  them, or export them to a separate package.
- Copy, paste, and duplicate entries. Undo and redo up to 50 edit snapshots.
- Manage categories; removing a category used by entries prompts for a
  replacement category.
- Save and reopen `.pmp` project drafts without rebuilding the package.
  Recovery autosave can be enabled or disabled and its debounce delay can be
  adjusted in settings.
- Reconnect missing source media by searching a selected folder recursively.
- Review the last 20 package backups and restore an earlier package version.

### Inspection, validation, and export

- Validate the SafeTensors structure and media integrity, including SHA-256
  checks, and review file size, tensor/video counts, category totals,
  duplicate media payloads, and duplicate video IDs.
- Review an export preflight summary with estimated media size, missing source
  files, and repeated source paths.
- Extract selected media, or all media when nothing is selected, together
  with a `manifest.json`.
- Choose embedded media for a self-contained package, or external media for
  local workflows that keep assets in a sibling folder.
- Sign packages on Windows with ECDSA keys protected for the current user by
  Windows DPAPI.
- Run longer package reads, writes, validation, and extraction in background
  tasks with progress dialogs.

### Video, preferences, and distribution

- Preview video inside the application and preview SRT/VTT subtitle text.
- Inspect technical video information and convert the selected video using
  FFprobe/FFmpeg available on the system `PATH`.
- Use the Turkish or English language choice and light or dark theme.
- Check for a new GitHub Release at startup or manually. The application shows
  release information and opens the release page; installing an update is a
  user-controlled step.
- Build a portable Windows folder and, when Inno Setup is installed, a Windows
  installer.

## Requirements

### To run the application

- Windows 10 or later.
- Use the portable application folder produced by the build script so the
  required Qt runtime files are beside the executable.
- Video playback and available codecs depend on the Qt Multimedia backend and
  codecs installed on the machine.

### To build from source

- Visual Studio 2022 C++ build tools (x64).
- CMake 3.21 or newer.
- Qt 6 with Core, Concurrent, Multimedia, MultimediaWidgets, Network, and
  Widgets components, built for the same MSVC toolchain.
- `windeployqt`, included with the Qt installation.
- Optional: Inno Setup 6 to generate the installer.

The checked-in `build.ps1` currently points to Qt `6.7.3\msvc2019_64` and a
Visual Studio 2022 Community installation at their default paths. Edit the
paths near the top of that script if your installation differs. CMake declares
Qt 6.2 as its minimum package version; the provided build script uses its
configured Qt installation.

### To run Python tests

- Python 3.
- `safetensors` and `numpy`.
- `huggingface_hub` is additionally required only when using the reference
  extractor to download a package from Hub.

## Build from source

From a PowerShell prompt in the repository root:

```powershell
.\build.ps1
```

The script configures and builds the Release C++ targets, copies the
application to `dist\`, and deploys the required Qt runtime files with
`windeployqt`. If Inno Setup is available, it also compiles the installer;
otherwise it reports that the portable build succeeded without an installer.

Expected outputs:

- `dist\PackManager.exe` and its deployed runtime files.
- `build\Release\PackManagerFormatTest.exe`, the C++ helper used by the
  interoperability tests.
- When Inno Setup is available, `dist\PackManager-Setup-0.1.0.exe`.

To produce a ZIP of the portable folder after building:

```powershell
.\package-portable.ps1
```

The script accepts an optional version argument:

```powershell
.\package-portable.ps1 -Version "0.1.0"
```

## Run the tests

Build the C++ test helper first, then install the Python test dependencies and
run the compatibility suite:

```powershell
python -m pip install safetensors numpy
python -m unittest -v test_packmanager
```

The suite covers project draft round-trips and corruption handling, package
metadata, embedded and external media, hashes and signatures, standard
SafeTensors interoperability, extraction, replacement, and the Hugging Face
reference extractor. Tests that launch the C++ helper locate it at
`build\Release\PackManagerFormatTest.exe` by default. Set
`PACKMANAGER_FORMAT_TEST_EXE` to use a helper at another path. If Qt is not on
`PATH`, set `PACKMANAGER_QT_BIN` to the directory containing the required Qt
runtime DLLs.

## Use PackManager

1. Start `dist\PackManager.exe` after building, or start the executable in the
   deployed portable folder.
2. Create a project, open an existing `.pmp` draft, or open a
   `.safetensors` package.
3. Add videos and optional covers. Review the title, description, category,
   tags, collection, subtitle, and package-level metadata.
4. Use search, category filtering, sorting, selection, and drag reordering to
   prepare the collection.
5. Run **Paket doğrula** before distribution. Resolve missing files or
   integrity issues reported by the validation result.
6. Save a `.pmp` draft if you want to preserve source paths and editing state.
   Export a `.safetensors` package when the collection is ready.
7. For a single-file Hub upload, choose the embedded-media option. External
   media mode depends on a sibling directory and is intended for local use.

### Useful workflows

- **Batch add:** Drop files onto the list or use **Videoları toplu ekle...**.
  Matching video and cover base names are paired automatically, and already
  added video paths are skipped.
- **Replace package media:** Open a package, select an entry, replace its
  source media or metadata, then save. Before replacing a package, PackManager
  creates a backup. Review or restore backups with **Sürüm geçmişi...**.
- **Recover moved media:** Use **Eksik medya dosyalarını bul...** and choose a
  folder to search recursively for missing files.
- **Extract:** Use **İçeriği klasöre çıkar...** to extract selected entries,
  or all entries if none are selected. The output includes the manifest.
- **Create a cover from a frame:** Play or pause at the desired frame and use
  **Bu kareyi kapak yap**.
- **Technical details or conversion:** Use **Video teknik bilgileri...** or
  **Videoyu dönüştür...**. These actions require `ffprobe` or `ffmpeg` on the
  system `PATH`; conversion runs in the background and only replaces the
  selected source after a successful output.
- **Shortcuts:** Ctrl+N (new), Ctrl+O (open), Ctrl+S (save), Ctrl+F (search),
  Ctrl+Shift+S (save/export as), Ctrl+D (duplicate), and F5 (validate).
  Undo/redo use the standard platform shortcuts; copy/paste entry actions use
  Ctrl+Shift+C and Ctrl+Shift+V.

## Package format

### SafeTensors container

The package uses the standard SafeTensors file layout: an 8-byte
little-endian header length, a JSON header, then contiguous tensor data. Video,
cover, and subtitle payloads are stored as one-dimensional `U8` tensors. Their
tensor bytes are the original file bytes—not re-encoded media—so a consumer
can write a tensor back to disk to reconstruct that asset.

Each tensor has a stable role-based name in the manifest, such as
`video_0000`, `cover_0000`, or `subtitle_0000`. Tensor descriptors in the
SafeTensors header include `dtype`, `shape`, and byte offsets. The manifest is
stored as a string in the `__metadata__` field under
`packmanager_manifest`.

### Manifest

The current writer emits manifest version 2 using the format identifier
`packmanager.video-pack`. It includes package metadata, a `videos` list, and an
`assets` list. The reader remains compatible with manifest version 1.

A shortened example:

```json
{
  "format": "packmanager.video-pack",
  "version": 2,
  "media_storage": "embedded",
  "package": {
    "title": "Example collection",
    "description": "A collection of videos",
    "version": "1.0",
    "created_at": "2026-10-04T00:00:00Z"
  },
  "videos": [
    {
      "id": "video-id",
      "title": "Example video",
      "description": "Video description",
      "category": "Tutorial",
      "video_filename": "example.mp4",
      "video_tensor": "video_0000",
      "video_sha256": "<sha256>",
      "cover_tensor": "cover_0000",
      "cover_sha256": "<sha256>",
      "subtitle_tensor": null,
      "tags": ["sample"],
      "collection": "Getting started"
    }
  ],
  "assets": [
    {
      "id": "video-id:video",
      "role": "video",
      "tensor": "video_0000",
      "filename": "example.mp4",
      "media_type": "video/mp4",
      "size_bytes": 123456,
      "sha256": "<sha256>",
      "storage": "tensor"
    }
  ]
}
```

The actual manifest can include additional fields, such as cover/subtitle
assets, duration, external paths, and signature information. A reader should
use `assets` to enumerate media and the `videos` records for user-facing
metadata. Do not assume every optional cover or subtitle exists.

### Embedded and external media

- **Embedded:** Media is included in the `.safetensors` data section as `U8`
  tensors. This is the recommended choice for a package uploaded as one file.
- **External:** The `.safetensors` file refers to media in its sibling folder.
  This can reduce the container file size, but the manifest alone is not
  sufficient to reconstruct the collection if the folder is missing. Do not
  use this mode for a single-file Hub upload.

### Integrity and signatures

Manifest assets include file size and SHA-256 digest. PackManager checks
package structure and hashes during validation. On Windows, optional ECDSA
signatures use a per-user key protected with DPAPI.

A signature helps detect changes relative to the public key included in the
package. To establish that a package was produced by a particular trusted
publisher, a consuming application must additionally trust that publisher's
public key or fingerprint through a separate channel. A signature is not
encryption, access control, or proof of identity on its own.

## Read a package from Python

The example extractor in `examples\extract_safetensors_pack.py` reads an
embedded package, checks the SafeTensors tensor ranges, extracts each asset,
and verifies its size and SHA-256 against the manifest. It writes an extracted
`manifest.json` alongside the assets.

Install dependencies:

```powershell
python -m pip install safetensors numpy huggingface_hub
```

Extract a local package:

```powershell
python examples\extract_safetensors_pack.py `
  --file path\to\video-pack.safetensors `
  --output extracted
```

Download a public or authorized Hub file and extract it:

```powershell
python examples\extract_safetensors_pack.py `
  --repo-id owner/repository `
  --filename video-pack.safetensors `
  --revision main `
  --output extracted
```

For gated or private repositories, authenticate using the Hugging Face
ecosystem's supported login flow, or pass a token through a secure mechanism.
Avoid putting access tokens in source code, committed scripts, or public shell
history.

The extractor accepts only embedded `packmanager.video-pack` packages. It
streams asset data in chunks rather than loading each full tensor into memory.
Consumers written in other languages can follow the same layout: parse the
SafeTensors header, read the `packmanager_manifest` metadata string, find each
asset's tensor, and copy its byte range to a file. Verify the manifest's
expected size and SHA-256 before using extracted content.

## Hugging Face Hub

1. Export from PackManager with **embedded media** enabled.
2. Validate the exported `.safetensors` file.
3. Upload the file to a Hub repository using the Hub website, Git, or the
   Hugging Face client library.
4. In the consuming application, download the file and parse its SafeTensors
   header and `packmanager_manifest` metadata. Extract the `U8` tensor ranges
   to recover the original media files.

Hub stores and serves the package, but it does not automatically preview
PackManager's custom video assets as video content. Consider publishing a
separate project page or documentation describing the manifest and providing
an appropriate consumer.

## Portable and installer builds

The portable build is the contents of `dist\`. Copy the full folder, not just
`PackManager.exe`, because the Qt runtime files are deployed beside it.

Create an archive after building:

```powershell
.\package-portable.ps1 -Version "0.1.0"
```

The installer definition is `installer\PackManager.iss`. `build.ps1` invokes
Inno Setup when it can find `ISCC.exe` on `PATH` or in the standard Inno Setup
6 installation directory. Installer metadata currently defines version
`0.1.0`; update it together with the CMake project version and portable
archive version when preparing a release.

## Repository layout

```text
.
├── examples/
│   └── extract_safetensors_pack.py  # Reference local/Hub package extractor
├── installer/
│   └── PackManager.iss             # Inno Setup installer definition
├── src/
│   ├── PackageFormat.h/.cpp        # SafeTensors model, reader, writer, signing
│   ├── ProjectFile.h/.cpp          # .pmp project draft serialization
│   ├── PackManagerWindow.h/.cpp    # Qt desktop interface and workflows
│   └── VideoListWidget.h/.cpp      # Video collection list widget
├── tests/
│   └── format_test_main.cpp        # C++ test helper
├── CMakeLists.txt
├── build.ps1
├── package-portable.ps1
└── test_packmanager.py             # Python interoperability/regression tests
```

## Known limitations

- PackManager currently targets Windows. The package format itself is
  SafeTensors-based, but the desktop application and DPAPI-protected signing
  implementation are Windows-oriented.
- The Hub viewer does not play or preview this package's custom media tensors.
- External-media packages rely on their sibling media directory and are not
  self-contained.
- Video playback, technical inspection, and conversion depend on the codecs
  available to Qt Multimedia and/or FFmpeg tools installed on the system.
- Update checking informs the user and opens a release page; the application
  does not silently download or install updates.
- A package signature does not encrypt media. Publisher trust requires
  out-of-band verification of the signing key.
- The supplied Windows build script has machine-specific Qt and Visual Studio
  paths that may need editing.

## Roadmap

See [ROADMAP.md](ROADMAP.md) for current priorities, proposed milestones, and
completion criteria. Roadmap items describe plans, not guarantees or a release
schedule.

## Contributing

Bug reports and focused pull requests are welcome. Before proposing a change:

1. Check existing issues and the roadmap so work does not duplicate an active
   task.
2. Describe the expected behavior and include reproducible steps for bugs.
3. Keep the C++ package-format implementation authoritative; Python package
   handling is for tests and interoperability examples.
4. Preserve the manifest's backward compatibility. If a format change is
   needed, document it and add fixtures/tests for old and new versions.
5. Build with `.\build.ps1` and run `python -m unittest -v test_packmanager`
   when your environment has the required tools.
6. Include test results and note platform/tooling limitations in the pull
   request.

## License

No license file is currently included. Until the repository publishes a
license, do not assume that others have permission to redistribute or reuse
the project's code.
