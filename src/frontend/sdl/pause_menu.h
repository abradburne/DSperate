// SPDX-License-Identifier: GPL-3.0-or-later
// DSperate - Nintendo DS emulator. Copyright (C) 2026 DSperate contributors.
#pragma once
// What the frontend asks of a pause menu, whichever draws it: DSperate's own
// (Menu, menu.h, a pixel-font list over the held frame) or the one DSlot and
// the other emulators share (ShelfMenu, shelf_menu.h, nibkit's rows on the
// bottom screen). The frontend picks one at start (ui.menu) and drives it
// only through this, so either can be taken out without touching the other.
#include "core/types.h"
#include "core/cheat/database.h"

#include <string>
#include <vector>

namespace ds::sdl {

struct Canvas;
struct SettingsHost;
struct CheevosHost;

class PauseMenu {
public:
  virtual ~PauseMenu() = default;

  // What the frontend should do after update(). Save/Load act on slot();
  // Launch acts on chosen(); Delete acts on doomed_slot(). Reset restarts
  // whatever was booted; FastForward flips fast forward.
  enum class Result : u8 { None, Resume, Save, Load, Quit, Launch, Reset, Delete, FastForward };

  // Slots 0-9, and the auto state (the resume point) after them.
  static constexpr int kAutoSlot = 10;

  // One game in the loader cart's picker. `title` is the ROM header's title
  // or the filename; `path` is what gets loaded.
  struct GameEntry { std::string title, path; };

  virtual bool open() const = 0;
  virtual void set_open(bool o) = 0;

  // The menu edits `codes` in place (the engine's own vector); `groups` give
  // headings and mark alternative sets. Null or empty: no cheats.
  virtual void set_cheats(std::vector<cheat::Code>* codes, const std::vector<cheat::Group>* groups) = 0;
  // Set when a toggle changed something; cleared by the frontend once saved.
  virtual bool cheats_dirty() const = 0;
  virtual void clear_cheats_dirty() = 0;

  virtual void set_settings_host(SettingsHost* host) = 0;
  // Null: no achievements (a build without them, or switched off).
  virtual void set_cheevos_host(CheevosHost* host) = 0;
  // A network session: no states (they would freeze this machine, not the
  // peer), and the game keeps running behind the menu.
  virtual void set_network_session(bool on) = 0;

  // The loader cart's picker (firmware boot only).
  virtual void set_games(const std::vector<GameEntry>* games) = 0;
  virtual void open_games() = 0;
  virtual const std::string& chosen() const = 0;

  // The quick slot: what the save/load hotkeys and Save/Load act on.
  virtual int  slot() const = 0;
  virtual void set_slot(int s) = 0;
  virtual int  doomed_slot() const = 0;
  virtual void set_slot_used(int s, bool used) = 0;
  // A word after the slot when a state was refused rather than loaded.
  virtual void set_slot_notice(const char* n) = 0;

  // One menu tick. `presses` are DS-button edges since the last call (the
  // player's own bindings), `held` what is down now (repeat), `ms` the time
  // since the last tick.
  virtual Result update(u32 presses, u32 held, u32 ms) = 0;
  // Pad face presses by position (Input::take_menu_faces).
  virtual void face_presses(u32 faces) = 0;

  // True when the picture would differ from the last one drawn.
  virtual bool dirty() const = 0;
  virtual void clear_dirty() = 0;

  // DSperate's own menu draws over the held frame (a canvas); the shared
  // one is a whole picture for the bottom screen instead, see ShelfMenu.
  virtual bool panel_ui() const { return false; }
  virtual void draw(const Canvas&) const {}
};

} // namespace ds::sdl
