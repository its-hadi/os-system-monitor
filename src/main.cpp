#include <SDL2/SDL.h>
#include <SDL2/SDL_ttf.h>
#include <SDL2/SDL_image.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cctype>
#include <dirent.h>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <map>
#include <sstream>
#include <string>
#include <sys/statvfs.h>
#include <unistd.h>
#include <vector>

using namespace std;

const int WINDOW_WIDTH = 1100;
const int WINDOW_HEIGHT = 720;
const int UPDATE_INTERVAL_MS = 500;

struct CpuSample {
    unsigned long long idle = 0;
    unsigned long long total = 0;
};

struct SystemStats {
    double cpuPercent = 0.0;
    double memoryPercent = 0.0;
    double diskPercent = 0.0;
    unsigned long long memoryUsedKB = 0;
    unsigned long long memoryTotalKB = 0;
    unsigned long long diskUsedBytes = 0;
    unsigned long long diskTotalBytes = 0;
};

struct ProcessInfo {
    int pid = 0;
    string name;
    double cpuPercent = 0.0;
    unsigned long long memoryKB = 0;
    unsigned long long ticks = 0;
};

enum class SortMode {
    CPU,
    MEMORY,
    NAME
};

struct Button {
    SDL_Rect rect{};
    string label;
    SortMode mode;
};

static map<int, unsigned long long> previousProcessTicks;
static CpuSample previousCpuSample;
static bool hasPreviousCpuSample = false;
static double lastTotalCpuDelta = 0.0;

string formatPercent(double value) {
    stringstream ss;
    ss << fixed << setprecision(1) << value << "%";
    return ss.str();
}

string formatKB(unsigned long long kb) {
    stringstream ss;
    if (kb >= 1024ULL * 1024ULL) {
        ss << fixed << setprecision(1) << (double)kb / (1024.0 * 1024.0) << " GB";
    } else if (kb >= 1024ULL) {
        ss << fixed << setprecision(1) << (double)kb / 1024.0 << " MB";
    } else {
        ss << kb << " KB";
    }
    return ss.str();
}

string formatBytes(unsigned long long bytes) {
    stringstream ss;
    const double gb = 1024.0 * 1024.0 * 1024.0;
    const double mb = 1024.0 * 1024.0;
    if (bytes >= (unsigned long long)gb) {
        ss << fixed << setprecision(1) << (double)bytes / gb << " GB";
    } else if (bytes >= (unsigned long long)mb) {
        ss << fixed << setprecision(1) << (double)bytes / mb << " MB";
    } else {
        ss << bytes << " B";
    }
    return ss.str();
}

vector<string> splitWhitespace(const string& text) {
    vector<string> tokens;
    stringstream ss(text);
    string token;
    while (ss >> token) {
        tokens.push_back(token);
    }
    return tokens;
}

CpuSample readCpuSample() {
    CpuSample sample;

#ifdef __linux__
    ifstream file("/proc/stat");
    string cpuLabel;
    unsigned long long user = 0, nice = 0, system = 0, idle = 0;
    unsigned long long iowait = 0, irq = 0, softirq = 0, steal = 0;

    if (file >> cpuLabel >> user >> nice >> system >> idle >> iowait >> irq >> softirq >> steal) {
        sample.idle = idle + iowait;
        sample.total = user + nice + system + idle + iowait + irq + softirq + steal;
    }
#else
    static unsigned long long fakeTotal = 10000;
    static unsigned long long fakeIdle = 6000;
    fakeTotal += 100;
    fakeIdle += 45 + (unsigned long long)(10 * sin(SDL_GetTicks() / 600.0));
    sample.total = fakeTotal;
    sample.idle = fakeIdle;
#endif

    return sample;
}

double calculateCpuPercent(const CpuSample& current) {
    if (!hasPreviousCpuSample || current.total <= previousCpuSample.total) {
        previousCpuSample = current;
        hasPreviousCpuSample = true;
        lastTotalCpuDelta = 0.0;
        return 0.0;
    }

    unsigned long long totalDelta = current.total - previousCpuSample.total;
    unsigned long long idleDelta = current.idle - previousCpuSample.idle;

    previousCpuSample = current;
    lastTotalCpuDelta = (double)totalDelta;

    if (totalDelta == 0) return 0.0;
    double usage = 100.0 * (1.0 - ((double)idleDelta / (double)totalDelta));
    return clamp(usage, 0.0, 100.0);
}

