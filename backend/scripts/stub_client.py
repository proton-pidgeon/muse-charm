"""Stub node client: posts a push-to-talk WAV note to the backend and consumes the SSE reply.

    uv run python scripts/stub_client.py                        # 1 turn, generated note
    uv run python scripts/stub_client.py --turns 5 --json-out .build/latency.json
    uv run python scripts/stub_client.py --wav note.wav --url http://[::1]:8796

It stands in for the firmware (task 09) and speaks docs/node-wire-protocol.md v1:

* the note is re-emitted in the **firmware's exact shape**: a 44-byte header with
  ``0xFFFFFFFF`` RIFF/data sizes, followed by 16 kHz mono PCM16. Without ``--wav`` it is
  generated with macOS ``say`` + ``afconvert``, cycling through harmless prompts that never
  ask for a device action, a purchase or a message;
* it uploads the note with ``Authorization: Bearer <VESPER_NODE_TOKEN>`` (read from the
  environment or ``~/.config/vesper-voice/node.env``, never printed) and ``X-Node-Id``, plus
  ``X-Node-Credential`` from ``VESPER_NODE_CREDENTIAL`` if set (task 08; never printed). The
  node id must be registered (``vesper-node nodes list``), or the backend answers 403;
* it prints each SSE event. The transcript and reply are printed to this client's own
  stdout; the server never logs them;
* it resolves the relative ``audio_url`` against the turn URL, downloads the MP3 with the
  bearer, checks it is a real MP3 (ID3 tag or MPEG frame sync), and prints per-stage timings.
  ``--turns N`` reports median / p90 / min / max.

Exit status: 0 if every turn produced text plus a valid MP3, 1 otherwise.
"""

from __future__ import annotations

import argparse
import json
import os
import statistics
import subprocess
import sys
import tempfile
import time
import wave
from pathlib import Path
from urllib.parse import urljoin

import httpx

from vesper_node.config import DEFAULT_NODE_ENV_FILE, NODE_ENV_FILE_VAR, load_env_file
from vesper_node.wav import wav_header

HARMLESS_PROMPTS = (
    "Hello Vesper, what is two plus two?",
    "Tell me a one-sentence fun fact about owls.",
    "What is the capital of France?",
    "How many days are in a week?",
    "Tell me a one-sentence fun fact about the moon.",
)
STAGES = ("upload_ms", "stt_ms", "ask_ms", "tts_ms", "total_ms")


def node_token() -> str:
    token = os.environ.get("VESPER_NODE_TOKEN")
    if token:
        return token
    path = Path(os.environ.get(NODE_ENV_FILE_VAR) or DEFAULT_NODE_ENV_FILE).expanduser()
    token = load_env_file(path).get("VESPER_NODE_TOKEN")
    if not token:
        sys.exit(f"stub: VESPER_NODE_TOKEN not set and not in {path}")
    return token


def say_to_wav(text: str, workdir: Path, n: int) -> Path:
    aiff, out = workdir / f"note{n}.aiff", workdir / f"note{n}.wav"
    subprocess.run(["/usr/bin/say", "-o", str(aiff), text], check=True)  # noqa: S603
    subprocess.run(  # noqa: S603
        ["/usr/bin/afconvert", "-f", "WAVE", "-d", "LEI16@16000", "-c", "1", str(aiff), str(out)],
        check=True,
    )
    return out


def firmware_note(path: Path) -> bytes:
    with wave.open(str(path), "rb") as w:
        if (w.getnchannels(), w.getframerate(), w.getsampwidth()) != (1, 16000, 2):
            sys.exit(f"stub: {path} must be 16 kHz mono 16-bit")
        pcm = w.readframes(w.getnframes())
    return wav_header(0, streaming=True) + pcm


def is_mp3(data: bytes) -> bool:
    return data[:3] == b"ID3" or (len(data) > 1 and data[0] == 0xFF and data[1] & 0xE0 == 0xE0)


def node_headers(token: str, node_id: str) -> dict[str, str]:
    headers = {"Authorization": f"Bearer {token}", "X-Node-Id": node_id}
    credential = os.environ.get("VESPER_NODE_CREDENTIAL")
    if credential:
        headers["X-Node-Credential"] = credential
    return headers


