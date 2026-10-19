// SPDX-License-Identifier: GPL-3.0-or-later
// DSperate - Nintendo DS emulator. Copyright (C) 2026 DSperate contributors.
#pragma once
// The pause menu DSlot and the other emulators share: nibkit's menu
// (nibOS's sdk/nibkit-menu-c), DSlot's rows, tabs, face and buttons, drawn
// as one picture for the bottom screen while the game is paused. The top
// screen shows the game, dimmed, or the highlighted save state's picture.
//
// Tabs: Play (resume, states, fast forward, reset, save and quit), States
// (a card per slot, the resume point last), Game (what is kept for this game:
// its cheats, achievements, settings for it alone), DS or GBA (DSperate's
// options for the console, under headings; controls) and System (volume).
// Built only with DSPERATE_NIBKIT_MENU; the frontend falls back to Menu.
#include "frontend/sdl/pause_menu.h"
#include "frontend/sdl/settings.h"

#include <map>
#include <string>
#include <vector>

struct NibMenu;
struct NibSettings;
struct NibOptions;

namespace ds::sdl {

// What only the frontend knows. Implemented in main.cpp.
struct ShelfHost {
  virtual ~ShelfHost() = default;
  virtual std::string game_title() const = 0;   // as DSlot names it: the ROM's file name
  virtual bool gba() const = 0;                 // the console's tab reads GBA, the bar shows slot-2
  virtual bool fast_forward() const = 0;
  // A slot's picture (RGBA bytes, rows top first): false when it has none.
  virtual bool slot_picture(int slot, std::vector<u8>& rgba, int& w, int& h) = 0;
  // When a slot was saved, as a card's detail ("Today 4:41 PM"); empty if unknown.
  virtual std::string slot_saved(int slot) = 0;
  virtual int  volume() const = 0;              // 0-100
  virtual void set_volume(int percent) = 0;
  virtual std::string clock() const = 0;        // the bar's clock, as shown
  virtual int  battery(bool& charging) const = 0;   // percent, or -1 without one
};

class ShelfMenu : public PauseMenu {
public:
  ShelfMenu();
  ~ShelfMenu() override;
  ShelfMenu(const ShelfMenu&) = delete;
  ShelfMenu& operator=(const ShelfMenu&) = delete;

  void set_host(ShelfHost* host) { shelf_ = host; }

  bool open() const override { return open_; }
  void set_open(bool o) override;
  void set_cheats(std::vector<cheat::Code>* codes, const std::vector<cheat::Group>* groups) override { codes_ = codes; groups_ = groups; }
  bool cheats_dirty() const override { return cheats_dirty_; }
  void clear_cheats_dirty() override { cheats_dirty_ = false; }
  void set_settings_host(SettingsHost* host) override { host_ = host; }
  void set_cheevos_host(CheevosHost* host) override { cheevos_ = host; }
  void set_network_session(bool on) override { net_ = on; dirty_ = true; }
  // The loader cart's picker is DSperate's own menu's; not raised here.
  void set_games(const std::vector<GameEntry>*) override {}
  void open_games() override {}
  const std::string& chosen() const override { return chosen_; }
  int  slot() const override { return slot_; }
  void set_slot(int s) override { slot_ = s; }
  int  doomed_slot() const override { return slot_; }
  void set_slot_used(int s, bool used) override;
  void set_slot_notice(const char* n) override { if (n && *n) message(std::string("Slot ") + std::to_string(slot_ + 1) + ": " + n); }
  Result update(u32 presses, u32 held, u32 ms) override;
  void face_presses(u32) override {}
  bool dirty() const override { return dirty_; }
  void clear_dirty() override { dirty_ = false; }
  bool panel_ui() const override { return true; }

  // A finger down on the bottom screen, in its pixels (1024 x 768).
  Result touch(float x, float y);
  // The bottom screen's picture: RGBA, rows top first, valid until the next call.
  const u8* render(int& w, int& h);
  // The top screen's picture, the same size: `game` (the top screen, DS
  // 0xAARRGGBB words, 256x192) dimmed under Paused and the game's name, or
  // the highlighted save state's picture with when it was saved.
  const u8* render_top(const u32* game, int& w, int& h);

private:
  enum Tab : int { Play, States, Game, Console, System, kTabs };
  enum class Sub : u8 { None, Cheats, Achievements, Controls };
  struct Row {
    enum Kind : u8 { Heading, Resume, SaveState, LoadState, FastForward, Reset, SaveQuit, SaveForGame,
                     Declared, Setting, Cheats, Cheat, Achievements, CheevoOption, Controls, Binding, BindReset,
                     Volume, Brightness, Clock, Info } kind;
    const ds::sdl::Setting* setting = nullptr;
    int at = 0;
  };

  NibMenu* m_ = nullptr;
  // DSlot's settings file and DSperate's options as its description
  // declares them: a declared option lives there, not in DSperate's ini.
  NibSettings* settings_ = nullptr;
  NibOptions* options_ = nullptr;
  bool options_gba_ = false;     // which console options_ was loaded for
  ShelfHost* shelf_ = nullptr;
  SettingsHost* host_ = nullptr;
  CheevosHost* cheevos_ = nullptr;
  std::vector<cheat::Code>* codes_ = nullptr;
  const std::vector<cheat::Group>* groups_ = nullptr;
  bool open_ = false, dirty_ = true, net_ = false, cheats_dirty_ = false;
  int tab_ = Play;
  Sub sub_ = Sub::None;
  int slot_ = 0;
  bool used_[kAutoSlot + 1] = {};
  std::string chosen_;
  std::vector<Row> rows_;        // what each row of the page shown does, headings included
  std::vector<int> cards_;       // the States page: the slot each card is
  bool reset_armed_ = false, bind_reset_armed_ = false;
  int save_armed_ = -1;          // the used slot an X has been pressed over once
  bool capturing_ = false; int capture_row_ = -1;
  std::string msg_; u32 msg_ms_ = 0;
  std::map<int, std::vector<u8>> pics_; std::map<int, int> pic_w_, pic_h_;   // slot pictures read so far
  int repeat_key_ = -1; u32 repeat_ms_ = 0; bool repeating_ = false;
  int row_of_tab_[kTabs] = {};   // each tab's highlighted row, kept while the menu is open
  int pending_row_ = -1;         // the row to highlight once the page now opening is filled

  void message(std::string m) { msg_ = std::move(m); msg_ms_ = 2000; dirty_ = true; }
  void fill();                   // the page as it stands, into nib's menu
  void fill_tabs();
  void fill_play();
  void fill_states();
  void fill_game();
  void fill_console();
  void fill_system();
  void fill_cheats();
  void fill_achievements();
  void fill_controls();
  void add_setting_rows(const ds::sdl::Setting* table);
  void add_declared_rows(bool game);
  std::string scope(bool game) const;
  void step_declared(int option, bool game, int dir);
  void use_declared(int option, bool game);
  void apply_declared(int option);   // the value now in force, into this run
  void row(Row r, const char* label, int kind, const char* text = nullptr, int number = 0);
  std::string hint() const;
  Result effect(int kind, int index, int step, int key);
  Result choose(int index);
  void change(int index, int step);
  Result card(int index, int key);
  void toggle_cheat(int at);
  void go_tab(int t);
  int  top_slot() const;
  const std::vector<u8>* picture(int slot, int& w, int& h);
  std::vector<u8> game_rgba_;    // render_top's copy of the game's top screen
};

} // namespace ds::sdl
