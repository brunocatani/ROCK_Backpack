#include "PCH.h"

#include "BackpackRuntime.h"
#include "NativeItemPreview.h"
#include "BackpackStorage.h"
#include "BackpackWeight.h"
#include "NativeInventoryAdmission.h"
#include "NativePipboyInventory.h"

#include <ShlObj.h>

namespace
{
    bool startupReady{}, runtimeRejected{}; // F4SE message thread only.
    bool rejectExperimentalRuntime() noexcept
    {
        if (!runtimeRejected && GetModuleHandleW(L"ROCK_V2.dll")) {
            runtimeRejected = true;
            logger::critical("ROCK Backpack requires regular ROCK.dll and refuses ROCK_V2.dll, including when both are loaded. Addon disabled.");
        }
        return runtimeRejected;
    }
    bool initializeRegularRuntime() noexcept
    {
        if (rejectExperimentalRuntime() || !GetModuleHandleW(L"ROCK.dll")) {
            logger::critical("ROCK Backpack startup refused: regular ROCK is required and ROCK V2 must be absent.");
            return false;
        }
        try {
            if (!rock_backpack::BackpackStorage::get().initialize() || !rock_backpack::backpack_weight::install()) {
                logger::error("Backpack native storage/weight initialization failed; addon stays inactive"); return false;
            }
            if(!rock_backpack::item_preview::install())
                logger::warn("Native previews unavailable; backpack storage and named inventory rows remain active");
            if (!rock_backpack::native_admission::install()) {
                logger::error("Native inventory admission unavailable; backpack cannot enforce capacity safely"); return false;
            }
            if(!rock_backpack::native_pipboy::install()) logger::warn("Pip-Boy inventory filtering unavailable; original pages retained");
            return true;
        } catch (...) {
            logger::critical("ROCK Backpack startup failed; addon stays inactive"); return false;
        }
    }
    std::optional<std::filesystem::path> getFallout4VRF4SEDirectory()
    {
        char documents[MAX_PATH]{};
        if (FAILED(SHGetFolderPathA(nullptr, CSIDL_MYDOCUMENTS, nullptr, SHGFP_TYPE_CURRENT, documents))) {
            return std::nullopt;
        }

        std::filesystem::path path = documents;
        path /= "My Games";
        path /= "Fallout4VR";
        path /= "F4SE";
        std::error_code ec;
        std::filesystem::create_directories(path, ec);
        return ec ? std::nullopt : std::optional<std::filesystem::path>{ path };
    }

    void f4seMessageHandler(F4SE::MessagingInterface::Message* message) noexcept
    {
        if (!message) {
            return;
        }

        // Recheck after DLL enumeration, before installing hooks or registering
        // serialization. A later-loaded V2 must not bypass the Load guard.
        if (rejectExperimentalRuntime()) return;
        if (message->type == F4SE::MessagingInterface::kGameDataReady && !startupReady)
            startupReady = initializeRegularRuntime();
        if (!startupReady) return;
        auto& runtime = rock_backpack::BackpackRuntime::get();
        switch (message->type) {
        case F4SE::MessagingInterface::kGameDataReady:
            runtime.onGameDataReady();
            break;
        case F4SE::MessagingInterface::kPreLoadGame:
            rock_backpack::item_preview::setSessionReady(false);
            runtime.onPreLoadGame();
            break;
        case F4SE::MessagingInterface::kPostLoadGame:
            rock_backpack::item_preview::setSessionReady(true);
            runtime.onGameSessionReady(false);
            break;
        case F4SE::MessagingInterface::kNewGame:
            rock_backpack::item_preview::setSessionReady(true);
            runtime.onGameSessionReady(true);
            break;
        default:
            break;
        }
    }

