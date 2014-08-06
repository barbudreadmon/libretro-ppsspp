#include "libretro.h"
#include "libretro_host.h"

#include "Core/Config.h"
#include "Core/CoreParameter.h"
#include "Core/CoreTiming.h"
#include "Core/HLE/sceCtrl.h"
#include "Core/HLE/sceDisplay.h"
#include "Core/HLE/sceUtility.h"
#include "Core/Host.h"
#include "Core/SaveState.h"
#include "Core/System.h"
#include "gfx/gl_common.h"
#include "file/vfs.h"
#include "file/zip_read.h"
#include "GPU/GPUState.h"
#include "input/input_state.h"
#include "native/gfx_es2/gl_state.h"

#include <cstring>

#ifdef _WIN32
// for MAX_PATH
#include <windows.h>
#endif

#if defined(MAX_PATH) && !defined(PATH_MAX)
#define PATH_MAX MAX_PATH
#endif

static CoreParameter coreParam;
static struct retro_hw_render_callback hw_render;
static retro_log_printf_t log_cb;
static retro_video_refresh_t video_cb;
static retro_audio_sample_batch_t audio_batch_cb;
static retro_input_poll_t input_poll_cb;
static retro_input_state_t input_state_cb;
static retro_environment_t environ_cb;
static bool _initialized;
static PMixer *libretro_mixer;

static uint32_t screen_width, screen_height,
                screen_pitch;

// linker stubs
std::string System_GetProperty(SystemProperty prop) { return ""; }
void NativeUpdate(InputState &input_state) { }
void NativeRender()
{
   glstate.Restore();

   ReapplyGfxState();

   // We just run the CPU until we get to vblank. This will quickly sync up pretty nicely.
   // The actual number of cycles doesn't matter so much here as we will break due to CORE_NEXTFRAME, most of the time hopefully...
   int blockTicks = usToCycles(1000000 / 10);

   // Run until CORE_NEXTFRAME
   while (coreState == CORE_RUNNING)
      PSP_RunLoopFor(blockTicks);

   // Hopefully coreState is now CORE_NEXTFRAME
   if (coreState == CORE_NEXTFRAME)
      // set back to running for the next frame
      coreState = CORE_RUNNING;
}
void NativeResized() { }
InputState input_state;

extern "C"
{
GLuint libretro_framebuffer;
retro_hw_get_proc_address_t libretro_get_proc_address;
}

static void update_sound(void)
{
   if (libretro_mixer && audio_batch_cb)
   {
      int16_t audio[8192 * 2];
      int samples = libretro_mixer->Mix(audio, 8192);
      audio_batch_cb(audio, samples);
   }
}

class LibretroHost : public Host {
public:
	LibretroHost() {
	}

	virtual void UpdateUI() {}

	virtual void UpdateMemView() {}
	virtual void UpdateDisassembly() {}

	virtual void SetDebugMode(bool mode) {}

	virtual bool InitGL(std::string *error_message) { return true; }
	virtual void ShutdownGL() {}

	virtual void InitSound(PMixer *mixer) { libretro_mixer = mixer; };
	virtual void UpdateSound() { update_sound(); }
	virtual void ShutdownSound() { libretro_mixer = nullptr; };

	virtual void BootDone() {}

	virtual bool IsDebuggingEnabled() { return false; }
	virtual bool AttemptLoadSymbolMap() { return false; }
	virtual void ResetSymbolMap() {}
	virtual void AddSymbol(std::string name, u32 addr, u32 size, int type=0) {}
	virtual void SetWindowTitle(const char *message) {}
};

