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
