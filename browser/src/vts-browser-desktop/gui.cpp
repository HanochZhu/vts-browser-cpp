/**
 * Copyright (c) 2017 Melown Technologies SE
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are met:
 *
 * *  Redistributions of source code must retain the above copyright notice,
 *    this list of conditions and the following disclaimer.
 *
 * *  Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
 * AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED.  IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS BE
 * LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
 * CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
 * SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
 * INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
 * CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
 * ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
 * POSSIBILITY OF SUCH DAMAGE.
 */

#include <cstring>
#include <set>

#include <vts-browser/map.hpp>
#include <vts-browser/statistics.hpp>
#include <vts-browser/options.hpp>
#include <vts-browser/view.hpp>
#include <vts-browser/search.hpp>
#include <vts-browser/log.hpp>

#include <vts-renderer/classes.hpp>

#include <SDL2/SDL.h>

#include "mainWindow.hpp"
#include "guiSkin.hpp"

using namespace vts;
using namespace renderer;

namespace
{

const nk_rune FontUnicodeRanges[] = {
    // 0x0020, 0x007F, // Basic Latin
    // 0x00A0, 0x00FF, // Latin-1 Supplement
    // 0x0100, 0x017F, // Latin Extended-A
    // 0x0180, 0x024F, // Latin Extended-B
    // 0x0300, 0x036F, // Combining Diacritical Marks
    // 0x0400, 0x04FF, // Cyrillic
    0x0001, 0x5000, // all multilingual characters
    0
};

void clipBoardPaste(nk_handle, struct nk_text_edit *edit)
{
    const char *text = SDL_GetClipboardText();
    if (text)
        nk_textedit_paste(edit, text, strlen(text));
}

void clipBoardCopy(nk_handle, const char *text, int len)
{
    assert(len < 300);
    char buffer[301];
    memcpy(buffer, text, len);
    buffer[len] = 0;
    SDL_SetClipboardText(buffer);
}

static const char *traverseModeNames[] = {
    "Hierarchical",
    "Flat",
    "Balanced",
};

static const char *navigationTypeNames[] = {
    "Instant",
    "Quick",
    "FlyOver",
};

static const char *navigationModeNames[] = {
    "Azimuthal",
    "Free",
    "Dynamic",
    "Seamless",
};

} // namespace

Mark::Mark() : open(false)
{}

class GuiImpl
{
public:
    struct vertex
    {
        float position[2];
        float uv[2];
        nk_byte col[4];
    };

