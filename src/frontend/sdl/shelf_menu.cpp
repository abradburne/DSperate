// SPDX-License-Identifier: GPL-3.0-or-later
// DSperate - Nintendo DS emulator. Copyright (C) 2026 DSperate contributors.
#include "frontend/sdl/shelf_menu.h"
#include "frontend/sdl/menu.h"   // CheevosHost
#include "core/io/io.h"

#include <nibkit_menu.h>

#include <algorithm>
#include <cctype>
#include <cstring>

namespace ds::sdl {

namespace {
using Btn = ds::io::Io::Button;

constexpr const char* kTabNames[] = {"Play", "States", "Game", nullptr, "System"};
constexpr int kSlotsShown = 6;   // slots 1-6 always; 7-10 only when they hold a state
constexpr u32 kRepeatDelayMs = 400, kRepeatRateMs = 55;

// DSperate's own pages are in capitals (its pixel font); DSlot's rows are
// written as sentences. Words that are names keep their capitals.
std::string sentence(const std::string& s) {
  static const char* const kKeep[] = {"DS", "DSI", "NAND", "DNS", "PIP", "LCD", "3D", "GBA", "MSAA", "FPS", "RGA", "PNG", "AR", "OK", "A", "B", "X", "Y", "L", "R"};
  std::string out;
  size_t i = 0;
  bool first = true;
  while (i < s.size()) {
    size_t j = i;
    while (j < s.size() && s[j] != ' ' && s[j] != '/' && s[j] != '(' && s[j] != ')') ++j;
    std::string w = s.substr(i, j - i);
    bool keep = false;
    for (const char* k : kKeep) if (w == k) keep = true;
    if (!keep) {
      for (char& c : w) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
      if (first && !w.empty()) w[0] = static_cast<char>(std::toupper(static_cast<unsigned char>(w[0])));
    }
    if (!w.empty()) first = false;
    out += w;
    if (j < s.size()) out += s[j];
    i = j + 1;
  }
  return out;
}

int key_of(int btn) {
  switch (btn) {
  case Btn::BTN_UP: return NIB_KEY_UP;
  case Btn::BTN_DOWN: return NIB_KEY_DOWN;
  case Btn::BTN_LEFT: return NIB_KEY_LEFT;
  case Btn::BTN_RIGHT: return NIB_KEY_RIGHT;
  case Btn::BTN_A: return NIB_KEY_A;
  case Btn::BTN_B: return NIB_KEY_B;
  case Btn::BTN_X: return NIB_KEY_X;
  case Btn::BTN_Y: return NIB_KEY_Y;
  case Btn::BTN_L: return NIB_KEY_L;
  case Btn::BTN_R: return NIB_KEY_R;
  default: return -1;
  }
}

std::string slot_name(int s) { return s == PauseMenu::kAutoSlot ? std::string("Resume point") : "Slot " + std::to_string(s + 1); }

// The options DSperate declares to DSlot (packaging/arch/dslot/dsperate.toml)
// and the settings they are in DSperate: those rows come from DSlot's file,
// never DSperate's own tables, so the two menus can't disagree.
struct DeclaredKey { const char* option; const char* ini; };
constexpr DeclaredKey kDeclared[] = {
  {"dsperate.hires3d", "video.hires3d"},
  {"dsperate.aa", "video.aa"},
  {"dsperate.frameskip", "emu.frameskip"},
  {"dsperate.linear", "video.linear"},
};
const char* ini_key_of(const char* option) {
  for (const DeclaredKey& d : kDeclared) if (option && std::strcmp(d.option, option) == 0) return d.ini;
  return nullptr;
}
bool declared_ini(const char* ini) {
  for (const DeclaredKey& d : kDeclared) if (std::strcmp(d.ini, ini) == 0) return true;
  return false;
}
} // namespace

ShelfMenu::ShelfMenu() : m_(nib_menu_new()) {}
ShelfMenu::~ShelfMenu() {
  if (options_) nib_options_free(options_);
  if (settings_) nib_settings_free(settings_);
  nib_menu_free(m_);
}

void ShelfMenu::set_open(bool o) {
  // Layout changes are applied when the menu closes, not as each is stepped.
  if (!o && open_ && host_) host_->commit();
  if (!o && open_) nib_backlight_save();   // the sliders set it as they move; kept once, on leaving
  if (o && !open_) {
    // DSlot doesn't write its file while a game runs, but the player may have
    // between this game's starts; and the console is known by now.
    if (settings_) nib_settings_reload(settings_); else settings_ = nib_settings_open();
    const bool gba = shelf_ && shelf_->gba();
    if (!options_ || options_gba_ != gba) {
      if (options_) nib_options_free(options_);
      options_ = nib_options_load("dsperate", gba ? "GBA" : "NDS");
      options_gba_ = gba;
    }
    tab_ = Play; sub_ = Sub::None;
    reset_armed_ = bind_reset_armed_ = false; save_armed_ = -1;
    capturing_ = false; msg_.clear();
    pics_.clear();   // states may have changed outside the menu (the hotkeys)
    for (int& r : row_of_tab_) r = 0;
    pending_row_ = 0;
  }
  if (!o && capturing_ && host_) { host_->cancel_capture(); capturing_ = false; }
  open_ = o;
  dirty_ = true;
}

void ShelfMenu::set_slot_used(int s, bool used) {
  if (s < 0 || s > kAutoSlot) return;
  if (used_[s] != used || used) pics_.erase(s);   // a save over a used slot changes its picture
  used_[s] = used;
  dirty_ = true;
}

void ShelfMenu::go_tab(int t) {
  if (t < 0 || t >= kTabs) return;
  if (sub_ == Sub::None) row_of_tab_[tab_] = nib_menu_row_at(m_);
  tab_ = t; sub_ = Sub::None;
  pending_row_ = row_of_tab_[t];
  reset_armed_ = bind_reset_armed_ = false; save_armed_ = -1;
  dirty_ = true;
}

void ShelfMenu::row(Row r, const char* label, int kind, const char* text, int number) {
  rows_.push_back(r);
  if (r.kind == Row::Heading) nib_menu_heading(m_, label);
  else nib_menu_row(m_, label, kind, text, number);
}

void ShelfMenu::fill_tabs() {
  for (int t = 0; t < kTabs; ++t) {
    const char* name = t == Console ? (shelf_ && shelf_->gba() ? "GBA" : "DS") : kTabNames[t];
    nib_menu_tab(m_, name, t == tab_);
  }
}

void ShelfMenu::fill_play() {
  row({Row::Resume}, "Resume", NIB_VALUE_NONE);
  if (!net_) {
    const std::string s = "Slot " + std::to_string(slot_ + 1);
    row({Row::SaveState}, "Save state", NIB_VALUE_MORE, s.c_str());
    row({Row::LoadState}, "Load state", NIB_VALUE_MORE, s.c_str());
  }
  row({Row::FastForward}, "Fast-forward", NIB_VALUE_TEXT, shelf_ && shelf_->fast_forward() ? "On" : "Off");
  if (!net_) row({Row::Reset}, "Reset game", reset_armed_ ? NIB_VALUE_WARNING : NIB_VALUE_NONE, reset_armed_ ? "A again to reset" : nullptr);
  row({Row::SaveQuit}, "Save and quit", NIB_VALUE_NONE);
}

void ShelfMenu::fill_states() {
  cards_.clear();
  if (net_) return;
  for (int s = 0; s < kAutoSlot; ++s)
    if (s < kSlotsShown || used_[s]) cards_.push_back(s);
  cards_.push_back(kAutoSlot);
  for (int s : cards_) {
    int w = 0, h = 0;
    const std::vector<u8>* pic = used_[s] ? picture(s, w, h) : nullptr;
    const std::string label = s == kAutoSlot ? std::string("Auto") : std::to_string(s + 1);
    nib_menu_card(m_, label.c_str(), pic ? pic->data() : nullptr, w, h);
  }
}

void ShelfMenu::fill_game() {
  const std::string title = shelf_ ? shelf_->game_title() : std::string();
  row({Row::Heading}, title.c_str(), NIB_VALUE_NONE);
  add_declared_rows(true);
  if (codes_ && !codes_->empty()) {
    int on = 0;
    for (const cheat::Code& c : *codes_) if (c.enabled && !c.is_note()) ++on;
    const std::string v = std::to_string(on) + " on";
    row({Row::Cheats}, "Cheats", NIB_VALUE_MORE, v.c_str());
  }
  if (cheevos_) {
    const std::string p = cheevos_->has_set() ? cheevos_->progress() : cheevos_->status();
    row({Row::Achievements}, "Achievements", NIB_VALUE_MORE, sentence(p).c_str());
  }
  if (host_ && host_->has_game())
    row({Row::SaveForGame}, "DSperate's options for this game only", NIB_VALUE_TEXT, host_->save_per_game() ? "Yes" : "No");
}

void ShelfMenu::add_setting_rows(const ds::sdl::Setting* table) {
  for (const ds::sdl::Setting* s = table; s && s->key; ++s) {
    if (s->type == ds::sdl::Setting::Type::Text) continue;   // no keyboard here: the ini keeps them
    if (options_ && declared_ini(s->key)) continue;          // DSlot's settings have it (add_declared_rows)
    if (!host_->enabled(*s)) continue;
    std::string v = host_->get(s->key);
    if (v.empty()) v = default_value(*s);
    std::string shown = s->type == ds::sdl::Setting::Type::Bool ? (v == "true" || v == "1" ? "On" : "Off") : sentence(display_value(*s, v));
    row({Row::Setting, s}, sentence(s->label).c_str(), NIB_VALUE_TEXT, shown.c_str());
  }
}

std::string ShelfMenu::scope(bool game) const {
  const std::string console = shelf_ && shelf_->gba() ? "GBA" : "NDS";
  return game && shelf_ ? console + "/" + shelf_->game_title() : console;
}

// A declared option's row as DSlot shows it: its value where this page sets
// it, else muted, saying what applies instead ("Use NDS (2x)").
void ShelfMenu::add_declared_rows(bool game) {
  if (!options_ || !settings_) return;
  const std::string rom = shelf_ ? shelf_->game_title() : std::string();
  const int here = game ? 3 : 2;
  for (int i = 0; i < nib_options_count(options_); ++i) {
    int level = 0;
    const char* v = nib_option_value(options_, i, settings_, game ? rom.c_str() : nullptr, &level);
    const std::string label = nib_option_label(options_, i);
    if (level == here && v) {
      const std::string shown = nib_option_show(options_, i, v);
      row({Row::Declared, nullptr, i}, label.c_str(), NIB_VALUE_TEXT, shown.c_str());
    } else {
      const std::string inherit = nib_option_inherit(options_, i, settings_, game ? rom.c_str() : nullptr);
      row({Row::Declared, nullptr, i}, label.c_str(), NIB_VALUE_INHERITED, inherit.c_str());
    }
  }
}

// The first step from a row this page doesn't set lands on the option's
// default, as DSlot's own rows do; after that, one value on or back.
void ShelfMenu::step_declared(int option, bool game, int dir) {
  if (!options_ || !settings_) return;
  const std::string rom = shelf_ ? shelf_->game_title() : std::string();
  int level = 0;
  const char* v = nib_option_value(options_, option, settings_, game ? rom.c_str() : nullptr, &level);
  const std::string current = v ? v : "";
  std::string next;
  if (level == (game ? 3 : 2)) next = nib_option_step(options_, option, current.c_str(), dir);
  else if (const char* d = nib_option_default(options_, option)) next = d;
  else next = nib_option_step(options_, option, nullptr, dir);
  if (nib_option_set(options_, option, settings_, scope(game).c_str(), next.c_str()) != 0) return;
  if (nib_settings_save(settings_) != 0) message("DSlot's settings could not be saved");
  apply_declared(option);
}

void ShelfMenu::use_declared(int option, bool game) {
  if (!options_ || !settings_) return;
  nib_settings_reset(settings_, scope(game).c_str(), nib_option_key(options_, option));
  if (nib_settings_save(settings_) != 0) message("DSlot's settings could not be saved");
  apply_declared(option);
}

void ShelfMenu::apply_declared(int option) {
  if (!host_ || !options_ || !settings_) return;
  const std::string key = nib_option_key(options_, option);
  const char* ini = ini_key_of(key.c_str());
  if (!ini) return;
  const std::string rom = shelf_ ? shelf_->game_title() : std::string();
  int level = 0;
  const char* v = nib_option_value(options_, option, settings_, rom.c_str(), &level);
  std::string value = v && level > 0 ? std::string(v) : std::string();
  if (value.empty())   // set nowhere: DSperate's own, its table's default
    for (const ds::sdl::Setting* t : {ds::sdl::kVideoSettings, ds::sdl::kEmuSettings})
      for (const ds::sdl::Setting* s = t; s->key; ++s) if (std::strcmp(s->key, ini) == 0) value = default_value(*s);
  if (!value.empty()) host_->apply_now(ini, value);
}

void ShelfMenu::fill_console() {
  const bool gba = shelf_ && shelf_->gba();
  if (options_) {
    row({Row::Heading}, gba ? "Every GBA game" : "Every DS game", NIB_VALUE_NONE);
    add_declared_rows(false);
  }
  if (!host_) return;
  row({Row::Heading}, "Screens", NIB_VALUE_NONE);
  add_setting_rows(ds::sdl::kVideoSettings);
  add_setting_rows(ds::sdl::kLayoutSettings);
  row({Row::Heading}, "Emulation", NIB_VALUE_NONE);
  add_setting_rows(ds::sdl::kEmuSettings);
  if (!gba) {
    row({Row::Heading}, "The DS's own settings", NIB_VALUE_NONE);
    add_setting_rows(ds::sdl::kUserSettings);
  }
  row({Row::Heading}, "Buttons", NIB_VALUE_NONE);
  row({Row::Controls}, "Controls", NIB_VALUE_MORE, host_->has_pad() ? "Pad" : "Keyboard");
}

// What DSlot's System page has that matters mid-game. Device volume and
// brightness where the system lets an app have them; else DSperate's volume.
void ShelfMenu::fill_system() {
  const int vol = nib_volume_get();
  row({Row::Volume}, "Volume", NIB_VALUE_SLIDER, nullptr, vol >= 0 ? vol : shelf_ ? shelf_->volume() : 100);
  const int top = nib_backlight_get(0), bottom = nib_backlight_get(1);
  if (top >= 0) row({Row::Brightness, nullptr, 0}, "Top screen brightness", NIB_VALUE_SLIDER, nullptr, top);
  if (bottom >= 0) row({Row::Brightness, nullptr, 1}, "Bottom screen brightness", NIB_VALUE_SLIDER, nullptr, bottom);
  row({Row::Clock}, "Clock", NIB_VALUE_TEXT, nib_clock_hour12() ? "12-hour" : "24-hour");
}

void ShelfMenu::fill_cheats() {
  if (!codes_) return;
  int last = -2;
  for (size_t i = 0; i < codes_->size(); ++i) {
    const cheat::Code& c = (*codes_)[i];
    if (c.group != last) {
      if (c.group >= 0 && groups_ && static_cast<size_t>(c.group) < groups_->size())
        row({Row::Heading}, (*groups_)[static_cast<size_t>(c.group)].name.c_str(), NIB_VALUE_NONE);
      last = c.group;
    }
    if (c.is_note()) row({Row::Heading}, c.name.c_str(), NIB_VALUE_NONE);
    else row({Row::Cheat, nullptr, static_cast<int>(i)}, c.name.c_str(), NIB_VALUE_TEXT, c.enabled ? "On" : "Off");
  }
}

void ShelfMenu::fill_achievements() {
  if (!cheevos_) return;
  row({Row::Heading}, sentence(cheevos_->status()).c_str(), NIB_VALUE_NONE);
  using O = CheevosHost::Option;
  row({Row::CheevoOption, nullptr, static_cast<int>(O::Toasts)}, "Show unlocks", NIB_VALUE_TEXT, cheevos_->option(O::Toasts) ? "On" : "Off");
  row({Row::CheevoOption, nullptr, static_cast<int>(O::Screenshot)}, "Screenshot each unlock", NIB_VALUE_TEXT, cheevos_->option(O::Screenshot) ? "On" : "Off");
  row({Row::CheevoOption, nullptr, static_cast<int>(O::Encore)}, "Encore (earn them again)", NIB_VALUE_TEXT, cheevos_->option(O::Encore) ? "On" : "Off");
  if (!cheevos_->has_set()) return;
  row({Row::Heading}, sentence(cheevos_->progress()).c_str(), NIB_VALUE_NONE);
  for (int i = 0; i < cheevos_->row_count(); ++i) {
    const CheevosHost::Row r = cheevos_->row(i);
    const std::string v = r.unsupported ? std::string("Not here") : r.unlocked ? std::string("Earned") : std::to_string(r.points) + " pts";
    row({Row::Info, nullptr, i}, r.title.c_str(), r.unlocked ? NIB_VALUE_TEXT : NIB_VALUE_INHERITED, v.c_str());
  }
}

void ShelfMenu::fill_controls() {
  if (!host_) return;
  const bool pad = host_->has_pad();
  for (int i = 0; i < host_->binding_count(pad); ++i) {
    const SettingsHost::Binding b = host_->binding(pad, i);
    const bool listening = capturing_ && capture_row_ == static_cast<int>(rows_.size());
    row({Row::Binding, nullptr, i}, sentence(b.label).c_str(), listening ? NIB_VALUE_PROMPT : NIB_VALUE_TEXT,
        listening ? "Press a button" : b.value.c_str());
  }
  row({Row::BindReset}, "Back to the defaults", bind_reset_armed_ ? NIB_VALUE_WARNING : NIB_VALUE_NONE, bind_reset_armed_ ? "A again" : nullptr);
}

std::string ShelfMenu::hint() const {
  if (!msg_.empty()) return msg_;
  if (capturing_) return "Press the button for it   \xc2\xb7   B cancel";
  switch (sub_) {
  case Sub::Cheats: return "A on / off   \xc2\xb7   D-pad move   \xc2\xb7   B back";
  case Sub::Achievements: return "D-pad change   \xc2\xb7   B back";
  case Sub::Controls: return "A set   \xc2\xb7   D-pad move   \xc2\xb7   B back";
  case Sub::None: break;
  }
  switch (tab_) {
  case Play: return "A choose   \xc2\xb7   D-pad move   \xc2\xb7   L / R page   \xc2\xb7   B resume";
  case States:
    if (save_armed_ >= 0) return "X again to save over slot " + std::to_string(save_armed_ + 1);
    return "A load   \xc2\xb7   X save here   \xc2\xb7   Y quick slot   \xc2\xb7   L / R page   \xc2\xb7   B back";
  default: break;
  }
  // A setting's note, when the highlighted row has one.
  const int at = nib_menu_row_at(m_);
  if (at >= 0 && static_cast<size_t>(at) < rows_.size() && rows_[static_cast<size_t>(at)].kind == Row::Declared) {
    const char* above = tab_ == Game ? (shelf_ && shelf_->gba() ? "GBA" : "DS") : "default";
    return std::string("D-pad change   \xc2\xb7   Y use ") + above + "   \xc2\xb7   L / R page   \xc2\xb7   B back";
  }
  if (at >= 0 && static_cast<size_t>(at) < rows_.size() && rows_[static_cast<size_t>(at)].kind == Row::Setting) {
    const ds::sdl::Setting* s = rows_[static_cast<size_t>(at)].setting;
    if (s->flags & ds::sdl::FlagRestart) return "Takes effect when the game next starts";
    if (s->note && *s->note) return sentence(s->note);
  }
  return "D-pad change   \xc2\xb7   L / R page   \xc2\xb7   B back";
}

void ShelfMenu::fill() {
  rows_.clear();
  std::string title = shelf_ ? shelf_->game_title() : std::string();
  for (char& c : title) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
  switch (sub_) {
  case Sub::Cheats: title = "CHEATS"; break;
  case Sub::Achievements: title = "ACHIEVEMENTS"; break;
  case Sub::Controls: title = "CONTROLS"; break;
  case Sub::None: break;
  }
  // The hint reads the highlight, so it goes in after the rows: fill once
  // for the rows, then again with the hint they call for.
  std::string h;
  for (int pass = 0; pass < 2; ++pass) {
    if (pass) h = hint();
    rows_.clear();
    nib_menu_begin(m_, title.c_str(), h.c_str());
    fill_tabs();
    switch (sub_) {
    case Sub::Cheats: fill_cheats(); break;
    case Sub::Achievements: fill_achievements(); break;
    case Sub::Controls: fill_controls(); break;
    case Sub::None:
      switch (tab_) {
      case Play: fill_play(); break;
      case States: fill_states(); break;
      case Game: fill_game(); break;
      case Console: fill_console(); break;
      case System: fill_system(); break;
      }
    }
    int charging = 0;
    int battery = nib_battery(&charging);
    bool host_charging = false;
    if (battery < 0 && shelf_) { battery = shelf_->battery(host_charging); charging = host_charging; }
    const char* nclock = nib_menu_clock(m_);
    const std::string clock = nclock && *nclock ? std::string(nclock) : shelf_ ? shelf_->clock() : std::string();
    const bool gba = shelf_ && shelf_->gba();
    nib_menu_status(m_, clock.c_str(), battery, charging, gba ? 2 : 1, gba ? 560.0f : 0.0f);
    // A page just opened: its highlight, once its rows are there to move on.
    if (pass == 0 && pending_row_ >= 0) { nib_menu_set_row(m_, pending_row_); pending_row_ = -1; }
  }
}

const std::vector<u8>* ShelfMenu::picture(int slot, int& w, int& h) {
  auto it = pics_.find(slot);
  if (it == pics_.end()) {
    std::vector<u8> px; int pw = 0, ph = 0;
    if (!shelf_ || !shelf_->slot_picture(slot, px, pw, ph)) px.clear();
    it = pics_.emplace(slot, std::move(px)).first;
    pic_w_[slot] = pw; pic_h_[slot] = ph;
  }
  if (it->second.empty()) return nullptr;
  w = pic_w_[slot]; h = pic_h_[slot];
  return &it->second;
}

int ShelfMenu::top_slot() const {
  if (!open_ || tab_ != States || sub_ != Sub::None) return -1;
  const int at = nib_menu_card_at(m_);
  if (at < 0 || static_cast<size_t>(at) >= cards_.size()) return -1;
  return cards_[static_cast<size_t>(at)];
}

const u8* ShelfMenu::render(int& w, int& h) {
  fill();
  return nib_menu_render(m_, &w, &h);
}

const u8* ShelfMenu::render_top(const u32* game, int& w, int& h) {
  const int s = top_slot();
  if (s >= 0) {
    int pw = 0, ph = 0;
    const std::vector<u8>* pic = used_[s] ? picture(s, pw, ph) : nullptr;
    const std::string name = slot_name(s);
    const std::string saved = used_[s] && shelf_ ? shelf_->slot_saved(s) : std::string();
    const char* labels[] = {"Saved"};
    const char* values[] = {saved.c_str()};
    const std::string under = used_[s] ? (s == slot_ ? std::string("Quick slot") : std::string()) : std::string("Empty");
    return nib_top_state(m_, pic ? pic->data() : nullptr, pw, ph, name.c_str(), under.c_str(), labels, values, saved.empty() ? 0 : 1, &w, &h);
  }
  game_rgba_.resize(static_cast<size_t>(256) * 192 * 4);
  for (size_t i = 0; i < static_cast<size_t>(256) * 192; ++i) {
    const u32 c = game[i];
    game_rgba_[i * 4 + 0] = static_cast<u8>(c >> 16);
    game_rgba_[i * 4 + 1] = static_cast<u8>(c >> 8);
    game_rgba_[i * 4 + 2] = static_cast<u8>(c);
    game_rgba_[i * 4 + 3] = 255;
  }
  const std::string name = shelf_ ? shelf_->game_title() : std::string();
  return nib_top_paused(m_, game_rgba_.data(), 256, 192, net_ ? "Menu" : "Paused", name.c_str(), &w, &h);
}

PauseMenu::Result ShelfMenu::update(u32 presses, u32 held, u32 ms) {
  if (!msg_.empty()) {
    if (msg_ms_ <= ms) { msg_.clear(); dirty_ = true; } else msg_ms_ -= ms;
  }
  if (capturing_ && host_) {
    if (presses & (1u << Btn::BTN_B) && !host_->capturing()) { capturing_ = false; dirty_ = true; return Result::None; }
    const std::string got = host_->take_capture();
    if (!got.empty()) {
      const SettingsHost::Binding b = host_->binding(host_->has_pad(), rows_[static_cast<size_t>(capture_row_)].at);
      host_->bind(b.key, got);
      capturing_ = false; dirty_ = true;
    }
    return Result::None;
  }
  // A held direction repeats after a moment, as DSlot's do.
  int repeat = -1;
  for (int b : {static_cast<int>(Btn::BTN_UP), static_cast<int>(Btn::BTN_DOWN), static_cast<int>(Btn::BTN_LEFT), static_cast<int>(Btn::BTN_RIGHT)})
    if (held & (1u << b)) { repeat = b; break; }
  if (repeat != repeat_key_ || (presses & (1u << repeat))) { repeat_key_ = repeat; repeat_ms_ = 0; repeating_ = false; }
  else if (repeat >= 0) {
    repeat_ms_ += ms;
    const u32 wait = repeating_ ? kRepeatRateMs : kRepeatDelayMs;
    if (repeat_ms_ >= wait) { repeat_ms_ -= wait; repeating_ = true; presses |= 1u << repeat; }
  }
  Result out = Result::None;
  for (int b = 0; b < Btn::BTN_COUNT; ++b) {
    if (!(presses & (1u << b))) continue;
    const int key = key_of(b);
    if (key < 0) continue;
    if (rows_.empty() && cards_.empty()) fill();   // nib needs the page before it can move on it
    const NibEffect e = nib_menu_key(m_, key);
    dirty_ = true;
    const Result r = effect(e.kind, e.index, e.step, e.key);
    if (r != Result::None) out = r;
  }
  return out;
}

PauseMenu::Result ShelfMenu::touch(float x, float y) {
  if (capturing_) return Result::None;
  const NibEffect e = nib_menu_touch(m_, x, y);
  dirty_ = true;
  return effect(e.kind, e.index, e.step, e.key);
}

PauseMenu::Result ShelfMenu::effect(int kind, int index, int step, int key) {
  switch (kind) {
  case NIB_EFFECT_PAGE: go_tab(index); return Result::None;
  case NIB_EFFECT_BACK:
    if (sub_ != Sub::None) { sub_ = Sub::None; pending_row_ = row_of_tab_[tab_]; return Result::None; }
    if (tab_ != Play) { go_tab(Play); return Result::None; }
    return Result::Resume;
  case NIB_EFFECT_CHOOSE: return choose(index);
  case NIB_EFFECT_CHANGE: change(index, step); return Result::None;
  case NIB_EFFECT_CARD: return card(index, key);
  case NIB_EFFECT_USE:
    if (index >= 0 && static_cast<size_t>(index) < rows_.size() && rows_[static_cast<size_t>(index)].kind == Row::Declared)
      use_declared(rows_[static_cast<size_t>(index)].at, tab_ == Game);
    return Result::None;
  default: return Result::None;
  }
}

PauseMenu::Result ShelfMenu::choose(int index) {
  if (index < 0 || static_cast<size_t>(index) >= rows_.size()) return Result::None;
  const Row r = rows_[static_cast<size_t>(index)];
  if (r.kind != Row::Reset) reset_armed_ = false;
  if (r.kind != Row::BindReset) bind_reset_armed_ = false;
  switch (r.kind) {
  case Row::Resume: return Result::Resume;
  case Row::SaveState: case Row::LoadState: {
    go_tab(States);
    // The quick slot's card highlighted, so A or X acts on it straight away.
    fill();
    for (size_t i = 0; i < cards_.size(); ++i) if (cards_[i] == slot_) nib_menu_set_card(m_, static_cast<int>(i));
    return Result::None;
  }
  case Row::FastForward: return Result::FastForward;
  case Row::Reset:
    if (!reset_armed_) { reset_armed_ = true; return Result::None; }
    reset_armed_ = false;
    return Result::Reset;
  case Row::SaveQuit: return Result::Quit;
  case Row::SaveForGame: if (host_) host_->set_save_per_game(!host_->save_per_game()); return Result::None;
  case Row::Setting: case Row::Declared: case Row::Clock: change(index, 1); return Result::None;
  case Row::Cheats: row_of_tab_[tab_] = index; sub_ = Sub::Cheats; pending_row_ = 0; return Result::None;
  case Row::Achievements: row_of_tab_[tab_] = index; sub_ = Sub::Achievements; pending_row_ = 0; return Result::None;
  case Row::Controls: row_of_tab_[tab_] = index; sub_ = Sub::Controls; pending_row_ = 0; return Result::None;
  case Row::Cheat: toggle_cheat(r.at); return Result::None;
  case Row::CheevoOption: change(index, 1); return Result::None;
  case Row::Binding:
    if (host_) { host_->begin_capture(host_->has_pad()); capturing_ = true; capture_row_ = index; }
    return Result::None;
  case Row::BindReset:
    if (!bind_reset_armed_) { bind_reset_armed_ = true; return Result::None; }
    bind_reset_armed_ = false;
    if (host_) host_->reset_bindings(host_->has_pad());
    return Result::None;
  default: return Result::None;
  }
}

void ShelfMenu::change(int index, int step) {
  if (index < 0 || static_cast<size_t>(index) >= rows_.size()) return;
  const Row r = rows_[static_cast<size_t>(index)];
  switch (r.kind) {
  case Row::Setting: {
    if (!host_) return;
    std::string v = host_->get(r.setting->key);
    if (v.empty()) v = default_value(*r.setting);
    host_->set(r.setting->key, step_value(*r.setting, v, step, *host_));
    return;
  }
  case Row::FastForward: (void)step; return;   // a press of A or a tap flips it: one effect for both
  case Row::SaveForGame: if (host_) host_->set_save_per_game(!host_->save_per_game()); return;
  case Row::Cheat: toggle_cheat(r.at); return;
  case Row::CheevoOption: {
    const auto o = static_cast<CheevosHost::Option>(r.at);
    if (cheevos_) cheevos_->set_option(o, !cheevos_->option(o));
    return;
  }
  case Row::Declared: step_declared(r.at, tab_ == Game, step); return;
  case Row::Volume: {
    const int vol = nib_volume_get();
    if (vol >= 0) nib_volume_set(std::max(0, std::min(100, vol + step * 10)));
    else if (shelf_) shelf_->set_volume(std::max(0, std::min(100, shelf_->volume() + step * 10)));
    return;
  }
  case Row::Brightness: {
    const int b = nib_backlight_get(r.at);
    if (b >= 0) nib_backlight_set(r.at, std::max(5, std::min(100, b + step * 5)));
    return;
  }
  case Row::Clock: nib_clock_set_hour12(nib_clock_hour12() ? 0 : 1); return;
  default: return;
  }
}

PauseMenu::Result ShelfMenu::card(int index, int key) {
  if (index < 0 || static_cast<size_t>(index) >= cards_.size()) return Result::None;
  const int s = cards_[static_cast<size_t>(index)];
  if (key != NIB_KEY_X) save_armed_ = -1;
  switch (key) {
  case NIB_KEY_A:
    if (!used_[s]) { message("Nothing saved in " + slot_name(s) + " yet"); return Result::None; }
    slot_ = s;
    return Result::Load;
  case NIB_KEY_X:
    if (s == kAutoSlot) { message("The resume point is saved by Save and quit"); return Result::None; }
    if (used_[s] && save_armed_ != s) { save_armed_ = s; return Result::None; }
    save_armed_ = -1;
    slot_ = s;
    message("Saved to slot " + std::to_string(s + 1));
    return Result::Save;
  case NIB_KEY_Y:
    if (s == kAutoSlot) return Result::None;
    slot_ = s;
    message("Quick slot is now " + std::to_string(s + 1));
    return Result::None;
  default: return Result::None;
  }
}

void ShelfMenu::toggle_cheat(int at) {
  if (!codes_ || at < 0 || static_cast<size_t>(at) >= codes_->size()) return;
  cheat::Code& c = (*codes_)[static_cast<size_t>(at)];
  c.enabled = !c.enabled;
  cheats_dirty_ = true;
  // In a group marked as alternatives, turning one on turns the rest off.
  if (!c.enabled || c.group < 0 || !groups_ || static_cast<size_t>(c.group) >= groups_->size()) return;
  if (!(*groups_)[static_cast<size_t>(c.group)].exclusive) return;
  for (size_t i = 0; i < codes_->size(); ++i)
    if (i != static_cast<size_t>(at) && (*codes_)[i].group == c.group) (*codes_)[i].enabled = false;
}

} // namespace ds::sdl
