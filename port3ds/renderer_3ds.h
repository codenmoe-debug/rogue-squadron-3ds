// The 3DS renderer (renderer_3ds.cpp): ultramodern's render context on citro3d.
#pragma once

#include <cstdint>
#include <memory>

#include "ultramodern/renderer_context.hpp"

namespace rs3ds {
std::unique_ptr<ultramodern::renderer::RendererContext> create_render_context(uint8_t* rdram, ultramodern::renderer::WindowHandle window, bool developer_mode);
}

// this build's release tag, compared with the latest GitHub release at boot (update_3ds.cpp)
#define RS3DS_VERSION "v0.1"
void rs3ds_check_for_update(const char* dir);
void rs3ds_update_from_pc(const char* dir);
