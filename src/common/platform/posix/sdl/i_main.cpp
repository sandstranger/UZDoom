/*
** i_main.cpp
**
** System-specific startup code. Eventually calls D_DoomMain.
**
**---------------------------------------------------------------------------
**
** Copyright 1998-2016 Marisa Heit
** Copyright 2007-2016 Christoph Oelckers
** Copyright 2017-2025 GZDoom Maintainers and Contributors
** Copyright 2025-2026 UZDoom Maintainers and Contributors
**
** SPDX-License-Identifier: GPL-3.0-or-later
**
**---------------------------------------------------------------------------
**
** Code written prior to 2026 is also licensed under:
**
** SPDX-License-Identifier: BSD-3-Clause
**
**---------------------------------------------------------------------------
**
*/

// HEADER FILES ------------------------------------------------------------

#include <SDL2/SDL.h>
#include <csignal>
#include <fcntl.h>
#include <locale.h>
#include <new>
#include <signal.h>
#include <sys/param.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/utsname.h>
#include <unistd.h>

#include "c_console.h"
#include "cmdlib.h"
#include "engineerrors.h"
#include "i_interface.h"
#include "i_system.h"
#include "m_argv.h"
#include "printf.h"
#include "version.h"
#include "zstring.h"

#include <string>
#ifdef ANDROID
#include "SwappyController.h"
#endif

using namespace std;

// MACROS ------------------------------------------------------------------

// TYPES -------------------------------------------------------------------

// EXTERNAL FUNCTION PROTOTYPES --------------------------------------------

extern "C" int cc_install_handlers(int, char**, int, int*, const char*, int(*)(char*, char*));

#if ANDROID
#include <atomic>
#include <string.h>

// The engine's copies of the launcher's folders, made at the start of SDL_main
string g_pathToUserFolder;
string gPathToCacheFolder;

// What the launcher gives through JNA, from its own thread: published atomically as copies that are never freed
// (the game may still be using the one a later call replaces)
static std::atomic<const char*> g_launcherUserFolder{nullptr};
static std::atomic<const char*> g_launcherCacheFolder{nullptr};
static std::atomic<const char*> g_launcherControllerDB{nullptr};

static void PublishLauncherString(std::atomic<const char*>& target, const char* value)
{
	if (value != nullptr)
		target.store(strdup(value), std::memory_order_release);
}

const char* I_AndroidControllerDBPath()
{
	return g_launcherControllerDB.load(std::memory_order_acquire);
}

namespace OpenGLRenderer
{
    extern void UnloadNGGL4ESPTR();
}

#endif

#ifdef __APPLE__
void Mac_I_FatalError(const char* errortext);
#endif

#if defined(__linux__) && !defined(ANDROID)
void Linux_I_FatalError(const char* errortext);

static void Linux_I_TryRestart(char **argv)
{
	// TODO: Check how Flatpak interacts with this, too

	const char *appimage = getenv("APPIMAGE");
	if (appimage)
	{
		int appimage_file = open(appimage, O_RDONLY);
		fexecve(appimage_file, argv, environ);
		return;
	}

	int self_file = open("/proc/self/exe", O_RDONLY);
	fexecve(self_file, argv, environ);
}
#endif

static void I_TryRestart(char **argv)
{
	// TODO: Mac support

#if defined(__linux__) && !defined(ANDROID)
	Linux_I_TryRestart(argv);
#endif
}

bool SDL_I_CheckForRestart(void);

// PUBLIC FUNCTION PROTOTYPES ----------------------------------------------
int GameMain();
void SignalHandler(int signal);

// PRIVATE FUNCTION PROTOTYPES ---------------------------------------------

// EXTERNAL DATA DECLARATIONS ----------------------------------------------

extern const char * const BACKEND = "SDL2";

// PUBLIC DATA DEFINITIONS -------------------------------------------------
FString sys_ostype;

// The command line arguments.
FArgs *Args;

// PRIVATE DATA DEFINITIONS ------------------------------------------------


// CODE --------------------------------------------------------------------



