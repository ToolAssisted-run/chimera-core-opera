/* cinterface.c - the Opera (3DO) libretro core behind the chimera guest ABI.
 *
 * Descended from the author's BizHawk waterbox/opera/bizhawk.cpp, reshaped
 * for chimera's generic waterbox adapter: settings arrive through the mounted
 * "settings" JSON, the disc and firmware arrive as real files in the guest
 * filesystem (upstream's retro_cdimage/VFS reads them - no CD callbacks at
 * all), and lag detection is the InputWasRead export over a flag the patched
 * opera_madam.c raises on player-bus DMA.
 *
 * The libretro surface is driven exactly as BizHawk drove it: the callback
 * shims answer the core's variables (opera_bios, opera_font, opera_region,
 * opera_active_devices, opera_vdlp_pixel_format), retro_load_game gets the
 * mounted disc's real name, and the input-state callback serves the wire
 * described below.
 *
 * This file compiles IDENTICALLY for the guest (miniBox emulibc) and for the
 * native reference build (native-shim/emulibc.h), which is what makes the
 * equivalence gate a real proof.
 */
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include <emulibc.h>
#include <waterbox_settings.h>
#include <waterbox_slots.h>

#include <libretro.h>
#include <lr_input.h>
#include <libopera/opera_mem.h>

extern void opera_lr_callbacks_set_audio_sample_batch(retro_audio_sample_batch_t cb);
extern void opera_lr_callbacks_set_environment(retro_environment_t cb);
extern void opera_lr_callbacks_set_input_poll(retro_input_poll_t cb);
extern void opera_lr_callbacks_set_input_state(retro_input_state_t cb);
extern void opera_lr_callbacks_set_video_refresh(retro_video_refresh_t cb);
extern int opera_input_ports_read; /* patched opera_madam.c raises it on PBus DMA */

/* The one file this machine keeps, in and out under the same name. */
#define NVRAM_FILE_NAME "NVRAM.ram"

static char g_loadError[512];
static int g_inited;

/* ---- the wire format: waterbox.config "input.buttons" order ----
 * 0 Reset, 1 Previous Disc, 2 Next Disc, then one 42-button block per port
 * (P1 at 3, P2 at 45); within a block:
 *   0..10  gamepad     Up Down Left Right X P A B C L R
 *  11..14  mouse       Left Middle Right Fourth
 *  15..26  flightstick Up Down Left Right Fire A B C X P LT RT
 *  27..30  lightgun    Trigger Select Reload Offscreen
 *  31..36  arcade gun  Trigger Select Start Reload AuxA Offscreen
 *  37..41  trackball   StartP1 StartP2 CoinP1 CoinP2 Service
 * Axes (SetAxis), 9 per port (P1 at 0, P2 at 9):
 *   0 MouseX 1 MouseY 2 StickH 3 StickV 4 StickAlt 5 GunX 6 GunY 7 TBX 8 TBY
 * Which block a port actually serves is the port1/port2 setting - exactly
 * how the author's BizHawk core keyed its controller definition. */
#define BTN_PORT_BASE 3
#define BTN_PER_PORT 42
#define BTN_COUNT (BTN_PORT_BASE + 2 * BTN_PER_PORT)
#define AXES_PER_PORT 9
#define AXIS_COUNT (2 * AXES_PER_PORT)

/* per-port block offsets */
#define GP_UP 0
#define GP_X 4
#define GP_P 5
#define GP_A 6
#define GP_B 7
#define GP_C 8
#define GP_L 9
#define GP_R 10
#define MS_LEFT 11
#define FS_UP 15
#define FS_FIRE 19
#define FS_A 20
#define FS_B 21
#define FS_C 22
#define FS_X 23
#define FS_P 24
#define FS_LT 25
#define FS_RT 26
#define LG_TRIGGER 27
#define AG_TRIGGER 31
#define TB_STARTP1 37

