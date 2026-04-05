GNURadio HiFi Capture Scripts for DomesdayDuplicator
=====================================================

These scripts capture VHS HiFi audio via an RTL-SDR dongle (RTL-SDR Blog V4)
and are launched automatically by the DomesdayDuplicator app when "SDR HiFi"
capture is enabled in Preferences.

SCRIPTS
-------
rtlsdr_to_flac.py       - Captures both FLAC audio and raw u8 baseband
rtlsdr_to_flac_only.py  - Captures FLAC audio only
rtlsdr_to_u8_only.py    - Captures raw u8 baseband only

All scripts accept:
  --output  <path>        Output base path (no extension), e.g. E:\Captures\tape1
  --system  PAL|NTSC      TV system (default: PAL)
  --gain    <dB>          Tuner gain, 0 = auto AGC (default: 0)

REQUIREMENTS
------------
1. Radioconda (GNURadio for Windows)
   https://github.com/radioconda/radioconda-installer

   Install radioconda, then set the Python path in DomesdayDuplicator
   Preferences > SDR HiFi to:
     C:\ProgramData\radioconda\python.exe
   (this is the default — leave blank to use it automatically)

2. fmedia (optional, for analog audio capture)
   https://www.videohelp.com/software/fmedia

   Used for capturing analog audio from a sound card input simultaneously
   with the RF capture. Configure in Preferences > Audio Capture.

HARDWARE
--------
RTL-SDR Blog V4 dongle connected to the VHS deck's HiFi output
via a simple FM antenna or direct coupling circuit.
Tune frequency: 1600000 Hz (PAL) or 1500000 Hz (NTSC)
