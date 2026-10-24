#include "ship.hpp"

#include <app/events/emu_event_factory.hpp>
#include <app/events/gui_event_factory.hpp>
#include <app/imgui_data.hpp>
#include <app/profile.hpp>
#include <app/services/disc_service.hpp>
#include <app/services/rom_service.hpp>
#include <app/settings.hpp>
#include <app/shared_context.hpp>

#include <build.hpp> // Drydock: recipe, disc check, game build
#include <ymir_recomp/runtime.hpp>

#include <ymir/core/hash.hpp>
#include <ymir/hw/vdp/vdp_configs.hpp>
#include <ymir/db/ipl_db.hpp>
#include <ymir/sys/memory_defs.hpp>

#include <SDL3/SDL_events.h>
#include <SDL3/SDL_misc.h>
#include <SDL3/SDL_surface.h>
#include <SDL3/SDL_video.h>
#include <stb_image.h>
#include <imgui.h>

#include <algorithm>
#include <atomic>
#include <fstream>
#include <iterator>
#include <mutex>
#include <thread>
#include <vector>

namespace app::ship {

namespace fs = std::filesystem;

namespace {

    enum class Phase { Off, NeedBIOS, NeedDisc, Building, Failed, Ready };

    struct State {
        Host host;
        fs::path exeDir;
        drydock::Recipe recipe;

        std::atomic<Phase> phase{Phase::Off};
        std::mutex mutex;     // guards the strings and paths below
        std::string message;  // why the last BIOS/disc was refused, or why the build failed
        fs::path disc;        // disc being built from
        fs::path pendingDisc; // disc remembered from last time, loaded once the BIOS is in place
        fs::path pickedBIOS;  // chosen in the file dialog, processed on the GUI thread

        // build worker
        std::thread worker;
        std::atomic<bool> cancel{false};
        std::atomic<bool> workerDone{false};
        bool workerOk = false;
        std::string workerError;
        std::atomic<float> fraction{0.0f};
        std::atomic<drydock::Stage> stage{drydock::Stage::CheckingDisc};
        std::string detail;

        // game menu (GUI thread only)
        bool menuOpen = false;
        bool menuFocus = false;
        bool resumeOnClose = false;
        bool advanced = false;
        bool controlsFromMenu = false;
        bool confirmRestart = false;
        bool aboutOpen = false;
    };

    State g_state;

    std::string U8(const fs::path &p) {
        const std::u8string s = p.u8string();
        return std::string(s.begin(), s.end());
    }

    fs::path FromU8(const char *s) {
        const std::string str = s;
        return fs::path(std::u8string(str.begin(), str.end()));
    }

    void SetMessage(std::string msg) {
        std::lock_guard lock(g_state.mutex);
        g_state.message = std::move(msg);
    }

    void Quit() {
        SDL_Event quit{};
        quit.type = SDL_EVENT_QUIT;
        SDL_PushEvent(&quit);
    }

    // Support link shown in the menus
    constexpr const char *kSupportLabel = "Support Saturn Recomp";
    constexpr const char *kSupportURL = "https://www.patreon.com/cw/luquimarti";

    void OpenSupportPage() {
        SDL_OpenURL(kSupportURL);
    }

    constexpr const char *kDrydockURL = "https://github.com/luquimarti/Drydock";
    constexpr const char *kShipsURLBase = "https://github.com/luquimarti/";
    constexpr const char *kYmirURL = "https://github.com/StrikerX3/Ymir";

    // -------------------------------------------------------------------------
    // BIOS

    // The BIOS region the game's region needs.
    ymir::db::SystemRegion WantedRegion() {
        const std::string &r = g_state.recipe.region;
        if (r == "USA" || r == "US" || r == "EUR" || r == "EU" || r == "PAL") return ymir::db::SystemRegion::US_EU;
        if (r == "JPN" || r == "JP" || r == "Japan") return ymir::db::SystemRegion::JP;
        if (r == "KOR" || r == "KR" || r == "Korea") return ymir::db::SystemRegion::KR;
        return ymir::db::SystemRegion::None;
    }

    const char *RegionName(ymir::db::SystemRegion region) {
        switch (region) {
        case ymir::db::SystemRegion::US_EU: return "North American / European";
        case ymir::db::SystemRegion::JP: return "Japanese";
        case ymir::db::SystemRegion::KR: return "Korean";
        default: return "unknown region";
        }
    }

