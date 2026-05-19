# Canvas Submission Text

## GitHub URL

Paste your GitHub repo URL here:

```text
https://github.com/its-hadi/os-system-monitor
```

## Compilation and running instructions

```bash
sudo apt update
sudo apt install build-essential git make g++ pkg-config libsdl2-dev libsdl2-image-dev libsdl2-ttf-dev
make run
```

## Short description of what I built

I built an interactive OS System Monitor using C++ and SDL2. The application displays live CPU usage, memory usage, disk usage, and a list of running processes. It reads OS data from Linux system files such as `/proc/stat`, `/proc/meminfo`, and `/proc/[pid]/stat`, then visualizes the information with bars, tables, text, lines, and an image.

## List of interactions

- Click buttons to sort processes by CPU, memory, or name
- Press C to sort by CPU
- Press M to sort by memory
- Press N to sort by process name
- Use the mouse wheel to scroll through the process list
- Use Up/Down/PageUp/PageDown to scroll
- Click a process row to select it
- Press P to pause or resume live updates
- Press R to reset scrolling
- Press Esc to quit

## Expected grade

47.5 / 47.5 points
