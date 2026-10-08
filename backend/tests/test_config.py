from __future__ import annotations

import logging
from pathlib import Path

import pytest

from vesper_node import logsafe
from vesper_node.config import ConfigError, load_settings

TOKEN = "t" * 48
BRAIN = "b" * 48


def write(path: Path, text: str, mode: int = 0o600) -> Path:
    path.write_text(text)
    path.chmod(mode)
    return path


@pytest.fixture
def files(tmp_path: Path):
    node = write(tmp_path / "node.env", f"VESPER_NODE_TOKEN={TOKEN}\n")
    voice = write(
        tmp_path / "env",
        "\n".join(
            [
                "export ELEVENLABS_API_KEY=sk_" + "e" * 48,
                f"VESPER_BRAIN_TOKEN='{BRAIN}'",
                "VESPER_BRAIN_URL=http://[::1]:8795",
                "VESPER_STT_PROVIDER=elevenlabs",
                "VESPER_PHONE_TTS_VOICE_ID=Voice12345678  # comment",
            ]
        ),
    )
    return node, voice


def load(files, env: dict[str, str] | None = None):
    node, voice = files
    return load_settings(env or {}, node_env_file=node, voice_env_file=voice)


def test_loads_both_files(files) -> None:
    s = load(files)
    assert s.node_token == TOKEN and s.brain_token == BRAIN
    assert s.ask_url == "http://[::1]:8795/ask"
    assert s.host == "::" and s.port == 8796
    assert s.stt_chain() == ["elevenlabs"]  # no Deepgram key -> no fallback
    assert s.tts_voice_id == "Voice12345678" and s.tts_enabled


def test_env_paths_overridable_by_env_var(files) -> None:
    node, voice = files
    s = load_settings({"VESPER_NODE_ENV_FILE": str(node), "VESPER_VOICE_ENV_FILE": str(voice)})
    assert s.node_token == TOKEN


def test_process_env_wins(files) -> None:
    s = load(files, {"VESPER_NODE_PORT": "9999", "VESPER_NODE_TTS": "off"})
    assert s.port == 9999 and not s.tts_enabled


def test_deepgram_fallback_chain(files) -> None:
    s = load(files, {"DEEPGRAM_API_KEY": "d" * 40})
    assert s.stt_chain() == ["elevenlabs", "deepgram"]
    s2 = load(files, {"DEEPGRAM_API_KEY": "d" * 40, "VESPER_STT_PROVIDER": "deepgram"})
    assert s2.stt_chain() == ["deepgram", "elevenlabs"]


@pytest.mark.parametrize("which", [0, 1])
def test_refuses_group_readable_files(files, which) -> None:
    files[which].chmod(0o640)
    with pytest.raises(ConfigError, match="must be 600"):
        load(files)


def test_refuses_missing_or_short_token(files, tmp_path) -> None:
    node, voice = files
    with pytest.raises(ConfigError, match="VESPER_NODE_TOKEN"):
        load_settings({}, node_env_file=tmp_path / "missing", voice_env_file=voice)
    write(node, "VESPER_NODE_TOKEN=short\n")
    with pytest.raises(ConfigError, match=">= 32"):
        load(files)


def test_refuses_node_token_equal_to_brain_token(files) -> None:
    with pytest.raises(ConfigError, match="must differ"):
        load(files, {"VESPER_NODE_TOKEN": BRAIN})


def test_error_never_contains_values(files) -> None:
    write(files[0], "VESPER_NODE_TOKEN=shortsecretvalue\n")
    with pytest.raises(ConfigError) as e:
        load(files)
    assert "shortsecretvalue" not in str(e.value)


@pytest.mark.parametrize(
    "url",
    ["http://brain.example.com", "ftp://x", "https://user:pw@x.example", "https://x.example/?q=1"],
)
def test_refuses_unsafe_brain_url(files, url) -> None:
    with pytest.raises(ConfigError, match="VESPER_BRAIN_URL"):
        load(files, {"VESPER_BRAIN_URL": url})


def test_allows_https_and_tailnet(files) -> None:
    assert load(files, {"VESPER_BRAIN_URL": "https://peggy.fly.dev/vesper/"}).ask_url == (
        "https://peggy.fly.dev/vesper/ask"
    )
    assert load(files, {"VESPER_BRAIN_URL": "http://100.101.102.103:8790"}).brain_url


