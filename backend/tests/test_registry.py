"""Task 08: node registry, claim flow, per-node credentials, room context."""

from __future__ import annotations

import json
import logging
import os
import stat
import threading

import pytest
from conftest import (
    NODE_CREDENTIAL,
    NODE_ID,
    NODE_TOKEN,
    TRANSCRIPT,
    bearer,
    client_for,
    make_note,
    make_settings,
    parse_sse,
)
from test_auth import call_asgi

from vesper_node import logsafe
from vesper_node.registry import (
    APPROVE_MAX_FAILS,
    CLAIM_TTL_S,
    MAX_BAD_POLLS,
    MAX_PENDING,
    ClaimError,
    Registry,
    RegistryError,
)

NEW_NODE = "homelink-112233445566"
ADMIN_TOKEN = "admin-token-" + "a" * 40


class Clock:
    def __init__(self, t: float = 1_800_000_000.0) -> None:
        self.t = t

    def __call__(self) -> float:
        return self.t


def node_headers(node_id: str, credential: str | None = None) -> dict[str, str]:
    h = {**bearer(), "X-Node-Id": node_id}
    if credential is not None:
        h["X-Node-Credential"] = credential
    return h


async def turn(app, node_id: str, credential: str | None = None):
    async with client_for(app) as c:
        return await c.post(
            "/turn",
            content=make_note(),
            headers={**node_headers(node_id, credential), "Content-Type": "audio/wav"},
        )


async def start(c, node_id: str = NEW_NODE):
    return await c.post("/claim/start", headers=node_headers(node_id))


async def poll(c, secret: str, node_id: str = NEW_NODE):
    return await c.post("/claim/poll", headers={**node_headers(node_id), "X-Claim-Secret": secret})


def ask_texts(providers) -> list[str]:
    return [json.loads(r.content)["text"] for r in providers.calls["ask"]]


# ---- the end-to-end claim ceremony -------------------------------------------------------


async def test_claim_issues_credential_and_turn_carries_room(build, providers, registry) -> None:
    app = build()
    r = await turn(app, NEW_NODE)  # unknown node: refused before STT
    assert (r.status_code, r.json()) == (403, {"error": "node_unauthorized"})

    async with client_for(app) as c:
        s = await start(c)
        assert s.status_code == 200 and s.headers["cache-control"] == "no-store"
        body = s.json()
        assert body["status"] == "pending" and body["expires_in"] == CLAIM_TTL_S
        code, secret = body["claim_code"], body["claim_secret"]
        assert len(code) == 9 and code[4] == "-"
        p = await poll(c, secret)
        assert p.status_code == 202 and p.json()["status"] == "pending"

        # Kevin types the code (lower-case, no dash is fine) via the local CLI path
        node_id, replaces = registry.approve(code.replace("-", "").lower(), "office")
        assert (node_id, replaces) == (NEW_NODE, False)

        got = await poll(c, secret)
        assert got.status_code == 200 and got.headers["cache-control"] == "no-store"
        claimed = got.json()
        assert claimed["status"] == "claimed" and claimed["room"] == "office"
        assert claimed["node_id"] == NEW_NODE
        credential = claimed["credential"]
        assert credential.startswith("vnc_") and len(credential) >= 40
        # delivered exactly once
        again = await poll(c, secret)
        assert (again.status_code, again.json()) == (404, {"error": "claim_not_found"})

    r = await turn(app, NEW_NODE, credential)
    assert r.status_code == 200
    assert dict(parse_sse(r.text))["done"] == {"ok": True}
    (req,) = providers.calls["ask"]
    assert json.loads(req.content) == {
        "text": f"[Vesper node in the office] {TRANSCRIPT}",
        "device_id": NEW_NODE,
        "channel": "node",
    }
    # only the hash is at rest
    raw = registry.path.read_text()
    assert credential not in raw and secret not in raw and code.replace("-", "") not in raw


async def test_each_node_gets_its_own_room(build, providers, registry) -> None:
    registry.add_node("homelink-000000000002", "office", credential="vnc_" + "o" * 43)
    app = build()
    assert (await turn(app, NODE_ID, NODE_CREDENTIAL)).status_code == 200
    assert (await turn(app, "homelink-000000000002", "vnc_" + "o" * 43)).status_code == 200
    # a node's credential does not work for another node id
    assert (await turn(app, "homelink-000000000002", NODE_CREDENTIAL)).status_code == 403
    assert ask_texts(providers) == [
        f"[Vesper node in the kitchen] {TRANSCRIPT}",
        f"[Vesper node in the office] {TRANSCRIPT}",
    ]