static int GetCrashInfo (char *buffer, char *end)
{
	if (sysCallbacks.CrashInfo) sysCallbacks.CrashInfo(buffer, end - buffer, "\n");
	return strlen(buffer);
}

FString I_DetectOS()
{
	FString operatingSystem;

	const char *paths[] = {"/etc/os-release", "/usr/lib/os-release"};

	for (const char *path : paths)
	{
		struct stat dummy;

		if (stat(path, &dummy) != 0)
			continue;

		char cmdline[256];
		snprintf(cmdline, sizeof cmdline, ". %s && echo ${PRETTY_NAME}", path);

		FILE *proc = popen(cmdline, "r");

		if (proc == nullptr)
			continue;

		char distribution[256] = {};
		fread(distribution, sizeof distribution - 1, 1, proc);

		const size_t length = strlen(distribution);

		if (length > 1)
		{
			distribution[length - 1] = '\0';
			operatingSystem = distribution;
		}

		pclose(proc);
		break;
	}

	struct utsname unameInfo;

	if (uname(&unameInfo) == 0)
	{
		const char* const separator = operatingSystem.Len() > 0 ? ", " : "";
		operatingSystem.AppendFormat("%s%s %s on %s", separator, unameInfo.sysname, unameInfo.release, unameInfo.machine);
		sys_ostype.Format("%s %s on %s", unameInfo.sysname, unameInfo.release, unameInfo.machine);
	}

	if (operatingSystem.Len() == 0)
		operatingSystem = "Unknown";

	return operatingSystem;
}

void I_StartupJoysticks();

#define SDL_SETENV(k, v)                                           \
	do {                                                           \
		auto old = SDL_getenv(k);                                  \
		if (old) DEBUG_LOG("%s already set as '%s'", k, old);      \
		if (SDL_setenv(k, v, 0)) DEBUG_LOG("Failed to set %s", k); \
	} while (0);

#ifdef ANDROID
#include "hw_programcache.h"
#include "etc2cache.h"
static int SDLCALL AndroidLifeCycleEventFilter(void*, SDL_Event* event);

int SDL_main(int argc, char **argv)
#else
int main (int argc, char **argv)
#endif
{
#if !defined (__APPLE__) && !ANDROID
	{
		int s[4] = { SIGSEGV, SIGILL, SIGFPE, SIGBUS };
		cc_install_handlers(argc, argv, 4, s, GAMENAMELOWERCASE "-crash.log", GetCrashInfo);
	}
#endif // !__APPLE__

#ifdef ANDROID
	// The launcher gives its folders (setPathsToFolders) before it starts the game: wait a little if they are late
	for (int i = 0; i < 500 && g_launcherUserFolder.load(std::memory_order_acquire) == nullptr; i++)
		usleep(10000);
	if (const char* userFolder = g_launcherUserFolder.load(std::memory_order_acquire))
		g_pathToUserFolder = userFolder;
	const char* cacheFolder = g_launcherCacheFolder.load(std::memory_order_acquire);
	gPathToCacheFolder = cacheFolder != nullptr ? cacheFolder : g_pathToUserFolder;
	if (g_pathToUserFolder.empty() || chdir(g_pathToUserFolder.c_str()) != 0)
		SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "No game folder from the launcher (setPathsToFolders)");
#endif

	signal(SIGINT, SignalHandler);
	signal(SIGTERM, SignalHandler);
	// signal(SIGHUP, SignalHandler);
	// signal(SIGQUIT, SignalHandler);

	seteuid (getuid ());
	// Set LC_NUMERIC environment variable in case some library decides to
	// clear the setlocale call at least this will be correct.
	// Note that the LANG environment variable is overridden by LC_*
	setenv ("LC_NUMERIC", "C", 1);

	setlocale (LC_ALL, "C");

	// despite the name, this also sets the wayland app_id
	SDL_SETENV("SDL_VIDEO_X11_WMCLASS", APPID);
	if (SDL_Init (0) < 0)
	{
		fprintf (stderr, "Could not initialize SDL:\n%s\n", SDL_GetError());
		return -1;
	}