static uint8_t g_buttons[BTN_COUNT];
static int32_t g_axes[AXIS_COUNT];

static int g_port1Type;
static int g_port2Type;
static int g_region; /* 0 ntsc, 1 pal1, 2 pal2 */

static char g_biosName[64];
static char g_fontName[64] = "None";

/* the cd slot's swap list; index is machine state (savestated) */
#define MAX_DISCS 32
static char g_discs[MAX_DISCS][256];
static int g_discCount;
static int g_discIndex;
static uint8_t g_prevDiscBtn[2];

/* ---- video: the core renders XRGB8888 into its own buffer and hands it
 * over per refresh; the ABI serves a packed opaque copy ---- */
#define VID_MAX_W 640
#define VID_MAX_H 576
static uint32_t g_videoOut[VID_MAX_W * VID_MAX_H];
static int g_vwidth = 320, g_vheight = 240;

static const uint32_t *g_lastFrame;

/* Turbo. patches/ makes the VDLP - the 3DO's display processor - read this and
 * skip its scanline renderer while it is 0. Everything the ARM can see, cel
 * engine included, runs regardless; only the picture stops. ECL_INVISIBLE
 * because it is the frontend's policy for the moment, not part of the machine:
 * a state saved while fast-forwarding must not put the machine back into it
 * when it is loaded to be looked at. */
ECL_INVISIBLE int chimera_render_enabled = 1;
static unsigned g_lastW, g_lastH;
static size_t g_lastPitch;

#define MAX_SAMPLES 8192
static int16_t g_soundbuffer[MAX_SAMPLES * 2];
static int g_nsamples;

static void RETRO_CALLCONV video_refresh_cb(const void *data, unsigned width,
	unsigned height, size_t pitch)
{
	g_lastFrame = (const uint32_t *)data;
	g_lastW = width;
	g_lastH = height;
	g_lastPitch = pitch;
}

static size_t RETRO_CALLCONV audio_sample_batch_cb(const int16_t *data, size_t frames)
{
	size_t room = MAX_SAMPLES - (size_t)g_nsamples;
	if (frames > room)
		frames = room;
	memcpy(g_soundbuffer + (size_t)g_nsamples * 2, data, frames * 2 * sizeof(int16_t));
	g_nsamples += (int)frames;
	return frames;
}

static void RETRO_CALLCONV input_poll_cb(void)
{
}

#include <stdarg.h>
static void RETRO_CALLCONV log_printf_cb(enum retro_log_level level, const char *format, ...)
{
	(void)level;
	va_list ap;
	va_start(ap, format);
	vfprintf(stderr, format, ap);
	va_end(ap);
}

/* the wire -> retro input-state mapping, the same table as the author's
 * BizHawk processController, keyed off the port's configured device */
