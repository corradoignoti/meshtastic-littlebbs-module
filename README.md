# meshtastic-littlebbs-module

A Meshtastic firmware module that provides a simple BBS (Bulletin Board System) over LoRa mesh. Users can send direct text messages to any node running this module to access commands, including a `/meteo` weather forecast feature based on the node's GPS coordinates.

*** This is an experimental module and in it's in a very early stage of developmnet. Use it at your own risk. ***

## Features

- Main menu via DM interaction
- `/meteo` — weather forecast (uses reverse geocoding + Open-Meteo API)
- Rate limiting: one message per minute per sender
- ESP32 only (requires WiFi for HTTP/weather features)

## Usage

This repository is intended to be used as a **git submodule** inside the [Meshtastic firmware](https://github.com/meshtastic/firmware) source tree, mounted at `src/modules/LittleBBS/`.

### Adding to firmware

```bash
git submodule add https://github.com/corradoignoti/meshtastic-littlebbs-module src/modules/LittleBBS
```

### Enabling the module

In your variant's `variant.h`, add:

```cpp
#undef MESHTASTIC_EXCLUDE_LITTLEBBS
```

Make sure `platformio.ini` has the default-exclude flag:

```ini
-DMESHTASTIC_EXCLUDE_LITTLEBBS=1
```

## License

GPL-3.0 — same as the Meshtastic firmware project.
