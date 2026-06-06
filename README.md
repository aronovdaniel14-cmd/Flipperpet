# FlipperPet v2.3

A Tamagotchi-style companion for the Flipper Zero: pet creation, real
animations, an adore screen, and real radio interaction (Sub-GHz, NFC, RFID).

## Files

- `application.fam` — build manifest (icon + `fap_libs=["subghz"]`)
- `flipperpet.c` — the app
- `flipperpet_radio.c` / `.h` — real radio access, portable across SDKs
- `dog.png` — 10x10 cute dog-face icon

## Cross-firmware notes

- One `.fap` per firmware. External apps are validated by an API hash at load,
  so you build separately for official and Momentum with that firmware's ufbt.
- **Sub-GHz** now uses the `subghz_devices` abstraction (`lib/subghz/devices`),
  the current radio API on official + Momentum. The old
  `furi_hal_subghz_load_preset()` was removed when external-radio-board support
  landed — that was the v2.2 build error. This needs `fap_libs=["subghz"]`
  (already in `application.fam`) so the device layer links.
- **NFC / RFID** are gated by `__has_include`: full detection where the unified
  NFC scanner / lfrfid worker exist (current official + Momentum), graceful
  "unavailable" otherwise. The build never breaks on a missing subsystem.

## Build

Official / stable:

```bash
python3 -m pip install --upgrade ufbt
ufbt update
cd flipperpet
ufbt launch
```

Momentum dev:

```bash
ufbt update --index-url https://up.momentum-fw.dev/firmware/directory.json
cd flipperpet
ufbt launch
```

## Features

Pet creation (Pup/Kit/Byte + gender) · real animations (bone-chomp feed, a hand
that pats the head with hearts, play bounce, RF rings) · Adore screen · real
radios (Sub-GHz RSSI 433/315/868, NFC tag detect, LF-RFID auto-read) · 13 menus
with 8x8 icons · save to `/ext/apps_data/flipperpet/save.dat`.

Controls: Up/Down/Left/Right move, OK selects/pets, Back exits; long-press Back
on Settings ▸ Reset re-runs the intro.

## If the build still complains

- **Undefined reference to `subghz_devices_*`** -> the subghz lib didn't link;
  confirm `fap_libs=["subghz"]` is present (it is) and `ufbt update` matches your
  firmware.
- **A renamed symbol in NFC/RFID** -> send me the firmware build number and I'll
  match it (including a legacy `furi_hal_nfc` path for pre-unified official).

I can't compile here (no SDK in my environment), so I'm targeting the current
documented APIs; `ufbt` will name any remaining drift line-by-line.
