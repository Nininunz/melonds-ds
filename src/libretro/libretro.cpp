/*
    Copyright 2023 Jesse Talavera-Greenberg

    melonDS DS is free software: you can redistribute it and/or modify it under
    the terms of the GNU General Public License as published by the Free
    Software Foundation, either version 3 of the License, or (at your option)
    any later version.

    melonDS DS is distributed in the hope that it will be useful, but WITHOUT ANY
    WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS
    FOR A PARTICULAR PURPOSE. See the GNU General Public License for more details.

    You should have received a copy of the GNU General Public License along
    with melonDS DS. If not, see http://www.gnu.org/licenses/.
*/

#include "libretro.hpp"

// NOT UNUSED; GPU.h doesn't #include OpenGL, so I do it here.
// This must come before <GPU.h>!
#include "PlatformOGLPrivate.h"

#include <cstdlib>
#include <cstring>
#include <ctime>
#include <memory>

#include <compat/strl.h>
#include <file/file_path.h>
#include <libretro.h>
#include <retro_assert.h>
#include <retro_miscellaneous.h>
#include <streams/rzip_stream.h>

#include <DSi.h>
#include <DSi_I2C.h>
#include <frontend/FrontendUtil.h>
#include <GBACart.h>
#include <GPU.h>
#include <NDS.h>
#include <NDSCart.h>
#include <Platform.h>
#include <SPI.h>
#include <SPU.h>

#include "config.hpp"
#include "content.hpp"
#include "dsi.hpp"
#include "environment.hpp"
#include "exceptions.hpp"
#include "file.hpp"
#include "info.hpp"
#include "input.hpp"
#include "memory.hpp"
#include "microphone.hpp"
#include "opengl.hpp"
#include "power.hpp"
#include "render.hpp"
#include "retro/task_queue.hpp"
#include "screenlayout.hpp"
#include "sram.hpp"
#include "tracy.hpp"

using std::make_optional;
using std::optional;
using std::nullopt;
using retro::task::TaskSpec;

namespace melonds {
    static InputState input_state;
    static ScreenLayoutData screenLayout;
    static bool mic_state_toggled = false;
    static bool isInDeinit = false;
    static bool isUnloading = false;
    static bool deferred_initialization_pending = false;
    static bool first_frame_run = false;
    static std::unique_ptr<NdsCart> _loaded_nds_cart;
    static std::unique_ptr<GbaCart> _loaded_gba_cart;
    static const char *const INTERNAL_ERROR_MESSAGE =
        "An internal error occurred with melonDS DS. "
        "Please contact the developer with the log file.";

    static const char *const UNKNOWN_ERROR_MESSAGE =
        "An unknown error has occurred with melonDS DS. "
        "Please contact the developer with the log file.";

    // functions for loading games
    static bool handle_load_game(unsigned type, const struct retro_game_info *info, size_t num) noexcept;
    static void load_games(
        const optional<retro_game_info> &nds_info,
        const optional<retro_game_info> &gba_info,
        const optional<retro_game_info> &gba_save_info
    );
    static void parse_nds_rom(const struct retro_game_info &info);
    static void parse_gba_rom(const struct retro_game_info &info);
    static void init_rendering();
    static void load_games_deferred(
        const optional<retro_game_info>& nds_info,
        const optional<retro_game_info>& gba_info
    );
    static void set_up_direct_boot(const retro_game_info &nds_info);

    // functions for running games
    static void read_microphone(melonds::InputState& inputState) noexcept;
    static void render_audio();


    bool IsUnloadingGame() noexcept
    {
        return isUnloading;
    }

    bool IsInDeinit() noexcept
    {
        return isInDeinit;
    }
    retro::task::TaskSpec OnScreenDisplayTask() noexcept;
}