static int16_t port_state(int portType, const uint8_t *b, const int32_t *a,
	unsigned device, unsigned index, unsigned id)
{
	(void)device;
	switch (portType)
	{
	case RETRO_DEVICE_JOYPAD:
		switch (id)
		{
			case RETRO_DEVICE_ID_JOYPAD_UP: return b[GP_UP + 0];
			case RETRO_DEVICE_ID_JOYPAD_DOWN: return b[GP_UP + 1];
			case RETRO_DEVICE_ID_JOYPAD_LEFT: return b[GP_UP + 2];
			case RETRO_DEVICE_ID_JOYPAD_RIGHT: return b[GP_UP + 3];
			case RETRO_DEVICE_ID_JOYPAD_L: return b[GP_L];
			case RETRO_DEVICE_ID_JOYPAD_R: return b[GP_R];
			case RETRO_DEVICE_ID_JOYPAD_SELECT: return b[GP_X];
			case RETRO_DEVICE_ID_JOYPAD_START: return b[GP_P];
			case RETRO_DEVICE_ID_JOYPAD_Y: return b[GP_A];
			case RETRO_DEVICE_ID_JOYPAD_B: return b[GP_B];
			case RETRO_DEVICE_ID_JOYPAD_A: return b[GP_C];
			default: return 0;
		}

	case RETRO_DEVICE_MOUSE:
		switch (id)
		{
			case RETRO_DEVICE_ID_MOUSE_X: return (int16_t)a[0];
			case RETRO_DEVICE_ID_MOUSE_Y: return (int16_t)a[1];
			case RETRO_DEVICE_ID_MOUSE_LEFT: return b[MS_LEFT + 0];
			case RETRO_DEVICE_ID_MOUSE_MIDDLE: return b[MS_LEFT + 1];
			case RETRO_DEVICE_ID_MOUSE_RIGHT: return b[MS_LEFT + 2];
			case RETRO_DEVICE_ID_MOUSE_BUTTON_4: return b[MS_LEFT + 3];
			default: return 0;
		}

	case RETRO_DEVICE_FLIGHTSTICK:
		if (index == RETRO_DEVICE_INDEX_ANALOG_BUTTON)
		{
			switch (id)
			{
				case RETRO_DEVICE_ID_JOYPAD_R2: return b[FS_FIRE];
				case RETRO_DEVICE_ID_JOYPAD_Y: return b[FS_A];
				case RETRO_DEVICE_ID_JOYPAD_B: return b[FS_B];
				case RETRO_DEVICE_ID_JOYPAD_A: return b[FS_C];
				case RETRO_DEVICE_ID_JOYPAD_UP: return b[FS_UP + 0];
				case RETRO_DEVICE_ID_JOYPAD_DOWN: return b[FS_UP + 1];
				case RETRO_DEVICE_ID_JOYPAD_LEFT: return b[FS_UP + 2];
				case RETRO_DEVICE_ID_JOYPAD_RIGHT: return b[FS_UP + 3];
				case RETRO_DEVICE_ID_JOYPAD_START: return b[FS_P];
				case RETRO_DEVICE_ID_JOYPAD_SELECT: return b[FS_X];
				case RETRO_DEVICE_ID_JOYPAD_L: return b[FS_LT];
				case RETRO_DEVICE_ID_JOYPAD_R: return b[FS_RT];
				default: return 0;
			}
		}
		switch (id)
		{
			case RETRO_DEVICE_ID_ANALOG_X:
				if (index == RETRO_DEVICE_INDEX_ANALOG_LEFT) return (int16_t)a[2];
				if (index == RETRO_DEVICE_INDEX_ANALOG_RIGHT) return (int16_t)a[4];
				return 0;
			case RETRO_DEVICE_ID_ANALOG_Y:
				if (index == RETRO_DEVICE_INDEX_ANALOG_LEFT) return (int16_t)a[3];
				if (index == RETRO_DEVICE_INDEX_ANALOG_RIGHT) return (int16_t)a[4];
				return 0;
			default: return 0;
		}

	case RETRO_DEVICE_LIGHTGUN:
		switch (id)
		{
			case RETRO_DEVICE_ID_LIGHTGUN_SCREEN_X: return (int16_t)a[5];
			case RETRO_DEVICE_ID_LIGHTGUN_SCREEN_Y: return (int16_t)a[6];
			case RETRO_DEVICE_ID_LIGHTGUN_TRIGGER: return b[LG_TRIGGER + 0];
			case RETRO_DEVICE_ID_LIGHTGUN_SELECT: return b[LG_TRIGGER + 1];
			case RETRO_DEVICE_ID_LIGHTGUN_RELOAD: return b[LG_TRIGGER + 2];
			case RETRO_DEVICE_ID_LIGHTGUN_IS_OFFSCREEN: return b[LG_TRIGGER + 3];
			default: return 0;
		}

	case RETRO_DEVICE_ARCADE_LIGHTGUN:
		switch (id)
		{
			case RETRO_DEVICE_ID_LIGHTGUN_SCREEN_X: return (int16_t)a[5];
			case RETRO_DEVICE_ID_LIGHTGUN_SCREEN_Y: return (int16_t)a[6];
			case RETRO_DEVICE_ID_LIGHTGUN_TRIGGER: return b[AG_TRIGGER + 0];
			case RETRO_DEVICE_ID_LIGHTGUN_SELECT: return b[AG_TRIGGER + 1];
			case RETRO_DEVICE_ID_LIGHTGUN_START: return b[AG_TRIGGER + 2];
			case RETRO_DEVICE_ID_LIGHTGUN_RELOAD: return b[AG_TRIGGER + 3];
			case RETRO_DEVICE_ID_LIGHTGUN_AUX_A: return b[AG_TRIGGER + 4];
			case RETRO_DEVICE_ID_LIGHTGUN_IS_OFFSCREEN: return b[AG_TRIGGER + 5];
			default: return 0;
		}

	case RETRO_DEVICE_ORBATAK_TRACKBALL:
		switch (id)
		{
			case RETRO_DEVICE_ID_ANALOG_X: return (int16_t)a[7];
			case RETRO_DEVICE_ID_ANALOG_Y: return (int16_t)a[8];
			case RETRO_DEVICE_ID_JOYPAD_SELECT: return b[TB_STARTP1 + 0];
			case RETRO_DEVICE_ID_JOYPAD_START: return b[TB_STARTP1 + 1];
			case RETRO_DEVICE_ID_JOYPAD_L: return b[TB_STARTP1 + 2];
			case RETRO_DEVICE_ID_JOYPAD_R: return b[TB_STARTP1 + 3];
			case RETRO_DEVICE_ID_JOYPAD_R2: return b[TB_STARTP1 + 4];
			default: return 0;
		}

	default:
		return 0;
	}
}

