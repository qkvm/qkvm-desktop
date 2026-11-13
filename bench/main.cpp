// The measuring rig: the engine in a bare window, with nothing of Qt, of a server or of a device.
// Its pictures are the ones tools/make_testdata.ps1 makes. What it measures goes into a report file,
// one line every few seconds and a summary at the end (SPEC.md, step M1).

#include <algorithm>
#include <array>
#include <atomic>
#include <charconv>
#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <exception>
#include <filesystem>
#include <fstream>
#include <map>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#include <windows.h>

#include <qkvm/base/clock.h>
#include <qkvm/engine/gpu.h>
#include <qkvm/engine/scene.h>

#include "clips.h"
#include "feeder.h"
#include "meter.h"

namespace qkvm::bench {
namespace {

constexpr const char* usage =
    "qkvm-bench: the video engine in a bare window, measured.\n"
    "\n"
    "  --data FOLDER        where tools\\make_testdata.ps1 put the pictures\n"
    "  --source NAME        a file (pattern-720p15.h264) or a folder of JPEGs (desktop-preview-480) in it;\n"
    "                       given several times, the tiles take them in turn\n"
    "  --tiles N            how many tiles (16)\n"
    "  --decoding HOW       auto, card or software (auto)\n"
    "  --large N            how many of the tiles count as shown large, which with auto puts them on the card (0)\n"
    "  --card-decoders N    how many streams the card may decode at once (16)\n"
    "  --at-once            show each picture when it is there instead of waiting for the monitor\n"
    "  --seconds N          how long to run; 0 until the window is closed (20)\n"
    "  --every N            seconds between two lines of the report (1)\n"
    "  --size WxH           the window's inside, in pixels (1600x900)\n"
    "  --at X,Y             where the window is put\n"
    "  --borderless         no frame around it\n"
    "  --adapter N          which graphics adapter (the one Windows chooses)\n"
    "  --software-adapter   Windows' software adapter instead of a graphics card\n"
    "  --lose-device N      after N seconds, treat the device as taken away by Windows, once\n"
    "  --report FILE        where the lines and the summary go (they are printed when this is left out)\n"
    "  --adapters           list the graphics adapters and end\n";

struct Options {
    std::filesystem::path data;
    std::vector<std::string> sources;
    int tiles = 16;
    engine::DecodingChoice decoding = engine::DecodingChoice::Automatic;
    int large = 0;
    int cardDecoders = 16;
    bool atOnce = false;
    int seconds = 20;
    int every = 1;
    int width = 1600;
    int height = 900;
    bool placed = false;
    int x = 0;
    int y = 0;
    bool borderless = false;
    int adapter = -1;
    bool softwareAdapter = false;
    int loseDeviceAfter = 0;
    std::filesystem::path report;
    bool listAdapters = false;
};

bool number(std::string_view text, int& value)
{
    const auto [end, error] = std::from_chars(text.data(), text.data() + text.size(), value);
    return error == std::errc{} && end == text.data() + text.size();
}

// "1600x900" or "40,60": two numbers with something between them.
bool pair(std::string_view text, char between, int& first, int& second)
{
    const std::size_t at = text.find(between);
    return at != std::string_view::npos && number(text.substr(0, at), first) && number(text.substr(at + 1), second);
}

bool read(int count, char** arguments, Options& options)
{
    for (int index = 1; index < count; ++index) {
        const std::string_view name = arguments[index];
        const auto value = [&]() -> std::string_view {
            return index + 1 < count ? arguments[++index] : std::string_view{};
        };
        bool understood = true;
        if (name == "--data") {
            options.data = value();
        } else if (name == "--source") {
            options.sources.emplace_back(value());
        } else if (name == "--tiles") {
            understood = number(value(), options.tiles) && options.tiles >= 1 && options.tiles <= 256;
        } else if (name == "--decoding") {
            const std::string_view how = value();
            understood = how == "auto" || how == "card" || how == "software";
            options.decoding = how == "card"       ? engine::DecodingChoice::GraphicsCard
                               : how == "software" ? engine::DecodingChoice::Software
                                                   : engine::DecodingChoice::Automatic;
        } else if (name == "--large") {
            understood = number(value(), options.large) && options.large >= 0;
        } else if (name == "--card-decoders") {
            understood = number(value(), options.cardDecoders) && options.cardDecoders >= 0;
        } else if (name == "--at-once") {
            options.atOnce = true;
        } else if (name == "--seconds") {
            understood = number(value(), options.seconds) && options.seconds >= 0;
        } else if (name == "--every") {
            understood = number(value(), options.every) && options.every >= 1;
        } else if (name == "--size") {
            understood = pair(value(), 'x', options.width, options.height) && options.width >= 64 &&
                         options.height >= 64 && options.width <= 16384 && options.height <= 16384;
        } else if (name == "--at") {
            understood = pair(value(), ',', options.x, options.y);
            options.placed = understood;
        } else if (name == "--borderless") {
            options.borderless = true;
        } else if (name == "--adapter") {
            understood = number(value(), options.adapter) && options.adapter >= 0;
        } else if (name == "--software-adapter") {
            options.softwareAdapter = true;
        } else if (name == "--lose-device") {
            understood = number(value(), options.loseDeviceAfter) && options.loseDeviceAfter >= 1;
        } else if (name == "--report") {
            options.report = value();
        } else if (name == "--adapters") {
            options.listAdapters = true;
        } else {
            understood = false;
        }
        if (!understood) {
            std::fprintf(stderr, "Not understood: %s\n\n%s", arguments[index], usage);
            return false;
        }
    }
    return true;
}

// Where the lines go: the report file, or the screen.
class Report {
public:
    explicit Report(const std::filesystem::path& file)
    {
        if (!file.empty()) {
            m_file.open(file, std::ios::out | std::ios::trunc);
        }
    }

