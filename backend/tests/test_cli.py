"""Task 08 operator CLI: `vesper-node claim` / `vesper-node nodes ...` (local registry edits)."""

from __future__ import annotations

import stat

import pytest

from vesper_node.app import main
from vesper_node.config import DEFAULT_REGISTRY_FILE, resolve_registry_path
from vesper_node.registry import Registry


@pytest.fixture
def reg(tmp_path, monkeypatch) -> Registry:
    path = tmp_path / "cfg" / "nodes.json"
    monkeypatch.setenv("VESPER_NODE_REGISTRY_FILE", str(path))
    monkeypatch.setenv("VESPER_NODE_ENV_FILE", str(tmp_path / "absent.env"))
    return Registry(path)


def test_claim_via_cli(reg, capsys) -> None:
    started = reg.start_claim("homelink-aabbccddeeff")
    assert main(["claim", started.code.lower(), "--room", "kitchen"]) == 0
    out = capsys.readouterr()
    assert "approved: homelink-aabbccddeeff -> room kitchen" in out.out
    assert started.code not in out.out + out.err and started.secret not in out.out + out.err
    cred = reg.poll("homelink-aabbccddeeff", started.secret).credential
    assert reg.authenticate("homelink-aabbccddeeff", cred).room == "kitchen"
    assert stat.S_IMODE(reg.path.stat().st_mode) == 0o600


def test_claim_wrong_code_and_bad_room(reg, capsys) -> None:
    reg.start_claim("homelink-aabbccddeeff")
    assert main(["claim", "2222-2222", "--room", "kitchen"]) == 65
    assert main(["claim", "2222-2222", "--room", "bad/room"]) == 65
    assert "refused:" in capsys.readouterr().err


def test_nodes_lifecycle_and_list(reg, capsys) -> None:
    assert (
        main(["nodes", "add", "homelink-c86320", "--room", "office", "--allow-shared-token"]) == 0
    )
    assert "WARNING: shared-token access is ON" in capsys.readouterr().out
    assert reg.authenticate("homelink-c86320", None).via == "shared_token"
    assert main(["nodes", "set-room", "homelink-c86320", "Living room"]) == 0
    assert main(["nodes", "list"]) == 0
    out = capsys.readouterr().out
    assert "homelink-c86320  room=Living room  access=SHARED-TOKEN (transition)" in out
    assert "sha256" not in out
    assert main(["nodes", "allow-shared-token", "homelink-c86320", "off"]) == 0
    assert not reg.authenticate("homelink-c86320", None).ok
    assert main(["nodes", "revoke", "homelink-c86320"]) == 0
    assert main(["nodes", "remove", "homelink-c86320"]) == 0
    assert main(["nodes", "remove", "homelink-c86320"]) == 65
    assert main(["nodes", "bogus"]) == 64


def test_cli_refuses_group_readable_registry(reg, capsys) -> None:
    reg.add_node("homelink-c86320", "office")
    reg.path.chmod(0o644)
    assert main(["nodes", "list"]) == 78
    assert "chmod 600" in capsys.readouterr().err


def test_registry_path_resolution(tmp_path) -> None:
    env_file = tmp_path / "node.env"
    env_file.write_text(f"VESPER_NODE_REGISTRY_FILE={tmp_path / 'r.json'}\n")
    env_file.chmod(0o600)
    assert resolve_registry_path({"VESPER_NODE_ENV_FILE": str(env_file)}) == tmp_path / "r.json"
    assert (
        resolve_registry_path(
            {"VESPER_NODE_ENV_FILE": str(env_file), "VESPER_NODE_REGISTRY_FILE": "/x"}
        ).as_posix()
        == "/x"
    )
    assert resolve_registry_path({"VESPER_NODE_ENV_FILE": str(tmp_path / "none")}) == (
        DEFAULT_REGISTRY_FILE.expanduser()
    )