void readMemoryStats(SystemStats& stats) {
#ifdef __linux__
    ifstream file("/proc/meminfo");
    string key;
    unsigned long long value = 0;
    string unit;

    unsigned long long total = 0;
    unsigned long long available = 0;

    while (file >> key >> value >> unit) {
        if (key == "MemTotal:") total = value;
        if (key == "MemAvailable:") available = value;
    }

    if (total > 0) {
        stats.memoryTotalKB = total;
        stats.memoryUsedKB = total - available;
        stats.memoryPercent = 100.0 * (double)stats.memoryUsedKB / (double)total;
    }
#else
    stats.memoryTotalKB = 16ULL * 1024ULL * 1024ULL;
    stats.memoryPercent = 42.0 + 15.0 * sin(SDL_GetTicks() / 1500.0);
    stats.memoryUsedKB = (unsigned long long)(stats.memoryTotalKB * (stats.memoryPercent / 100.0));
#endif
}

void readDiskStats(SystemStats& stats) {
    struct statvfs disk{};
    if (statvfs("/", &disk) == 0) {
        unsigned long long total = (unsigned long long)disk.f_blocks * (unsigned long long)disk.f_frsize;
        unsigned long long freeSpace = (unsigned long long)disk.f_bfree * (unsigned long long)disk.f_frsize;
        unsigned long long used = total - freeSpace;

        stats.diskTotalBytes = total;
        stats.diskUsedBytes = used;
        if (total > 0) {
            stats.diskPercent = 100.0 * (double)used / (double)total;
        }
    }
}

bool isNumberString(const string& text) {
    if (text.empty()) return false;
    for (char c : text) {
        if (!isdigit((unsigned char)c)) return false;
    }
    return true;
}

bool readProcessFromProc(int pid, ProcessInfo& process) {
#ifdef __linux__
    string path = "/proc/" + to_string(pid) + "/stat";
    ifstream file(path);
    if (!file.is_open()) return false;

    string line;
    getline(file, line);
    if (line.empty()) return false;

    size_t openParen = line.find('(');
    size_t closeParen = line.rfind(')');
    if (openParen == string::npos || closeParen == string::npos || closeParen <= openParen) {
        return false;
    }

    string name = line.substr(openParen + 1, closeParen - openParen - 1);
    string rest = line.substr(closeParen + 2);
    vector<string> fields = splitWhitespace(rest);

    // /proc/[pid]/stat fields after process name start at field 3.
    // field 14 = utime -> fields[11], field 15 = stime -> fields[12]
    // field 24 = rss pages -> fields[21]
    if (fields.size() < 22) return false;

    unsigned long long utime = stoull(fields[11]);
    unsigned long long stime = stoull(fields[12]);
    long rssPages = stol(fields[21]);
    long pageSizeKB = sysconf(_SC_PAGESIZE) / 1024;

    process.pid = pid;
    process.name = name;
    process.ticks = utime + stime;
    process.memoryKB = rssPages > 0 ? (unsigned long long)rssPages * (unsigned long long)pageSizeKB : 0;

    auto previous = previousProcessTicks.find(pid);
    if (previous != previousProcessTicks.end() && lastTotalCpuDelta > 0) {
        unsigned long long delta = process.ticks >= previous->second ? process.ticks - previous->second : 0;
        process.cpuPercent = 100.0 * (double)delta / lastTotalCpuDelta;
    } else {
        process.cpuPercent = 0.0;
    }

    previousProcessTicks[pid] = process.ticks;
    return true;
#else
    (void)pid;
    (void)process;
    return false;
#endif
}

