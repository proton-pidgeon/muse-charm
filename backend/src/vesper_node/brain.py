"""Client for the Vesper brain's ``POST /ask`` (the agent turn): a thin shim, no agent code.

It mirrors ``vesper-voice/server/src/vesper_voice/brain_llm.py::BrainLLM.ask``:

* request ``{"text", "device_id": node_id, "channel": "node"}`` with
  ``Authorization: Bearer <VESPER_BRAIN_TOKEN>``. The node token is never forwarded.
* client timeout 30 s, above the brain's 25 s ``ASK_DEADLINE_S``. The brain does not cancel
  a turn when its client disconnects, so a shorter timeout could report failure while Lobe
  mutations are still running.
* **no retries and no redirects**, because a retried ask could toggle a device twice.
* a text over the ``/ask`` 1000-char limit is refused rather than clipped.
* logs carry status codes, exception class names, char counts and latency. They never carry
  the token, the request text or the reply.

Announcements (task 18): :meth:`BrainClient.claim_announcements` calls the brain's
``POST /node/announcements/claim`` (same host, port and bearer as ``/ask``) for the node's due
timers and reminders. The brain marks what it returns as delivered (at most once), so the call
is never retried. Any failure is an empty list and a warning (status or exception class only);
the announcement text is never logged.

Room context (task 08): the caller prepends the node's registry room with
:func:`with_room_context` before calling :meth:`BrainClient.ask`, so ``text`` here is already
the final turn text.
"""

from __future__ import annotations

import logging
import time
from dataclasses import dataclass
from typing import Any
from urllib.parse import urlsplit, urlunsplit

import httpx

log = logging.getLogger("vesper_node.brain")

ASK_TIMEOUT_S = 30.0
MAX_TEXT_CHARS = 1000  # brain_service.MAX_TEXT_CHARS
NODE_CHANNEL = "node"
CLAIM_PATH = "/node/announcements/claim"
CLAIM_TIMEOUT_S = 5.0
MAX_ANNOUNCEMENTS = 5  # the brain returns at most 5 per claim
MAX_ANNOUNCEMENT_CHARS = 200  # the brain's template cap


def claim_url_for(ask_url: str) -> str:
    """The brain's claim URL: the ``/ask`` URL with its ``/ask`` path replaced by
    :data:`CLAIM_PATH` (any prefix the brain is mounted under is kept)."""
    parts = urlsplit(ask_url)
    path = parts.path[: -len("/ask")] if parts.path.endswith("/ask") else ""
    return urlunsplit((parts.scheme, parts.netloc, path.rstrip("/") + CLAIM_PATH, "", ""))


def parse_announcements(payload: Any) -> tuple[list[str], int]:
    """``(texts, dropped)`` from a claim response body. Strict: the body must be an object
    whose ``announcements`` is a list; only its first :data:`MAX_ANNOUNCEMENTS` entries are
    looked at, and each must be an object with a non-empty string ``text`` of at most
    :data:`MAX_ANNOUNCEMENT_CHARS` chars. Anything else is dropped (and counted)."""
    items = payload.get("announcements") if isinstance(payload, dict) else None
    if not isinstance(items, list):
        return [], 0
    texts: list[str] = []
    for item in items[:MAX_ANNOUNCEMENTS]:
        text = item.get("text") if isinstance(item, dict) else None
        if isinstance(text, str) and text.strip() and len(text) <= MAX_ANNOUNCEMENT_CHARS:
            texts.append(" ".join(text.split()))
    return texts, len(items) - len(texts)


def with_room_context(text: str, room: str | None) -> str:
    """Prepend the node's room (registry value, operator-set and charset-restricted)."""
    if not room:
        return text
    return f"[Vesper node in the {room}] {text}"


class AskError(RuntimeError):
    """The brain could not produce a reply. The message is a short label."""


@dataclass(frozen=True)
class Reply:
    text: str
    actions: int
    latency_s: float


class BrainClient:
    def __init__(
        self,
        *,
        ask_url: str,
        token: str,
        timeout_s: float = ASK_TIMEOUT_S,
        transport: httpx.AsyncBaseTransport | None = None,
    ) -> None:
        self._ask_url = ask_url
        self._claim_url = claim_url_for(ask_url)
        self._token = token
        self._client = httpx.AsyncClient(
            timeout=httpx.Timeout(timeout_s), follow_redirects=False, transport=transport
        )

    async def ask(self, text: str, *, node_id: str) -> Reply:
        started = time.monotonic()
        try:
            resp = await self._client.post(
                self._ask_url,
                json={"text": text, "device_id": node_id, "channel": NODE_CHANNEL},
                headers={"Authorization": f"Bearer {self._token}"},
            )
        except httpx.TimeoutException:
            log.warning("ask timed out: node=%s after=%.1fs", node_id, time.monotonic() - started)
            raise AskError("timeout") from None
        except httpx.HTTPError as e:  # class name only: messages can embed URLs
            log.warning("ask failed: node=%s reason=%s", node_id, type(e).__name__)
            raise AskError("unreachable") from None
        latency = time.monotonic() - started
        if resp.status_code != 200:
            log.warning("ask failed: node=%s status=%d", node_id, resp.status_code)
            raise AskError(f"http_{resp.status_code}")
        try:
            payload = resp.json()
            spoken = payload.get("text")
            actions = payload.get("actions") or []
        except (ValueError, AttributeError):
            spoken, actions = None, []
        if not isinstance(spoken, str) or not spoken.strip():
            log.warning("ask failed: node=%s reason=no_text", node_id)
            raise AskError("no_text")
        n_actions = len(actions) if isinstance(actions, list) else 0
        log.info(
            "ask ok: node=%s chars_in=%d chars_out=%d actions=%d latency=%.2fs",
            node_id,
            len(text),
            len(spoken),
            n_actions,
            latency,
        )
        return Reply(spoken.strip(), n_actions, latency)

    async def claim_announcements(self, *, node_id: str) -> list[str]:
        """Due announcements for ``node_id`` (spoken sentences), oldest first. Never raises:
        any failure is ``[]`` plus a warning. No retries (the claim is at-most-once)."""
        started = time.monotonic()
        try:
            resp = await self._client.post(
                self._claim_url,
                json={"device_id": node_id},
                headers={"Authorization": f"Bearer {self._token}"},
                timeout=CLAIM_TIMEOUT_S,
            )
        except httpx.TimeoutException:
            log.warning("announcements claim timed out: node=%s", node_id)
            return []
        except httpx.HTTPError as e:  # class name only: messages can embed URLs
            log.warning("announcements claim failed: node=%s reason=%s", node_id, type(e).__name__)
            return []
        if resp.status_code != 200:
            log.warning("announcements claim failed: node=%s status=%d", node_id, resp.status_code)
            return []
        try:
            payload = resp.json()
        except ValueError:
            log.warning("announcements claim failed: node=%s reason=bad_json", node_id)
            return []
        texts, dropped = parse_announcements(payload)
        malformed = not isinstance(payload, dict) or not isinstance(
            payload.get("announcements"), list
        )
        if malformed or dropped:
            log.warning(
                "announcements claim: node=%s malformed=%s dropped=%d",
                node_id,
                malformed,
                dropped,
            )
        if texts:
            log.info(
                "announcements claim ok: node=%s count=%d latency=%.2fs",
                node_id,
                len(texts),
                time.monotonic() - started,
            )
        return texts

    async def aclose(self) -> None:
        await self._client.aclose()
