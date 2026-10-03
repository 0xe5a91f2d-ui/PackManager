# PackManager Roadmap

This document tracks the direction of PackManager and the conditions for
considering planned work complete. It is a planning document, not a promise of
delivery dates. Priorities may change based on user feedback, compatibility
requirements, and test results.

## Status key

- **Done** — implemented in the current codebase.
- **Next** — recommended next work; not started unless a task is explicitly
  marked in progress.
- **Planned** — accepted direction, awaiting prioritization.
- **Exploring** — requires a design decision or prototype before commitment.

## Product goal

Make PackManager a dependable Windows authoring tool for video collections
stored in interoperable `.safetensors` packages. Keep media bytes lossless,
make metadata understandable to independent consumers, and maintain
backward-compatible reading as the format evolves.

## Current baseline — Done

### Package authoring and format

- Create and open SafeTensors video-pack files.
- Store embedded videos, optional covers, and optional subtitles as original
  one-dimensional `U8` tensor bytes.
- Store user-facing video and package metadata in the SafeTensors
  `packmanager_manifest` metadata value.
- Write manifest version 2 and read versions 1 and 2.
- Support a local external-media mode with package-folder path restrictions.
- Export selected entries as another package and extract assets with a
  `manifest.json`.
- Preserve source files; write package outputs separately.

### Editing and project workflow

- Search and filter entries, sort by primary and secondary criteria, and
  choose thumbnail or detailed list presentation.
- Batch add and pair video/cover filenames, reorder entries, and manage
  categories.
- Edit metadata in single or bulk operations; copy, paste, duplicate, delete,
  undo, and redo.
- Save `.pmp` project drafts and offer recovery after an abnormal exit.
- Find moved or missing media, keep package backups, and restore recent
  versions.
- Configure recovery autosave behavior.

### Media and validation

- Preview video and covers; capture a video frame as a cover.
- Preview subtitle files and extract subtitles.
- Read technical video information and convert a video through FFprobe/FFmpeg.
- Validate package structure and media SHA-256 digests; report duplicate
  payloads and duplicate video IDs.
- Show export preflight information and progress for long-running operations.

### Distribution and interoperability

- Choose Turkish/English and light/dark preferences.
- Check GitHub Releases and show release information without automatically
  installing updates.
- Build a portable Windows folder and optionally an Inno Setup installer.
- Provide a Python extractor for local files and Hugging Face Hub downloads.
- Cover format interoperability, media hashes, signatures, external-media
  safety, project drafts, extraction, and replacements in the regression
  suite (14 Python tests at the time this roadmap was written).

## Priority 0 — Release and contributor reliability

**Status: Next**

The goal is for a new contributor or release operator to build and verify the
project without editing machine-specific assumptions unnecessarily.

### Build configuration

- [ ] Move Qt and Visual Studio paths out of hard-coded script constants:
  - accept explicit script parameters and/or environment variables;
  - preserve useful defaults for the current maintainer;
  - validate paths before invoking CMake and show actionable errors.
- [ ] Document and test the supported Qt/MSVC combinations rather than
  relying on a single local installation.
- [ ] Provide a CMake preset or equivalent repeatable configuration for the
  supported Windows Release build.
- [ ] Make the app, installer, and portable archive read the version from one
  release value instead of maintaining separate `0.1.0` literals.
- [ ] Ensure a clean build directory produces the same targets as an
  incremental build.

### Automated validation

- [ ] Add a documented one-command validation path that builds the app and
  C++ helper, then runs the Python interoperability suite.
- [ ] Add the supported build/test workflow to GitHub Actions for Windows.
- [ ] Publish test summaries and retain failure logs as workflow artifacts.
- [ ] Make CI explicitly report when an optional integration test is skipped
  because FFmpeg, Inno Setup, or Hub credentials are unavailable.
- [ ] Add a release checklist that requires a successful build, all required
  tests, and a startup smoke test.

### Completion criteria