vector<ProcessInfo> readProcesses() {
    vector<ProcessInfo> processes;

#ifdef __linux__
    DIR* procDir = opendir("/proc");
    if (!procDir) return processes;

    dirent* entry = nullptr;
    while ((entry = readdir(procDir)) != nullptr) {
        string name = entry->d_name;
        if (!isNumberString(name)) continue;

        int pid = stoi(name);
        ProcessInfo info;
        if (readProcessFromProc(pid, info)) {
            processes.push_back(info);
        }
    }
    closedir(procDir);
#else
    vector<string> fakeNames = {"WindowServer", "Terminal", "Browser", "Music", "Code", "Database", "Mail", "System"};
    for (int i = 0; i < (int)fakeNames.size(); i++) {
        ProcessInfo p;
        p.pid = 1000 + i;
        p.name = fakeNames[i];
        p.cpuPercent = 2.0 + 20.0 * fabs(sin((SDL_GetTicks() / 900.0) + i));
        p.memoryKB = (80 + i * 55) * 1024;
        processes.push_back(p);
    }
#endif

    return processes;
}

SystemStats readSystemStats() {
    SystemStats stats;
    CpuSample currentCpu = readCpuSample();
    stats.cpuPercent = calculateCpuPercent(currentCpu);
    readMemoryStats(stats);
    readDiskStats(stats);
    return stats;
}

string findFontPath() {
    vector<string> candidates = {
        "assets/font.ttf",
        "/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf",
        "/usr/share/fonts/truetype/liberation2/LiberationSans-Regular.ttf",
        "/usr/share/fonts/truetype/freefont/FreeSans.ttf",
        "/System/Library/Fonts/Supplemental/Arial.ttf",
        "/Library/Fonts/Arial.ttf"
    };

    for (const string& path : candidates) {
        if (access(path.c_str(), R_OK) == 0) {
            return path;
        }
    }
    return "";
}

void setColor(SDL_Renderer* renderer, SDL_Color color) {
    SDL_SetRenderDrawColor(renderer, color.r, color.g, color.b, color.a);
}

void drawFilledRect(SDL_Renderer* renderer, const SDL_Rect& rect, SDL_Color color) {
    setColor(renderer, color);
    SDL_RenderFillRect(renderer, &rect);
}

void drawOutlineRect(SDL_Renderer* renderer, const SDL_Rect& rect, SDL_Color color) {
    setColor(renderer, color);
    SDL_RenderDrawRect(renderer, &rect);
}

void drawLine(SDL_Renderer* renderer, int x1, int y1, int x2, int y2, SDL_Color color) {
    setColor(renderer, color);
    SDL_RenderDrawLine(renderer, x1, y1, x2, y2);
}

void drawText(SDL_Renderer* renderer, TTF_Font* font, const string& text, int x, int y, SDL_Color color) {
    if (!font || text.empty()) return;

    SDL_Surface* surface = TTF_RenderUTF8_Blended(font, text.c_str(), color);
    if (!surface) return;

    SDL_Texture* texture = SDL_CreateTextureFromSurface(renderer, surface);
    if (!texture) {
        SDL_FreeSurface(surface);
        return;
    }

    SDL_Rect dst = {x, y, surface->w, surface->h};
    SDL_RenderCopy(renderer, texture, nullptr, &dst);

    SDL_DestroyTexture(texture);
    SDL_FreeSurface(surface);
}

void drawBar(SDL_Renderer* renderer, TTF_Font* font, int x, int y, int w, int h,
             const string& label, double percent, SDL_Color fillColor) {
    SDL_Color textColor = {235, 239, 245, 255};
    SDL_Color mutedText = {160, 169, 181, 255};
    SDL_Color panel = {28, 35, 47, 255};
    SDL_Color border = {63, 74, 91, 255};

    drawText(renderer, font, label, x, y - 25, textColor);
    drawText(renderer, font, formatPercent(percent), x + w - 70, y - 25, mutedText);

    SDL_Rect background = {x, y, w, h};
    drawFilledRect(renderer, background, panel);
    drawOutlineRect(renderer, background, border);

    int fillWidth = (int)(w * clamp(percent, 0.0, 100.0) / 100.0);
    SDL_Rect fill = {x, y, fillWidth, h};
    drawFilledRect(renderer, fill, fillColor);
}

