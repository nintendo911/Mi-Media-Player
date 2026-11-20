# Mi Media Player

Windows desktop UI written in C++/Win32. The existing `play_mi.py` remains the
audio backend, so the UI exposes its current playback and MI01 tools without
reimplementing the codec.

## Build

Build with a Win64 MinGW-w64 UCRT toolchain. Add its `bin` directory to `PATH`,
then run these commands from the project directory:

```powershell
windres app.rc -O coff -o app_res.o
g++ -std=c++17 -municode -mwindows -DUNICODE -D_UNICODE main.cpp app_res.o -o MiMediaPlayer.exe -lcomdlg32 -lgdi32 -luser32
```

The build embeds `app.ico` in the executable.
Python must be installed and available as `python` on `PATH`. FFmpeg is needed
for audio formats beyond PCM WAV and RAW/PCM; the converter can handle formats
supported by the installed FFmpeg build. When available, ffprobe preserves the
input sample rate and channel count (up to six); otherwise audio is converted
as 44.1 kHz stereo. The app can select multiple audio files and batch-convert
them to `.mi` beside their source files. For example:

```powershell
python play_mi.py -b "C:\Music\song one.flac" "C:\Music\song two.mp3"
```

When multiple files are selected in the UI, **Phát** plays the first one.

## UI actions

- **Phát**: play a selected `.mi` or supported audio file.
- **Dừng**: terminate the active backend process.
- **Chuyển .mi**: convert one or more selected audio files to MI01 ADPCM.
- **Giải mã RAW**: export a selected `.mi` as raw PCM.
- **Thông tin .mi**: display format, duration, and size details in the log.
