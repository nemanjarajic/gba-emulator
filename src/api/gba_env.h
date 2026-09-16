// A C interface to the emulator as a vectorised reinforcement-learning
// environment: many independent GBA machines stepped together, each with its
// own controller input, observation and episode lifetime.
//
// Deliberately a flat C ABI rather than a C++ or pybind interface, so it can be
// loaded from Python with ctypes and no build step, and from anything else that
// speaks C. Buffers handed out stay valid until the next call that changes
// them, which lets NumPy wrap them without copying.

#ifndef GBA_ENV_H
#define GBA_ENV_H

#include <stdint.h>

#ifdef _WIN32
#define GBA_ENV_API __declspec(dllexport)
#else
#define GBA_ENV_API __attribute__((visibility("default")))
#endif

#ifdef __cplusplus
extern "C" {
#endif

// The library and its bindings are versioned separately once they live in
// different repositories, so a caller must be able to tell whether the shared
// library it loaded speaks the interface it was built against. Bump this
// whenever anything in this header changes shape.
#define GBA_ENV_ABI_VERSION 1

GBA_ENV_API uint32_t gba_env_abi_version(void);

typedef struct GbaEnv GbaEnv;

// Creation flags.
enum {
    // Do not allocate save memory. Reclaims 128 KiB per instance, a quarter of
    // the footprint. Some games refuse to run without it -- Pokemon Emerald
    // identifies its flash chip on boot and gives up if there is none.
    GBA_ENV_NO_SAVE = 1u
};

// Buttons, in the order the hardware numbers them. An action is the bitwise OR
// of the buttons held; the register's active-low encoding is handled inside.
enum {
    GBA_BUTTON_A = 1u << 0,
    GBA_BUTTON_B = 1u << 1,
    GBA_BUTTON_SELECT = 1u << 2,
    GBA_BUTTON_START = 1u << 3,
    GBA_BUTTON_RIGHT = 1u << 4,
    GBA_BUTTON_LEFT = 1u << 5,
    GBA_BUTTON_UP = 1u << 6,
    GBA_BUTTON_DOWN = 1u << 7,
    GBA_BUTTON_R = 1u << 8,
    GBA_BUTTON_L = 1u << 9
};

// Returns NULL on failure; gba_env_last_error() says why.
GBA_ENV_API GbaEnv* gba_env_create(const char* rom_path, uint32_t num_instances, uint32_t flags);
GBA_ENV_API void gba_env_destroy(GbaEnv* env);
GBA_ENV_API const char* gba_env_last_error(void);

GBA_ENV_API uint32_t gba_env_num_instances(const GbaEnv* env);
GBA_ENV_API uint32_t gba_env_obs_width(void);
GBA_ENV_API uint32_t gba_env_obs_height(void);

// --- episode boundaries ----------------------------------------------------
//
// A reset point is one instance's whole machine, memory included. Capture it
// once after driving a game to wherever episodes should begin, then reset any
// subset of instances back to it. Restoring registers alone would leave the
// game's own state behind and the next episode would start mid-scene.

// Captures `instance` as the reset point.
GBA_ENV_API int gba_env_capture_reset_point(GbaEnv* env, uint32_t instance);
GBA_ENV_API int gba_env_save_reset_point(GbaEnv* env, const char* path);
GBA_ENV_API int gba_env_load_reset_point(GbaEnv* env, const char* path);

// Resets the listed instances, leaving every other instance running. This is
// the call episodic RL needs: episodes end at different times.
GBA_ENV_API void gba_env_reset(GbaEnv* env, const uint32_t* instances, uint32_t count);
GBA_ENV_API void gba_env_reset_all(GbaEnv* env);

// --- stepping --------------------------------------------------------------

// Holds each instance's action for `frames` frames, the usual action-repeat.
// Only the final frame is rendered, since the intervening pictures are never
// looked at and rendering has no effect on the emulated machine.
GBA_ENV_API void gba_env_step(GbaEnv* env, const uint16_t* actions, uint32_t frames);

// --- reading state out -----------------------------------------------------

// num_instances * height * width bytes, one instance after another, 8-bit
// grayscale. Valid until the next step or reset.
GBA_ENV_API const uint8_t* gba_env_observations(GbaEnv* env);

// Gathers `count` GBA addresses from every instance into `out`, which must hold
// count * num_instances words, address-major: out[a * num_instances + i]. This
// is how a reward is read: a score or a hit-point counter in the game's RAM.
GBA_ENV_API void gba_env_read_probes(GbaEnv* env, const uint32_t* addresses, uint32_t count,
                                     uint32_t* out);

// The full 240x160 framebuffer for one instance, as BGR555 halfwords packed two
// per word. For inspection and debugging rather than for training.
GBA_ENV_API const uint32_t* gba_env_framebuffer(GbaEnv* env, uint32_t instance);

#ifdef __cplusplus
}
#endif
#endif  // GBA_ENV_H
