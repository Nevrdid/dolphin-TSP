
// Copyright 2018 Dolphin Emulator Project
// Licensed under GPLv2+
// Refer to the license.txt file included.

#include <cstdio>
#include <thread>
#include "Core/Core.h"
#include "Core/System.h"

#include "DolphinNoGUI/Platform.h"
#include "Core/Config/MainSettings.h"
#include "Core/ConfigManager.h"

#include <SDL.h>
#include <SDL_vulkan.h>
#include <SDL_haptic.h>
#include <SDL_error.h>

#define VK_NO_PROTOTYPES

#define VK_USE_PLATFORM_SDL_EXT


namespace
{
class PlatformSDL : public Platform
{
public:
  ~PlatformSDL() override;

  void SetTitle(const std::string& title) override;
  void MainLoop() override;
  bool Init() override;

  WindowSystemInfo GetWindowSystemInfo() const override;
private:
  SDL_Window* m_window;
  int m_window_x = Config::Get(Config::MAIN_RENDER_WINDOW_XPOS);
  int m_window_y = Config::Get(Config::MAIN_RENDER_WINDOW_YPOS);
  unsigned int m_window_width = Config::Get(Config::MAIN_RENDER_WINDOW_WIDTH);
  unsigned int m_window_height = Config::Get(Config::MAIN_RENDER_WINDOW_HEIGHT);

};

bool PlatformSDL::Init(){

  SDL_SetHint(SDL_HINT_JOYSTICK_HIDAPI_PS4_RUMBLE, "1");
  SDL_SetHint(SDL_HINT_JOYSTICK_THREAD, "1");
  SDL_SetHint(SDL_HINT_GAMECONTROLLER_USE_BUTTON_LABELS, "0");
  SDL_SetHint(SDL_HINT_JOYSTICK_HIDAPI_PS5_PLAYER_LED, "0");
  // If Vulkan backend is set, create a Vulkan window
  if (Config::Get(Config::MAIN_GFX_BACKEND) == "Vulkan")
  {
    // Make sure SDL is initialized with Vulkan support.
    if (!SDL_Vulkan_LoadLibrary(nullptr))  // Load Vulkan dynamically if needed
    {
      SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "Failed to load Vulkan library: %s", SDL_GetError());
      return false;
    }

    if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_JOYSTICK | SDL_INIT_HAPTIC | SDL_INIT_GAMECONTROLLER) != 0)
    {
      SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "SDL failed to initialize: %s", SDL_GetError());
      return false;
    }

    // Create vulkan window
    m_window = SDL_CreateWindow("Vulkan Window",m_window_x, m_window_y, m_window_width, m_window_height, SDL_WINDOW_VULKAN);
    if (m_window == nullptr) {
      SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "Failed to create window: %s", SDL_GetError());
      return false;
    }
    return true;
  } else if (Config::Get(Config::MAIN_GFX_BACKEND) == "OGL")
  {
    if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_JOYSTICK | SDL_INIT_HAPTIC | SDL_INIT_GAMECONTROLLER) != 0)
    {
      SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "SDL failed to initialize: %s", SDL_GetError());
      return false;
    }

    // Create OGL window
    m_window = SDL_CreateWindow("OpenGL Window",m_window_x, m_window_y, m_window_width, m_window_height, SDL_WINDOW_OPENGL);
    if (m_window == nullptr) {
      SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "Failed to create window: %s", SDL_GetError());
      return false;
    }
    return true;
  }
  return false;
}

PlatformSDL::~PlatformSDL()
{
  if (m_window != nullptr)
    SDL_DestroyWindow(m_window);
  if (SDL_WasInit(SDL_INIT_VIDEO))
    SDL_Quit();
}

void PlatformSDL::SetTitle(const std::string& title)
{
  std::fprintf(stdout, "%s\n", title.c_str());
}

void PlatformSDL::MainLoop()
{
  while (m_running.IsSet())
  {
    UpdateRunningFlag();
    Core::HostDispatchJobs(Core::System::GetInstance());
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
  }
}

WindowSystemInfo PlatformSDL::GetWindowSystemInfo() const
{
  WindowSystemInfo wsi;
  wsi.type = WindowSystemType::SDL;
  wsi.display_connection = nullptr;
  wsi.render_window = reinterpret_cast<void*>(m_window);
  wsi.render_surface = reinterpret_cast<void*>(m_window);
  return wsi;
}

}  // namespace

std::unique_ptr<Platform> Platform::CreateSDLPlatform()
{
  return std::make_unique<PlatformSDL>();
}
