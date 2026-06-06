#pragma once
#include <stdint.h>

/*
 * Real radio access for FlipperPet. Implemented against the unified driver
 * stack shared by current official/stable firmware and Momentum dev:
 *   - Sub-GHz : furi_hal_subghz RSSI
 *   - NFC     : nfc/nfc_scanner.h (NfcScanner)
 *   - RFID    : lfrfid worker auto-read
 */

/* Returns peak RSSI in dBm (negative; closer to 0 == stronger signal). */
int flipperpet_subghz_sense(uint32_t frequency_hz);

/* 1 = NFC tag detected, 0 = nothing found. */
int flipperpet_nfc_detect(void);

/* 1 = LF RFID tag/field interaction, 0 = nothing found. */
int flipperpet_rfid_detect(void);
