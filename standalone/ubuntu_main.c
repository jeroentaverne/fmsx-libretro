/*
 * Native Ubuntu/WSLg SDL2 window for fMSX. RetroArch is not used.
 *
 * Loads the local fmsx_libretro.so core directly through libretro ABI,
 * providing keyboard, video, audio, BIOS path and 50/60 Hz event pacing.
 * This tiny host owns the SDL window title and updates it when Panasonic
 * switched I/O changes the emulated Z80 frequency.
 */
#define _POSIX_C_SOURCE 200809L
#include "libretro.h"
#include "avi_recorder.h"
#include "png_screenshot.h"
#include "wav_recorder.h"
#include <SDL2/SDL.h>
#include <dlfcn.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

typedef struct fmsx_core {
    void (*set_environment)(retro_environment_t);
    void (*set_video_refresh)(retro_video_refresh_t);
    void (*set_audio_sample_batch)(retro_audio_sample_batch_t);
    void (*set_input_poll)(retro_input_poll_t);
    void (*set_input_state)(retro_input_state_t);
    void (*set_controller_port_device)(unsigned, unsigned);
    void (*init)(void);
    bool (*load_game)(const struct retro_game_info *);
    void (*run)(void);
    void (*unload_game)(void);
    void (*deinit)(void);
    void (*get_system_av_info)(struct retro_system_av_info *);
    int (*cpu_frequency_hz)(void);
} fmsx_core_t;

static char bios_directory[4096] = ".";
static SDL_Window *window;
static SDL_Renderer *renderer;
static SDL_Texture *texture;
static SDL_AudioDeviceID audio_device;
static unsigned texture_width, texture_height;
static struct retro_keyboard_callback keyboard_callback;
static bool key_states[RETROK_LAST];
static bool quitting;
static fmsx_core_t core;
static avi_recorder_t recorder;
static wav_recorder_t audio_capture;
static unsigned recorder_sample_rate = 48000;
static bool media_capture_failed;
static unsigned recorder_fps = 60;
static char recorder_path[4096];
static char recording_final_avi[4096];
static char recording_video_part[4096];
static char recording_wav_path[4096];
static unsigned recorder_sequence;
static bool auto_record;
static bool self_test_shortcuts;
static uint64_t frame_limit;
static uint64_t frames_executed;
static bool screenshot_requested;
static bool screenshot_failed;
static char screenshot_path[4096];
static unsigned screenshot_sequence;

/* The fork ships its original BIOS files. Locate them relative to the
 * executable, never requiring an external BIOS download or RetroArch. */
/* Optional out-of-the-box Nextor cartridge and virtual SD image.
 * Explicit environment variables always take precedence. */
static void find_bundled_pico_media(void)
{
    if (getenv("MSXPICO_SIM_ROM") || getenv("MSXPICO_SIM_SD") ||
        getenv("MSXPICO_SIM_MAPPER")) return;
    char executable[4096];
    const ssize_t n = readlink("/proc/self/exe", executable, sizeof executable - 1);
    if (n <= 0) return;
    executable[n] = 0;
    char *end = strrchr(executable, '/');
    if (!end) return;
    *end = 0;
    char rom[4096], sd[4096];
    /* Nextor 3 is the preferred bundled cartridge. Keep the older
     * Nextor 2 cartridge as a fallback for existing installations. */
    const char *const rom_names[] = {
        "sdmapper_select.rom", "sdmapper_one_drive.rom"
    };
    if (snprintf(sd, sizeof sd, "%s/virtual_sd.img", executable)
        >= (int)sizeof sd || access(sd, R_OK | W_OK)) return;
    bool found = false;
    for (size_t i = 0; i < sizeof rom_names / sizeof rom_names[0]; ++i)
    {
        if (snprintf(rom, sizeof rom, "%s/ROMs/%s", executable, rom_names[i])
            >= (int)sizeof rom) continue;
        if (!access(rom, R_OK)) { found = true; break; }
    }
    if (!found) return;
    (void)setenv("MSXPICO_SIM_MAPPER", "nextor", 0);
    (void)setenv("MSXPICO_SIM_ROM", rom, 0);
    (void)setenv("MSXPICO_SIM_SD", sd, 0);
}

