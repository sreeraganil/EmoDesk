# EmoDesk

An Emo-style desktop companion for the **ESP32-C3 Super Mini** with a **128x64 SSD1306 OLED** and a **TTP223 touch pad**. A **MAX98357A** speaker amp can be added later. Everything is set up from a web dashboard on your phone or computer: cards, Emo's personality, sounds, alarms, gestures, services, pictures and updates.

- 68 card types. You can add, copy, reorder, schedule and limit any of them to profiles. Each copy has its own settings.
- Emo has 15 expressions and moods that drift over time. It gets bored, sleepy and hungry, and it naps and goes to bed at night. You can poke it, pet it and feed it, and it reacts to the weather.
- Live data: weather, forecast, air quality, live cricket and football, league tables, crypto, stocks, currency, news, quotes, jokes, facts, the ISS, YouTube and GitHub.
- Productivity: Pomodoro, timer, stopwatch, alarms with snooze, reminders, to-dos, habits, water, notes, calendar events and a morning briefing.
- Home: Telegram bot, MQTT / Home Assistant, PC stats and now playing, and optional Bluetooth presence or media remote.
- Sound (after adding the amp): robot voice, emotion chirps, effects, melodies, your own clips, sleep sounds, metronome, hourly chime, quiet hours and per-event switches.
- Fun: 6 one-button games, dice, magic answers, a decision maker, screensavers, pictures, GIF animations and a pixel editor.
- Live mirror of the OLED in the dashboard, with a virtual touch pad.
- Wi-Fi setup hotspot, over-the-air updates, backup and restore, and logs.
- Battery saver, low-battery warnings, deep sleep (touch to wake) and optional night sleep.
- Self-healing: a frozen loop reboots itself, and repeated crashes start a safe mode.

See **WIRING.md** for connections.

## 1. Install the tools

You can use either the Arduino IDE or PlatformIO. The project folder works with both.

### Arduino IDE 2.x

1. **Board package.** File → Preferences → Additional boards manager URLs, add
   `https://espressif.github.io/arduino-esp32/package_esp32_index.json`.
   Then Tools → Board → Boards Manager → install **esp32 by Espressif, version 3.0 or newer** (2.x will not compile).
2. **Libraries** (Sketch → Include Library → Manage Libraries):
   - **U8g2** by olikraus
   - **ArduinoJson** by Benoit Blanchon, **version 7.x**
   - **PubSubClient** by Nick O'Leary
   - **ESP8266Audio** and **ESP8266SAM** by Earle Philhower (the robot voice). If they fail to build, set `ENABLE_TTS 0` in `config.h`. Emo will then babble instead of speaking.
3. Open `EmoDesk/EmoDesk.ino`.
4. **Tools menu:**
   - Board: **ESP32C3 Dev Module**
   - USB CDC On Boot: **Enabled**
   - Flash size: **4MB**
   - Partition scheme: **"Huge APP (3MB No OTA/1MB SPIFFS)"** (do NOT use "Custom"). The sketch's own `partitions.csv` defines the 3MB layout, and "Huge APP" ensures the IDE size check matches the slot needed for BLE builds.
5. Upload. If the port doesn't appear, hold **BOOT**, plug in USB, release, then upload.

### PlatformIO

Open the project root (the folder containing `platformio.ini`) and run **Upload**. The ini uses the *pioarduino* platform, because the stock espressif32 platform is still on core 2.x. Libraries install automatically.

## 2. First start

1. Emo wakes up and shows **Wi-Fi setup**.
2. On your phone, join the Wi-Fi network **EmoDesk-Setup**. A setup page opens; if it doesn't, browse to **192.168.4.1**.
3. Pick your network, enter the password and tap **Connect and restart**.
4. Rejoin your normal Wi-Fi and open **http://emo.local**. Android phones may need the IP address shown on the **Network** card instead.
5. In **Connect**, set your home location. Searching a city also sets the time zone. Your browser's time zone is offered automatically.

If the saved network later disappears, Emo keeps working and reopens the setup hotspot in the background while it retries.

## 3. Using it

### Touch gestures (defaults, change them in **Controls**)

| Gesture | Most cards | Emo face | Timers, to-do, music, dice… | Games |
|---|---|---|---|---|
| Tap | next card | poke (spam it and see) | next card | jump / flap / drop |
| Double-tap | previous card | next card | start / pause / select | - |
| Triple-tap | home (Emo) | three pokes | home | - |
| Hold (0.7 s) | read card aloud | pet (hearts while held) | reset / lap / next preset | hold 1.4 s to leave |
| Long hold (3 s) | mute / unmute | - | - | - |

