/* SPDX-License-Identifier: GPL-2.0-or-later
 * Pad ABI, button maps and SDL sampling adapted from the user-provided GoW3
 * Windows source, src/runtime_pad.c, reference commit
 * 97f1f8d4410ac8ad4fde29d29041f06e3af51b66. Existing GPL attribution is retained.
 * SDL events are pumped by the selected renderer's window thread. No GoW3
 * replay/recording, mapping overrides, overlay or executable hooks are used.
 */
#include "windows_pad.h"
#include <windows.h>
#include <stdio.h>
#include <string.h>
#include <limits.h>
#include <stdbool.h>
#ifdef TLG_WITH_RENDERER
#include <SDL3/SDL.h>
#endif
#define ABI __attribute__((sysv_abi))
#define PAD_ERROR(n) ((int32_t)(UINT32_C(0x80920000)|(n)))
extern uint64_t runtime_process_time_us(void);
static uint64_t now_us(void) {return runtime_process_time_us();}
enum {
    BTN_L3=0x2, BTN_R3=0x4, BTN_OPTIONS=0x8, BTN_UP=0x10, BTN_RIGHT=0x20, BTN_DOWN=0x40, BTN_LEFT=0x80,
    BTN_L2=0x100, BTN_R2=0x200, BTN_L1=0x400, BTN_R1=0x800, BTN_TRIANGLE=0x1000, BTN_CIRCLE=0x2000,
    BTN_CROSS=0x4000, BTN_SQUARE=0x8000, BTN_TOUCHPAD=0x100000,
};
typedef struct { uint16_t x, y; uint8_t id, reserve[3]; } PadTouch;
typedef struct {
    uint32_t buttons;
    uint8_t left_x, left_y, right_x, right_y;
    uint8_t l2, r2, analog_padding[2];
    float orientation[4], acceleration[3], angular_velocity[3];
    uint8_t touch_count, touch_reserve[3];
    uint32_t touch_held_time;
    PadTouch touches[2];
    uint8_t connected, pad0[3];
    uint64_t timestamp;
    uint8_t extension[16];
    uint8_t connected_count, reserve[2], unique_length, unique[12];
} PadData;
typedef struct {
    float pixel_density; uint16_t resolution_x, resolution_y;
    uint8_t dead_zone_left, dead_zone_right, connection_type, connected_count;
    uint8_t connected, pad[3];
    int32_t device_class;
    uint8_t reserve[8];
} ControllerInfo;
_Static_assert(sizeof(PadData)==120,"OrbisPadData layout");
_Static_assert(sizeof(PadTouch)==8,"OrbisPadTouch layout");
_Static_assert(__builtin_offsetof(PadData,touches)==60,"OrbisPadData touch offset");
_Static_assert(__builtin_offsetof(PadData,timestamp)==80,"OrbisPadData timestamp offset");
_Static_assert(sizeof(ControllerInfo)==28,"OrbisPadControllerInformation layout");


static SRWLOCK lock=SRWLOCK_INIT;
static int initialized,opened,next_handle=1;
static uint8_t connected_count;
static int was_connected;
static uint8_t axis(int16_t v) {return (uint8_t)(((int)v+32768)>>8);}
static uint8_t trigger(int16_t v) {return v<=0?0:(uint8_t)(v>>7);}
static uint16_t touch_axis(float v,int max) {return (uint16_t)(v<=0?0:v>=1?max:(int)(v*max+0.5f));}
static void touch_click(PadData *d,int right) {
    d->buttons|=BTN_TOUCHPAD;d->touch_count=1;
    d->touches[0]=(PadTouch){.x=right?1440:480,.y=471,.id=0};
}
static void key_axis(uint8_t *axis, bool negative, bool positive) {
    if (negative || positive) *axis=(uint8_t)(128-(negative ? 128 : 0)+(positive ? 127 : 0));
}