static void find_bundled_bios(void)
{
    char executable[4096];
    const ssize_t length = readlink("/proc/self/exe", executable,
                                     sizeof executable - 1);
    if (length <= 0) return;
    executable[length] = 0;
    char *slash = strrchr(executable, '/');
    if (!slash) return;
    *slash = 0;
    char candidate[4096];
    const char *const folders[] = { "ROMs", "fMSX/ROMs" };
    for (unsigned i = 0; i < 2; ++i)
    {
        if (snprintf(candidate, sizeof candidate, "%s/%s", executable,
                     folders[i]) >= (int)sizeof candidate)
            continue;
        char required[4096];
        if (snprintf(required, sizeof required, "%s/MSX2P.ROM", candidate)
            >= (int)sizeof required) continue;
        if (access(required, R_OK) == 0)
        {
            strcpy(bios_directory, candidate);
            return;
        }
    }
}

static void update_title(void)
{
    if (!window) return;
    const int hz = core.cpu_frequency_hz ? core.cpu_frequency_hz() : 3579545;
    char title[128];
    snprintf(title, sizeof title, "fMSX MSX2+ / MSX Pico - Z80 %.2f MHz%s",
             (double)hz / 1000000.0, recorder.active ? " [REC FFV1 AVI + WAV]" : "");
    SDL_SetWindowTitle(window, title);
}

static bool environment(unsigned cmd, void *data)
{
    switch (cmd)
    {
    case RETRO_ENVIRONMENT_GET_SYSTEM_DIRECTORY:
        if (!data) return false;
        *(const char **)data = bios_directory;
        return true;
    case RETRO_ENVIRONMENT_SET_PIXEL_FORMAT:
        return data && *(const enum retro_pixel_format *)data == RETRO_PIXEL_FORMAT_RGB565;
    case RETRO_ENVIRONMENT_SET_KEYBOARD_CALLBACK:
        if (!data) return false;
        keyboard_callback = *(const struct retro_keyboard_callback *)data;
        return true;
    case RETRO_ENVIRONMENT_GET_VARIABLE_UPDATE:
        if (!data) return false;
        *(bool *)data = false;
        return true;
    case RETRO_ENVIRONMENT_SET_MESSAGE:
        if (data)
        {
            const struct retro_message *msg = (const struct retro_message *)data;
            if (msg->msg) fprintf(stderr, "fMSX: %s\n", msg->msg);
        }
        return true;
    case RETRO_ENVIRONMENT_SET_VARIABLES:
    case RETRO_ENVIRONMENT_SET_SUPPORT_NO_GAME:
    case RETRO_ENVIRONMENT_SET_INPUT_DESCRIPTORS:
    case RETRO_ENVIRONMENT_SET_CONTROLLER_INFO:
    case RETRO_ENVIRONMENT_SET_DISK_CONTROL_INTERFACE:
        return true;
    default:
        /* Optional libretro frontend features (VFS, SRAM, cheats, etc.)
         * are intentionally not advertised to avoid false compatibility. */
        return false;
    }
}

