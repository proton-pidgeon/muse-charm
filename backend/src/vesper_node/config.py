"""Settings for the node backend, read from env-style files.

Mirrors ``vesper-voice/server/src/vesper_voice/config.py`` (same parser, same 600-mode rule,
same cleartext-URL rule). It does not import it, because that package pulls in
livekit-agents.

Resolution order for every key: process environment, then the **node** env file
(``~/.config/vesper-voice/node.env``, override ``VESPER_NODE_ENV_FILE``), then the shared
**voice** env file (``~/.config/vesper-voice/env``, override ``VESPER_VOICE_ENV_FILE``).

* ``VESPER_NODE_TOKEN`` (>= 32 chars) normally lives in ``node.env``.
* ``VESPER_NODE_ADMIN_TOKEN`` (optional, >= 32 chars, distinct from the other tokens) enables
  the ``POST /admin/claim`` route (task 08). Without it, claims are approved by the local CLI only.
* ``VESPER_NODE_REGISTRY_FILE`` (optional) moves the node registry from its default
  ``~/.config/vesper-voice/nodes.json``.
* ``VESPER_NODE_FIRMWARE_DIR`` (optional) moves the published node firmware (task 13) from its
  default ``~/.config/vesper-voice/firmware``.
* Provider and brain keys (``ELEVENLABS_API_KEY``, ``DEEPGRAM_API_KEY``,
  ``VESPER_STT_PROVIDER``, ``VESPER_PHONE_TTS_VOICE_ID``, ``VESPER_BRAIN_URL``,
  ``VESPER_BRAIN_TOKEN``) come from the existing voice env file.

Both files must be mode 600 or stricter. A group- or other-readable file is refused, and
nothing is read from it. Every secret value is registered with :mod:`vesper_node.logsafe` so
it is masked if it ever reaches a log line. Errors name variables, never values.
"""

from __future__ import annotations

import ipaddress
import os
import re
import stat
from collections.abc import Mapping
from dataclasses import dataclass, field
from pathlib import Path
from urllib.parse import urlsplit

from . import logsafe

NODE_ENV_FILE_VAR = "VESPER_NODE_ENV_FILE"
VOICE_ENV_FILE_VAR = "VESPER_VOICE_ENV_FILE"
DEFAULT_NODE_ENV_FILE = Path("~/.config/vesper-voice/node.env")
DEFAULT_VOICE_ENV_FILE = Path("~/.config/vesper-voice/env")
REGISTRY_FILE_VAR = "VESPER_NODE_REGISTRY_FILE"
DEFAULT_REGISTRY_FILE = Path("~/.config/vesper-voice/nodes.json")
FIRMWARE_DIR_VAR = "VESPER_NODE_FIRMWARE_DIR"
DEFAULT_FIRMWARE_DIR = Path("~/.config/vesper-voice/firmware")

DEFAULT_HOST = "::"  # same precedent as the live brain: 6PN + loopback
DEFAULT_PORT = 8796
DEFAULT_BRAIN_HOST = "127.0.0.1"
DEFAULT_BRAIN_PORT = 8790
WILDCARD_BIND_HOSTS = frozenset({"0.0.0.0", "::", "[::]"})  # noqa: S104 - matched, not bound
MIN_NODE_TOKEN_LEN = 32
STT_PROVIDERS = ("elevenlabs", "deepgram")
DEFAULT_STT_PROVIDER = "elevenlabs"
DEFAULT_STT_LANGUAGE = "en-US"
TTS_VOICE_ID_RE = re.compile(r"^[A-Za-z0-9]{8,64}$")
TTS_OFF_VALUES = frozenset({"off", "0", "false", "no", "disabled"})


class ConfigError(RuntimeError):
    """A configuration problem. The message names variables or files, never values."""


def check_env_file_mode(path: Path) -> None:
    mode = stat.S_IMODE(path.stat().st_mode)
    if mode & 0o077:
        raise ConfigError(
            f"refusing to read secrets from {path}: mode is {mode:03o}, must be 600 "
            f"(run: chmod 600 {path})"
        )


def parse_env_file(text: str) -> dict[str, str]:
    """``KEY=VALUE`` lines; ``export`` prefix, quotes and ``#`` comments allowed."""
    values: dict[str, str] = {}
    for raw in text.splitlines():
        line = raw.strip()
        if not line or line.startswith("#"):
            continue
        if line.startswith("export "):
            line = line[len("export ") :].lstrip()
        if "=" not in line:
            continue
        key, _, value = line.partition("=")
        key, value = key.strip(), value.strip()
        if len(value) >= 2 and value[0] == value[-1] and value[0] in {"'", '"'}:
            value = value[1:-1]
        elif " #" in value:
            value = value.split(" #", 1)[0].rstrip()
        if key:
            values[key] = value
    return values