PUBLIC_SYMBOL void retro_init(void) {
    ZoneScopedN("retro_init");
    retro::log(RETRO_LOG_DEBUG, "retro_init");
    retro_assert(melonds::_loaded_nds_cart == nullptr);
    retro_assert(melonds::_loaded_gba_cart == nullptr);
    retro_assert(retro::content::get_loaded_nds_info() == nullopt);
    retro_assert(retro::content::get_loaded_gba_info() == nullopt);
    retro_assert(retro::content::get_loaded_gba_save_info() == nullopt);
    retro_assert(!melonds::first_frame_run);
    retro_assert(!melonds::deferred_initialization_pending);
    retro_assert(!melonds::isInDeinit);
    retro_assert(!melonds::isUnloading);
    retro_assert(!melonds::mic_state_toggled);
    srand(time(nullptr));
    melonds::input_state = melonds::InputState();
    melonds::sram::init();


    melonds::file::init();
    melonds::first_frame_run = false;
    retro::task::init(false, nullptr);

    // ScreenLayoutData is initialized in its constructor
}

static bool melonds::handle_load_game(unsigned type, const struct retro_game_info *info, size_t num) noexcept try {
    ZoneScopedN("melonds::handle_load_game");
    retro_assert(_loaded_nds_cart == nullptr);
    retro_assert(_loaded_gba_cart == nullptr);
    retro_assert(retro::content::get_loaded_nds_info() == nullopt);
    retro_assert(retro::content::get_loaded_gba_info() == nullopt);
    retro_assert(retro::content::get_loaded_gba_save_info() == nullopt);

    // First initialize the content info...
    switch (type) {
        case melonds::MELONDSDS_GAME_TYPE_NDS:
            // ...which refers to a Nintendo DS game...
            retro::content::set_loaded_content_info(info, nullptr);
            break;
        case melonds::MELONDSDS_GAME_TYPE_SLOT_1_2_BOOT:
            // ...which refers to both a Nintendo DS and Game Boy Advance game...
            switch (num) {
                case 2: // NDS ROM and GBA ROM
                    retro::content::set_loaded_content_info(info, info + 1);
                    break;
                case 3: // NDS ROM, GBA ROM, and GBA SRAM
                    retro::content::set_loaded_content_info(info, info + 1, info + 2);
                    break;
                default:
                    retro::log(RETRO_LOG_ERROR, "Invalid number of ROMs (%u) for slot-1/2 boot", num);
                    retro::set_error_message(melonds::INTERNAL_ERROR_MESSAGE);
            }
            break;
        default:
            retro::log(RETRO_LOG_ERROR, "Unknown game type %d", type);
            retro::set_error_message(melonds::INTERNAL_ERROR_MESSAGE);
            return false;
    }

    // ...then load the game.
    melonds::load_games(
        retro::content::get_loaded_nds_info(),
        retro::content::get_loaded_gba_info(),
        retro::content::get_loaded_gba_save_info()
    );

    return true;
}
catch (const melonds::emulator_exception &e) {
    // Thrown for invalid ROMs
    retro::error("%s", e.what());
    retro::set_error_message(e.user_message());
    _loaded_nds_cart.reset();
    _loaded_gba_cart.reset();
    return false;
}
catch (const std::exception &e) {
    retro::log(RETRO_LOG_ERROR, "%s", e.what());
    retro::set_error_message(melonds::INTERNAL_ERROR_MESSAGE);
    _loaded_nds_cart.reset();
    _loaded_gba_cart.reset();
    return false;
}
catch (...) {
    retro::set_error_message(melonds::UNKNOWN_ERROR_MESSAGE);
    _loaded_nds_cart.reset();
    _loaded_gba_cart.reset();
    return false;
}

PUBLIC_SYMBOL bool retro_load_game(const struct retro_game_info *info) {
    ZoneScopedN("retro_load_game");
    if (info) {
        retro::debug("retro_load_game(\"%s\", %d)", info->path, info->size);
    }
    else {
        retro::debug("retro_load_game(<no content>)");
    }

    return melonds::handle_load_game(melonds::MELONDSDS_GAME_TYPE_NDS, info, 1);
}

