"""Auth before body: a missing/wrong token is a 401 and the request body is never read."""

from __future__ import annotations

from typing import Any

import pytest
from conftest import BRAIN_TOKEN, NODE_ID, NODE_TOKEN, client_for, make_note


class BodyTripwire:
    """ASGI ``receive`` that records any attempt to pull the request body."""

    def __init__(self) -> None:
        self.pulled = 0

    async def __call__(self) -> dict[str, Any]:
        self.pulled += 1
        raise AssertionError("request body was read before auth")


async def call_asgi(app, method: str, path: str, headers: list[tuple[bytes, bytes]]):
    receive = BodyTripwire()
    sent: list[dict[str, Any]] = []

    async def send(message: dict[str, Any]) -> None:
        sent.append(message)

    scope = {
        "type": "http",
        "asgi": {"version": "3.0"},
        "http_version": "1.1",
        "method": method,
        "scheme": "http",
        "path": path,
        "raw_path": path.encode(),
        "query_string": b"",
        "root_path": "",
        "headers": headers,
        "client": ("127.0.0.1", 5555),
        "server": ("127.0.0.1", 8796),
    }
    await app(scope, receive, send)
    start = next(m for m in sent if m["type"] == "http.response.start")
    body = b"".join(m.get("body", b"") for m in sent if m["type"] == "http.response.body")
    return receive.pulled, start["status"], dict(start["headers"]), body


BASE = [
    (b"x-node-id", NODE_ID.encode()),
    (b"content-type", b"audio/wav"),
    (b"content-length", str(len(make_note())).encode()),
]


@pytest.mark.parametrize(
    "auth_header",
    [
        None,
        b"Bearer wrong-token-" + b"x" * 40,
        b"Bearer " + NODE_TOKEN.encode()[:-1],
        b"Bearer " + BRAIN_TOKEN.encode(),  # the brain token is not a node token
        b"Basic " + NODE_TOKEN.encode(),
        NODE_TOKEN.encode(),
        b"Bearer",
    ],
)
@pytest.mark.parametrize(
    ("method", "path"), [("POST", "/turn"), ("GET", "/audio/" + "a" * 24 + ".mp3"), ("GET", "/x")]
)
async def test_bad_token_401_without_reading_body(build, auth_header, method, path) -> None:
    headers = list(BASE)
    if auth_header is not None:
        headers.append((b"authorization", auth_header))
    pulled, status, hdrs, body = await call_asgi(build(), method, path, headers)
    assert status == 401
    assert pulled == 0
    assert hdrs[b"www-authenticate"] == b"Bearer"
    assert hdrs[b"x-vesper-node-protocol"] == b"1"
    assert body == b'{"error": "unauthorized"}'


async def test_duplicate_authorization_headers_rejected(build) -> None:
    good = (b"authorization", b"Bearer " + NODE_TOKEN.encode())
    headers = [*BASE, good, good]
    pulled, status, _h, _b = await call_asgi(build(), "POST", "/turn", headers)
    assert (status, pulled) == (401, 0)


async def test_healthz_open_and_minimal(build) -> None:
    async with client_for(build()) as c:
        r = await c.get("/healthz")
    assert r.status_code == 200
    assert r.json() == {"ok": True}
    assert r.headers["x-vesper-node-protocol"] == "1"


async def test_healthz_post_still_needs_auth(build) -> None:
    async with client_for(build()) as c:
        r = await c.post("/healthz")
    assert r.status_code == 401


async def test_unknown_route_with_token_is_404_json(build) -> None:
    async with client_for(build()) as c:
        r = await c.get("/nope", headers={"Authorization": f"Bearer {NODE_TOKEN}"})
        r2 = await c.get("/turn", headers={"Authorization": f"Bearer {NODE_TOKEN}"})
    assert (r.status_code, r.json()) == (404, {"error": "not_found"})
    assert (r2.status_code, r2.json()) == (405, {"error": "method_not_allowed"})