void retro_set_environment(retro_environment_t cb)
{
   static const struct retro_variable vars[] = {
      { "ppsspp_cpu_core", "CPU Core; jit|interpreter" },
      { "ppsspp_locked_cpu_speed", "Locked CPU Speed; off|222MHz|266MHz|333MHz" },
      { "ppsspp_language", "Language; automatic|english|japanese|french|spanish|german|italian|dutch|portuguese|russian|korean|chinese_traditional|chinese_simplified" },
      { "ppsspp_rendering_mode", "Rendering Mode; buffered|nonbuffered|read_framebuffers_to_memory_cpu|read_framebuffers_to_memory_gpu" },
      { "ppsspp_auto_frameskip", "Auto Frameskip; disabled|enabled" },
      { "ppsspp_frameskip", "Frameskip; 0|1|2|3|4|5|6|7|8|9" },
      { "ppsspp_internal_resolution",
         "Internal Resolution ; 480x272|960x544|1440x816|1920x1088|2400x1360|2880x1632|3360x1904|3840x2176|4320x2448|4800x2720" 
      },
      { "ppsspp_output_resolution",
         "Output Resolution ; 480x272|960x544|1440x816|1920x1088|2400x1360|2880x1632|3360x1904|3840x2176|4320x2448|4800x2720" 
      },
      { "ppsspp_button_preference", "Confirmation Button; cross|circle" },
      { "ppsspp_fast_memory", "Fast Memory (Speedhack); disabled|enabled" },
      { "ppsspp_texture_scaling_level", "Texture Scaling Level; 0|1|2|3|4|5" },
      { "ppsspp_texture_scaling_type", "Texture Scaling Type; xbrz|hybrid|bicubic|hybrid_bicubic" },
#ifdef USING_GLES2
      // TODO/FIXME - several GLES2-only devices actually have 
      // GL_EXT_texture_filter_anisotropic extension, so check 
      // if extension is available at runtime
      { "ppsspp_texture_anisotropic_filtering", "Anisotropic Filtering; off" },
#else
      { "ppsspp_texture_anisotropic_filtering", "Anisotropic Filtering; off|1x|2x|3x|4x|5x" },
#endif
      { "ppsspp_texture_deposterize", "Texture Deposterize; disabled|enabled" }, 
      { "ppsspp_gpu_hardware_transform", "GPU Hardware T&L; enabled|disabled" },
      { "ppsspp_vertex_cache", "Vertex Cache (Speedhack); disabled|enabled" },
      { "ppsspp_separate_cpu_thread", "CPU Threading; disabled|enabled" },
      { "ppsspp_separate_io_thread", "IO Threading; disabled|enabled" },
      { "ppsspp_unsafe_func_replacements", "Unsafe FuncReplacements; enabled|disabled" },
      { "ppsspp_sound_speedhack", "Sound Speedhack; disabled|enabled" },
      { NULL, NULL },
   };

   environ_cb = cb;

   cb(RETRO_ENVIRONMENT_SET_VARIABLES, (void*)vars);
}

void retro_set_audio_sample(retro_audio_sample_t cb)
{
   (void)cb;
}

void retro_set_audio_sample_batch(retro_audio_sample_batch_t cb)
{
   audio_batch_cb = cb;
}

void retro_set_input_poll(retro_input_poll_t cb)
{
   input_poll_cb = cb;
}

void retro_set_input_state(retro_input_state_t cb)
{
   input_state_cb = cb;
}

void retro_set_video_refresh(retro_video_refresh_t cb)
{
   video_cb = cb;
}

std::string retro_base_dir;
bool retro_base_dir_found;

void retro_init(void)
{
   const char *dir_ptr = NULL;
   struct retro_log_callback log;

   if (environ_cb(RETRO_ENVIRONMENT_GET_LOG_INTERFACE, &log))
      log_cb = log.log;
   else
      log_cb = NULL;

   retro_base_dir_found = false;

   if (environ_cb(RETRO_ENVIRONMENT_GET_SYSTEM_DIRECTORY, &dir_ptr) && dir_ptr)
   {
      retro_base_dir = dir_ptr;
      // Make sure that we don't have any lingering slashes, etc, as they break Windows.
      size_t last = retro_base_dir.find_last_not_of("/\\");
      if (last != std::string::npos)
         last++;

      retro_base_dir = retro_base_dir.substr(0, last);
      retro_base_dir_found = true;
   }
}

void retro_deinit(void) {}

void retro_set_controller_port_device(unsigned port, unsigned device)
{
   (void)port;
   (void)device;
}

void retro_get_system_info(struct retro_system_info *info)
{
   memset(info, 0, sizeof(*info));
   info->library_name     = "PPSSPP";
   info->library_version  = PPSSPP_GIT_VERSION;
   info->need_fullpath    = true;
   info->valid_extensions = "elf|iso|cso|prx|pbp";
}