    void reportPluginBoundaryFailure(const char* boundary, const char* detail) noexcept
    {
        char message[512]{};
        const int written = std::snprintf(
            message,
            sizeof(message),
            "ROCK Backpack: unhandled exception in %s%s%s\n",
            boundary ? boundary : "plugin boundary",
            detail ? ": " : "",
            detail ? detail : "");
        if (written > 0) {
            ::OutputDebugStringA(message);
        }
    }
}

extern "C" DLLEXPORT bool F4SEAPI F4SEPlugin_Query(
    const F4SE::QueryInterface* a_f4se,
    F4SE::PluginInfo* a_info) noexcept
{
    try {
        if (!a_f4se || !a_info) {
            return false;
        }
        auto path = getFallout4VRF4SEDirectory();
        if (!path) {
            return false;
        }
        *path /= "ROCKBackpack.log";
        auto sink = std::make_shared<spdlog::sinks::basic_file_sink_mt>(path->string(), true);
        auto log = std::make_shared<spdlog::logger>("global log"s, std::move(sink));
        log->set_level(spdlog::level::info);
        log->flush_on(spdlog::level::info);
        spdlog::set_default_logger(std::move(log));
        spdlog::set_pattern("[%Y-%m-%d %T.%e] [%l] [%t] [%s:%#] %v");

        logger::info("ROCK Backpack v0.1.0 query.");

        a_info->infoVersion = F4SE::PluginInfo::kVersion;
        a_info->name = "ROCKBackpack";
        a_info->version = 1;

        if (a_f4se->IsEditor()) {
            logger::critical("ROCK Backpack loaded in the editor; aborting.");
            return false;
        }
        if (!REL::Module::IsVR()) {
            logger::critical("ROCK Backpack requires Fallout 4 VR; aborting.");
            return false;
        }
        // F4SEVR exposes a flat-compatible runtimeVersion value (observed as
        // 1.10.138.0) through QueryInterface. REL::Module reads Fallout4VR.exe's
        // actual file version and is therefore the correct compatibility authority.
        const auto executableVersion = REL::Module::get().version();
        if (executableVersion != F4SE::RUNTIME_VR_1_2_72) {
            logger::critical(
                "ROCK Backpack supports only Fallout 4 VR executable 1.2.72; executable={}, F4SE compatibility runtime={}.",
                executableVersion.string(), a_f4se->RuntimeVersion().string());
            return false;
        }
        logger::info(
            "ROCK Backpack runtime gate passed: Fallout4VR.exe {}, F4SE compatibility runtime {}.",
            executableVersion.string(), a_f4se->RuntimeVersion().string());
        return true;
    } catch (const std::exception& error) {
        reportPluginBoundaryFailure("F4SEPlugin_Query", error.what());
        return false;
    } catch (...) {
        reportPluginBoundaryFailure("F4SEPlugin_Query", nullptr);
        return false;
    }
}

extern "C" DLLEXPORT bool F4SEAPI F4SEPlugin_Load(const F4SE::LoadInterface* a_f4se) noexcept
{
    bool resident{};
    try {
        if (!a_f4se) {
            return false;
        }
        F4SE::Init(a_f4se, false);
        logger::info("ROCK Backpack load.");
        if (rejectExperimentalRuntime()) return false;

        const auto* messaging = F4SE::GetMessagingInterface();
        if (!messaging) {
            logger::critical("ROCK Backpack failed to acquire the F4SE messaging interface.");
            return false;
        }
        if (!messaging->RegisterListener(f4seMessageHandler)) {
            logger::critical("ROCK Backpack failed to register the F4SE message listener.");
            return false;
        }
        resident = true;

        logger::info("ROCK Backpack listener registered; regular-ROCK-only initialization waits for game-data-ready.");
        return true;
    } catch (const std::exception& error) {
        reportPluginBoundaryFailure("F4SEPlugin_Load", error.what());
        return resident;
    } catch (...) {
        reportPluginBoundaryFailure("F4SEPlugin_Load", nullptr);
        return resident;
    }
}
