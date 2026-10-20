# ESP32-CAM Face Recognition Smart Lock

An embedded systems project that combines an ESP32-CAM smart lock with a connected Windows laptop application for face enrollment and recognition. The full system was completed and demonstrated successfully by the project author. Face recognition runs on the laptop; the ESP32-CAM provides the camera stream and controls the lock.

## How it works

1. The ESP32-CAM starts a local Wi-Fi access point and streams camera frames over MJPEG.
2. The laptop application reads the stream, detects faces with OpenCV, and compares them with locally stored LBPH profiles.
3. After repeated matching results, the laptop sends an authenticated unlock request to the ESP32-CAM.
4. The ESP32-CAM accepts that request only while face-unlock mode is active. A keypad PIN remains available as a fallback.

The firmware also provides a keypad and OLED interface, status/control endpoints, and timed solenoid control. See [`wiring-schematic.svg`](wiring-schematic.svg) for the wiring diagram.

## Main components

- AI-Thinker ESP32-CAM
- 4x4 keypad with I2C expander
- 128x64 SSD1306 OLED
- Solenoid and L298N driver
- Windows laptop running Python, NumPy, and OpenCV

## Repository layout

- `src/main.cpp` — ESP32-CAM firmware
- `laptop/laptop_face_lock.py` — laptop-side face enrollment and recognition application
- `laptop/requirements.txt` — Python dependencies
- `platformio.ini` — PlatformIO firmware configuration
- `lib/Keypad_I2C/` — keypad library source used by the firmware
- `wiring-schematic.svg` — project wiring diagram

## Setup

### 1. Configure private local settings

Copy `include/secrets.example.h` to `include/secrets.h` and `laptop/local_config.example.py` to `laptop/local_config.py`. Set your own Wi-Fi access-point name and password, keypad PIN, and a new random 64-character unlock token. Put the exact same token in both local configuration files. The real configuration files are ignored by Git.

Do not publish face samples, real access codes, Wi-Fi passwords, or unlock tokens. Enrolled profiles are stored locally in `laptop/face_data.npz`, which is ignored by Git.

### 2. Build and upload firmware

Install Visual Studio Code with PlatformIO, connect the ESP32-CAM, and build/upload the `esp32cam` environment from the project root. Check the schematic and board pinout before wiring; hardware variants may differ.

### 3. Install the laptop application dependencies

Use 64-bit Python 3.11 on Windows. From the project root, create a virtual environment and install the pinned packages:

```powershell
py -3.11 -m venv .venv
.\.venv\Scripts\python.exe -m pip install -r laptop\requirements.txt
```

Connect the laptop to the ESP32-CAM access point. With the default firmware address, the app uses `http://192.168.4.1`.

### 4. Enroll and run face recognition

From the project root, enroll a profile:

```powershell
.\.venv\Scripts\python.exe laptop\laptop_face_lock.py enroll --name YourName
```

Then start recognition:

```powershell
.\.venv\Scripts\python.exe laptop\laptop_face_lock.py run
```

Use the keypad to select face enrollment or face unlock. The app supports up to three local profiles. The keypad PIN can be used when the laptop application is unavailable.

## Project status and limits

The author reports that the integrated project worked successfully. It is a laptop-assisted system rather than an ESP32-only device: the laptop must run the recognition application for face unlock. The implementation uses OpenCV's LBPH recognizer and does not include liveness or presentation-attack detection, so it should be treated as a student prototype rather than production access-control equipment.

## Skills demonstrated

Embedded C++ · ESP32-CAM · Arduino framework · PlatformIO · OpenCV · Python · camera streaming · HTTP communication · keypad/OLED integration · hardware/software integration

## Author

[Intoy-xy](https://github.com/Intoy-xy)
Jos Luvi Amantillo Published - October 20
