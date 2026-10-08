"""Node registry + claim flow (task 08 / B2): ``node_id`` -> room, per-node credentials.

Credential model (conflict C2, recorded in ``docs/node-wire-protocol.md``): every node request
carries the **shared edge bearer** ``Authorization: Bearer <VESPER_NODE_TOKEN>`` (the one token
Peggy's ``/vesper-node/*`` matcher checks) **plus** ``X-Node-Id`` and a **per-node**
``X-Node-Credential`` issued by the claim flow. The backend checks both.

The registry is one JSON file (default ``~/.config/vesper-voice/nodes.json``, override
``VESPER_NODE_REGISTRY_FILE``), shared by the running service and the ``vesper-node`` CLI:

* created mode 600 in a 700 directory; a group/other-readable file is refused (fail closed);
* every write is load-modify-write under an exclusive ``flock`` on ``<file>.lock`` (so the CLI
  and the service never lose each other's updates), then temp file + ``fsync`` + ``os.replace``
  + directory ``fsync`` (crash-safe; a reader sees the old or the new file, never a torn one);
* readers (the per-request auth check) use a snapshot reloaded whenever the file's
  ``(inode, mtime, size)`` changes, so a CLI edit takes effect on the next request without a
  restart.

Secrets at rest: only SHA-256 hashes of credentials, claim codes and claim secrets are stored.
Credentials and claim secrets are 256-bit random values, so an unsalted hash is enough. A
credential's plaintext exists only in the one ``/claim/poll`` response that delivers it.
Comparisons use :func:`hmac.compare_digest`. Nothing here logs a code, secret or credential.
"""

from __future__ import annotations

import contextlib
import fcntl
import hashlib
import hmac
import json
import logging
import os
import re
import secrets
import stat
import tempfile
import threading
import time
from collections.abc import Callable, Iterator
from dataclasses import dataclass
from pathlib import Path
from typing import Any

log = logging.getLogger("vesper_node.registry")

SCHEMA_VERSION = 1
NODE_ID_RE = re.compile(r"^[A-Za-z0-9._:-]{1,128}$")
# Free-form room names (open question Q5: rooms for nodes 2/3 unknown), but bounded and inert:
# letters, digits, space, '-', '_' only; they are pasted into the brain prompt.
ROOM_RE = re.compile(r"^[A-Za-z0-9](?:[A-Za-z0-9 _-]{0,38}[A-Za-z0-9])?$")

CREDENTIAL_PREFIX = "vnc_"  # vesper node credential
CLAIM_SECRET_PREFIX = "vcs_"  # noqa: S105 - a prefix, not a secret (node-side poll secret)
# Readable on a 128x128 screen: no 0/O, 1/I/L, no U (avoids accidental words).
CLAIM_ALPHABET = "23456789ABCDEFGHJKMNPQRSTVWXYZ"
CLAIM_CODE_LEN = 8  # 30^8 ~ 6.6e11 (~39 bits), shown as XXXX-XXXX

CLAIM_TTL_S = 600  # a code is valid for 10 minutes
CLAIM_RESTART_INTERVAL_S = 5  # min seconds between /claim/start for one node
MAX_PENDING = 8  # pending claims across all nodes
MAX_BAD_POLLS = 5  # wrong claim secrets before a pending claim is dropped
APPROVE_FAIL_WINDOW_S = 900
APPROVE_MAX_FAILS = 10  # wrong codes per window before approvals lock (and pending is wiped)
POLL_INTERVAL_S = 3


class RegistryError(RuntimeError):
    """Unusable registry file (bad mode, bad JSON, bad schema). Message names the file only."""


class ClaimError(Exception):
    """A claim operation was refused. ``code`` is a stable wire error code."""

    def __init__(self, code: str, *, retry_after: int | None = None) -> None:
        super().__init__(code)
        self.code = code
        self.retry_after = retry_after


def sha256_hex(value: str) -> str:
    return hashlib.sha256(value.encode()).hexdigest()


def normalize_code(code: str) -> str:
    return re.sub(r"[\s-]", "", code).upper()


def format_code(raw: str) -> str:
    return f"{raw[:4]}-{raw[4:]}"


def valid_room(room: str) -> bool:
    return bool(ROOM_RE.fullmatch(room))


@dataclass(frozen=True)
class NodeAuth:
    """Outcome of a node authentication check."""

    ok: bool
    node_id: str
    room: str | None = None
    via: str = "-"  # "credential" | "shared_token" (legacy transition) | "-"
    reason: str = "-"  # log-only: unknown | unclaimed | bad_credential | registry_error


@dataclass(frozen=True)
class ClaimStart:
    code: str  # XXXX-XXXX, shown on the node's screen
    secret: str  # node keeps it in RAM and polls with it
    expires_in: int