static int16_t RETRO_CALLCONV input_state_cb(unsigned port, unsigned device,
	unsigned index, unsigned id)
{
	if (port == 0)
		return port_state(g_port1Type, &g_buttons[BTN_PORT_BASE], &g_axes[0],
			device, index, id);
	if (port == 1)
		return port_state(g_port2Type, &g_buttons[BTN_PORT_BASE + BTN_PER_PORT],
			&g_axes[AXES_PER_PORT], device, index, id);
	return 0;
}

/* the core's variables: BIOS/font by mounted file name, region and device
 * count from settings, XRGB8888 output; everything else keeps its default */
static char g_deviceCountStr[8];
static char g_randomSeedStr[16];
static bool RETRO_CALLCONV environment_cb(unsigned cmd, void *data)
{
	switch (cmd)
	{
	case RETRO_ENVIRONMENT_GET_LOG_INTERFACE:
		((struct retro_log_callback *)data)->log = log_printf_cb;
		return true;
	case RETRO_ENVIRONMENT_SET_PERFORMANCE_LEVEL:
	case RETRO_ENVIRONMENT_SET_SERIALIZATION_QUIRKS:
	case RETRO_ENVIRONMENT_SET_PIXEL_FORMAT:
	case RETRO_ENVIRONMENT_GET_VARIABLE_UPDATE:
		return true;
	case RETRO_ENVIRONMENT_GET_SYSTEM_DIRECTORY:
	case RETRO_ENVIRONMENT_GET_SAVE_DIRECTORY:
		/* empty: fill_pathname_join degenerates to the bare file name, which
		 * is exactly what the guest's mounted-file namespace understands */
		*(const char **)data = "";
		return true;
	case RETRO_ENVIRONMENT_GET_VARIABLE:
	{
		struct retro_variable *var = (struct retro_variable *)data;
		const char *key = var->key;
		if (strcmp(key, "opera_bios") == 0 && g_biosName[0] != 0)
			var->value = g_biosName;
		else if (strcmp(key, "opera_font") == 0 && strcmp(g_fontName, "None") != 0)
			var->value = g_fontName;
		else if (strcmp(key, "opera_region") == 0)
			var->value = g_region == 0 ? "ntsc" : (g_region == 1 ? "pal1" : "pal2");
		else if (strcmp(key, "opera_vdlp_pixel_format") == 0)
			var->value = "XRGB8888";
		else if (strcmp(key, "opera_random_seed") == 0)
			/* upstream falls back to time(NULL) without a fixed seed - a
			 * wallclock in the machine. The randomSeed SETTING pins it and
			 * movies record it. */
			var->value = g_randomSeedStr;
		else if (strcmp(key, "opera_active_devices") == 0)
		{
			int count = 0;
			if (g_port1Type != RETRO_DEVICE_NONE) count++;
			if (g_port2Type != RETRO_DEVICE_NONE) count++;
			snprintf(g_deviceCountStr, sizeof g_deviceCountStr, "%d", count);
			var->value = g_deviceCountStr;
		}
		return true;
	}
	default:
		return false;
	}
}