@pytest.mark.parametrize(
    "credential", [None, "", "vnc_" + "x" * 43, NODE_CREDENTIAL[:-1], NODE_TOKEN]
)
async def test_wrong_or_missing_credential_rejected(build, providers, credential) -> None:
    r = await turn(build(), NODE_ID, credential)
    assert (r.status_code, r.json()) == (403, {"error": "node_unauthorized"})
    assert providers.calls["eleven_stt"] == [] and providers.calls["ask"] == []


async def test_unclaimed_and_unknown_nodes_rejected_before_body(build, registry) -> None:
    registry.add_node("homelink-unclaimed01", "hall")  # registered, never claimed
    app = build()
    for node_id, cred in (
        ("homelink-unclaimed01", None),
        ("homelink-unclaimed01", NODE_CREDENTIAL),
        ("homelink-unknown0001", None),
        ("homelink-unknown0001", NODE_CREDENTIAL),
    ):
        headers = [
            (b"authorization", f"Bearer {NODE_TOKEN}".encode()),
            (b"x-node-id", node_id.encode()),
            (b"content-type", b"audio/wav"),
        ]
        if cred:
            headers.append((b"x-node-credential", cred.encode()))
        pulled, status, _h, body = await call_asgi(app, "POST", "/turn", headers)
        assert (status, pulled, body) == (403, 0, b'{"error": "node_unauthorized"}')


async def test_duplicate_credential_headers_rejected(build) -> None:
    headers = [
        (b"authorization", f"Bearer {NODE_TOKEN}".encode()),
        (b"x-node-id", NODE_ID.encode()),
        (b"x-node-credential", NODE_CREDENTIAL.encode()),
        (b"x-node-credential", NODE_CREDENTIAL.encode()),
    ]
    pulled, status, _h, _b = await call_asgi(build(), "POST", "/turn", headers)
    assert (status, pulled) == (403, 0)


async def test_audio_needs_node_credential_too(build) -> None:
    app = build()
    r = await turn(app, NODE_ID, NODE_CREDENTIAL)
    url = "/" + dict(parse_sse(r.text))["message_done"]["audio_url"]
    async with client_for(app) as c:
        ok = await c.get(url, headers=node_headers(NODE_ID, NODE_CREDENTIAL))
        no_cred = await c.get(url, headers=node_headers(NODE_ID))
        no_node = await c.get(url, headers=bearer())
    assert ok.status_code == 200
    assert (no_cred.status_code, no_node.status_code) == (403, 400)


async def test_room_context_counts_toward_ask_limit(build, providers) -> None:
    providers.transcript = "a" * 990  # fits alone, not with the room prefix
    events = parse_sse((await turn(build(), NODE_ID, NODE_CREDENTIAL)).text)
    assert dict(events)["error"]["code"] == "transcript_too_long"
    assert providers.calls["ask"] == []


# ---- the shared token alone can't claim ---------------------------------------------------


async def test_shared_token_cannot_approve_or_reach_admin(build, registry) -> None:
    app = build()  # admin route disabled (no VESPER_NODE_ADMIN_TOKEN)
    async with client_for(app) as c:
        code = (await start(c)).json()["claim_code"]
        r = await c.post("/admin/claim", headers=bearer(), json={"code": code, "room": "x"})
        assert (r.status_code, r.json()) == (404, {"error": "not_found"})
    assert registry.listing()["pending"][NEW_NODE]["approved_room"] is None


async def test_admin_claim_route(build, registry) -> None:
    app = build(make_settings(admin_token=ADMIN_TOKEN))
    async with client_for(app) as c:
        body = (await start(c)).json()
        payload = {"code": body["claim_code"], "room": "garage"}
        for bad in ({}, {"X-Vesper-Node-Admin": NODE_TOKEN}, {"X-Vesper-Node-Admin": "x" * 52}):
            r = await c.post("/admin/claim", headers={**bearer(), **bad}, json=payload)
            assert (r.status_code, r.json()) == (403, {"error": "forbidden"})
        # the admin token without the shared bearer is refused at layer 1
        r = await c.post("/admin/claim", headers={"X-Vesper-Node-Admin": ADMIN_TOKEN}, json=payload)
        assert r.status_code == 401
        admin = {**bearer(), "X-Vesper-Node-Admin": ADMIN_TOKEN}
        r = await c.post("/admin/claim", headers=admin, json={"code": "ZZZZ-ZZZZ", "room": "x"})
        assert (r.status_code, r.json()) == (404, {"error": "claim_not_found"})
        r = await c.post("/admin/claim", headers=admin, json={**payload, "room": "bad/room"})
        assert (r.status_code, r.json()) == (400, {"error": "bad_room"})
        r = await c.post("/admin/claim", headers=admin, content=b"not json")
        assert (r.status_code, r.json()) == (400, {"error": "bad_request"})
        r = await c.post("/admin/claim", headers=admin, json=payload)
        assert r.status_code == 200
        assert r.json() == {"node_id": NEW_NODE, "room": "garage", "replaces_access": False}
        got = await poll(c, body["claim_secret"])
    assert got.json()["room"] == "garage"