The **BOOT** button defaults to: tap volume up, double volume down, triple next profile, hold screen off/on, long hold auto-rotate on/off.

### Dashboard tabs

- **Home:** live screen, messages to the screen, moods, pet care, quick volume and profile.
- **Cards:** add from the library, then edit, copy, reorder, switch on/off, and set profiles, days and hours.
- **Emo:** eye shape with a live preview, liveliness, boredom, naps, bedtime and weather reactions.
- **Sound:** speaker switch, volume, style, voice, quiet hours, per-event switches, test pads, clip upload (MP3 is converted in your browser) and custom RTTTL melodies.
- **Alarms:** alarms, reminders, hourly chime and morning briefing.
- **Display:** brightness, night dimming, screen-off rules, burn-in protection and the LED.
- **Controls:** what every gesture does, for both buttons, plus timing.
- **Connect:** location, time zone, units, match server, YouTube key, Telegram, MQTT, PC companion, Bluetooth and profiles.
- **Media:** picture converter, GIF to animation, pixel editor and storage.
- **System:** Wi-Fi, device name, firmware update, backup and restore, log, restart and reset.

## 4. Services

- **Cricket:** run the match server in `match_server/` (Node.js 18+, no dependencies) on any host, then paste its address, e.g. `https://your-server.onrender.com/api/cricket`, into **Connect > Cricket**. See `match_server/README.md` for Render, Railway, Vercel and VPS steps. Football cards use the same server when it is set.
- **Football:** no key needed. Uses ESPN's public scoreboard and standings feeds.
- **Weather and air quality:** Open-Meteo, no key.
- **Stocks:** Yahoo Finance chart data, no key. Use `.NS` or `.BO` suffixes for Indian exchanges.
- **Crypto:** CoinGecko. **Currency:** open.er-api.com.
- **YouTube:** a YouTube Data API v3 key from Google Cloud Console.
- **Telegram:** create a bot with @BotFather, paste the token in Connect and send the bot a message to pair. Commands: `/say text`, `/emo happy`, `/play westminster`, `/stop`, `/status`. Plain text appears on the screen.
- **MQTT:** publish to `emodesk/cmd/message`, `/say`, `/emo`, `/play`, `/stop` or `/notify`. `/notify` takes JSON such as `{"title":"Door","text":"Front door opened","emo":"surprised","sound":"sfx:bell","speak":true}`. State is published to `emodesk/state`. Add a **Home Assistant** card to show topic values and toggle switches.
- **PC companion:** `pip install psutil requests`, then `python tools/pc_companion.py --host emo.local`.
- **Bluetooth:** set `ENABLE_BLE 1` in `config.h`, rebuild, and flash over USB. Bluetooth requires the 3MB app partition in `partitions.csv` ("Huge APP" in Arduino IDE) because the NimBLE stack and controller add ~280 KB. It uses the BLE library that comes with the ESP32 core (3.3 or newer), so **do not install the separate NimBLE-Arduino library**; on core 3.3.7+ the two stacks clash. Bluetooth costs about 45 KB of RAM and starts about 12 seconds after Wi-Fi. If it ever crashes while starting, the next boot switches it off and Connect shows a **Try again** button.

### Battery and power (Power tab)

- **Battery saver:** off, automatic below a percentage, or always on. While saving, the processor runs at 80 MHz, Wi-Fi sleeps deeper, the screen runs at a lower frame rate, is dimmer and switches off sooner, online data refreshes 3x less often, Bluetooth scanning pauses and the LED goes dark. Sounds can be muted too; alarms always ring.
- **Low battery:** a warning at 15% and deep sleep at a level you choose (5% by default). Touch the pad to wake.
- **Night sleep:** deep sleep between two times. Emo wakes a couple of minutes before any alarm, and only sleeps after 5 minutes without touches or an open dashboard.
- Gestures can be set to **Battery saver on/off** or **Deep sleep** in Controls.
- The battery reading needs a 1:2 divider (two 10k resistors) from the battery to GPIO0. Without it the automatic modes stay idle.

### HTTP API (handy for scripts)

```
curl -X POST http://emo.local/api/message -H 'Content-Type: application/json' -d '{"text":"Dinner is ready!","speak":true}'
curl -X POST http://emo.local/api/emo     -H 'Content-Type: application/json' -d '{"e":"love"}'
curl -X POST http://emo.local/api/sound   -H 'Content-Type: application/json' -d '{"spec":"mel:2"}'
curl -X POST "http://emo.local/api/action?a=next"
curl http://emo.local/api/status
```

