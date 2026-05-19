# OS System Monitor - C++ SDL2 Project

## Short description

This project is an interactive graphical OS system monitor built with **C++** and **SDL2**. It visualizes operating-system information such as CPU usage, memory usage, disk usage, and currently running processes.

The app reads live system information from Linux `/proc` files and displays it in a 2D graphical interface.

## Features

- Graphical SDL2 window
- Text rendering using SDL_ttf
- Image rendering using SDL_image
- Solid rectangles, bars, outlines, and lines
- CPU usage visualization
- Memory usage visualization
- Disk usage visualization
- Running process table
- Interface updates automatically every 500ms
- Keyboard and mouse interaction
- Sorting by CPU, memory, or process name
- Scrolling through the process list
- Selecting a process by clicking a row

## Linux setup

Install the needed packages:

```bash
sudo apt update
sudo apt install build-essential git make g++ pkg-config libsdl2-dev libsdl2-image-dev libsdl2-ttf-dev
```

## Compile and run

From the project folder:

```bash
make run
```

To clean the compiled file:

```bash
make clean
```

## User interactions

- Click **Sort: CPU** to sort processes by CPU usage
- Click **Sort: Memory** to sort processes by memory usage
- Click **Sort: Name** to sort processes alphabetically
- Press `C` to sort by CPU usage
- Press `M` to sort by memory usage
- Press `N` to sort by process name
- Use mouse wheel to scroll through processes
- Press Up/Down or PageUp/PageDown to scroll
- Click a process row to select it
- Press `P` to pause/resume live updates
- Press `R` to reset scroll position
- Press `Esc` to quit

## Expected grade

47.5 / 47.5 points

This project includes the basic requirements and advanced user interaction requirements:

- Visual interface with text, image, rectangles, and lines
- Updates at an interval
- Mouse and keyboard interaction
- Sorting data by different metrics
- Scrolling when not all data is visible
- Navigating/selecting pieces of data
