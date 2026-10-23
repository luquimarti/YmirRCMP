#pragma once

// Ship mode (Drydock): this build of Ymir is one recompiled game.
//
// Enabled when <exe dir>/ship/recipe.toml exists. Then the frontend:
//   - keeps its profile (settings, saves) next to the executable
//   - only accepts the disc the recipe describes (product number and the
//     SHA-256 of every code file), refusing anything else
//   - on first use, recompiles the game from that disc into
//     <exe dir>/recomp/<product>.dll with the bundled compiler, showing
//     progress, and loads it
//   - holds emulation until all of that is done

#include <filesystem>
#include <string>

namespace ymir_recomp {
class Runtime;
} // namespace ymir_recomp

namespace app {

class SharedContext;

/// The native code runtime (null in builds without Drydock).
ymir_recomp::Runtime *GetRecompRuntime();

namespace services {
    class DiscService;
} // namespace services

namespace ship {

/// Loads the ship recipe from <exeDir>/ship. Returns true if this is a ship.
bool Init(SharedContext &context, const std::filesystem::path &exeDir);

bool Enabled();

/// Window title, e.g. "TheBortas - PDZ II".
std::string WindowTitle();

/// Called before a disc image is loaded (emulator thread). Returns true to
/// let it load; false if it is refused, or deferred until the game is built.
bool BeforeLoadDisc(const std::filesystem::path &path);

/// Draws the ship's own windows (disc selection, build progress). GUI thread,
/// once per frame.
void Draw(services::DiscService &discs);

/// Stops a build in progress.
void Shutdown();

} // namespace ship
} // namespace app