### Backup and Restore

You can back up and restore everything on EmoDesk — including server URLs, API keys & tokens, Wi-Fi credentials, all configured cards, pet data, and custom media files (pictures, animations, audio clips).

**Option 1: From the Web Dashboard (System tab)**
- **Download full backup:** One click downloads a complete backup file containing all settings, server URLs, API keys, all cards, pet care stats, and all custom uploaded media files.
- **Upload & restore:** Select your `.json` backup file. The dashboard inspects it and shows a preview of all contained URLs, keys, cards, and media files. Click **Upload and restore to Emo** to restore all files, settings, and cards back to the device. EmoDesk restarts automatically.

**Option 2: From the Command Line (Python CLI)**
```bash
# Back up everything to your PC (downloads config, keys, cards, and all media files)
python tools/backup_restore.py backup --host emo.local

# Inspect any backup file offline
python tools/backup_restore.py inspect --file emodesk_backup_emo_20261012.json

# Restore everything back to EmoDesk
python tools/backup_restore.py restore --host emo.local --file emodesk_backup_emo_20261012.json
```

## 5. Changing the dashboard

The page lives in `web/dashboard.html`. After editing, run `python3 tools/build_ui.py`. This regenerates `EmoDesk/web_ui.h` (gzipped into the firmware); then re-upload.

## Troubleshooting

- **Boot-loop or crashes after enabling Bluetooth:** Check that your firmware fits the partition. With Bluetooth compiled in, the image is ~2.0 MB, so you must use the 3MB app partition in `partitions.csv` and select **"Huge APP (3MB No OTA/1MB SPIFFS)"** in Arduino IDE (never "Custom" or 1.75MB OTA schemes). Flash over USB. Also ensure you do NOT have the separate "NimBLE-Arduino" library installed (EmoDesk uses the ESP32 core's built-in BLE library).
- **"Safe mode" on the screen:** Emo crashed three times in a row, so Bluetooth, MQTT and the speaker are off until the next restart. The System tab shows the reason for the last restart and the log.
- **Black screen:** check SDA and SCL (4 and 5) and that the module address is 0x3C. Try `OLED_I2C_HZ 400000`.
- **Touch does nothing or acts constantly:** the TTP223 OUT goes to GPIO 3, and both solder jumpers should be open. Re-power after wiring, because it calibrates at power-on.
- **Doesn't join Wi-Fi:** the setup hotspot appears after 20 seconds. Super Mini antennas are weak, so keep **Low transmit power** on and move closer. Use a 2.4 GHz network.
- **emo.local doesn't open:** use the IP from the Network card. Many Android browsers don't support .local names.
- **Robot voice libraries won't compile:** set `ENABLE_TTS 0`, and remove the two audio libraries from `platformio.ini` if you use PlatformIO.
- **"Loading" forever on a card:** see the log in System. API keys, network or rate limits are the usual causes.

## Changes in 1.1

- Fixed the Bluetooth boot loop (NimBLE-Arduino clashing with core 3.3), fixed presence scanning (it only listened for 4 ms), and Bluetooth now waits for Wi-Fi and free memory.
- Lower RAM use: smaller task stacks, the audio task only exists when a speaker is on, JSON is streamed to the browser, and HTTPS waits for enough memory instead of hanging.
- Main-loop watchdog, crash counter and safe mode.
- Battery saver, low-battery handling, deep sleep and night sleep, a better battery percentage curve, a new Battery card and a Health panel in System.
- Dashboard: switches no longer overlap their rows, rows and button bars wrap, and the layout works on phones.

## Limitations

- **No internet radio or MP3 streaming.** There isn't room for an MP3 decoder alongside everything else. Upload MP3 clips instead; the dashboard converts them to WAV in your browser.
- **No voice commands** (there's no microphone), and **no Bluetooth speakers** (the C3 only has Bluetooth Low Energy).
- **Calendar events:** repeating (RRULE) events in iCal feeds aren't expanded. Very large calendars are only read partially.
- **Unofficial feeds:** stocks (Yahoo) and football (ESPN) use public endpoints without official support and may change.
- **Melodies:** the built-in melodies are public-domain tunes. Custom RTTTL tunes are your responsibility.
- **Bluetooth presence:** phones randomise their Bluetooth address, so presence works best with a watch or band that advertises a fixed name.
