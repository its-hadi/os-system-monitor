#include <SDL2/SDL.h>
#include <SDL2/SDL_ttf.h>
#include <SDL2/SDL_image.h>

#include <algorithm>
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
const int WINDOW_HEIGHT = 820;
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
    while (ss >> token) tokens.push_back(token);
    return tokens;
}

bool isNumberString(const string& text) {
    if (text.empty()) return false;
    for (char c : text) {
        if (!isdigit((unsigned char)c)) return false;
    }
    return true;
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
        if (total > 0) stats.diskPercent = 100.0 * (double)used / (double)total;
    }
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
    if (openParen == string::npos || closeParen == string::npos || closeParen <= openParen) return false;

    string name = line.substr(openParen + 1, closeParen - openParen - 1);
    string rest = line.substr(closeParen + 2);
    vector<string> fields = splitWhitespace(rest);

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
        if (readProcessFromProc(pid, info)) processes.push_back(info);
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
        if (access(path.c_str(), R_OK) == 0) return path;
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

void drawGradientBackground(SDL_Renderer* renderer) {
    for (int y = 0; y < WINDOW_HEIGHT; y++) {
        Uint8 r = 10 + (Uint8)(y / 90);
        Uint8 g = 14 + (Uint8)(y / 130);
        Uint8 b = 22 + (Uint8)(y / 85);
        SDL_SetRenderDrawColor(renderer, r, g, b, 255);
        SDL_RenderDrawLine(renderer, 0, y, WINDOW_WIDTH, y);
    }
}

void drawSoftPanel(SDL_Renderer* renderer, const SDL_Rect& rect, SDL_Color fill, SDL_Color border) {
    SDL_Rect shadow = {rect.x + 3, rect.y + 4, rect.w, rect.h};
    drawFilledRect(renderer, shadow, SDL_Color{5, 8, 14, 120});
    drawFilledRect(renderer, rect, fill);
    drawOutlineRect(renderer, rect, border);
}

void drawChipIcon(SDL_Renderer* renderer, int x, int y, SDL_Color accent) {
    SDL_Rect outer = {x, y, 44, 44};
    SDL_Rect middle = {x + 8, y + 8, 28, 28};
    SDL_Rect core = {x + 18, y + 18, 8, 8};

    drawFilledRect(renderer, outer, accent);
    drawFilledRect(renderer, middle, SDL_Color{13, 18, 26, 255});
    drawFilledRect(renderer, core, SDL_Color{160, 230, 255, 255});

    for (int i = 0; i < 4; i++) {
        int offset = 7 + i * 10;
        drawLine(renderer, x + offset, y - 4, x + offset, y + 4, accent);
        drawLine(renderer, x + offset, y + 40, x + offset, y + 48, accent);
        drawLine(renderer, x - 4, y + offset, x + 4, y + offset, accent);
        drawLine(renderer, x + 40, y + offset, x + 48, y + offset, accent);
    }
}

void drawMemoryIcon(SDL_Renderer* renderer, int x, int y, SDL_Color accent) {
    SDL_Rect body = {x, y + 8, 48, 28};
    SDL_Rect cut = {x + 8, y + 14, 32, 8};

    drawFilledRect(renderer, body, accent);
    drawFilledRect(renderer, cut, SDL_Color{13, 18, 26, 255});

    for (int i = 0; i < 5; i++) {
        int pinX = x + 5 + i * 9;
        drawLine(renderer, pinX, y + 36, pinX, y + 44, accent);
    }
}

void drawDiskIcon(SDL_Renderer* renderer, int x, int y, SDL_Color accent) {
    SDL_Rect body = {x, y + 4, 48, 38};
    SDL_Rect slot = {x + 11, y + 11, 26, 7};
    SDL_Rect light = {x + 34, y + 30, 6, 6};

    drawFilledRect(renderer, body, accent);
    drawFilledRect(renderer, slot, SDL_Color{13, 18, 26, 255});
    drawFilledRect(renderer, light, SDL_Color{13, 18, 26, 255});
}

void drawBar(SDL_Renderer* renderer, int x, int y, int w, int h, double percent, SDL_Color fillColor) {
    SDL_Color background = {23, 30, 42, 255};
    SDL_Color border = {61, 73, 92, 255};

    SDL_Rect bg = {x, y, w, h};
    drawFilledRect(renderer, bg, background);
    drawOutlineRect(renderer, bg, border);

    int fillWidth = (int)(w * clamp(percent, 0.0, 100.0) / 100.0);
    SDL_Rect fill = {x, y, fillWidth, h};
    drawFilledRect(renderer, fill, fillColor);
}

