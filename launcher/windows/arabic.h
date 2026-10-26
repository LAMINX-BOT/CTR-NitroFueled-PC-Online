// SPDX-License-Identifier: GPL-2.0-or-later
// Arabic text in the launcher's Dear ImGui (arabic.cpp, gpu/patches/imgui).
#pragma once

namespace arabic {
// Installs (or removes) the shaping and right-to-left ordering of every text ImGui draws.
void Install(bool on);
} // namespace arabic