    bool Usable(const ymir::db::IPLROMInfo *info) {
        if (info == nullptr) return false;
        const ymir::db::SystemRegion wanted = WantedRegion();
        return info->regionFree || wanted == ymir::db::SystemRegion::None || info->region == wanted;
    }

    std::string NeedsText() {
        const ymir::db::SystemRegion wanted = WantedRegion();
        if (wanted == ymir::db::SystemRegion::None) return "a Sega Saturn BIOS";
        return std::string("a ") + RegionName(wanted) + " Sega Saturn BIOS (or a region-free one)";
    }

    std::string RefusalFor(const ymir::db::IPLROMInfo *info) {
        if (info == nullptr) {
            return "This BIOS is not recognized. It may be a modified or bad dump; please dump it again from "
                   "your console.";
        }
        return std::string("This is a ") + RegionName(info->region) + " Saturn BIOS. " +
               g_state.recipe.DisplayName() + " needs " + NeedsText() + ".";
    }

    // Uses the BIOS at `path` (already in roms/ipl and scanned) from now on.
    bool UseBIOS(const fs::path &path) {
        State &s = g_state;
        Settings &settings = *s.host.settings;
        if (!settings.system.ipl.overrideImage || settings.system.ipl.path != path) {
            settings.system.ipl.overrideImage = true;
            settings.system.ipl.path = path;
            settings.MakeDirty();
        }
        const util::ROMLoadResult result = s.host.roms->LoadIPLROM();
        if (!result.succeeded) {
            SetMessage("Could not load the BIOS: " + result.errorMessage);
            return false;
        }

        // on to the disc; the one used last time goes straight in
        fs::path pending;
        {
            std::lock_guard lock(s.mutex);
            pending = std::move(s.pendingDisc);
            s.pendingDisc.clear();
            s.message.clear();
        }
        s.phase = Phase::NeedDisc;
        if (!pending.empty()) s.host.context->EnqueueEvent(events::emu::LoadDisc(pending));
        return true;
    }

    // Looks for a usable BIOS among the scanned ones. Returns false (with a
    // message if something unusable was found) if there is none.
    bool PickScannedBIOS() {
        State &s = g_state;
        SharedContext &ctx = *s.host.context;
        const Settings &settings = *s.host.settings;

        fs::path chosen;
        std::string refusal;
        {
            std::lock_guard lock(ctx.locks.romManager);
            const auto &roms = ctx.romManager.GetIPLROMs();
            if (settings.system.ipl.overrideImage) {
                auto it = roms.find(settings.system.ipl.path);
                if (it != roms.end() && Usable(it->second.info)) chosen = it->first;
            }
            for (const auto &[path, entry] : roms) {
                if (!chosen.empty()) break;
                if (Usable(entry.info)) chosen = path;
                else if (refusal.empty()) refusal = RefusalFor(entry.info);
            }
        }
        if (chosen.empty()) {
            if (!refusal.empty()) SetMessage("The BIOS in roms/ipl can't be used. " + refusal);
            return false;
        }
        return UseBIOS(chosen);
    }

    // Checks the file chosen in the dialog and copies it to roms/ipl.
    void InstallBIOS(const fs::path &src) {
        State &s = g_state;
        std::error_code ec;
        const auto size = fs::file_size(src, ec);
        if (ec || size != ymir::sys::kIPLSize) {
            SetMessage("That file is not a Saturn BIOS (a BIOS image is exactly 512 KB).");
            return;
        }
        std::vector<char> data(ymir::sys::kIPLSize);
        {
            std::ifstream in(src, std::ios::binary);
            in.read(data.data(), data.size());
            if (!in) {
                SetMessage("Could not read " + U8(src.filename()) + ".");
                return;
            }
        }
        const ymir::XXH128Hash hash = ymir::CalcHash128(data.data(), data.size(), ymir::sys::kIPLHashSeed);
        const ymir::db::IPLROMInfo *info = ymir::db::GetIPLROMInfo(hash);
        if (!Usable(info)) {
            SetMessage(RefusalFor(info));
            return;
        }

        const fs::path dir = s.host.context->profile.GetPath(ProfilePath::IPLROMImages);
        fs::create_directories(dir, ec);
        fs::path dst = dir / src.filename();
        if (!fs::equivalent(src, dst, ec)) {
            fs::copy_file(src, dst, fs::copy_options::overwrite_existing, ec);
            if (ec) {
                SetMessage("Could not copy the BIOS to roms/ipl: " + ec.message());
                return;
            }
        }
        s.host.roms->ScanIPLROMs();
        const fs::path canonical = fs::canonical(dst, ec);
        UseBIOS(ec ? dst : canonical);
    }

