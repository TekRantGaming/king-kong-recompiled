  // ------------------------------------------------------------- New in this update (job J6) ---
  // Players who had the first-time setup on an older build (owner, 17 Oct, DECISIONS "Setup details"; spec in
  // codex/state/setup-flow.md) get ONE screen, on the first launcher start after updating, with what the new
  // setup offers: the HD texture pack (Download / I already have one / Not now), the restored PS2 cutscenes
  // (Yes, restore them / No thanks; only where PS2 movies play) and a button to the setup's graphics pages 1 to 4
  // (which come back here), then Continue. In the launcher's game-asset look (AssetSetup), SetupStage 14.
  // Included inside class Launcher (launcher.cpp).
  //
  // kk_setup_version remembers it: the constructor sets "waiting" when an older build ran last (a fresh install
  // has no settings file and gets the full setup instead, StartFullSetup), and Continue sets this version, so it
  // is never shown again (also not after the next update, unless a later version asks for a screen of its own).
  // Closing the launcher on it shows it again next time. What's new waits until it is dismissed
  // (WhatsNewGate::new_screen) and then opens once on the main menu. Downloads behave as from Settings: they ask
  // first, then run in the background, and the screen shows how far they are.
  //
  // Keyboard, mouse and controller: Up / Down move between the rows of buttons, Left / Right along a row, A /
  // Enter / a click use a button. B does nothing here (Continue is the way out, so nothing is skipped by mistake).

  static constexpr int kNewStage = 14;
  static constexpr const char* kSetupWaiting = "waiting";

  bool update_new_ = false;  // the New in this update screen is up (SetupStage 14)
  int new_row_id_ = -1;      // the focused row: 0 texture pack, 1 PS2 cutscenes, 2 graphics, 3 Continue (-1: first)
  int new_col_ = 0;
  int new_tex_answer_ = -1;  // what was pressed for the texture pack: 0 Download, 1 I already have one, 2 Not now
  int new_cut_answer_ = -1;  // for the cutscenes: 0 Yes, 1 No thanks

  struct NewButton {
    const char* label;  // English, through T()
    bool selected;      // the answer already given
    std::function<void()> press;
  };
  struct NewSection {
    int id;
    const char* caption;      // English
    std::string text;         // translated
    std::string status;       // translated; empty: none
    bool status_good = false; // installed / restored
    std::vector<NewButton> buttons;
  };

  void OpenNewGraphics() {
    setup_graphics_ = true;
    gfx_from_new_ = true;
    gfx_entered_ = false;
    gfx_on_cards_ = false;
    gfx_cards_page_ = -1;
    GfxGo(0);
    REXLOG_INFO("KK: New in this update: the graphics pages");
  }

  void BackToNewScreen() {
    setup_graphics_ = false;
    gfx_entered_ = false;
    gfx_from_new_ = false;
    gfx_on_cards_ = false;
    LogGraphicsChoices("graphics pages left (back to New in this update)");
#if defined(KK_DEV_TOOLS)
    if (dev_setup_ >= 0) dev_setup_ = kNewStage;
#endif
    REXLOG_INFO("KK: New in this update: back from the graphics pages");
  }

  void DismissNewScreen() {
    update_new_ = false;
#if defined(KK_DEV_TOOLS)
    if (dev_setup_ == kNewStage) dev_setup_ = -1;
#endif
    Set("kk_setup_version", KK_VERSION);
    SaveSettings(paths_.config_path);
    page_ = kHome;
    home_focus_ = 0;
    REXLOG_INFO("KK: New in this update: dismissed (kk_setup_version={}; texture pack answer {}, cutscenes answer {}); "
                "main menu{}",
                Get("kk_setup_version"), new_tex_answer_, new_cut_answer_,
                open_whats_new_ ? ", What's new opens on it" : "");
  }

  std::vector<NewSection> NewSections() {
    using namespace content;
    std::vector<NewSection> out;
    auto& service = Service::Get();
    const bool ready = service.Ready();
    {
      NewSection sec{0, "HD texture pack"};
      const Info& info = InfoFor(Kind::kTexturePack);
      sec.text = F("Sharper versions of the game's textures, made at four times their original size. A separate "
                   "download of {0} GB that you can turn on or off at any time in Settings > Graphics.",
                   {Gb(info.zip_size)});
      const content::Status st = ready ? service.Get(Kind::kTexturePack) : content::Status{};
      const bool other = ready && service.Get(Kind::kCutscenes).Busy();
      if (st.installed) {
        sec.status = st.imported ? F("Installed: your texture pack ({0})", {st.installed_name})
                                 : T(std::string("Installed"));
        sec.status_good = true;
      } else if (st.Busy() || st.stage == Stage::kPaused) {
        sec.status = ContentProgressLine(st, other);
        sec.buttons.push_back({"Watch progress", false, [this] { OpenContentProgress(Kind::kTexturePack, 0); }});
      } else {
        if (!Get("native_texture_pack_folder").empty()) sec.status = T(std::string("You use a texture pack folder of your own."));
        if (new_tex_answer_ == 2) sec.status = T(std::string("Not now. You can get it later in Settings > Graphics."));
        sec.buttons.push_back({"Download", new_tex_answer_ == 0, [this] {
                                 new_tex_answer_ = 0;
                                 REXLOG_INFO("KK: New in this update: texture pack Download");
                                 AskContentDownload(Kind::kTexturePack, 0);
                               }});
        sec.buttons.push_back({"I already have one", new_tex_answer_ == 1, [this] {
                                 new_tex_answer_ = 1;
                                 REXLOG_INFO("KK: New in this update: texture pack I already have one");
                                 ChooseImportZip(content::ImportTarget::kTexturePack, 0);
                               }});
        sec.buttons.push_back({"Not now", new_tex_answer_ == 2, [this] {
                                 new_tex_answer_ = 2;
                                 REXLOG_INFO("KK: New in this update: texture pack Not now");
                               }});
      }
      out.push_back(std::move(sec));
    }
    if (movies::PlaybackAvailable()) {  // only where the PS2 movies can play (as the setup's cutscene page)
      NewSection sec{1, "PS2 cutscenes"};
      const Info& info = InfoFor(Kind::kCutscenes);
      sec.text = F("The PS2 version has cutscenes between some missions that the Xbox 360 version left out. "
                   "Restore them as upscaled videos? A separate download of {0} GB.",
                   {Gb(info.zip_size)});
      const content::Status st = ready ? service.Get(Kind::kCutscenes) : content::Status{};
      const bool other = ready && service.Get(Kind::kTexturePack).Busy();
      if (st.installed) {
        sec.status = T(std::string("Restored"));
        sec.status_good = true;
      } else if (st.Busy() || st.stage == Stage::kPaused) {
        sec.status = ContentProgressLine(st, other);
        sec.buttons.push_back({"Watch progress", false, [this] { OpenContentProgress(Kind::kCutscenes, 0); }});
      } else {
        if (new_cut_answer_ == 1) sec.status = T(std::string("No thanks. You can restore them later in Settings > Advanced."));
        sec.buttons.push_back({"Yes, restore them", new_cut_answer_ == 0, [this] {
                                 new_cut_answer_ = 0;
                                 REXLOG_INFO("KK: New in this update: PS2 cutscenes Yes");
                                 AskContentDownload(Kind::kCutscenes, 0);
                               }});
        sec.buttons.push_back({"No thanks", new_cut_answer_ == 1, [this] {
                                 new_cut_answer_ = 1;
                                 REXLOG_INFO("KK: New in this update: PS2 cutscenes No thanks");
                               }});
      }
      out.push_back(std::move(sec));
    }
    {
      NewSection sec{2, "Graphics"};
      sec.text = T(std::string("New graphics options: display and frame rate, upscaling and sharpness, lighting and "
                               "effects, and the Original look. Your settings stay as they are until you change them."));
      sec.buttons.push_back({"See the new graphics options", false, [this] { OpenNewGraphics(); }});
      out.push_back(std::move(sec));
    }
    {
      NewSection sec{3, ""};
      sec.buttons.push_back({"Continue", false, [this] { DismissNewScreen(); }});
      out.push_back(std::move(sec));
    }
    return out;
  }

  void DrawUpdateNew(ImDrawList* dl) {
    const float s = S();
    const ui::Fonts f = ui::GetFonts();
    const float x0 = o_.x + Margin(), right = o_.x + w_ - Margin();
    const float list_w = std::round(std::min(700 * s, (right - x0) * 0.56f));
    side_focus_ = -1;
    std::vector<NewSection> secs = NewSections();
    const float top = o_.y + std::max(40 * s, h_ * 0.06f);
    float y = DrawEndHeader(dl, "New", "New in this update",
                            T(std::string("This update adds a few things you can set up now. Everything here is also in "
                                          "Settings, so you can leave it for later.")),
                            x0, list_w, "Update");

    // The focus: a row (a section with buttons) and a button along it.
    std::vector<int> rows;  // indexes into secs
    for (int i = 0; i < int(secs.size()); ++i)
      if (!secs[size_t(i)].buttons.empty()) rows.push_back(i);
    int row = 0;
    for (int r = 0; r < int(rows.size()); ++r)
      if (secs[size_t(rows[size_t(r)])].id == new_row_id_) row = r;
    const int before_row = row, before_col = new_col_;
    int chosen_sec = -1, chosen_btn = -1;
    if (!modal_.open) {
      if (input_.Pressed(Action::kUp) && row > 0) --row;
      if (input_.Pressed(Action::kDown) && row + 1 < int(rows.size())) ++row;
      if (row != before_row) new_col_ = std::min(new_col_, int(secs[size_t(rows[size_t(row)])].buttons.size()) - 1);
      const int n = int(secs[size_t(rows[size_t(row)])].buttons.size());
      new_col_ = std::clamp(new_col_, 0, n - 1);
      if (input_.Pressed(Action::kLeft) && new_col_ > 0) --new_col_;
      if (input_.Pressed(Action::kRight) && new_col_ + 1 < n) ++new_col_;
      if (input_.Pressed(Action::kAccept)) chosen_sec = rows[size_t(row)], chosen_btn = new_col_;
      if (input_.Pressed(Action::kBack)) ui::Cue(ui::Sound::kDeny);
      for (Action a : {Action::kUp, Action::kDown, Action::kLeft, Action::kRight, Action::kAccept, Action::kBack})
        input_.Consume(a);
    }
    new_row_id_ = secs[size_t(rows[size_t(row)])].id;

    // The sections: a caption with the state at the right, a sentence, the buttons (wrapped when long).
    const float bh = 40 * s, gap = 10 * s, r = 6 * s;
    for (int i = 0; i < int(secs.size()); ++i) {
      NewSection& sec = secs[size_t(i)];
      if (sec.id == 3) y += 2 * s;  // Continue, a little apart
      if (*sec.caption) {
        ui::DrawTracked(dl, f.display, 12 * s, ImVec2(x0 + 2 * s, y), color::kAccent, Upper(T(sec.caption)), 2.6f * s);
        if (!sec.status.empty()) {
          const float cw = ui::TrackedWidth(f.display, 12 * s, Upper(T(sec.caption)), 2.6f * s) + 18 * s;
          const std::string st = ui::Ellipsize(f.semibold, 13 * s, sec.status, list_w - cw);
          dl->AddText(f.semibold, 13 * s, ImVec2(x0 + cw, y - 1 * s), sec.status_good ? color::kAccent : color::kTextDim,
                      st.c_str());
        }
        y += 22 * s;
      }
      if (!sec.text.empty()) {
        dl->AddText(f.text, 14.5f * s, ImVec2(x0, y), color::kTextDim, sec.text.c_str(), nullptr, list_w);
        y += ui::TextSize(f.text, 14.5f * s, sec.text, list_w).y + 12 * s;
      }
      float bx = x0;
      const bool primary = sec.id == 3;
      for (int b = 0; b < int(sec.buttons.size()); ++b) {
        const NewButton& btn = sec.buttons[size_t(b)];
        const std::string label = T(btn.label);
        const float fs = (primary ? 16 : 15) * s, h = primary ? 48 * s : bh;
        const float w = std::max(primary ? 260 * s : 120 * s, ui::TextSize(f.semibold, fs, label).x + 44 * s);
        if (bx > x0 && bx + w > x0 + list_w) {  // on to the next line
          bx = x0;
          y += h + gap;
        }
        const ImVec2 p0(bx, y), p1(bx + w, y + h);
        const bool hovered = !modal_.open && ImGui::IsMouseHoveringRect(p0, p1, false);
        if (hovered && (input_.mouse_moved() || ImGui::IsMouseClicked(0))) {
          new_row_id_ = sec.id;
          new_col_ = b;
        }
        if (hovered && ImGui::IsMouseClicked(0)) chosen_sec = i, chosen_btn = b;
        const bool focused = new_row_id_ == sec.id && new_col_ == b;
        dl->AddRectFilled(ImVec2(p0.x, p0.y + 3 * s), ImVec2(p1.x, p1.y + 3 * s), IM_COL32(0, 0, 0, 40), r);
        if (focused) {
          dl->AddRectFilled(p0, p1, hovered ? IM_COL32_WHITE : color::kFocus, r);
        } else {
          dl->AddRectFilled(p0, p1, ui::WithAlpha(IM_COL32(16, 18, 22, 255), hovered ? 0.86f : 0.70f), r);
          dl->AddRect(p0, p1, btn.selected ? color::kAccent : ui::WithAlpha(color::kText, hovered ? 0.30f : 0.14f), r, 0,
                      (btn.selected ? 1.5f : 1.0f) * s);
        }
        const ImVec2 ts = ui::TextSize(f.semibold, fs, label);
        dl->AddText(f.semibold, fs, ImVec2(p0.x + (w - ts.x) * 0.5f, p0.y + (h - ts.y) * 0.5f),
                    focused ? color::kOnFocus : btn.selected ? color::kAccent : color::kText, label.c_str());
        bx += w + gap;
        if (b + 1 == int(sec.buttons.size())) y += h;
      }
      y += 18 * s;
    }
    if (new_row_id_ != secs[size_t(rows[size_t(before_row)])].id || new_col_ != before_col) ui::Cue(ui::Sound::kMove);

    // The right side: the focused download's facts, else one of the game's stills; a running download's strip.
    const int focus_id = new_row_id_;
    if (focus_id == 0 || focus_id == 1) {
      DrawContentCard(dl, focus_id == 0 ? content::Kind::kTexturePack : content::Kind::kCutscenes);
    } else if (!slides_.empty() && slides_[0].sharp) {
      const float rx0 = x0 + list_w + 56 * s, pw = right - rx0;
      const float ry = top + 72 * s;
      const float ph = std::min(pw * 9.0f / 16.0f, h_ * 0.5f);
      if (pw > 120 * s && ph > 60 * s) {
        const float aspect = slides_[0].aspect > 0 ? slides_[0].aspect : 16.0f / 9.0f, want = pw / ph;
        float u0 = 0, u1 = 1, v0 = 0, v1 = 1;
        if (want > aspect) {
          const float v = aspect / want;
          v0 = (1 - v) * 0.5f, v1 = v0 + v;
        } else {
          const float u = want / aspect;
          u0 = (1 - u) * 0.5f, u1 = u0 + u;
        }
        const ImVec2 p0(rx0, ry), p1(right, ry + ph);
        dl->AddRectFilled(ImVec2(p0.x, p0.y + 5 * s), ImVec2(p1.x, p1.y + 5 * s), IM_COL32(0, 0, 0, 50), 10 * s);
        dl->AddImageRounded(ui::Tex(slides_[0].sharp), p0, p1, ImVec2(u0, v0), ImVec2(u1, v1), IM_COL32_WHITE, 10 * s);
        dl->AddRect(p0, p1, ui::WithAlpha(color::kText, 0.12f), 10 * s, 0, 1.0f * s);
      }
    }
    DrawContentStrip(dl, 1.0f, false);

#if defined(KK_DEV_TOOLS)
    {
      static std::string logged;
      const std::string now = std::to_string(new_row_id_) + ":" + std::to_string(new_col_) + " y " +
                              std::to_string(int((y - o_.y) / std::max(h_, 1.0f) * 100)) + "%";
      if (now != logged) {
        logged = now;
        REXLOG_INFO("KK dev: New in this update focus {}", now);
      }
    }
#endif
    if (chosen_sec >= 0 && chosen_btn >= 0 && !modal_.open) {
      NewSection& sec = secs[size_t(chosen_sec)];
      if (chosen_btn < int(sec.buttons.size())) {
        ui::Cue(ui::Sound::kSelect);
        sec.buttons[size_t(chosen_btn)].press();
      }
    }
  }

  std::vector<ui::Prompt> NewPrompts() const { return {{Action::kAccept, "Select"}}; }