void drawMetricCard(SDL_Renderer* renderer, TTF_Font* font, TTF_Font* smallFont,
                    int x, int y, const string& title, const string& detail,
                    double percent, SDL_Color accent, int iconType) {
    SDL_Rect card = {x, y, 320, 108};
    drawSoftPanel(renderer, card, SDL_Color{18, 24, 35, 255}, SDL_Color{58, 70, 90, 255});

    if (iconType == 0) drawChipIcon(renderer, x + 24, y + 31, accent);
    if (iconType == 1) drawMemoryIcon(renderer, x + 22, y + 31, accent);
    if (iconType == 2) drawDiskIcon(renderer, x + 23, y + 31, accent);

    drawText(renderer, font, title, x + 88, y + 22, SDL_Color{222, 229, 239, 255});
    drawText(renderer, smallFont, formatPercent(percent), x + 238, y + 25, SDL_Color{166, 176, 190, 255});

    drawBar(renderer, x + 88, y + 56, 205, 13, percent, accent);
    drawText(renderer, smallFont, detail, x + 88, y + 77, SDL_Color{147, 158, 174, 255});
}

void drawButton(SDL_Renderer* renderer, TTF_Font* font, const Button& button, bool active) {
    SDL_Color buttonColor = active ? SDL_Color{66, 123, 255, 255} : SDL_Color{30, 38, 53, 255};
    SDL_Color borderColor = active ? SDL_Color{136, 171, 255, 255} : SDL_Color{66, 78, 99, 255};
    SDL_Color textColor = {240, 244, 250, 255};

    drawFilledRect(renderer, button.rect, buttonColor);
    drawOutlineRect(renderer, button.rect, borderColor);
    drawText(renderer, font, button.label, button.rect.x + 15, button.rect.y + 10, textColor);
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

string shorten(const string& text, size_t maxLen) {
    if (text.size() <= maxLen) return text;
    if (maxLen <= 3) return text.substr(0, maxLen);
    return text.substr(0, maxLen - 3) + "...";
}

void drawSummaryCard(SDL_Renderer* renderer, TTF_Font* font, TTF_Font* smallFont,
                     int x, int y, const string& label, const string& value,
                     SDL_Texture* iconTexture, SDL_Color accent) {
    SDL_Rect card = {x, y, 320, 82};
    drawSoftPanel(renderer, card, SDL_Color{18, 24, 35, 255}, SDL_Color{58, 70, 90, 255});

    drawText(renderer, smallFont, label, x + 22, y + 17, SDL_Color{147, 158, 174, 255});
    drawText(renderer, font, shorten(value, 23), x + 22, y + 44, SDL_Color{235, 239, 245, 255});

    SDL_Rect badge = {x + 262, y + 21, 38, 38};
    drawFilledRect(renderer, badge, SDL_Color{27, 35, 49, 255});
    drawOutlineRect(renderer, badge, accent);

    if (iconTexture) {
        SDL_Rect icon = {x + 269, y + 28, 24, 24};
        SDL_RenderCopy(renderer, iconTexture, nullptr, &icon);
    } else {
        SDL_Rect dot = {x + 276, y + 35, 10, 10};
        drawFilledRect(renderer, dot, accent);
    }
}

void drawProcessTable(SDL_Renderer* renderer, TTF_Font* font, TTF_Font* smallFont,
                      const vector<ProcessInfo>& processes, int scrollOffset,
                      int selectedPid, SortMode sortMode) {
    SDL_Color panel = {18, 24, 35, 255};
    SDL_Color header = {30, 38, 53, 255};
    SDL_Color border = {58, 70, 90, 255};
    SDL_Color text = {235, 239, 245, 255};
    SDL_Color muted = {147, 158, 174, 255};
    SDL_Color line = {45, 55, 72, 255};
    SDL_Color selected = {43, 82, 145, 255};

    SDL_Rect table = {40, 470, 1020, 286};
    drawSoftPanel(renderer, table, panel, border);

    SDL_Rect tableHeader = {40, 470, 1020, 46};
    drawFilledRect(renderer, tableHeader, header);

    drawText(renderer, font, "Running Processes", 58, 482, text);
    drawText(renderer, smallFont, "Sorted by: " + sortModeName(sortMode), 850, 486, muted);

    drawText(renderer, smallFont, "PID", 60, 532, muted);
    drawText(renderer, smallFont, "Process Name", 150, 532, muted);
    drawText(renderer, smallFont, "CPU", 610, 532, muted);
    drawText(renderer, smallFont, "Memory", 760, 532, muted);

    drawLine(renderer, 55, 557, 1045, 557, line);

    const int rowHeight = 27;
    const int firstRowY = 566;
    const int maxRows = 6;

    for (int i = 0; i < maxRows; i++) {
        int index = scrollOffset + i;
        if (index >= (int)processes.size()) break;

        const ProcessInfo& p = processes[index];
        int y = firstRowY + i * rowHeight;

        if (p.pid == selectedPid) {
            SDL_Rect selectedRow = {52, y - 3, 990, rowHeight};
            drawFilledRect(renderer, selectedRow, selected);
        } else if (i % 2 == 1) {
            SDL_Rect altRow = {52, y - 3, 990, rowHeight};
            drawFilledRect(renderer, altRow, SDL_Color{22, 30, 43, 255});
        }

        drawText(renderer, smallFont, to_string(p.pid), 60, y, text);
        drawText(renderer, smallFont, shorten(p.name, 38), 150, y, text);
        drawText(renderer, smallFont, formatPercent(p.cpuPercent), 610, y, text);
        drawText(renderer, smallFont, formatKB(p.memoryKB), 760, y, text);
    }

    string scrollText = "Showing: " + to_string(min(scrollOffset + maxRows, (int)processes.size())) + " / " + to_string(processes.size());
    drawText(renderer, smallFont, scrollText, 58, 727, muted);
    drawText(renderer, smallFont, "Click a row to select. Mouse wheel or Up/Down scrolls.", 360, 727, muted);
}

void drawFooter(SDL_Renderer* renderer, TTF_Font* smallFont, bool paused) {
    SDL_Color muted = {147, 158, 174, 255};
    SDL_Color warning = {255, 196, 87, 255};

    string controls = "Controls: C = CPU | M = Memory | N = Name | P = pause updates | R = reset scroll | Esc = quit";
    drawText(renderer, smallFont, controls, 40, 786, paused ? warning : muted);

    if (paused) drawText(renderer, smallFont, "Updates paused", 930, 786, warning);
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
        {{40, 405, 150, 44}, "Sort: CPU", SortMode::CPU},
        {{210, 405, 180, 44}, "Sort: Memory", SortMode::MEMORY},
        {{410, 405, 160, 44}, "Sort: Name", SortMode::NAME}
    };

    bool running = true;
    bool paused = false;
    SortMode sortMode = SortMode::MEMORY;
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
                    if (pointInRect(mx, my, button.rect)) sortMode = button.mode;
                }

                const int rowHeight = 27;
                const int firstRowY = 566;
                const int maxRows = 6;
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

        int maxScroll = max(0, (int)processes.size() - 6);
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

        drawGradientBackground(renderer);

        SDL_Rect header = {0, 0, WINDOW_WIDTH, 128};
        drawFilledRect(renderer, header, SDL_Color{14, 20, 31, 255});
        drawLine(renderer, 0, 128, WINDOW_WIDTH, 128, SDL_Color{49, 61, 80, 255});

        if (iconTexture) {
            SDL_Rect iconDst = {40, 32, 56, 56};
            SDL_RenderCopy(renderer, iconTexture, nullptr, &iconDst);
        } else {
            drawIconFallback(renderer, 40, 32);
        }

        drawText(renderer, titleFont, "OS System Monitor", 120, 27, SDL_Color{245, 248, 252, 255});
        drawText(renderer, font, "Live C++ / SDL2 dashboard for CPU, memory, disk, and Linux process activity", 122, 72, SDL_Color{166, 176, 190, 255});

        SDL_Rect liveDot = {1008, 45, 10, 10};
        drawFilledRect(renderer, liveDot, paused ? SDL_Color{255, 196, 87, 255} : SDL_Color{126, 231, 135, 255});
        drawText(renderer, smallFont, paused ? "PAUSED" : "LIVE", 1024, 39, paused ? SDL_Color{255, 196, 87, 255} : SDL_Color{126, 231, 135, 255});

        string selectedText = selectedPid == -1 ? "None" : to_string(selectedPid) + " - " + selectedName;
        drawSummaryCard(renderer, font, smallFont, 40, 150, "Selected Process", selectedText, iconTexture, SDL_Color{88, 166, 255, 255});
        drawSummaryCard(renderer, font, smallFont, 390, 150, "Update Interval", paused ? "Paused" : "Every 500ms", iconTexture, SDL_Color{126, 231, 135, 255});
        drawSummaryCard(renderer, font, smallFont, 740, 150, "Processes Found", to_string(processes.size()), iconTexture, SDL_Color{255, 184, 87, 255});

        drawMetricCard(renderer, font, smallFont, 40, 255, "CPU Usage", "Total processor activity", stats.cpuPercent, SDL_Color{88, 166, 255, 255}, 0);
        drawMetricCard(renderer, font, smallFont, 390, 255, "Memory Usage", formatKB(stats.memoryUsedKB) + " / " + formatKB(stats.memoryTotalKB), stats.memoryPercent, SDL_Color{126, 231, 135, 255}, 1);
        drawMetricCard(renderer, font, smallFont, 740, 255, "Disk Usage", formatBytes(stats.diskUsedBytes) + " / " + formatBytes(stats.diskTotalBytes), stats.diskPercent, SDL_Color{255, 184, 87, 255}, 2);

        for (const Button& button : buttons) {
            drawButton(renderer, font, button, button.mode == sortMode);
        }
        drawText(renderer, smallFont, "Tip: press C / M / N to sort. Default is Memory so the table stays calmer.", 610, 419, SDL_Color{147, 158, 174, 255});

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
