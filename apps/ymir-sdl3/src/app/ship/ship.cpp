#include "ship.hpp"

#include <app/events/emu_event_factory.hpp>
#include <app/services/disc_service.hpp>
#include <app/shared_context.hpp>

#include <build.hpp> // Drydock: recipe, disc check, game build
#include <ymir_recomp/runtime.hpp>

#include <SDL3/SDL_events.h>
#include <imgui.h>

#include <atomic>
#include <mutex>
#include <thread>

namespace app::ship {

namespace {

    enum class Phase { Off, NeedDisc, Building, Failed, Ready };

    struct State {
        SharedContext *context = nullptr;
        std::filesystem::path exeDir;
        drydock::Recipe recipe;

        std::atomic<Phase> phase{Phase::Off};
        std::mutex mutex;           // guards the strings and paths below
        std::string message;        // why the last disc was refused / the build failed
        std::filesystem::path disc; // disc being built from

        // build worker
        std::thread worker;
        std::atomic<bool> cancel{false};
        std::atomic<bool> workerDone{false};
        bool workerOk = false;
        std::string workerError;
        std::atomic<float> fraction{0.0f};
        std::atomic<drydock::Stage> stage{drydock::Stage::CheckingDisc};
        std::string detail;
    };

    State g_state;

    std::string U8(const std::filesystem::path &p) {
        const std::u8string s = p.u8string();
        return std::string(s.begin(), s.end());
    }

    void SetMessage(std::string msg) {
        std::lock_guard lock(g_state.mutex);
        g_state.message = std::move(msg);
    }

    void StartBuild(const std::filesystem::path &disc) {
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

    void Quit() {
        SDL_Event quit{};
        quit.type = SDL_EVENT_QUIT;
        SDL_PushEvent(&quit);
    }

} // namespace

bool Init(SharedContext &context, const std::filesystem::path &exeDir) {
    State &s = g_state;
    const std::filesystem::path recipePath = exeDir / "ship" / "recipe.toml";
    std::error_code ec;
    if (!std::filesystem::exists(recipePath, ec)) return false;
    std::string error;
    if (!drydock::LoadRecipe(U8(recipePath), s.recipe, error)) {
        context.DisplayMessage(error);
        return false;
    }
    s.context = &context;
    s.exeDir = exeDir;
    s.phase = Phase::NeedDisc;
    // nothing runs until the game disc is validated and the game is built
    context.EnqueueEvent(events::emu::SetPaused(true));
    return true;
}

bool Enabled() {
    return g_state.phase != Phase::Off;
}

std::string WindowTitle() {
    const State &s = g_state;
    return (s.recipe.ship.empty() ? "Drydock" : s.recipe.ship) + " - " + s.recipe.title;
}

bool BeforeLoadDisc(const std::filesystem::path &path) {
    State &s = g_state;
    if (s.phase == Phase::Off) return true;
    if (s.phase == Phase::Building) {
        s.context->DisplayMessage("Please wait until " + s.recipe.title + " is ready.");
        return false;
    }

    const drydock::DiscCheck check = drydock::CheckDisc(s.recipe, U8(path));
    if (!check.ok) {
        SetMessage(check.error);
        if (s.phase != Phase::Ready) s.phase = Phase::NeedDisc;
        else s.context->DisplayMessage(check.error);
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
            SetMessage("Could not load the recompiled game: " + error);
            s.phase = Phase::Failed;
            return false;
        }
        rt->Attach();
    }

    // start the game from a clean boot with the disc in place
    s.phase = Phase::Ready;
    s.context->EnqueueEvent(events::emu::HardReset());
    s.context->EnqueueEvent(events::emu::SetPaused(false));
    return true;
}

void Draw(services::DiscService &discs) {
    State &s = g_state;
    const Phase phase = s.phase;
    if (phase == Phase::Off || phase == Phase::Ready) return;

    if (phase == Phase::Building && s.workerDone) {
        s.worker.join();
        bool ok;
        std::string error;
        std::filesystem::path disc;
        {
            std::lock_guard lock(s.mutex);
            ok = s.workerOk;
            error = s.workerError;
            disc = s.disc;
        }
        if (ok) {
            s.phase = Phase::NeedDisc; // BeforeLoadDisc takes it from here
            s.context->EnqueueEvent(events::emu::LoadDisc(disc));
        } else {
            SetMessage(error);
            s.phase = s.cancel ? Phase::NeedDisc : Phase::Failed;
        }
    }

    const ImGuiViewport *vp = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(ImVec2(vp->WorkPos.x + vp->WorkSize.x * 0.5f, vp->WorkPos.y + vp->WorkSize.y * 0.5f),
                            ImGuiCond_Always, ImVec2(0.5f, 0.5f));
    ImGui::SetNextWindowSizeConstraints(ImVec2(480, 0), ImVec2(720, FLT_MAX));
    const ImGuiWindowFlags flags = ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_AlwaysAutoResize |
                                   ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoMove;
    if (ImGui::Begin((WindowTitle() + "###drydock_ship").c_str(), nullptr, flags)) {
        const std::string game = s.recipe.DisplayName();
        std::string message, detail;
        {
            std::lock_guard lock(s.mutex);
            message = s.message;
            detail = s.detail;
        }
        const ImVec4 errorColor(1.0f, 0.45f, 0.4f, 1.0f);

        if (s.phase == Phase::Building) {
            ImGui::TextWrapped("Preparing %s for this PC. This happens only once and takes a few minutes.",
                               s.recipe.title.c_str());
            ImGui::Spacing();
            ImGui::TextUnformatted(drydock::StageName(s.stage));
            ImGui::ProgressBar(s.fraction, ImVec2(-FLT_MIN, 0));
            ImGui::TextDisabled("%s", detail.c_str());
            ImGui::Spacing();
            if (ImGui::Button("Cancel")) s.cancel = true;
        } else {
            if (s.phase == Phase::Failed) {
                ImGui::TextColored(errorColor, "Could not prepare %s.", s.recipe.title.c_str());
            } else {
                ImGui::TextWrapped("Select your copy of %s (a .cue, .chd, .iso or .ccd disc image).", game.c_str());
            }
            if (!message.empty()) {
                ImGui::Spacing();
                ImGui::PushStyleColor(ImGuiCol_Text, errorColor);
                ImGui::TextWrapped("%s", message.c_str());
                ImGui::PopStyleColor();
            }
            ImGui::Spacing();
            if (ImGui::Button("Select disc...")) discs.OpenLoadDiscDialog();
            if (s.phase == Phase::Failed) {
                ImGui::SameLine();
                std::filesystem::path disc;
                {
                    std::lock_guard lock(s.mutex);
                    disc = s.disc;
                }
                if (ImGui::Button("Retry") && !disc.empty()) {
                    s.phase = Phase::NeedDisc;
                    s.context->EnqueueEvent(events::emu::LoadDisc(disc));
                }
            }
            ImGui::SameLine();
            if (ImGui::Button("Quit")) Quit();
        }
    }
    ImGui::End();
}

void Shutdown() {
    State &s = g_state;
    s.cancel = true;
    if (s.worker.joinable()) s.worker.join();
}

} // namespace app::ship