void drawButton(SDL_Renderer* renderer, TTF_Font* font, const Button& button, bool active) {
    SDL_Color buttonColor = active ? SDL_Color{62, 118, 255, 255} : SDL_Color{35, 43, 58, 255};
    SDL_Color borderColor = active ? SDL_Color{125, 161, 255, 255} : SDL_Color{70, 82, 100, 255};
    SDL_Color textColor = {240, 244, 250, 255};

    drawFilledRect(renderer, button.rect, buttonColor);
    drawOutlineRect(renderer, button.rect, borderColor);
    drawText(renderer, font, button.label, button.rect.x + 13, button.rect.y + 9, textColor);
}

void drawIconFallback(SDL_Renderer* renderer, int x, int y) {
    SDL_Rect outer = {x, y, 56, 56};
    SDL_Rect middle = {x + 10, y + 10, 36, 36};
    SDL_Rect inner = {x + 22, y + 22, 12, 12};

    drawFilledRect(renderer, outer, SDL_Color{58, 123, 255, 255});
    drawFilledRect(renderer, middle, SDL_Color{18, 24, 34, 255});
    drawFilledRect(renderer, inner, SDL_Color{93, 220, 255, 255});

    for (int i = 0; i < 4; i++) {
        drawLine(renderer, x + 8 + i * 12, y - 4, x + 8 + i * 12, y + 5, SDL_Color{93, 220, 255, 255});
        drawLine(renderer, x + 8 + i * 12, y + 51, x + 8 + i * 12, y + 60, SDL_Color{93, 220, 255, 255});
        drawLine(renderer, x - 4, y + 8 + i * 12, x + 5, y + 8 + i * 12, SDL_Color{93, 220, 255, 255});
        drawLine(renderer, x + 51, y + 8 + i * 12, x + 60, y + 8 + i * 12, SDL_Color{93, 220, 255, 255});
    }
}

SDL_Texture* loadIconTexture(SDL_Renderer* renderer) {
    SDL_Surface* surface = IMG_Load("assets/system_icon.bmp");
    if (!surface) {
        cerr << "Could not load assets/system_icon.bmp. Using fallback icon. SDL_image: " << IMG_GetError() << endl;
        return nullptr;
    }

    SDL_Texture* texture = SDL_CreateTextureFromSurface(renderer, surface);
    SDL_FreeSurface(surface);
    return texture;
}

void sortProcesses(vector<ProcessInfo>& processes, SortMode mode) {
    if (mode == SortMode::CPU) {
        sort(processes.begin(), processes.end(), [](const ProcessInfo& a, const ProcessInfo& b) {
            return a.cpuPercent > b.cpuPercent;
        });
    } else if (mode == SortMode::MEMORY) {
        sort(processes.begin(), processes.end(), [](const ProcessInfo& a, const ProcessInfo& b) {
            return a.memoryKB > b.memoryKB;
        });
    } else {
        sort(processes.begin(), processes.end(), [](const ProcessInfo& a, const ProcessInfo& b) {
            return a.name < b.name;
        });
    }
}

string sortModeName(SortMode mode) {
    if (mode == SortMode::CPU) return "CPU";
    if (mode == SortMode::MEMORY) return "Memory";
    return "Name";
}

bool pointInRect(int x, int y, const SDL_Rect& rect) {
    return x >= rect.x && x <= rect.x + rect.w && y >= rect.y && y <= rect.y + rect.h;
}