PUBLIC_SYMBOL void retro_get_system_av_info(struct retro_system_av_info *info) {
    ZoneScopedN("retro_get_system_av_info");
    using melonds::screenLayout;

    retro_assert(melonds::render::CurrentRenderer() != melonds::Renderer::None);

    info->timing.fps = 32.0f * 1024.0f * 1024.0f / 560190.0f;
    info->timing.sample_rate = 32.0f * 1024.0f;
    info->geometry = screenLayout.Geometry(melonds::render::CurrentRenderer());
}

PUBLIC_SYMBOL [[gnu::hot]] void retro_run(void) {
    {
        ZoneScopedN("retro_run");
        using namespace melonds;
        using retro::log;

        if (deferred_initialization_pending) {
            try {
                log(RETRO_LOG_DEBUG, "Starting deferred initialization");
                melonds::load_games_deferred(
                        retro::content::get_loaded_nds_info(),
                        retro::content::get_loaded_gba_info()
                );
                deferred_initialization_pending = false;

                log(RETRO_LOG_DEBUG, "Completed deferred initialization");
            }
            catch (const melonds::emulator_exception &e) {
                log(RETRO_LOG_ERROR, "Deferred initialization failed; exiting core");
                retro::error("%s", e.what());
                retro::set_error_message(e.user_message());
                retro::shutdown();
                return;
            }
            catch (const std::exception &e) {
                log(RETRO_LOG_ERROR, "Deferred initialization failed; exiting core");
                retro::set_error_message(e.what());
                retro::shutdown();
                return;
            }
            catch (...) {
                log(RETRO_LOG_ERROR, "Deferred initialization failed; exiting core");
                retro::set_error_message(UNKNOWN_ERROR_MESSAGE);
                retro::shutdown();
                return;
            }
        }

        if (!first_frame_run) {
            using namespace sram;
            // Apply the save data from the core's SRAM buffer to the cart's SRAM;
            // we need to do this in the first frame of retro_run because
            // retro_get_memory_data is used to copy the loaded SRAM
            // in between retro_load and the first retro_run call.

            // Nintendo DS SRAM is loaded by the frontend
            // and copied into NdsSaveManager via the pointer returned by retro_get_memory.
            // This is where we install the SRAM data into the emulated DS.
            if (retro::content::get_loaded_nds_info() && NdsSaveManager && NdsSaveManager->SramLength() > 0) {
                // If we're loading a NDS game that has SRAM...
                NDS::LoadSave(NdsSaveManager->Sram(), NdsSaveManager->SramLength());
            }

            // GBA SRAM is selected by the user explicitly (due to libretro limits) and loaded by the frontend,
            // but is not processed by retro_get_memory (again due to libretro limits).
            if (retro::content::get_loaded_nds_info() && GbaSaveManager && GbaSaveManager->SramLength() > 0) {
                // If we're loading a GBA game that has existing SRAM...
                // TODO: Decide what to do about SRAM files that append extra metadata like the RTC
                GBACart::LoadSave(GbaSaveManager->Sram(), GbaSaveManager->SramLength());
            }

            // We could've installed the GBA's SRAM in retro_load_game (since it's not processed by retro_get_memory),
            // but doing so here helps keep things tidier since the NDS SRAM is installed here too.

            // This has to be deferred even if we're not using OpenGL,
            // because libretro doesn't set the SRAM until after retro_load_game
            first_frame_run = true;
        }

        if (retro::is_variable_updated()) {
            // If any settings have changed...
            melonds::UpdateConfig(screenLayout, input_state);
        }

        if (melonds::render::ReadyToRender()) {
            // If the global state needed for rendering is ready...
            HandleInput(input_state, screenLayout);
            read_microphone(input_state);

            if (screenLayout.Dirty()) {
                // If the active screen layout has changed (either by settings or by hotkey)...
                Renderer renderer = melonds::render::CurrentRenderer();
                retro_assert(renderer != Renderer::None);

                // Apply the new screen layout
                screenLayout.Update(renderer);

                // And update the geometry
                retro_game_geometry geometry = screenLayout.Geometry(renderer);
                if (!retro::environment(RETRO_ENVIRONMENT_SET_GEOMETRY, &geometry)) {
                    retro::warn("Failed to update geometry after screen layout change");
                }

                melonds::opengl::RequestOpenGlRefresh();
            }

            // NDS::RunFrame renders the Nintendo DS state to a framebuffer,
            // which is then drawn to the screen by melonds::render::Render
            {
                ZoneScopedN("NDS::RunFrame");
                NDS::RunFrame();
            }

            render::Render(input_state, screenLayout);
            melonds::render_audio();

            retro::task::check();
        }
    }
    FrameMark;
}

