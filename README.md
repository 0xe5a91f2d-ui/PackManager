# PackManager

PackManager is a Windows desktop video-pack builder. The application and the
SafeTensors reader, writer, and package-signature support are implemented in
C++17 with Qt 6 Widgets and Qt Multimedia. The package-format API and its
implementation are grouped in `src\PackageFormat.h` and
`src\PackageFormat.cpp`; project drafts remain a separate format module.
Python is used only for interoperability tests; it is not required to run the
application.

## Build on Windows

The build script expects Visual Studio 2022 C++ build tools, CMake, and a Qt 6
MSVC installation. Update the Qt and Visual Studio paths at the top of
`build.ps1` if they differ on your machine.

```powershell
.\build.ps1
```

The deployable application and its Qt runtime files are written to `dist\`.
The script also builds the C++ format test helper at
`build\Release\PackManagerFormatTest.exe`.

## Run SafeTensors compatibility tests

Install Python and the test-only SafeTensors dependency, build the C++ helper,
then run the tests:

```powershell
python -m pip install safetensors numpy
python -m unittest -v test_packmanager
```

The Python tests launch the C++ writer and read its output using the official
SafeTensors reader. This keeps C++ as the single implementation while checking
that its format stays compatible.

## Package format

Each video and optional cover image is stored as a one-dimensional `U8` tensor
in the Hugging Face-compatible export profile. Those tensor bytes are the
original media file bytes, so a compatible consumer can write each tensor
directly back to its original media file. Export also offers an external-media
option for local use; do not use that option when uploading a single file to
the Hub because its sibling media folder is not embedded. Manifest version 2
includes tensor names, filenames, MIME types, byte sizes, SHA-256 digests, tags,
collections, and optional subtitle metadata; the reader remains compatible
with version 1. On Windows, packages can also be signed with a per-user ECDSA
key protected by Windows DPAPI. The input media files are copied and are not
modified.

The manifest is stored as the string metadata value
`packmanager_manifest` in the SafeTensors header. Standard SafeTensors readers
can list and read the `U8` tensors without PackManager; another program reads
the manifest from the header metadata to map tensor names to media roles.
Hugging Face Hub can store and serve the `.safetensors` file, but its standard
model-weight viewer does not automatically render these custom video-pack
tensors as playable videos.

The reference extractor in `examples\extract_safetensors_pack.py` can read a
local file or download it from a Hub repository, check each asset's size and
SHA-256, then write the media and `manifest.json`:

```powershell
python -m pip install safetensors numpy huggingface_hub
python examples\extract_safetensors_pack.py --repo-id owner/repository `
  --filename video-pack.safetensors --output extracted
```

PackManager can open its video-pack files, list their metadata, preview cover
images, and play selected videos inside the application. It reads only the
SafeTensors header initially; the selected video is copied to a temporary file
for Qt Multimedia playback, and that file is removed when PackManager exits.
Select a video in the list to edit its metadata, replace its video or cover,
remove its cover, or save changes over the opened package. The `.safetensors`
format is not encrypted or restricted to PackManager.

## Editing and project workflow

- Unsaved metadata/media edits prompt before switching items, opening a project,
  starting a new project, or closing the app. A recovery draft is autosaved
  locally and offered at the next launch if the app did not close normally.
- Drop videos and cover images onto the video list, or use **Videoları toplu
  ekle...**. Matching video and cover base names are paired automatically;
  already-added video paths are skipped.
- Drag list entries to set the order that will be written into the package.
- The list shows cover thumbnails, media sizes, and video duration after the
  video has been opened for playback. Missing source files are marked.
- **Paket doğrula** checks the SafeTensors structure and reports file/media
  sizes, tensor/video counts, and category totals.
- **Taslağı kaydet** stores the project metadata and source paths in a `.pmp`
  JSON project file, so editing can continue without rebuilding the package.
- Search across titles, descriptions, categories, tags, and collections; filter
  by category and sort by a primary and secondary criterion. Thumbnail and
  detailed list modes are available from the list controls.
- Select multiple videos to change categories, apply a cover, delete them, or
  export them as a separate `.safetensors` package. Undo/redo keeps up to 50
  edit snapshots. Copy/paste and duplicate work on selected entries.
- Edit tags, collections, and SRT/VTT subtitle paths; **Altyazıyı görüntüle...**
  previews subtitle text, and folder extraction includes subtitle files.
- **Video teknik bilgileri...** and **Videoyu dönüştür...** use FFprobe/FFmpeg
  from the system `PATH`; conversion runs in the background and replaces the
  selected entry only after the MP4 has been written successfully.
- The app checks the configured GitHub Releases endpoint on startup (toggle it
  in **Ayarlar...**) and provides a manual **Güncellemeleri denetle...** action.
  It reports release notes and opens the release page; installation remains a
  user-controlled step.
- Keyboard shortcuts include Ctrl+N/O/S, Ctrl+F, Ctrl+Shift+S, Ctrl+D, and F5.
- Manage categories from the list. Removing an in-use category asks which
  remaining category should receive its videos.
- Use **Paket bilgileri...** to edit package title, description, version, and
  creation time. These fields are included in both SafeTensors manifests and
  `.pmp` projects.
- **Sürüm geçmişi...** restores one of the last 20 package backups. Replacing a
  package first saves its previous container and any referenced external media.
- **Ayarlar...** enables/disables recovery autosave and sets its debounce delay.
- Export asks whether media should be embedded (portable single file) or stored
  in an adjacent folder (smaller container, with the folder required at runtime).
- **Bu kareyi kapak yap** captures a frame from the selected video. Play or
  pause at the desired moment, then save the captured still as a cover image.
- The package menu can extract selected videos (or all videos when nothing is
  selected), their cover images, and a `manifest.json` into a folder. Large
  package writes, reads, validation, and extraction run in background tasks
  with progress dialogs.
- Package validation hashes embedded and referenced external media and reports
  exact duplicate payloads and duplicate video IDs in its detailed report.
  Export preflight shows the
  estimated media size, missing sources, and repeated source paths.
- **Eksik medya dosyalarını bul...** searches a chosen folder recursively and
  reconnects missing draft files or moved source packages by filename.
- The View menu persists the Turkish/English UI choice and light/dark theme.
  The app displays its version in **Hakkında...** and the executable carries
  Windows version information and a PackManager application icon.

## Portable and installer distribution

After building, `dist\` is a runnable portable application folder. To create a
zip archive:

```powershell
.\package-portable.ps1
```

An Inno Setup installer definition is provided at
`installer\PackManager.iss`. If Inno Setup is installed and `ISCC.exe` is on
`PATH` (or in its standard installation directory), `build.ps1` also compiles
the installer. Otherwise it still builds the portable folder and prints a
warning.
