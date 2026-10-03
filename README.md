# muse-charm

DIY Meta Muse Charm — a desktop companion with avatar display, microphone,
and speaker, built on Meta's open-source
[muse-gadget-sdk](https://github.com/facebookincubator/muse-gadget-sdk)
(ESP32 firmware + Linux SDK, Apache 2.0, released 2026-10-02).

Build log: we start by getting the **UI simulator** running, so the Charm
interface (avatar, states, captions) can be tried before buying hardware.

- `docs/simulator-setup.md` — the setup guide: prereqs, build, test,
  interactive and headless runs.
- `tasks/` — work items for Claude Code (`/implement`).
- `HANDOVER.md` — session state.
