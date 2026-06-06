/*
 * Real radio access for FlipperPet.
 *
 * Sub-GHz uses the subghz_devices abstraction (lib/subghz/devices), which is the
 * current radio API on official/stable and Momentum after external-radio-board
 * support landed. The old furi_hal_subghz_load_preset() was removed in that
 * change, which is what broke the previous build.
 *
 * NFC and RFID are detected at compile time with __has_include so the same
 * source compiles on every SDK; present -> full detection, absent -> graceful -1.
 *
 * Requires `fap_libs=["subghz"]` in application.fam so subghz_devices links.
 */

#include "flipperpet_radio.h"
#include <furi.h>
#include <furi_hal.h>

#if defined(__has_include)
#if __has_include(<lib/subghz/devices/devices.h>)
#include <lib/subghz/devices/devices.h>
#define FP_HAS_SUBGHZ 1
#endif
#if __has_include(<nfc/nfc_scanner.h>)
#include <nfc/nfc.h>
#include <nfc/nfc_scanner.h>
#define FP_HAS_NFC 1
#endif
#if __has_include(<lfrfid/lfrfid_worker.h>) && __has_include(<lfrfid/protocols/lfrfid_protocols.h>)
#include <lfrfid/lfrfid_worker.h>
#include <lfrfid/protocols/lfrfid_protocols.h>
#include <toolbox/protocols/protocol_dict.h>
#define FP_HAS_LFRFID 1
#endif
#endif

#define DETECT_FLAG (1u << 0)

#if defined(FP_HAS_NFC) || defined(FP_HAS_LFRFID)
typedef struct {
    FuriEventFlag* flag;
    volatile bool detected;
} DetectCtx;
#endif

/* ---- Sub-GHz: real RSSI via the device abstraction ---- */

int flipperpet_subghz_sense(uint32_t freq) {
#ifdef FP_HAS_SUBGHZ
    subghz_devices_init();
    const SubGhzDevice* device = subghz_devices_get_by_name("cc1101_int");
    if(!device) {
        subghz_devices_deinit();
        return -110;
    }

    subghz_devices_begin(device);
    subghz_devices_reset(device);
    subghz_devices_idle(device);
    subghz_devices_load_preset(device, FuriHalSubGhzPresetOok650Async, NULL);
    if(!subghz_devices_is_frequency_valid(device, freq)) freq = 433920000;
    subghz_devices_set_frequency(device, freq);
    subghz_devices_flush_rx(device);
    subghz_devices_set_rx(device);

    int best = -127;
    for(int i = 0; i < 6; i++) {
        furi_delay_ms(3);
        int r = (int)subghz_devices_get_rssi(device);
        if(r > best) best = r;
    }

    subghz_devices_idle(device);
    subghz_devices_sleep(device);
    subghz_devices_end(device);
    subghz_devices_deinit();
    return best;
#else
    (void)freq;
    return -110;
#endif
}

/* ---- NFC: presence detection via NfcScanner ---- */

#ifdef FP_HAS_NFC
static void nfc_scan_callback(NfcScannerEvent event, void* context) {
    DetectCtx* ctx = context;
    if(event.type == NfcScannerEventTypeDetected) {
        ctx->detected = true;
        furi_event_flag_set(ctx->flag, DETECT_FLAG);
    }
}
#endif

int flipperpet_nfc_detect(void) {
#ifdef FP_HAS_NFC
    Nfc* nfc = nfc_alloc();
    NfcScanner* scanner = nfc_scanner_alloc(nfc);
    DetectCtx ctx = {.flag = furi_event_flag_alloc(), .detected = false};

    nfc_scanner_start(scanner, nfc_scan_callback, &ctx);
    furi_event_flag_wait(ctx.flag, DETECT_FLAG, FuriFlagWaitAny, 700);
    nfc_scanner_stop(scanner);

    nfc_scanner_free(scanner);
    nfc_free(nfc);
    furi_event_flag_free(ctx.flag);
    return ctx.detected ? 1 : 0;
#else
    return -1;
#endif
}

/* ---- LF RFID: auto-read via lfrfid worker ---- */

#ifdef FP_HAS_LFRFID
static void rfid_read_callback(LFRFIDWorkerReadResult result, ProtocolId protocol, void* context) {
    UNUSED(protocol);
    DetectCtx* ctx = context;
    if(result == LFRFIDWorkerReadDone || result == LFRFIDWorkerReadSenseCardStart) {
        ctx->detected = true;
        furi_event_flag_set(ctx->flag, DETECT_FLAG);
    }
}
#endif

int flipperpet_rfid_detect(void) {
#ifdef FP_HAS_LFRFID
    ProtocolDict* dict = protocol_dict_alloc(lfrfid_protocols, LFRFIDProtocolMax);
    LFRFIDWorker* worker = lfrfid_worker_alloc(dict);
    DetectCtx ctx = {.flag = furi_event_flag_alloc(), .detected = false};

    lfrfid_worker_start_thread(worker);
    lfrfid_worker_read_start(worker, LFRFIDWorkerReadTypeAuto, rfid_read_callback, &ctx);
    furi_event_flag_wait(ctx.flag, DETECT_FLAG, FuriFlagWaitAny, 1200);
    lfrfid_worker_stop(worker);
    lfrfid_worker_stop_thread(worker);

    lfrfid_worker_free(worker);
    protocol_dict_free(dict);
    furi_event_flag_free(ctx.flag);
    return ctx.detected ? 1 : 0;
#else
    return -1;
#endif
}