    void line(const std::string& text)
    {
        if (m_file.is_open()) {
            m_file << text << '\n';
            m_file.flush(); // written out at once: a run that is cut short keeps what it had
        } else {
            std::puts(text.c_str());
            std::fflush(stdout);
        }
    }

private:
    std::ofstream m_file;
};

std::string formatted(const char* format, ...)
{
    char text[1024] = {};
    va_list arguments;
    va_start(arguments, format);
    std::vsnprintf(text, sizeof(text), format, arguments);
    va_end(arguments);
    return text;
}

// The tiles in a grid that fills the window, a few pixels apart.
std::vector<engine::Placed> grid(int tiles, engine::SizePx window)
{
    const int columns = static_cast<int>(std::ceil(std::sqrt(static_cast<double>(tiles))));
    const int rows = (tiles + columns - 1) / columns;
    const int gap = 4;
    const int width = (window.width - gap * (columns + 1)) / columns;
    const int height = (window.height - gap * (rows + 1)) / rows;
    std::vector<engine::Placed> placed;
    for (int index = 0; index < tiles; ++index) {
        const int column = index % columns;
        const int row = index / columns;
        placed.push_back(
            {static_cast<engine::StreamId>(index + 1),
             {gap + column * (width + gap), gap + row * (height + gap), std::max(width, 1), std::max(height, 1)}});
    }
    return placed;
}

// The sum of every stream's counts, and the means of their times weighted alike.
struct Totals {
    std::uint64_t pushed = 0;
    std::uint64_t droppedBehind = 0;
    std::uint64_t refused = 0;
    std::uint64_t decoded = 0;
    std::uint64_t replaced = 0;
    std::uint64_t shown = 0;
    int onTheCard = 0;
    int inSoftware = 0;
    double decodeMs = 0;
    double uploadMs = 0;
    double arrivedToShownMs = 0;
    double arrivedToShownMaxMs = 0;
    std::array<std::uint64_t, 8> arrivedToShownCounts{};
};

Totals totalsOf(const std::vector<engine::Stream*>& streams)
{
    Totals totals;
    int decoding = 0;
    int uploading = 0;
    int showing = 0;
    for (const engine::Stream* stream : streams) {
        const engine::Stats stats = stream->stats();
        totals.pushed += stats.pushed;
        totals.droppedBehind += stats.droppedBehind;
        totals.refused += stats.refused;
        totals.decoded += stats.decoded;
        totals.replaced += stats.replaced;
        totals.shown += stats.shown;
        totals.onTheCard += stats.decoding == engine::Decoding::OnTheCard ? 1 : 0;
        totals.inSoftware += stats.decoding == engine::Decoding::InSoftware ? 1 : 0;
        if (stats.decodeMs > 0) {
            totals.decodeMs += stats.decodeMs;
            ++decoding;
        }
        if (stats.uploadMs > 0) {
            totals.uploadMs += stats.uploadMs;
            ++uploading;
        }
        if (stats.arrivedToShownMs > 0) {
            totals.arrivedToShownMs += stats.arrivedToShownMs;
            ++showing;
        }
        totals.arrivedToShownMaxMs = std::max(totals.arrivedToShownMaxMs, stats.arrivedToShownMaxMs);
        for (std::size_t place = 0; place < totals.arrivedToShownCounts.size(); ++place) {
            totals.arrivedToShownCounts[place] += stats.arrivedToShownCounts[place];
        }
    }
    totals.decodeMs /= std::max(decoding, 1);
    totals.uploadMs /= std::max(uploading, 1);
    totals.arrivedToShownMs /= std::max(showing, 1);
    return totals;
}

// What the window's procedure tells the loop.
struct WindowState {
    std::atomic<bool> closed{false};
    std::atomic<bool> resized{false};
    engine::SizePx size;
    HANDLE somethingNew = nullptr;
};

LRESULT CALLBACK windowProcedure(HWND window, UINT message, WPARAM wide, LPARAM longer)
{
    auto* state = reinterpret_cast<WindowState*>(GetWindowLongPtrW(window, GWLP_USERDATA));
    switch (message) {
    case WM_SIZE:
        if (state != nullptr && wide != SIZE_MINIMIZED) {
            state->size = {LOWORD(longer), HIWORD(longer)};
            state->resized.store(true);
            SetEvent(state->somethingNew);
        }
        return 0;
    case WM_KEYDOWN:
        if (wide == VK_ESCAPE) {
            PostMessageW(window, WM_CLOSE, 0, 0);
        }
        return 0;
    case WM_CLOSE:
        if (state != nullptr) {
            state->closed.store(true);
            SetEvent(state->somethingNew);
        }
        return 0;
    default:
        return DefWindowProcW(window, message, wide, longer);
    }
}

HWND makeWindow(const Options& options, WindowState& state)
{
    const HINSTANCE instance = GetModuleHandleW(nullptr);
    WNDCLASSEXW kind{};
    kind.cbSize = sizeof(kind);
    kind.lpfnWndProc = windowProcedure;
    kind.hInstance = instance;
    kind.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    kind.lpszClassName = L"qkvm-bench";
    RegisterClassExW(&kind);

    const DWORD style = options.borderless ? WS_POPUP : WS_OVERLAPPEDWINDOW;
    RECT outside{0, 0, options.width, options.height};
    AdjustWindowRectEx(&outside, style, FALSE, 0);
    const HWND window =
        CreateWindowExW(0, kind.lpszClassName, L"qkvm-bench", style, options.placed ? options.x : CW_USEDEFAULT,
                        options.placed ? options.y : CW_USEDEFAULT, outside.right - outside.left,
                        outside.bottom - outside.top, nullptr, nullptr, instance, nullptr);
    if (window != nullptr) {
        SetWindowLongPtrW(window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(&state));
        // Shown without taking the keyboard from whoever is working on this PC.
        ShowWindow(window, SW_SHOWNOACTIVATE);
        RECT inside{};
        GetClientRect(window, &inside);
        state.size = {inside.right, inside.bottom};
    }
    return window;
}

int run(const Options& options)
{
    if (options.listAdapters) {
        int index = 0;
        for (const std::string& name : engine::Gpu::adapters()) {
            std::printf("%d  %s\n", index++, name.c_str());
        }
        return 0;
    }
    if (options.data.empty() || options.sources.empty()) {
        std::fputs(usage, stderr);
        return 2;
    }

    std::map<std::string, Clip> clips;
    for (const std::string& name : options.sources) {
        if (!clips.contains(name)) {
            base::Result<Clip> clip = readClip(options.data, name);
            if (!clip) {
                std::fprintf(stderr, "%s\n", clip.error().message.c_str());
                return 1;
            }
            clips.emplace(name, std::move(clip).value());
        }
    }

    // Sizes are in the monitor's own pixels, whatever scale Windows is set to.
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    WindowState state;
    state.somethingNew = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    const HWND window = makeWindow(options, state);
    if (window == nullptr || state.somethingNew == nullptr) {
        std::fputs("The window could not be made.\n", stderr);
        return 1;
    }

    engine::Gpu::Options gpuOptions;
    gpuOptions.software = options.softwareAdapter;
    gpuOptions.adapter = options.adapter;
    gpuOptions.decoding = options.decoding;
    gpuOptions.cardDecoders = options.cardDecoders;
    base::Result<std::unique_ptr<engine::Gpu>> made = engine::Gpu::create(gpuOptions);
    if (!made) {
        std::fprintf(stderr, "%s\n", made.error().message.c_str());
        return 1;
    }
    const std::unique_ptr<engine::Gpu> gpu = std::move(made).value();
    base::Result<std::unique_ptr<engine::Surface>> surfaceMade = gpu->makeSurface(window);
    if (!surfaceMade) {
        std::fprintf(stderr, "%s\n", surfaceMade.error().message.c_str());
        return 1;
    }
    const std::unique_ptr<engine::Surface> surface = std::move(surfaceMade).value();

    std::atomic<int> streamsFailed{0};
    const HANDLE somethingNew = state.somethingNew;
    const std::unique_ptr<engine::Scene> scene = gpu->makeScene({
        [somethingNew] { SetEvent(somethingNew); },
        [somethingNew, &streamsFailed](engine::Fault fault) {
            streamsFailed += fault == engine::Fault::StreamFailed ? 1 : 0;
            SetEvent(somethingNew);
        },
        // (This window shows every picture the moment it is ready already.)
        {},
    });

    std::vector<engine::Stream*> streams;
    std::vector<Player> players;
    double wantedPerSecond = 0;
    for (int index = 0; index < options.tiles; ++index) {
        const Clip& clip = clips.at(options.sources[static_cast<std::size_t>(index) % options.sources.size()]);
        engine::Stream& stream = scene->add(static_cast<engine::StreamId>(index + 1), clip.codec);
        stream.shownAt({}, index < options.large ? engine::Importance::ShownLarge : engine::Importance::Preview);
        streams.push_back(&stream);
        wantedPerSecond += clip.perSecond;
        players.push_back({&clip, &stream});
    }
    scene->place(grid(options.tiles, state.size));

    Report report(options.report);
    std::string sources;
    for (const std::string& name : options.sources) {
        sources += (sources.empty() ? "" : ",") + name;
    }
    report.line(formatted(
        "run tiles=%d sources=%s decoding=%s large=%d card_decoders=%d pace=%s window=%dx%d card_decodes_h264=%d",
        options.tiles, sources.c_str(),
        options.decoding == engine::DecodingChoice::GraphicsCard ? "card"
        : options.decoding == engine::DecodingChoice::Software   ? "software"
                                                                 : "auto",
        options.large, options.cardDecoders, options.atOnce ? "at_once" : "monitor", state.size.width,
        state.size.height, gpu->decodesH264() ? 1 : 0));
    // What the adapter calls itself stays on this PC: the report is not a file that is kept.
    report.line("adapter " + gpu->adapterName());
    report.line(formatted("card_budget_mb=%.0f", static_cast<double>(gpu->memory().budgetBytes) / 1048576.0));

    Meter meter;
    meter.ask();
    const std::int64_t began = base::steadyNowUs();
    std::int64_t nextLine = began + static_cast<std::int64_t>(options.every) * 1'000'000;
    std::int64_t lastLine = began;
    std::uint64_t draws = 0;
    std::uint64_t drawsAtLastLine = 0;
    double drawingMs = 0;
    Totals last;
    bool lostOnce = options.loseDeviceAfter == 0;

    // The means over the run, leaving out the first two lines while everything starts.
    int lines = 0;
    int counted = 0;
    double sumProcessor = 0;
    double sumDecoding = 0;
    double sumDrawing = 0;
    double sumShownPerSecond = 0;
    double sumDecodedPerSecond = 0;
    std::uint64_t mostCardBytes = 0;
    std::uint64_t mostCommitted = 0;

    {
        const Feeder feeder(std::move(players));
        const engine::Surface::Pace pace =
            options.atOnce ? engine::Surface::Pace::AtOnce : engine::Surface::Pace::WithTheMonitor;
        while (!state.closed.load()) {
            MsgWaitForMultipleObjects(1, &state.somethingNew, FALSE, 100, QS_ALLINPUT);
            MSG message{};
            while (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE) != 0) {
                TranslateMessage(&message);
                DispatchMessageW(&message);
            }
            if (state.resized.exchange(false)) {
                surface->resize(state.size);
                scene->place(grid(options.tiles, state.size));
            }

            if (!options.atOnce) {
                surface->waitForRoom(100);
            }
            const std::int64_t before = base::steadyNowUs();
            if (scene->draw(surface->target())) {
                if (surface->present(pace)) {
                    const std::int64_t after = base::steadyNowUs();
                    scene->shown(after);
                    ++draws;
                    drawingMs += static_cast<double>(after - before) / 1000.0;
                }
            }

            const std::int64_t now = base::steadyNowUs();
            if (!lostOnce && now - began >= static_cast<std::int64_t>(options.loseDeviceAfter) * 1'000'000) {
                lostOnce = true;
                gpu->loseDevice();
                report.line(formatted("lost at=%.1f", static_cast<double>(now - began) / 1e6));
                SetEvent(state.somethingNew);
            }
            if (now >= nextLine) {
                // Over the time that passed, which is longer than was asked for when a draw took long.
                const double span = static_cast<double>(now - lastLine) / 1e6;
                lastLine = now;
                const Cost cost = meter.ask();
                const Totals totals = totalsOf(streams);
                const engine::Gpu::Memory memory = gpu->memory();
                const std::uint64_t drawn = draws - drawsAtLastLine;
                const double shownPerSecond = static_cast<double>(totals.shown - last.shown) / span;
                const double decodedPerSecond = static_cast<double>(totals.decoded - last.decoded) / span;
                report.line(formatted(
                    "at=%.0f processor=%.1f decoding=%.1f drawing=%.1f card_mb=%.0f committed_mb=%.0f handles=%u "
                    "window_objects=%u "
                    "on_card=%d in_software=%d pushed_s=%.0f decoded_s=%.0f shown_s=%.0f dropped=%llu replaced=%llu "
                    "refused=%llu "
                    "decode_ms=%.2f upload_ms=%.2f arrived_to_shown_ms=%.2f worst_ms=%.1f draws_s=%.0f draw_ms=%.2f "
                    "unseen=%llu "
                    "recovered=%llu",
                    static_cast<double>(now - began) / 1e6, cost.processorPercent, cost.decodingPercent,
                    cost.drawingPercent, static_cast<double>(memory.usedBytes) / 1048576.0,
                    static_cast<double>(cost.committedBytes) / 1048576.0, cost.handles, cost.windowObjects,
                    totals.onTheCard, totals.inSoftware, static_cast<double>(totals.pushed - last.pushed) / span,
                    decodedPerSecond, shownPerSecond, static_cast<unsigned long long>(totals.droppedBehind),
                    static_cast<unsigned long long>(totals.replaced), static_cast<unsigned long long>(totals.refused),
                    totals.decodeMs, totals.uploadMs, totals.arrivedToShownMs, totals.arrivedToShownMaxMs,
                    static_cast<double>(drawn) / span, drawn > 0 ? drawingMs / static_cast<double>(drawn) : 0.0,
                    static_cast<unsigned long long>(surface->timesUnseen()),
                    static_cast<unsigned long long>(gpu->timesRecovered())));
                if (++lines > 2) {
                    ++counted;
                    sumProcessor += cost.processorPercent;
                    sumDecoding += cost.decodingPercent;
                    sumDrawing += cost.drawingPercent;
                    sumShownPerSecond += shownPerSecond;
                    sumDecodedPerSecond += decodedPerSecond;
                }
                mostCardBytes = std::max(mostCardBytes, memory.usedBytes);
                mostCommitted = std::max(mostCommitted, cost.committedBytes);
                last = totals;
                drawsAtLastLine = draws;
                drawingMs = 0;
                nextLine = now + static_cast<std::int64_t>(options.every) * 1'000'000;
            }
            if (options.seconds > 0 && now - began >= static_cast<std::int64_t>(options.seconds) * 1'000'000) {
                break;
            }
        }
    }

