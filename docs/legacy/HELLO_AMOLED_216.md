# Waveshare ESP32-S3-Touch-AMOLED-2.16 Hello World

This note records the minimal firmware flow used in this workspace.

## Project

The standalone Hello World firmware lives in:

```text
hello_amoled_216/
```

It is separate from `Clawdmeter/`, so the original project is not modified.

Main files:

```text
hello_amoled_216/platformio.ini
hello_amoled_216/src/main.cpp
```

The firmware initializes the Waveshare AMOLED display and prints `Hello World`
on both the screen and USB serial.

## USB Port

On this machine the board appeared as:

```text
COM5 - USB Serial Device
```

Other Bluetooth COM ports were ignored.

## PlatformIO

`pio` was not available on `PATH`, so PlatformIO was installed into the active
Python environment:

```powershell
python -m pip install platformio
```

All PlatformIO commands were then run through Python:

```powershell
python -m platformio --version
```

## Build

From the Hello firmware directory:

```powershell
cd C:\Users\kvenu\playground\mynomi\hello_amoled_216
python -m platformio run
```

The first build downloads the ESP32 platform and Arduino GFX dependency, so it
takes longer than later builds.

## Flash

Upload to the detected USB serial port:

```powershell
python -m platformio run -t upload --upload-port COM5
```

The successful upload identified the board as an ESP32-S3 with USB-Serial/JTAG:

```text
Chip type: ESP32-S3
USB mode: USB-Serial/JTAG
MAC: a4:cb:8f:d7:6a:3c
```

PlatformIO printed a Windows `UnicodeEncodeError` after reporting success. This
was only a terminal encoding issue while printing progress characters; the
firmware upload had already completed successfully.

## Serial Verification

Read serial output at 115200 baud:

```powershell
@'
import serial, time
port = "COM5"
ser = serial.Serial(port, 115200, timeout=0.5)
ser.dtr = False
ser.rts = False
end = time.time() + 8
while time.time() < end:
    data = ser.readline()
    if data:
        print(data.decode("utf-8", errors="replace").rstrip())
ser.close()
'@ | python -
```

Expected output:

```text
Hello World from Waveshare ESP32-S3-Touch-AMOLED-2.16
Hello World, uptime=0s
Hello World, uptime=1s
Hello World, uptime=2s
```

The board screen should show a dark Hello World panel with an uptime counter.
