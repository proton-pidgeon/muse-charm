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
- [x] Review-gate fixes (escalation rung): backend mints all announcement TTS concurrently under
  one 2 s budget (over budget → `audio_url: null`, never a late reply) and the brain claim is 3 s,
  so a poll answers in ≤ ~5 s; `HEAD /announcements` is 405 and can never claim (pinned by test);
  kept items missing `id`/`kind` are counted (`unlabeled=`) in the log. Firmware: `ann_get`
  timeouts split 2 s connect / 8 s response (a dead server stalls a press ≤ ~2 s; a slow backend
  is still waited for; README states the worst case); announcements a press lands on are
  deferred, not dropped (`s_ann_pending`, `vn_keep_from` host-tested): only the one mid-speech
  is lost. `backend make verify` 582 green, `firmware make test` green, fresh-tree ESP-IDF build
  exit 0, 0 warnings, 2,035,712 B signed (unchanged).
- [ ] HUMAN GATE: publish 1.0.2 (`vesper-node firmware publish`) → node OTAs → set a timer and a
  reminder by voice → hear them announced on the node; press PTT during one (it stops at once).
- [ ] HUMAN GATE: live PTT tests of web/news/calendar/camera, timers, reminders, lists, weather
  (depend on 18A being merged and the brain restarted).
- No irreversible tool was added by 18B (it only reads/plays what the brain hands out).

## Result — /implement orchestrator, 2026-10-08 ~18:30 CDT
- **18A merged** in vesper-voice (merge 145973d + persona fixes ce36ce8, 9ecf1d4; pushed). Lint baseline cleared (43 ruff findings; blocking I/O in async tools moved to threads). Node persona unlocked web/news/calendar/camera + the new tools, kept the irreversible refusal, added the shared-device rule ("never assume the speaker is Kev"). New `household.py`: timers, reminders, shopping/todo lists, Open-Meteo weather, node channel only; store `~/.config/vesper-voice/household.json` (mode 600). `POST /node/announcements/claim` (at-most-once, oldest first, max 5, >6 h overdue dropped). `make verify` green.
- **18B merged** here (f95e635): `GET /announcements` + firmware 1.0.2 idle poll.
- **Review gate:** GPT-5.5 → Fable. 18A blocked twice → fixed on the fable rung: (1) `set_kev_location` removed from the node channel (it was pre-existing, but it contradicts the shared-device rule, and the new tools read Kev's location); (2) `list_clear` **not built**, because confirmation was only a model-set flag, which doesn't count as a confirmation design. 18B blocked on announcements being lost when claim + sequential TTS took longer than the node's 5 s wait. Fixed with concurrent TTS under a 2 s budget, a 3 s claim and a 2 s/8 s firmware timeout split. Delta reviews passed; advisories applied.
- **Live (stub node `homelink-stubclient`, real STT/brain/TTS, since removed):** timer set by voice → `GET /announcements` returned "Your one minute timer is done." + a valid MP3; a second poll returned [] (at-most-once); a poll within 5 s got 429. Reminder "check the oven in two minutes" → "Reminder: check the oven." announced on time. Shopping list add/read/remove, weather (Westview + Ravens), news, web search (Moby Dick), calendar ("not connected", said plainly), driveway camera described in words. Refusals: Amazon order refused (offered to add it to the list instead); "clear the list" → one item at a time only; "Kev is at Ravens" → store unchanged, fixed sentence "I can't update Kev's location from this speaker." (the first live run claimed "I'll use that", which is what the persona fixes above address). No label/list text in any log.
- [x] Tests green + adversarial review per the gates; merged to main in both repos; brain + node backend redeployed.
- [x] No irreversible tool added (bulk list clear deliberately not built pending a server-enforced confirmation design).
- [ ] HUMAN GATE (Kevin): real-board PTT for each tool; publish firmware 1.0.2 (`vesper-node firmware publish <fresh build from main>`) so the board OTAs and can *speak* timers/reminders (the board needs to be claimed first, see task 13).
- Notes for Kevin: reminders/weather use Kev's location, or Westview if that's unknown, for the time zone (a per-node property map would fix a Ravens node); delivery is at-most-once and up to ~15 s late (poll interval); unlock-by-voice on the shared speaker is unchanged and still allowed.
