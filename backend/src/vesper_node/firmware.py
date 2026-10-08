"""Firmware updates for the nodes (task 13 / F4): the one published image and its manifest.

The operator publishes a build on the Studio (``vesper-node firmware publish <bin>``). Nodes
ask ``GET /firmware/manifest`` and, when the published version is newer than the one they
run, download ``GET /firmware/<sha256>.bin`` and install it with the SDK's kept ``ota.c``.
Both routes need the same credentials as ``/turn``: the shared bearer plus the node's own
``X-Node-Credential`` (``app.BearerAuth``). The wire shape is in
``docs/node-wire-protocol.md`` (*Firmware updates*).

The store is one directory (default ``~/.config/vesper-voice/firmware``, override
``VESPER_NODE_FIRMWARE_DIR``), mode 700:

* ``<sha256>.bin``: the published image, mode 600, named by its SHA-256;
* ``manifest.json``: ``{"version", "sha256", "size", "published_at"}``, mode 600, written
  last (temp file + ``fsync`` + ``os.replace``), so a node sees the old release or the new
  one, never half of one.

One image is published at a time: the whole fleet runs one firmware (only the NVS identity
differs per node). Publishing keeps the previous image file for downloads already under way
and removes older ones. ``withdraw`` removes the manifest, so nodes see "nothing published"
(204) and stay on what they run.

What ``publish`` checks before a node ever sees the image (the node checks again, and the
bootloader-side checks in ``esp_https_ota`` verify the image's own SHA-256 and its RSA
signature):

* an ESP-IDF app image (magic ``0xE9``) for the ESP32-S3 (chip id 9) with the app
  descriptor (magic ``0xABCD5432``) of the ``muse-gadget`` project;
* the descriptor's version is plain ``MAJOR.MINOR.PATCH`` (the node compares versions
  numerically and never installs one that is not newer);
* at most 4 MiB (an app slot of the AIPI's partition table);
* newer than the version already published, unless ``force`` is given (a node still refuses
  anything not newer than what it runs).
"""

from __future__ import annotations

import contextlib
import hashlib
import json
import os
import re
import stat
import struct
import tempfile
import time
from dataclasses import asdict, dataclass
from pathlib import Path
from typing import Any

MANIFEST_NAME = "manifest.json"
MAX_IMAGE_BYTES = 4 * 1024 * 1024  # ota_0/ota_1 in partitions_muse.csv
IMAGE_MAGIC = 0xE9
ESP32S3_CHIP_ID = 9
APP_DESC_MAGIC = 0xABCD5432
APP_DESC_OFFSET = 24 + 8  # esp_image_header_t + the first esp_image_segment_header_t
PROJECT_NAME = "muse-gadget"
VERSION_RE = re.compile(r"^(0|[1-9][0-9]{0,4})\.(0|[1-9][0-9]{0,4})\.(0|[1-9][0-9]{0,4})$")
SHA_RE = re.compile(r"^[0-9a-f]{64}$")
IMAGE_NAME_RE = re.compile(r"^([0-9a-f]{64})\.bin$")


class FirmwareError(Exception):
    """A refused publish, or an unusable store. ``code`` is short and safe to print."""

    def __init__(self, code: str, detail: str = "") -> None:
        super().__init__(f"{code}: {detail}" if detail else code)
        self.code = code
        self.detail = detail


def parse_version(version: str) -> tuple[int, int, int] | None:
    """``"1.2.3"`` -> ``(1, 2, 3)``; anything else (and any part over 65535) -> None."""
    m = VERSION_RE.fullmatch(version)
    if not m:
        return None
    parts = tuple(int(g) for g in m.groups())
    if any(p > 0xFFFF for p in parts):
        return None
    return parts[0], parts[1], parts[2]


@dataclass(frozen=True)
class AppInfo:
    version: str
    project: str
    chip_id: int


def _cstr(raw: bytes) -> str:
    return raw.split(b"\0", 1)[0].decode("ascii", errors="replace")