#ifdef ANDROID
	SDL_AddEventWatch(AndroidLifeCycleEventFilter, nullptr);
#endif

	Args = new FArgs(argc, argv);

#ifdef PROGDIR
	progdir = PROGDIR;
#else
	char program[PATH_MAX];
	if (realpath (argv[0], program) == NULL)
		strcpy (program, argv[0]);
	char *slash = strrchr (program, '/');
	if (slash != NULL)
	{
		*(slash + 1) = '\0';
		progdir = program;
	}
	else
	{
		progdir = "./";
	}
#endif

	I_StartupJoysticks();

	const int result = GameMain();

	if (SDL_I_CheckForRestart())
	{
		I_TryRestart(argv);
	}

#ifdef ANDROID
    OpenGLRenderer::UnloadNGGL4ESPTR();
	DestroySwappy();
#endif

	SDL_SetRelativeMouseMode(SDL_FALSE);
	SDL_Quit();

	return result;
}


#ifdef ANDROID
#include "menustate.h"
#include "i_soundinternal.h"
extern bool StartScreenRendered;
// (atomic: the launcher sets it through JNA from its own thread, the renderers read it when they build shaders)
std::atomic<bool> gl_lite_shader{false};
extern bool AppActive;

// menuactive as the launcher's thread sees it: the game loop publishes it once a frame (menuactive itself, also
// written by the scripts, is only for the game thread)
static std::atomic<int> g_launcherMenuActive{MENU_Off};

void I_PublishAndroidFrameState()
{
	g_launcherMenuActive.store(menuactive, std::memory_order_relaxed);
}

// JNA, called from the launcher's thread. JNA passes and reads a Java boolean as a 32 bit int: explicit ints here.
extern "C"{
__attribute__((used)) __attribute__((visibility("default")))
void UpdateGLLiteShaderState (int enableGLLiteShader){
    gl_lite_shader.store(enableGLLiteShader != 0, std::memory_order_relaxed);
}

__attribute__((used)) __attribute__((visibility("default")))
void onNativeResume() {
}
__attribute__((used)) __attribute__((visibility("default")))
void onNativePause() {
}
__attribute__((used)) __attribute__((visibility("default")))
int needToShowScreenControls() {
    return g_launcherMenuActive.load(std::memory_order_relaxed) == MENU_Off ? 1 : 0;
}
__attribute__((used)) __attribute__((visibility("default")))
int needToInvokeMouseButtonsEvents(){
    return g_launcherMenuActive.load(std::memory_order_relaxed) != MENU_Off ? 1 : 0;
}
__attribute__((used)) __attribute__((visibility("default")))
int needToReInitGameControllers (){
    return 0;
}
__attribute__((used)) __attribute__((visibility("default")))
void setPathsToFolders (const char *pathToUserFolder, const char* pathToCacheFolder) {
    // (null-safe; the engine copies them at the start of SDL_main)
    PublishLauncherString(g_launcherUserFolder, pathToUserFolder);
    PublishLauncherString(g_launcherCacheFolder, pathToCacheFolder);
}

__attribute__((used)) __attribute__((visibility("default")))
void setPathToSDLControllerDB (const char *pathToSDLControllerDB){
    PublishLauncherString(g_launcherControllerDB, pathToSDLControllerDB);
}
__attribute__((used)) __attribute__((visibility("default")))
void setUseGLES2_0State(const int useGLES2_0) {
}
}

static int SDLCALL AndroidLifeCycleEventFilter(void*, SDL_Event* event){
	switch (event->type)
	{
		case SDL_APP_WILLENTERBACKGROUND:
			S_SetSoundPaused(0);
			AppActive = false;
			break;
		case SDL_APP_DIDENTERFOREGROUND:
			S_SetSoundPaused(1);
			AppActive = true;
			break;
		case SDL_APP_LOWMEMORY:
			// may arrive on another thread: only flag it, the renderer trims at the end of a frame
			ProgramBinaryCache::RequestTrim();
			ETC2Cache::TryClearRam();	// (the transcoded textures' RAM copies; skipped if a loader is using them)
			break;
	}
	return 1;
}
#endif