void drawProcessTable(SDL_Renderer* renderer, TTF_Font* font, TTF_Font* smallFont,
                      const vector<ProcessInfo>& processes, int scrollOffset,
                      int selectedPid, SortMode sortMode) {
    SDL_Color panel = {22, 28, 39, 255};
    SDL_Color header = {33, 41, 55, 255};
    SDL_Color border = {65, 77, 96, 255};
    SDL_Color text = {235, 239, 245, 255};
    SDL_Color muted = {160, 169, 181, 255};
    SDL_Color line = {45, 55, 72, 255};
    SDL_Color selected = {42, 77, 135, 255};

    SDL_Rect table = {40, 300, 1020, 350};
    drawFilledRect(renderer, table, panel);
    drawOutlineRect(renderer, table, border);

    SDL_Rect tableHeader = {40, 300, 1020, 42};
    drawFilledRect(renderer, tableHeader, header);

    drawText(renderer, font, "Running Processes", 58, 310, text);
    drawText(renderer, smallFont, "Sorted by: " + sortModeName(sortMode), 860, 314, muted);

    drawText(renderer, smallFont, "PID", 60, 355, muted);
    drawText(renderer, smallFont, "Process Name", 150, 355, muted);
    drawText(renderer, smallFont, "CPU", 610, 355, muted);
    drawText(renderer, smallFont, "Memory", 760, 355, muted);

    drawLine(renderer, 55, 380, 1045, 380, line);

    const int rowHeight = 28;
    const int firstRowY = 388;
    const int maxRows = 9;

    for (int i = 0; i < maxRows; i++) {
        int index = scrollOffset + i;
        if (index >= (int)processes.size()) break;

        const ProcessInfo& p = processes[index];
        int y = firstRowY + i * rowHeight;

        if (p.pid == selectedPid) {
            SDL_Rect selectedRow = {52, y - 3, 990, rowHeight};
            drawFilledRect(renderer, selectedRow, selected);
        }

        if (i % 2 == 1 && p.pid != selectedPid) {
            SDL_Rect altRow = {52, y - 3, 990, rowHeight};
            drawFilledRect(renderer, altRow, SDL_Color{26, 33, 45, 255});
        }

        string processName = p.name;
        if (processName.size() > 38) {
            processName = processName.substr(0, 35) + "...";
        }

        drawText(renderer, smallFont, to_string(p.pid), 60, y, text);
        drawText(renderer, smallFont, processName, 150, y, text);
        drawText(renderer, smallFont, formatPercent(p.cpuPercent), 610, y, text);
        drawText(renderer, smallFont, formatKB(p.memoryKB), 760, y, text);
    }

    string scrollText = "Scroll: " + to_string(min(scrollOffset + maxRows, (int)processes.size())) + "/" + to_string(processes.size());
    drawText(renderer, smallFont, scrollText, 58, 620, muted);
    drawText(renderer, smallFont, "Click a row to select a process. Use mouse wheel or Up/Down to scroll.", 330, 620, muted);
}

void drawFooter(SDL_Renderer* renderer, TTF_Font* smallFont, bool paused) {
    SDL_Color muted = {160, 169, 181, 255};
    SDL_Color warning = {255, 196, 87, 255};

    string controls = "Controls: C = sort CPU | M = sort Memory | N = sort Name | P = pause updates | R = reset scroll | Esc = quit";
    drawText(renderer, smallFont, controls, 40, 674, paused ? warning : muted);

    if (paused) {
        drawText(renderer, smallFont, "Updates paused", 920, 674, warning);
    }
}