static void melonds::read_microphone(melonds::InputState& inputState) noexcept {
    ZoneScopedN("melonds::read_microphone");
    MicInputMode mic_input_mode = config::audio::MicInputMode();
    MicButtonMode mic_button_mode = config::audio::MicButtonMode();
    bool should_mic_be_on = false;

    switch (mic_button_mode) {
        // If the microphone button...
        case MicButtonMode::Hold: {
            // ...must be held...
            mic_state_toggled = false;
            if (!inputState.MicButtonDown()) {
                // ...but it isn't...
                mic_input_mode = MicInputMode::None;
            }
            should_mic_be_on = inputState.MicButtonDown();
            break;
        }
        case MicButtonMode::Toggle: {
            // ...must be toggled...
            if (inputState.MicButtonPressed()) {
                // ...but it isn't...
                mic_state_toggled = !mic_state_toggled;
                if (!mic_state_toggled) {
                    mic_input_mode = MicInputMode::None;
                }
            }
            should_mic_be_on = mic_state_toggled;
            break;
        }
        case MicButtonMode::Always: {
            // ...is unnecessary...
            // Do nothing, the mic input mode is already set
            should_mic_be_on = true;
            break;
        }
    }

    if (retro::microphone::is_open()) {
        // TODO: Don't set constantly, only when the mic button state changes (or when the input mode is set to Always)
        retro::microphone::set_state(should_mic_be_on);
    }

    switch (mic_input_mode) {
        case MicInputMode::WhiteNoise: // random noise
        {
            s16 tmp[735];
            for (int i = 0; i < 735; i++) tmp[i] = rand() & 0xFFFF;
            NDS::MicInputFrame(tmp, 735);
            break;
        }
        case MicInputMode::BlowNoise: // blow noise
        {
            Frontend::Mic_FeedNoise(); // despite the name, this feeds a blow noise
            break;
        }
        case MicInputMode::HostMic: // microphone input
        {
            // TODO: Consider switching to Mic_FeedExternalBuffer
            const auto mic_state = retro::microphone::get_state();
            if (mic_state.has_value() && mic_state.value()) {
                // If the microphone is open and turned on...
                s16 samples[735];
                retro::microphone::read(samples, ARRAY_SIZE(samples));
                NDS::MicInputFrame(samples, ARRAY_SIZE(samples));
                break;
            }
            // If the mic isn't available, feed silence instead
        }
        // Intentional fall-through
        default:
            Frontend::Mic_FeedSilence();
    }
}

static void melonds::render_audio() {
    ZoneScopedN("melonds::render_audio");
    static int16_t audio_buffer[0x1000]; // 4096 samples == 2048 stereo frames
    u32 size = std::min(SPU::GetOutputSize(), static_cast<int>(sizeof(audio_buffer) / (2 * sizeof(int16_t))));
    // Ensure that we don't overrun the buffer

    size_t read = SPU::ReadOutput(audio_buffer, size);
    retro::audio_sample_batch(audio_buffer, read);
}

namespace NDS {
    extern bool Running;
}