void retro_get_system_av_info(struct retro_system_av_info *info)
{
   info->timing.fps = 60.0f / 1.001f;
   info->timing.sample_rate = 44100.0;

   info->geometry.base_width = screen_width;
   info->geometry.base_height = screen_height;
   info->geometry.max_width = screen_width;
   info->geometry.max_height = screen_height;
   info->geometry.aspect_ratio = 16.0 / 9.0;
}

unsigned retro_api_version(void)
{
   return RETRO_API_VERSION;
}

static void extract_directory(char *buf, const char *path, size_t size)
{
   char *base;
   strncpy(buf, path, size - 1);
   buf[size - 1] = '\0';

   base = strrchr(buf, '/');
   if (!base)
      base = strrchr(buf, '\\');

   if (base)
      *base = '\0';
   else
   {
      buf[0] = '.';
      buf[1] = '\0';
   }
}

static void context_reset(void)
{
   if (log_cb)
      log_cb(RETRO_LOG_INFO, "Context reset!\n");
}

static void set_language_auto(void)
{
   unsigned int val = 1;

   if (environ_cb(RETRO_ENVIRONMENT_GET_LANGUAGE, &val))
   {
      // PPSSPP language values for these two languages 
      // differ from the RETRO_LANGUAGE enum values
      if (val == RETRO_LANGUAGE_ENGLISH)
         val = 1;
      else if (val == RETRO_LANGUAGE_JAPANESE)
         val = 0;
   }
   else
      val = 1;

   g_Config.iLanguage = val;
}