@dataclass(frozen=True)
class Delivery:
    """``/claim/poll`` result: pending (credential None) or claimed (credential, once)."""

    status: str  # "pending" | "claimed"
    room: str | None = None
    credential: str | None = None
    expires_in: int | None = None


def _empty() -> dict[str, Any]:
    return {"version": SCHEMA_VERSION, "nodes": {}, "pending": {}, "approve_failures": []}


def _validate(data: Any) -> dict[str, Any]:
    if not isinstance(data, dict) or data.get("version") != SCHEMA_VERSION:
        raise ValueError("schema")
    nodes, pending = data.get("nodes"), data.get("pending")
    if not isinstance(nodes, dict) or not isinstance(pending, dict):
        raise ValueError("schema")
    for node_id, rec in nodes.items():
        if not NODE_ID_RE.fullmatch(node_id) or not isinstance(rec, dict):
            raise ValueError("node")
        if not isinstance(rec.get("room"), str) or not valid_room(rec["room"]):
            raise ValueError("room")
        cred = rec.get("credential_sha256")
        if cred is not None and not (isinstance(cred, str) and re.fullmatch(r"[0-9a-f]{64}", cred)):
            raise ValueError("credential")
        if not isinstance(rec.get("allow_shared_token", False), bool):
            raise ValueError("allow_shared_token")
    for node_id, rec in pending.items():
        if not NODE_ID_RE.fullmatch(node_id) or not isinstance(rec, dict):
            raise ValueError("pending")
    if not isinstance(data.setdefault("approve_failures", []), list):
        raise ValueError("approve_failures")
    return data