PUBLIC_SYMBOL void retro_unload_game(void) {
    ZoneScopedN("retro_unload_game");
    melonds::isUnloading = true;
    retro::log(RETRO_LOG_DEBUG, "retro_unload_game()");
    // No need to flush SRAM to the buffer, Platform::WriteNDSSave has been doing that for us this whole time
    // No need to flush the homebrew save data either, the CartHomebrew destructor does that

    // The cleanup handlers for each task will flush data to disk if needed
    retro::task::reset();
    retro::task::wait();
    retro::task::deinit();

    if (NDS::Running)
    { // If the NDS wasn't already stopped due to some internal event...
        ZoneScopedN("NDS::Stop");
        NDS::Stop();
    }
    {
        ZoneScopedN("NDS::DeInit");
        NDS::DeInit();
    }

    const optional<struct retro_game_info>& nds_info = retro::content::get_loaded_nds_info();
    if (nds_info && melonds::_loaded_nds_cart && melonds::_loaded_nds_cart->GetHeader().IsDSiWare()) {
        melonds::dsi::uninstall_dsiware(*nds_info, *melonds::_loaded_nds_cart);
    }

    melonds::_loaded_nds_cart.reset();
    melonds::_loaded_gba_cart.reset();
    melonds::isUnloading = false;
}

PUBLIC_SYMBOL unsigned retro_get_region(void) {
    return RETRO_REGION_NTSC;
}

PUBLIC_SYMBOL bool retro_load_game_special(unsigned type, const struct retro_game_info *info, size_t num) {
    ZoneScopedN("retro_load_game_special");
    retro::log(RETRO_LOG_DEBUG, "retro_load_game_special(%s, %p, %u)", melonds::get_game_type_name(type), info, num);

    return melonds::handle_load_game(type, info, num);
}

// We deinitialize all these variables just in case the frontend doesn't unload the dynamic library.
// It might be keeping the library around for debugging purposes,
// or it might just be buggy.
PUBLIC_SYMBOL void retro_deinit(void) {
    ZoneScopedN("retro_deinit");
    melonds::isInDeinit = true;
    retro::log(RETRO_LOG_DEBUG, "retro_deinit()");
    retro::task::deinit();
    melonds::file::deinit();
    retro::clear_environment();
    retro::content::clear();
    melonds::clear_memory_config();
    melonds::_loaded_nds_cart.reset();
    melonds::_loaded_gba_cart.reset();
    Platform::DeInit();
    melonds::sram::deinit();
    melonds::mic_state_toggled = false;
    melonds::isUnloading = false;
    melonds::deferred_initialization_pending = false;
    melonds::first_frame_run = false;
    melonds::isInDeinit = false;
}

PUBLIC_SYMBOL unsigned retro_api_version(void) {
    return RETRO_API_VERSION;
}

PUBLIC_SYMBOL void retro_get_system_info(struct retro_system_info *info) {
    info->library_name = MELONDSDS_NAME;
    info->block_extract = false;
    info->library_version = MELONDSDS_VERSION;
    info->need_fullpath = false;
    info->valid_extensions = "nds|ids|dsi";
}

PUBLIC_SYMBOL void retro_reset(void) {
    ZoneScopedN("retro_reset");
    retro::log(RETRO_LOG_DEBUG, "retro_reset()\n");

    {
        ZoneScopedN("NDS::Reset");
        NDS::Reset();
    }

    melonds::first_frame_run = false;

    const auto &nds_info = retro::content::get_loaded_nds_info();
    if (nds_info && melonds::_loaded_nds_cart && !melonds::_loaded_nds_cart->GetHeader().IsDSiWare()) {
        melonds::set_up_direct_boot(nds_info.value());
    }
}