static void video_frame(const void *data, unsigned width,
                        unsigned height, size_t pitch)
{
    /* Feed every emulated video frame to the lossless encoder before SDL
     * presentation. The pipe blocks instead of dropping delayed frames. */
    if (recorder.active &&
        !avi_recorder_frame(&recorder, data, width, height, pitch))
    {
        fputs("AVI: encoder failed while writing video frame\n", stderr);
        quitting = true;
        return;
    }
    if (screenshot_requested && data && width && height)
    {
        char filename[4096];
        bool name_ready = false;
        if (screenshot_path[0])
        {
            strcpy(filename, screenshot_path);
            screenshot_path[0] = 0; /* Later hotkeys use numbered filenames. */
            name_ready = true;
        }
        else
        {
            do
            {
                if (++screenshot_sequence > 9999) break;
                snprintf(filename, sizeof filename, "fmsx-screenshot-%04u.png",
                         screenshot_sequence);
                name_ready = true;
            } while (access(filename, F_OK) == 0);
            if (screenshot_sequence > 9999) name_ready = false;
        }
        const bool ok = name_ready &&
            fmsx_write_png(filename, data, width, height, pitch);
        if (ok)
            fprintf(stderr, "PNG: saved %s (%ux%u)\n", filename, width, height);
        else
        {
            fputs("PNG: screenshot failed\n", stderr);
            screenshot_failed = true;
        }
        screenshot_requested = false;
    }
    if (!renderer || !data || !width || !height) return;
    if (!texture || width != texture_width || height != texture_height)
    {
        if (texture) SDL_DestroyTexture(texture);
        texture_width = width;
        texture_height = height;
        texture = SDL_CreateTexture(renderer, SDL_PIXELFORMAT_RGB565,
                                    SDL_TEXTUREACCESS_STREAMING,
                                    (int)width, (int)height);
        if (!texture)
        {
            fprintf(stderr, "SDL texture: %s\n", SDL_GetError());
            quitting = true;
            return;
        }
    }
    if (SDL_UpdateTexture(texture, NULL, data, (int)pitch))
    {
        fprintf(stderr, "SDL_UpdateTexture: %s\n", SDL_GetError());
        quitting = true;
        return;
    }
    SDL_RenderClear(renderer);
    SDL_RenderCopy(renderer, texture, NULL, NULL);
    SDL_RenderPresent(renderer);
}

static size_t audio_samples(const int16_t *samples, size_t frames)
{
    /* Capture before SDL latency throttling: never drop recorded audio
     * even if the host device is muted, disabled, or running behind. */
    if (audio_capture.active &&
        !wav_recorder_samples(&audio_capture, samples, frames))
    {
        fputs("WAV: write failed; stopping capture\n", stderr);
        media_capture_failed = true;
        quitting = true;
    }
    if (audio_device && samples && frames)
    {
        /* At most ~100 ms of queued audio; don't build unbounded latency. */
        const Uint32 limit = 48000u * 2u * sizeof(int16_t) / 10u;
        const size_t bytes = frames * 2 * sizeof(int16_t);
        if (SDL_GetQueuedAudioSize(audio_device) < limit &&
            bytes <= UINT32_MAX)
            SDL_QueueAudio(audio_device, samples, (Uint32)bytes);
    }
    return frames;
}

static void input_poll(void) {}

static int16_t input_state(unsigned port, unsigned device,
                           unsigned index, unsigned id)
{
    (void)index;
    if (port != 0) return 0;
    if (device == RETRO_DEVICE_KEYBOARD && id < RETROK_LAST)
        return key_states[id] ? 1 : 0;
    return 0;
}