def load_env_file(path: Path) -> dict[str, str]:
    """Read ``path`` after the mode check; a missing file is ``{}``."""
    if not path.exists():
        return {}
    check_env_file_mode(path)
    return parse_env_file(path.read_text(encoding="utf-8"))


def _is_private_transport_host(host: str) -> bool:
    """Loopback, or a Tailscale address/name (WireGuard already encrypts the hop)."""
    if host == "localhost" or host.endswith(".ts.net"):
        return True
    try:
        addr = ipaddress.ip_address(host)
    except ValueError:
        return False
    return addr.is_loopback or addr in ipaddress.ip_network("100.64.0.0/10")


def validate_brain_url(url: str) -> str:
    """Plain http(s) origin(+path); cleartext only to loopback/tailnet (``brain_llm.py`` rule)."""
    parts = urlsplit(url.strip())
    if parts.scheme not in {"http", "https"} or not parts.hostname:
        raise ConfigError("VESPER_BRAIN_URL must be an http(s) URL with a host")
    if parts.username or parts.password or parts.query or parts.fragment:
        raise ConfigError("VESPER_BRAIN_URL must not contain credentials, a query or a fragment")
    if parts.scheme == "http" and not _is_private_transport_host(parts.hostname):
        raise ConfigError("VESPER_BRAIN_URL must use https unless it is loopback or tailnet")
    return url.strip().rstrip("/")


def _derived_brain_url(host: str | None, port: str | None) -> str:
    host = host or DEFAULT_BRAIN_HOST
    if host in WILDCARD_BIND_HOSTS:
        host = "::1" if ":" in host else "127.0.0.1"
    host = host.strip("[]")
    bracketed = f"[{host}]" if ":" in host else host
    return f"http://{bracketed}:{port or DEFAULT_BRAIN_PORT}"


def _port(value: str | None) -> int:
    if value is None:
        return DEFAULT_PORT
    try:
        port = int(value)
    except ValueError:
        raise ConfigError("VESPER_NODE_PORT must be an integer") from None
    if not 1 <= port <= 65535:
        raise ConfigError("VESPER_NODE_PORT must be between 1 and 65535")
    return port


@dataclass(frozen=True)
class Settings:
    node_token: str
    brain_url: str
    brain_token: str
    host: str = DEFAULT_HOST
    port: int = DEFAULT_PORT
    stt_provider: str = DEFAULT_STT_PROVIDER
    stt_language: str = DEFAULT_STT_LANGUAGE
    elevenlabs_api_key: str | None = None
    deepgram_api_key: str | None = None
    tts_voice_id: str | None = None
    tts_off: bool = False
    # Node registry (task 08). None only in hand-built test settings: create_app then needs
    # an explicit registry, so a test can never touch the real ~/.config file.
    registry_file: str | None = None
    # Published node firmware (task 13). None in hand-built test settings: no firmware routes
    # then serve anything (manifest 204) unless create_app is given a store.
    firmware_dir: str | None = None
    admin_token: str | None = None
    env_files: tuple[str, ...] = field(default=())

    @property
    def ask_url(self) -> str:
        return f"{self.brain_url}/ask"

    @property
    def tts_enabled(self) -> bool:
        return bool(not self.tts_off and self.elevenlabs_api_key and self.tts_voice_id)

    def stt_key(self, provider: str) -> str | None:
        return self.elevenlabs_api_key if provider == "elevenlabs" else self.deepgram_api_key

    def stt_chain(self) -> list[str]:
        """Primary provider, then the other one, keeping only providers with a key."""
        order = [self.stt_provider] + [p for p in STT_PROVIDERS if p != self.stt_provider]
        return [p for p in order if self.stt_key(p)]


def _clean(value: str | None) -> str | None:
    if value is None:
        return None
    value = value.strip()
    return value or None


