# EmoDesk wiring

Everything runs from the ESP32-C3 Super Mini's USB-C port. Use a good 5 V / 1 A (or better) USB supply once the speaker is connected.

## Pin map

| Part | Part pin | Super Mini pin | Notes |
|---|---|---|---|
| SSD1306 OLED 128x64 (I2C) | VCC | 3V3 | Some modules accept 5 V too; 3.3 V is always safe |
| | GND | GND | |
| | SDA | GPIO 4 | |
| | SCL | GPIO 5 | |
| TTP223 touch | VCC | 3V3 | |
| | GND | GND | |
| | I/O (OUT) | GPIO 3 | Default jumpers: momentary, output HIGH when touched |
| Buzzer (optional, passive or active) | + (SIG) | GPIO 6 | Default pin (configurable in **Sound -> Buzzer GPIO pin**: 6, 7, 10, 1, or 2) |
| | - (GND) | GND | If using a 3-pin buzzer module, connect VCC to 3V3 |
| MAX98357A amp (optional, add later) | VIN | 5V | Not 3V3 - the amp needs 5 V for full power |
| | GND | GND | |
| | BCLK | GPIO 6 | |
| | LRC | GPIO 7 | Sometimes labelled WS or LRCLK |
| | DIN | GPIO 10 | |
| | SD | GPIO 1 | Lets the firmware switch the amp off when silent (no hiss, less power). Leave unconnected if you prefer - the amp then stays on |
| | GAIN | not connected | 9 dB. See the gain table below |
| | + / - | Speaker | 4 Ω or 8 Ω, 2-3 W. Never connect either speaker wire to GND |
| On-board | Blue LED | GPIO 8 | Status light (alerts, sleep breathing) |
| On-board | BOOT button | GPIO 9 | Second control button after boot |
| Battery (ADC) | V_BAT | GPIO 0 | Optional voltage divider (e.g. 10k/10k) from battery positive to GND |

## Base build (screen + touch)

```
                 ESP32-C3 Super Mini
               +-------------------+
               |  [USB-C]          |
   OLED VCC ---| 3V3          5V   |
   OLED GND ---| GND          GND  |
               |                   |
   OLED SDA ---| GPIO4             |
   OLED SCL ---| GPIO5             |
   TTP223 OUT -| GPIO3             |
               +-------------------+

   TTP223 VCC -> 3V3        TTP223 GND -> GND
```

## Adding a buzzer (Passive or Active, no amplifier needed)

If you prefer a simple 2-wire or 3-pin buzzer instead of the MAX98357A speaker:

- **2-pin buzzer**:
  - Passive piezo: `+` -> **GPIO 6 / 10** and `-` -> **GND** (dashboard: *Passive Buzzer*).
  - Active buzzer (standard): `+` -> **GPIO 10** and `-` -> **GND** (dashboard: *Active Buzzer*).
  - Active buzzer (loud without transistor - Active-Low): `+` -> **3V3 pin** and `-` -> **GPIO 10** (dashboard: *Active Buzzer* + enable *Active-low drive*). Draws from the 500mA regulator rail instead of sourcing through the pin.
- **High-Volume Active Buzzer circuit (Transistor driver - 100% full battery volume)**:
  ```text
       Battery (+) 3.7V / 5V
                │
              ┌─┴─┐
              │ + │ Active Buzzer
              │ - │
              └─┬─┘
                │
                ├── Collector (C) [2N2222 / 2N3904 / S8050 NPN]
   GPIO 10 ───[ 1kΩ ]─── Base (B)
                │
                └── Emitter (E) ──── GND
  ```
- **3-pin buzzer module** (`VCC`, `GND`, `I/O` / `SIG` - has built-in transistor):
  - `VCC` -> **3.7V Battery / 5V** (or **3V3**)
  - `GND` -> **GND**
  - `I/O` (or `SIG`) -> **GPIO 10** (or GPIO 6)
- Then open the dashboard **Sound** tab and set **Output device** to **Passive Buzzer (PWM tones & melodies)** (or **Active Buzzer** if it beeps continuously when connected straight to 3.3V). You can also switch via Serial/Web CLI: `sound mode buzzer` and `sound pin 6`.

## Adding the speaker (MAX98357A)

```
     ESP32-C3 Super Mini                  MAX98357A                 Speaker
   +-------------------+              +--------------+            4-8 ohm 3W
   | 5V      ----------+------------->| VIN          |
   | GND     ----------+--+---------->| GND          |
   |                   |  |           |              |   (+)---->  +
   | GPIO6   ----------+--|---------->| BCLK         |   (-)---->  -
   | GPIO7   ----------+--|---------->| LRC          |
   | GPIO10  ----------+--|---------->| DIN          |
   | GPIO1   ----------+--|---------->| SD           |
   +-------------------+  |           | GAIN (open)  |
                          |           +--------------+
          470uF 10V cap:  5V (+) ---||--- GND (-)   placed close to the amp's VIN/GND
```

Then open the dashboard, go to **Sound** and switch on **Speaker installed**.

### Gain

| GAIN pin | Gain |
|---|---|
| 100 kΩ to GND | 15 dB (loudest) |
| GND | 12 dB |
| not connected | 9 dB (default) |
| VIN | 6 dB |
| 100 kΩ to VIN | 3 dB (quietest) |

Start at 9 dB with the dashboard volume around 40 %. Small desk speakers distort quickly at full power.

## Tips

- **Touch through the case.** The TTP223 works through up to about 3 mm of plastic or wood. Stick the pad flat against the inside wall with double-sided tape. Its ground plane should face away from your finger.
- **TTP223 jumpers.** Leave both solder jumpers open (momentary, active-high). If you closed jumper A (toggle mode) the gestures will not work.
- **Short I2C wires.** Keep the OLED wires under 15 cm. If the screen glitches, lower `OLED_I2C_HZ` in `config.h` to `400000`.
- **Hiss or buzz.** Add the 470 µF capacitor, keep audio wires away from the antenna end of the board, and route the speaker wires as a twisted pair.
- **Weak Wi-Fi.** Many Super Mini boards have a poorly tuned antenna. EmoDesk lowers transmit power by default, which counter-intuitively fixes most connection problems (Dashboard → System → Low transmit power). Keep metal and the speaker magnet away from the antenna.
- **Pins to avoid.** GPIO 2, 8 and 9 are strapping pins (8 and 9 are used for the LED and BOOT button on purpose). GPIO 20/21 are the UART. GPIO 0 and 1 are free if you want to extend the project later. GPIO 1 is used for the amp SD pin.
- **Powering from a battery.** Feed 5 V into the 5V pin (not 3V3) through a boost module, and add a switch. Expect roughly 80-120 mA idle with Wi-Fi, peaking well above 500 mA with loud audio.