static void melonds::parse_nds_rom(const struct retro_game_info &info) {
    ZoneScopedN("melonds::parse_nds_rom");
    _loaded_nds_cart = NDSCart::ParseROM(static_cast<const u8 *>(info.data), static_cast<u32>(info.size));

    if (!_loaded_nds_cart) {
        throw invalid_rom_exception("Failed to parse the DS ROM image. Is it valid?");
    }

    retro::log(RETRO_LOG_DEBUG, "Loaded NDS ROM: \"%s\"", info.path);
}

static void melonds::parse_gba_rom(const struct retro_game_info &info) {
    ZoneScopedN("melonds::parse_gba_rom");
    _loaded_gba_cart = GBACart::ParseROM(static_cast<const u8 *>(info.data), static_cast<u32>(info.size));

    if (!_loaded_gba_cart) {
        throw invalid_rom_exception("Failed to parse the GBA ROM image. Is it valid?");
    }

    retro::log(RETRO_LOG_DEBUG, "Loaded GBA ROM: \"%s\"", info.path);
}


static void melonds::load_games(
    const optional<struct retro_game_info> &nds_info,
    const optional<struct retro_game_info> &gba_info,
    const optional<struct retro_game_info> &gba_save_info
) {
    ZoneScopedN("melonds::load_games");
    melonds::clear_memory_config();
    NDSHeader header;
    if (nds_info) {
        // Need to get the header before parsing the ROM,
        // as parsing the ROM can depend on the config
        // but the config can depend on the header.
        memcpy(&header, nds_info->data, sizeof(header));
    }
    melonds::InitConfig(nds_info, nds_info ? make_optional(header) : nullopt, screenLayout, input_state);

    Platform::Init(0, nullptr);

    if (retro::supports_power_status())
    {
        retro::task::push(melonds::power::PowerStatusUpdateTask());
    }

    retro::task::push(melonds::OnScreenDisplayTask());

    using retro::environment;
    using retro::log;
    using retro::set_message;

    retro_assert(_loaded_nds_cart == nullptr);
    retro_assert(_loaded_gba_cart == nullptr);

    // First parse the ROMs...
    if (nds_info) {
        // NDS::Reset() calls wipes the cart buffer so on invoke we need a reload from info->data.
        // Since retro_reset callback doesn't pass the info struct we need to cache it.
        parse_nds_rom(*nds_info);

        // sanity check; parse_nds_rom does the real validation
        retro_assert(_loaded_nds_cart != nullptr);

        if (!_loaded_nds_cart->GetHeader().IsDSiWare()) {
            // If this ROM represents a cartridge, rather than DSiWare...
            sram::InitNdsSave(*_loaded_nds_cart);
        }
    }

    if (gba_info) {
        if (config::system::ConsoleType() == ConsoleType::DSi) {
            // TODO: What if I force DS mode when using GBA SRAM?
            retro::set_warn_message("The DSi does not support GBA connectivity. Not loading the requested GBA ROM or SRAM.");
        } else {
            parse_gba_rom(*gba_info);

            if (gba_save_info) {
                sram::InitGbaSram(*_loaded_gba_cart, *gba_save_info);
            }
            else {
                retro::info("No GBA SRAM was provided.");
            }
        }
    }

    if (config::system::ConsoleType() == ConsoleType::DSi || (_loaded_nds_cart && _loaded_nds_cart->GetHeader().IsHomebrew() && config::save::DldiEnable() && !config::save::DldiReadOnly())) {
        // If we're dealing with any FAT filesystem (because of the DSi or because of homebrew)...
        retro::task::push(file::FlushTask());
    }

    retro::task::push(sram::FlushFirmwareTask(config::system::EffectiveFirmwarePath()));

    if (!config::system::ExternalBiosEnable() && _loaded_gba_cart) {
        // If we're using FreeBIOS and are trying to load a GBA cart...
        retro::set_warn_message(
            "FreeBIOS does not support GBA connectivity. "
            "Please install a native BIOS and enable it in the options menu."
        );
    }

    environment(RETRO_ENVIRONMENT_SET_INPUT_DESCRIPTORS, (void *) &melonds::input_descriptors);

    init_rendering();

    bool ok;
    {
        ZoneScopedN("NDS::Init");
        ok = NDS::Init();
    }
    if (!ok) {
        retro::log(RETRO_LOG_ERROR, "Failed to initialize melonDS DS.");
        throw std::runtime_error("Failed to initialize NDS emulator.");
    }

    SPU::SetInterpolation(static_cast<int>(config::audio::Interpolation()));
    NDS::SetConsoleType(static_cast<int>(config::system::ConsoleType()));

    if (render::CurrentRenderer() == Renderer::OpenGl) {
        log(RETRO_LOG_INFO, "Deferring initialization until the OpenGL context is ready");
        deferred_initialization_pending = true;
    } else {
        log(RETRO_LOG_INFO, "No need to defer initialization, proceeding now");
        load_games_deferred(nds_info, gba_info);
    }
}