- A clean Windows CI runner can configure and build both executables.
- The full required regression suite passes in CI.
- Build failures identify the missing dependency or invalid path.
- Version strings in the executable, installer, and archive name match.

## Priority 1 — Package-format contract and compatibility

**Status: Next**

The goal is to let a separate program implement a reader without depending
on PackManager internals or guessing how fields are represented.

### Format specification

- [ ] Document the complete version 2 manifest schema, including required
  fields, optional values, nullability, identifiers, data types, and
  enumerated asset roles/storage values.
- [ ] Explain the relationship between `videos`, `assets`, SafeTensors
  tensor names, and byte offsets.
- [ ] Specify filename handling, media MIME-type detection, empty/missing
  cover behavior, subtitle behavior, and timestamp conventions.
- [ ] Add a compact, real-world sample package manifest that is validated by
  the C++ writer and Python extractor.
- [ ] Define how future optional fields are ignored and how unsupported
  manifest versions are reported.

### Compatibility fixtures and consumers

- [ ] Check in small synthetic version 1 and version 2 fixtures that contain
  no copyrighted or private media.
- [ ] Test reading each fixture in C++ and Python and verify extracted bytes,
  IDs, names, metadata, and hashes.
- [ ] Add a consumer-facing example that maps manifest assets to video,
  cover, and subtitle roles without relying on ordering assumptions.
- [ ] Test malformed manifests and tensor headers: overlapping ranges,
  out-of-bounds offsets, missing tensors, invalid data types, bad hashes,
  duplicate IDs, and unsupported versions.
- [ ] Define a migration policy before introducing manifest version 3.

### Signature trust model

- [ ] Document signature coverage, canonicalization, and verification
  outcomes at the format level.
- [ ] Make the UI distinguish “signature is mathematically valid” from
  “signing key belongs to a trusted publisher”.
- [ ] Provide a way for users or consuming applications to compare a trusted
  publisher public-key fingerprint with the package key.
- [ ] Add test vectors for valid, altered, malformed, and unknown-key
  signatures.

### Completion criteria

- An independent implementer can create a conforming reader from the docs
  and fixtures alone.
- Version 1 and 2 compatibility is covered by automated fixtures.
- Hash and signature failure cases report the affected asset clearly.
- Signature validation never implies publisher identity without a trusted
  key comparison.

## Priority 2 — Data safety and large-package resilience

**Status: Planned**

The goal is to keep the UI responsive and protect users from partial or
surprising writes when handling large media.

### Safe writes and recovery

- [ ] Review all save/export paths for atomic replacement and cleanup on
  failure, including external media folders.
- [ ] Preserve an existing destination package if a new write fails before
  completion.
- [ ] Define and test what happens if the app exits during autosave, package
  writing, conversion, or extraction.
- [ ] Surface recovery-draft timestamps and give users an explicit choice to
  restore, inspect, or discard a draft.
- [ ] Add a configurable backup retention policy and explain storage usage.

### Large files and background tasks

- [ ] Add regression tests with sparse or generated large files so tests do
  not require committing large media.
- [ ] Verify that read, write, hash, and extraction operations stream data and
  do not allocate a full video tensor in memory.
- [ ] Add safe cancellation for supported background tasks and define what
  partial output is removed or retained.
- [ ] Keep progress totals accurate for embedded and external media, including
  packages with no cover or subtitle.
- [ ] Benchmark package open, validation, export, and extraction at increasing
  package sizes; record machine and file characteristics with results.
- [ ] Add a clear error when a file exceeds format or platform size limits.

### Completion criteria

- Failed operations leave the previous package intact and do not report
  success.
- Test memory usage remains bounded relative to media size for streaming
  paths.
- Cancellation leaves no misleading, apparently complete package.
- Progress and error messages identify the affected operation and file.

## Priority 3 — Usability, accessibility, and localization

**Status: Planned**

The goal is to make routine editing discoverable, keyboard-operable, and
consistent across supported languages.

### Localization

- [ ] Audit every visible string, dialog, validation message, menu, and
  confirmation for translation coverage.
