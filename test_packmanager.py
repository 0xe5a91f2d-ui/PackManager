import hashlib
import json
import os
import struct
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

from safetensors import safe_open


PROJECT_ROOT = Path(__file__).resolve().parent
DEFAULT_TEST_EXE = PROJECT_ROOT / "build" / "Release" / "PackManagerFormatTest.exe"
FORMAT_TEST_EXE = Path(os.environ.get("PACKMANAGER_FORMAT_TEST_EXE", DEFAULT_TEST_EXE))


def cpp_test_environment() -> dict[str, str]:
    environment = os.environ.copy()
    qt_bin = os.environ.get("PACKMANAGER_QT_BIN")
    cache_path = PROJECT_ROOT / "build" / "CMakeCache.txt"
    if not qt_bin and cache_path.is_file():
        for line in cache_path.read_text(encoding="utf-8").splitlines():
            if line.startswith("CMAKE_PREFIX_PATH:"):
                qt_bin = str(Path(line.partition("=")[2]) / "bin")
                break
    if qt_bin:
        environment["PATH"] = qt_bin + os.pathsep + environment.get("PATH", "")
    return environment


@unittest.skipUnless(FORMAT_TEST_EXE.is_file(), "Build PackManagerFormatTest first")
class CppSafeTensorsCompatibilityTests(unittest.TestCase):
    def test_project_draft_roundtrips_video_metadata_and_media_paths(self) -> None:
        with tempfile.TemporaryDirectory() as temporary_directory:
            directory = Path(temporary_directory)
            video_path = directory / "ornek video.mp4"
            cover_path = directory / "kapak.png"
            project_path = directory / "proje.pmp"
            roundtrip_path = directory / "proje-roundtrip.pmp"
            video_path.write_bytes(b"project video")
            cover_path.write_bytes(b"project cover")

            subprocess.run(
                [
                    str(FORMAT_TEST_EXE),
                    "project-create",
                    str(video_path),
                    str(cover_path),
                    str(project_path),
                ],
                check=True,
                env=cpp_test_environment(),
            )
            project = json.loads(project_path.read_text(encoding="utf-8"))
            self.assertEqual(project["format"], "packmanager.project")
            self.assertEqual(project["version"], 2)
            self.assertEqual(project["videos"][0]["title"], "Taslak video")
            self.assertEqual(project["videos"][0]["category"], "Eğitim")
            self.assertEqual(project["videos"][0]["video_path"], str(video_path))
            self.assertEqual(project["videos"][0]["cover_path"], str(cover_path))
            self.assertEqual(
                project["package"],
                {
                    "title": "Ornek paket",
                    "description": "Paket açıklaması",
                    "version": "2.3.1",
                    "created_at": "2026-05-12T10:30:00Z",
                },
            )

            subprocess.run(
                [
                    str(FORMAT_TEST_EXE),
                    "project-roundtrip",
                    str(project_path),
                    str(roundtrip_path),
                ],
                check=True,
                env=cpp_test_environment(),
            )
            roundtrip = json.loads(roundtrip_path.read_text(encoding="utf-8"))
            self.assertEqual(roundtrip["videos"], project["videos"])
            self.assertEqual(roundtrip["package"], project["package"])

    def test_package_metadata_roundtrips_in_safetensors_manifest(self) -> None:
        with tempfile.TemporaryDirectory() as temporary_directory:
            directory = Path(temporary_directory)
            video_path = directory / "metadata-video.mp4"
            package_path = directory / "metadata.safetensors"
            roundtrip_path = directory / "metadata-roundtrip.safetensors"
            video_path.write_bytes(b"metadata video")

            subprocess.run(
                [
                    str(FORMAT_TEST_EXE),
                    "package-create",
                    str(video_path),
                    str(package_path),
                ],
                check=True,
                env=cpp_test_environment(),
            )
            subprocess.run(
                [
                    str(FORMAT_TEST_EXE),
                    "roundtrip",
                    str(package_path),
                    str(roundtrip_path),
                ],
                check=True,
                env=cpp_test_environment(),
            )

            with safe_open(roundtrip_path, framework="np") as package:
                manifest = json.loads(
                    package.metadata()["packmanager_manifest"]
                )
            self.assertEqual(
                manifest["package"],
                {
                    "title": "Örnek paket",
                    "description": "Paket açıklaması",
                    "version": "2.3.1",
                    "created_at": "2026-05-12T10:30:00Z",
                },
            )

    def test_cover_can_be_extracted_from_a_safetensors_package(self) -> None:
        with tempfile.TemporaryDirectory() as temporary_directory:
            directory = Path(temporary_directory)
            video_path = directory / "extract-video.mp4"
            cover_path = directory / "extract-cover.png"
            package_path = directory / "with-cover.safetensors"
            extracted_cover = directory / "extracted-cover.png"
            video_bytes = b"video data for cover extraction"
            cover_bytes = b"cover image bytes"
            video_path.write_bytes(video_bytes)
            cover_path.write_bytes(cover_bytes)

            subprocess.run(
                [
                    str(FORMAT_TEST_EXE),
                    "package-create",
                    str(video_path),
                    str(cover_path),
                    str(package_path),
                ],
                check=True,
                env=cpp_test_environment(),
            )
            subprocess.run(
                [
                    str(FORMAT_TEST_EXE),
                    "extract-cover",
                    str(package_path),
                    str(extracted_cover),
                ],
                check=True,
                env=cpp_test_environment(),
            )
            self.assertEqual(extracted_cover.read_bytes(), cover_bytes)

    def test_media_hashes_are_checked_and_signed_packages_detect_changes(self) -> None:
        with tempfile.TemporaryDirectory() as temporary_directory:
            directory = Path(temporary_directory)
            video_path = directory / "signed-video.mp4"
            package_path = directory / "signed.safetensors"
            video_path.write_bytes(b"signed video payload")

            subprocess.run(
                [str(FORMAT_TEST_EXE), "signed-create", str(video_path), str(package_path)],
                check=True,
                env=cpp_test_environment(),
            )
            with safe_open(package_path, framework="np") as package:
                manifest = json.loads(package.metadata()["packmanager_manifest"])
            self.assertEqual(manifest["version"], 2)
            self.assertEqual(
                manifest["videos"][0]["video_sha256"],
                hashlib.sha256(video_path.read_bytes()).hexdigest(),
            )

            valid = subprocess.run(
                [str(FORMAT_TEST_EXE), "validate", str(package_path)],
                check=True,
                capture_output=True,
                text=True,
                env=cpp_test_environment(),
            )
            self.assertEqual(valid.stdout.strip(), "signed-valid")

            data_start = 8 + struct.unpack("<Q", package_path.read_bytes()[:8])[0]
            with package_path.open("r+b") as package_file:
                package_file.seek(data_start)
                first_byte = package_file.read(1)
                package_file.seek(data_start)
                package_file.write(bytes([first_byte[0] ^ 1]))
            invalid = subprocess.run(
                [str(FORMAT_TEST_EXE), "validate", str(package_path)],
                check=False,
                capture_output=True,
                text=True,
                env=cpp_test_environment(),
            )
            self.assertNotEqual(invalid.returncode, 0)

    def test_external_media_packages_are_portable_and_roundtrip_to_embedded(self) -> None:
        with tempfile.TemporaryDirectory() as temporary_directory:
            directory = Path(temporary_directory)
            video_path = directory / "external-video.mp4"
            cover_path = directory / "external-cover.png"
            subtitle_path = directory / "external-subtitle.srt"
            package_path = directory / "external.safetensors"
            roundtrip_path = directory / "embedded-roundtrip.safetensors"
            video_bytes = b"large external media payload"
            cover_bytes = b"external cover payload"
            subtitle_bytes = b"1\n00:00:01,000 --> 00:00:02,000\nHello\n"
            video_path.write_bytes(video_bytes)
            cover_path.write_bytes(cover_bytes)
            subtitle_path.write_bytes(subtitle_bytes)

            subprocess.run(
                [
                    str(FORMAT_TEST_EXE),
                    "external-create",
                    str(video_path),
                    str(cover_path),
                    str(subtitle_path),
                    str(package_path),
                ],
                check=True,
                env=cpp_test_environment(),
            )
            with safe_open(package_path, framework="np") as package:
                self.assertEqual(package.keys(), [])
                manifest = json.loads(package.metadata()["packmanager_manifest"])
            external_path = directory / manifest["videos"][0]["video_external_path"]
            self.assertEqual(external_path.read_bytes(), video_bytes)
            self.assertEqual(manifest["videos"][0]["video_tensor"], None)
            self.assertEqual(
                (directory / manifest["videos"][0]["cover_external_path"]).read_bytes(),
                cover_bytes,
            )
            self.assertEqual(
                (directory / manifest["videos"][0]["subtitle_external_path"]).read_bytes(),
                subtitle_bytes,
            )
            self.assertEqual(manifest["videos"][0]["tags"], ["test", "external"])
            self.assertEqual(manifest["videos"][0]["collection"], "compatibility")

            subprocess.run(
                [str(FORMAT_TEST_EXE), "validate", str(package_path)],
                check=True,
                env=cpp_test_environment(),
            )
            subprocess.run(
                [str(FORMAT_TEST_EXE), "roundtrip", str(package_path), str(roundtrip_path)],
                check=True,
                env=cpp_test_environment(),
            )
            with safe_open(roundtrip_path, framework="np") as package:
                self.assertEqual(package.get_tensor("video_0000").tobytes(), video_bytes)
                self.assertEqual(package.get_tensor("cover_0000").tobytes(), cover_bytes)
                self.assertEqual(
                    package.get_tensor("subtitle_0000").tobytes(), subtitle_bytes
                )
                roundtrip_manifest = json.loads(
                    package.metadata()["packmanager_manifest"]
                )
            self.assertEqual(roundtrip_manifest["videos"][0]["tags"], ["test", "external"])
            self.assertEqual(roundtrip_manifest["videos"][0]["collection"], "compatibility")

    def test_external_media_reader_rejects_paths_outside_package_folder(self) -> None:
        with tempfile.TemporaryDirectory() as temporary_directory:
            directory = Path(temporary_directory)
            package_path = directory / "unsafe.safetensors"
            output_path = directory / "must-not-exist.safetensors"
            manifest = {
                "format": "packmanager.video-pack",
                "version": 2,
                "videos": [
                    {
                        "title": "Unsafe path",
                        "description": "",
                        "category": "Test",
                        "video_tensor": None,
                        "video_external_path": "../outside.mp4",
                        "cover_tensor": None,
                        "subtitle_tensor": None,
                    }
                ],
            }
            header = {
                "__metadata__": {
                    "packmanager_manifest": json.dumps(manifest, separators=(",", ":"))
                }
            }
            header_bytes = json.dumps(header, separators=(",", ":")).encode("utf-8")
            header_bytes += b" " * (-len(header_bytes) % 8)
            package_path.write_bytes(struct.pack("<Q", len(header_bytes)) + header_bytes)

            result = subprocess.run(
                [
                    str(FORMAT_TEST_EXE),
                    "roundtrip",
                    str(package_path),
                    str(output_path),
                ],
                check=False,
                capture_output=True,
                text=True,
                env=cpp_test_environment(),
            )
            self.assertNotEqual(result.returncode, 0)
            self.assertIn("Harici video yolu", result.stderr)
            self.assertFalse(output_path.exists())

    def test_project_reader_rejects_corrupt_draft(self) -> None:
        with tempfile.TemporaryDirectory() as temporary_directory:
            directory = Path(temporary_directory)
            corrupt_path = directory / "corrupt.pmp"
            output_path = directory / "output.pmp"
            corrupt_path.write_text("{ not json", encoding="utf-8")

            result = subprocess.run(
                [
                    str(FORMAT_TEST_EXE),
                    "project-roundtrip",
                    str(corrupt_path),
                    str(output_path),
                ],
                check=False,
                env=cpp_test_environment(),
            )
            self.assertNotEqual(result.returncode, 0)
            self.assertFalse(output_path.exists())

    def test_cpp_reader_accepts_standard_safetensors_packages(self) -> None:
        with tempfile.TemporaryDirectory() as temporary_directory:
            directory = Path(temporary_directory)
            package_path = directory / "third-party.safetensors"
            roundtrip_path = directory / "packmanager-roundtrip.safetensors"
            video_bytes = b"standard-video"
            cover_bytes = b"standard-cover"
            manifest = {
                "format": "packmanager.video-pack",
                "version": 1,
                "videos": [
                    {
                        "title": "Üçüncü taraf paket",
                        "description": "Standart SafeTensors başlığı",
                        "category": "Eğitim",
                        "video_tensor": "video_0000",
                        "cover_tensor": "cover_0000",
                    }
                ],
            }
            header = {
                "__metadata__": {
                    "packmanager_manifest": json.dumps(
                        manifest, ensure_ascii=False, separators=(",", ":")
                    )
                },
                "video_0000": {
                    "dtype": "U8",
                    "shape": [len(video_bytes)],
                    "data_offsets": [0, len(video_bytes)],
                },
                "cover_0000": {
                    "dtype": "U8",
                    "shape": [len(cover_bytes)],
                    "data_offsets": [
                        len(video_bytes),
                        len(video_bytes) + len(cover_bytes),
                    ],
                },
            }
            header_bytes = json.dumps(
                header, ensure_ascii=False, separators=(",", ":")
            ).encode("utf-8")
            header_bytes += b" " * (-len(header_bytes) % 8)
            package_path.write_bytes(
                struct.pack("<Q", len(header_bytes))
                + header_bytes
                + video_bytes
                + cover_bytes
            )

            subprocess.run(
                [
                    str(FORMAT_TEST_EXE),
                    "roundtrip",
                    str(package_path),
                    str(roundtrip_path),
                ],
                check=True,
                env=cpp_test_environment(),
            )

            with safe_open(roundtrip_path, framework="np") as result:
                self.assertEqual(
                    result.get_tensor("video_0000").tobytes(), video_bytes
                )
                self.assertEqual(
                    result.get_tensor("cover_0000").tobytes(), cover_bytes
                )
                result_manifest = json.loads(
                    result.metadata()["packmanager_manifest"]
                )
            self.assertEqual(
                result_manifest["videos"][0]["title"], "Üçüncü taraf paket"
            )
            self.assertEqual(
                result_manifest["videos"][0]["video_filename"],
                "Üçüncü taraf paket.mp4",
            )
            self.assertEqual(
                result_manifest["videos"][0]["cover_filename"],
                "Üçüncü taraf paket.img",
            )

    def test_cpp_export_is_readable_and_preserves_assets_and_json(self) -> None:
        with tempfile.TemporaryDirectory() as temporary_directory:
            directory = Path(temporary_directory)
            video_bytes = b"video-content"
            cover_bytes = b"cover-content"
            video_path = directory / "video.mp4"
            cover_path = directory / "cover.png"
            package_path = directory / "pack.safetensors"
            video_path.write_bytes(video_bytes)
            cover_path.write_bytes(cover_bytes)

            subprocess.run(
                [
                    str(FORMAT_TEST_EXE),
                    str(video_path),
                    str(cover_path),
                    str(package_path),
                ],
                check=True,
                env=cpp_test_environment(),
            )

            with safe_open(package_path, framework="np") as package:
                self.assertEqual(set(package.keys()), {"video_0000", "cover_0000"})
                self.assertEqual(package.get_tensor("video_0000").tobytes(), video_bytes)
                self.assertEqual(package.get_tensor("cover_0000").tobytes(), cover_bytes)
                package_metadata = package.metadata()
                manifest = json.loads(package_metadata["packmanager_manifest"])

            self.assertEqual(manifest["format"], "packmanager.video-pack")
            self.assertEqual(manifest["version"], 2)
            self.assertEqual(manifest["videos"][0]["title"], "Örnek video")
            self.assertEqual(manifest["videos"][0]["description"], "Türkçe açıklama")
            self.assertEqual(manifest["videos"][0]["category"], "Eğitim")
            self.assertEqual(manifest["videos"][0]["video_tensor"], "video_0000")
            self.assertEqual(manifest["videos"][0]["cover_tensor"], "cover_0000")
            self.assertEqual(manifest["videos"][0]["video_filename"], "video.mp4")
            self.assertEqual(manifest["videos"][0]["cover_filename"], "cover.png")
            self.assertEqual(manifest["media_storage"], "embedded")
            self.assertEqual(
                manifest["assets"],
                [
                    {
                        "id": manifest["videos"][0]["id"] + ":video",
                        "role": "video",
                        "tensor": "video_0000",
                        "filename": "video.mp4",
                        "media_type": "video/mp4",
                        "size_bytes": len(video_bytes),
                        "sha256": hashlib.sha256(video_bytes).hexdigest(),
                        "storage": "tensor",
                    },
                    {
                        "id": manifest["videos"][0]["id"] + ":cover",
                        "role": "cover",
                        "tensor": "cover_0000",
                        "filename": "cover.png",
                        "media_type": "image/png",
                        "size_bytes": len(cover_bytes),
                        "sha256": hashlib.sha256(cover_bytes).hexdigest(),
                        "storage": "tensor",
                    },
                ],
            )
            self.assertEqual(package_metadata["format"], "packmanager.video-pack")

            with package_path.open("rb") as package_file:
                header_size = struct.unpack("<Q", package_file.read(8))[0]
            self.assertEqual(header_size % 8, 0)

            roundtrip_path = directory / "roundtrip.safetensors"
            subprocess.run(
                [
                    str(FORMAT_TEST_EXE),
                    "roundtrip",
                    str(package_path),
                    str(roundtrip_path),
                ],
                check=True,
                env=cpp_test_environment(),
            )
            with safe_open(roundtrip_path, framework="np") as roundtrip:
                self.assertEqual(
                    roundtrip.get_tensor("video_0000").tobytes(), video_bytes
                )
                self.assertEqual(
                    roundtrip.get_tensor("cover_0000").tobytes(), cover_bytes
                )
                roundtrip_manifest = json.loads(
                    roundtrip.metadata()["packmanager_manifest"]
                )
            self.assertEqual(
                roundtrip_manifest["videos"][0]["video_filename"], "video.mp4"
            )

            extracted_video_path = directory / "extracted.mp4"
            subprocess.run(
                [
                    str(FORMAT_TEST_EXE),
                    "extract",
                    str(package_path),
                    str(extracted_video_path),
                ],
                check=True,
                env=cpp_test_environment(),
            )
            self.assertEqual(extracted_video_path.read_bytes(), video_bytes)

    def test_huggingface_extractor_reads_embedded_media_and_manifest(self) -> None:
        with tempfile.TemporaryDirectory() as temporary_directory:
            directory = Path(temporary_directory)
            video_path = directory / "shared-name.mp4"
            cover_path = directory / "shared-name.png"
            package_path = directory / "huggingface-pack.safetensors"
            output_directory = directory / "extracted"
            video_bytes = b"hub-compatible-video"
            cover_bytes = b"hub-compatible-cover"
            video_path.write_bytes(video_bytes)
            cover_path.write_bytes(cover_bytes)
            subprocess.run(
                [
                    str(FORMAT_TEST_EXE),
                    str(video_path),
                    str(cover_path),
                    str(package_path),
                ],
                check=True,
                env=cpp_test_environment(),
            )
            subprocess.run(
                [
                    sys.executable,
                    str(PROJECT_ROOT / "examples" / "extract_safetensors_pack.py"),
                    "--file",
                    str(package_path),
                    "--output",
                    str(output_directory),
                ],
                check=True,
                env=cpp_test_environment(),
            )

            extracted_video = next(output_directory.glob("*video_shared-name.mp4"))
            extracted_cover = next(output_directory.glob("*cover_shared-name.png"))
            self.assertEqual(extracted_video.read_bytes(), video_bytes)
            self.assertEqual(extracted_cover.read_bytes(), cover_bytes)
            extracted_manifest = json.loads(
                (output_directory / "manifest.json").read_text(encoding="utf-8")
            )
            self.assertEqual(len(extracted_manifest["assets"]), 2)

    def test_cpp_export_supports_missing_cover(self) -> None:
        with tempfile.TemporaryDirectory() as temporary_directory:
            directory = Path(temporary_directory)
            video_path = directory / "video.mp4"
            package_path = directory / "pack.safetensors"
            video_path.write_bytes(b"video")
            subprocess.run(
                [str(FORMAT_TEST_EXE), str(video_path), "-", str(package_path)],
                check=True,
                env=cpp_test_environment(),
            )

            with safe_open(package_path, framework="np") as package:
                self.assertEqual(set(package.keys()), {"video_0000"})
                manifest = json.loads(package.metadata()["packmanager_manifest"])
            self.assertIsNone(manifest["videos"][0]["cover_tensor"])
            self.assertEqual(manifest["videos"][0]["video_filename"], "video.mp4")

    def test_cpp_reader_rejects_non_package_files(self) -> None:
        with tempfile.TemporaryDirectory() as temporary_directory:
            directory = Path(temporary_directory)
            invalid_package = directory / "invalid.safetensors"
            output_package = directory / "output.safetensors"
            invalid_package.write_bytes(b"not a safetensors file")

            result = subprocess.run(
                [
                    str(FORMAT_TEST_EXE),
                    "roundtrip",
                    str(invalid_package),
                    str(output_package),
                ],
                check=False,
                env=cpp_test_environment(),
            )
            self.assertNotEqual(result.returncode, 0)
            self.assertFalse(output_package.exists())

    def test_cpp_writer_replaces_video_and_cover_without_losing_other_asset(self) -> None:
        with tempfile.TemporaryDirectory() as temporary_directory:
            directory = Path(temporary_directory)
            video_path = directory / "original.mp4"
            cover_path = directory / "original.png"
            replacement_video_path = directory / "replacement.mkv"
            replacement_cover_path = directory / "replacement.jpg"
            package_path = directory / "original.safetensors"
            replaced_video_path = directory / "replaced-video.safetensors"
            replaced_cover_path = directory / "replaced-cover.safetensors"
            original_video = b"original-video"
            original_cover = b"original-cover"
            replacement_video = b"replacement-video"
            replacement_cover = b"replacement-cover"
            video_path.write_bytes(original_video)
            cover_path.write_bytes(original_cover)
            replacement_video_path.write_bytes(replacement_video)
            replacement_cover_path.write_bytes(replacement_cover)

            subprocess.run(
                [
                    str(FORMAT_TEST_EXE),
                    str(video_path),
                    str(cover_path),
                    str(package_path),
                ],
                check=True,
                env=cpp_test_environment(),
            )
            subprocess.run(
                [
                    str(FORMAT_TEST_EXE),
                    "replace-video",
                    str(package_path),
                    str(replacement_video_path),
                    str(replaced_video_path),
                ],
                check=True,
                env=cpp_test_environment(),
            )
            with safe_open(replaced_video_path, framework="np") as replaced_video_pack:
                self.assertEqual(
                    replaced_video_pack.get_tensor("video_0000").tobytes(),
                    replacement_video,
                )
                self.assertEqual(
                    replaced_video_pack.get_tensor("cover_0000").tobytes(),
                    original_cover,
                )
                video_manifest = json.loads(
                    replaced_video_pack.metadata()["packmanager_manifest"]
                )
            self.assertEqual(
                video_manifest["videos"][0]["video_filename"], "replacement.mkv"
            )

            subprocess.run(
                [
                    str(FORMAT_TEST_EXE),
                    "replace-cover",
                    str(replaced_video_path),
                    str(replacement_cover_path),
                    str(replaced_cover_path),
                ],
                check=True,
                env=cpp_test_environment(),
            )
            with safe_open(replaced_cover_path, framework="np") as replaced_cover_pack:
                self.assertEqual(
                    replaced_cover_pack.get_tensor("video_0000").tobytes(),
                    replacement_video,
                )
                self.assertEqual(
                    replaced_cover_pack.get_tensor("cover_0000").tobytes(),
                    replacement_cover,
                )
                cover_manifest = json.loads(
                    replaced_cover_pack.metadata()["packmanager_manifest"]
                )
            self.assertEqual(
                cover_manifest["videos"][0]["cover_filename"], "replacement.jpg"
            )

    def test_cpp_reader_can_save_changes_over_the_opened_package(self) -> None:
        with tempfile.TemporaryDirectory() as temporary_directory:
            directory = Path(temporary_directory)
            video_path = directory / "video.mp4"
            cover_path = directory / "cover.png"
            package_path = directory / "package.safetensors"
            video_bytes = b"video-content"
            cover_bytes = b"cover-content"
            video_path.write_bytes(video_bytes)
            cover_path.write_bytes(cover_bytes)
            subprocess.run(
                [
                    str(FORMAT_TEST_EXE),
                    str(video_path),
                    str(cover_path),
                    str(package_path),
                ],
                check=True,
                env=cpp_test_environment(),
            )
            subprocess.run(
                [
                    str(FORMAT_TEST_EXE),
                    "roundtrip",
                    str(package_path),
                    str(package_path),
                ],
                check=True,
                env=cpp_test_environment(),
            )
            with safe_open(package_path, framework="np") as package:
                self.assertEqual(package.get_tensor("video_0000").tobytes(), video_bytes)
                self.assertEqual(package.get_tensor("cover_0000").tobytes(), cover_bytes)


if __name__ == "__main__":
    unittest.main()