static void check_variables(void)
{
   struct retro_variable var;

   var.key = "ppsspp_internal_resolution";
   var.value = NULL;

   if (environ_cb(RETRO_ENVIRONMENT_GET_VARIABLE, &var) && var.value)
   {
      if (sscanf(var.value ? var.value : "480x272", "%dx%d", &coreParam.renderWidth, &coreParam.renderHeight) != 2)
      {
         coreParam.renderWidth = 480;
         coreParam.renderHeight = 272;
      }
   }
   else
   {
      coreParam.renderWidth = 480;
      coreParam.renderHeight = 272;
   }

   var.key = "ppsspp_output_resolution";
   var.value = NULL;

   if (environ_cb(RETRO_ENVIRONMENT_GET_VARIABLE, &var) && var.value)
   {
      if (sscanf(var.value ? var.value : "480x272", "%dx%d", &screen_width, &screen_height) != 2)
      {
         screen_width = 480;
         screen_height = 272;
      }
   }
   else
   {
      screen_width = 480;
      screen_height = 272;
   }
   coreParam.pixelWidth = screen_width;
   coreParam.pixelHeight = screen_height;

   var.key = "ppsspp_button_preference";
   var.value = NULL;

   if (environ_cb(RETRO_ENVIRONMENT_GET_VARIABLE, &var) && var.value)
   {
      if (strcmp(var.value, "cross") == 0)
         g_Config.iButtonPreference = PSP_SYSTEMPARAM_BUTTON_CROSS;
      else if (strcmp(var.value, "circle") == 0)
         g_Config.iButtonPreference = PSP_SYSTEMPARAM_BUTTON_CIRCLE;
   }
   else
         g_Config.iButtonPreference = PSP_SYSTEMPARAM_BUTTON_CROSS;

   var.key = "ppsspp_fast_memory";
   var.value = NULL;

   if (environ_cb(RETRO_ENVIRONMENT_GET_VARIABLE, &var) && var.value)
   {
      if (!strcmp(var.value, "enabled"))
         g_Config.bFastMemory = true;
      else if (!strcmp(var.value, "disabled"))
         g_Config.bFastMemory = false;
   }
   else
         g_Config.bFastMemory = false;

   var.key = "ppsspp_vertex_cache";
   var.value = NULL;

   if (environ_cb(RETRO_ENVIRONMENT_GET_VARIABLE, &var) && var.value)
   {
      if (!strcmp(var.value, "enabled"))
         g_Config.bVertexCache = true;
      else if (!strcmp(var.value, "disabled"))
         g_Config.bVertexCache = false;
   }
   else
         g_Config.bVertexCache = false;

   var.key = "ppsspp_gpu_hardware_transform";
   var.value = NULL;

   if (environ_cb(RETRO_ENVIRONMENT_GET_VARIABLE, &var) && var.value)
   {
      if (!strcmp(var.value, "enabled"))
         g_Config.bHardwareTransform = true;
      else if (!strcmp(var.value, "disabled"))
         g_Config.bHardwareTransform = false;
   }
   else
         g_Config.bHardwareTransform = true;

   var.key = "ppsspp_frameskip";
   var.value = NULL;

   if (environ_cb(RETRO_ENVIRONMENT_GET_VARIABLE, &var) && var.value)
   {
      int new_val = atoi(var.value);
      g_Config.iFrameSkip = new_val;
   }
   else
      g_Config.iFrameSkip = 0;

   var.key = "ppsspp_language";
   var.value = NULL;

   if (environ_cb(RETRO_ENVIRONMENT_GET_VARIABLE, &var) && var.value)
   {
      if (!strcmp(var.value, "automatic"))
         set_language_auto();
      else if (!strcmp(var.value, "japanese"))
         g_Config.iLanguage = 0;
      else if (!strcmp(var.value, "english"))
         g_Config.iLanguage = 1;
      else if (!strcmp(var.value, "french"))
         g_Config.iLanguage = 2;
      else if (!strcmp(var.value, "spanish"))
         g_Config.iLanguage = 3;
      else if (!strcmp(var.value, "german"))
         g_Config.iLanguage = 4;
      else if (!strcmp(var.value, "italian"))
         g_Config.iLanguage = 5;
      else if (!strcmp(var.value, "dutch"))
         g_Config.iLanguage = 6;
      else if (!strcmp(var.value, "portuguese"))
         g_Config.iLanguage = 7;
      else if (!strcmp(var.value, "russian"))
         g_Config.iLanguage = 8;
      else if (!strcmp(var.value, "korean"))
         g_Config.iLanguage = 9;
      else if (!strcmp(var.value, "chinese_traditional"))
         g_Config.iLanguage = 10;
      else if (!strcmp(var.value, "chinese_simplified"))
         g_Config.iLanguage = 11;
   }
   else
      set_language_auto();

   var.key = "ppsspp_auto_frameskip";
   var.value = NULL;

   if (environ_cb(RETRO_ENVIRONMENT_GET_VARIABLE, &var) && var.value)
   {
      if (!strcmp(var.value, "enabled"))
         g_Config.bAutoFrameSkip = true;
      else if (!strcmp(var.value, "disabled"))
         g_Config.bAutoFrameSkip = false;
   }
   else
         g_Config.bAutoFrameSkip = false;

   var.key = "ppsspp_texture_scaling_type";
   var.value = NULL;

   if (environ_cb(RETRO_ENVIRONMENT_GET_VARIABLE, &var) && var.value)
   {
      if (!strcmp(var.value, "xbrz"))
         g_Config.iTexScalingType = 0;
      else if (!strcmp(var.value, "hybrid"))
         g_Config.iTexScalingType = 1;
      else if (!strcmp(var.value, "bicubic"))
         g_Config.iTexScalingType = 2;
      else if (!strcmp(var.value, "hybrid_bicubic"))
         g_Config.iTexScalingType = 3;
   }
   else
      g_Config.iTexScalingType = 0;

   var.key = "ppsspp_texture_scaling_level";
   var.value = NULL;

   if (environ_cb(RETRO_ENVIRONMENT_GET_VARIABLE, &var) && var.value)
   {
      int new_val = atoi(var.value);
      g_Config.iTexScalingLevel = new_val;
   }
   else
      g_Config.iTexScalingLevel = 1;

   var.key = "ppsspp_texture_anisotropic_filtering";
   var.value = NULL;

   if (environ_cb(RETRO_ENVIRONMENT_GET_VARIABLE, &var) && var.value)
   {
#ifdef USING_GLES2
      // TODO/FIXME - several GLES2-only devices actually have 
      // GL_EXT_texture_filter_anisotropic extension, so check 
      // if extension is available at runtime
      g_Config.iAnisotropyLevel = 0;
#else
      if (!strcmp(var.value, "off"))
         g_Config.iAnisotropyLevel = 0;
      else if (!strcmp(var.value, "1x"))
         g_Config.iAnisotropyLevel = 1;
      else if (!strcmp(var.value, "2x"))
         g_Config.iAnisotropyLevel = 2;
      else if (!strcmp(var.value, "3x"))
         g_Config.iAnisotropyLevel = 3;
      else if (!strcmp(var.value, "4x"))
         g_Config.iAnisotropyLevel = 4;
      else if (!strcmp(var.value, "5x"))
         g_Config.iAnisotropyLevel = 5;
#endif
   }
   else
      g_Config.iAnisotropyLevel = 0;

   var.key = "ppsspp_texture_deposterize";
   var.value = NULL;

   if (environ_cb(RETRO_ENVIRONMENT_GET_VARIABLE, &var) && var.value)
   {
      if (!strcmp(var.value, "enabled"))
         g_Config.bTexDeposterize = true;
      else if (!strcmp(var.value, "disabled"))
         g_Config.bTexDeposterize = false;
   }
   else
      g_Config.bTexDeposterize = false;


   var.key = "ppsspp_separate_cpu_thread";
   var.value = NULL;

   if (environ_cb(RETRO_ENVIRONMENT_GET_VARIABLE, &var) && var.value)
   {
      if (!strcmp(var.value, "enabled"))
         g_Config.bSeparateCPUThread = true;
      else if (!strcmp(var.value, "disabled"))
         g_Config.bSeparateCPUThread = false;
   }
   else
      g_Config.bSeparateCPUThread = false;

   var.key = "ppsspp_separate_io_thread";
   var.value = NULL;

   if (environ_cb(RETRO_ENVIRONMENT_GET_VARIABLE, &var) && var.value)
   {
      if (!strcmp(var.value, "enabled"))
         g_Config.bSeparateIOThread = true;
      else if (!strcmp(var.value, "disabled"))
         g_Config.bSeparateIOThread = false;
   }
   else
      g_Config.bSeparateIOThread = false;


   var.key = "ppsspp_unsafe_func_replacements";
   var.value = NULL;

   if (environ_cb(RETRO_ENVIRONMENT_GET_VARIABLE, &var) && var.value)
   {
      if (!strcmp(var.value, "enabled"))
         g_Config.bUnsafeFuncReplacements = true;
      else if (!strcmp(var.value, "disabled"))
         g_Config.bUnsafeFuncReplacements = false;
   }
   else
         g_Config.bUnsafeFuncReplacements = true;
   
   var.key = "ppsspp_sound_speedhack";
   var.value = NULL;

   if (environ_cb(RETRO_ENVIRONMENT_GET_VARIABLE, &var) && var.value)
   {
      if (!strcmp(var.value, "enabled"))
         g_Config.bSoundSpeedHack = true;
      else if (!strcmp(var.value, "disabled"))
         g_Config.bSoundSpeedHack = false;
   }
   else
      g_Config.bSoundSpeedHack = false;


   var.key = "ppsspp_cpu_core";
   var.value = NULL;

   if (environ_cb(RETRO_ENVIRONMENT_GET_VARIABLE, &var) && var.value)
   {
      if (!strcmp(var.value, "jit"))
         coreParam.cpuCore = CPU_JIT;
      else if (!strcmp(var.value, "interpreter"))
         coreParam.cpuCore = CPU_INTERPRETER;
   }
   else
      coreParam.cpuCore = CPU_JIT;

   var.key = "ppsspp_locked_cpu_speed";
   var.value = NULL;

   if (environ_cb(RETRO_ENVIRONMENT_GET_VARIABLE, &var) && var.value)
   {
      if (!strcmp(var.value, "off"))
         g_Config.iLockedCPUSpeed = 0;
      else if (!strcmp(var.value, "222MHz"))
         g_Config.iLockedCPUSpeed = 222;
      else if (!strcmp(var.value, "266MHz"))
         g_Config.iLockedCPUSpeed = 266;
      else if (!strcmp(var.value, "333MHz"))
         g_Config.iLockedCPUSpeed = 333;
   }
   else
      g_Config.iLockedCPUSpeed = 0;

   var.key = "ppsspp_rendering_mode";
   var.value = NULL;

   if (environ_cb(RETRO_ENVIRONMENT_GET_VARIABLE, &var) && var.value)
   {
      if (!strcmp(var.value, "nonbuffered"))
         g_Config.iRenderingMode = 0;
      else if (!strcmp(var.value, "buffered"))
         g_Config.iRenderingMode = 1;
      else if (!strcmp(var.value, "read_framebuffers_to_memory_cpu"))
         g_Config.iRenderingMode = 2;
      else if (!strcmp(var.value, "read_framebuffers_to_memory_gpu"))
         g_Config.iRenderingMode = 3;
   }
   else
      g_Config.iRenderingMode = 1;

}


