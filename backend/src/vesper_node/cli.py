"""Operator CLI for the node registry (task 08). Edits the registry file directly, locally.

    vesper-node claim CODE --room ROOM                  approve the code a node shows
    vesper-node nodes list                              nodes + pending claims (no secrets)
    vesper-node nodes add NODE_ID --room ROOM [--allow-shared-token]
    vesper-node nodes set-room NODE_ID ROOM
    vesper-node nodes allow-shared-token NODE_ID on|off
    vesper-node nodes revoke NODE_ID                    drop credential + shared-token access
    vesper-node nodes remove NODE_ID                    delete the row

Being able to run this as the Studio user (who owns the 600 registry file) **is** the claim
authority: nothing here goes over the network, and the shared node token is not involved.
The running service picks up every change on its next request (no restart).

``--allow-shared-token`` is the **transition switch** for boards whose firmware predates the
claim flow (task 11 / F3): that one registered node may send turns with only the shared
``VESPER_NODE_TOKEN`` + ``X-Node-Id``. It is per node, off by default, and switched off
automatically when the node completes a claim. Unregistered nodes are always refused.

Output never contains a claim code, claim secret or credential.
"""

from __future__ import annotations

import argparse
import datetime as dt
import sys
from collections.abc import Sequence

from .config import ConfigError, resolve_registry_path
from .registry import ClaimError, Registry, RegistryError

EX_USAGE = 64
EX_DATAERR = 65
EX_CONFIG = 78

_MESSAGES = {
    "bad_room": "room must be 1-40 letters/digits/space/-/_ (starting and ending alnum)",
    "bad_node_id": "node id must match [A-Za-z0-9._:-]{1,128}",
    "unknown_node": "no such node in the registry",
    "claim_not_found": "no pending claim with that code (wrong, expired or replaced)",
    "node_claimed": "node holds a credential; shared-token access is pre-claim only (revoke first)",
    "claim_locked": "too many wrong codes: approvals locked for 15 min, pending claims dropped",
}


def _parser() -> argparse.ArgumentParser:
    ap = argparse.ArgumentParser(prog="vesper-node", description="node registry operator CLI")
    sub = ap.add_subparsers(dest="cmd", required=True)
    claim = sub.add_parser("claim", help="approve a claim code shown on a node's screen")
    claim.add_argument("code")
    claim.add_argument("--room", required=True)

    nodes = sub.add_parser("nodes", help="list or edit registered nodes")
    nsub = nodes.add_subparsers(dest="action", required=True)
    nsub.add_parser("list")
    add = nsub.add_parser("add", help="pre-register a node with a room")
    add.add_argument("node_id")
    add.add_argument("--room", required=True)
    add.add_argument(
        "--allow-shared-token",
        action="store_true",
        help="TRANSITION: let this node use the shared token only (pre-claim-flow firmware)",
    )
    sr = nsub.add_parser("set-room")
    sr.add_argument("node_id")
    sr.add_argument("room")
    ast = nsub.add_parser("allow-shared-token")
    ast.add_argument("node_id")
    ast.add_argument("state", choices=["on", "off"])
    for name in ("revoke", "remove"):
        nsub.add_parser(name).add_argument("node_id")
    return ap


def _when(epoch: object) -> str:
    if not isinstance(epoch, int | float):
        return "-"
    return dt.datetime.fromtimestamp(epoch).strftime("%Y-%m-%d %H:%M")


def _list(reg: Registry) -> None:
    data = reg.listing()
    print(f"registry: {reg.path}")
    if not data["nodes"]:
        print("nodes: (none)")
    for nid, n in data["nodes"].items():
        access = []
        if n["credential"]:
            access.append("credential")
        if n["allow_shared_token"]:
            access.append("SHARED-TOKEN (transition)")
        print(
            f"  {nid}  room={n['room']}  access={'+'.join(access) or 'none (unclaimed)'}  "
            f"claimed={_when(n['claimed_at'])}"
        )
    if data["pending"]:
        print("pending claims:")
    for nid, p in data["pending"].items():
        state = f"approved for {p['approved_room']}" if p["approved_room"] else "awaiting approval"
        print(f"  {nid}  {state}  expires_in={p['expires_in']}s")


def registry_cli(argv: Sequence[str]) -> int:
    try:
        args = _parser().parse_args(list(argv))
    except SystemExit as e:
        return EX_USAGE if e.code else 0
    try:
        reg = Registry(resolve_registry_path())
        if args.cmd == "claim":
            node_id, replaces = reg.approve(args.code, args.room.strip())
            print(f"approved: {node_id} -> room {args.room.strip()}")
            print("the node collects its credential on its next poll (within a few seconds).")
            if replaces:
                print(
                    "note: this node already had access; that access is replaced when the "
                    "node collects the new credential."
                )
        elif args.action == "list":
            _list(reg)
        elif args.action == "add":
            reg.add_node(
                args.node_id, args.room.strip(), allow_shared_token=args.allow_shared_token
            )
            print(f"registered: {args.node_id} -> room {args.room.strip()}")
            if args.allow_shared_token:
                print(
                    "WARNING: shared-token access is ON for this node: anyone holding "
                    "VESPER_NODE_TOKEN can speak as it. Turn it off once it has claimed."
                )
        elif args.action == "set-room":
            reg.set_room(args.node_id, args.room.strip())
            print(f"room: {args.node_id} -> {args.room.strip()}")
        elif args.action == "allow-shared-token":
            reg.set_shared_token(args.node_id, args.state == "on")
            print(f"shared-token access for {args.node_id}: {args.state}")
        elif args.action == "revoke":
            reg.revoke(args.node_id)
            print(f"revoked: {args.node_id} (credential and shared-token access removed)")
        elif args.action == "remove":
            reg.remove(args.node_id)
            print(f"removed: {args.node_id}")
    except ClaimError as e:
        print(f"refused: {_MESSAGES.get(e.code, e.code)}", file=sys.stderr)
        return EX_DATAERR
    except (RegistryError, ConfigError) as e:
        print(f"config error: {e}", file=sys.stderr)
        return EX_CONFIG
    return 0
