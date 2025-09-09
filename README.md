## Introduction
The QO-100 LNB/PA Controller is designed to control the polarisation and PA relay, measure forward/reflected power and DC rails, and protect against high SWR. It includes a built-in Wi-Fi web interface and a simulation mode for testing without hardware.

![Webinterface - layout qo-100 controller](https://github.com/user-attachments/assets/0a60077f-b7f8-4a03-b4a6-5238126e9da7)


## Functionality
- Forward/Reflected power and SWR monitoring with protection
- Main PA relay switching with SWR protection
- Polarisation relay (SSB/DATV)
- DC rails monitoring (5V, 12V, 18V, 28V)
- PA temperature monitoring
- Adjustable settings for thresholds, needle appearance, and calibration factors
- Stored in Preferences (persistent)
- OTA Update support
- Simulation mode for testing without hardware
## Installation from blank ESP32
1. Install Arduino IDE 2.x and ESP32 board support.
2. Connect ESP32 via USB.
3. Select board: ESP32 Dev Module.
4. Select partition scheme: Default 4MB with spiffs.
5. Upload firmware (polarisation_xx.ino).
6. Upload web files via Tools → ESP32 Sketch Data Upload.
7. Reboot ESP32. If no Wi-Fi found, it creates AP 'QO100-Controller-xxxx'. Connect and configure Wi-Fi.
8. Log in via browser (default user: admin, password: password).
## Usage
- Dashboard: shows power, SWR and temperature
- Controls: Main PA ON/OFF and mode toggle
- SWR colour indication (green, orange, red) with latch protection
- Settings: adjustable and stored persistently
- OTA Update: upload new firmware
- Simulation mode: test without hardware

## Hardware connections
- GPIO14 → PA relay
- GPIO26 → Polarisation relay
- GPIO32–39 → ADC1 inputs for voltage monitoring
- GPIO4 → Recovery pin (resets login)
## Voltage dividers:
- 5V: 15k/22k
- 12V: 68k/22k
- 18V: 120k/22k
- 28V: 200k/22k
#  Troubleshooting
- 'Not found': LittleFS upload failed or wrong FS used
- No meter movement: check WS-CON indicator
- After Save no change: hard refresh (Ctrl+F5 / Cmd+Shift+R)
- No Wi-Fi: reconnect to AP and configure Wi-Fi again

