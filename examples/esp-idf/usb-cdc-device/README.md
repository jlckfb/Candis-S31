# Candis-S31 USB CDC device console

This image presents Type-C2 as a USB CDC ACM device to a host PC. The board is
explicitly placed in the Type-C Sink role and never enables the OTG 5 V boost.

Attach a host PC to **Type-C2** with a data cable; a USB stick must be removed
from this connector. After the CDC ACM device enumerates, open that new port
(not the Type-C1 console) at any baud rate, assert DTR, and send newline-terminated
commands:

```text
ping
hello
art
status
led on
led off
```

The firmware replies with a banner when DTR is asserted and echoes unknown
commands. `status` reports the live FUSB303B connection state and uptime.
Each command ends with LF, CR or CRLF. Reads are assembled as a byte stream,
so split packets and several lines in one packet are supported. The command
limit is 255 bytes; longer lines are rejected once at the delimiter without
executing their truncated prefix.

Allow **10–15 s after the host port opens** for the command sequence. Expect
`ping` → `pong`, `hello` → `Hello from Candis-S31!`, `status` → live status,
an unknown line → `echo: ...`, and `led on` / `led off` responses; the LED
shows the physical effect. Send `pi` then `ng\n` in separate writes and
`ping\nhello\n` in one write to exercise framing. A host PC on **Type-C2**
provides the round trip.

Windows PowerShell example:

```powershell
$p = [System.IO.Ports.SerialPort]::new(
    'COM6', 115200,
    [System.IO.Ports.Parity]::None, 8,
    [System.IO.Ports.StopBits]::One)
$p.DtrEnable = $true
$p.RtsEnable = $true
$p.Open()
Start-Sleep -Milliseconds 500
$p.ReadExisting()

foreach ($cmd in 'ping','hello','art','status','led on','led off') {
    $p.WriteLine($cmd)
    Start-Sleep -Milliseconds 500
    "--- $cmd ---"
    $p.ReadExisting()
}

$p.Close()
```

## Build, flash, and monitor

The example is a standalone ESP-IDF project at `examples/esp-idf/usb-cdc-device/`.
It builds against the repository-owned board component through
`cmake/candis_components.cmake`. With ESP-IDF `v6.1-rc1` (`esp32s31` is a
preview target, hence `--preview`), run from this example directory:

```bash
idf.py --preview set-target esp32s31
idf.py --preview build
idf.py --preview -p /dev/ttyACM0 -b 4000000 flash
idf.py --preview -p /dev/ttyACM0 monitor
```

## Connectors and ports

- **Type-C1** powers the board and carries flashing and the console. It is
  wired through a CH343P USB-UART bridge. Replace `/dev/ttyACM0` with the
  bridge's port on your host (for example, `COM5` on Windows).
- **Type-C2** carries the USB Device data lines to the SoC's native USB
  pads; the CDC ACM device enumerates through this connector. This image
  never enables the OTG 5 V boost, so C2 is for data only — power and
  flashing always come from Type-C1.

With both connectors attached to the same host, Windows enumerates **two
different COM ports**: the CH343P bridge port used by `flash`/`monitor`, and
the CDC ACM port created by this firmware. The `idf.py` commands above must
point at the CH343P port; the PowerShell session above must open the CDC ACM
port. They are not interchangeable.

The 4,000,000 baud setting applies to flashing through the C1 UART bridge: it
is the highest reliable rate on this board, and 5 Mbaud does not work here.
Reduce the rate if the host or cable is unreliable. C2 USB CDC line coding is
virtual and is not a UART flashing-speed measurement.
