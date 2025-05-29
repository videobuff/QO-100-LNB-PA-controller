### Description of QO-100 LNB & PA Controller Functionality

The QO-100 LNB & PA Controller is a web-based interface designed to manage and monitor a Low-Noise Block downconverter (LNB) and Power Amplifier (PA) for the QO-100 satellite, a geostationary amateur radio transponder. It provides real-time monitoring of forward and reflected power, SWR (Standing Wave Ratio), and PA temperature, displayed on a customizable gauge. Users can toggle the PA and polarization modes (SSB/DATV) remotely, set SWR thresholds for automatic PA shutdown, and perform over-the-air (OTA) firmware updates. The system includes a WebSocket connection for live data updates, a settings panel for gauge configuration, and alerts for critical conditions like high SWR or temperature.

---

### Hardware Requirements

- **ESP32 Microcontroller**: Serves as the core hardware, handling Wi-Fi connectivity, GPIO control, and ADC readings for power and temperature.
- **Relay Modules**: One for the LNB polarization switch (connected to a GPIO pin, e.g., D14) and one for the PA (connected to another GPIO pin, e.g., D26).
- **Power Sensors**: Analog sensors (e.g., AD8307 or similar) connected to ADC pins (e.g., GPIO 34 for forward power, GPIO 35 for reflected power) to measure power levels.
- **Temperature Sensor**: Optional (e.g., DS18B20) for PA temperature monitoring, connected to a GPIO pin.
- **Power Supply**: Suitable voltage supply for the ESP32, relays, and connected devices.
- **SPIFFS Storage**: Required for storing the web interface files (`index.html`, `script.js`, `style.css`, and `meter-satpwr.png`).
- **Wiring and Connectors**: Jumper wires, breadboard, or a custom PCB to connect components.

---

### Mini How-To

1. **Set Up Hardware**:
   - Connect the ESP32 to a power supply and wire the relay modules to control the LNB and PA (e.g., D14 for LNB, D26 for PA).
   - Attach power sensors to GPIO 34 (forward) and GPIO 35 (reflected), and optionally a temperature sensor to another GPIO.
   - Upload the `meter-satpwr.png` image and web files (`index.html`, `script.js`, `style.css`) to the ESP32’s SPIFFS using the ESP32 Sketch Data Upload tool.

2. **Flash Firmware**:
   - Load the `polarization-10.ino` sketch onto the ESP32 using the Arduino IDE, ensuring libraries (WiFi, WiFiManager, ESPAsyncWebServer, ArduinoJson, Preferences, Update) are installed.
   - Configure Wi-Fi credentials via WiFiManager during the first boot.

3. **Access the Interface**:
   - Connect to the ESP32’s Wi-Fi AP (e.g., "QO100-Controller-...") or join its network after configuration.
   - Open a web browser and navigate to the ESP32’s IP address (e.g., `192.168.4.1` or assigned IP).
   - Authenticate with the default credentials (username: `admin`, password: `password`).

4. **Configure and Use**:
   - Monitor power, SWR, and temperature on the gauge and metrics display.
   - Toggle the PA or polarization mode using the respective buttons.
   - Adjust gauge settings (e.g., power max, needle color) in the settings panel and save them.
   - Check for firmware updates and perform OTA updates via the OTA form, entering a valid firmware URL.
