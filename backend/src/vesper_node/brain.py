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

Task 08 hook: room context (node registry lookup) will be prepended to ``text`` here, before
the POST. Not built in task 07.
"""

from __future__ import annotations

import logging
import time
from dataclasses import dataclass

import httpx

log = logging.getLogger("vesper_node.brain")

ASK_TIMEOUT_S = 30.0
MAX_TEXT_CHARS = 1000  # brain_service.MAX_TEXT_CHARS
NODE_CHANNEL = "node"


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
        self._token = token
        self._client = httpx.AsyncClient(
            timeout=httpx.Timeout(timeout_s), follow_redirects=False, transport=transport
        )

    async def ask(self, text: str, *, node_id: str) -> Reply:
        # TODO(task 08): prepend registry room context for node_id here.
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

    async def aclose(self) -> None:
        await self._client.aclose()
