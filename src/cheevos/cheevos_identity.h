// SPDX-License-Identifier: GPL-3.0-or-later
// DSperate - Nintendo DS emulator. Copyright (C) 2026 DSperate contributors.
//
// What this build of DSperate calls itself on the network. RetroAchievements tells
// clients apart by the User-Agent's product token alone, refuses one that is not
// unique, and asks for any earlier identity to be disclosed: until this name, the
// fork sent upstream's, "DSperate/<upstream's latest tag, 3.0.5> rcheevos/12.4"
// (docs/retroachievements.md).
#pragma once

namespace ds::cheevos {

// The product name, the one constant to change if the client is ever renamed.
inline constexpr char kClientName[] = "DSperate-nibOS";

// The system whose achievements are played: RetroAchievements' guidance for an
// emulator of more than one system is a core segment in the User-Agent.
enum class Core : unsigned char { Nds, Gba };

// "0.<n>": n is the fork's commit count since upstream 7c1fb67, the same number as
// the package's pkgver (packaging/arch/PKGBUILD), so it rises with every commit and
// names the build it came from. -DDSPERATE_CHEEVOS_VERSION= overrides it.
const char* client_version();

// "DSperate-nibOS/0.226": for requests that are not RetroAchievements' (the DSi TMD download).
const char* product_token();

// "DSperate-nibOS/0.226 nds/0.226 rcheevos/12.4": the product, the core (versioned with
// the emulator, it is ours), then rcheevos' own clause (rc_client_get_user_agent_clause).
const char* user_agent(Core core = Core::Nds);

} // namespace ds::cheevos