#ifdef TLG_WITH_RENDERER
static int sdl_ready;
static SDL_Gamepad *gamepad;
/* Opens the first gamepad SDL knows about; called under lock. */
static SDL_Gamepad *current_gamepad(void) {
    if (!sdl_ready) sdl_ready = SDL_WasInit(SDL_INIT_GAMEPAD) ? 1 : SDL_InitSubSystem(SDL_INIT_GAMEPAD) ? 1 : -1;
    if (sdl_ready<0) return NULL;
    if (gamepad && !SDL_GamepadConnected(gamepad)) { SDL_CloseGamepad(gamepad); gamepad=NULL; }
    if (!gamepad) {
        int count=0;
        SDL_JoystickID *ids=SDL_GetGamepads(&count);
        if (ids && count>0) {
            gamepad=SDL_OpenGamepad(ids[0]);
            if (gamepad) { printf("Runtime: gamepad connected: %s\n",SDL_GetGamepadName(gamepad)); }
        }
        SDL_free(ids);
    }
    return gamepad;
}
typedef struct { int source; uint32_t ps; } ButtonSource;
typedef struct { ButtonSource entries[48]; size_t count; } ButtonMap;
static ButtonMap pad_map={{
    {SDL_GAMEPAD_BUTTON_SOUTH,BTN_CROSS}, {SDL_GAMEPAD_BUTTON_EAST,BTN_CIRCLE},
    {SDL_GAMEPAD_BUTTON_WEST,BTN_SQUARE}, {SDL_GAMEPAD_BUTTON_NORTH,BTN_TRIANGLE},
    {SDL_GAMEPAD_BUTTON_LEFT_SHOULDER,BTN_L1}, {SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER,BTN_R1},
    {SDL_GAMEPAD_BUTTON_LEFT_STICK,BTN_L3}, {SDL_GAMEPAD_BUTTON_RIGHT_STICK,BTN_R3},
    {SDL_GAMEPAD_BUTTON_START,BTN_OPTIONS}, {SDL_GAMEPAD_BUTTON_BACK,BTN_TOUCHPAD},
    {SDL_GAMEPAD_BUTTON_TOUCHPAD,BTN_TOUCHPAD},
    {SDL_GAMEPAD_BUTTON_DPAD_UP,BTN_UP}, {SDL_GAMEPAD_BUTTON_DPAD_DOWN,BTN_DOWN},
    {SDL_GAMEPAD_BUTTON_DPAD_LEFT,BTN_LEFT}, {SDL_GAMEPAD_BUTTON_DPAD_RIGHT,BTN_RIGHT},
},15};
static ButtonMap key_map={{
    {SDL_SCANCODE_SPACE,BTN_CROSS}, {SDL_SCANCODE_LSHIFT,BTN_CIRCLE}, {SDL_SCANCODE_E,BTN_SQUARE},
    {SDL_SCANCODE_Q,BTN_TRIANGLE}, {SDL_SCANCODE_1,BTN_L1}, {SDL_SCANCODE_3,BTN_R1},
    {SDL_SCANCODE_R,BTN_L2}, {SDL_SCANCODE_F,BTN_R2}, {SDL_SCANCODE_Z,BTN_L3}, {SDL_SCANCODE_C,BTN_R3},
    {SDL_SCANCODE_RETURN,BTN_OPTIONS},
    {SDL_SCANCODE_I,BTN_UP}, {SDL_SCANCODE_K,BTN_DOWN}, {SDL_SCANCODE_J,BTN_LEFT}, {SDL_SCANCODE_L,BTN_RIGHT},
},15};