static int port_type(const char *name)
{
	char buf[32];
	strncpy(buf, name[4] == '1' ? "gamepad" : "none", sizeof buf - 1);
	buf[sizeof buf - 1] = 0;
	wbx_setting_str(name, buf, sizeof buf);
	if (strcmp(buf, "none") == 0) return RETRO_DEVICE_NONE;
	if (strcmp(buf, "gamepad") == 0) return RETRO_DEVICE_JOYPAD;
	if (strcmp(buf, "mouse") == 0) return RETRO_DEVICE_MOUSE;
	if (strcmp(buf, "flightStick") == 0) return RETRO_DEVICE_FLIGHTSTICK;
	if (strcmp(buf, "lightGun") == 0) return RETRO_DEVICE_LIGHTGUN;
	if (strcmp(buf, "arcadeLightGun") == 0) return RETRO_DEVICE_ARCADE_LIGHTGUN;
	if (strcmp(buf, "orbatakTrackball") == 0) return RETRO_DEVICE_ORBATAK_TRACKBALL;
	fprintf(stderr, "chimera-opera: unknown %s '%s', using gamepad\n", name, buf);
	return RETRO_DEVICE_JOYPAD;
}

/* systemType option -> the canonical dump filename the firmware channel
 * mounts (opera's own table names, extended by the author's BizHawk list) */
static const char *bios_file_for(const char *systemType)
{
	static const struct { const char *type; const char *file; } map[] = {
		{ "panasonicFZ1U", "panafz1.bin" },
		{ "panasonicFZ1E", "panafz1e.bin" },
		{ "panasonicFZ1J", "panafz1j.bin" },
		{ "panasonicFZ10U", "panafz10.bin" },
		{ "panasonicFZ10E", "panafz10e-anvil.bin" },
		{ "panasonicFZ10J", "panafz10j.bin" },
		{ "goldstarGDO101P", "goldstar.bin" },
		{ "goldstarFC1", "goldstar_fc1_enc.bin" },
		{ "sanyoIMP21JTry", "sanyotry.bin" },
		{ "sanyoHC21", "sanyo_hc21_b3_unenc.bin" },
		{ "shootoutAtOldTucson", "3do_arcade_saot.bin" },
		{ "threeDONTSC1fc2", "3do_devkit_1.0fc2.bin" },
	};
	for (size_t i = 0; i < sizeof map / sizeof map[0]; i++)
		if (strcmp(systemType, map[i].type) == 0)
			return map[i].file;
	return NULL;
}

ECL_EXPORT const char *GetLoadError(void)
{
	return g_loadError;
}

