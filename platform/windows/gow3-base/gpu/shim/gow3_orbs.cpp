// SPDX-License-Identifier: GPL-2.0-or-later
#include "gow3_orbs.h"
#include "gow3_settings.h"
#include <cstring>
#include <cstdint>
#include "../../src/cheat_hook.h"

extern "C" float __attribute__((sysv_abi)) gow3_orbs_gain(void* player, float requested) {
    (void)player;
    return requested; // TLG: no game-specific actor reads or orb modification.
}

extern "C" uintptr_t gow3_cheat_flag(unsigned id) {
    (void)id;return 0; // No writable cheat flag is exposed to a loader.
}

extern "C" void gow3_cheat_status(unsigned id) {
    (void)id; // Game-specific hooks remain unavailable.
}

extern "C" void gow3_orbs_status(int supported) {
    (void)supported;Gow3Settings::Get().red_orbs_supported = false;
}

/// Damage (src/damage_hook.h): the multiplier the player's (taken) or another actor's hits use.
extern "C" uintptr_t gow3_damage_multiplier(int taken) {
    (void)taken;return 0;
}

/// Orb pickup (src/orb_hook.h): 0 green, 1 blue, 2 gold.
extern "C" uintptr_t gow3_orb_pickup_multiplier(unsigned kind) {
    (void)kind;return 0;
}

extern "C" void gow3_stat_multipliers_status(int damage, int orb_pickup) {
    (void)damage;Gow3Settings::Get().damage_supported = false;
    (void)orb_pickup;Gow3Settings::Get().orb_pickup_supported = false;
}