static void sample_host(PadData *d) {
    memset(d,0,sizeof(*d));
    d->left_x=d->left_y=d->right_x=d->right_y=128;
    d->orientation[3]=1.0f;
    d->timestamp=now_us();
    SDL_Gamepad *g=current_gamepad();
    d->connected=g || SDL_WasInit(SDL_INIT_VIDEO);
    const bool *k=SDL_GetKeyboardFocus() ? SDL_GetKeyboardState(NULL) : NULL;
    if (g) {
        for (size_t i=0;i<pad_map.count;++i)
            if (SDL_GetGamepadButton(g,(SDL_GamepadButton)pad_map.entries[i].source)) d->buttons|=pad_map.entries[i].ps;
        const uint32_t mapped=d->buttons;
        d->left_x=axis(SDL_GetGamepadAxis(g,SDL_GAMEPAD_AXIS_LEFTX)); d->left_y=axis(SDL_GetGamepadAxis(g,SDL_GAMEPAD_AXIS_LEFTY));
        d->right_x=axis(SDL_GetGamepadAxis(g,SDL_GAMEPAD_AXIS_RIGHTX)); d->right_y=axis(SDL_GetGamepadAxis(g,SDL_GAMEPAD_AXIS_RIGHTY));
        d->l2=trigger(SDL_GetGamepadAxis(g,SDL_GAMEPAD_AXIS_LEFT_TRIGGER)); d->r2=trigger(SDL_GetGamepadAxis(g,SDL_GAMEPAD_AXIS_RIGHT_TRIGGER));
        if (d->l2>30) d->buttons|=BTN_L2;
        if (d->r2>30) d->buttons|=BTN_R2;
        if (mapped & BTN_L2) d->l2=255;
        if (mapped & BTN_R2) d->r2=255;
        if (SDL_GetNumGamepadTouchpads(g)>0) {
            const int fingers=SDL_GetNumGamepadTouchpadFingers(g,0);
            for (int finger=0;finger<fingers && d->touch_count<2;++finger) {
                bool down=false;
                float x=0, y=0;
                if (SDL_GetGamepadTouchpadFinger(g,0,finger,&down,&x,&y,NULL) && down) {
                    d->touches[d->touch_count++]=(PadTouch){.x=touch_axis(x,1919),
                        .y=touch_axis(y,942),.id=(uint8_t)finger};
                }
            }
        }
        // Back/Select on pads without a touch surface is a left-side click.
        if ((d->buttons & BTN_TOUCHPAD) && !d->touch_count) touch_click(d,0);
    }
    /* The keyboard works with or without a gamepad: its buttons add to the pad's, and a stick
     * follows the keys only while one of them is held. */
    if (!k) return;
    for (size_t i=0;i<key_map.count;++i) if (k[key_map.entries[i].source]) d->buttons|=key_map.entries[i].ps;
    if (k[SDL_SCANCODE_TAB]) touch_click(d,0);
    if ((d->buttons & BTN_TOUCHPAD) && !d->touch_count) touch_click(d,0); /* a remapped touchpad key */
    if (k[SDL_SCANCODE_BACKSPACE]) touch_click(d,1);
    if (d->buttons & BTN_L2) d->l2=255;
    if (d->buttons & BTN_R2) d->r2=255;
    key_axis(&d->left_x,k[SDL_SCANCODE_A],k[SDL_SCANCODE_D]);
    key_axis(&d->left_y,k[SDL_SCANCODE_W],k[SDL_SCANCODE_S]);
    key_axis(&d->right_x,k[SDL_SCANCODE_LEFT],k[SDL_SCANCODE_RIGHT]);
    key_axis(&d->right_y,k[SDL_SCANCODE_UP],k[SDL_SCANCODE_DOWN]);
}