def load_settings(
    environ: Mapping[str, str] | None = None,
    *,
    node_env_file: Path | None = None,
    voice_env_file: Path | None = None,
) -> Settings:
    """Load and validate settings. Raises :class:`ConfigError` (names only) on any problem."""
    env = dict(os.environ if environ is None else environ)
    node_path = (
        node_env_file or Path(env.get(NODE_ENV_FILE_VAR) or DEFAULT_NODE_ENV_FILE).expanduser()
    )
    voice_path = (
        voice_env_file or Path(env.get(VOICE_ENV_FILE_VAR) or DEFAULT_VOICE_ENV_FILE).expanduser()
    )
    node_file = load_env_file(node_path)
    voice_file = load_env_file(voice_path)

    def get(name: str) -> str | None:
        for source in (env, node_file, voice_file):
            value = _clean(source.get(name))
            if value is not None:
                return value
        return None

    problems: list[str] = []
    node_token = get("VESPER_NODE_TOKEN")
    if not node_token:
        problems.append("VESPER_NODE_TOKEN (missing)")
    elif len(node_token) < MIN_NODE_TOKEN_LEN:
        problems.append(f"VESPER_NODE_TOKEN (must be >= {MIN_NODE_TOKEN_LEN} chars)")
    brain_token = get("VESPER_BRAIN_TOKEN")
    if not brain_token:
        problems.append("VESPER_BRAIN_TOKEN (missing)")
    elif node_token and brain_token == node_token:
        problems.append("VESPER_NODE_TOKEN (must differ from VESPER_BRAIN_TOKEN)")
    stt_provider = (get("VESPER_STT_PROVIDER") or DEFAULT_STT_PROVIDER).lower()
    if stt_provider not in STT_PROVIDERS:
        problems.append(f"VESPER_STT_PROVIDER (must be one of {'|'.join(STT_PROVIDERS)})")
    voice_id = get("VESPER_PHONE_TTS_VOICE_ID")
    if voice_id is not None and not TTS_VOICE_ID_RE.fullmatch(voice_id):
        problems.append("VESPER_PHONE_TTS_VOICE_ID (must be 8-64 letters/digits)")
    admin_token = get("VESPER_NODE_ADMIN_TOKEN")
    if admin_token is not None:
        if len(admin_token) < MIN_NODE_TOKEN_LEN:
            problems.append(f"VESPER_NODE_ADMIN_TOKEN (must be >= {MIN_NODE_TOKEN_LEN} chars)")
        elif admin_token in (node_token, brain_token):
            problems.append("VESPER_NODE_ADMIN_TOKEN (must differ from the other tokens)")
    if problems:
        raise ConfigError("invalid configuration: " + ", ".join(problems))

    raw_url = get("VESPER_BRAIN_URL")
    brain_url = validate_brain_url(
        raw_url or _derived_brain_url(get("VESPER_BRAIN_HOST"), get("VESPER_BRAIN_PORT"))
    )
    settings = Settings(
        node_token=node_token or "",
        brain_url=brain_url,
        brain_token=brain_token or "",
        host=get("VESPER_NODE_HOST") or DEFAULT_HOST,
        port=_port(get("VESPER_NODE_PORT")),
        stt_provider=stt_provider,
        stt_language=get("VESPER_STT_LANGUAGE") or DEFAULT_STT_LANGUAGE,
        elevenlabs_api_key=get("ELEVENLABS_API_KEY") or get("ELEVEN_API_KEY"),
        deepgram_api_key=get("DEEPGRAM_API_KEY"),
        tts_voice_id=voice_id,
        tts_off=(get("VESPER_NODE_TTS") or "").lower() in TTS_OFF_VALUES,
        registry_file=str(Path(get(REGISTRY_FILE_VAR) or DEFAULT_REGISTRY_FILE).expanduser()),
        firmware_dir=str(Path(get(FIRMWARE_DIR_VAR) or DEFAULT_FIRMWARE_DIR).expanduser()),
        admin_token=admin_token,
        env_files=(str(node_path), str(voice_path)),
    )
    for secret in (
        settings.node_token,
        settings.brain_token,
        settings.elevenlabs_api_key,
        settings.deepgram_api_key,
        settings.admin_token,
    ):
        logsafe.register_secret(secret)
    if not settings.stt_chain():
        raise ConfigError(
            "invalid configuration: no STT key (ELEVENLABS_API_KEY or DEEPGRAM_API_KEY)"
        )
    return settings


def resolve_registry_path(environ: Mapping[str, str] | None = None) -> Path:
    """Registry path for the CLI: process env, then ``node.env``. Needs no tokens."""
    env = dict(os.environ if environ is None else environ)
    node_path = Path(env.get(NODE_ENV_FILE_VAR) or DEFAULT_NODE_ENV_FILE).expanduser()
    value = _clean(env.get(REGISTRY_FILE_VAR)) or _clean(
        load_env_file(node_path).get(REGISTRY_FILE_VAR)
    )
    return Path(value or DEFAULT_REGISTRY_FILE).expanduser()


def resolve_firmware_dir(environ: Mapping[str, str] | None = None) -> Path:
    """Firmware store for the CLI: process env, then ``node.env``. Needs no tokens."""
    env = dict(os.environ if environ is None else environ)
    node_path = Path(env.get(NODE_ENV_FILE_VAR) or DEFAULT_NODE_ENV_FILE).expanduser()
    value = _clean(env.get(FIRMWARE_DIR_VAR)) or _clean(
        load_env_file(node_path).get(FIRMWARE_DIR_VAR)
    )
    return Path(value or DEFAULT_FIRMWARE_DIR).expanduser()
