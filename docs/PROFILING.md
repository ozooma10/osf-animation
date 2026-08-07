# Profiling OSF Animation

The ordinary releasedbg DLL contains no profiler client, branch, timer read, background thread,
or profiler state. Use WPR/WPA for routine sampling of that shipping-equivalent build. Use the
separate MO2 override only when named Tracy zones or plots are needed.

## Routine CPU sampling (normal DLL)

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

## Named Tracy capture (instrumented override)

Install the pinned official viewer once:

    .\tools\profiling\Install-TracyViewer.ps1

The installer queries the official GitHub release metadata, requires its SHA-256 asset digest, and
extracts the verified viewer under build/tools. It does not install anything system-wide.

With Starfield closed, build and deploy the override:

    .\tools\profiling\Build-TracyOverride.ps1

This builds an optimized/symbolized releasedbg DLL with Tracy v0.13.1, deploys only the DLL/PDB to
OSF Animation Profiling, verifies that the ordinary mod's DLL hash did not change, and restores
xmake to the normal non-profiling configuration in a finally block.

In MO2, enable OSF Animation Profiling at higher conflict priority than the ordinary OSF Animation
mod. Confirm in the Conflicts view that the profiling mod wins SFSE/Plugins/OSF Animation.dll.
Start Starfield, launch tracy-profiler.exe, and connect manually to 127.0.0.1. The profile build uses
on-demand, localhost-only collection with network broadcast, Tracy crash handling, sampling, system
tracing, context switches, callstacks, frame images, VSync capture, code transfer, and fibers disabled.

There is deliberately no Tracy FrameMark: neither native hook is a verified once-per-render-frame
boundary. Use the named hook zones and plots without interpreting them as render frames.

After the capture, disable the profiling override and restart Starfield. The normal DLL will win
again without rebuilding. A profile session is not a release validation: run a separate normal-DLL
scene and overlay smoke test before shipping.

## Release safety

packaging/build-archive.ps1 clean-configures osf_profiler=n and rejects any DLL containing the
OSF_TRACY_PROFILE_BUILD marker, including when -SkipBuild is used. The profiling override is
DLL/PDB-only and is never part of the release archive.

References: [WPR command line](https://learn.microsoft.com/windows-hardware/test/wpt/wpr-command-line-options),
[WPA symbols](https://learn.microsoft.com/windows-hardware/test/wpt/loading-symbols), and
[Tracy](https://github.com/wolfpld/tracy).