// melonDS tightly couples the renderer with the rest of the emulation code,
// so we can't initialize the emulator until the OpenGL context is ready.
static void melonds::load_games_deferred(
    const optional<retro_game_info>& nds_info,
    const optional<retro_game_info>& gba_info
) {
    ZoneScopedN("melonds::load_games_deferred");
    using retro::log;

    // GPU config must be initialized before NDS::Reset is called.
    // Ensure that there's a renderer, even if we're about to throw it out.
    // (GPU::SetRenderSettings may try to deinitialize a non-existing renderer)
    bool isOpenGl = render::CurrentRenderer() == Renderer::OpenGl;
    {
        ZoneScopedN("GPU::InitRenderer");
        GPU::InitRenderer(isOpenGl);
    }
    {
        GPU::RenderSettings render_settings = config::video::RenderSettings();
        ZoneScopedN("GPU::SetRenderSettings");
        GPU::SetRenderSettings(isOpenGl, render_settings);
    }

    // Loads the BIOS, too
    {
        ZoneScopedN("NDS::Reset");
        NDS::Reset();
    }

    // The ROM must be inserted after NDS::Reset is called

    retro_assert(NDSCart::Cart == nullptr);

    if (_loaded_nds_cart) {
        // If we want to insert a NDS ROM that was previously loaded...

        if (!_loaded_nds_cart->GetHeader().IsDSiWare()) {
            // If we're running a physical cartridge...

            bool ok;
            {
                ZoneScopedN("NDSCart::InsertROM");
                ok = NDSCart::InsertROM(std::move(_loaded_nds_cart));
            }
            if (!ok) {
                // If we failed to insert the ROM, we can't continue
                retro::log(RETRO_LOG_ERROR, "Failed to insert \"%s\" into the emulator. Exiting.", nds_info->path);
                throw std::runtime_error("Failed to insert the loaded ROM. Please report this issue.");
            }

            // Just to be sure
            retro_assert(_loaded_nds_cart == nullptr);
            retro_assert(NDSCart::Cart != nullptr);
        }
        else {
            // We're running a DSiWare game, then
            bool ok;
            {
                ZoneScopedN("DSi::LoadNAND");
                ok = DSi::LoadNAND();
            }
            if (!ok) {
                throw std::runtime_error("Failed to load NAND. Please report this issue.");
            }
            melonds::dsi::install_dsiware(*nds_info, *_loaded_nds_cart);
        }
    }

    retro_assert(GBACart::Cart == nullptr);

    if (gba_info && _loaded_gba_cart) {
        // If we want to insert a GBA ROM that was previously loaded...
        bool inserted;
        {
            ZoneScopedN("GBACart::InsertROM");
            inserted = GBACart::InsertROM(std::move(_loaded_gba_cart));
        }
        if (!inserted) {
            // If we failed to insert the ROM, we can't continue
            retro::log(RETRO_LOG_ERROR, "Failed to insert \"%s\" into the emulator. Exiting.", gba_info->path);
            throw std::runtime_error("Failed to insert the loaded ROM. Please report this issue.");
        }

        retro_assert(_loaded_gba_cart == nullptr);
    }

    if (nds_info && NDSCart::Cart && !NDSCart::Cart->GetHeader().IsDSiWare()) {
        set_up_direct_boot(*nds_info);
    }

    NDS::Start();

    log(RETRO_LOG_INFO, "Initialized emulated console and loaded emulated game");
}

