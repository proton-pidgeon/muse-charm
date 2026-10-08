# Task 15: Claim-code display legibility (F3 follow-up)

**Goal:** Make the claim code READABLE on the AiPi Lite's 128×128 display. Right now it is not.
**Source:** Kevin hit this live 2026-10-08 ~14:55 CDT — the board showed `CLAIM CODE EZ..` (truncated, no scroll) during the claim flow, making the 8-char code (XXXX-XXXX) unreadable and the claim UX effectively bricked. Vesper approved the claim via the backend registry as a workaround; the display bug remains.
**Depends on:** 11 (claim flow), 13 (OTA delivery if usable)
**Relevant code:** `firmware/hatch/vesper_claim.c` (`vc_caption`, line ~398: `snprintf(out, cap, "CLAIM CODE %s", c->code)`), the caption renderer that truncates it (find where captions are drawn — there is currently NO scroll mechanism in firmware; grep found none).

## The problem
- `vc_caption` formats `"CLAIM CODE %s"` = 20 chars (`CLAIM CODE XXXX-XXXX`).
- The 128×128 caption area fits ~14 chars; the string is truncated to `CLAIM CODE EZ..` with no scrolling.
- The claim code is the ONLY thing standing between the user and a working node — if they can't read it, they can't claim.

## Fix (implementer's call, but the full XXXX-XXXX MUST be legible)
Pick the approach that fits the firmware's display architecture best:
- **Option A — scroll/marquee:** long captions scroll horizontally (or vertically) so the full text becomes readable. Most general fix — also helps other long captions (`CHECK THE SERVER URL`, etc.).
- **Option B — reformat the claim caption:** show just `XXXX-XXXX` (9 chars, fits) possibly with a static `CLAIM CODE` label on a second line or as a smaller header. Simplest, claim-specific.
- Either is acceptable; a combination is fine. Do NOT shrink the font to illegibility to "fit" — the code must be readable at arm's length.

## Deliverables
- [ ] Firmware fix making the full claim code legible on the 128×128 display (scroll and/or reformat)
- [ ] Host tests covering the new behavior (extend `firmware/hatch/test/test_vesper_claim.c` and/or caption-renderer tests — follow the existing test pattern)
- [ ] Desktop/simulator verification: render the claim caption, confirm the full XXXX-XXXX is legible (screenshot/GIF as proof)
- [ ] Deliver to the REAL AiPi Lite: use the OTA pipeline from task 13 if it supports pushing this update; otherwise USB flash. (Note: the board is currently CLAIMED and working — coordinate so the update doesn't brick its credential; if the claim state must be re-triggered for verification, re-claim afterward.)
- [ ] On-device verification: the real display shows a fully legible claim code (device snap as proof)

## Definition of done
- [ ] Green build + adversarial review per the /implement gates; merged to main
- [ ] Firmware delivered to the real board (OTA preferred, USB acceptable)
- [ ] Device snap proves the full XXXX-XXXX claim code is legible on the 128×128 display
- [ ] Board still claimed and working after the update (PTT → reply, or re-claimed cleanly)

## Anti-deliverables (do NOT build in this task)
- Wake-word / trigger-word activation — that's a separate track (design only for now)
- Changes to the claim protocol itself (code format, expiry, backend) — display only