bool retro_load_game(const struct retro_game_info *game)
{
   const char *tmp = NULL;
   enum retro_pixel_format fmt = RETRO_PIXEL_FORMAT_XRGB8888;

   if (!retro_base_dir_found)
   {
      char _dir[PATH_MAX];
      extract_directory(_dir, game->path, sizeof(_dir));
      retro_base_dir = std::string(_dir);
   }

   check_variables();

#ifdef _WIN32
   retro_base_dir  += "\\";
#else
   retro_base_dir += "/";
#endif
   retro_base_dir += "PPSSPP";
#ifdef _WIN32
   retro_base_dir += "\\";
#else
   retro_base_dir += "/";
#endif

   if (!environ_cb(RETRO_ENVIRONMENT_SET_PIXEL_FORMAT, &fmt))
   {
      if (log_cb)
         log_cb(RETRO_LOG_ERROR, "XRGB8888 is not supported.\n");
      return false;
   }

#ifdef GLES
   hw_render.context_type = RETRO_HW_CONTEXT_OPENGLES2;
#else
   hw_render.context_type = RETRO_HW_CONTEXT_OPENGL;
#endif
   hw_render.context_reset = context_reset;
   hw_render.bottom_left_origin = true;
   hw_render.depth = true;
   if (!environ_cb(RETRO_ENVIRONMENT_SET_HW_RENDER, &hw_render))
      return false;

   libretro_get_proc_address = hw_render.get_proc_address;

   VFSRegister("", new DirectoryAssetReader(retro_base_dir.c_str()));

   host = new LibretroHost;

   g_Config.Load("");

   g_Config.currentDirectory      = retro_base_dir;
   g_Config.externalDirectory     = retro_base_dir;
   g_Config.memCardDirectory      = retro_base_dir;
   g_Config.flash0Directory       = retro_base_dir + "flash0/";
   g_Config.internalDataDirectory = retro_base_dir;
   g_Config.iShowFPSCounter = false;
   g_Config.bVertexDecoderJit = (coreParam.cpuCore == CPU_JIT) ? true : false;
   g_Config.bFrameSkipUnthrottle = false;
   g_Config.bVSync = false;

   if (environ_cb(RETRO_ENVIRONMENT_GET_USERNAME, &tmp) && tmp)
      g_Config.sNickName = std::string(tmp);

   coreParam.gpuCore = GPU_GLES;
   coreParam.enableSound = true;
   coreParam.fileToStart = std::string(game->path);
   coreParam.mountIso = "";
   coreParam.startPaused = false;
   coreParam.printfEmuLog = false;
   coreParam.headLess = true;
   coreParam.unthrottle = true;

   _initialized = false;
   return true;
}