class Registry:
    """The node registry file. Safe to share between threads, processes and the CLI."""

    def __init__(self, path: Path | str, *, clock: Callable[[], float] = time.time) -> None:
        self.path = Path(path).expanduser()
        self.lock_path = self.path.with_name(self.path.name + ".lock")
        self._clock = clock
        self._mutex = threading.Lock()
        self._snap_key: tuple[int, int, int] | None = None
        self._snap: dict[str, Any] = _empty()
        # A dummy hash so unknown nodes cost the same compare as known ones.
        self._dummy = sha256_hex(secrets.token_urlsafe(32))

    # ---- file I/O -------------------------------------------------------------------------

    def _ensure_dir(self) -> None:
        parent = self.path.parent
        if not parent.exists():
            parent.mkdir(mode=0o700, parents=True, exist_ok=True)

    def _read_file(self) -> dict[str, Any]:
        """Read + validate. Missing file is an empty registry. Raises :class:`RegistryError`."""
        try:
            fd = os.open(self.path, os.O_RDONLY | getattr(os, "O_NOFOLLOW", 0))
        except FileNotFoundError:
            return _empty()
        except OSError as e:
            raise RegistryError(f"cannot open node registry {self.path}") from e
        with os.fdopen(fd, "rb") as fh:
            st = os.fstat(fh.fileno())
            if not stat.S_ISREG(st.st_mode) or stat.S_IMODE(st.st_mode) & 0o077:
                raise RegistryError(
                    f"refusing node registry {self.path}: must be a regular file with mode 600 "
                    f"(run: chmod 600 {self.path})"
                )
            raw = fh.read()
        try:
            return _validate(json.loads(raw.decode("utf-8")))
        except (ValueError, UnicodeDecodeError) as e:
            raise RegistryError(f"node registry {self.path} is malformed") from e

    def _write_file(self, data: dict[str, Any]) -> None:
        self._ensure_dir()
        payload = json.dumps(data, indent=2, sort_keys=True).encode() + b"\n"
        fd, tmp = tempfile.mkstemp(dir=self.path.parent, prefix=f".{self.path.name}.")
        try:
            os.fchmod(fd, 0o600)
            with os.fdopen(fd, "wb") as fh:
                fh.write(payload)
                fh.flush()
                os.fsync(fh.fileno())
            os.replace(tmp, self.path)
        except BaseException:
            with contextlib.suppress(FileNotFoundError):
                os.unlink(tmp)
            raise
        dfd = os.open(self.path.parent, os.O_RDONLY)
        try:
            os.fsync(dfd)
        finally:
            os.close(dfd)

    @contextlib.contextmanager
    def _locked(self) -> Iterator[dict[str, Any]]:
        """Exclusive load-modify-write. Yields the current data and writes back any change.

        Changes are written even when the body raises a :class:`ClaimError`: a refusal can
        still record state (a failed-attempt counter, a dropped claim).
        """
        with self._mutex:
            self._ensure_dir()
            lfd = os.open(self.lock_path, os.O_RDWR | os.O_CREAT, 0o600)
            try:
                fcntl.flock(lfd, fcntl.LOCK_EX)
                data = self._read_file()
                before = json.dumps(data, sort_keys=True)
                try:
                    yield data
                except ClaimError:
                    if json.dumps(data, sort_keys=True) != before:
                        self._write_file(data)
                    raise
                if json.dumps(data, sort_keys=True) != before:
                    self._write_file(data)
            finally:
                fcntl.flock(lfd, fcntl.LOCK_UN)
                os.close(lfd)
        self._snap_key = None  # force a reload on the next read

    def snapshot(self) -> dict[str, Any]:
        """The current registry, reloaded if the file changed. Raises :class:`RegistryError`."""
        try:
            st = os.stat(self.path)
            key = (st.st_ino, st.st_mtime_ns, st.st_size)
        except FileNotFoundError:
            key = (0, 0, 0)
        with self._mutex:
            if key != self._snap_key:
                self._snap = self._read_file()
                self._snap_key = key
            return self._snap

    def check(self) -> int:
        """Validate the file now (startup / check-config). Returns the node count."""
        self._snap_key = None
        return len(self.snapshot()["nodes"])

    # ---- node authentication (per request, read-only) -------------------------------------

    def authenticate(self, node_id: str, credential: str | None) -> NodeAuth:
        try:
            nodes = self.snapshot()["nodes"]
        except RegistryError as e:
            log.error("node registry unusable: %s", e)
            return NodeAuth(False, node_id, reason="registry_error")
        rec = nodes.get(node_id)
        stored = rec.get("credential_sha256") if rec else None
        presented = sha256_hex(credential) if credential else sha256_hex("")
        match = hmac.compare_digest(presented, stored or self._dummy)
        if rec is None:
            return NodeAuth(False, node_id, reason="unknown")
        if credential:
            if stored and match:
                return NodeAuth(True, node_id, rec["room"], via="credential")
            return NodeAuth(False, node_id, reason="bad_credential")
        if rec.get("allow_shared_token"):
            return NodeAuth(True, node_id, rec["room"], via="shared_token")
        return NodeAuth(False, node_id, reason="unclaimed")

    # ---- claim flow -----------------------------------------------------------------------

    def _prune(self, data: dict[str, Any], now: float) -> None:
        pending = data["pending"]
        for nid in [n for n, p in pending.items() if p.get("expires", 0) <= now]:
            del pending[nid]
        data["approve_failures"] = [
            t for t in data["approve_failures"] if now - t < APPROVE_FAIL_WINDOW_S
        ]

    def start_claim(self, node_id: str) -> ClaimStart:
        """Node side: open (or restart) a pending claim. Needs only the shared node token."""
        now = self._clock()
        code = "".join(secrets.choice(CLAIM_ALPHABET) for _ in range(CLAIM_CODE_LEN))
        secret = CLAIM_SECRET_PREFIX + secrets.token_urlsafe(32)
        with self._locked() as data:
            self._prune(data, now)
            pending = data["pending"]
            prev = pending.get(node_id)
            if prev is not None and now - prev.get("created", 0) < CLAIM_RESTART_INTERVAL_S:
                raise ClaimError("rate_limited", retry_after=CLAIM_RESTART_INTERVAL_S)
            if prev is None and len(pending) >= MAX_PENDING:
                raise ClaimError("too_many_pending", retry_after=60)
            # A restart replaces the old code: only the newest code on the screen is valid.
            pending[node_id] = {
                "code_sha256": sha256_hex(code),
                "secret_sha256": sha256_hex(secret),
                "created": now,
                "expires": now + CLAIM_TTL_S,
                "approved_room": None,
                "bad_polls": 0,
            }
        return ClaimStart(format_code(code), secret, CLAIM_TTL_S)

    def approve(self, code: str, room: str) -> tuple[str, bool]:
        """Admin side (CLI or admin route): bind a shown code to a room.

        Returns ``(node_id, replaces_credential)``. The credential itself is minted later, by
        the node's own poll, so its plaintext never touches disk or the approver.
        """
        if not valid_room(room):
            raise ClaimError("bad_room")
        now = self._clock()
        presented = sha256_hex(normalize_code(code))
        with self._locked() as data:
            self._prune(data, now)
            if len(data["approve_failures"]) >= APPROVE_MAX_FAILS:
                data["pending"].clear()  # a locked-out guesser gets nothing even later
                raise ClaimError("claim_locked", retry_after=APPROVE_FAIL_WINDOW_S)
            found = None
            for nid, p in data["pending"].items():
                if hmac.compare_digest(presented, str(p.get("code_sha256", ""))):
                    found = nid
            if found is None:
                data["approve_failures"].append(now)
                if len(data["approve_failures"]) >= APPROVE_MAX_FAILS:
                    data["pending"].clear()
                raise ClaimError("claim_not_found")
            data["pending"][found]["approved_room"] = room
            data["pending"][found]["approved_at"] = now
            rec = data["nodes"].get(found)
            replaces = bool(rec and (rec.get("credential_sha256") or rec.get("allow_shared_token")))
        log.info("claim approved: node=%s room=%s", found, room)
        return found, replaces

    def poll(self, node_id: str, secret: str) -> Delivery:
        """Node side: poll a pending claim. On approval, mint + deliver the credential once."""
        now = self._clock()
        presented = sha256_hex(secret or "")
        credential = None
        with self._locked() as data:
            self._prune(data, now)
            p = data["pending"].get(node_id)
            if p is None:
                raise ClaimError("claim_not_found")
            if not hmac.compare_digest(presented, str(p.get("secret_sha256", ""))):
                p["bad_polls"] = int(p.get("bad_polls", 0)) + 1
                if p["bad_polls"] >= MAX_BAD_POLLS:
                    del data["pending"][node_id]
                raise ClaimError("claim_not_found")
            room = p.get("approved_room")
            if room is None:
                return Delivery("pending", expires_in=max(0, int(p["expires"] - now)))
            credential = CREDENTIAL_PREFIX + secrets.token_urlsafe(32)
            data["nodes"][node_id] = {
                "room": room,
                "credential_sha256": sha256_hex(credential),
                "allow_shared_token": False,  # claimed: the legacy transition ends here
                "claimed_at": int(now),
            }
            del data["pending"][node_id]
        log.info("credential issued: node=%s room=%s", node_id, room)
        return Delivery("claimed", room=room, credential=credential)

    # ---- operator edits (CLI) -------------------------------------------------------------

    def add_node(
        self,
        node_id: str,
        room: str,
        *,
        allow_shared_token: bool = False,
        credential: str | None = None,
    ) -> None:
        """Pre-register a node. ``credential`` (plaintext, hashed here) is for tests only."""
        if not NODE_ID_RE.fullmatch(node_id):
            raise ClaimError("bad_node_id")
        if not valid_room(room):
            raise ClaimError("bad_room")
        with self._locked() as data:
            rec = data["nodes"].setdefault(node_id, {"credential_sha256": None})
            if allow_shared_token and credential is None and rec.get("credential_sha256"):
                raise ClaimError("node_claimed")
            rec["room"] = room
            rec["allow_shared_token"] = bool(allow_shared_token)
            if credential is not None:
                rec["credential_sha256"] = sha256_hex(credential)
            rec.setdefault("claimed_at", None)

    def set_room(self, node_id: str, room: str) -> None:
        if not valid_room(room):
            raise ClaimError("bad_room")
        with self._locked() as data:
            if node_id not in data["nodes"]:
                raise ClaimError("unknown_node")
            data["nodes"][node_id]["room"] = room

    def set_shared_token(self, node_id: str, allow: bool) -> None:
        with self._locked() as data:
            if node_id not in data["nodes"]:
                raise ClaimError("unknown_node")
            # the transition is pre-claim only: a claimed node must be revoked first
            if allow and data["nodes"][node_id].get("credential_sha256"):
                raise ClaimError("node_claimed")
            data["nodes"][node_id]["allow_shared_token"] = bool(allow)

    def revoke(self, node_id: str) -> None:
        """Drop the node's credential and legacy allowance (it keeps its room row)."""
        with self._locked() as data:
            if node_id not in data["nodes"]:
                raise ClaimError("unknown_node")
            data["nodes"][node_id]["credential_sha256"] = None
            data["nodes"][node_id]["allow_shared_token"] = False
            data["pending"].pop(node_id, None)

    def remove(self, node_id: str) -> None:
        with self._locked() as data:
            if data["nodes"].pop(node_id, None) is None:
                raise ClaimError("unknown_node")
            data["pending"].pop(node_id, None)

    def listing(self) -> dict[str, Any]:
        """Nodes and pending claims, without any hash or secret material."""
        now = self._clock()
        data = self.snapshot()
        nodes = {
            nid: {
                "room": rec["room"],
                "credential": bool(rec.get("credential_sha256")),
                "allow_shared_token": bool(rec.get("allow_shared_token")),
                "claimed_at": rec.get("claimed_at"),
            }
            for nid, rec in sorted(data["nodes"].items())
        }
        pending = {
            nid: {
                "expires_in": int(p.get("expires", 0) - now),
                "approved_room": p.get("approved_room"),
            }
            for nid, p in sorted(data["pending"].items())
            if p.get("expires", 0) > now
        }
        return {"nodes": nodes, "pending": pending}