    void OpenBIOSDialog() {
        FileDialogParams params{};
        params.dialogTitle = "Select your Sega Saturn BIOS";
        params.filters = {{.name = "BIOS images (*.bin, *.rom)", .filters = "bin;rom"},
                          {.name = "All files (*.*)", .filters = "*"}};
        params.userdata = nullptr;
        params.callback = [](void *, const char *const *filelist, int) {
            if (filelist == nullptr || *filelist == nullptr) return; // cancelled or failed
            std::lock_guard lock(g_state.mutex);
            g_state.pickedBIOS = FromU8(*filelist);
        };
        g_state.host.context->EnqueueEvent(events::gui::OpenFile(std::move(params)));
    }

    // -------------------------------------------------------------------------
    // Build

    void StartBuild(const fs::path &disc) {
        State &s = g_state;
        if (s.worker.joinable()) s.worker.join();
        {
            std::lock_guard lock(s.mutex);
            s.disc = disc;
            s.message.clear();
            s.detail.clear();
        }
        s.cancel = false;
        s.workerDone = false;
        s.fraction = 0.0f;
        s.phase = Phase::Building;
        s.worker = std::thread([disc] {
            State &s = g_state;
            const drydock::BuildResult r = drydock::BuildGame(
                s.recipe, U8(disc), U8(s.exeDir), drydock::BundledToolchain(U8(s.exeDir)),
                [](drydock::Stage stage, float fraction, const std::string &detail) {
                    g_state.stage = stage;
                    g_state.fraction = fraction;
                    std::lock_guard lock(g_state.mutex);
                    g_state.detail = detail;
                },
                &s.cancel);
            {
                std::lock_guard lock(s.mutex);
                s.workerOk = r.ok;
                s.workerError = r.error;
            }
            s.workerDone = true;
        });
    }

    void PollBuild() {
        State &s = g_state;
        if (s.phase != Phase::Building || !s.workerDone) return;
        s.worker.join();
        bool ok;
        std::string error;
        fs::path disc;
        {
            std::lock_guard lock(s.mutex);
            ok = s.workerOk;
            error = s.workerError;
            disc = s.disc;
        }
        if (ok) {
            s.phase = Phase::NeedDisc; // BeforeLoadDisc takes it from here
            s.host.context->EnqueueEvent(events::emu::LoadDisc(disc));
        } else {
            SetMessage(error);
            s.phase = s.cancel ? Phase::NeedDisc : Phase::Failed;
        }
    }

    // -------------------------------------------------------------------------
    // Drawing

    float Scale() {
        const YmirImGuiData *d = GetYmirImGuiData();
        return d ? d->displayScale : 1.0f;
    }

    const ImVec4 kAccent(0.96f, 0.62f, 0.22f, 1.0f);
    const ImVec4 kError(1.0f, 0.45f, 0.40f, 1.0f);
    const ImVec4 kDim(0.62f, 0.64f, 0.70f, 1.0f);
    const ImVec4 kFaint(0.42f, 0.44f, 0.50f, 1.0f);

    void PushFont(bool bold, float size) {
        const YmirImGuiData *d = GetYmirImGuiData();
        ImGui::PushFont(bold ? d->fonts.sansSerif.bold : d->fonts.sansSerif.regular, size);
    }