int main(int argc, char* argv[]) {
    (void)argc;
    (void)argv;

    if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_TIMER) != 0) {
        cerr << "SDL_Init failed: " << SDL_GetError() << endl;
        return 1;
    }

    if (TTF_Init() != 0) {
        cerr << "TTF_Init failed: " << TTF_GetError() << endl;
        SDL_Quit();
        return 1;
    }

    int imageFlags = IMG_INIT_PNG | IMG_INIT_JPG;
    IMG_Init(imageFlags);

    SDL_Window* window = SDL_CreateWindow(
        "SDL2 OS System Monitor",
        SDL_WINDOWPOS_CENTERED,
        SDL_WINDOWPOS_CENTERED,
        WINDOW_WIDTH,
        WINDOW_HEIGHT,
        SDL_WINDOW_SHOWN
    );

    if (!window) {
        cerr << "SDL_CreateWindow failed: " << SDL_GetError() << endl;
        IMG_Quit();
        TTF_Quit();
        SDL_Quit();
        return 1;
    }

    SDL_Renderer* renderer = SDL_CreateRenderer(window, -1, SDL_RENDERER_ACCELERATED | SDL_RENDERER_PRESENTVSYNC);
    if (!renderer) {
        cerr << "SDL_CreateRenderer failed: " << SDL_GetError() << endl;
        SDL_DestroyWindow(window);
        IMG_Quit();
        TTF_Quit();
        SDL_Quit();
        return 1;
    }

    string fontPath = findFontPath();
    if (fontPath.empty()) {
        cerr << "No font found. Install DejaVu fonts or copy a .ttf to assets/font.ttf" << endl;
        SDL_DestroyRenderer(renderer);
        SDL_DestroyWindow(window);
        IMG_Quit();
        TTF_Quit();
        SDL_Quit();
        return 1;
    }

    TTF_Font* titleFont = TTF_OpenFont(fontPath.c_str(), 30);
    TTF_Font* font = TTF_OpenFont(fontPath.c_str(), 18);
    TTF_Font* smallFont = TTF_OpenFont(fontPath.c_str(), 15);

    if (!titleFont || !font || !smallFont) {
        cerr << "TTF_OpenFont failed: " << TTF_GetError() << endl;
        if (titleFont) TTF_CloseFont(titleFont);
        if (font) TTF_CloseFont(font);
        if (smallFont) TTF_CloseFont(smallFont);
        SDL_DestroyRenderer(renderer);
        SDL_DestroyWindow(window);
        IMG_Quit();
        TTF_Quit();
        SDL_Quit();
        return 1;
    }

    SDL_Texture* iconTexture = loadIconTexture(renderer);

    vector<Button> buttons = {
        {{40, 230, 135, 42}, "Sort: CPU", SortMode::CPU},
        {{190, 230, 165, 42}, "Sort: Memory", SortMode::MEMORY},
        {{370, 230, 145, 42}, "Sort: Name", SortMode::NAME}
    };

    bool running = true;
    bool paused = false;
    SortMode sortMode = SortMode::CPU;
    int scrollOffset = 0;
    int selectedPid = -1;
    string selectedName = "None";

    SystemStats stats = readSystemStats();
    vector<ProcessInfo> processes = readProcesses();
    sortProcesses(processes, sortMode);
    Uint32 lastUpdate = SDL_GetTicks();

    while (running) {
        SDL_Event event;
        while (SDL_PollEvent(&event)) {
            if (event.type == SDL_QUIT) {
                running = false;
            } else if (event.type == SDL_KEYDOWN) {
                SDL_Keycode key = event.key.keysym.sym;
                if (key == SDLK_ESCAPE) running = false;
                if (key == SDLK_c) sortMode = SortMode::CPU;
                if (key == SDLK_m) sortMode = SortMode::MEMORY;
                if (key == SDLK_n) sortMode = SortMode::NAME;
                if (key == SDLK_p) paused = !paused;
                if (key == SDLK_r) scrollOffset = 0;
                if (key == SDLK_DOWN) scrollOffset++;
                if (key == SDLK_UP) scrollOffset--;
                if (key == SDLK_PAGEDOWN) scrollOffset += 5;
                if (key == SDLK_PAGEUP) scrollOffset -= 5;
            } else if (event.type == SDL_MOUSEWHEEL) {
                scrollOffset -= event.wheel.y;
            } else if (event.type == SDL_MOUSEBUTTONDOWN) {
                int mx = event.button.x;
                int my = event.button.y;

                for (const Button& button : buttons) {
                    if (pointInRect(mx, my, button.rect)) {
                        sortMode = button.mode;
                    }
                }

                const int rowHeight = 28;
                const int firstRowY = 388;
                const int maxRows = 9;
                for (int i = 0; i < maxRows; i++) {
                    int rowY = firstRowY + i * rowHeight;
                    SDL_Rect rowRect = {52, rowY - 3, 990, rowHeight};
                    int index = scrollOffset + i;
                    if (index < (int)processes.size() && pointInRect(mx, my, rowRect)) {
                        selectedPid = processes[index].pid;
                        selectedName = processes[index].name;
                    }
                }
            }
        }

        int maxScroll = max(0, (int)processes.size() - 9);
        scrollOffset = clamp(scrollOffset, 0, maxScroll);

        Uint32 now = SDL_GetTicks();
        if (!paused && now - lastUpdate >= UPDATE_INTERVAL_MS) {
            stats = readSystemStats();
            processes = readProcesses();
            sortProcesses(processes, sortMode);
            lastUpdate = now;
        } else {
            sortProcesses(processes, sortMode);
        }

        setColor(renderer, SDL_Color{13, 18, 26, 255});
        SDL_RenderClear(renderer);

        // Header panel
        SDL_Rect header = {0, 0, WINDOW_WIDTH, 110};
        drawFilledRect(renderer, header, SDL_Color{18, 24, 34, 255});
        drawLine(renderer, 0, 110, WINDOW_WIDTH, 110, SDL_Color{48, 58, 74, 255});

        if (iconTexture) {
            SDL_Rect iconDst = {38, 27, 56, 56};
            SDL_RenderCopy(renderer, iconTexture, nullptr, &iconDst);
        } else {
            drawIconFallback(renderer, 38, 27);
        }

        drawText(renderer, titleFont, "OS System Monitor", 112, 24, SDL_Color{245, 248, 252, 255});
        drawText(renderer, font, "C++ / SDL2 visual interface for CPU, memory, disk, and process activity", 114, 64, SDL_Color{166, 176, 190, 255});

        // Summary cards
        SDL_Rect card1 = {40, 140, 320, 65};
        SDL_Rect card2 = {390, 140, 320, 65};
        SDL_Rect card3 = {740, 140, 320, 65};
        drawFilledRect(renderer, card1, SDL_Color{22, 28, 39, 255});
        drawFilledRect(renderer, card2, SDL_Color{22, 28, 39, 255});
        drawFilledRect(renderer, card3, SDL_Color{22, 28, 39, 255});
        drawOutlineRect(renderer, card1, SDL_Color{63, 74, 91, 255});
        drawOutlineRect(renderer, card2, SDL_Color{63, 74, 91, 255});
        drawOutlineRect(renderer, card3, SDL_Color{63, 74, 91, 255});

        drawText(renderer, smallFont, "Selected Process", 60, 150, SDL_Color{160, 169, 181, 255});
        string selectedText = selectedPid == -1 ? "None" : to_string(selectedPid) + " - " + selectedName;
        if (selectedText.size() > 28) selectedText = selectedText.substr(0, 25) + "...";
        drawText(renderer, font, selectedText, 60, 174, SDL_Color{235, 239, 245, 255});

        drawText(renderer, smallFont, "Update Interval", 410, 150, SDL_Color{160, 169, 181, 255});
        drawText(renderer, font, paused ? "Paused" : "Every 500ms", 410, 174, SDL_Color{235, 239, 245, 255});

        drawText(renderer, smallFont, "Processes Found", 760, 150, SDL_Color{160, 169, 181, 255});
        drawText(renderer, font, to_string(processes.size()), 760, 174, SDL_Color{235, 239, 245, 255});

        // Buttons
        for (const Button& button : buttons) {
            drawButton(renderer, font, button, button.mode == sortMode);
        }
        drawText(renderer, smallFont, "Tip: click buttons or use C / M / N keys", 540, 243, SDL_Color{160, 169, 181, 255});

        // Bars
        drawBar(renderer, font, 40, 94, 260, 14, "CPU", stats.cpuPercent, SDL_Color{88, 166, 255, 255});
        drawBar(renderer, font, 320, 94, 260, 14, "Memory", stats.memoryPercent, SDL_Color{126, 231, 135, 255});
        drawBar(renderer, font, 600, 94, 260, 14, "Disk", stats.diskPercent, SDL_Color{255, 184, 87, 255});

        drawText(renderer, smallFont, formatKB(stats.memoryUsedKB) + " / " + formatKB(stats.memoryTotalKB), 320, 112, SDL_Color{160, 169, 181, 255});
        drawText(renderer, smallFont, formatBytes(stats.diskUsedBytes) + " / " + formatBytes(stats.diskTotalBytes), 600, 112, SDL_Color{160, 169, 181, 255});

        drawProcessTable(renderer, font, smallFont, processes, scrollOffset, selectedPid, sortMode);
        drawFooter(renderer, smallFont, paused);

        SDL_RenderPresent(renderer);
    }

    if (iconTexture) SDL_DestroyTexture(iconTexture);
    TTF_CloseFont(titleFont);
    TTF_CloseFont(font);
    TTF_CloseFont(smallFont);
    SDL_DestroyRenderer(renderer);
    SDL_DestroyWindow(window);
    IMG_Quit();
    TTF_Quit();
    SDL_Quit();

    return 0;
}