    const Totals totals = totalsOf(streams);
    const double runs = std::max(counted, 1);
    report.line(formatted(
        "summary wanted_s=%.0f processor=%.1f decoding=%.1f drawing=%.1f card_mb_most=%.0f committed_mb_most=%.0f "
        "decoded_s=%.0f "
        "shown_s=%.0f "
        "pushed=%llu decoded=%llu shown=%llu dropped=%llu replaced=%llu refused=%llu streams_failed=%d on_card=%d "
        "in_software=%d "
        "decode_ms=%.2f upload_ms=%.2f arrived_to_shown_ms=%.2f worst_ms=%.1f unseen=%llu recovered=%llu",
        wantedPerSecond, sumProcessor / runs, sumDecoding / runs, sumDrawing / runs,
        static_cast<double>(mostCardBytes) / 1048576.0, static_cast<double>(mostCommitted) / 1048576.0,
        sumDecodedPerSecond / runs, sumShownPerSecond / runs, static_cast<unsigned long long>(totals.pushed),
        static_cast<unsigned long long>(totals.decoded), static_cast<unsigned long long>(totals.shown),
        static_cast<unsigned long long>(totals.droppedBehind), static_cast<unsigned long long>(totals.replaced),
        static_cast<unsigned long long>(totals.refused), streamsFailed.load(), totals.onTheCard, totals.inSoftware,
        totals.decodeMs, totals.uploadMs, totals.arrivedToShownMs, totals.arrivedToShownMaxMs,
        static_cast<unsigned long long>(surface->timesUnseen()),
        static_cast<unsigned long long>(gpu->timesRecovered())));
    // How long pictures took from arriving to being on the screen, as shares of all that were shown.
    std::string spread = "arrived_to_shown";
    const double all = static_cast<double>(std::max<std::uint64_t>(totals.shown, 1));
    for (std::size_t place = 0; place < totals.arrivedToShownCounts.size(); ++place) {
        const double share = static_cast<double>(totals.arrivedToShownCounts[place]) / all * 100.0;
        spread += place < engine::arrivedToShownStepsMs.size()
                      ? formatted(" up_to_%.0fms=%.1f%%", engine::arrivedToShownStepsMs[place], share)
                      : formatted(" slower=%.1f%%", share);
    }
    report.line(spread);

    DestroyWindow(window);
    CloseHandle(state.somethingNew);
    return 0;
}

} // namespace
} // namespace qkvm::bench

int main(int count, char** arguments)
{
    // Nothing of the rig's own throws; what the standard library might throw ends the run with a
    // sentence instead of with Windows' report of a crash.
    try {
        qkvm::bench::Options options;
        if (!qkvm::bench::read(count, arguments, options)) {
            return 2;
        }
        return qkvm::bench::run(options);
    } catch (const std::exception& error) {
        std::fprintf(stderr, "qkvm-bench ended: %s\n", error.what());
        return 1;
    } catch (...) {
        std::fputs("qkvm-bench ended on something that was thrown.\n", stderr);
        return 1;
    }
}
