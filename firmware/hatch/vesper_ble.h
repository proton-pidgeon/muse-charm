/*
 * The node's BLE host (task 11). The task-09 build deleted Home Link's BLE
 * server (ble_server.c, with the Muse app's pairing) and so had no BLE host
 * at all. This brings back only what a Vesper node needs:
 *
 *   - Muse's phone-setup service, muse_ble.c UNCHANGED (wifi.ssid / wifi.pass
 *     / wifi.connect / hatch.* commands, the STATUS JSON; passkey pairing with
 *     the code on screen, encrypted + MITM-protected), and
 *   - the Vesper claim service: one READ/NOTIFY characteristic with the claim
 *     state as JSON, {"state":"pending","code":"K7M2-QX9P"},
 *     {"state":"starting"} or {"state":"claimed","room":"kitchen"}. It needs
 *     no pairing: the code is not a secret (it is on the screen, and is
 *     useless without the claim secret that never leaves the node's RAM). It
 *     never carries the claim secret or the credential.
 *
 * No Meta pairing, Muse-app setup protocol or Meta endpoint is involved.
 *
 * It advertises (as identity_ble_name(), "MuseGadget-XXXXXX") while a claim
 * is in progress or while Muse's BLE setting is on, and not otherwise. The
 * host starts on first need, from Home Link's keeper task (an internal-RAM
 * stack after Wi-Fi is up, as the stock server started), and then stays up.
 *
 * Part of muse-charm (Vesper node firmware). Vesper-owned; the host start and
 * the advertising follow the stock main/ble_server.c (Meta Platforms,
 * Apache-2.0), with Link's own pairing service left out.
 *
 *   claim service         76657370-6572-4e6f-6465-000000000001
 *   claim characteristic  76657370-6572-4e6f-6465-000000000002
 */
#pragma once

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Muse's BLE setting changed (app_ble_companion_set, on the keeper task):
 * starts the host if it or a claim needs it, and updates advertising. name is
 * the advertised name (identity_ble_name()), used when the host starts.
 */
void vesper_ble_apply(const char *name, bool setup_on);

/*
 * The claim state for the claim characteristic, from any task. state is
 * vc_state_name(); code (pending) or room (claimed) may be NULL. advertise:
 * a claim is in progress, so advertise even with Muse's BLE setting off.
 */
void vesper_ble_set_claim(const char *state, const char *code, const char *room, bool advertise);

/* The host has started (muse_link ops .ble_started). */
bool vesper_ble_started(void);

#ifdef __cplusplus
}
#endif