static unsigned map_key(SDL_Keycode code)
{
    if (code >= SDLK_SPACE && code <= SDLK_z && code < RETROK_LAST)
        return (unsigned)code; /* ASCII letter, digit and punctuation */
    switch (code)
    {
    case SDLK_RETURN: return RETROK_RETURN;
    case SDLK_ESCAPE: return RETROK_ESCAPE;
    case SDLK_BACKSPACE: return RETROK_BACKSPACE;
    case SDLK_TAB: return RETROK_TAB;
    case SDLK_DELETE: return RETROK_DELETE;
    case SDLK_UP: return RETROK_UP;
    case SDLK_DOWN: return RETROK_DOWN;
    case SDLK_LEFT: return RETROK_LEFT;
    case SDLK_RIGHT: return RETROK_RIGHT;
    case SDLK_INSERT: return RETROK_INSERT;
    case SDLK_HOME: return RETROK_HOME;
    case SDLK_END: return RETROK_END;
    case SDLK_PAGEUP: return RETROK_PAGEUP;
    case SDLK_PAGEDOWN: return RETROK_PAGEDOWN;
    case SDLK_LSHIFT: return RETROK_LSHIFT;
    case SDLK_RSHIFT: return RETROK_RSHIFT;
    case SDLK_LCTRL: return RETROK_LCTRL;
    case SDLK_RCTRL: return RETROK_RCTRL;
    case SDLK_LALT: return RETROK_LALT;
    case SDLK_RALT: return RETROK_RALT;
    case SDLK_CAPSLOCK: return RETROK_CAPSLOCK;
    case SDLK_F1: return RETROK_F1;
    case SDLK_F2: return RETROK_F2;
    case SDLK_F3: return RETROK_F3;
    case SDLK_F4: return RETROK_F4;
    case SDLK_F5: return RETROK_F5;
    case SDLK_F6: return RETROK_F6;
    case SDLK_F7: return RETROK_F7;
    case SDLK_F8: return RETROK_F8;
    case SDLK_F9: return RETROK_F9;
    case SDLK_F10: return RETROK_F10;
    case SDLK_F11: return RETROK_F11;
    case SDLK_F12: return RETROK_F12;
    case SDLK_KP_0: return RETROK_KP0;
    case SDLK_KP_1: return RETROK_KP1;
    case SDLK_KP_2: return RETROK_KP2;
    case SDLK_KP_3: return RETROK_KP3;
    case SDLK_KP_4: return RETROK_KP4;
    case SDLK_KP_5: return RETROK_KP5;
    case SDLK_KP_6: return RETROK_KP6;
    case SDLK_KP_7: return RETROK_KP7;
    case SDLK_KP_8: return RETROK_KP8;
    case SDLK_KP_9: return RETROK_KP9;
    case SDLK_KP_ENTER: return RETROK_KP_ENTER;
    case SDLK_KP_PLUS: return RETROK_KP_PLUS;
    case SDLK_KP_MINUS: return RETROK_KP_MINUS;
    case SDLK_KP_MULTIPLY: return RETROK_KP_MULTIPLY;
    case SDLK_KP_DIVIDE: return RETROK_KP_DIVIDE;
    case SDLK_KP_PERIOD: return RETROK_KP_PERIOD;
    default: return RETROK_UNKNOWN;
    }
}

/* Ctrl+Shift+R toggles recording without injecting a key into the MSX.
 * Subsequent recording sessions use distinct filenames, never overwrite. */
static bool start_recording(void)
{
    char filename[4096];
    if (recorder_path[0])
    {
        if (!recorder_sequence)
            strcpy(filename, recorder_path);
        else
        {
            const size_t n = strlen(recorder_path);
            const bool avi = n >= 4 && !strcmp(recorder_path + n - 4, ".avi");
            const int result = snprintf(filename, sizeof filename,
                "%.*s-%04u.avi", (int)(avi ? n - 4 : n),
                recorder_path, recorder_sequence);
            if (result < 0 || result >= (int)sizeof filename) return false;
        }
    }
    else
    {
        do
        {
            if (recorder_sequence >= 9999) return false;
            ++recorder_sequence;
            snprintf(filename, sizeof filename, "fmsx-capture-%04u.avi",
                     recorder_sequence);
        } while (access(filename, F_OK) == 0);
    }

    const size_t n = strlen(filename);
    if (n < 4 || strcmp(filename + n - 4, ".avi")) return false;
    /* Write temporary FFV1 video while WAV gets the exact audio samples;
     * after recording, stream-copy both into the final AVI with PCM audio.
     * Final AVI and WAV are distinct outputs with the same base filename. */
    char wav_path[4096], video_part[4096];
    if (snprintf(video_part, sizeof video_part, "%.*s.video-only.part.avi",
                 (int)(n - 4), filename) >= (int)sizeof video_part)
        return false;
    memcpy(wav_path, filename, n + 1);
    memcpy(wav_path + n - 4, ".wav", 5);
    if (access(filename, F_OK) == 0 || access(video_part, F_OK) == 0)
        return false;

    if (!wav_recorder_start(&audio_capture, wav_path, recorder_sample_rate))
        return false;
    if (!avi_recorder_start(&recorder, video_part, recorder_fps))
    {
        (void)wav_recorder_stop(&audio_capture);
        (void)unlink(wav_path); /* Created exclusively during this attempt. */
        return false;
    }
    strcpy(recording_final_avi, filename);
    strcpy(recording_video_part, video_part);
    strcpy(recording_wav_path, wav_path);
    if (recorder_path[0]) ++recorder_sequence;
    update_title();
    return true;
}