- [ ] Move UI text into Qt translation resources rather than selecting a
  language value without a complete translation catalog.
- [ ] Add a repeatable translation update and validation workflow.
- [ ] Verify both Turkish and English flows for menus, errors, dialogs, and
  first-run behavior.

### Accessibility and interaction

- [ ] Audit keyboard navigation and focus order across the main window,
  dialogs, list, player, and progress operations.
- [ ] Add accessible names and descriptions for controls whose purpose is not
  conveyed by visible text.
- [ ] Verify Windows high-DPI scaling and test long translated labels.
- [ ] Review destructive operations for selection summaries, confirmation,
  and undo/recovery options.
- [ ] Add configurable shortcuts only after documenting existing defaults and
  checking conflicts with text-entry controls.

### Settings

- [ ] Group settings into clear sections for recovery, export, media tools,
  appearance, and updates.
- [ ] Store user preferences with explicit defaults and handle invalid or
  outdated saved values safely.
- [ ] Provide a reset-to-default action with confirmation.
- [ ] Do not store Hub access tokens in plain application settings.

### Completion criteria

- All currently supported user-facing workflows have complete Turkish and
  English strings.
- Core editing actions can be completed by keyboard.
- Destructive or irreversible actions clearly explain their effect.

## Priority 4 — Release, installation, and updates

**Status: Planned**

The goal is a predictable release artifact users can install, verify, and
upgrade deliberately.

### Packaging and provenance

- [ ] Validate the portable build on a clean Windows machine without Qt
  installed.
- [ ] Test install, launch, upgrade, uninstall, and per-user installation
  without administrator privileges.
- [ ] Generate release notes and checksums for each published artifact.
- [ ] Document code-signing options and identify unsigned builds clearly.
- [ ] Verify that installer contents match the portable build.

### Update experience

- [ ] Show the current and available versions and link to release notes.
- [ ] Handle network failures, rate limits, missing release metadata, and
  prerelease versions with clear status messages.
- [ ] Keep update installation user-initiated unless a separate, explicit
  opt-in design is approved.
- [ ] Verify downloads with a published checksum or signature before offering
  installation.

### Completion criteria

- Release artifacts have reproducible versioned names and published
  checksums.
- Upgrade and uninstall preserve user projects and media.
- Update checks fail safely and never replace the running executable without
  user confirmation.

## Priority 5 — Advanced media and collection workflows

**Status: Exploring**

These ideas need user feedback and design work before implementation scope is
committed.

- [ ] Configurable thumbnail generation and cache cleanup for large libraries.
- [ ] Better playback navigation, frame stepping, and precise timestamp entry
  for cover capture.
- [ ] Optional metadata presets or reusable templates for repeated collections.
- [ ] Import/export of a documented standalone JSON catalog for systems that
  do not read SafeTensors metadata.
- [ ] Compare two package manifests and show added, removed, or changed
  assets before replacing a package.
- [ ] Optional checksum manifest export for external-media folders.
- [ ] Duplicate detection by content hash across different filenames and an
  explicit user-controlled deduplication workflow.
- [ ] Optional support for additional subtitle formats after validating
  encoding, timing, preview, and round-trip behavior.

## Explicit non-goals unless separately approved

- Lossy or automatic compression of video bytes during package creation.
- Treating custom media tensors as model weights or implying that the Hub
  model viewer can play them.
- Uploading user media or access tokens without an explicit user action.
- Automatically installing application updates without user confirmation.
- Claiming a signature proves a real-world publisher identity without a
  separately trusted public key.

## How to propose a roadmap change

Open an issue or discussion with:

1. The user problem and the workflow affected.
2. Expected behavior and at least one acceptance scenario.
3. Whether the change alters the manifest, package bytes, or existing project
   files.
4. Compatibility and migration implications.
5. Relevant test cases and any external dependency required.

Changes to the package format should include a compatibility plan and fixtures
before implementation. Update this roadmap when work is accepted, when its
scope changes, and when its acceptance criteria are met.