def test_derived_brain_url_from_wildcard_host(files) -> None:
    voice = files[1]
    write(voice, voice.read_text().replace("VESPER_BRAIN_URL=http://[::1]:8795", ""))
    s = load(files, {"VESPER_BRAIN_HOST": "::", "VESPER_BRAIN_PORT": "8795"})
    assert s.ask_url == "http://[::1]:8795/ask"


def test_bad_voice_id_and_provider(files) -> None:
    with pytest.raises(ConfigError, match="VESPER_PHONE_TTS_VOICE_ID"):
        load(files, {"VESPER_PHONE_TTS_VOICE_ID": "bad/id"})
    with pytest.raises(ConfigError, match="VESPER_STT_PROVIDER"):
        load(files, {"VESPER_STT_PROVIDER": "whisper"})


def test_loaded_secrets_are_redacted_in_logs(files, caplog) -> None:
    load(files)
    logsafe.install_log_redaction()
    caplog.set_level(logging.INFO)
    logging.getLogger("t").info("leak %s / %s / %r", TOKEN, BRAIN, b"\x00" * 64)
    assert TOKEN not in caplog.text and BRAIN not in caplog.text
    assert "<redacted:bytes len=64>" in caplog.text
    assert logsafe.redact_text("Authorization: Bearer abcdefghijkl") == (
        "Authorization: [REDACTED]"
    )


def test_registry_and_admin_token_settings(files, tmp_path) -> None:
    s = load(files)
    assert s.registry_file == str(Path("~/.config/vesper-voice/nodes.json").expanduser())
    assert s.admin_token is None
    s = load(
        files,
        {
            "VESPER_NODE_REGISTRY_FILE": str(tmp_path / "r.json"),
            "VESPER_NODE_ADMIN_TOKEN": "a" * 40,
        },
    )
    assert s.registry_file == str(tmp_path / "r.json") and s.admin_token == "a" * 40


@pytest.mark.parametrize("admin", ["short-admin", TOKEN, BRAIN])
def test_refuses_weak_or_reused_admin_token(files, admin) -> None:
    with pytest.raises(ConfigError) as e:
        load(files, {"VESPER_NODE_ADMIN_TOKEN": admin})
    assert "VESPER_NODE_ADMIN_TOKEN" in str(e.value) and admin not in str(e.value)


def test_check_config_names_the_registry(files, tmp_path, monkeypatch, capsys) -> None:
    from vesper_node.app import main

    node, voice = files
    monkeypatch.setenv("VESPER_NODE_ENV_FILE", str(node))
    monkeypatch.setenv("VESPER_VOICE_ENV_FILE", str(voice))
    monkeypatch.setenv("VESPER_NODE_REGISTRY_FILE", str(tmp_path / "nodes.json"))
    assert main(["check-config"]) == 0
    out = capsys.readouterr().out
    assert f"node registry: {tmp_path / 'nodes.json'} (VESPER_NODE_REGISTRY_FILE)" in out
    assert "registered nodes: 0" in out and "admin claim route: off" in out
    (tmp_path / "nodes.json").write_text("{}")
    (tmp_path / "nodes.json").chmod(0o644)
    assert main(["check-config"]) == 78


def test_memory_settings(files, tmp_path) -> None:
    """Task 16: conversation memory file + session idle minutes (defaults and overrides)."""
    s = load(files)
    assert s.memory_file == str(Path("~/.config/vesper-voice/node-memory.json").expanduser())
    assert s.session_idle_minutes == 15
    s = load(
        files,
        {
            "VESPER_NODE_MEMORY_FILE": str(tmp_path / "m.json"),
            "VESPER_NODE_SESSION_IDLE_MINUTES": "30",
        },
    )
    assert s.memory_file == str(tmp_path / "m.json") and s.session_idle_minutes == 30


@pytest.mark.parametrize("value", ["soon", "0", "-5", "1441"])
def test_refuses_bad_session_idle(files, value) -> None:
    with pytest.raises(ConfigError) as e:
        load(files, {"VESPER_NODE_SESSION_IDLE_MINUTES": value})
    assert "VESPER_NODE_SESSION_IDLE_MINUTES" in str(e.value)
