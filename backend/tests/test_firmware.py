"""Task 13: node firmware updates — the store, the routes behind node auth, and the CLI."""

from __future__ import annotations

import hashlib
import json
import logging
import stat
import struct

import pytest
from conftest import NODE_CREDENTIAL, NODE_ID, auth, bearer, client_for

from vesper_node.app import main
from vesper_node.firmware import (
    MAX_IMAGE_BYTES,
    FirmwareError,
    FirmwareStore,
    parse_version,
    read_app_info,
)


def make_image(
    version: str = "1.0.1",
    *,
    project: str = "muse-gadget",
    chip_id: int = 9,
    size: int = 8192,
    seed: bytes = b"",
) -> bytes:
    """A minimal ESP-IDF app image: image header, one segment header, the app descriptor."""
    header = bytes([0xE9, 1, 2, 0x2F]) + struct.pack("<I", 0x40370000) + bytes([0xEE, 0, 0, 0])
    header += struct.pack("<H", chip_id) + bytes(9) + bytes([1])
    assert len(header) == 24
    segment = struct.pack("<II", 0x3C000020, 256)
    desc = struct.pack("<IIII", 0xABCD5432, 0, 0, 0)
    desc += version.encode().ljust(32, b"\0") + project.encode().ljust(32, b"\0")
    body = header + segment + desc + seed
    return body + bytes(size - len(body))


@pytest.fixture
def store(tmp_path) -> FirmwareStore:
    return FirmwareStore(tmp_path / "fw")


def write_bin(tmp_path, data: bytes, name: str = "muse-gadget.bin"):
    path = tmp_path / name
    path.write_bytes(data)
    return path


# ---- the image and the store ----------------------------------------------------------------


def test_parse_version() -> None:
    assert parse_version("1.0.1") == (1, 0, 1)
    assert parse_version("0.0.0") == (0, 0, 0)
    assert parse_version("65535.0.9") == (65535, 0, 9)
    bad_versions = ("1.0", "1.0.0.0", "v1.0.0", "01.0.0", "1.0.0-dirty", "65536.0.0", "", "1..0")
    for bad in (*bad_versions, " 1.0.0"):
        assert parse_version(bad) is None, bad


def test_read_app_info() -> None:
    info = read_app_info(make_image("2.3.4"))
    assert (info.version, info.project, info.chip_id) == ("2.3.4", "muse-gadget", 9)
    with pytest.raises(FirmwareError, match="not_an_app_image"):
        read_app_info(b"\x00" + make_image()[1:])
    with pytest.raises(FirmwareError, match="not_an_app_image"):
        read_app_info(make_image()[:100])
    bad_desc = bytearray(make_image())
    bad_desc[32] ^= 0xFF
    with pytest.raises(FirmwareError, match="not_an_app_image"):
        read_app_info(bytes(bad_desc))


def test_publish_writes_private_files_and_manifest(store, tmp_path) -> None:
    data = make_image("1.0.1")
    m = store.publish(write_bin(tmp_path, data))
    assert (m.version, m.size, m.sha256) == ("1.0.1", len(data), hashlib.sha256(data).hexdigest())
    assert stat.S_IMODE(store.dir.stat().st_mode) == 0o700
    for f in store.dir.iterdir():
        assert stat.S_IMODE(f.stat().st_mode) == 0o600, f
    assert json.loads((store.dir / "manifest.json").read_text())["sha256"] == m.sha256
    assert store.current() == m
    got = store.image(m.sha256)
    assert got is not None and got[1] == data


@pytest.mark.parametrize(
    ("image", "code"),
    [
        (make_image(chip_id=5), "wrong_chip"),
        (make_image(project="hello-world"), "wrong_project"),
        (make_image("1.0"), "bad_version"),
        (make_image("v1.0.0-15-gabc-dirty"), "bad_version"),
        (b"\x7fELF" + bytes(4000), "not_an_app_image"),
    ],
)
def test_publish_refuses_bad_images(store, tmp_path, image, code) -> None:
    with pytest.raises(FirmwareError) as e:
        store.publish(write_bin(tmp_path, image))
    assert e.value.code == code
    assert store.current() is None


def test_publish_refuses_oversized_image(store, tmp_path) -> None:
    with pytest.raises(FirmwareError) as e:
        store.publish(write_bin(tmp_path, make_image(size=MAX_IMAGE_BYTES + 1)))
    assert e.value.code == "too_large"


def test_publish_refuses_older_or_equal_unless_forced(store, tmp_path) -> None:
    store.publish(write_bin(tmp_path, make_image("1.2.0")))
    for version in ("1.2.0", "1.1.9", "0.9.9"):
        with pytest.raises(FirmwareError) as e:
            store.publish(write_bin(tmp_path, make_image(version, seed=version.encode())))
        assert e.value.code == "not_newer"
    assert store.current().version == "1.2.0"
    assert store.publish(write_bin(tmp_path, make_image("1.1.9")), force=True).version == "1.1.9"