static bool stop_recording(void)
{
    /* First finalize the original video and WAV streams. No audio or
     * video samples are changed when the two streams are muxed below. */
    const bool video = !recorder.active || avi_recorder_stop(&recorder);
    const bool audio = !audio_capture.active ||
                       wav_recorder_stop(&audio_capture);
    bool ok = video && audio;
    if (ok)
        ok = avi_recorder_mux_audio(recording_video_part,
                                    recording_wav_path, recording_final_avi);
    if (ok)
    {
        /* Keep both final .avi (video+audio) and .wav (audio only). */
        if (unlink(recording_video_part) != 0)
            perror("AVI: unable to remove temporary video file");
    }
    update_title();
    return ok;
}

/* Test the exact SDL shortcut paths without relying on a real keyboard.
 * No shortcut events are injected in normal emulation sessions. */
static bool inject_capture_key(SDL_Keycode key)
{
    SDL_Event event = {0};
    event.type = SDL_KEYDOWN;
    event.key.state = SDL_PRESSED;
    event.key.repeat = 0;
    event.key.keysym.sym = key;
    event.key.keysym.mod = KMOD_CTRL | KMOD_SHIFT;
    return SDL_PushEvent(&event) == 1;
}

static void handle_events(void)
{
    SDL_Event event;
    while (SDL_PollEvent(&event))
    {
        if (event.type == SDL_QUIT)
            quitting = true;
        if (event.type == SDL_WINDOWEVENT &&
            event.window.event == SDL_WINDOWEVENT_FOCUS_LOST)
        {
            memset(key_states, 0, sizeof key_states);
        }
        if (event.type != SDL_KEYDOWN && event.type != SDL_KEYUP)
            continue;
        const bool down = event.type == SDL_KEYDOWN;
        const SDL_Keycode key = event.key.keysym.sym;
        const SDL_Keymod mods = (SDL_Keymod)event.key.keysym.mod;
        if (key == SDLK_s && (mods & KMOD_CTRL) && (mods & KMOD_SHIFT))
        {
            if (down && !event.key.repeat) screenshot_requested = true;
            continue;
        }
        if (key == SDLK_r && (mods & KMOD_CTRL) && (mods & KMOD_SHIFT))
        {
            if (down && !event.key.repeat)
            {
                if (recorder.active)
                {
                    if (!stop_recording()) fputs("AVI: recorder failed\n", stderr);
                }
                else if (!start_recording())
                    fputs("AVI: cannot start recording\n", stderr);
            }
            continue;
        }
        if (down && key == SDLK_RETURN && (mods & KMOD_ALT))
        {
            SDL_SetWindowFullscreen(window,
                (SDL_GetWindowFlags(window) & SDL_WINDOW_FULLSCREEN_DESKTOP) ?
                0 : SDL_WINDOW_FULLSCREEN_DESKTOP);
            continue;
        }
        if (down && key == SDLK_q && (mods & KMOD_CTRL) && (mods & KMOD_SHIFT))
        {
            quitting = true;
            continue;
        }
        unsigned retro = map_key(key);
        if (retro >= RETROK_LAST) continue;
        key_states[retro] = down;
        if (keyboard_callback.callback)
            keyboard_callback.callback(down, retro,
                                       (uint32_t)((key >= 32 && key < 127) ? key : 0),
                                       (uint16_t)mods);
    }
}

