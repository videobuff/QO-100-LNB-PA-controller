
### Release: v15
### Author: Erik Schott - PA0ESH
### Mail: info@pa0esh.com
### Licensing: Gpl V3.0 see LICENSE file.
### Languages: This application is written in Arduino code, Javascript and HTML


## Introduction
The QO-100 LNB/PA Controller is designed to control the polarisation and PA relay, measure forward/reflected power and DC rails, and protect against high SWR. It includes a built-in Wi-Fi web interface and a simulation mode for testing without hardware.

![schermprint 2025-09-14 om 00 15 47](https://github.com/user-attachments/assets/f8ee8201-3f2f-435c-aaaf-810131394f56)


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
## Interface pins
- mainPaPin    = 14;   // PA relais
- polPin       = 26;   // SSB/DATV
- RECOVERY_PIN = 4;    // bij LOW op boot -> login reset

## Alleen ADC1 pins gebruiken (ADC2 stoort met WiFi)
- PIN_FORWARD   = 34;  ADC1_CH6  (FOR)
- PIN_REFLECTED = 35;  ADC1_CH7  (REF)
- PIN_V5        = 32;  ADC1_CH4
- PIN_V12       = 33;  ADC1_CH5
- PIN_V18       = 36;  ADC1_CH0
- PIN_V28       = 39;  ADC1_CH3
- PIN_TEMP      = -1;  NTC optioneel (ADC1-pin), -1 = uit
## Resistor bridges for ADC inputs
- 5 V rail → ADC ~3.2 V Rt = 15 kΩ, Rb = 22 kΩ K ≈ 1.68
- 12 V rail → ADC ~3.2 V Rt = 68 kΩ, Rb = 22 kΩ K ≈ 4.09
- 18 V rail → ADC ~3.2 V Rt = 120 kΩ, Rb = 22 kΩ K ≈ 6.45
- 28 V rail → ADC ~3.2 V Rt = 200 kΩ, Rb = 22 kΩ K ≈ 10.09
- Forward/Reflected 5 V → ADC ~3.2 V Rt = 10 kΩ, Rb = 18 kΩ K ≈ 1.78

## Installation from blank ESP32
1. Install Arduino IDE 2.x and ESP32 board support.
2. Connect ESP32 via USB.
3. Select board: ESP32 Dev Module.
4. Select partition scheme: Default 4MB with spiffs.
5. Upload firmware (polarisation_xx.ino).
6. Upload web files via Tools → ESP32 Sketch Data Upload.
7. Reboot ESP32. If no Wi-Fi found, it creates AP 'QO100-Controller-xxxx'. Connect and configure Wi-Fi.
9. Log in via browser (default user: admin, password: password).
    
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

#  Troubleshooting
- 'Not found': LittleFS upload failed or wrong FS used
- No meter movement: check WS-CON indicator
- After Save no change: hard refresh (Ctrl+F5 / Cmd+Shift+R)
- No Wi-Fi: reconnect to AP and configure Wi-Fi again

