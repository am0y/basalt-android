#pragma once
#include <stdint.h>

/* Startup only; verifies the pinned ELF and rolls back a failed installation. */
int runtime_protection_init(void);
uint32_t runtime_protection_state(void);
/* Diagnostic implementation is present only in development builds. */
int runtime_protection_probe(void);