static void *load_symbol(void *library, const char *name)
{
    void *value = dlsym(library, name);
    if (!value)
    {
        fprintf(stderr, "Missing fMSX core symbol: %s\n", name);
        exit(EXIT_FAILURE);
    }
    return value;
}

#define LOAD(name,field) do { *(void **)(&core.field) = load_symbol(library, "retro_" #name); } while (0)

static void load_fmsx(void *library)
{
    LOAD(set_environment,set_environment);
    LOAD(set_video_refresh,set_video_refresh);
    LOAD(set_audio_sample_batch,set_audio_sample_batch);
    LOAD(set_input_poll,set_input_poll);
    LOAD(set_input_state,set_input_state);
    LOAD(set_controller_port_device,set_controller_port_device);
    LOAD(init,init);
    LOAD(load_game,load_game);
    LOAD(run,run);
    LOAD(unload_game,unload_game);
    LOAD(deinit,deinit);
    LOAD(get_system_av_info,get_system_av_info);
    *(void **)(&core.cpu_frequency_hz) = dlsym(library, "fmsx_cpu_frequency_hz");
}

static void usage(const char *exe)
{
    printf("Usage: %s [--core fmsx_libretro.so] [--bios BIOS_DIR] [--disk DISK.DSK]\n"
           "       [--record VIDEO.avi] [--screenshot SCREEN.png] [--frames N] [--self-test-shortcuts]\n"
           "       %s --self-test-window\n"
           "Ubuntu / WSLg standalone SDL2 MSX2+ emulator, without RetroArch.\n"
           "Alt+Enter: fullscreen. Ctrl+Shift+R: FFV1 AVI, Ctrl+Shift+S: PNG.\n"
           "AVI video requires ffmpeg; WAV audio saves alongside each AVI.\n"
           "MSX Pico: MSXPICO_SIM_MAPPER=nextor, MSXPICO_SIM_ROM=...,\n"
           "          MSXPICO_SIM_SD=/path/to/raw_sd_image.img.\n",
           exe, exe);
}