    GuiImpl(MainWindow *window) :
        posAutoMotion(0,0,0),
        viewExtentLimitScaleMin(0),
        viewExtentLimitScaleMax(std::numeric_limits<double>::infinity()),
        statTraversedDetails(false), statRenderedDetails(false),
        optSensitivityDetails(false), posAutoDetails(false),
        positionSrs(2), searchDetails(-1), window(window), prepareFirst(true)
    {
        gladLoadGLLoader(&SDL_GL_GetProcAddress);

        searchText[0] = 0;
        searchTextPrev[0] = 0;
        positionInputText[0] = 0;

        // load font
        {
            struct nk_font_config cfg = nk_font_config(0);
            cfg.oversample_h = 3;
            cfg.oversample_v = 2;
            cfg.range = FontUnicodeRanges;
            nk_font_atlas_init_default(&atlas);
            nk_font_atlas_begin(&atlas);
            Buffer buffer = readInternalMemoryBuffer(
                        "data/fonts/roboto-regular.ttf");
            font = nk_font_atlas_add_from_memory(&atlas,
                buffer.data(), buffer.size(), 14, &cfg);
            GpuTextureSpec spec;
            const void* img = nk_font_atlas_bake(&atlas,
                (int*)&spec.width, (int*)&spec.height, NK_FONT_ATLAS_RGBA32);
            spec.components = 4;
            spec.buffer.allocate(spec.width * spec.height * spec.components);
            memcpy(spec.buffer.data(), img, spec.buffer.size());
            fontTexture = std::make_shared<Texture>();
            ResourceInfo info;
            fontTexture->load(info, spec);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
            nk_font_atlas_end(&atlas, nk_handle_id(fontTexture->getId()),
                              &null);

            // lodepng_encode32_file("font_atlas.png",
            //     (unsigned char*)spec.buffer.data(), spec.width, spec.height);
        }

        nk_init_default(&ctx, &font->handle);
        nk_buffer_init_default(&cmds);
        
        ctx.clip.paste = &clipBoardPaste;
        ctx.clip.copy = &clipBoardCopy;
        ctx.clip.userdata.ptr = window->window;

        static const nk_draw_vertex_layout_element vertex_layout[] =
        {
            { NK_VERTEX_POSITION, NK_FORMAT_FLOAT, 0 },
            { NK_VERTEX_TEXCOORD, NK_FORMAT_FLOAT, 8 },
            { NK_VERTEX_COLOR, NK_FORMAT_R8G8B8A8, 16 },
            { NK_VERTEX_LAYOUT_END }
        };
        memset(&config, 0, sizeof(config));
        config.vertex_layout = vertex_layout;
        config.vertex_size = sizeof(vertex);
        config.vertex_alignment = alignof(vertex);
        config.circle_segment_count = 22;
        config.curve_segment_count = 22;
        config.arc_segment_count = 22;
        config.global_alpha = 1.0f;
        config.shape_AA = NK_ANTI_ALIASING_ON;
        config.line_AA = NK_ANTI_ALIASING_ON;
        config.null = null;

        initializeGuiSkin(ctx, skinMedia, skinTexture);

        // load shader
        {
            shader = std::make_shared<Shader>();
            Buffer vert = readInternalMemoryBuffer(
                        "data/shaders/gui.vert.glsl");
            Buffer frag = readInternalMemoryBuffer(
                        "data/shaders/gui.frag.glsl");
            shader->load(
                std::string(vert.data(), vert.size()),
                std::string(frag.data(), frag.size()));
            std::vector<uint32> &uls = shader->uniformLocations;
            GLuint id = shader->getId();
            uls.push_back(glGetUniformLocation(id, "ProjMtx"));
            glUseProgram(id);
            glUniform1i(glGetUniformLocation(id, "Texture"), 0);
        }

        // prepare mesh buffers
        {
            GLuint vao = 0, vbo = 0, vio = 0;
            glGenVertexArrays(1, &vao);
            glGenBuffers(1, &vbo);
            glGenBuffers(1, &vio);
            glBindVertexArray(vao);
            glBindBuffer(GL_ARRAY_BUFFER, vbo);
            glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, vio);
            glBufferData(GL_ARRAY_BUFFER, MaxVertexMemory,
                         NULL, GL_STREAM_DRAW);
            glBufferData(GL_ELEMENT_ARRAY_BUFFER, MaxElementMemory,
                         NULL, GL_STREAM_DRAW);
            glEnableVertexAttribArray(0);
            glEnableVertexAttribArray(1);
            glEnableVertexAttribArray(2);
            glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE,
                sizeof(vertex), (void*)0);
            glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, 
                sizeof(vertex), (void*)8);
            glVertexAttribPointer(2, 4, GL_UNSIGNED_BYTE, GL_TRUE,
                sizeof(vertex), (void*)16);
            mesh = std::make_shared<Mesh>();
            mesh->load(vao, vbo, vio);
        }
    }

    ~GuiImpl()
    {
        nk_buffer_free(&cmds);
        nk_font_atlas_clear(&atlas);
        nk_free(&ctx);
    }

    void dispatch(int width, int height)
    {
        glEnable(GL_BLEND);
        glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
        glDisable(GL_CULL_FACE);
        glDisable(GL_DEPTH_TEST);
        glEnable(GL_SCISSOR_TEST);
        glActiveTexture(GL_TEXTURE0);
        glBindVertexArray(mesh->getVao());
        glBindBuffer(GL_ARRAY_BUFFER, mesh->getVbo());
        glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, mesh->getVio());
        shader->bind();

        // proj matrix
        {
            GLfloat ortho[4][4] = {
                    {2.0f, 0.0f, 0.0f, 0.0f},
                    {0.0f,-2.0f, 0.0f, 0.0f},
                    {0.0f, 0.0f,-1.0f, 0.0f},
                    {-1.0f,1.0f, 0.0f, 1.0f},
            };
            ortho[0][0] /= (GLfloat)width;
            ortho[1][1] /= (GLfloat)height;
            glUniformMatrix4fv(shader->uniformLocations[0], 1,
                    GL_FALSE, &ortho[0][0]);
        }

        // upload buffer data
        {
            void *vertices = glMapBuffer(GL_ARRAY_BUFFER,
                                         GL_WRITE_ONLY);
            void *elements = glMapBuffer(GL_ELEMENT_ARRAY_BUFFER,
                                         GL_WRITE_ONLY);
            nk_buffer vbuf, ebuf;
            nk_buffer_init_fixed(&vbuf, vertices, MaxVertexMemory);
            nk_buffer_init_fixed(&ebuf, elements, MaxElementMemory);
            nk_convert(&ctx, &cmds, &vbuf, &ebuf, &config);
            glUnmapBuffer(GL_ARRAY_BUFFER);
            glUnmapBuffer(GL_ELEMENT_ARRAY_BUFFER);
        }

        // draw commands
        {
            struct nk_vec2 scale;
            scale.x = 1;
            scale.y = 1;
            const nk_draw_command *cmd;
            const nk_draw_index *offset = NULL;
            nk_draw_foreach(cmd, &ctx, &cmds)
            {
                if (!cmd->elem_count)
                    continue;
                glBindTexture(GL_TEXTURE_2D, cmd->texture.id);
                glScissor(
                    (GLint)(cmd->clip_rect.x * scale.x),
                    (GLint)((height - (GLint)(cmd->clip_rect.y
                                              + cmd->clip_rect.h)) * scale.y),
                    (GLint)(cmd->clip_rect.w * scale.x),
                    (GLint)(cmd->clip_rect.h * scale.y));
                glDrawElements(GL_TRIANGLES, (GLsizei)cmd->elem_count,
                               GL_UNSIGNED_SHORT, offset);
                offset += cmd->elem_count;
            }
        }

        nk_clear(&ctx);

        glUseProgram(0);
        glBindVertexArray(0);
        glBindBuffer(GL_ARRAY_BUFFER, 0);
        glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, 0);
        glDisable(GL_SCISSOR_TEST);
    }

    int handleEvent(SDL_Event *evt)
    {
        // some code copied from the demos

        if (evt->type == SDL_KEYUP || evt->type == SDL_KEYDOWN) {
            /* key events */
            int down = evt->type == SDL_KEYDOWN;
            const Uint8* state = SDL_GetKeyboardState(0);
            SDL_Keycode sym = evt->key.keysym.sym;
            if (sym == SDLK_RSHIFT || sym == SDLK_LSHIFT)
                nk_input_key(&ctx, NK_KEY_SHIFT, down);
            else if (sym == SDLK_DELETE)
                nk_input_key(&ctx, NK_KEY_DEL, down);
            else if (sym == SDLK_RETURN)
                nk_input_key(&ctx, NK_KEY_ENTER, down);
            else if (sym == SDLK_TAB)
                nk_input_key(&ctx, NK_KEY_TAB, down);
            else if (sym == SDLK_BACKSPACE)
                nk_input_key(&ctx, NK_KEY_BACKSPACE, down);
            else if (sym == SDLK_HOME) {
                nk_input_key(&ctx, NK_KEY_TEXT_START, down);
                nk_input_key(&ctx, NK_KEY_SCROLL_START, down);
            } else if (sym == SDLK_END) {
                nk_input_key(&ctx, NK_KEY_TEXT_END, down);
                nk_input_key(&ctx, NK_KEY_SCROLL_END, down);
            } else if (sym == SDLK_PAGEDOWN) {
                nk_input_key(&ctx, NK_KEY_SCROLL_DOWN, down);
            } else if (sym == SDLK_PAGEUP) {
                nk_input_key(&ctx, NK_KEY_SCROLL_UP, down);
            } else if (sym == SDLK_z)
                nk_input_key(&ctx, NK_KEY_TEXT_UNDO, down && state[SDL_SCANCODE_LCTRL]);
            else if (sym == SDLK_r)
                nk_input_key(&ctx, NK_KEY_TEXT_REDO, down && state[SDL_SCANCODE_LCTRL]);
            else if (sym == SDLK_c)
                nk_input_key(&ctx, NK_KEY_COPY, down && state[SDL_SCANCODE_LCTRL]);
            else if (sym == SDLK_v)
                nk_input_key(&ctx, NK_KEY_PASTE, down && state[SDL_SCANCODE_LCTRL]);
            else if (sym == SDLK_x)
                nk_input_key(&ctx, NK_KEY_CUT, down && state[SDL_SCANCODE_LCTRL]);
            else if (sym == SDLK_b)
                nk_input_key(&ctx, NK_KEY_TEXT_LINE_START, down && state[SDL_SCANCODE_LCTRL]);
            else if (sym == SDLK_e)
                nk_input_key(&ctx, NK_KEY_TEXT_LINE_END, down && state[SDL_SCANCODE_LCTRL]);
            else if (sym == SDLK_UP)
                nk_input_key(&ctx, NK_KEY_UP, down);
            else if (sym == SDLK_DOWN)
                nk_input_key(&ctx, NK_KEY_DOWN, down);
            else if (sym == SDLK_LEFT) {
                if (state[SDL_SCANCODE_LCTRL])
                    nk_input_key(&ctx, NK_KEY_TEXT_WORD_LEFT, down);
                else nk_input_key(&ctx, NK_KEY_LEFT, down);
            } else if (sym == SDLK_RIGHT) {
                if (state[SDL_SCANCODE_LCTRL])
                    nk_input_key(&ctx, NK_KEY_TEXT_WORD_RIGHT, down);
                else nk_input_key(&ctx, NK_KEY_RIGHT, down);
            } else return 0;
            return 1;
        } else if (evt->type == SDL_MOUSEBUTTONDOWN || evt->type == SDL_MOUSEBUTTONUP) {
            /* mouse button */
            int down = evt->type == SDL_MOUSEBUTTONDOWN;
            const int x = evt->button.x, y = evt->button.y;
            if (evt->button.button == SDL_BUTTON_LEFT) {
                if (evt->button.clicks > 1)
                    nk_input_button(&ctx, NK_BUTTON_DOUBLE, x, y, down);
                nk_input_button(&ctx, NK_BUTTON_LEFT, x, y, down);
            } else if (evt->button.button == SDL_BUTTON_MIDDLE)
                nk_input_button(&ctx, NK_BUTTON_MIDDLE, x, y, down);
            else if (evt->button.button == SDL_BUTTON_RIGHT)
                nk_input_button(&ctx, NK_BUTTON_RIGHT, x, y, down);
            return 1;
        } else if (evt->type == SDL_MOUSEMOTION) {
            /* mouse motion */
            if (ctx.input.mouse.grabbed) {
                int x = (int)ctx.input.mouse.prev.x, y = (int)ctx.input.mouse.prev.y;
                nk_input_motion(&ctx, x + evt->motion.xrel, y + evt->motion.yrel);
            } else nk_input_motion(&ctx, evt->motion.x, evt->motion.y);
            return 1;
        } else if (evt->type == SDL_TEXTINPUT) {
            /* text input */
            nk_glyph glyph;
            memcpy(glyph, evt->text.text, NK_UTF_SIZE);
            nk_input_glyph(&ctx, glyph);
            return 1;
        } else if (evt->type == SDL_MOUSEWHEEL) {
            /* mouse wheel */
            nk_input_scroll(&ctx,nk_vec2((float)evt->wheel.x,(float)evt->wheel.y));
            return 1;
        }
        return 0;
    }

    void prepareOptions()
    {
        int flags = NK_WINDOW_BORDER | NK_WINDOW_MOVABLE
                | NK_WINDOW_SCALABLE | NK_WINDOW_TITLE
                | NK_WINDOW_MINIMIZABLE;
        if (prepareFirst)
            flags |= NK_WINDOW_MINIMIZED;
        if (nk_begin(&ctx, "Options", nk_rect(10, 10, 250, 650), flags))
        {
            MapOptions &o = window->map->options();
            AppOptions &a = window->appOptions;
            float width = nk_window_get_content_region_size(&ctx).x - 15;
            float ratio[] = { width * 0.4f, width * 0.45f, width * 0.15f };
            nk_layout_row(&ctx, NK_STATIC, 16, 3, ratio);
            char buffer[256];

            // camera control sensitivity
            nk_label(&ctx, "Mouse sensitivity:", NK_TEXT_LEFT);
            nk_checkbox_label(&ctx, "", &optSensitivityDetails);
            nk_label(&ctx, "", NK_TEXT_LEFT);
            if (optSensitivityDetails)
            {
                nk_layout_row(&ctx, NK_STATIC, 16, 3, ratio);
                nk_label(&ctx, "Pan speed:", NK_TEXT_LEFT);
                o.cameraSensitivityPan = nk_slide_float(&ctx,
                        0.1, o.cameraSensitivityPan, 3, 0.01);
                sprintf(buffer, "%4.2f", o.cameraSensitivityPan);
                nk_label(&ctx, buffer, NK_TEXT_RIGHT);
                nk_label(&ctx, "Zoom speed:", NK_TEXT_LEFT);
                o.cameraSensitivityZoom = nk_slide_float(&ctx,
                        0.1, o.cameraSensitivityZoom, 3, 0.01);
                sprintf(buffer, "%4.2f", o.cameraSensitivityZoom);
                nk_label(&ctx, buffer, NK_TEXT_RIGHT);
                nk_label(&ctx, "Rotate speed:", NK_TEXT_LEFT);
                o.cameraSensitivityRotate = nk_slide_float(&ctx,
                        0.1, o.cameraSensitivityRotate, 3, 0.01);
                sprintf(buffer, "%4.2f", o.cameraSensitivityRotate);
                nk_label(&ctx, buffer, NK_TEXT_RIGHT);
                
                nk_label(&ctx, "Pan inertia:", NK_TEXT_LEFT);
                o.cameraInertiaPan = nk_slide_float(&ctx,
                        0, o.cameraInertiaPan, 0.99, 0.01);
                sprintf(buffer, "%4.2f", o.cameraInertiaPan);
                nk_label(&ctx, buffer, NK_TEXT_RIGHT);
                nk_label(&ctx, "Zoom inertia:", NK_TEXT_LEFT);
                o.cameraInertiaZoom = nk_slide_float(&ctx,
                        0, o.cameraInertiaZoom, 0.99, 0.01);
                sprintf(buffer, "%4.2f", o.cameraInertiaZoom);
                nk_label(&ctx, buffer, NK_TEXT_RIGHT);
                nk_label(&ctx, "Rotate inertia:", NK_TEXT_LEFT);
                o.cameraInertiaRotate = nk_slide_float(&ctx,
                        0, o.cameraInertiaRotate, 0.99, 0.01);
                sprintf(buffer, "%4.2f", o.cameraInertiaRotate);
                nk_label(&ctx, buffer, NK_TEXT_RIGHT);
                
                nk_label(&ctx, "", NK_TEXT_LEFT);
                if (nk_button_label(&ctx, "Reset sensitivity"))
                {
                    MapOptions d;
                    o.cameraSensitivityPan       = d.cameraSensitivityPan;
                    o.cameraSensitivityZoom      = d.cameraSensitivityZoom;
                    o.cameraSensitivityRotate    = d.cameraSensitivityRotate;
                    o.cameraInertiaPan           = d.cameraInertiaPan;
                    o.cameraInertiaZoom          = d.cameraInertiaZoom;
                    o.cameraInertiaRotate        = d.cameraInertiaRotate;
                }
                nk_label(&ctx, "", NK_TEXT_LEFT);
            }

            // traverse mode
            {
                nk_label(&ctx, "Traverse:", NK_TEXT_LEFT);
                if (nk_combo_begin_label(&ctx,
                                 traverseModeNames[(int)o.traverseMode],
                                 nk_vec2(nk_widget_width(&ctx), 200)))
                {
                    nk_layout_row_dynamic(&ctx, 16, 1);
                    for (unsigned i = 0; i < sizeof(traverseModeNames)
                         / sizeof(traverseModeNames[0]); i++)
                    {
                        if (nk_combo_item_label(&ctx, traverseModeNames[i],
                                                NK_TEXT_LEFT))
                            o.traverseMode = (TraverseMode)i;
                    }
                    nk_combo_end(&ctx);
                }
                nk_label(&ctx, "", NK_TEXT_LEFT);
            }

            // navigation
            {
                // navigation type
                {
                    nk_label(&ctx, "Nav. type:", NK_TEXT_LEFT);
                    if (nk_combo_begin_label(&ctx,
                                 navigationTypeNames[(int)o.navigationType],
                                 nk_vec2(nk_widget_width(&ctx), 200)))
                    {
                        nk_layout_row_dynamic(&ctx, 16, 1);
                        for (unsigned i = 0; i < sizeof(navigationTypeNames)
                             / sizeof(navigationTypeNames[0]); i++)
                        {
                            if (nk_combo_item_label(&ctx,
                                    navigationTypeNames[i], NK_TEXT_LEFT))
                                o.navigationType = (NavigationType)i;
                        }
                        nk_combo_end(&ctx);
                    }
                    nk_label(&ctx, "", NK_TEXT_LEFT);
                }

                // navigation mode
                {
                    nk_label(&ctx, "Nav. mode:", NK_TEXT_LEFT);
                    if (nk_combo_begin_label(&ctx,
                                 navigationModeNames[(int)o.navigationMode],
                                 nk_vec2(nk_widget_width(&ctx), 200)))
                    {
                        nk_layout_row_dynamic(&ctx, 16, 1);
                        for (unsigned i = 0; i < sizeof(navigationModeNames)
                             / sizeof(navigationModeNames[0]); i++)
                        {
                            if (nk_combo_item_label(&ctx,
                                        navigationModeNames[i],NK_TEXT_LEFT))
                                o.navigationMode = (NavigationMode)i;
                        }
                        nk_combo_end(&ctx);
                    }
                    nk_label(&ctx, "", NK_TEXT_LEFT);
                }

                // navigation max view extent multiplier
                nk_label(&ctx, "Piha zoom:", NK_TEXT_LEFT);
                o.navigationMaxViewExtentMult = nk_slide_float(&ctx,
                        1.002, o.navigationMaxViewExtentMult, 1.2, 0.002);
                sprintf(buffer, "%5.3f", o.navigationMaxViewExtentMult);
                nk_label(&ctx, buffer, NK_TEXT_RIGHT);

                // navigation max position change
                nk_label(&ctx, "Piha move:", NK_TEXT_LEFT);
                o.navigationMaxPositionChange = nk_slide_float(&ctx,
                        0.002, o.navigationMaxPositionChange, 0.2, 0.002);
                sprintf(buffer, "%5.3f", o.navigationMaxPositionChange);
                nk_label(&ctx, buffer, NK_TEXT_RIGHT);

                // navigation samples per view extent
                nk_label(&ctx, "Nav. samples:", NK_TEXT_LEFT);
                o.navigationSamplesPerViewExtent = nk_slide_int(&ctx,
                        1, o.navigationSamplesPerViewExtent, 16, 1);
                sprintf(buffer, "%3d", o.navigationSamplesPerViewExtent);
                nk_label(&ctx, buffer, NK_TEXT_RIGHT);
            }

            // details
            {
                // maxTexelToPixelScale
                nk_label(&ctx, "Texel to pixel:", NK_TEXT_LEFT);
                double prev = o.maxTexelToPixelScale;
                o.maxTexelToPixelScale = nk_slide_float(&ctx,
                        1, o.maxTexelToPixelScale, 5, 0.01);
                sprintf(buffer, "%3.1f", o.maxTexelToPixelScale);
                nk_label(&ctx, buffer, NK_TEXT_RIGHT);
                o.maxBalancedCoarsenessScale += o.maxTexelToPixelScale - prev;

                // maxBalancedCoarsenessScale
                nk_label(&ctx, "Balanced ratio:", NK_TEXT_LEFT);
                o.maxBalancedCoarsenessScale = nk_slide_float(&ctx,
                        o.maxTexelToPixelScale, o.maxBalancedCoarsenessScale,
                        o.maxTexelToPixelScale + 5, 0.01);
                sprintf(buffer, "%3.1f", o.maxBalancedCoarsenessScale);
                nk_label(&ctx, buffer, NK_TEXT_RIGHT);
            }

            // antialiasing samples
            {
                nk_label(&ctx, "Antialiasing:", NK_TEXT_LEFT);
                a.render.antialiasingSamples = nk_slide_int(&ctx,
                        1, a.render.antialiasingSamples, 16, 1);
                if (a.render.antialiasingSamples > 1)
                {
                    sprintf(buffer, "%d", a.render.antialiasingSamples);
                    nk_label(&ctx, buffer, NK_TEXT_RIGHT);
                }
                else
                    nk_label(&ctx, "no", NK_TEXT_RIGHT);
            }

            // maxResourcesMemory
            nk_label(&ctx, "Max memory:", NK_TEXT_LEFT);
            o.maxResourcesMemory = 1024 * 1024 * (uint64)nk_slide_int(&ctx,
                    0, o.maxResourcesMemory / 1024 / 1024, 2048, 32);
            sprintf(buffer, "%3d", (int)(o.maxResourcesMemory / 1024 / 1024));
            nk_label(&ctx, buffer, NK_TEXT_RIGHT);

            // display
            {
                // render atmosphere
                nk_label(&ctx, "Display:", NK_TEXT_LEFT);
                a.render.renderAtmosphere = nk_check_label(&ctx, "atmosphere",
                                                   a.render.renderAtmosphere);
                nk_label(&ctx, "", NK_TEXT_LEFT);

                // render mesh wire boxes
                nk_label(&ctx, "", NK_TEXT_LEFT);
                o.debugRenderMeshBoxes = nk_check_label(&ctx, "mesh boxes",
                                                   o.debugRenderMeshBoxes);
                nk_label(&ctx, "", NK_TEXT_LEFT);

                // render tile corners
                nk_label(&ctx, "", NK_TEXT_LEFT);
                o.debugRenderTileBoxes = nk_check_label(&ctx, "tile boxes",
                                                    o.debugRenderTileBoxes);
                nk_label(&ctx, "", NK_TEXT_LEFT);

                // render surrogates
                nk_label(&ctx, "", NK_TEXT_LEFT);
                o.debugRenderSurrogates = nk_check_label(&ctx, "surrogates",
                                                    o.debugRenderSurrogates);
                nk_label(&ctx, "", NK_TEXT_LEFT);

                // render objective position
                nk_label(&ctx, "", NK_TEXT_LEFT);
                o.debugRenderObjectPosition = nk_check_label(&ctx,
                                "object. pos.", o.debugRenderObjectPosition);
                nk_label(&ctx, "", NK_TEXT_LEFT);

                // render target position
                nk_label(&ctx, "", NK_TEXT_LEFT);
                o.debugRenderTargetPosition = nk_check_label(&ctx,
                                "target. pos.", o.debugRenderTargetPosition);
                nk_label(&ctx, "", NK_TEXT_LEFT);

                // render altitude shift corners
                nk_label(&ctx, "", NK_TEXT_LEFT);
                o.debugRenderAltitudeShiftCorners = nk_check_label(&ctx,
                    "alt. shift corns.", o.debugRenderAltitudeShiftCorners);
                nk_label(&ctx, "", NK_TEXT_LEFT);

                // flat shading
                nk_label(&ctx, "", NK_TEXT_LEFT);
                o.debugFlatShading = nk_check_label(&ctx, "flat shading",
                                                    o.debugFlatShading);
                nk_label(&ctx, "", NK_TEXT_LEFT);

                // polygon edges
                nk_label(&ctx, "", NK_TEXT_LEFT);
                a.render.renderPolygonEdges = nk_check_label(&ctx, "edges",
                                                a.render.renderPolygonEdges);
                nk_label(&ctx, "", NK_TEXT_LEFT);

                // render no meshes
                nk_label(&ctx, "", NK_TEXT_LEFT);
                o.debugRenderNoMeshes = nk_check_label(&ctx,
                    "no meshes", o.debugRenderNoMeshes);
                nk_label(&ctx, "", NK_TEXT_LEFT);
            }

            // debug
            {
                // enable camera normalization
                nk_label(&ctx, "Debug:", NK_TEXT_LEFT);
                o.enableCameraNormalization = nk_check_label(&ctx,
                                "cam. norm.", o.enableCameraNormalization);
                nk_label(&ctx, "", NK_TEXT_LEFT);

                // disable camera zoom limit
                {
                    int e = viewExtentLimitScaleMax
                            == std::numeric_limits<double>::infinity();
                    int ePrev = e;
                    nk_label(&ctx, "", NK_TEXT_LEFT);
                    nk_checkbox_label(&ctx, "zoom limit", &e);
                    nk_label(&ctx, "", NK_TEXT_RIGHT);
                    if (e != ePrev)
                    {
                        std::swap(viewExtentLimitScaleMin,
                                  o.viewExtentLimitScaleMin);
                        std::swap(viewExtentLimitScaleMax,
                                  o.viewExtentLimitScaleMax);
                    }
                }

                // detached camera
                nk_label(&ctx, "", NK_TEXT_LEFT);
                o.debugDetachedCamera = nk_check_label(&ctx,
                                "detached camera", o.debugDetachedCamera);
                nk_label(&ctx, "", NK_TEXT_LEFT);

                // debug disable meta 5
                {
                    nk_label(&ctx, "", NK_TEXT_LEFT);
                    bool old = o.debugDisableMeta5;
                    o.debugDisableMeta5 = nk_check_label(&ctx, "disable meta5",
                                                        o.debugDisableMeta5);
                    nk_label(&ctx, "", NK_TEXT_LEFT);
                    if (old != o.debugDisableMeta5)
                        window->map->purgeViewCache();
                }

                // debug disable virtual surfaces
                {
                    nk_label(&ctx, "", NK_TEXT_LEFT);
                    bool old = o.debugDisableVirtualSurfaces;
                    o.debugDisableVirtualSurfaces = nk_check_label(&ctx,
                            "disable virt.s.", o.debugDisableVirtualSurfaces);
                    nk_label(&ctx, "", NK_TEXT_LEFT);
                    if (old != o.debugDisableVirtualSurfaces)
                        window->map->purgeViewCache();
                }

                // print debug info
                nk_label(&ctx, "", NK_TEXT_LEFT);
                if (nk_button_label(&ctx, "Print debug info"))
                    window->map->printDebugInfo();
                nk_label(&ctx, "", NK_TEXT_LEFT);

                // purge disk cache
                nk_label(&ctx, "", NK_TEXT_LEFT);
                if (nk_button_label(&ctx, "Purge disk cache"))
                    window->map->purgeDiskCache();
                nk_label(&ctx, "", NK_TEXT_LEFT);
            }
        }
        nk_end(&ctx);
    }

    void prepareStatistics()
    {
        int flags = NK_WINDOW_BORDER | NK_WINDOW_MOVABLE
                | NK_WINDOW_SCALABLE | NK_WINDOW_TITLE
                | NK_WINDOW_MINIMIZABLE;
        if (prepareFirst)
            flags |= NK_WINDOW_MINIMIZED;
        if (nk_begin(&ctx, "Statistics", nk_rect(270, 10, 250, 700), flags))
        {
            MapStatistics &s = window->map->statistics();
            float width = nk_window_get_content_region_size(&ctx).x - 15;
            float ratio[] = { width * 0.5f, width * 0.5f };
            nk_layout_row(&ctx, NK_STATIC, 16, 2, ratio);
            nk_label(&ctx, "Loading:", NK_TEXT_LEFT);
            nk_prog(&ctx, (int)(1000 * window->map->getMapRenderProgress()),
                    1000, false);
            char buffer[256];
#define S(NAME, VAL, UNIT) { \
                nk_label(&ctx, NAME, NK_TEXT_LEFT); \
                sprintf(buffer, "%d" UNIT, VAL); \
                nk_label(&ctx, buffer, NK_TEXT_RIGHT); \
            }

            // general
            S("Time map:", window->timingMapProcess, " ms");
            S("Time app:", window->timingAppProcess, " ms");
            S("Time frame:", window->timingTotalFrame, " ms");
            S("Time data:", window->timingDataFrame, " ms");
            S("Render ticks:", s.renderTicks, "");
            S("Data ticks:", s.dataTicks, "");
            S("Downloading:", s.currentResourceDownloads, "");
            S("Node meta updates:", s.currentNodeMetaUpdates, "");
            S("Node draw updates:", s.currentNodeDrawsUpdates, "");
            S("Resources gpu mem.:", \
                            (int)(s.currentGpuMemUse / 1024 / 1024), " MB");
            S("Resources ram mem.:", \
                            (int)(s.currentRamMemUse / 1024 / 1024), " MB");
            S("Des. Nav. lod:", s.desiredNavigationLod, "");
            S("Used Nav. lod:", s.usedNavigationlod, "");
            nk_label(&ctx, "Z range:", NK_TEXT_LEFT);
            sprintf(buffer, "%0.0f - %0.0f", window->map->draws().camera.near,
                    window->map->draws().camera.far);
            nk_label(&ctx, buffer, NK_TEXT_RIGHT);
            nk_label(&ctx, "Nav. type:", NK_TEXT_LEFT);
            nk_label(&ctx, navigationTypeNames[(int)s.currentNavigationType],
                    NK_TEXT_RIGHT);
            nk_label(&ctx, "Nav. mode:", NK_TEXT_LEFT);
            nk_label(&ctx, navigationModeNames[(int)s.currentNavigationMode],
                    NK_TEXT_RIGHT);

            // resources
            S("Res. active:", s.currentResources, "");
            S("Res. preparing:", s.currentResourcePreparing, "");
            S("Res. downloaded:", s.resourcesDownloaded, "");
            S("Res. disk loaded:", s.resourcesDiskLoaded, "");
            S("Res. processed:", s.resourcesProcessLoaded, "");
            S("Res. released:", s.resourcesReleased, "");
            S("Res. ignored:", s.resourcesIgnored, "");
            S("Res. failed:", s.resourcesFailed, "");

            // traversed
            S("Traversed:", s.metaNodesTraversedTotal, "");
            nk_label(&ctx, "", NK_TEXT_LEFT);
            nk_checkbox_label(&ctx, "details", &statTraversedDetails);
            if (statTraversedDetails)
            {
                for (unsigned i = 0; i < MapStatistics::MaxLods; i++)
                {
                    if (s.metaNodesTraversedPerLod[i] == 0)
                        continue;
                    sprintf(buffer, "[%d]:", i);
                    S(buffer, s.metaNodesTraversedPerLod[i], "");
                }
            }

            // rendered
            S("Rendered:", s.meshesRenderedTotal, "");
            nk_label(&ctx, "", NK_TEXT_LEFT);
            nk_checkbox_label(&ctx, "details", &statRenderedDetails);
            if (statRenderedDetails)
            {
                for (unsigned i = 0; i < MapStatistics::MaxLods; i++)
                {
                    if (s.meshesRenderedPerLod[i] == 0)
                        continue;
                    sprintf(buffer, "[%d]:", i);
                    S(buffer, s.meshesRenderedPerLod[i], "");
                }
            }
#undef S
        }
        nk_end(&ctx);
    }

    void preparePosition()
    {
        int flags = NK_WINDOW_BORDER | NK_WINDOW_MOVABLE
                | NK_WINDOW_SCALABLE | NK_WINDOW_TITLE
                | NK_WINDOW_MINIMIZABLE;
        if (prepareFirst)
            flags |= NK_WINDOW_MINIMIZED;
        if (nk_begin(&ctx, "Position", nk_rect(530, 10, 300, 500), flags))
        {
            float width = nk_window_get_content_region_size(&ctx).x - 15;
            float ratio[] = { width * 0.3f, width * 0.7f };
            nk_layout_row(&ctx, NK_STATIC, 16, 2, ratio);
            char buffer[256];
            // input
            {
                nk_label(&ctx, "Input:", NK_TEXT_LEFT);
                if (nk_button_label(&ctx, "Use from clipboard"))
                {
                    try
                    {
                        const char *text = SDL_GetClipboardText();
                        window->map->setPositionUrl(text,
                                    NavigationType::Instant);
                    }
                    catch(...)
                    {
                        // do nothing
                    }
                }
            }
            // subjective position
            {
                int subj = window->map->getPositionSubjective();
                int prev = subj;
                nk_label(&ctx, "Type:", NK_TEXT_LEFT);
                nk_checkbox_label(&ctx, "subjective", &subj);
                if (subj != prev)
                    window->map->setPositionSubjective(!!subj, true);
            }
            // srs
            {
                static const char *names[] = {
                    "Physical",
                    "Navigation",
                    "Public",
                };
                nk_label(&ctx, "Srs:", NK_TEXT_LEFT);
                if (nk_combo_begin_label(&ctx,
                                 names[positionSrs],
                                 nk_vec2(nk_widget_width(&ctx), 200)))
                {
                    nk_layout_row_dynamic(&ctx, 16, 1);
                    for (int i = 0; i < 3; i++)
                        if (nk_combo_item_label(&ctx, names[i], NK_TEXT_LEFT))
                            positionSrs = i;
                    nk_combo_end(&ctx);
                }
            }
            nk_layout_row(&ctx, NK_STATIC, 16, 2, ratio);
            // position
            {
                double n[3];
                window->map->getPositionPoint(n);
                try
                {
                    window->map->convert(n, n, Srs::Navigation,
                                         (Srs)positionSrs);
                }
                catch (const std::exception &)
                {
                    for (int i = 0; i < 3; i++)
                        n[i] = std::numeric_limits<double>::quiet_NaN();
                }
                nk_label(&ctx, "X:", NK_TEXT_LEFT);
                sprintf(buffer, "%.8f", n[0]);
                nk_label(&ctx, buffer, NK_TEXT_RIGHT);
                nk_label(&ctx, "Y:", NK_TEXT_LEFT);
                sprintf(buffer, "%.8f", n[1]);
                nk_label(&ctx, buffer, NK_TEXT_RIGHT);
                nk_label(&ctx, "Z:", NK_TEXT_LEFT);
                sprintf(buffer, "%.8f", n[2]);
                nk_label(&ctx, buffer, NK_TEXT_RIGHT);
                nk_label(&ctx, "", NK_TEXT_LEFT);
                if (nk_button_label(&ctx, "Reset altitude"))
                    window->map->resetPositionAltitude();
            }
            // rotation
            {
                double n[3];
                window->map->getPositionRotation(n);
                nk_label(&ctx, "Rotation:", NK_TEXT_LEFT);
                sprintf(buffer, "%5.1f", n[0]);
                nk_label(&ctx, buffer, NK_TEXT_RIGHT);
                nk_label(&ctx, "", NK_TEXT_LEFT);
                sprintf(buffer, "%5.1f", n[1]);
                nk_label(&ctx, buffer, NK_TEXT_RIGHT);
                nk_label(&ctx, "", NK_TEXT_LEFT);
                sprintf(buffer, "%5.1f", n[2]);
                nk_label(&ctx, buffer, NK_TEXT_RIGHT);
                nk_label(&ctx, "", NK_TEXT_LEFT);
                if (nk_button_label(&ctx, "Reset rotation"))
                {
                    window->map->setPositionRotation({0,270,0},
                                            NavigationType::Quick);
                    window->map->resetNavigationMode();
                }
            }
            // view extent
            {
                nk_label(&ctx, "View extent:", NK_TEXT_LEFT);
                sprintf(buffer, "%10.1f", window->map->getPositionViewExtent());
                nk_label(&ctx, buffer, NK_TEXT_RIGHT);
            }
            // fov
            {
                nk_label(&ctx, "Fov:", NK_TEXT_LEFT);
                window->map->setPositionFov(nk_slide_float(&ctx, 10,
                                    window->map->getPositionFov(), 100, 1));
                nk_label(&ctx, "", NK_TEXT_LEFT);
                sprintf(buffer, "%5.1f", window->map->getPositionFov());
                nk_label(&ctx, buffer, NK_TEXT_RIGHT);
            }
            // output
            {
                nk_label(&ctx, "Output:", NK_TEXT_LEFT);
                if (nk_button_label(&ctx, "Copy to clipboard"))
                    SDL_SetClipboardText(window->map->getPositionUrl().c_str());
            }
            // auto movement
            {
                nk_label(&ctx, "Automatic:", NK_TEXT_LEFT);
                nk_checkbox_label(&ctx, "", &posAutoDetails);
                if (posAutoDetails)
                {
                    bool nomove = true;
                    for (int i = 0; i < 3; i++)
                    {
                        nk_label(&ctx, i == 0 ? "Move:" : "", NK_TEXT_LEFT);
                        posAutoMotion[i] = nk_slide_float(&ctx, -3,
                                                    posAutoMotion[i], 3, 0.1);
                        if (std::abs(posAutoMotion[i]) > 1e-5)
                            nomove = false;
                    }
                    if (!nomove)
                        window->map->pan(posAutoMotion.data());
                    nk_label(&ctx, "Rotate:", NK_TEXT_LEFT);
                    window->map->setAutoRotation(nk_slide_float(&ctx, -1,
                                    window->map->getAutoRotation(), 1, 0.05));
                }
            }
        }
        nk_end(&ctx);
    }

    bool prepareViewsBoundLayers(MapView &,
                                 MapView::BoundLayerInfo::Map &bl)
    {
        const std::vector<std::string> boundLayers
                = window->map->getResourceBoundLayers();
        std::set<std::string> bls(boundLayers.begin(), boundLayers.end());
        float width = nk_window_get_content_region_size(&ctx).x - 15 - 10 - 25;
        float ratio[] = { 10, width * 0.7f, width * 0.3f, 20};
        nk_layout_row(&ctx, NK_STATIC, 16, 4, ratio);
        bool changed = false;
        int idx = 0;
        for (auto &&bn : bl)
        {
            nk_label(&ctx, "", NK_TEXT_LEFT);
            if (!nk_check_label(&ctx, bn.id.c_str(), 1))
            {
                bl.erase(bl.begin() + idx);
                return true;
            }
            bls.erase(bn.id);
            // alpha
            double a2 = nk_slide_float(&ctx, 0.1, bn.alpha , 1, 0.1);
            if (bn.alpha != a2)
            {
                bn.alpha = a2;
                changed = true;
            }
            // arrows
            if (idx > 0)
            {
                if (nk_button_label(&ctx, "^"))
                {
                    std::swap(bl[idx - 1], bl[idx]);
                    return true;
                }
            }
            else
                nk_label(&ctx, "", NK_TEXT_LEFT);
            idx++;
        }
        for (auto &&bn : bls)
        {
            nk_label(&ctx, "", NK_TEXT_LEFT);
            if (nk_check_label(&ctx, bn.c_str(), 0))
            {
                bl.push_back(MapView::BoundLayerInfo(bn));
                return true;
            }
            nk_label(&ctx, "", NK_TEXT_LEFT);
            nk_label(&ctx, "", NK_TEXT_LEFT);
        }
        return changed;
    }

    void prepareViews()
    {
        int flags = NK_WINDOW_BORDER | NK_WINDOW_MOVABLE
                | NK_WINDOW_SCALABLE | NK_WINDOW_TITLE
                | NK_WINDOW_MINIMIZABLE;
        if (prepareFirst)
            flags |= NK_WINDOW_MINIMIZED;
        if (nk_begin(&ctx, "Views", nk_rect(840, 10, 300, 400), flags))
        {
            float width = nk_window_get_content_region_size(&ctx).x - 15;

            // mapconfig selector
            if (window->appOptions.paths.size() > 1)
            {
                float ratio[] = { width * 0.2f, width * 0.8f };
                nk_layout_row(&ctx, NK_STATIC, 20, 2, ratio);
                nk_label(&ctx, "Config:", NK_TEXT_LEFT);
                if (nk_combo_begin_label(&ctx,
                                 window->map->getMapConfigPath().c_str(),
                                 nk_vec2(nk_widget_width(&ctx), 200)))
                {
                    nk_layout_row_dynamic(&ctx, 16, 1);
                    for (int i = 0, e = window->appOptions.paths.size();
                         i < e; i++)
                    {
                        if (nk_combo_item_label(&ctx,
                                window->appOptions.paths[i].mapConfig.c_str(),
                                                NK_TEXT_LEFT))
                        {
                            window->marks.clear();
                            window->setMapConfigPath(
                                        window->appOptions.paths[i]);
                            nk_combo_end(&ctx);
                            nk_end(&ctx);
                            return;
                        }
                    }
                    nk_combo_end(&ctx);
                }
            }

            std::vector<std::string> names = window->map->getViewNames();
            if (!names.empty())
            {
                // view selector
                if (names.size() > 1)
                {
                    float ratio[] = { width * 0.2f, width * 0.8f };
                    nk_layout_row(&ctx, NK_STATIC, 20, 2, ratio);
                    nk_label(&ctx, "View:", NK_TEXT_LEFT);
                    if (nk_combo_begin_label(&ctx,
                                     window->map->getViewCurrent().c_str(),
                                     nk_vec2(nk_widget_width(&ctx), 200)))
                    {
                        nk_layout_row_dynamic(&ctx, 16, 1);
                        for (int i = 0, e = names.size(); i < e; i++)
                            if (nk_combo_item_label(&ctx, names[i].c_str(),
                                                    NK_TEXT_LEFT))
                                window->map->setViewCurrent(names[i]);
                        nk_combo_end(&ctx);
                    }
                }

                // current view
                bool viewChanged = false;
                MapView view;
                window->map->getViewData(window->map->getViewCurrent(), view);
                // surfaces
                {
                    const std::vector<std::string> surfaces
                            = window->map->getResourceSurfaces();
                    for (const std::string &sn : surfaces)
                    {
                        float ratio[] = { width };
                        nk_layout_row(&ctx, NK_STATIC, 16, 1, ratio);
                        bool v1 = view.surfaces.find(sn) != view.surfaces.end();
                        bool v2 = nk_check_label(&ctx, sn.c_str(), v1);
                        if (v2)
                        { // bound layers
                            MapView::SurfaceInfo &s = view.surfaces[sn];
                            viewChanged = viewChanged
                                || prepareViewsBoundLayers(view, s.boundLayers);
                        }
                        else
                            view.surfaces.erase(sn);
                        if (v1 != v2)
                            viewChanged = true;
                    }
                }
                // free layers
                {
                    // todo
                }
                if (viewChanged)
                {
                    window->map->setViewData("", view);
                    window->map->setViewCurrent("");
                }
            }
        }
        nk_end(&ctx);
    }

    void prepareMarks()
    {
        int flags = NK_WINDOW_BORDER | NK_WINDOW_MOVABLE
                | NK_WINDOW_SCALABLE | NK_WINDOW_TITLE
                | NK_WINDOW_MINIMIZABLE;
        if (prepareFirst)
            flags |= NK_WINDOW_MINIMIZED;
        if (nk_begin(&ctx, "Marks", nk_rect(1150, 10, 250, 400), flags))
        {
            std::vector<Mark> &marks = window->marks;
            float width = nk_window_get_content_region_size(&ctx).x - 15;
            float ratio[] = { width * 0.6f, width * 0.4f };
            nk_layout_row(&ctx, NK_STATIC, 16, 2, ratio);
            char buffer[256];
            Mark *prev = nullptr;
            int i = 0;
            double length = 0;
            for (Mark &m : marks)
            {
                sprintf(buffer, "%d", (i + 1));
                nk_checkbox_label(&ctx, buffer, &m.open);
                double l = prev
                        ? vts::length(vec3(prev->coord - m.coord))
                        : 0;
                length += l;
                sprintf(buffer, "%.3f", l);
                nk_color c;
                c.r = 255 * m.color(0);
                c.g = 255 * m.color(1);
                c.b = 255 * m.color(2);
                c.a = 255;
                nk_label_colored(&ctx, buffer, NK_TEXT_RIGHT, c);
                if (m.open)
                {
                    double n[3] = { m.coord(0), m.coord(1), m.coord(2) };
                    try
                    {
                        window->map->convert(n, n, Srs::Physical,
                                             (Srs)positionSrs);
                    }
                    catch(...)
                    {
                        for (int i = 0; i < 3; i++)
                            n[i] = std::numeric_limits<double>::quiet_NaN();
                    }
                    sprintf(buffer, "%.8f", n[0]);
                    nk_label(&ctx, buffer, NK_TEXT_RIGHT);
                    if (nk_button_label(&ctx, "Go"))
                    {
                        double n[3] = { m.coord(0), m.coord(1), m.coord(2) };
                        window->map->convert(n, n, Srs::Physical,
                                                 Srs::Navigation);
                        window->map->setPositionPoint(n,
                                                NavigationType::Quick);
                    }
                    sprintf(buffer, "%.8f", n[1]);
                    nk_label(&ctx, buffer, NK_TEXT_RIGHT);
                    nk_label(&ctx, "", NK_TEXT_RIGHT);
                    sprintf(buffer, "%.8f", n[2]);
                    nk_label(&ctx, buffer, NK_TEXT_RIGHT);
                    if (nk_button_label(&ctx, "Remove"))
                    {
                        marks.erase(marks.begin() + i);
                        break;
                    }
                }
                prev = &m;
                i++;
            }
            nk_label(&ctx, "Total:", NK_TEXT_LEFT);
            sprintf(buffer, "%.3f", length);
            nk_label(&ctx, buffer, NK_TEXT_RIGHT);
            nk_label(&ctx, "", NK_TEXT_LEFT);
            if (nk_button_label(&ctx, "Clear all"))
                marks.clear();
        }
        nk_end(&ctx);
    }

    void prepareSearch()
    {
        if (search && window->map->statistics().renderTicks % 120 == 60)
        {
            try
            {
                double point[3];
                window->map->getPositionPoint(point);
                search->updateDistances(point);
            }
            catch (...)
            {
                search.reset();
            }
        }

        int flags = NK_WINDOW_BORDER | NK_WINDOW_MOVABLE
                | NK_WINDOW_SCALABLE | NK_WINDOW_TITLE
                | NK_WINDOW_MINIMIZABLE;
        if (prepareFirst)
            flags |= NK_WINDOW_MINIMIZED;
        if (nk_begin(&ctx, "Search", nk_rect(1410, 10, 350, 500), flags))
        {
            float width = nk_window_get_content_region_size(&ctx).x - 15;
            if (!window->map->searchable())
            {
                nk_layout_row(&ctx, NK_STATIC, 20, 1, &width);
                nk_label(&ctx, "Search not available.", NK_TEXT_LEFT);
                nk_end(&ctx);
                return;
            }
            // search query
            {
                float ratio[] = { width * 0.2f, width * 0.8f };
                nk_layout_row(&ctx, NK_STATIC, 22, 2, ratio);
                nk_label(&ctx, "Query:", NK_TEXT_LEFT);
                int len = strlen(searchText);
                nk_edit_string(&ctx, NK_EDIT_FIELD | NK_EDIT_AUTO_SELECT,
                               searchText, &len,
                        MaxSearchTextLength - 1, nullptr);
                searchText[len] = 0;
                if (strcmp(searchText, searchTextPrev) != 0)
                {
                    if (nk_utf_len(searchText, len) >= 3)
                        search = window->map->search(searchText);
                    else
                        search.reset();
                    strcpy(searchTextPrev, searchText);
                    searchDetails = -1;
                }
            }
            // search results
            if (!search)
            {
                nk_end(&ctx);
                return;
            }
            if (!search->done)
            {
                nk_layout_row(&ctx, NK_STATIC, 20, 1, &width);
                nk_label(&ctx, "Searching...", NK_TEXT_LEFT);
                nk_end(&ctx);
                return;
            }
            if (search->results.empty())
            {
                nk_layout_row(&ctx, NK_STATIC, 20, 1, &width);
                nk_label(&ctx, "No results.", NK_TEXT_LEFT);
                nk_end(&ctx);
                return;
            }
            char buffer[200];
            std::vector<SearchItem> &res = search->results;
            int index = 0;
            for (auto &r : res)
            {
                float ratio[] = { width * 0.8f, width * 0.2f };
                nk_layout_row(&ctx, NK_STATIC, 18, 2, ratio);
                nk_label(&ctx, r.title.c_str(), NK_TEXT_LEFT);
                if (r.position[0] == r.position[0])
                {
                    if (nk_button_label(&ctx, "Go"))
                    {
                        window->map->setPositionSubjective(false, false);
                        window->map->setPositionViewExtent(
                                    std::max(6667.0, r.radius * 2),
                                    NavigationType::FlyOver);
                        window->map->setPositionRotation({0,270,0},
                                    NavigationType::FlyOver);
                        window->map->resetPositionAltitude();
                        window->map->resetNavigationMode();
                        window->map->setPositionPoint(r.position,
                                    NavigationType::FlyOver);
                    }
                }
                else
                    nk_label(&ctx, "", NK_TEXT_LEFT);
                nk_layout_row(&ctx, NK_STATIC, 16, 2, ratio);
                bool details = nk_check_label(&ctx, r.region.c_str(),
                                              searchDetails == index);
                if (r.distance >= 1e3)
                    sprintf(buffer, "%.1lf km", r.distance / 1e3);
                else
                    sprintf(buffer, "%.1lf m", r.distance);
                nk_label(&ctx, buffer, NK_TEXT_RIGHT);
                if (details)
                {
                    searchDetails = index;
                    float ratio[] = { width * 0.2f, width * 0.8f };
                    nk_layout_row(&ctx, NK_STATIC, 16, 2, ratio);
                    nk_label(&ctx, "Name:", NK_TEXT_LEFT);
                    nk_label(&ctx, r.displayName.c_str(), NK_TEXT_LEFT);
                    nk_label(&ctx, "Type:", NK_TEXT_LEFT);
                    nk_label(&ctx, r.type.c_str(), NK_TEXT_LEFT);
                    nk_label(&ctx, "Road:", NK_TEXT_LEFT);
                    nk_label(&ctx, r.road.c_str(), NK_TEXT_LEFT);
                    nk_label(&ctx, "City:", NK_TEXT_LEFT);
                    nk_label(&ctx, r.city.c_str(), NK_TEXT_LEFT);
                    nk_label(&ctx, "County:", NK_TEXT_LEFT);
                    nk_label(&ctx, r.county.c_str(), NK_TEXT_LEFT);
                    nk_label(&ctx, "State:", NK_TEXT_LEFT);
                    nk_label(&ctx, r.state.c_str(), NK_TEXT_LEFT);
                    nk_label(&ctx, "Number:", NK_TEXT_LEFT);
                    nk_label(&ctx, r.houseNumber.c_str(), NK_TEXT_LEFT);
                    nk_label(&ctx, "District:", NK_TEXT_LEFT);
                    nk_label(&ctx, r.stateDistrict.c_str(), NK_TEXT_LEFT);
                    nk_label(&ctx, "Country:", NK_TEXT_LEFT);
                    nk_label(&ctx, r.country.c_str(), NK_TEXT_LEFT);
                    nk_label(&ctx, "Code:", NK_TEXT_LEFT);
                    nk_label(&ctx, r.countryCode.c_str(), NK_TEXT_LEFT);
                    nk_label(&ctx, "Importance:", NK_TEXT_LEFT);
                    sprintf(buffer, "%lf", r.importance);
                    nk_label(&ctx, buffer, NK_TEXT_LEFT);
                    nk_label(&ctx, "Radius:", NK_TEXT_LEFT);
                    sprintf(buffer, "%lf", r.radius);
                    nk_label(&ctx, buffer, NK_TEXT_LEFT);
                }
                else if (searchDetails == index)
                    searchDetails = -1;
                index++;
            }
        }
        nk_end(&ctx);
    }

    void prepare(int, int)
    {
        prepareOptions();
        prepareStatistics();
        preparePosition();
        prepareViews();
        prepareMarks();
        prepareSearch();
        prepareFirst = false;
    }

    void render(int width, int height)
    {
        prepare(width, height);
        dispatch(width, height);
    }

    static const int MaxSearchTextLength = 200;
    char searchText[MaxSearchTextLength];
    char searchTextPrev[MaxSearchTextLength];
    char positionInputText[MaxSearchTextLength];

    std::shared_ptr<Texture> fontTexture;
    std::shared_ptr<Texture> skinTexture;
    std::shared_ptr<Shader> shader;
    std::shared_ptr<Mesh> mesh;
    std::shared_ptr<SearchTask> search;

    GuiSkinMedia skinMedia;
    nk_context ctx;
    nk_font_atlas atlas;
    nk_font *font;
    nk_buffer cmds;
    nk_convert_config config;
    nk_draw_null_texture null;

    vec3 posAutoMotion;
    double viewExtentLimitScaleMin;
    double viewExtentLimitScaleMax;

    int statTraversedDetails;
    int statRenderedDetails;
    int optSensitivityDetails;
    int posAutoDetails;
    int positionSrs;
    int searchDetails;

    MainWindow *window;
    bool prepareFirst;

    static const int MaxVertexMemory = 4 * 1024 * 1024;
    static const int MaxElementMemory = 4 * 1024 * 1024;
};

void MainWindow::Gui::initialize(MainWindow *window)
{
    impl = std::make_shared<GuiImpl>(window);
}

void MainWindow::Gui::render(int width, int height)
{
    impl->render(width, height);
}

void MainWindow::Gui::inputBegin()
{
    nk_input_begin(&impl->ctx);
}

bool MainWindow::Gui::input(SDL_Event &event)
{
    impl->handleEvent(&event);
    return nk_item_is_any_active(&impl->ctx);
}

void MainWindow::Gui::inputEnd()
{
    nk_input_end(&impl->ctx);
}

void MainWindow::Gui::finalize()
{
    impl.reset();
}