void retro_unload_game(void)
{
    PSP_Shutdown();
}

static bool should_reset = false;

void retro_reset(void)
{
   should_reset = true;
}

const int buttonMap[] = 
{ 
   CTRL_UP,
   CTRL_DOWN,
   CTRL_LEFT,
   CTRL_RIGHT,
   CTRL_TRIANGLE,
   CTRL_CIRCLE,
   CTRL_CROSS,
   CTRL_SQUARE,
   CTRL_LTRIGGER,
   CTRL_RTRIGGER,
   CTRL_START,
   CTRL_SELECT
};

static void retro_input(void)
{
   int i;
   float analogX, analogY;

   static unsigned map[] = {
      RETRO_DEVICE_ID_JOYPAD_UP,
      RETRO_DEVICE_ID_JOYPAD_DOWN,
      RETRO_DEVICE_ID_JOYPAD_LEFT,
      RETRO_DEVICE_ID_JOYPAD_RIGHT,
      RETRO_DEVICE_ID_JOYPAD_X,
      RETRO_DEVICE_ID_JOYPAD_A,
      RETRO_DEVICE_ID_JOYPAD_B,
      RETRO_DEVICE_ID_JOYPAD_Y,
      RETRO_DEVICE_ID_JOYPAD_L,
      RETRO_DEVICE_ID_JOYPAD_R,
      RETRO_DEVICE_ID_JOYPAD_START,
      RETRO_DEVICE_ID_JOYPAD_SELECT,
   };

   for (i = 0; i < 12; i++)
   {
      if (input_state_cb(0, RETRO_DEVICE_JOYPAD, 0, map[i]))
         __CtrlButtonDown(buttonMap[i]);
      else
         __CtrlButtonUp  (buttonMap[i]);
   }

   analogX = (float) input_state_cb(0, RETRO_DEVICE_ANALOG, RETRO_DEVICE_INDEX_ANALOG_LEFT, RETRO_DEVICE_ID_ANALOG_X) / 32768.0f;
   analogY = (float) input_state_cb(0, RETRO_DEVICE_ANALOG, RETRO_DEVICE_INDEX_ANALOG_LEFT, RETRO_DEVICE_ID_ANALOG_Y) / -32768.0f;
   __CtrlSetAnalogX(analogX);
   __CtrlSetAnalogY(analogY);
}