#else
static void sample_host(PadData *d) {
    memset(d,0,sizeof(*d));d->left_x=d->left_y=d->right_x=d->right_y=128;
    d->orientation[3]=1;d->timestamp=now_us();
    /* CPU-only diagnostics have no input backend and report disconnected. */
}
#endif
static int host_init(void) {
#ifdef TLG_WITH_RENDERER
    if(!SDL_WasInit(SDL_INIT_GAMEPAD) && !SDL_InitSubSystem(SDL_INIT_GAMEPAD)) {
        fprintf(stderr,"Pad: SDL initialization failed: %s\n",SDL_GetError());return 0;
    }
    sdl_ready=1;
#endif
    return 1;
}
static void host_close(void) {
#ifdef TLG_WITH_RENDERER
    if(gamepad) {SDL_CloseGamepad(gamepad);gamepad=NULL;}
#endif
}
static int32_t host_vibrate(const uint8_t *motors) {
#ifdef TLG_WITH_RENDERER
    SDL_Gamepad *g=current_gamepad();
    if(!g) return PAD_ERROR(7);
    if(!SDL_RumbleGamepad(g,(uint16_t)(motors[0]*257),(uint16_t)(motors[1]*257),1000)) return PAD_ERROR(0x101);
    return 0;
#else
    (void)motors;return PAD_ERROR(7);
#endif
}
/* Tests inject samples instead of claiming a physical device is connected. */
static void (*sample_backend)(PadData *)=sample_host;
static int (*init_backend)(void)=host_init;
static int32_t (*vibrate_backend)(const uint8_t *)=host_vibrate;
static void collect(PadData *out) {
    PadData data;sample_backend(&data);
    if(data.connected && !was_connected) {if(++connected_count==0) connected_count=1;}
    was_connected=data.connected!=0;
    data.connected_count=connected_count;*out=data;
}
static ABI int32_t pad_init(void) {
    AcquireSRWLockExclusive(&lock);
    if(!initialized && !init_backend()) {ReleaseSRWLockExclusive(&lock);return PAD_ERROR(0xff);}
    initialized=1;ReleaseSRWLockExclusive(&lock);
    puts("Runtime: Pad library initialized (SDL gamepad / keyboard with renderer; no backend in CPU-only mode)");return 0;
}
static ABI int32_t pad_open(int32_t user,int32_t type,int32_t index,const void *params) {
    if(user!=1 || type || index || params) return PAD_ERROR(1);
    AcquireSRWLockExclusive(&lock);
    int32_t result;
    if(!initialized) result=PAD_ERROR(5);
    else if(opened) result=PAD_ERROR(4);
    else if(next_handle==INT_MAX) result=PAD_ERROR(0xff);
    else {opened=next_handle++;was_connected=0;connected_count=0;result=opened;}
    ReleaseSRWLockExclusive(&lock);return result;
}
static ABI int32_t pad_close(int32_t handle) {
    AcquireSRWLockExclusive(&lock);
    if(handle<=0 || handle!=opened) {ReleaseSRWLockExclusive(&lock);return PAD_ERROR(3);}
    host_close();opened=0;was_connected=0;
    ReleaseSRWLockExclusive(&lock);return 0;
}
static ABI int32_t pad_read_state(int32_t handle,PadData *out) {
    AcquireSRWLockExclusive(&lock);
    int32_t result=handle<=0 || handle!=opened?PAD_ERROR(3):!out?PAD_ERROR(1):0;
    if(!result) collect(out);
    ReleaseSRWLockExclusive(&lock);return result;
}
/* One fresh sample per buffered read; historical samples are not fabricated. */
static ABI int32_t pad_read(int32_t handle,PadData *out,int32_t count) {
    AcquireSRWLockExclusive(&lock);
    int32_t result=handle<=0 || handle!=opened?PAD_ERROR(3):
                   !out || count<1 || count>64?PAD_ERROR(1):1;
    if(result==1) collect(out);
    ReleaseSRWLockExclusive(&lock);return result;
}
static ABI int32_t pad_info(int32_t handle,ControllerInfo *out) {
    AcquireSRWLockExclusive(&lock);
    int32_t result=handle<=0 || handle!=opened?PAD_ERROR(3):!out?PAD_ERROR(1):0;
    if(!result) {
        PadData data;collect(&data);ControllerInfo info={0};
        /* Virtual DS4 touch coordinates also support keyboard touch clicks. */
        info.pixel_density=44.86f;info.resolution_x=1920;info.resolution_y=943;
        info.dead_zone_left=info.dead_zone_right=2;
        info.connected=data.connected;info.connected_count=data.connected_count;*out=info;
    }
    ReleaseSRWLockExclusive(&lock);return result;
}
static ABI int32_t pad_vibration(int32_t handle,const uint8_t *motors) {
    AcquireSRWLockExclusive(&lock);
    int32_t result=handle<=0 || handle!=opened?PAD_ERROR(3):!motors?PAD_ERROR(1):vibrate_backend(motors);
    ReleaseSRWLockExclusive(&lock);return result;
}
uintptr_t windows_pad_resolve(const char *name) {
#define EXPORT(n,f) if(!strcmp(name,n "#pad")) return (uintptr_t)f
    EXPORT("hv1luiJrqQM",pad_init);EXPORT("xk0AcarP3V4",pad_open);
    EXPORT("6ncge5+l5Qs",pad_close);EXPORT("YndgXqQVV7c",pad_read_state);
    EXPORT("gjP9-KQzoUk",pad_info);EXPORT("yFVnOdGxvZY",pad_vibration);
    EXPORT("q1cHNfGycLI",pad_read);
#undef EXPORT
    return 0;
}