async def test_admin_token_checked_before_body(build) -> None:
    app = build(make_settings(admin_token=ADMIN_TOKEN))
    headers = [
        (b"authorization", f"Bearer {NODE_TOKEN}".encode()),
        (b"x-vesper-node-admin", b"wrong-" + b"w" * 40),
        (b"content-type", b"application/json"),
    ]
    pulled, status, _h, _b = await call_asgi(app, "POST", "/admin/claim", headers)
    assert (status, pulled) == (403, 0)


# ---- claim-code hygiene -------------------------------------------------------------------


def test_codes_expire(tmp_path) -> None:
    clock = Clock()
    reg = Registry(tmp_path / "nodes.json", clock=clock)
    started = reg.start_claim(NEW_NODE)
    clock.t += CLAIM_TTL_S + 1
    with pytest.raises(ClaimError) as e:
        reg.approve(started.code, "office")
    assert e.value.code == "claim_not_found"
    with pytest.raises(ClaimError):
        reg.poll(NEW_NODE, started.secret)


def test_approved_but_uncollected_claim_expires(tmp_path) -> None:
    clock = Clock()
    reg = Registry(tmp_path / "nodes.json", clock=clock)
    started = reg.start_claim(NEW_NODE)
    reg.approve(started.code, "office")
    clock.t += CLAIM_TTL_S + 1
    with pytest.raises(ClaimError):
        reg.poll(NEW_NODE, started.secret)
    assert NEW_NODE not in reg.listing()["nodes"]


def test_codes_single_use_and_restart_replaces(tmp_path) -> None:
    clock = Clock()
    reg = Registry(tmp_path / "nodes.json", clock=clock)
    first = reg.start_claim(NEW_NODE)
    with pytest.raises(ClaimError) as e:
        reg.start_claim(NEW_NODE)  # restart too soon
    assert (e.value.code, e.value.retry_after) == ("rate_limited", 5)
    clock.t += 6
    second = reg.start_claim(NEW_NODE)
    assert second.code != first.code
    with pytest.raises(ClaimError):
        reg.approve(first.code, "office")  # replaced
    with pytest.raises(ClaimError):
        reg.poll(NEW_NODE, first.secret)
    reg.approve(second.code, "office")
    assert reg.poll(NEW_NODE, second.secret).credential
    with pytest.raises(ClaimError):
        reg.approve(second.code, "office")  # consumed


def test_pending_claims_are_bounded(tmp_path) -> None:
    reg = Registry(tmp_path / "nodes.json")
    for i in range(MAX_PENDING):
        reg.start_claim(f"homelink-{i:012d}")
    with pytest.raises(ClaimError) as e:
        reg.start_claim("homelink-overflow")
    assert e.value.code == "too_many_pending"


async def test_claim_start_429_over_http(build) -> None:
    async with client_for(build()) as c:
        assert (await start(c)).status_code == 200
        r = await start(c)
    assert (r.status_code, r.json()) == (429, {"error": "rate_limited"})
    assert r.headers["retry-after"] == "5"


def test_wrong_code_guessing_locks_out_and_wipes(tmp_path) -> None:
    reg = Registry(tmp_path / "nodes.json")
    real = reg.start_claim(NEW_NODE)
    for _ in range(APPROVE_MAX_FAILS):
        with pytest.raises(ClaimError):
            reg.approve("2222-2222" if real.code != "2222-2222" else "3333-3333", "office")
    with pytest.raises(ClaimError) as e:
        reg.approve(real.code, "office")  # the right code no longer works
    assert e.value.code == "claim_locked"
    assert reg.listing()["pending"] == {}