ECL_EXPORT int Init(void)
{
	g_loadError[0] = 0;

	char systemType[64] = "panasonicFZ1U";
	wbx_setting_str("systemType", systemType, sizeof systemType);
	const char *biosFile = bios_file_for(systemType);
	if (biosFile == NULL)
	{
		snprintf(g_loadError, sizeof g_loadError, "unknown systemType '%s'", systemType);
		return 0;
	}
	snprintf(g_biosName, sizeof g_biosName, "%s", biosFile);

	char fontROM[64] = "none";
	wbx_setting_str("fontROM", fontROM, sizeof fontROM);
	if (strcmp(fontROM, "panasonicFZ1Kanji") == 0)
		snprintf(g_fontName, sizeof g_fontName, "panafz1-kanji.bin");
	else if (strcmp(fontROM, "panasonicFZ10Kanji") == 0)
		snprintf(g_fontName, sizeof g_fontName, "panafz10ja-anvil-kanji.bin");
	else
		snprintf(g_fontName, sizeof g_fontName, "None");

	{
		char std[16] = "ntsc";
		wbx_setting_str("videoStandard", std, sizeof std);
		g_region = strcmp(std, "pal1") == 0 ? 1 : (strcmp(std, "pal2") == 0 ? 2 : 0);
	}

	snprintf(g_randomSeedStr, sizeof g_randomSeedStr, "%d",
		(int)wbx_setting_double("randomSeed", 0));

	g_port1Type = port_type("port1");
	g_port2Type = port_type("port2");

	/* Save data the project brought is mounted under its own name and read in
	 * below, once the machine exists. What happens here is a REFUSAL: a file
	 * named something this machine never opens would be carried by the project
	 * and pinned by the movie, and then ignored. */
	{
		char entry[512];
		int32_t saves = wbx_slot_count("savedata");
		for (int32_t i = 0; i < saves; i++)
		{
			if (wbx_slot_name("savedata", i, entry, sizeof entry) == NULL)
				continue;
			if (strcmp(entry, NVRAM_FILE_NAME) != 0)
			{
				snprintf(g_loadError, sizeof g_loadError,
					"this machine does not read save data called \"%s\". It reads %s - "
					"the name Export Save Data writes.", entry, NVRAM_FILE_NAME);
				return 0;
			}
		}
	}

	/* the disc list: the project's cd slot, else the plain "rom" mount */
	g_discCount = wbx_slot_count("cd");
	if (g_discCount > MAX_DISCS) g_discCount = MAX_DISCS;
	for (int i = 0; i < g_discCount; i++)
		wbx_slot_name("cd", i, g_discs[i], sizeof g_discs[i]);
	if (g_discCount == 0)
	{
		/* the bare rom mount is named rom.iso (waterbox.config romFile):
		 * upstream dispatches disc opens by extension */
		FILE *f = fopen("rom.iso", "rb");
		if (f == NULL)
		{
			snprintf(g_loadError, sizeof g_loadError, "no disc image is mounted");
			return 0;
		}
		fclose(f);
		snprintf(g_discs[0], sizeof g_discs[0], "rom.iso");
		g_discCount = 1;
	}
	g_discIndex = 0;

	/* the BIOS must be mounted, or the boot is a silent black screen */
	{
		FILE *f = fopen(g_biosName, "rb");
		if (f == NULL)
		{
			snprintf(g_loadError, sizeof g_loadError,
				"the '%s' BIOS rom is not mounted; satisfy the firmware page", g_biosName);
			return 0;
		}
		fclose(f);
	}

	opera_lr_callbacks_set_environment(environment_cb);
	opera_lr_callbacks_set_input_state(input_state_cb);
	opera_lr_callbacks_set_input_poll(input_poll_cb);
	opera_lr_callbacks_set_audio_sample_batch(audio_sample_batch_cb);
	opera_lr_callbacks_set_video_refresh(video_refresh_cb);

	retro_set_controller_port_device(0, (unsigned)g_port1Type);
	retro_set_controller_port_device(1, (unsigned)g_port2Type);
	retro_init();

	struct retro_game_info game;
	memset(&game, 0, sizeof game);
	game.path = g_discs[0];
	if (!retro_load_game(&game))
	{
		snprintf(g_loadError, sizeof g_loadError, "Opera could not load '%s'", g_discs[0]);
		return 0;
	}

	/* What the console starts with already remembered.
	 *
	 * NVRAM is where a 3DO keeps saved games and its own settings, and it is
	 * allocated blank when the machine is built. A project that brought one -
	 * the file Export Save Data writes, mounted back under the same name -
	 * has it read in here: after retro_load_game, so the machine exists, and
	 * still inside Init, so it lands in the sealed baseline rather than in
	 * every savestate. */
	if (NVRAM != NULL)
	{
		FILE *nv = fopen(NVRAM_FILE_NAME, "rb");
		if (nv != NULL)
		{
			size_t got = fread(NVRAM, 1, NVRAM_SIZE, nv);
			fclose(nv);
			if (got != NVRAM_SIZE)
				fprintf(stderr, "chimera: %s is %zu bytes, not %u; the rest is left blank\n",
					NVRAM_FILE_NAME, got, (unsigned)NVRAM_SIZE);
		}
	}

	g_inited = 1;
	return 1;
}