#ifdef TLG_WITH_RENDERER
static int test_sdl_sampling(void) {
#define SDL_CHECK(c) do {if(!(c)) {fprintf(stderr,"SDL pad check failed at %d: %s\n",__LINE__,SDL_GetError());return 1;}}while(0)
    SDL_CHECK(host_init());
    SDL_VirtualJoystickDesc desc;SDL_INIT_INTERFACE(&desc);
    desc.type=SDL_JOYSTICK_TYPE_GAMEPAD;desc.name="TLG diagnostic virtual controller";
    desc.naxes=SDL_GAMEPAD_AXIS_COUNT;desc.nbuttons=SDL_GAMEPAD_BUTTON_COUNT;
    desc.axis_mask=(1u<<SDL_GAMEPAD_AXIS_COUNT)-1;
    desc.button_mask=(1u<<SDL_GAMEPAD_BUTTON_COUNT)-1;
    SDL_JoystickID id=SDL_AttachVirtualJoystick(&desc);SDL_CHECK(id);
    SDL_Joystick *joystick=SDL_OpenJoystick(id);SDL_CHECK(joystick);
    gamepad=SDL_OpenGamepad(id);SDL_CHECK(gamepad);
    SDL_CHECK(SDL_SetJoystickVirtualButton(joystick,SDL_GAMEPAD_BUTTON_SOUTH,true));
    SDL_CHECK(SDL_SetJoystickVirtualAxis(joystick,SDL_GAMEPAD_AXIS_LEFTX,INT16_MIN));
    SDL_CHECK(SDL_SetJoystickVirtualAxis(joystick,SDL_GAMEPAD_AXIS_LEFTY,INT16_MAX));
    SDL_CHECK(SDL_SetJoystickVirtualAxis(joystick,SDL_GAMEPAD_AXIS_LEFT_TRIGGER,INT16_MIN));
    SDL_CHECK(SDL_SetJoystickVirtualAxis(joystick,SDL_GAMEPAD_AXIS_RIGHT_TRIGGER,INT16_MAX));
    SDL_UpdateJoysticks();PadData sample;sample_host(&sample);
    SDL_CHECK(sample.connected && (sample.buttons&BTN_CROSS) && sample.left_x==0 &&
              sample.left_y==255 && sample.l2==0 && sample.r2==255 && (sample.buttons&BTN_R2));
    SDL_CHECK(SDL_SetJoystickVirtualButton(joystick,SDL_GAMEPAD_BUTTON_SOUTH,false));
    SDL_UpdateJoysticks();sample_host(&sample);SDL_CHECK(!(sample.buttons&BTN_CROSS));
    host_close();SDL_CloseJoystick(joystick);SDL_CHECK(SDL_DetachVirtualJoystick(id));
    puts("SDL pad sampling: virtual gamepad buttons, stick endpoints, triggers and release passed");return 0;
#undef SDL_CHECK
}
#endif
static int test_connected,test_init_ok,test_vibrations;
static int test_init(void) {return test_init_ok;}
static void test_sample(PadData *out) {
    memset(out,0,sizeof(*out));out->left_x=out->left_y=out->right_x=out->right_y=128;
    out->orientation[3]=1;out->timestamp=now_us();out->connected=(uint8_t)test_connected;
    if(test_connected) {out->buttons=BTN_CROSS;out->left_x=axis(INT16_MIN);out->r2=trigger(INT16_MAX);}
}
static int32_t test_vibrate(const uint8_t *motors) {
    test_vibrations++;
    return !test_connected?PAD_ERROR(7):motors[0]==12 && motors[1]==34?0:PAD_ERROR(1);
}
int windows_pad_selftest(void) {
#define PAD_CHECK(c) do {if(!(c)) {fprintf(stderr,"Pad check failed at %d\n",__LINE__);return 1;}}while(0)
#ifdef TLG_WITH_RENDERER
    if(test_sdl_sampling()) return 1;
#endif
    init_backend=test_init;sample_backend=test_sample;vibrate_backend=test_vibrate;
    PAD_CHECK(pad_open(1,0,0,NULL)==PAD_ERROR(5));
    PAD_CHECK(pad_init()==PAD_ERROR(0xff));test_init_ok=1;
    PAD_CHECK(!pad_init() && !pad_init());
    PAD_CHECK(pad_open(2,0,0,NULL)==PAD_ERROR(1) && pad_open(1,2,0,NULL)==PAD_ERROR(1) &&
              pad_open(1,0,1,NULL)==PAD_ERROR(1) && pad_open(1,0,0,&initialized)==PAD_ERROR(1));
    int32_t handle=pad_open(1,0,0,NULL);PAD_CHECK(handle>0);
    PAD_CHECK(pad_open(1,0,0,NULL)==PAD_ERROR(4));
    struct {PadData data;uint64_t guard;} packet;memset(&packet,0x55,sizeof(packet));
    PAD_CHECK(pad_read_state(handle+1,&packet.data)==PAD_ERROR(3) && packet.data.buttons==0x55555555);
    PAD_CHECK(pad_read_state(handle,NULL)==PAD_ERROR(1));
    PAD_CHECK(pad_read(handle,&packet.data,0)==PAD_ERROR(1) && packet.data.buttons==0x55555555);
    PAD_CHECK(pad_read(handle,&packet.data,65)==PAD_ERROR(1));
    PAD_CHECK(pad_read(handle+1,NULL,0)==PAD_ERROR(3));
    PAD_CHECK(!pad_read_state(handle,&packet.data) && !packet.data.connected && !packet.data.buttons &&
              packet.data.left_x==128 && !packet.data.connected_count && packet.guard==UINT64_C(0x5555555555555555));
    uint8_t motors[2]={12,34};
    PAD_CHECK(pad_vibration(handle+1,motors)==PAD_ERROR(3) && !test_vibrations);
    PAD_CHECK(pad_vibration(handle,NULL)==PAD_ERROR(1) && !test_vibrations);
    PAD_CHECK(pad_vibration(handle,motors)==PAD_ERROR(7) && test_vibrations==1);
    test_connected=1;
    PadData samples[2];memset(samples,0x55,sizeof(samples));
    PAD_CHECK(pad_read(handle,samples,2)==1 && samples[0].connected && samples[0].buttons==BTN_CROSS &&
              samples[0].left_x==0 && samples[0].r2==255 && samples[0].connected_count==1 &&
              samples[1].buttons==0x55555555);
    PAD_CHECK(!pad_vibration(handle,motors) && test_vibrations==2);
    struct {ControllerInfo data;uint32_t guard;} info;memset(&info,0x55,sizeof(info));
    PAD_CHECK(pad_info(handle+1,&info.data)==PAD_ERROR(3) && info.data.connected==0x55);
    PAD_CHECK(pad_info(handle,NULL)==PAD_ERROR(1) && !pad_info(handle,&info.data));
    PAD_CHECK(info.data.connected && info.data.connected_count==1 && info.data.resolution_x==1920 &&
              info.data.resolution_y==943 && info.guard==0x55555555);
    test_connected=0;PAD_CHECK(!pad_read_state(handle,&packet.data) && !packet.data.connected);
    test_connected=1;PAD_CHECK(!pad_read_state(handle,&packet.data) && packet.data.connected_count==2);
    PAD_CHECK(!pad_close(handle) && pad_close(handle)==PAD_ERROR(3));
    int32_t reopened=pad_open(1,0,0,NULL);PAD_CHECK(reopened>handle && pad_read_state(handle,&packet.data)==PAD_ERROR(3));
    PAD_CHECK(!pad_close(reopened));
    PAD_CHECK(axis(INT16_MIN)==0 && axis(0)==128 && axis(INT16_MAX)==255 &&
              trigger(INT16_MIN)==0 && trigger(0)==0 && trigger(INT16_MAX)==255);
    uint8_t value=128;key_axis(&value,true,false);PAD_CHECK(value==0);
    key_axis(&value,false,true);PAD_CHECK(value==255);
    PAD_CHECK(touch_axis(-1,1919)==0 && touch_axis(2,1919)==1919);
    memset(&packet.data,0,sizeof(packet.data));touch_click(&packet.data,1);
    PAD_CHECK(packet.data.buttons==BTN_TOUCHPAD && packet.data.touch_count==1 && packet.data.touches[0].x==1440);
    PAD_CHECK(windows_pad_resolve("hv1luiJrqQM#pad")== (uintptr_t)pad_init &&
              !windows_pad_resolve("hv1luiJrqQM#other"));
    init_backend=host_init;sample_backend=sample_host;vibrate_backend=host_vibrate;
    puts("Pad: lifecycle, sample bounds, conversion, disconnect/reconnect, stale handles and rumble errors passed");return 0;
#undef PAD_CHECK
}
