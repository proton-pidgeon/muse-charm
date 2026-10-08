# Task 18: Node tool access — Option B (Kevin approved 2026-10-08)

## Context
Kevin picked Option B from `vesper-node-tool-access-proposal.md`: unlock the node's
existing toolbox PLUS build new tools. The node is a shared household device.

## Part A — Prompt unlock (no code, fast)
File: `/Users/k3v/vesper-voice/server/src/vesper_voice/persona.py`, the `"node"` channel
(lines ~117-130). Replace the v1-scope paragraph:

> "v1 scope is conversation and home control... If he asks you to buy something,
> message a person, or do anything irreversible, do not do it."

with a version that ALLOWS the read-only tools the node channel already has:
`web_search`, `news_headlines`, `calendar_events`, `camera_snapshot` (describe only —
the node has no display for images). KEEP the refusal for irreversible actions
(buying, messaging people, anything that can't be undone). Keep the spoken-reply
rules (1-3 short sentences, no URLs/lists/markdown, say numbers aloud).

## Part B — New brain tools (build)
Build as brain tools in the vesper-voice repo, available to the `node` channel:
1. **Timers** — "set a timer for 10 minutes". Needs a timer service; the board needs
   an audio alert when it fires (firmware piece — note it, coordinate with the node
   firmware; a spoken "your timer is done" via TTS is acceptable v1).
2. **Reminders** — "remind me to call the plumber at 3". Storage + delivery as a
   voice announcement on the node at the right time.
3. **Shopping/todo lists** — "add milk to the shopping list", "what's on my list".
   List store, readable aloud.
4. **Dedicated weather** — faster/cleaner than web_search for voice ("what's the
   weather").

## Safety (non-negotiable)
- Shared household device: NEVER assume the speaker is Kevin.
- Read-only tools: safe to open, no auth question.
- Irreversible actions: stay refused. If a new tool has irreversible effects, FLAG
  it in the task notes and do NOT build it without a confirmation design.
- Home control on a shared device (unlock doors etc. by voice) is already allowed;
  note it, don't expand it.

## Done when
- Node can answer web/news/calendar/camera questions by voice (live PTT test).
- Timers, reminders, lists, weather work by voice (live PTT test each).
- Tests green, adversarial review per the skill gates.
- No irreversible tool was added without a confirmation design.

## Progress — lane 18B (muse-charm: node backend + firmware), 2026-10-08
Contract: `/tmp/t18-contract.md` (orchestrator). Lane 18A (vesper-voice: persona unlock, tools,
`POST /node/announcements/claim`) is separate.
- [x] Backend `GET /announcements` (same three auth layers as `/turn`; brain claim, 5 s, no
  retries/redirects; strict validation; reply TTS per item; brain failure → `200 []`; 5 s per-node
  rate limit → 429; not a conversation turn; text never logged). `backend make verify` green.
- [x] Wire doc: endpoint row, `GET /announcements` section, changelog. Peggy's
  `/vesper-node/*` handle is a wildcard + `strip_prefix`: no Peggy change.
- [x] Firmware 1.0.2: idle 15 s poll, `vesper_announce.c` parser/scheduler (host tests in
  `make test`), playback through the reply-MP3 path, PTT wins, SDK patch 0006. ESP-IDF build of a
  fresh `b1a3822` tree: exit 0, 0 warnings, 2,035,712 B.
- [ ] HUMAN GATE: publish 1.0.2 (`vesper-node firmware publish`) → node OTAs → set a timer and a
  reminder by voice → hear them announced on the node; press PTT during one (it stops at once).
- [ ] HUMAN GATE: live PTT tests of web/news/calendar/camera, timers, reminders, lists, weather
  (depend on 18A being merged and the brain restarted).
- No irreversible tool was added by 18B (it only reads/plays what the brain hands out).