def one_turn(client: httpx.Client, base: str, token: str, node_id: str, note: bytes) -> dict:
    turn_url = base.rstrip("/") + "/turn"
    headers = {
        **node_headers(token, node_id),
        "X-Vesper-Node-Protocol": "1",
        "Content-Type": "audio/wav",
        "Accept": "text/event-stream",
    }
    t0 = time.monotonic()
    result: dict = {"events": [], "ok": False, "audio_ms": (len(note) - 44) * 1000 // 32000}
    with client.stream("POST", turn_url, content=note, headers=headers) as r:
        result["status"] = r.status_code
        if r.status_code != 200:
            r.read()
            print(f"  HTTP {r.status_code}: {r.text}")
            return result
        event = None
        for line in r.iter_lines():
            if line.startswith(":"):
                continue
            if line.startswith("event: "):
                event = line[len("event: ") :]
            elif line.startswith("data: ") and event:
                data = json.loads(line[len("data: ") :])
                elapsed = time.monotonic() - t0
                result["events"].append(event)
                if event == "text_delta" and "first_text_s" not in result:
                    result["first_text_s"] = elapsed
                if event == "message_done":
                    result["message_done_s"] = elapsed
                    result["audio_url"] = data.get("audio_url")
                if event == "timing":
                    result["timing"] = data
                if event == "done":
                    result["ok"] = bool(data.get("ok"))
                print(f"  [{elapsed:5.2f}s] {event}: {json.dumps(data)}")
                event = None
    result["client_total_s"] = time.monotonic() - t0
    url = result.get("audio_url")
    if not url:
        print("  no audio_url")
        result["ok"] = False
        return result
    t1 = time.monotonic()
    audio = client.get(urljoin(turn_url, url), headers=node_headers(token, node_id))
    result["audio_fetch_s"] = time.monotonic() - t1
    result["mp3_bytes"] = len(audio.content)
    result["mp3_valid"] = audio.status_code == 200 and is_mp3(audio.content)
    print(
        f"  audio GET {audio.status_code} {audio.headers.get('content-type')} "
        f"bytes={len(audio.content)} mp3={'yes' if result['mp3_valid'] else 'NO'} "
        f"fetch={result['audio_fetch_s']:.2f}s"
    )
    result["ok"] = result["ok"] and result["mp3_valid"]
    return result


def summarize(results: list[dict]) -> dict:
    def stats(values: list[float]) -> dict:
        values = sorted(values)
        if not values:
            return {}
        p90 = values[min(len(values) - 1, round(0.9 * (len(values) - 1)))]
        return {
            "median": statistics.median(values),
            "p90": p90,
            "min": values[0],
            "max": values[-1],
            "n": len(values),
        }

    ok = [r for r in results if r.get("ok")]
    out = {
        s: stats([r["timing"][s] / 1000 for r in ok if r["timing"].get(s) is not None])
        for s in STAGES
    }
    out["first_text_s"] = stats([r["first_text_s"] for r in ok if "first_text_s" in r])
    out["client_total_s"] = stats([r["client_total_s"] for r in ok])
    out["audio_fetch_s"] = stats([r["audio_fetch_s"] for r in ok if "audio_fetch_s" in r])
    out["ok_turns"], out["turns"] = len(ok), len(results)
    providers = {r["timing"].get("stt_provider") for r in ok}
    out["stt_provider"] = ",".join(sorted(p for p in providers if p))
    return out


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__.split("\n", 1)[0])
    ap.add_argument("--url", default="http://[::1]:8796", help="backend base URL (no /turn)")
    ap.add_argument("--wav", type=Path, help="16 kHz mono 16-bit WAV (default: generate)")
    ap.add_argument("--say", help="text to synthesize with macOS say (default: harmless set)")
    ap.add_argument("--turns", type=int, default=1)
    ap.add_argument("--node-id", default="homelink-stubclient")
    ap.add_argument("--json-out", type=Path)
    args = ap.parse_args()

    token = node_token()
    notes: list[bytes] = []
    with tempfile.TemporaryDirectory(prefix="vesper-node-stub-") as tmp:
        for i in range(args.turns):
            if args.wav:
                path = args.wav
            else:
                prompt = args.say or HARMLESS_PROMPTS[i % len(HARMLESS_PROMPTS)]
                path = say_to_wav(prompt, Path(tmp), i)
            notes.append(firmware_note(path))

    results = []
    with httpx.Client(timeout=httpx.Timeout(60.0)) as client:
        for i, note in enumerate(notes):
            print(f"turn {i + 1}/{len(notes)}: note {len(note)} bytes")
            results.append(one_turn(client, args.url, token, args.node_id, note))
    summary = summarize(results)
    print("\nsummary (seconds; server-side stages from the SSE timing event):")
    for key in (*STAGES, "first_text_s", "client_total_s", "audio_fetch_s"):
        s = summary.get(key) or {}
        if s:
            print(
                f"  {key:15s} median={s['median']:.2f} p90={s['p90']:.2f} "
                f"min={s['min']:.2f} max={s['max']:.2f} n={s['n']}"
            )
    print(f"  ok_turns={summary['ok_turns']}/{summary['turns']} stt={summary['stt_provider']}")
    if args.json_out:
        args.json_out.parent.mkdir(parents=True, exist_ok=True)
        slim = [{k: v for k, v in r.items() if k != "audio_url"} for r in results]
        args.json_out.write_text(json.dumps({"summary": summary, "turns": slim}, indent=2))
    return 0 if summary["ok_turns"] == summary["turns"] else 1


if __name__ == "__main__":
    raise SystemExit(main())