def test_publish_keeps_previous_image_and_prunes_older(store, tmp_path) -> None:
    shas = [
        store.publish(write_bin(tmp_path, make_image(f"1.0.{i}", seed=bytes([i])))).sha256
        for i in range(1, 4)
    ]
    names = sorted(p.name for p in store.dir.glob("*.bin"))
    assert names == sorted(f"{s}.bin" for s in shas[1:])
    # Only the current release is served, never the kept previous one.
    assert store.image(shas[1]) is None
    assert store.image(shas[2]) is not None


def test_withdraw(store, tmp_path) -> None:
    assert store.withdraw() is None
    store.publish(write_bin(tmp_path, make_image("1.0.1")))
    assert store.withdraw().version == "1.0.1"
    assert store.current() is None


def test_store_refuses_insecure_modes(store, tmp_path) -> None:
    m = store.publish(write_bin(tmp_path, make_image()))
    store.dir.chmod(0o755)
    with pytest.raises(FirmwareError, match="store_insecure"):
        store.current()
    store.dir.chmod(0o700)
    (store.dir / f"{m.sha256}.bin").chmod(0o644)
    with pytest.raises(FirmwareError, match="store_insecure"):
        store.image(m.sha256)


def test_tampered_image_is_never_served(store, tmp_path) -> None:
    m = store.publish(write_bin(tmp_path, make_image()))
    path = store.dir / f"{m.sha256}.bin"
    data = bytearray(path.read_bytes())
    data[-1] ^= 1
    path.write_bytes(bytes(data))
    with pytest.raises(FirmwareError, match="store_corrupt"):
        store.image(m.sha256)


# ---- the routes ---------------------------------------------------------------------------


@pytest.fixture
def fw_app(build, store):
    return build(firmware=store)


async def test_manifest_204_when_nothing_published(fw_app) -> None:
    async with client_for(fw_app) as c:
        r = await c.get("/firmware/manifest", headers=auth())
    assert r.status_code == 204
    assert r.content == b""


async def test_manifest_without_a_store_is_204(build) -> None:
    async with client_for(build()) as c:
        r = await c.get("/firmware/manifest", headers=auth())
    assert r.status_code == 204


async def test_manifest_and_image(fw_app, store, tmp_path, caplog) -> None:
    data = make_image("1.0.1")
    m = store.publish(write_bin(tmp_path, data))
    caplog.set_level(logging.INFO)
    async with client_for(fw_app) as c:
        r = await c.get("/firmware/manifest", headers={**auth(), "X-Node-Firmware": "1.0.0"})
        assert r.status_code == 200
        body = r.json()
        assert body == {
            "version": "1.0.1",
            "sha256": hashlib.sha256(data).hexdigest(),
            "size": len(data),
            "published_at": m.published_at,
        }
        assert r.headers["cache-control"] == "no-store"
        img = await c.get(f"/firmware/{body['sha256']}.bin", headers=auth())
    assert img.status_code == 200
    assert img.content == data
    assert hashlib.sha256(img.content).hexdigest() == body["sha256"]
    assert int(img.headers["content-length"]) == body["size"]
    assert img.headers["content-type"] == "application/octet-stream"
    logs = caplog.text
    assert f"firmware check: node={NODE_ID} running=1.0.0 published=1.0.1" in logs
    assert f"firmware image: served node={NODE_ID} version=1.0.1 bytes={len(data)}" in logs
    assert NODE_CREDENTIAL not in logs


async def test_node_firmware_header_is_sanitised_in_logs(fw_app, caplog) -> None:
    caplog.set_level(logging.INFO)
    async with client_for(fw_app) as c:
        await c.get("/firmware/manifest", headers={**auth(), "X-Node-Firmware": "1.0.0\rforged"})
    assert "running=?" in caplog.text and "forged" not in caplog.text


@pytest.mark.parametrize("path", ["/firmware/manifest", "/firmware/IMAGE"])
@pytest.mark.parametrize(
    ("headers", "status"),
    [
        ({}, 401),
        (bearer("wrong-token-" + "x" * 40), 401),
        ({**auth(), "Authorization": "Bearer wrong-token-" + "x" * 40}, 401),
        (bearer(), 400),  # no X-Node-Id
        ({**bearer(), "X-Node-Id": NODE_ID}, 403),  # no credential
        ({**auth(), "X-Node-Credential": "vnc_" + "x" * 43}, 403),  # wrong credential
        ({**auth(), "X-Node-Id": "homelink-000000000000"}, 403),  # unregistered node
    ],
)
async def test_firmware_routes_need_node_auth(fw_app, store, tmp_path, path, headers, status):
    m = store.publish(write_bin(tmp_path, make_image()))
    path = path.replace("IMAGE", f"{m.sha256}.bin")
    async with client_for(fw_app) as c:
        r = await c.get(path, headers=headers)
    assert r.status_code == status
    assert r.json()["error"] in {"unauthorized", "node_unauthorized", "bad_node_id"}
    assert b"\xe9" not in r.content


