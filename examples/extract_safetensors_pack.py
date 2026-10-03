#!/usr/bin/env python3
"""Extract media from a PackManager SafeTensors file, locally or on the Hub."""

from __future__ import annotations

import argparse
import hashlib
import json
import re
import struct
import sys
from pathlib import Path

MAX_HEADER_SIZE = 100 * 1024 * 1024
CHUNK_SIZE = 8 * 1024 * 1024


def safe_filename(value: str, index: int, role: str) -> str:
    basename = Path(value.replace("\\", "/")).name
    basename = re.sub(r'[<>:"/\\|?*\x00-\x1f]', "_", basename).strip(" .")
    if not basename:
        basename = f"{role}.bin"
    return f"{index:04d}_{role}_{basename}"


def read_header(package_path: Path) -> tuple[dict, int, int]:
    file_size = package_path.stat().st_size
    with package_path.open("rb") as package_file:
        prefix = package_file.read(8)
        if len(prefix) != 8:
            raise ValueError("File is too small to contain a SafeTensors header.")
        header_size = struct.unpack("<Q", prefix)[0]
        if not 0 < header_size <= MAX_HEADER_SIZE or header_size > file_size - 8:
            raise ValueError("Invalid SafeTensors header size.")
        header_bytes = package_file.read(header_size)
        if len(header_bytes) != header_size:
            raise ValueError("SafeTensors header is truncated.")
    header = json.loads(header_bytes)
    if not isinstance(header, dict):
        raise ValueError("SafeTensors header must be a JSON object.")
    data_start = 8 + header_size
    data_size = file_size - data_start
    ranges = []
    for name, tensor in header.items():
        if name == "__metadata__":
            continue
        if not isinstance(tensor, dict):
            raise ValueError(f"Invalid tensor descriptor: {name!r}.")
        offsets = tensor.get("data_offsets")
        shape = tensor.get("shape")
        if (
            not isinstance(offsets, list)
            or len(offsets) != 2
            or not all(isinstance(value, int) and value >= 0 for value in offsets)
            or not isinstance(shape, list)
            or not shape
            or not all(isinstance(value, int) and value >= 0 for value in shape)
        ):
            raise ValueError(f"Invalid tensor offsets or shape: {name!r}.")
        start, end = offsets
        element_count = 1
        for dimension in shape:
            element_count *= dimension
        if end < start or end - start != element_count or end > data_size:
            raise ValueError(f"Tensor byte range is invalid: {name!r}.")
        ranges.append((start, end, name))
    ranges.sort()
    expected_offset = 0
    for start, end, name in ranges:
        if start != expected_offset:
            raise ValueError(f"Tensor ranges overlap or have gaps at {name!r}.")
        expected_offset = end
    if expected_offset != data_size:
        raise ValueError("Tensor ranges do not cover the complete data section.")
    return header, data_start, data_size


def extract_tensor(
    package_path: Path,
    output_path: Path,
    tensor: dict,
    data_start: int,
    expected_size: int | None,
    expected_hash: str | None,
) -> str:
    if tensor.get("dtype") != "U8" or len(tensor.get("shape", [])) != 1:
        raise ValueError("Media tensors must be one-dimensional U8 tensors.")
    start, end = tensor["data_offsets"]
    if expected_size is not None and end - start != expected_size:
        raise ValueError("Asset byte size does not match the manifest.")
    digest = hashlib.sha256()
    with package_path.open("rb") as source, output_path.open("wb") as destination:
        source.seek(data_start + start)
        remaining = end - start
        while remaining:
            block = source.read(min(CHUNK_SIZE, remaining))
            if not block:
                raise ValueError("Unexpected end of SafeTensors data.")
            destination.write(block)
            digest.update(block)
            remaining -= len(block)
    actual_hash = digest.hexdigest()
    if expected_hash and actual_hash.lower() != expected_hash.lower():
        output_path.unlink(missing_ok=True)
        raise ValueError("Asset SHA-256 does not match the manifest.")
    return actual_hash


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    source = parser.add_mutually_exclusive_group(required=True)
    source.add_argument("--file", type=Path, help="Local .safetensors file")
    source.add_argument("--repo-id", help="Hugging Face repository, e.g. user/repo")
    parser.add_argument("--filename", help="Repository filename when using --repo-id")
    parser.add_argument("--revision", default="main")
    parser.add_argument("--output", type=Path, default=Path("extracted-pack"))
    parser.add_argument("--token", help="Optional Hugging Face access token")
    args = parser.parse_args()

    if args.repo_id:
        if not args.filename:
            parser.error("--filename is required with --repo-id")
        try:
            from huggingface_hub import hf_hub_download
        except ImportError:
            parser.error("Install huggingface_hub to download from the Hub.")
        package_path = Path(
            hf_hub_download(
                repo_id=args.repo_id,
                filename=args.filename,
                revision=args.revision,
                token=args.token,
            )
        )
    else:
        package_path = args.file

    args.output.mkdir(parents=True, exist_ok=True)
    header, data_start, _ = read_header(package_path)
    metadata = header.get("__metadata__")
    if not isinstance(metadata, dict):
        raise ValueError("SafeTensors header has no metadata object.")
    manifest_text = metadata.get("packmanager_manifest")
    if not isinstance(manifest_text, str):
        raise ValueError("SafeTensors metadata has no packmanager_manifest.")
    manifest = json.loads(manifest_text)
    if manifest.get("format") != "packmanager.video-pack":
        raise ValueError("Unsupported package manifest format.")
    if manifest.get("media_storage", "embedded") != "embedded":
        raise ValueError("This extractor requires media embedded as tensors.")

    assets = manifest.get("assets")
    if not isinstance(assets, list):
        raise ValueError("Manifest has no assets list.")
    written_assets = []
    for index, asset in enumerate(assets):
        tensor_name = asset.get("tensor")
        if not isinstance(tensor_name, str) or tensor_name not in header:
            raise ValueError(f"Missing tensor for asset at index {index}.")
        role = str(asset.get("role", "asset"))
        filename = safe_filename(str(asset.get("filename", "")), index, role)
        output_path = args.output / filename
        actual_hash = extract_tensor(
            package_path,
            output_path,
            header[tensor_name],
            data_start,
            asset.get("size_bytes"),
            asset.get("sha256"),
        )
        written_assets.append(
            {
                **asset,
                "extracted_filename": filename,
                "verified_sha256": actual_hash,
            }
        )

    extracted_manifest = {**manifest, "assets": written_assets}
    (args.output / "manifest.json").write_text(
        json.dumps(extracted_manifest, ensure_ascii=False, indent=2) + "\n",
        encoding="utf-8",
    )
    print(f"Extracted {len(written_assets)} assets to {args.output}")
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except (OSError, ValueError, json.JSONDecodeError) as error:
        print(f"Extraction failed: {error}", file=sys.stderr)
        raise SystemExit(1)
