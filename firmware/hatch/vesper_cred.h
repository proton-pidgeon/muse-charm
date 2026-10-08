/*
 * The node credential (task 11): kept in NVS namespace "muse" under the new
 * key "node_cred", next to the stock keys (which are left as they are), and
 * in RAM for the turns.
 *
 * Part of muse-charm (Vesper node firmware). Vesper-owned, written for this
 * project; not derived from the Meta muse-gadget-sdk sources.
 *
 * NVS is plaintext on this board (see firmware/README.md, "NVS encryption:
 * decision"). The value is never logged: only whether one is stored.
 *
 * Flash may only be touched from a task with an internal-RAM stack (the
 * hatch task's stack is in PSRAM), so writes and erases run on a short-lived
 * internal-stack helper task, and the caller waits for it.
 */
#pragma once

#include <stdbool.h>

#include "vesper_claim.h"

#ifdef __cplusplus
extern "C" {
#endif

#define VESPER_CRED_NS "muse"
#define VESPER_CRED_KEY "node_cred"

/* Reads the stored credential into RAM. Call once at boot, from an internal-stack task, after nvs_flash_init. */
void vesper_cred_load(void);
/* A well-formed credential is held. */
bool vesper_cred_present(void);
/* Copies it into out; false (out empty) if there is none. Wipe out after use. */
bool vesper_cred_get(char out[VC_CRED_MAX + 1]);
/* Holds it in RAM and writes it to NVS. False if it isn't well-formed or NVS failed (RAM still has it). */
bool vesper_cred_store(const char *credential);
/* Wipes it from RAM and erases the NVS key. False if NVS failed. A forget always wins over a store it overlaps. */
bool vesper_cred_forget(void);
/* Counts forgets (bumped as each forget is requested). A caller that read it before a store, and
 * sees it changed after, must treat the credential as forgotten. See the interleavings in vesper_cred.c. */
unsigned vesper_cred_generation(void);

#ifdef __cplusplus
}
#endif