void retro_run(void)
{
   bool updated = false;
   if (environ_cb(RETRO_ENVIRONMENT_GET_VARIABLE_UPDATE, &updated) && updated)
      check_variables();

   libretro_framebuffer = (GLuint) hw_render.get_current_framebuffer();
   input_poll_cb();

   retro_input();

   if (should_reset)
      PSP_Shutdown();

   
   if (!_initialized || should_reset)
   {
      static bool gl_initialized = false;
      should_reset = false;

      if (!gl_initialized)
      {
         if (glewInit() != GLEW_OK)
         {
            if (log_cb)
               log_cb(RETRO_LOG_ERROR, "glewInit() failed.\n");
            environ_cb(RETRO_ENVIRONMENT_SHUTDOWN, nullptr);
            return;
         }

         glstate.Initialize();
         CheckGLExtensions();
         gl_initialized = true;
      }

      std::string error_string;
      if(!PSP_Init(coreParam, &error_string))
      {
         if (log_cb)
            log_cb(RETRO_LOG_ERROR, "PSP_Init() failed: %s.\n", error_string.c_str());
         environ_cb(RETRO_ENVIRONMENT_SHUTDOWN, nullptr);
      }

      host->BootDone();
      _initialized = true;
   }

#if 0
   if (log_cb)
      //log_cb(RETRO_LOG_INFO, "Locked CPU Speed: %d\n", g_Config.iLockedCPUSpeed);
      //log_cb(RETRO_LOG_INFO, "Audio Latency: %d\n", g_Config.IaudioLatency);
      //log_cb(RETRO_LOG_INFO, "Rendering Mode: %d\n", g_Config.iRenderingMode);
      log_cb(RETRO_LOG_INFO, "Function replacements: %d\n", g_Config.bFuncReplacements);
#endif

   NativeRender();


   update_sound();
   video_cb(RETRO_HW_FRAME_BUFFER_VALID, screen_width, screen_height, 0);
}

unsigned retro_get_region(void)
{
   return RETRO_REGION_NTSC;
}

bool retro_load_game_special(unsigned type, const struct retro_game_info *info, size_t num)
{
   (void)type;
   (void)info;
   (void)num;
   return false;
}

size_t retro_serialize_size(void)
{
   return 0;
}

bool retro_serialize(void *data, size_t size)
{
   (void)data;
   (void)size;
   return false;
}

bool retro_unserialize(const void *data, size_t size)
{
   (void)data;
   (void)size;
   return false;
}

void *retro_get_memory_data(unsigned id)
{
   (void)id;
   return NULL;
}

size_t retro_get_memory_size(unsigned id)
{
   (void)id;
   return 0;
}

void retro_cheat_reset(void)
{}

void retro_cheat_set(unsigned index, bool enabled, const char *code)
{
   (void)index;
   (void)enabled;
   (void)code;
}

void System_SendMessage(const char *command, const char *parameter) {}