int main(int argc, char **argv)
{
    const char *library_path = "./fmsx_libretro.so";
    const char *disk_path = NULL;
    bool self_test_window = false;
    bool manual_bios = false;
    for (int i = 1; i < argc; ++i)
    {
        if (!strcmp(argv[i], "--help") || !strcmp(argv[i], "-h"))
        {
            usage(argv[0]);
            return EXIT_SUCCESS;
        }
        if (!strcmp(argv[i], "--self-test-window")) { self_test_window = true; continue; }
        if (!strcmp(argv[i], "--self-test-shortcuts")) { self_test_shortcuts = true; continue; }
        if (!strcmp(argv[i], "--screenshot") && i + 1 < argc)
        {
            const char *name = argv[++i];
            const size_t n = strlen(name);
            if (n < 5 || n >= sizeof screenshot_path ||
                strcmp(name + n - 4, ".png"))
            {
                fputs("PNG: --screenshot requires a .png output path\n", stderr);
                return EXIT_FAILURE;
            }
            strcpy(screenshot_path, name);
            screenshot_requested = true;
            continue;
        }
        if (!strcmp(argv[i], "--record") && i + 1 < argc)
        {
            const char *value = argv[++i];
            const size_t n = strlen(value);
            if (n < 5 || n >= sizeof recorder_path ||
                strcmp(value + n - 4, ".avi"))
            {
                fputs("AVI: --record requires a .avi output path\n", stderr);
                return EXIT_FAILURE;
            }
            strcpy(recorder_path, value);
            auto_record = true;
            continue;
        }
        if (!strcmp(argv[i], "--frames") && i + 1 < argc)
        {
            const char *number = argv[++i];
            char *endptr;
            unsigned long long limit = strtoull(number, &endptr, 10);
            if (!number[0] || *endptr || !limit || limit > 1000000000ull)
            {
                fputs("Invalid --frames value\n", stderr);
                return EXIT_FAILURE;
            }
            frame_limit = (uint64_t)limit;
            continue;
        }
        if ((!strcmp(argv[i], "--core") || !strcmp(argv[i], "--bios") ||
             !strcmp(argv[i], "--disk")) && i + 1 < argc)
        {
            const char *option = argv[i++];
            const char *value = argv[i];
            if (!strcmp(option, "--core")) library_path = value;
            else if (!strcmp(option, "--disk")) disk_path = value;
            else
            {
                if (strlen(value) >= sizeof bios_directory)
                {
                    fputs("BIOS path too long\n", stderr);
                    return EXIT_FAILURE;
                }
                strcpy(bios_directory, value);
                manual_bios = true;
            }
            continue;
        }
        usage(argv[0]);
        return EXIT_FAILURE;
    }

    if (!manual_bios) find_bundled_bios();
    find_bundled_pico_media();

    if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_AUDIO | SDL_INIT_EVENTS))
    {
        fprintf(stderr, "SDL initialization: %s\n", SDL_GetError());
        return EXIT_FAILURE;
    }
    window = SDL_CreateWindow("fMSX MSX2+ / MSX Pico - Z80 3.58 MHz",
                              SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,
                              1024, 768, SDL_WINDOW_SHOWN | SDL_WINDOW_RESIZABLE);
    if (!window)
    {
        fprintf(stderr, "SDL window: %s\n", SDL_GetError());
        SDL_Quit();
        return EXIT_FAILURE;
    }
    if (self_test_window)
    {
        SDL_SetWindowTitle(window, "fMSX MSX2+ / MSX Pico - Z80 8.00 MHz");
        if (!strstr(SDL_GetWindowTitle(window), "8.00 MHz"))
            return EXIT_FAILURE;
        SDL_DestroyWindow(window);
        SDL_Quit();
        puts("PASS: standalone Ubuntu SDL window and dynamic CPU title");
        return EXIT_SUCCESS;
    }

    renderer = SDL_CreateRenderer(window, -1,
                   SDL_RENDERER_ACCELERATED | SDL_RENDERER_PRESENTVSYNC);
    if (!renderer) renderer = SDL_CreateRenderer(window, -1, SDL_RENDERER_SOFTWARE);
    if (!renderer)
    {
        fprintf(stderr, "SDL renderer: %s\n", SDL_GetError());
        SDL_DestroyWindow(window);
        SDL_Quit();
        return EXIT_FAILURE;
    }
    SDL_AudioSpec requested = {0};
    requested.freq = 48000;
    requested.format = AUDIO_S16SYS;
    requested.channels = 2;
    requested.samples = 2048;
    audio_device = SDL_OpenAudioDevice(NULL, 0, &requested, NULL, 0);
    if (audio_device) SDL_PauseAudioDevice(audio_device, 0);

    void *library = dlopen(library_path, RTLD_NOW | RTLD_LOCAL);
    if (!library)
    {
        fprintf(stderr, "Cannot load %s: %s\n", library_path, dlerror());
        return EXIT_FAILURE;
    }
    load_fmsx(library);
    core.set_environment(environment);
    core.set_video_refresh(video_frame);
    core.set_audio_sample_batch(audio_samples);
    core.set_input_poll(input_poll);
    core.set_input_state(input_state);
    core.init();
    struct retro_game_info disk = {0};
    if (disk_path) disk.path = disk_path;
    if (!core.load_game(disk_path ? &disk : NULL))
    {
        fprintf(stderr, "MSX startup failed. Checked BIOS directory: %s\n",
                bios_directory);
        core.deinit();
        dlclose(library);
        return EXIT_FAILURE;
    }
    core.set_controller_port_device(0, RETRO_DEVICE_KEYBOARD);
    struct retro_system_av_info av = {0};
    core.get_system_av_info(&av);
    const double fps = av.timing.fps > 0 ? av.timing.fps : 60.0;
    recorder_fps = fps <= 55.0 ? 50 : 60;
    recorder_sample_rate = (unsigned)(av.timing.sample_rate + 0.5);
    if (recorder_sample_rate < 8000 || recorder_sample_rate > 192000)
        recorder_sample_rate = 48000;
    bool recording_failed = false;
    if (self_test_shortcuts)
    {
        if (auto_record || screenshot_requested)
        {
            fputs("Shortcut test cannot combine with --record/--screenshot\\n", stderr);
            core.unload_game();
            core.deinit();
            dlclose(library);
            return EXIT_FAILURE;
        }
        if (!frame_limit) frame_limit = 32;
        if (frame_limit < 30)
        {
            fputs("Shortcut test needs at least 30 frames\\n", stderr);
            return EXIT_FAILURE;
        }
    }
    if (auto_record && !start_recording())
    {
        recording_failed = true;
        quitting = true;
    }
    const uint64_t counter_frequency = SDL_GetPerformanceFrequency();
    const uint64_t period_ticks = (uint64_t)((double)counter_frequency / fps);
    uint64_t next_frame = SDL_GetPerformanceCounter();
    int title_hz = -1;
    while (!quitting)
    {
        /* Two PNG snapshots and matching AVI/WAV via SDL hotkeys. */
        if (self_test_shortcuts)
        {
            if (frames_executed == 6 || frames_executed == 16)
            {
                if (!inject_capture_key(SDLK_s)) recording_failed = true;
            }
            if (frames_executed == 10 || frames_executed == 25)
            {
                if (!inject_capture_key(SDLK_r)) recording_failed = true;
            }
        }
        handle_events();
        core.run();
        ++frames_executed;
        if (frame_limit && frames_executed >= frame_limit) quitting = true;
        const int hz = core.cpu_frequency_hz ? core.cpu_frequency_hz() : 3579545;
        if (hz != title_hz)
        {
            update_title();
            title_hz = hz;
        }
        next_frame += period_ticks;
        const uint64_t now = SDL_GetPerformanceCounter();
        if (next_frame > now && period_ticks)
        {
            const uint64_t ms = (next_frame - now) * 1000 / counter_frequency;
            if (ms) SDL_Delay((uint32_t)ms);
        }
        else if (now - next_frame > period_ticks * 2)
            next_frame = now; /* discard missed frames instead of catching up */
    }
    if ((recorder.active || audio_capture.active) && !stop_recording()) recording_failed = true;
    if (media_capture_failed) recording_failed = true;
    if (self_test_shortcuts)
    {
        const bool ok = screenshot_sequence == 2 &&
                        recorder_sequence == 1 &&
                        access("fmsx-screenshot-0001.png", R_OK) == 0 &&
                        access("fmsx-screenshot-0002.png", R_OK) == 0 &&
                        access("fmsx-capture-0001.avi", R_OK) == 0 &&
                        access("fmsx-capture-0001.wav", R_OK) == 0 &&
                        !recording_failed && !screenshot_failed;
        if (!ok) recording_failed = true;
        fprintf(stderr, "%s: SDL keyboard shortcuts (2 PNG + AVI + WAV)\n",
                ok ? "PASS" : "FAIL");
    }
    core.unload_game();
    core.deinit();
    dlclose(library);
    if (audio_device) SDL_CloseAudioDevice(audio_device);
    if (texture) SDL_DestroyTexture(texture);
    SDL_DestroyRenderer(renderer);
    SDL_DestroyWindow(window);
    SDL_Quit();
    return recording_failed || screenshot_failed ? EXIT_FAILURE : EXIT_SUCCESS;
}
