// SSNE-Native launcher: asks for the user's GameCube disc, verifies it, installs the game data and
// launches the PC port.
#include <SDL.h>
#include <SDL_opengl.h>
#include <imgui.h>
#include <imgui_impl_opengl3.h>
#include <imgui_impl_sdl2.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

#include "launcher_app.h"
#include "theme.h"

// Development: SSNE_SCREENSHOT=file.ppm saves the window after ~1 s and quits.
static void save_screenshot(SDL_Window* win, const char* path) {
    int w, h;
    SDL_GL_GetDrawableSize(win, &w, &h);
    std::vector<unsigned char> px(size_t(w) * h * 3);
    glPixelStorei(GL_PACK_ALIGNMENT, 1);
    glReadPixels(0, 0, w, h, GL_RGB, GL_UNSIGNED_BYTE, px.data());
    if (FILE* f = fopen(path, "wb")) {
        fprintf(f, "P6\n%d %d\n255\n", w, h);
        for (int y = h - 1; y >= 0; y--) fwrite(&px[size_t(y) * w * 3], 1, size_t(w) * 3, f);
        fclose(f);
    }
}

int main(int argc, char** argv) {
    bool updated = false;  // started by the self-update
    for (int i = 1; i < argc; i++) updated |= strcmp(argv[i], "--updated") == 0;
    if (SDL_Init(SDL_INIT_VIDEO) != 0) {
        fprintf(stderr, "SDL_Init failed: %s\n", SDL_GetError());
        return 1;
    }
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 3);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 3);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_CORE);
    SDL_Window* win = SDL_CreateWindow("Serious Sam: Next Encounter Native", SDL_WINDOWPOS_CENTERED,
                                       SDL_WINDOWPOS_CENTERED, 1024, 640,
                                       SDL_WINDOW_OPENGL | SDL_WINDOW_RESIZABLE | SDL_WINDOW_ALLOW_HIGHDPI);
    if (!win) { fprintf(stderr, "SDL_CreateWindow failed: %s\n", SDL_GetError()); return 1; }
    SDL_SetWindowMinimumSize(win, 860, 560);
    SDL_GLContext gl = SDL_GL_CreateContext(win);
    bool core = gl != nullptr;
    if (!gl) {  // older drivers: any 3.0 context
        SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, 0);
        SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 0);
        gl = SDL_GL_CreateContext(win);
    }
    if (!gl) { fprintf(stderr, "SDL_GL_CreateContext failed: %s\n", SDL_GetError()); return 1; }
    SDL_GL_SetSwapInterval(1);
    SDL_EventState(SDL_DROPFILE, SDL_ENABLE);

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGui::GetIO().IniFilename = nullptr;
    theme::load_fonts(1.0f);
    theme::apply_style();
    const char* shot = getenv("SSNE_SCREENSHOT");
    int frames = 0;
    ImGui_ImplSDL2_InitForOpenGL(win, gl);
    ImGui_ImplOpenGL3_Init(core ? "#version 330 core" : "#version 130");

    {
        LauncherApp app(updated);
        bool quit = false;
        while (!quit && !app.wants_quit()) {
            SDL_Event e;
            while (SDL_PollEvent(&e)) {
                ImGui_ImplSDL2_ProcessEvent(&e);
                if (e.type == SDL_QUIT) quit = true;
                if (e.type == SDL_DROPFILE) {
                    app.on_file_dropped(e.drop.file);
                    SDL_free(e.drop.file);
                }
            }
            ImGui_ImplOpenGL3_NewFrame();
            ImGui_ImplSDL2_NewFrame();
            ImGui::NewFrame();
            app.frame();
            ImGui::Render();
            int w, h;
            SDL_GL_GetDrawableSize(win, &w, &h);
            glViewport(0, 0, w, h);
            glClearColor(0.01f, 0.02f, 0.10f, 1);
            glClear(GL_COLOR_BUFFER_BIT);
            ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());
            if (shot && ++frames == 60) { save_screenshot(win, shot); quit = true; }
            SDL_GL_SwapWindow(win);
        }
    }
    ImGui_ImplOpenGL3_Shutdown();
    ImGui_ImplSDL2_Shutdown();
    ImGui::DestroyContext();
    SDL_GL_DeleteContext(gl);
    SDL_DestroyWindow(win);
    SDL_Quit();
    return 0;
}