def test_wrong_claim_secret_polls_drop_the_claim(tmp_path) -> None:
    reg = Registry(tmp_path / "nodes.json")
    real = reg.start_claim(NEW_NODE)
    reg.approve(real.code, "office")
    for _ in range(MAX_BAD_POLLS):
        with pytest.raises(ClaimError):
            reg.poll(NEW_NODE, "vcs_" + "w" * 43)
    with pytest.raises(ClaimError):
        reg.poll(NEW_NODE, real.secret)
    assert NEW_NODE not in reg.listing()["nodes"]


def test_poll_is_bound_to_the_requesting_node(tmp_path) -> None:
    reg = Registry(tmp_path / "nodes.json")
    real = reg.start_claim(NEW_NODE)
    reg.approve(real.code, "office")
    with pytest.raises(ClaimError):
        reg.poll("homelink-attacker", real.secret)  # right secret, wrong node
    assert reg.poll(NEW_NODE, real.secret).status == "claimed"


@pytest.mark.parametrize("room", ["", " ", "a" * 41, "kitchen/../x", "kitchen\nignore", "-x", "ü"])
def test_bad_rooms_refused(registry, room) -> None:
    with pytest.raises(ClaimError):
        registry.add_node(NEW_NODE, room)


def test_free_form_rooms_allowed(registry) -> None:
    for room in ("office", "Kids room 2", "guest_bath", "Upstairs-Landing"):
        registry.set_room(NODE_ID, room)
        assert registry.listing()["nodes"][NODE_ID]["room"] == room


# ---- re-claim, revoke, transition ---------------------------------------------------------


async def test_reclaim_replaces_old_credential(build, registry) -> None:
    app = build()
    started = registry.start_claim(NODE_ID)
    assert registry.approve(started.code, "den") == (NODE_ID, True)
    # old credential still works until the node collects the new one
    assert (await turn(app, NODE_ID, NODE_CREDENTIAL)).status_code == 200
    new = registry.poll(NODE_ID, started.secret).credential
    assert (await turn(app, NODE_ID, NODE_CREDENTIAL)).status_code == 403
    assert (await turn(app, NODE_ID, new)).status_code == 200


async def test_revoke_and_remove(build, registry) -> None:
    app = build()
    registry.revoke(NODE_ID)
    assert (await turn(app, NODE_ID, NODE_CREDENTIAL)).status_code == 403
    registry.remove(NODE_ID)
    assert NODE_ID not in registry.listing()["nodes"]
    with pytest.raises(ClaimError):
        registry.revoke(NODE_ID)


async def test_shared_token_transition_is_per_node_and_ends_at_claim(
    build, providers, registry
) -> None:
    legacy = "homelink-c86320"
    registry.add_node(legacy, "office", allow_shared_token=True)
    app = build()
    r = await turn(app, legacy)  # old firmware: shared token + X-Node-Id only
    assert r.status_code == 200
    assert ask_texts(providers)[-1] == f"[Vesper node in the office] {TRANSCRIPT}"
    # a wrong credential is still a failure (no silent downgrade)
    assert (await turn(app, legacy, "vnc_" + "z" * 43)).status_code == 403
    # other nodes get nothing from it
    assert (await turn(app, "homelink-c86321")).status_code == 403
    # F3 firmware claims -> the shared-token allowance is gone
    started = registry.start_claim(legacy)
    registry.approve(started.code, "office")
    cred = registry.poll(legacy, started.secret).credential
    assert (await turn(app, legacy)).status_code == 403
    assert (await turn(app, legacy, cred)).status_code == 200
    assert registry.listing()["nodes"][legacy]["allow_shared_token"] is False


def test_transition_switch_is_off_by_default(registry) -> None:
    registry.add_node(NEW_NODE, "office")
    assert registry.listing()["nodes"][NEW_NODE]["allow_shared_token"] is False
    assert not registry.authenticate(NEW_NODE, None).ok


# ---- storage ------------------------------------------------------------------------------


def test_registry_file_is_600_in_700_dir(registry) -> None:
    assert stat.S_IMODE(registry.path.stat().st_mode) == 0o600
    assert stat.S_IMODE(registry.path.parent.stat().st_mode) == 0o700
    assert stat.S_IMODE(registry.lock_path.stat().st_mode) == 0o600
    leftovers = [p for p in registry.path.parent.iterdir() if p.name.startswith(".nodes.json.")]
    assert leftovers == []


