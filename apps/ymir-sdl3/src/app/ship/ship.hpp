#pragma once

// Ship mode (Drydock): this build of Ymir is one recompiled game.
//
// Enabled when <exe dir>/ship/recipe.toml exists. Then the frontend:
//   - keeps its profile (settings, saves) next to the executable
//   - shows its own setup screens before anything of the emulator:
//       1. BIOS: asks for a Saturn BIOS that can boot the game's region,
//          checks it against Ymir's BIOS database and copies it to roms/ipl
//       2. Disc: only accepts the disc the recipe describes (product number
//          and the SHA-256 of every code file), refusing anything else
//       3. Prepare: on first use, recompiles the game from that disc into
//          <exe dir>/recomp/<product>.dll with the bundled compiler
//   - hides Ymir's menus: Esc (or the controller's Guide button) opens a
//     simple game menu instead, with an "Advanced" switch that brings back
//     all of Ymir's menus and settings
//   - holds emulation until the setup is done

#include <app/ui/defs/settings_defs.hpp>

#include <filesystem>
#include <functional>
#include <string>

namespace ymir_recomp {
class Runtime;
} // namespace ymir_recomp

struct SDL_Window;

namespace app {

class SharedContext;
struct Settings;

/// The native code runtime (null in builds without Drydock).
ymir_recomp::Runtime *GetRecompRuntime();

namespace services {
    class DiscService;
    class ROMService;
} // namespace services

namespace ship {

/// What the ship needs from the frontend.
struct Host {
    SharedContext *context = nullptr;
    Settings *settings = nullptr;
    services::DiscService *discs = nullptr;
    services::ROMService *roms = nullptr;
    std::function<void(ui::SettingsTab)> openSettings;
    std::function<bool()> isSettingsOpen;
};

/// Loads the ship recipe from <exeDir>/ship. Returns true if this is a ship.
bool Init(const Host &host, const std::filesystem::path &exeDir);

bool Enabled();

/// Called once the IPL ROMs have been scanned at startup: picks a usable BIOS
/// or stays on the BIOS screen.
void Start();

/// Window title, e.g. "TheBortas - PDZ II".
std::string WindowTitle();

/// Sets the window icon from <exe dir>/ship/icon.png, if present.
void ApplyWindowIcon(SDL_Window *window);

/// Whether Ymir's own menu bar may be shown (only with "Advanced" on).
bool ShowYmirMenus();

/// Esc / Guide button. Returns true if the ship handled it (opened or closed
/// its menu), in which case the frontend does nothing else with it.
bool OnMenuButton();

/// Called before a disc image is loaded (emulator thread). Returns true to
/// let it load; false if it is refused, or deferred until the setup is done.
bool BeforeLoadDisc(const std::filesystem::path &path);

/// Draws the ship's screens and menu. GUI thread, once per frame.
void Draw();

/// Stops a build in progress.
void Shutdown();

} // namespace ship
} // namespace app