static void melonds::init_rendering() {
    ZoneScopedN("melonds::init_rendering");
    using retro::environment;
    using retro::log;

    enum retro_pixel_format fmt = RETRO_PIXEL_FORMAT_XRGB8888;
    if (!environment(RETRO_ENVIRONMENT_SET_PIXEL_FORMAT, &fmt)) {
        throw std::runtime_error("Failed to set the required XRGB8888 pixel format for rendering; it may not be supported.");
    }

    melonds::render::Initialize(config::video::ConfiguredRenderer());

}

// Decrypts the ROM's secure area
static void melonds::set_up_direct_boot(const retro_game_info &nds_info) {
    ZoneScopedN("melonds::set_up_direct_boot");
    if (config::system::DirectBoot() || NDS::NeedsDirectBoot()) {
        char game_name[256];
        const char *ptr = path_basename(nds_info.path);
        if (ptr)
            strlcpy(game_name, ptr, sizeof(game_name));
        else
            strlcpy(game_name, nds_info.path, sizeof(game_name));

        {
            ZoneScopedN("NDS::SetupDirectBoot");
            NDS::SetupDirectBoot(game_name);
        }
        retro::log(RETRO_LOG_DEBUG, "Initialized direct boot for \"%s\"", game_name);
    }
}

retro::task::TaskSpec melonds::OnScreenDisplayTask() noexcept {
    return retro::task::TaskSpec([](retro::task::TaskHandle&) noexcept {
        using std::to_string;
        ZoneScopedN("melonds::OnScreenDisplayTask");
        constexpr const char* const OSD_DELIMITER = " || ";
        constexpr const char* const OSD_YES = "✔";
        constexpr const char* const OSD_NO = "✘";

        std::string text;
        text.reserve(1024);

        if (config::osd::ShowPointerCoordinates()) {
            glm::i16vec2 pointerInput = input_state.PointerInput();
            glm::ivec2 touch = input_state.TouchPosition();
            text += "Pointer: (" + to_string(pointerInput.x) + ", " + to_string(pointerInput.y) + ") → (" + to_string(touch.x) + ", " + to_string(touch.y) + ")";
        }

        if (config::osd::ShowMicState()) {
            optional<bool> mic_state = retro::microphone::get_state();

            if (mic_state && *mic_state) {
                // If the microphone is open and turned on...
                if (!text.empty()) {
                    text += OSD_DELIMITER;
                }

                // Toggle between a filled circle and an empty one every 1.5 seconds
                // (kind of like a blinking "recording" light)
                text += (NDS::NumFrames % 120 > 60) ? "●" : "○";
            }
        }

        if (config::osd::ShowCurrentLayout()) {
            if (!text.empty()) {
                text += OSD_DELIMITER;
            }

            unsigned layout = screenLayout.LayoutIndex();
            unsigned numberOfLayouts = screenLayout.NumberOfLayouts();

            text += "Layout " + to_string(layout + 1) + "/" + to_string(numberOfLayouts);
        }

        if (config::osd::ShowLidState() && NDS::IsLidClosed()) {
            if (!text.empty()) {
                text += OSD_DELIMITER;
            }

            text += "Closed";
        }

        if (!text.empty()) {
            retro_message_ext message {
                .msg = text.c_str(),
                .duration = 60,
                .priority = 0,
                .level = RETRO_LOG_DEBUG,
                .target = RETRO_MESSAGE_TARGET_OSD,
                .type = RETRO_MESSAGE_TYPE_STATUS,
                .progress = -1
            };
            retro::set_message(&message);
        }

    });
}