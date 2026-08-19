# Candis-S31 USB CDC Device Diagnostics

This image presents Type-C2 as a USB CDC ACM device to a host PC. The board is
explicitly placed in the Type-C Sink role and never enables the OTG 5 V boost.

After Windows enumerates the device, open the new COM port at any baud rate and
send one of these commands:

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