ECL_EXPORT void SetButton(int32_t index, int32_t state)
{
	if (index >= 0 && index < BTN_COUNT)
		g_buttons[index] = state != 0;
}

ECL_EXPORT void SetAxis(int32_t index, int32_t value)
{
	if (index >= 0 && index < AXIS_COUNT)
		g_axes[index] = value;
}

ECL_EXPORT void FrameAdvance(uint64_t packed)
{
	(void)packed; /* > 64 buttons: input rides the SetButton/SetAxis channels */
	if (!g_inited)
		return;

	opera_input_ports_read = 0;
	g_nsamples = 0;
	g_lastFrame = NULL;

	/* disc swapping is declared on the wire but lands with the multi-disc
	 * milestone (upstream has no public swap entry point yet); the pending
	 * index is tracked so the wire is stable */
	if (g_discCount > 1)
	{
		if (g_buttons[1] && !g_prevDiscBtn[0]) { /* previous: reserved */ }
		if (g_buttons[2] && !g_prevDiscBtn[1]) { /* next: reserved */ }
		g_prevDiscBtn[0] = g_buttons[1];
		g_prevDiscBtn[1] = g_buttons[2];
	}

	/* Reset is the console button, exactly as the author's BizHawk core:
	 * the frame either resets or runs */
	if (g_buttons[0])
		retro_reset();
	else
		retro_run();

	/* In turbo the VDLP rendered nothing, so the buffer the callback handed
	 * over holds the last frame that was drawn and there is nothing to copy. */
	if (g_lastFrame != NULL && chimera_render_enabled)
	{
		unsigned w = g_lastW <= VID_MAX_W ? g_lastW : VID_MAX_W;
		unsigned h = g_lastH <= VID_MAX_H ? g_lastH : VID_MAX_H;
		size_t spitch = g_lastPitch / 4;
		for (unsigned y = 0; y < h; y++)
		{
			const uint32_t *s = g_lastFrame + (size_t)y * spitch;
			uint32_t *d = g_videoOut + (size_t)y * w;
			for (unsigned x = 0; x < w; x++)
				d[x] = s[x] | 0xff000000u; /* opaque; the core leaves X alpha 0 */
		}
		g_vwidth = (int)w;
		g_vheight = (int)h;
	}
}

/* Turbo (optional guest ABI group): while off the core must produce no picture
 * and must otherwise be exactly the machine it would have been. run-gate.sh's
 * turbo leg is the proof - N undrawn frames plus one drawn one come out byte for
 * byte the same machine, and the same picture, as N+1 drawn ones. */
ECL_EXPORT void SetRenderingEnabled(int on) { chimera_render_enabled = on != 0; }

ECL_EXPORT uint32_t *GetVideoBgra(void) { return g_videoOut; }
ECL_EXPORT int GetVideoWidth(void) { return g_vwidth; }
ECL_EXPORT int GetVideoHeight(void) { return g_vheight; }

