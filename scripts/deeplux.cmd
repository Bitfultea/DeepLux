@echo off
rem ===========================================================================
rem DeepLux CLI wrapper (Windows)
rem ===========================================================================
rem DeepLux.exe is a GUI-subsystem executable. cmd.exe waits for it and
rem forwards its stdout/stderr, while PowerShell's call operator does not
rem reliably wait for GUI-subsystem processes. Use this wrapper for CLI mode:
rem
rem   deeplux --cli info
rem   deeplux --cli list-plugins
rem ===========================================================================
"%~dp0DeepLux.exe" %*
