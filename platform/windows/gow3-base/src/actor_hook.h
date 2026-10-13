/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Inert overlay ABI only. No game hook implementation is imported. */
#pragma once
#include <stdint.h>
typedef struct {
    uint64_t actor;
    float health, max_health;
    uint32_t seq, pad;
    uint64_t reserved;
} Gow3ActorSlot;
