# Profiling OSF Animation

Use Windows Performance Recorder and Analyzer to sample the ordinary releasedbg DLL. This keeps
profiling external to OSF Animation and measures the same native binary used for normal play.

## CPU sampling

Windows Performance Recorder and Analyzer are part of the Windows Performance Toolkit. Start
Starfield normally, load the state to measure, then run an elevated PowerShell from this repository:

    .\tools\profiling\Capture-Cpu.ps1 -Label camera-orbit

Reproduce for 10-20 seconds and press Enter in the PowerShell window. The script saves a compressed
ETL under build/profiles, configures the local normal/profile PDB directories ahead of Microsoft's
symbol server, and opens WPA. In WPA:

1. Add **Computation > CPU Usage (Sampled)**.
2. Filter Process to Starfield.exe.
3. Load symbols, group by Stack, and find OSF Animation.dll frames.
4. Use inclusive samples to find expensive call trees and exclusive samples to find leaf work.

Recording has system-wide sampling overhead only while WPR is active. If capture fails, the script
cancels its named WPR session instead of leaving a recorder running.

References: [WPR command line](https://learn.microsoft.com/windows-hardware/test/wpt/wpr-command-line-options),
[WPA symbols](https://learn.microsoft.com/windows-hardware/test/wpt/loading-symbols).