    // A full-window layer with a centered column; returns false if not drawn.
    bool BeginScreen(const char *id, ImVec4 background, float width) {
        const ImGuiViewport *vp = ImGui::GetMainViewport();
        ImGui::SetNextWindowPos(vp->Pos);
        ImGui::SetNextWindowSize(vp->Size);
        ImGui::SetNextWindowViewport(vp->ID);
        ImGui::PushStyleColor(ImGuiCol_WindowBg, background);
        ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 0.0f);
        ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.0f);
        const ImGuiWindowFlags flags = ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove |
                                       ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoDocking;
        const bool open = ImGui::Begin(id, nullptr, flags);
        ImGui::PopStyleVar(2);
        ImGui::PopStyleColor();
        if (!open) {
            ImGui::End();
            return false;
        }
        const float w = std::min(width * Scale(), vp->Size.x - 32.0f * Scale());
        ImGui::SetCursorPos(ImVec2((vp->Size.x - w) * 0.5f, std::max(24.0f * Scale(), vp->Size.y * 0.14f)));
        ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(14.0f * Scale(), 8.0f * Scale()));
        ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(10.0f * Scale(), 10.0f * Scale()));
        ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, 6.0f * Scale());
        ImGui::BeginChild("##column", ImVec2(w, 0), ImGuiChildFlags_AutoResizeY, ImGuiWindowFlags_NoBackground);
        return true;
    }

    void EndScreen() {
        ImGui::EndChild();
        ImGui::PopStyleVar(3);
        ImGui::End();
    }

    void Header(const std::string &subtitle) {
        const YmirImGuiData *d = GetYmirImGuiData();
        PushFont(true, d->fontSizes.display * 0.75f);
        ImGui::TextUnformatted(g_state.recipe.title.c_str());
        ImGui::PopFont();
        PushFont(false, d->fontSizes.medium);
        ImGui::TextColored(kDim, "%s", subtitle.c_str());
        ImGui::PopFont();
        ImGui::Spacing();
    }

    // "1 BIOS  2 Disc  3 Prepare", with the current step highlighted
    void Steps(int current) {
        const YmirImGuiData *d = GetYmirImGuiData();
        static const char *kNames[] = {"BIOS", "Disc", "Prepare"};
        PushFont(true, d->fontSizes.medium);
        for (int i = 0; i < 3; ++i) {
            if (i > 0) ImGui::SameLine(0.0f, 28.0f * Scale());
            const ImVec4 color = i + 1 == current ? kAccent : (i + 1 < current ? kDim : kFaint);
            ImGui::TextColored(color, "%d  %s", i + 1, kNames[i]);
        }
        ImGui::PopFont();
        ImGui::Separator();
        ImGui::Spacing();
    }

    void ErrorText(const std::string &message) {
        if (message.empty()) return;
        ImGui::PushStyleColor(ImGuiCol_Text, kError);
        ImGui::TextWrapped("%s", message.c_str());
        ImGui::PopStyleColor();
    }

    bool PrimaryButton(const char *label, ImVec2 size = ImVec2(0, 0)) {
        ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.80f, 0.45f, 0.12f, 1.0f));
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.92f, 0.55f, 0.18f, 1.0f));
        ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImVec4(0.70f, 0.38f, 0.10f, 1.0f));
        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.08f, 0.06f, 0.04f, 1.0f));
        const bool pressed = ImGui::Button(label, size);
        ImGui::PopStyleColor(4);
        return pressed;
    }

    void DrawSetup() {
        State &s = g_state;
        const YmirImGuiData *d = GetYmirImGuiData();
        if (!BeginScreen("##drydock_setup", ImVec4(0.035f, 0.04f, 0.06f, 1.0f), 620.0f)) return;

        std::string message, detail;
        {
            std::lock_guard lock(s.mutex);
            message = s.message;
            detail = s.detail;
        }
        const Phase phase = s.phase;
        Header(s.recipe.region.empty() ? s.recipe.ship : s.recipe.ship + "  -  " + s.recipe.region + " release");
        PushFont(false, d->fontSizes.large);

        switch (phase) {
        case Phase::NeedBIOS:
            Steps(1);
            ImGui::TextWrapped("%s needs %s, dumped from your own console.", s.recipe.title.c_str(),
                               NeedsText().c_str());
            ImGui::TextColored(kDim, "It is copied to roms/ipl and remembered.");
            ErrorText(message);
            ImGui::Spacing();
            if (PrimaryButton("Select BIOS file...")) OpenBIOSDialog();
            break;

        case Phase::NeedDisc:
            Steps(2);
            ImGui::TextWrapped("Select your copy of %s: a .cue, .chd, .iso, .ccd or .mds disc image.",
                               s.recipe.DisplayName().c_str());
            ImGui::TextColored(kDim, "Only this release is accepted.");
            ErrorText(message);
            ImGui::Spacing();
            if (PrimaryButton("Select disc image...")) s.host.discs->OpenLoadDiscDialog();
            break;

        case Phase::Building:
            Steps(3);
            ImGui::TextWrapped("Preparing %s for this PC: its code is recompiled from your disc. This happens "
                               "only once and takes a few minutes.",
                               s.recipe.title.c_str());
            ImGui::Spacing();
            ImGui::TextUnformatted(drydock::StageName(s.stage));
            ImGui::PushStyleColor(ImGuiCol_PlotHistogram, kAccent);
            ImGui::ProgressBar(s.fraction, ImVec2(-FLT_MIN, 0), "");
            ImGui::PopStyleColor();
            ImGui::TextColored(kDim, "%s", detail.c_str());
            ImGui::Spacing();
            if (ImGui::Button("Cancel")) s.cancel = true;
            break;

        case Phase::Failed:
        default:
            Steps(3);
            ImGui::TextColored(kError, "Could not prepare %s.", s.recipe.title.c_str());
            ErrorText(message);
            ImGui::Spacing();
            if (PrimaryButton("Retry")) {
                fs::path disc;
                {
                    std::lock_guard lock(s.mutex);
                    disc = s.disc;
                }
                if (!disc.empty()) {
                    s.phase = Phase::NeedDisc;
                    s.host.context->EnqueueEvent(events::emu::LoadDisc(disc));
                }
            }
            ImGui::SameLine();
            if (ImGui::Button("Select disc image...")) s.host.discs->OpenLoadDiscDialog();
            break;
        }

        if (phase != Phase::Building) {
            ImGui::SameLine();
            if (ImGui::Button("Quit")) Quit();
        }
        ImGui::PopFont();

        ImGui::Spacing();
        ImGui::Spacing();
        PushFont(false, d->fontSizes.small);
        ImGui::TextColored(kFaint, "This program contains no game data. Built with Drydock on the Ymir emulator.");
        ImGui::PushStyleColor(ImGuiCol_Text, kAccent);
        if (ImGui::Selectable(kSupportLabel, false, ImGuiSelectableFlags_None, ImGui::CalcTextSize(kSupportLabel))) {
            OpenSupportPage();
        }
        ImGui::PopStyleColor();
        ImGui::PopFont();
        EndScreen();
    }

    void CloseMenu() {
        State &s = g_state;
        s.menuOpen = false;
        s.confirmRestart = false;
        s.aboutOpen = false;
        s.controlsFromMenu = false;
        if (s.resumeOnClose) s.host.context->EnqueueEvent(events::emu::SetPaused(false));
        s.resumeOnClose = false;
    }

    void OpenMenu() {
        State &s = g_state;
        s.menuOpen = true;
        s.menuFocus = true;
        s.resumeOnClose = !s.host.context->paused;
        if (s.resumeOnClose) s.host.context->EnqueueEvent(events::emu::SetPaused(true));
    }

    void SectionTitle(const char *title) {
        const YmirImGuiData *d = GetYmirImGuiData();
        ImGui::Spacing();
        PushFont(true, d->fontSizes.medium);
        ImGui::TextColored(kAccent, "%s", title);
        ImGui::PopFont();
    }

    void LinkButton(const char *label, const std::string &url) {
        if (ImGui::Button(label)) {
            SDL_OpenURL(url.c_str());
        }
        if (ImGui::IsItemHovered()) {
            ImGui::SetTooltip("%s", url.c_str());
        }
    }

    void DrawAbout() {
        State &s = g_state;
        const YmirImGuiData *d = GetYmirImGuiData();
        if (!BeginScreen("##drydock_about", ImVec4(0.02f, 0.02f, 0.03f, 0.90f), 620.0f)) return;

        const std::string &ship = s.recipe.ship;
        const std::string &game = s.recipe.title;
        Header("About " + ship);
        PushFont(false, d->fontSizes.large);
        ImGui::TextWrapped("%s is a native PC version of %s, built with Drydock.", ship.c_str(), game.c_str());
        ImGui::Spacing();
        ImGui::TextWrapped("Every game Drydock can rebuild is a ship: a recipe that describes where the game's code "
                           "lives and how to rebuild it. %s is the first one to leave the dock.",
                           ship.c_str());
        ImGui::Spacing();
        ImGui::TextWrapped("Drydock's static recompilation is made with the help of AI. Studying each game's code and "
                           "turning it into a working recompilation takes a lot of AI work, and every new ship needs "
                           "more of it. If you'd like to see more ships set sail, you can support the project on "
                           "Patreon: it pays for those tokens and for the time it takes to polish each one. Thank you!");
        ImGui::Spacing();

        if (s.menuFocus) {
            ImGui::SetKeyboardFocusHere();
            s.menuFocus = false;
        }
        if (PrimaryButton("Support on Patreon")) OpenSupportPage();
        ImGui::SameLine();
        LinkButton("Drydock", kDrydockURL);
        ImGui::SameLine();
        LinkButton(ship.c_str(), kShipsURLBase + ship);
        ImGui::SameLine();
        LinkButton("Ymir", kYmirURL);
        ImGui::PopFont();

        ImGui::Spacing();
        ImGui::Separator();
        ImGui::Spacing();
        PushFont(false, d->fontSizes.medium);
        ImGui::PushStyleColor(ImGuiCol_Text, kDim);
        ImGui::TextWrapped("The first time you launch it, Drydock reads the game's program code from your own disc and "
                           "recompiles it: the instructions written for the Saturn's SH-2 processors are translated into "
                           "C++ and compiled into native code for your PC. The rest of the console (graphics, sound, "
                           "the CD drive) is emulated by Ymir, a Sega Saturn emulator by StrikerX3 and contributors. "
                           "Nothing from the game is included in this download; it all comes from your disc.");
        ImGui::PopStyleColor();
        ImGui::PopFont();

        ImGui::Spacing();
        PushFont(false, d->fontSizes.large);
        if (ImGui::Button("Close", ImVec2(-FLT_MIN, 0))) {
            s.aboutOpen = false;
            s.menuFocus = true;
        }
        ImGui::PopFont();
        EndScreen();
    }

    void DrawMenu() {
        State &s = g_state;
        if (s.aboutOpen) {
            DrawAbout();
            return;
        }
        Settings &settings = *s.host.settings;
        const YmirImGuiData *d = GetYmirImGuiData();
        if (!BeginScreen("##drydock_menu", ImVec4(0.02f, 0.02f, 0.03f, 0.82f), 540.0f)) return;

        Header("Paused");
        PushFont(false, d->fontSizes.large);
        const ImVec2 wide(-FLT_MIN, 0);

        if (s.menuFocus) {
            ImGui::SetKeyboardFocusHere();
            s.menuFocus = false;
        }
        bool close = PrimaryButton("Resume", wide);

        SectionTitle("Display");
        bool fullScreen = settings.video.fullScreen.Get();
        if (ImGui::Checkbox("Full screen", &fullScreen)) {
            settings.video.fullScreen = fullScreen;
            settings.MakeDirty();
        }
        ImGui::AlignTextToFramePadding();
        ImGui::TextUnformatted("Aspect ratio");
        ImGui::SameLine();
        const bool is169 = settings.video.forceAspectRatio && settings.video.forcedAspect == Ratio{16, 9};
        const bool is43 = settings.video.forceAspectRatio && settings.video.forcedAspect == Ratio{4, 3};
        if (ImGui::RadioButton("16:9", is169)) {
            settings.video.forceAspectRatio = true;
            settings.video.forcedAspect = {16, 9};
            settings.MakeDirty();
        }
        ImGui::SameLine();
        if (ImGui::RadioButton("4:3 (original)", is43)) {
            settings.video.forceAspectRatio = true;
            settings.video.forcedAspect = {4, 3};
            settings.MakeDirty();
        }
        settings.MakeDirty(ImGui::Checkbox("Show frame rate", &settings.gui.showFrameRateOSD));

        SectionTitle("Graphics");
        {
            // Internal resolution (hardware renderer). Up to the size allocated at start it applies right away;
            // going higher needs a restart.
            ImGui::AlignTextToFramePadding();
            ImGui::TextUnformatted("Resolution");
            uint32 &res = settings.video.enhancements.internalResolution;
            static const char *kLabels[] = {"1x (original)", "2x", "3x", "4x"};
            for (uint32 i = 1; i <= 4; ++i) {
                ImGui::SameLine();
                if (ImGui::RadioButton(kLabels[i - 1], res == i)) {
                    res = i;
                    settings.MakeDirty();
                }
            }
            settings.MakeDirty(ImGui::Checkbox("Lower automatically to keep full speed",
                                               &settings.video.enhancements.dynamicResolution));
            const uint32 allocated = ymir::vdp::config::GetHWInternalScale();
            const uint32 current = ymir::vdp::config::hwCurrentScale.load(std::memory_order_relaxed);
            PushFont(false, d->fontSizes.medium);
            if (!settings.video.useHardwareAcceleration) {
                ImGui::TextColored(kDim, "Needs video by GPU (Advanced > Video).");
            } else if (res > allocated) {
                ImGui::TextColored(d->colors.notice, "Restart %s to apply.", s.recipe.title.c_str());
            } else if (allocated > 1) {
                ImGui::TextColored(kDim, "Now rendering at %ux.", current);
            }
            ImGui::PopFont();
        }
        bool transparent = settings.video.enhancements.transparentMeshes.Get();
        if (ImGui::Checkbox("Real transparency (experimental)", &transparent)) {
            settings.video.enhancements.transparentMeshes = transparent;
            settings.MakeDirty();
        }
        PushFont(false, d->fontSizes.medium);
        ImGui::PushStyleColor(ImGuiCol_Text, transparent ? d->colors.notice : kDim);
        ImGui::TextWrapped("Draws see-through effects as real transparency instead of the original dot pattern. "
                           "Experimental: it can crash the game. If %s closes or freezes, turn this off.",
                           s.recipe.title.c_str());
        ImGui::PopStyleColor();
        ImGui::PopFont();

        SectionTitle("Sound");
        float volume = settings.audio.volume.Get() * 100.0f;
        ImGui::SetNextItemWidth(-120.0f * Scale());
        if (ImGui::SliderFloat("##volume", &volume, 0.0f, 100.0f, "Volume %.0f%%", ImGuiSliderFlags_AlwaysClamp)) {
            settings.audio.volume = volume * 0.01f;
            settings.MakeDirty();
        }
        ImGui::SameLine();
        bool mute = settings.audio.mute.Get();
        if (ImGui::Checkbox("Mute", &mute)) {
            settings.audio.mute = mute;
            settings.MakeDirty();
        }

        ImGui::Spacing();
        ImGui::Separator();
        ImGui::Spacing();
        if (ImGui::Button("Controls...", wide)) {
            s.menuOpen = false; // stays paused; the menu comes back when the window is closed
            s.controlsFromMenu = true;
            s.host.openSettings(ui::SettingsTab::Input);
        }
        if (ImGui::Button("Advanced settings...", wide)) {
            s.menuOpen = false; // stays paused; the menu comes back when the window is closed
            s.controlsFromMenu = true;
            s.host.openSettings(ui::SettingsTab::General);
        }
        PushFont(false, d->fontSizes.medium);
        ImGui::Checkbox("Show the emulator's menu bar", &s.advanced);
        ImGui::PopFont();
        if (s.confirmRestart) {
            PushFont(false, d->fontSizes.medium);
            ImGui::TextColored(d->colors.notice, "Restart %s? Progress since your last save will be lost.",
                               s.recipe.title.c_str());
            ImGui::PopFont();
            if (PrimaryButton("Yes, restart")) {
                s.confirmRestart = false;
                s.host.context->EnqueueEvent(events::emu::HardReset());
                close = true;
            }
            ImGui::SameLine();
            if (ImGui::Button("Cancel")) {
                s.confirmRestart = false;
            }
        } else if (ImGui::Button("Restart game", wide)) {
            s.confirmRestart = true;
        }
        if (ImGui::Button(("About " + s.recipe.ship).c_str(), wide)) {
            s.aboutOpen = true;
            s.menuFocus = true;
        }
        if (ImGui::Button("Quit", wide)) Quit();
        ImGui::PopFont();

        ImGui::Spacing();
        PushFont(false, d->fontSizes.small);
        ImGui::TextColored(kFaint, "%s",
                           s.advanced ? "Esc or the controller's Guide button opens this menu. The emulator's menu "
                                        "bar is at the top of the window."
                                      : "Esc or the controller's Guide button opens this menu.");
        ImGui::PopFont();
        EndScreen();

        if (close) CloseMenu();
    }

} // namespace