def read_app_info(image: bytes) -> AppInfo:
    """The version, project and chip id of an ESP-IDF app image. Raises :class:`FirmwareError`."""
    if len(image) < APP_DESC_OFFSET + 256:
        raise FirmwareError("not_an_app_image", "too short")
    if image[0] != IMAGE_MAGIC:
        raise FirmwareError("not_an_app_image", "bad image magic")
    (chip_id,) = struct.unpack_from("<H", image, 12)
    (desc_magic,) = struct.unpack_from("<I", image, APP_DESC_OFFSET)
    if desc_magic != APP_DESC_MAGIC:
        raise FirmwareError("not_an_app_image", "no app descriptor")
    version = _cstr(image[APP_DESC_OFFSET + 16 : APP_DESC_OFFSET + 48])
    project = _cstr(image[APP_DESC_OFFSET + 48 : APP_DESC_OFFSET + 80])
    return AppInfo(version=version, project=project, chip_id=chip_id)


@dataclass(frozen=True)
class Manifest:
    version: str
    sha256: str
    size: int
    published_at: int

    def wire(self) -> dict[str, Any]:
        return asdict(self)


def _check_private_dir(path: Path) -> None:
    st = os.lstat(path)
    if not stat.S_ISDIR(st.st_mode) or stat.S_IMODE(st.st_mode) & 0o077:
        raise FirmwareError(
            "store_insecure", f"{path} must be a directory with mode 700 (run: chmod 700 {path})"
        )


def _read_private_file(path: Path, limit: int) -> bytes | None:
    """The file's bytes if it is a regular mode-600 file; None if it doesn't exist."""
    try:
        fd = os.open(path, os.O_RDONLY | getattr(os, "O_NOFOLLOW", 0))
    except FileNotFoundError:
        return None
    except OSError as e:
        raise FirmwareError("store_unreadable", str(path)) from e
    with os.fdopen(fd, "rb") as fh:
        st = os.fstat(fh.fileno())
        if not stat.S_ISREG(st.st_mode) or stat.S_IMODE(st.st_mode) & 0o077:
            raise FirmwareError("store_insecure", f"{path} must be a regular file with mode 600")
        if st.st_size > limit:
            raise FirmwareError("store_corrupt", f"{path} is too large")
        return fh.read(limit + 1)


def _write_private_file(directory: Path, name: str, payload: bytes) -> None:
    fd, tmp = tempfile.mkstemp(dir=directory, prefix=f".{name}.")
    try:
        os.fchmod(fd, 0o600)
        with os.fdopen(fd, "wb") as fh:
            fh.write(payload)
            fh.flush()
            os.fsync(fh.fileno())
        os.replace(tmp, directory / name)
    except BaseException:
        with contextlib.suppress(FileNotFoundError):
            os.unlink(tmp)
        raise
    dfd = os.open(directory, os.O_RDONLY)
    try:
        os.fsync(dfd)
    finally:
        os.close(dfd)


