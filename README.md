# Offgrid Tracker

Offgrid Tracker is a local-first Android companion and embedded firmware for an
offline Wi-Fi HaLow mesh. It provisions ESP32 and nRF54L15 devices over BLE,
exchanges private user messages and clear-text channel messages over
BATMAN-adv-lite, discovers peers from mesh beacons, and stores profiles,
devices, and conversations locally.

[![Use this template](https://img.shields.io/badge/Use%20this-template-238636?style=for-the-badge&logo=github)](https://github.com/new?template_name=template-offgrid-tracker&template_owner=edgez-ai)
[![Deploy on EdgeZ](https://img.shields.io/badge/Deploy%20on-EdgeZ-6c5ce7?style=for-the-badge)](https://appwrite.edgez.ai/console/deploy?repo=https%3A%2F%2Fgithub.com%2Fedgez-ai%2Ftemplate-offgrid-tracker)

Offline operation remains the default and does not require Appwrite, MQTT, an
Internet connection, or an upstream Wi-Fi network. A profile can optionally
enable cloud connectivity. Cloud status is online only while that profile is
enabled, the user has an authenticated session, and the phone has verified
Internet reachability.

When cloud connectivity is enabled but the app has no authenticated session,
the status badge reads `LOGIN` and opens the EdgeZ Cloud sign-in dialog. Mobile
release builds obtain the public endpoint and project ID from the
`APPWRITE_PUBLIC_ENDPOINT` and `APPWRITE_PROJECT_ID` repository variables.
After authentication, the app creates or reuses a Team whose ID matches the
local profile, writes the profile row with team-scoped permissions, and saves
the selected profile in Appwrite account preferences. The badge changes to
`ONLINE` only after this setup succeeds and Internet reachability is verified.

## Current features

- Profile-specific HaLow country, bandwidth/channel, mesh ID, and passphrase,
  with shared phone identity, BLE selection, location mode, icon, and color.
- SQLite-backed profiles, devices, peer MAC addresses/public keys, messages,
  sensor history, retry state, and last-seen time.
- Optional profile-level EdgeZ cloud connectivity with explicit online/offline
  status on the map and in Settings.
- Offline map and list views with day/night and current-location controls.
- `Channels`, `People`, and `Sensors` message tabs. Channels are available
  before the first message; discovered users and sensors share the persisted
  device list.
- BLE provisioning for ESP32 and nRF54L15, followed by an automatic realtime
  BLE session for status, mesh traffic, and profile changes.
- Unified beacon and messaging protocol over BLE, PAwR, 802.11s, and
  BATMAN-adv-lite. Undelivered messages are retried up to three times while a
  peer is online and can be retried manually afterward.
- Profile-aware mesh reinitialization. Switching profiles clears the old peer
  and BATMAN routing state before joining the new mesh.
- Low-power nRF54L15 sensor mode for the EdgeZ FGH200M/MM8108 board.

## Repository layout

| Path | Purpose |
| --- | --- |
| `app/` | Expo/React Native Android app and local SQLite data model |
| `site/` | Optional authenticated Next.js cloud portal |
| `function/` | Appwrite MQTT ingestion and telemetry processing Function |
| `infra/` | Rerunnable Appwrite CLI installer used by Deploy on EdgeZ |
| `firmware/` | PlatformIO ESP-IDF and Zephyr firmware |
| `firmware/platforms/esp-halow/` | Pinned ESP32 HaLow platform submodule |
| `firmware/platforms/mm-iot-zephyr/` | Pinned nRF54L15 HaLow platform submodule |
| `docs/` | Product screenshots and hardware images |
| `.github/workflows/` | Android and supported firmware CI builds |

See [app/README.md](app/README.md) and
[firmware/README.md](firmware/README.md) for component-specific details.

## Optional EdgeZ cloud deployment

The root `edgez.json` selects `appwrite.config.json` as the version-controlled
solution plan. **Deploy on EdgeZ** provisions email/password and JWT auth,
web/Android platforms, the profile and telemetry database, Time Series storage,
the MQTT ingestion Function, the Next.js Site, resource variables, and proxy
domains. The deployment is optional; the mobile app and mesh continue to work
locally when the cloud is disabled or unreachable.

For a local infrastructure check:

```sh
cd infra
npm ci
npm run check
```

Managed deployments inject `APP_NAME`, `DOMAIN_SUFFIX`, `APPWRITE_ENDPOINT`,
`APPWRITE_PROJECT_ID`, `APPWRITE_PROJECT_NAME`, and `APPWRITE_API_KEY`. Run
`npm run deploy` only when you intend to reconcile a remote Appwrite project;
`npm run clean` removes the template Site, Function, tables, Time Series store,
proxy rules, and auth platforms while preserving project users and Devices.

For a local mobile build connected to that deployment, copy `app/.env.example`
to `app/.env` and set its public `APPWRITE_PROJECT_ID`. Never put the Appwrite
API key in the mobile environment.

## Clone and initialize

The two HaLow platform directories are Git submodules, and each has its own
nested Morse Micro and EdgeZ dependencies. Always initialize recursively:

```sh
git clone --recurse-submodules <repository-url>
cd template-offgrid-tracker
git submodule update --init --recursive
git submodule status --recursive
```

The repository currently pins the top-level platform submodules to:

| Submodule | Revision |
| --- | --- |
| `firmware/platforms/esp-halow` | `576ef9bdc04aa2c44a27a82af556fe16a5497a3c` |
| `firmware/platforms/mm-iot-zephyr` | `8694828e926487ddda5da5967e3c830dafac6be6` |

Do not independently update nested Morse SDK, firmware, RF API, or EdgeZ
prebuilt revisions. Update the owning platform submodule and then commit its new
top-level gitlink so the tested dependency set remains reproducible.

## Android app

Requirements:

- Node.js and npm
- Android SDK, Java 17, and an attached Android device
- Bluetooth and location permissions on the phone

```sh
cd app
npm ci
npm run typecheck
npm run android
```

`npm run android` uses the standard `expo run:android` flow. This app contains
native BLE, SQLite, location, and Organic Maps integrations and therefore does
not run in Expo Go. When multiple phones are connected, select one with
`ANDROID_SERIAL`.

On first launch, create a profile. Country defaults to `US`, the channel
defaults to `27`, the mesh ID defaults to the profile name, and the passphrase
may be empty or 8–63 characters. Location mode can be `Off`, `Fixed`, or
`Live`. Live mode uses the phone's current location in periodic self beacons;
a beacon is still sent when location is temporarily unavailable.

The app scans ESP provisioning advertisements by the `EZ_` prefix. Both
platforms use a 12-digit HaLow MAC suffix so the displayed identity stays stable
before and after provisioning; nRF54 provisioning advertisements use the
`NRF_` prefix.

## Firmware

Install PlatformIO, initialize all submodules, and build from `firmware/`:

```sh
cd firmware
pio run -e heltec-hc33
pio run -e edgez-nrf54l15-fgh200m-companion
pio run -e edgez-nrf54l15-fgh200m-sensor
pio run -e edgez-nrf54l15-vc0706-camera-test
```

Supported development environments are:

| Environment | Hardware | HaLow radio | Upload |
| --- | --- | --- | --- |
| `waveshare-esp32-p4` | Waveshare ESP32-P4 board | MM8108 over USB | esptool |
| `esp32-s3-pawr` | Generic ESP32-S3 test board | MM6108 over SPI | esptool |
| `heltec-hc33` | Heltec HT-HC33 | MM6108 over SPI | esptool |
| `heltec-hc33-mesh-ps` | HT-HC33 mesh power experiment | MM6108 over SPI | esptool |
| `seeed-xiao-nrf54l15-hc01` | XIAO nRF54L15 + HT-HC01 | MM6108 over SPI | J-Link |
| `seeed-xiao-nrf54l15-sense` | XIAO nRF54L15 Sense + FGH100M | MM6108 over SPI | pyOCD |
| `edgez-nrf54l15-fgh200m-companion` | FGH200M BLE companion and full mesh node | MM8108 over SPI | J-Link |
| `edgez-nrf54l15-fgh200m-sensor` | FGH200M low-power connectionless sensor; no BLE | MM8108 over SPI | OpenOCD/CMSIS-DAP |
| `edgez-nrf54l15-vc0706-camera-test` | nRF54L15 + TTL camera; BLE-triggered JPEG with selectable resolution to Python, UART 921600 | Disabled | OpenOCD/CMSIS-DAP |

Release-style builds use the `edgez-halow-mesh` public headers and protected
prebuilt libraries already pinned inside the platform submodules. To diagnose
the mesh implementation, point both ESP32 and nRF54 builds at a source checkout:

```sh
export EDGEZ_HALOW_MESH_SOURCE_ROOT=/absolute/path/to/edgez-halow-mesh
pio run -e heltec-hc33
pio run -e edgez-nrf54l15-fgh200m-companion
pio run -e edgez-nrf54l15-fgh200m-sensor
```

Unset the variable to verify the same application against the pinned prebuilts:

```sh
unset EDGEZ_HALOW_MESH_SOURCE_ROOT
pio run -e heltec-hc33
pio run -e edgez-nrf54l15-fgh200m-companion
```

### FGH200M low-power sensor image

`edgez-nrf54l15-fgh200m-sensor` is the dedicated low-power target. Bluetooth is
removed at build time, and the image never enters companion mesh mode. With a
stored sensor profile, it:

1. Stops BLE and does not start mesh peering, beacons, or BATMAN routing.
2. Keeps the LSM6DSL gyroscope off and lets its accelerometer FIFO batch samples
   while the MCU uses Zephyr tickless System ON idle.
3. Wakes on a 15-second reporting cycle and drains the FIFO.
4. Keeps the L76K GPS disabled; the sensor report is valid without a location.
5. Wakes the initialized MM8108 from chip sleep, sends one connectionless HaLow
   PAwR-like beacon, and waits up to 500 ms for an application acknowledgement.
6. Returns the MM8108 to chip sleep without cutting its rail or reloading its
   firmware, then lets the nRF54L15 return to tickless idle.

Watch the [narrated low-power sensor demonstration](docs/halow-pawr-low-power-sensor.md)
for the mobile sensor view and measured wake/transmit waveform.

The power image disables INFO logs, printk, the console, and RTT to avoid
measurement overhead. Garbage from an RTT viewer is therefore expected and is
not a runtime diagnostic. Use `edgez-nrf54l15-fgh200m-companion` or a sensor
source build with logging enabled when logs are required, and use a current
meter to validate the sensor image.

Build the image with:

```sh
cd firmware
pio run -e edgez-nrf54l15-fgh200m-sensor
```

Flash the generated HEX through the CMSIS-DAP probe with OpenOCD:

```sh
cd firmware
pio run -e edgez-nrf54l15-fgh200m-sensor -t upload
```

The sensor, sensor-debug, and sensor-bulk-test environments use OpenOCD over
CMSIS-DAP for both upload and debugging. PlatformIO programs
`.pio/build/edgez-nrf54l15-fgh200m-sensor/firmware.hex`, resets the target, and
starts the image. To select one probe when several are connected, pass its
CMSIS-DAP serial as the upload port:

```sh
pio run -e edgez-nrf54l15-fgh200m-sensor -t upload \
  --upload-port <CMSIS_DAP_SERIAL>
```

The companion and factory-partition provisioning flows continue to use J-Link.

### nRF54 factory identity

Each FGH200M needs an immutable serial number and X25519 keypair in its
dedicated factory partition before zero-touch HaLow provisioning. Generate and
program it independently of the application image:

```sh
firmware/scripts/provision_nrf54_factory.sh \
  --serial FGH200M-000001 \
  --jlink-serial <JLINK_SN> \
  --country US --channel 27 \
  --output-dir .factory/FGH200M-000001
```

Pass `--private-key existing-x25519-private.pem` to import an existing key, or
omit it to generate a new one. Use `--build-only` to create and inspect the
image without touching hardware. The script writes and verifies only the 4 KiB
factory partition at `0x165000`; normal profile reset and application flashing
do not modify it. Keep `identity-private.pem` secret and backed up. Avoid a
whole-chip erase after manufacturing because that also removes factory data.
Country and channel always come from this factory record for discovery. Omit
mesh ID/passphrase, as above, to require discovery; provide both only when the
sensor should join that factory mesh immediately and skip discovery.
The factory write/verify sequence is committed as
`firmware/scripts/jlink/factory_partition.jlink.in`; the shell script fills in
the generated image path in a temporary copy.

Firmware reads and validates this record once during boot. Provisioning packets
always use the cached factory public key. If the sensor has no valid factory
keypair, it leaves the MM8108 off and sleeps instead of advertising an
undecryptable temporary identity.

### Factory reset

On supported nRF54 carriers, hold the user button for five seconds and release
it to erase the stored profile and return to BLE provisioning. ESP32 profiles
use their board-specific factory-reset button behavior described in the
firmware README.

## CI builds

- `mobile.yml` builds the React Native bundle and can optionally create signed
  Android APKs when the organization signing secrets are configured.
- `firmware.yml` builds only the supported release targets `heltec-hc33` and
  both FGH200M roles, packages licensing/source manifests, and uploads
  short-lived Actions artifacts.

## Licensing and firmware distribution

The root [Apache-2.0 license](LICENSE) covers only the original Offgrid Tracker
code. It does **not** relicense the firmware submodules, static libraries, Morse
radio firmware, or board-configuration blobs.

| Component | License boundary |
| --- | --- |
| Offgrid Tracker application and original firmware code | Apache-2.0 |
| Morse Micro `mm-iot-sdk/framework/morselib` | `GPL-3.0-or-later OR LicenseRef-MorseMicroCommercial` |
| EdgeZ `edgez-halow-mesh` source, public headers, and prebuilt archives | EdgeZ Non-Commercial and Commercial Software License |
| Morse Micro radio firmware and BCF blobs | Morse Micro Binary Distribution License |

The GPL option for `morselib` carries GPL obligations for a distributed
combined firmware binary, including the applicable notices and complete
corresponding source. A distributor that does not use the GPL path must obtain
and comply with the applicable Morse Micro commercial license.

`edgez-halow-mesh` is not covered by this repository's Apache-2.0 license. Its
bundled EdgeZ license permits the stated non-commercial and internal-evaluation
uses; production or other commercial use requires a separate written commercial
license from EdgeZeron AI AB. Permitted redistribution must retain the EdgeZ
license, attribution, and third-party notices.

Building in GitHub Actions or downloading an Actions artifact does not grant
either commercial license. Do not publish or distribute firmware binaries until
the chosen Morse Micro and EdgeZ licensing paths, notice requirements, and
source obligations have been satisfied. See
[FIRMWARE-LICENSING.md](FIRMWARE-LICENSING.md) and the license files shipped
beside the pinned dependencies for the complete terms.

## Validation

```sh
cd app && npm run typecheck
cd ../firmware && pio run -e heltec-hc33
pio run -e edgez-nrf54l15-fgh200m-companion
pio run -e edgez-nrf54l15-fgh200m-sensor
```

For hardware verification, confirm BLE provisioning/realtime status on Android,
peer discovery and messaging between two devices, PAwR reception for a sensor,
and the expected low-current interval between FGH200M reports.