// -----------------------------------------------------------------------------

bool Init(const Host &host, const fs::path &exeDir) {
    State &s = g_state;
    const fs::path recipePath = exeDir / "ship" / "recipe.toml";
    std::error_code ec;
    if (!fs::exists(recipePath, ec)) return false;
    std::string error;
    if (!drydock::LoadRecipe(U8(recipePath), s.recipe, error)) {
        host.context->DisplayMessage(error);
        return false;
    }
    s.host = host;
    s.exeDir = exeDir;
    s.phase = Phase::NeedBIOS;
    // nothing runs until the BIOS and the game disc are in place and the game is built
    host.context->EnqueueEvent(events::emu::SetPaused(true));
    return true;
}

void ApplyWindowIcon(SDL_Window *window) {
    State &s = g_state;
    if (s.phase == Phase::Off || window == nullptr) {
        return;
    }
    // Read through std::filesystem so that non-ASCII install paths work on Windows
    std::vector<char> file;
    {
        std::ifstream in(s.exeDir / "ship" / "icon.png", std::ios::binary);
        if (!in) {
            return;
        }
        file.assign(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
    }
    int w = 0, h = 0, channels = 0;
    stbi_uc *pixels = stbi_load_from_memory(reinterpret_cast<const stbi_uc *>(file.data()),
                                            static_cast<int>(file.size()), &w, &h, &channels, 4);
    if (pixels == nullptr) {
        return;
    }
    if (SDL_Surface *surface = SDL_CreateSurfaceFrom(w, h, SDL_PIXELFORMAT_RGBA32, pixels, w * 4)) {
        SDL_SetWindowIcon(window, surface);
        SDL_DestroySurface(surface);
    }
    stbi_image_free(pixels);
}

bool Enabled() {
    return g_state.phase != Phase::Off;
}

void Start() {
    if (g_state.phase != Phase::NeedBIOS) return;
    PickScannedBIOS();
}

std::string WindowTitle() {
    const State &s = g_state;
    return (s.recipe.ship.empty() ? "Drydock" : s.recipe.ship) + " - " + s.recipe.title;
}

bool ShowYmirMenus() {
    const State &s = g_state;
    return s.phase == Phase::Off || (s.phase == Phase::Ready && s.advanced && !s.menuOpen);
}

bool OnMenuButton() {
    State &s = g_state;
    if (s.phase == Phase::Off) return false;
    if (s.phase != Phase::Ready) return true; // the setup screens have their own buttons
    if (s.aboutOpen) {
        s.aboutOpen = false; // back to the menu
        s.menuFocus = true;
    } else if (s.menuOpen) {
        CloseMenu();
    } else {
        OpenMenu();
    }
    return true;
}

bool BeforeLoadDisc(const fs::path &path) {
    State &s = g_state;
    switch (s.phase.load()) {
    case Phase::Off: return true;
    case Phase::NeedBIOS: {
        std::lock_guard lock(s.mutex);
        s.pendingDisc = path; // loaded once the BIOS is in place
        return false;
    }
    case Phase::Building:
        s.host.context->DisplayMessage("Please wait until " + s.recipe.title + " is ready.");
        return false;
    default: break;
    }

    const drydock::DiscCheck check = drydock::CheckDisc(s.recipe, U8(path));
    if (!check.ok) {
        SetMessage(check.error);
        if (s.phase != Phase::Ready) s.phase = Phase::NeedDisc;
        else s.host.context->DisplayMessage(check.error);
        return false;
    }

    if (!drydock::IsLibraryCurrent(s.recipe, U8(s.exeDir))) {
        StartBuild(path);
        return false;
    }

    ymir_recomp::Runtime *rt = GetRecompRuntime();
    if (rt && rt->Product() != s.recipe.product) {
        std::string error;
        if (!rt->LoadGameLibrary(drydock::LibraryPath(s.recipe, U8(s.exeDir)), error)) {
            {
                std::lock_guard lock(s.mutex);
                s.disc = path;
            }
            SetMessage("Could not load the recompiled game: " + error);
            s.phase = Phase::Failed;
            return false;
        }
        rt->Attach();
    }

    // start the game from a clean boot with the disc in place
    SetMessage("");
    s.phase = Phase::Ready;
    s.host.context->EnqueueEvent(events::emu::HardReset());
    s.host.context->EnqueueEvent(events::emu::SetPaused(false));
    return true;
}

void Draw() {
    State &s = g_state;
    if (s.phase == Phase::Off) return;

    // the BIOS picked in the file dialog
    fs::path picked;
    {
        std::lock_guard lock(s.mutex);
        picked = std::move(s.pickedBIOS);
        s.pickedBIOS.clear();
    }
    if (!picked.empty() && s.phase == Phase::NeedBIOS) InstallBIOS(picked);

    PollBuild();

    if (s.phase != Phase::Ready) {
        DrawSetup();
        return;
    }

    // back to the menu once the controls window is closed
    if (s.controlsFromMenu && !s.host.isSettingsOpen()) {
        s.controlsFromMenu = false;
        s.menuOpen = true;
        s.menuFocus = true;
    }
    if (s.menuOpen) DrawMenu();
}

void Shutdown() {
    State &s = g_state;
    s.cancel = true;
    if (s.worker.joinable()) s.worker.join();
}

} // namespace app::ship