ECL_EXPORT int16_t *GetAudio(void) { return g_soundbuffer; }
ECL_EXPORT int GetAudioSampleCount(void) { return g_nsamples; }

/* The machine's field rate, keyed off the videoStandard setting.
 *
 * Not the standard's round number: opera's own clock keeps the rate as a
 * 16.16 fixed-point value and drives the machine from it, so that is what a
 * frame of this core actually lasts. OPERA_NTSC_FIELD_RATE_1616 is 3928227,
 * over the 65536 of the fixed point, which is 59.93998718Hz; PAL's constant
 * comes out at exactly 50. One FrameAdvance is one retro_run is one field, so
 * these are the rate a movie of this machine ran at.
 *
 * It said a flat 60 for a while, which is nobody's rate: not the standard's
 * 59.94, and not the 59.93998718 this core times itself by. Half a second of
 * drift over a two-hour movie, in the number a frontend turns into a running
 * time. */
ECL_EXPORT int GetVsyncNumerator(void)
{
	return g_region == 0 ? 3928227 : 50;
}
ECL_EXPORT int GetVsyncDenominator(void)
{
	return g_region == 0 ? 65536 : 1;
}

ECL_EXPORT int InputWasRead(void)
{
	return opera_input_ports_read;
}

/* ---- memory domains: the author's GetMemoryAreas list ---- */
static const char *memdom(int i, void **area, int64_t *size, int *writable)
{
	switch (i)
	{
	case 0:
		*area = retro_get_memory_data(RETRO_MEMORY_SYSTEM_RAM);
		*size = (int64_t)retro_get_memory_size(RETRO_MEMORY_SYSTEM_RAM);
		*writable = 1;
		return "System RAM";
	case 1:
		*area = retro_get_memory_data(RETRO_MEMORY_VIDEO_RAM);
		*size = (int64_t)retro_get_memory_size(RETRO_MEMORY_VIDEO_RAM);
		*writable = 1;
		return "Video RAM";
	case 2:
		*area = NVRAM;
		*size = NVRAM_SIZE;
		*writable = 1;
		return "Non-volatile RAM";
	default:
		return NULL;
	}
}

ECL_EXPORT int GetMemoryDomainCount(void) { return 3; }
ECL_EXPORT const char *GetMemoryDomainName(int i)
{
	void *area;
	int64_t size;
	int w;
	return memdom(i, &area, &size, &w);
}
ECL_EXPORT uint8_t *GetMemoryDomainPtr(int i)
{
	void *area = NULL;
	int64_t size;
	int w;
	return memdom(i, &area, &size, &w) != NULL ? (uint8_t *)area : NULL;
}
ECL_EXPORT int64_t GetMemoryDomainSize(int i)
{
	void *area;
	int64_t size = 0;
	int w;
	return memdom(i, &area, &size, &w) != NULL ? size : 0;
}
ECL_EXPORT int GetMemoryDomainWritable(int i)
{
	void *area;
	int64_t size;
	int w = 0;
	return memdom(i, &area, &size, &w) != NULL ? w : 0;
}

/* ---- savedata export (chimera's persistent-data channel) ----
 * The console's non-volatile RAM starts fresh every boot (no ambient
 * persistent state); this group is the way OUT for the user's saves. */
ECL_EXPORT int32_t GetSaveDataFileCount(void)
{
	return NVRAM != NULL ? 1 : 0;
}
ECL_EXPORT const char *GetSaveDataFileName(int32_t i)
{
	return i == 0 && NVRAM != NULL ? NVRAM_FILE_NAME : NULL;
}
ECL_EXPORT int64_t GetSaveDataFileSize(int32_t i)
{
	return i == 0 && NVRAM != NULL ? NVRAM_SIZE : 0;
}
ECL_EXPORT const uint8_t *GetSaveDataFileBuffer(int32_t i)
{
	return i == 0 ? (const uint8_t *)NVRAM : NULL;
}