async def test_revoked_node_gets_no_firmware(fw_app, store, registry, tmp_path) -> None:
    m = store.publish(write_bin(tmp_path, make_image()))
    registry.revoke(NODE_ID)
    async with client_for(fw_app) as c:
        r1 = await c.get("/firmware/manifest", headers=auth())
        r2 = await c.get(f"/firmware/{m.sha256}.bin", headers=auth())
    assert (r1.status_code, r2.status_code) == (403, 403)


async def test_image_names_that_are_not_the_current_hash_are_404(fw_app, store, tmp_path):
    m = store.publish(write_bin(tmp_path, make_image()))
    (store.dir / "secret.bin").write_bytes(b"not for nodes")
    (store.dir / "secret.bin").chmod(0o600)
    names = [
        f"{m.sha256.upper()}.bin",
        f"{m.sha256}",
        f"{m.sha256}.bin.bak",
        f"{'0' * 64}.bin",
        "manifest.json",
        "secret.bin",
        f"../{m.sha256}.bin",
        "..%2fmanifest.json",
        "%2e%2e/%2e%2e/nodes.json",
        f"{m.sha256}.bin/../manifest.json",
        f"sub/{m.sha256}.bin",
    ]
    async with client_for(fw_app) as c:
        for name in names:
            r = await c.get(f"/firmware/{name}", headers=auth())
            assert r.status_code == 404, name
            assert r.json() == {"error": "not_found"}, name


async def test_tampered_or_insecure_store_is_503(fw_app, store, tmp_path) -> None:
    m = store.publish(write_bin(tmp_path, make_image()))
    path = store.dir / f"{m.sha256}.bin"
    path.write_bytes(path.read_bytes()[:-1] + b"\x01")
    async with client_for(fw_app) as c:
        r = await c.get(f"/firmware/{m.sha256}.bin", headers=auth())
        assert (r.status_code, r.json()) == (503, {"error": "firmware_unavailable"})
        store.dir.chmod(0o755)
        r = await c.get("/firmware/manifest", headers=auth())
        assert (r.status_code, r.json()) == (503, {"error": "firmware_unavailable"})
    store.dir.chmod(0o700)


# ---- the CLI ------------------------------------------------------------------------------


@pytest.fixture
def cli_store(tmp_path, monkeypatch) -> FirmwareStore:
    path = tmp_path / "cfg" / "firmware"
    monkeypatch.setenv("VESPER_NODE_FIRMWARE_DIR", str(path))
    monkeypatch.setenv("VESPER_NODE_ENV_FILE", str(tmp_path / "absent.env"))
    return FirmwareStore(path)


def test_cli_publish_status_withdraw(cli_store, tmp_path, capsys) -> None:
    data = make_image("1.0.1")
    assert main(["firmware", "status"]) == 0
    assert "published: none" in capsys.readouterr().out
    assert main(["firmware", "publish", str(write_bin(tmp_path, data))]) == 0
    out = capsys.readouterr().out
    assert f"published: version 1.0.1, {len(data)} bytes, sha256 " in out
    assert hashlib.sha256(data).hexdigest() in out
    assert main(["firmware", "status"]) == 0
    assert "published: version 1.0.1" in capsys.readouterr().out
    assert main(["firmware", "publish", str(write_bin(tmp_path, make_image("1.0.0")))]) == 65
    assert "refused: not_newer" in capsys.readouterr().err
    assert main(["firmware", "withdraw"]) == 0
    assert "withdrawn: 1.0.1" in capsys.readouterr().out
    assert main(["firmware", "publish", str(tmp_path / "missing.bin")]) == 65
    assert main(["firmware", "bogus"]) == 64
    assert main(["firmware"]) == 64


def test_cli_firmware_dir_resolution(tmp_path) -> None:
    from vesper_node.config import DEFAULT_FIRMWARE_DIR, resolve_firmware_dir

    env_file = tmp_path / "node.env"
    env_file.write_text(f"VESPER_NODE_FIRMWARE_DIR={tmp_path / 'fw'}\n")
    env_file.chmod(0o600)
    assert resolve_firmware_dir({"VESPER_NODE_ENV_FILE": str(env_file)}) == tmp_path / "fw"
    assert resolve_firmware_dir({"VESPER_NODE_ENV_FILE": str(tmp_path / "none")}) == (
        DEFAULT_FIRMWARE_DIR.expanduser()
    )
