# Pocket Signer — firmware

Firmware for the Nostr Onchain hardware pocket signer
(Waveshare ESP32-S3 Touch AMOLED 1.8).

- `poc_usbnet/` — main firmware: USB-C network device (`http://10.77.7.1`),
  NIP-07-shaped signing API with on-screen tap-to-approve, real BIP-340
  Schnorr signatures on-device, taproot PSBT review + signing, NIP-46 bridge
  page, SD-card key import and air-gapped PSBT signing.
- `mini_screen/` — earlier display/audio bring-up sketch.

## Security model

- Private keys never leave the device over the network. Export is SD-card only.
- Every Nostr event and every Bitcoin transaction is rendered on-screen and
  requires a physical tap to sign (relay auth kind 22242 may auto-sign if enabled).
- The HTTP API rejects requests from arbitrary web origins; only the device's
  own pages and the Signer Link browser extension may call it.

## Build

Arduino CLI, ESP32 core, FQBN:
`esp32:esp32:esp32s3:USBMode=default,CDCOnBoot=cdc,PartitionScheme=app3M_fat9M_16MB,PSRAM=opi,FlashSize=16M`

Never commit `nostr-keys.json` / `nostr-keys.txt` (SD-card key import files).