async def test_group_readable_registry_fails_closed(build, registry, caplog) -> None:
    os.chmod(registry.path, 0o640)
    with pytest.raises(RegistryError):
        registry.check()
    caplog.set_level(logging.WARNING)
    assert (await turn(build(), NODE_ID, NODE_CREDENTIAL)).status_code == 403
    assert "registry_error" in caplog.text
    with pytest.raises(RegistryError):
        registry.start_claim(NEW_NODE)  # writes refuse too


async def test_malformed_registry_fails_closed(build, registry) -> None:
    registry.path.write_text("{not json")
    assert (await turn(build(), NODE_ID, NODE_CREDENTIAL)).status_code == 403
    async with client_for(build()) as c:
        r = await start(c)
    assert (r.status_code, r.json()) == (503, {"error": "registry_unavailable"})


def test_symlinked_registry_refused(tmp_path, registry) -> None:
    link = tmp_path / "link.json"
    link.symlink_to(registry.path)
    with pytest.raises(RegistryError):
        Registry(link).check()


def test_missing_registry_is_empty_and_strict(tmp_path) -> None:
    reg = Registry(tmp_path / "absent" / "nodes.json")
    assert reg.check() == 0
    assert not reg.authenticate(NODE_ID, NODE_CREDENTIAL).ok
    assert not (tmp_path / "absent").exists()  # reading never creates anything


async def test_service_sees_cli_edits_without_restart(build, registry) -> None:
    app = build()
    assert (await turn(app, NEW_NODE, "vnc_" + "n" * 43)).status_code == 403
    cli_side = Registry(registry.path)  # a separate process's view of the same file
    cli_side.add_node(NEW_NODE, "porch", credential="vnc_" + "n" * 43)
    assert (await turn(app, NEW_NODE, "vnc_" + "n" * 43)).status_code == 200


def test_concurrent_writers_lose_nothing(registry) -> None:
    other = Registry(registry.path)
    errors: list[BaseException] = []

    def work(reg: Registry, prefix: str) -> None:
        try:
            for i in range(25):
                reg.add_node(f"homelink-{prefix}{i:04d}", "room")
        except BaseException as e:  # noqa: BLE001
            errors.append(e)

    threads = [
        threading.Thread(target=work, args=(r, p))
        for r, p in ((registry, "a"), (other, "b"), (registry, "c"), (other, "d"))
    ]
    for t in threads:
        t.start()
    for t in threads:
        t.join()
    assert errors == []
    assert len(registry.listing()["nodes"]) == 1 + 4 * 25


def test_listing_has_no_secret_material(registry) -> None:
    started = registry.start_claim(NEW_NODE)
    text = json.dumps(registry.listing())
    assert "sha256" not in text and started.secret not in text
    assert started.code not in text and started.code.replace("-", "") not in text


# ---- logs ---------------------------------------------------------------------------------


async def test_claim_flow_never_logs_codes_secrets_or_credentials(build, registry, caplog) -> None:
    caplog.set_level(logging.DEBUG)
    app = build(make_settings(admin_token=ADMIN_TOKEN))
    async with client_for(app) as c:
        body = (await start(c)).json()
        await poll(c, body["claim_secret"])
        await poll(c, "vcs_" + "w" * 43)
        admin = {**bearer(), "X-Vesper-Node-Admin": ADMIN_TOKEN}
        await c.post("/admin/claim", headers=admin, json={"code": "2222-2223", "room": "x"})
        await c.post(
            "/admin/claim", headers=admin, json={"code": body["claim_code"], "room": "lab"}
        )
        cred = (await poll(c, body["claim_secret"])).json()["credential"]
    await turn(app, NEW_NODE, cred)
    await turn(app, NEW_NODE, cred[:-1] + "x")
    text = caplog.text + "".join(str(r.args) for r in caplog.records)
    assert "claim started: node=" + NEW_NODE in text
    assert "credential issued: node=" + NEW_NODE in text
    for needle in (
        cred,
        cred[4:],
        body["claim_secret"],
        body["claim_code"],
        body["claim_code"].replace("-", ""),
        ADMIN_TOKEN,
        NODE_TOKEN,
    ):
        assert needle not in text, "secret material in logs"


def test_logsafe_masks_credential_shapes() -> None:
    cred = "vnc_" + "Q" * 43
    out = logsafe.redact_text(f"oops {cred} and X-Node-Credential: abc123 and vcs_{'s' * 43}")
    assert cred not in out and "abc123" not in out and "vcs_" + "s" * 43 not in out