class FirmwareStore:
    """The published firmware. Reads are per request (a publish shows up without a restart)."""

    def __init__(self, directory: Path | str) -> None:
        self.dir = Path(directory).expanduser()

    def _ensure_dir(self) -> None:
        if not self.dir.exists():
            self.dir.mkdir(mode=0o700, parents=True, exist_ok=True)
        _check_private_dir(self.dir)

    def current(self) -> Manifest | None:
        """The published release, or None. Raises :class:`FirmwareError` if the store is bad."""
        if not self.dir.exists():
            return None
        _check_private_dir(self.dir)
        raw = _read_private_file(self.dir / MANIFEST_NAME, 4096)
        if raw is None:
            return None
        try:
            data = json.loads(raw.decode("utf-8"))
            manifest = Manifest(
                version=str(data["version"]),
                sha256=str(data["sha256"]),
                size=int(data["size"]),
                published_at=int(data["published_at"]),
            )
        except (ValueError, KeyError, TypeError, UnicodeDecodeError) as e:
            raise FirmwareError("store_corrupt", "manifest.json is malformed") from e
        if (
            parse_version(manifest.version) is None
            or not SHA_RE.fullmatch(manifest.sha256)
            or not 0 < manifest.size <= MAX_IMAGE_BYTES
        ):
            raise FirmwareError("store_corrupt", "manifest.json is malformed")
        return manifest

    def image(self, sha256: str) -> tuple[Manifest, bytes] | None:
        """The published image if ``sha256`` names it (and it still hashes to it), else None.

        Only the current release is served: an image named by any other hash, or a name that is
        not 64 lowercase hex digits, is not found. The file is re-hashed on every read, so a
        damaged or swapped file is never sent.
        """
        if not SHA_RE.fullmatch(sha256):
            return None
        manifest = self.current()
        if manifest is None or manifest.sha256 != sha256:
            return None
        data = _read_private_file(self.dir / f"{sha256}.bin", MAX_IMAGE_BYTES)
        if data is None or len(data) != manifest.size:
            raise FirmwareError("store_corrupt", "the published image is missing or truncated")
        if hashlib.sha256(data).hexdigest() != sha256:
            raise FirmwareError("store_corrupt", "the published image does not match its hash")
        return manifest, data

    def publish(self, image_path: Path | str, *, force: bool = False) -> Manifest:
        """Validate and publish a build. Raises :class:`FirmwareError` (nothing changes then)."""
        src = Path(image_path).expanduser()
        try:
            size = src.stat().st_size
        except OSError as e:
            raise FirmwareError("no_such_file", str(src)) from e
        if size > MAX_IMAGE_BYTES:
            raise FirmwareError("too_large", f"{size} bytes, at most {MAX_IMAGE_BYTES}")
        data = src.read_bytes()
        if len(data) > MAX_IMAGE_BYTES:
            raise FirmwareError("too_large", f"at most {MAX_IMAGE_BYTES} bytes")
        info = read_app_info(data)
        if info.chip_id != ESP32S3_CHIP_ID:
            raise FirmwareError("wrong_chip", f"chip id {info.chip_id}, the nodes are ESP32-S3")
        if info.project != PROJECT_NAME:
            raise FirmwareError("wrong_project", f"project {info.project!r}")
        new_version = parse_version(info.version)
        if new_version is None:
            raise FirmwareError(
                "bad_version",
                f"version {info.version!r} is not MAJOR.MINOR.PATCH "
                "(set firmware/hatch/VERSION and rebuild)",
            )
        self._ensure_dir()
        previous = self.current()
        if previous is not None and not force:
            old_version = parse_version(previous.version)
            if old_version is not None and new_version <= old_version:
                raise FirmwareError(
                    "not_newer",
                    f"{info.version} is not newer than the published {previous.version} "
                    "(nodes never install an older or equal version; bump firmware/hatch/VERSION)",
                )
        sha = hashlib.sha256(data).hexdigest()
        _write_private_file(self.dir, f"{sha}.bin", data)
        manifest = Manifest(
            version=info.version, sha256=sha, size=len(data), published_at=int(time.time())
        )
        _write_private_file(
            self.dir, MANIFEST_NAME, json.dumps(manifest.wire(), indent=2).encode() + b"\n"
        )
        keep = {f"{sha}.bin"} | ({f"{previous.sha256}.bin"} if previous else set())
        for old in self.dir.glob("*.bin"):
            if old.name not in keep and IMAGE_NAME_RE.fullmatch(old.name):
                with contextlib.suppress(FileNotFoundError):
                    old.unlink()
        return manifest

    def withdraw(self) -> Manifest | None:
        """Unpublish (nodes then see 204 and keep what they run). Returns what was published."""
        manifest = self.current()
        if manifest is not None:
            (self.dir / MANIFEST_NAME).unlink()
        return manifest
