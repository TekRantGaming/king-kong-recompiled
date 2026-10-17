  // --------------------------------------------------------------- setup: the last steps ---
  // Setup steps 9 and 10 (codex/state/setup-flow.md, job J5), after the graphics pages, in the launcher's
  // game-asset look (AssetSetup): "Choose your button prompts" (SetupStage 12) and "You're all set"
  // (SetupStage 13). Included inside class Launcher (launcher.cpp).
  //
  // Both pages are a column on the left (the logo, a heading, cards, then the page's menu) and a panel on the
  // right. Arrows move between the cards, Down from the last row goes to the menu and Up from its first entry
  // back to the cards; A or a click uses a card; B goes back a page.

  static constexpr int kPromptsStage = 12;
  bool EndSetupStage(int stage) const { return stage == kPromptsStage; }

  bool setup_prompts_ = false;  // setup step 9: the button prompts (SetupStage 12)
  bool end_on_cards_ = true;    // the cards have the focus (else the menu under them)
  int end_card_focus_ = -1;     // -1: set from the page's selected card as it comes up
  int end_menu_focus_ = 0;
  int end_page_ = -1;           // the page the focus was set up for

  struct EndCard {
    std::string label, caption;  // the label as given (brand names stay), the caption in English
    bool selected = false;
    std::function<void(ImVec2, ImVec2, bool)> art;  // draws the card's picture between the two points
    std::function<void()> pick;
  };

  // The left column's top: the logo, "PC PORT · SETUP · <step>", the heading and what the page is for.
  // Returns where the cards start.
  float DrawEndHeader(ImDrawList* dl, const char* step, const char* title, const std::string& intro, float x0,
                      float width) {
    const float s = S();
    const ui::Fonts f = ui::GetFonts();
    float y = o_.y + std::max(40 * s, h_ * 0.06f);
    if (logo_) {
      const float lh = 72 * s, lw = lh * logo_aspect_;
      dl->AddImage(ui::Tex(logo_), ImVec2(x0 - 6 * s, y - 8 * s), ImVec2(x0 - 6 * s + lw, y - 8 * s + lh));
      y += lh;
    }
    ui::DrawTracked(dl, f.display, 13 * s, ImVec2(x0 + 2 * s, y), color::kTextDim,
                    Upper(T("PC port")) + "  " KK_DOT "  " + Upper(T("Setup")) + "  " KK_DOT "  " + Upper(T(step)),
                    3 * s);
    y += 30 * s;
    dl->AddText(f.display, 30 * s, ImVec2(x0, y), color::kText, T(title));
    y += 44 * s;
    dl->AddText(f.text, 15 * s, ImVec2(x0, y), color::kTextDim, intro.c_str(), nullptr, width);
    return y + ui::TextSize(f.text, 15 * s, intro, width).y + 20 * s;
  }

  // The cards, `cols` to a row; returns the height used. Handles the arrows, A and the mouse while the cards
  // have the focus (the menu takes Up and Down otherwise).
  float DrawEndCards(ImDrawList* dl, std::vector<EndCard>& cards, int cols, float x0, float y, float width, float ch) {
    const float s = S();
    const ui::Fonts f = ui::GetFonts();
    const int n = int(cards.size());
    const int page = SetupStage();
    if (end_page_ != page) {  // a page coming up: the focus on its selected card
      end_page_ = page;
      end_on_cards_ = true;
      end_menu_focus_ = 0;
      end_card_focus_ = 0;
      for (int i = 0; i < n; ++i)
        if (cards[size_t(i)].selected) end_card_focus_ = i;
    }
    end_card_focus_ = std::clamp(end_card_focus_, 0, n - 1);
    const float gap = 12 * s, r = 8 * s;
    const float cw = (width - gap * float(cols - 1)) / float(cols);
    const int rows = (n + cols - 1) / cols;
    auto pick = [&](int i) {
      ui::Cue(ui::Sound::kSelect);
      REXLOG_INFO("KK: setup: step {} card {}", page == kPromptsStage ? 9 : 10, cards[size_t(i)].label);
      cards[size_t(i)].pick();
    };
    if (end_on_cards_ && !modal_.open) {
      const int before = end_card_focus_, col = end_card_focus_ % cols, row = end_card_focus_ / cols;
      if (input_.Pressed(Action::kLeft) && col > 0) --end_card_focus_;
      if (input_.Pressed(Action::kRight) && col < cols - 1 && end_card_focus_ + 1 < n) ++end_card_focus_;
      if (input_.Pressed(Action::kUp) && row > 0) end_card_focus_ -= cols;
      if (input_.Pressed(Action::kDown)) {
        if (row + 1 < rows) {
          end_card_focus_ = std::min(n - 1, end_card_focus_ + cols);
        } else {  // on to the menu
          end_on_cards_ = false;
          end_menu_focus_ = 0;
          ui::Cue(ui::Sound::kMove);
        }
      }
      if (end_card_focus_ != before) ui::Cue(ui::Sound::kMove);
      if (input_.Pressed(Action::kAccept)) pick(end_card_focus_);
      for (Action a : {Action::kLeft, Action::kRight, Action::kAccept, Action::kDown, Action::kUp}) input_.Consume(a);
    }
    for (int i = 0; i < n; ++i) {
      EndCard& c = cards[size_t(i)];
      const ImVec2 p0(x0 + float(i % cols) * (cw + gap), y + float(i / cols) * (ch + gap)), p1(p0.x + cw, p0.y + ch);
      const bool hovered = !modal_.open && ImGui::IsMouseHoveringRect(p0, p1, false);
      if (hovered && (input_.mouse_moved() || ImGui::IsMouseClicked(0))) {
        if (!end_on_cards_ || end_card_focus_ != i) ui::Cue(ui::Sound::kMove);
        end_on_cards_ = true;
        end_card_focus_ = i;
      }
      if (hovered && ImGui::IsMouseClicked(0)) pick(i);
      const bool focused = end_on_cards_ && end_card_focus_ == i, on = focused || hovered;
      dl->AddRectFilled(ImVec2(p0.x, p0.y + 4 * s), ImVec2(p1.x, p1.y + 4 * s), IM_COL32(0, 0, 0, 40), r);
      dl->AddRectFilled(p0, p1, ui::WithAlpha(c.selected ? IM_COL32(34, 37, 44, 255) : IM_COL32(16, 18, 22, 255),
                                              c.selected || on ? 0.86f : 0.62f), r);
      dl->AddRect(p0, p1, ui::WithAlpha(c.selected ? color::kAccent : color::kText, c.selected ? 0.9f : on ? 0.3f : 0.10f),
                  r, 0, (c.selected ? 1.5f : 1.0f) * s);
      if (c.selected) dl->AddRectFilled(ImVec2(p0.x + r, p0.y), ImVec2(p1.x - r, p0.y + 3 * s), color::kAccent);
      if (focused)
        dl->AddRect(ImVec2(p0.x - 3 * s, p0.y - 3 * s), ImVec2(p1.x + 3 * s, p1.y + 3 * s),
                    ui::WithAlpha(color::kFocus, 0.9f), r + 3 * s, 0, 2.0f * s);
      const float pad = 12 * s, room = cw - pad * 2;
      const float text_h = 16 * s + 6 * s + 12.5f * s;
      if (c.art) c.art(ImVec2(p0.x + pad, p0.y + pad), ImVec2(p1.x - pad, p1.y - pad - text_h - 8 * s), on);
      // The label smaller when it is long ("Keyboard and mouse"), shortened only if it still does not fit.
      const float lw = ui::TextSize(f.semibold, 16 * s, T(c.label)).x;
      const float lfs = lw > room ? std::max(13 * s, 16 * s * room / lw) : 16 * s;
      const std::string label = ui::Ellipsize(f.semibold, lfs, T(c.label), room);
      const float ly = p1.y - pad - text_h;
      dl->AddText(f.semibold, lfs, ImVec2(p0.x + pad, ly + (16 * s - lfs) * 0.6f), color::kText, label.c_str());
      const std::string sub = ui::Ellipsize(f.text, 12.5f * s, T(c.caption), room - (c.selected ? 14 * s : 0));
      dl->AddText(f.text, 12.5f * s, ImVec2(p0.x + pad, ly + 22 * s), c.selected ? color::kAccent : color::kTextDim,
                  sub.c_str());
      if (c.selected)  // the chosen card's mark
        dl->AddCircleFilled(ImVec2(p1.x - pad - 3 * s, ly + 30 * s), 4.5f * s, color::kAccent, 16);
    }
    return float(rows) * ch + float(rows - 1) * gap;
  }

  // The page's menu under the cards: Up from its first entry goes back to the cards; B goes back a page.
  void DrawEndMenu(ImDrawList* dl, std::vector<HomeEntry>& entries, float x0, float y, std::function<void()> back) {
    if (end_on_cards_ && !modal_.open && input_.Pressed(Action::kBack)) {
      input_.Consume(Action::kBack);
      ui::Cue(ui::Sound::kBack);
      return back();
    }
    if (!end_on_cards_ && !modal_.open && end_menu_focus_ == 0 && input_.Pressed(Action::kUp)) {
      input_.Consume(Action::kUp);
      end_on_cards_ = true;
      ui::Cue(ui::Sound::kMove);
    }
    side_focus_ = -1;
    const float s = S();
    if (end_on_cards_ && !modal_.open && input_.mouse_moved() &&
        ImGui::IsMouseHoveringRect(ImVec2(x0 - 12 * s, y), ImVec2(x0 + 420 * s, y + 42 * s * float(entries.size())), false))
      end_on_cards_ = false;
    if (const int i = DrawMenu(dl, entries, end_menu_focus_, x0, y, 1.0f, 0, !end_on_cards_); i >= 0)
      entries[size_t(i)].action();
    if (!end_on_cards_ && !modal_.open && input_.Pressed(Action::kBack)) {
      input_.Consume(Action::kBack);
      ui::Cue(ui::Sound::kBack);
      back();
    }
  }

  std::vector<ui::Prompt> EndPrompts() const {
    return {{Action::kAccept, "Select"}, {Action::kBack, "Back"}};
  }

  // ------------------------------------------------------------------- step 9: button prompts ---
  struct PromptStyle {
    const char* value;  // kk_button_prompts
    const char* name;
  };
  static constexpr PromptStyle kPromptStyles[] = {
      {"auto", "Automatic"},          {"xbox360", "Xbox 360"},     {"xbox_series", "Xbox Series"},
      {"ps5", "PlayStation 5"},       {"ps2", "PlayStation 2"},    {"steam_deck", "Steam Deck"},
      {"keyboard", "Keyboard and mouse"}};

  static const char* PromptStyleName(const std::string& value) {
    for (const auto& p : kPromptStyles)
      if (value == p.value) return p.name;
    return "Xbox 360";
  }
  // Automatic's style now: the controller last used, else the first connected one, else the Xbox 360's.
  std::string AutoPromptStyle() const {
    return input_.pad_style().empty() ? DetectedPadPrompts() : input_.pad_style();
  }
  // The card that is chosen: Keyboard and mouse while Input is Keyboard & mouse.
  std::string ChosenPromptStyle() const {
    const std::string p = Get("kk_button_prompts");
    if (GetBool("mnk_mode") || p == "keyboard") return "keyboard";
    for (const auto& s : kPromptStyles)
      if (p == s.value) return p;
    return "auto";
  }

  void PickPromptStyle(const std::string& value) {
    if (value == "keyboard") {
      SetInputMode(true);  // the keys in the game's hints, the mouse turns the camera (as Controls > Input)
    } else {
      if (GetBool("mnk_mode") || Get("kk_button_prompts") == "keyboard") SetInputMode(false);
      Set("kk_button_prompts", value);
    }
    REXLOG_INFO("KK: setup: button prompts {} (kk_button_prompts={} mnk_mode={})", value, Get("kk_button_prompts"),
                Get("mnk_mode"));
  }

  // A button's picture in a style: a/b/x/y/lb/rb/lt/rt; for the keyboard, the first key bound to that button
  // that has a picture (Settings > Controls > Keyboard bindings), else `key` names it for a plain box.
  rex::ui::ImmediateTexture* StyleGlyph(const std::string& style, const std::string& button, float* aspect,
                                        std::string* key) {
    if (style != "keyboard") {
      if (auto* t = glyphs_.Named(style, button, aspect)) return t;
      return glyphs_.Named("xbox_series", button, aspect);
    }
    static const std::pair<const char*, const char*> kBinds[] = {
        {"a", "keybind_a"}, {"b", "keybind_b"}, {"x", "keybind_x"}, {"y", "keybind_y"},
        {"lb", "keybind_left_shoulder"}, {"rb", "keybind_right_shoulder"},
        {"lt", "keybind_left_trigger"}, {"rt", "keybind_right_trigger"}};
    std::string binds;
    for (const auto& [b, cvar] : kBinds)
      if (button == b) binds = Get(cvar);
    std::stringstream in(binds);
    bool first = true;
    for (std::string k; std::getline(in, k, ',');) {
      if (const size_t plus = k.rfind('+'); plus != std::string::npos) k = k.substr(plus + 1);
      k.erase(0, k.find_first_not_of(' '));
      k.erase(k.find_last_not_of(' ') + 1);
      if (k == "LMB") k = "Mouse_Left";
      else if (k == "RMB") k = "Mouse_Right";
      else if (k == "MMB") k = "Mouse_Middle";
      if (k.empty()) continue;
      if (first && key) *key = k;
      first = false;
      if (auto* t = glyphs_.Named("keyboard", k, aspect)) return t;
    }
    return nullptr;
  }

  // One button picture `h` tall, centred on (cx, cy); its key's name in a box if there is no picture.
  // Returns its width.
  float DrawStyleGlyph(ImDrawList* dl, const std::string& style, const std::string& button, float cx, float cy,
                       float h, bool draw = true, float alpha = 1.0f) {
    float aspect = 1.0f;
    std::string key;
    if (auto* t = StyleGlyph(style, button, &aspect, &key)) {
      const float w = h * aspect;
      if (draw)
        dl->AddImage(ui::Tex(t), ImVec2(cx - w * 0.5f, cy - h * 0.5f), ImVec2(cx + w * 0.5f, cy + h * 0.5f),
                     ImVec2(0, 0), ImVec2(1, 1), ui::WithAlpha(IM_COL32_WHITE, alpha));
      return w;
    }
    const ui::Fonts f = ui::GetFonts();
    const float fs = h * 0.5f;
    const std::string label = key.empty() ? Upper(button) : key;
    const float w = std::max(h, ui::TextSize(f.semibold, fs, label).x + h * 0.5f);
    if (draw) {
      const ImVec2 p0(cx - w * 0.5f, cy - h * 0.5f), p1(cx + w * 0.5f, cy + h * 0.5f);
      dl->AddRectFilled(p0, p1, ui::WithAlpha(color::kFocus, 0.9f * alpha), h * 0.2f);
      const ImVec2 ts = ui::TextSize(f.semibold, fs, label);
      dl->AddText(f.semibold, fs, ImVec2(cx - ts.x * 0.5f, cy - ts.y * 0.5f), ui::WithAlpha(color::kOnFocus, alpha),
                  label.c_str());
    }
    return w;
  }

  // The four face buttons of a style as on the controller (Y top, X left, B right, A bottom); the keyboard's
  // four keys in a row.
  void DrawFaceButtons(ImDrawList* dl, const std::string& style, ImVec2 p0, ImVec2 p1, float alpha) {
    const float cx = (p0.x + p1.x) * 0.5f, cy = (p0.y + p1.y) * 0.5f;
    const float room = std::min(p1.x - p0.x, p1.y - p0.y);
    if (style == "keyboard") {
      const float h = std::min(room * 0.62f, (p1.x - p0.x) / 5.2f);
      const char* const keys[] = {"a", "b", "x", "y"};
      float widths[4], total = 0;
      for (int i = 0; i < 4; ++i) total += widths[i] = DrawStyleGlyph(dl, style, keys[i], 0, 0, h, false);
      const float gap = h * 0.18f;
      total += gap * 3;
      float x = cx - total * 0.5f;
      for (int i = 0; i < 4; ++i) {
        DrawStyleGlyph(dl, style, keys[i], x + widths[i] * 0.5f, cy, h, true, alpha);
        x += widths[i] + gap;
      }
      return;
    }
    const float h = room * 0.36f, d = room * 0.32f;
    DrawStyleGlyph(dl, style, "y", cx, cy - d, h, true, alpha);
    DrawStyleGlyph(dl, style, "x", cx - d, cy, h, true, alpha);
    DrawStyleGlyph(dl, style, "b", cx + d, cy, h, true, alpha);
    DrawStyleGlyph(dl, style, "a", cx, cy + d, h, true, alpha);
  }

  // A line of text with button pictures in place of {0}, {1}... (the English is translated first); returns the
  // height used. Pictures are `gh` tall, the text `fs`.
  float DrawGlyphLine(ImDrawList* dl, const char* english, const std::string& style,
                      std::initializer_list<const char*> buttons, float x, float y, float fs, float gh, ImU32 col) {
    const ui::Fonts f = ui::GetFonts();
    const std::vector<const char*> list(buttons);
    const std::string text = T(english);
    const float cy = y + gh * 0.5f;
    size_t at = 0;
    while (at < text.size()) {
      const size_t open = text.find('{', at);
      const std::string part = text.substr(at, open == std::string::npos ? std::string::npos : open - at);
      if (!part.empty()) {
        dl->AddText(f.text, fs, ImVec2(x, cy - fs * 0.62f), col, part.c_str());
        x += ui::TextSize(f.text, fs, part).x;
      }
      if (open == std::string::npos) break;
      const size_t close = text.find('}', open);
      const int index = std::atoi(text.c_str() + open + 1);
      if (index >= 0 && index < int(list.size())) {
        const float w = DrawStyleGlyph(dl, style, list[size_t(index)], 0, 0, gh, false);
        DrawStyleGlyph(dl, style, list[size_t(index)], x + 3 * S() + w * 0.5f, cy, gh);
        x += w + 6 * S();
      }
      at = close == std::string::npos ? text.size() : close + 1;
    }
    return gh;
  }

  void DrawSetupPrompts(ImDrawList* dl) {
    const float s = S();
    const ui::Fonts f = ui::GetFonts();
    const float x0 = o_.x + Margin(), right = o_.x + w_ - Margin();
    const float list_w = std::round(std::min(660 * s, (right - x0) * 0.5f));
    const float top = o_.y + std::max(40 * s, h_ * 0.06f);
    float y = DrawEndHeader(dl, "Button prompts", "Choose your button prompts",
                            T(std::string("Pick which buttons the game and this launcher show in menus and hints.")),
                            x0, list_w);

    const std::string chosen = ChosenPromptStyle(), automatic = AutoPromptStyle();
    std::vector<EndCard> cards;
    for (const auto& p : kPromptStyles) {
      EndCard c;
      const std::string value = p.value;
      c.label = p.name;
      c.caption = value == "auto" ? F("Now: {0}", {T(std::string(PromptStyleName(automatic)))})
                  : value == "keyboard" ? std::string("Mouse camera")
                                        : std::string();
      c.selected = chosen == value;
      const std::string shown = value == "auto" ? automatic : value;
      c.art = [this, dl, shown](ImVec2 a, ImVec2 b, bool on) { DrawFaceButtons(dl, shown, a, b, on ? 1.0f : 0.85f); };
      c.pick = [this, value] { PickPromptStyle(value); };
      cards.push_back(std::move(c));
    }
    y += DrawEndCards(dl, cards, 4, x0, y, list_w, 116 * s) + 26 * s;

    std::vector<HomeEntry> entries;
    entries.push_back({"Next", true, [this] { FinishSetupPrompts(); }});
    entries.push_back({"Back", true, [this] { BackFromPrompts(); }});
    DrawEndMenu(dl, entries, x0, y, [this] { BackFromPrompts(); });

    // The right side: a download's strip (when one runs), then the preview of the focused (or chosen) style.
    const float rx0 = x0 + list_w + 56 * s, rx1 = right;
    float ry = top;
    if (DrawContentStrip(dl, 1.0f, true)) ry = o_.y + std::max(84 * s, h_ * 0.12f) + 66 * s + 18 * s;
    const int shown_i = end_on_cards_ ? std::clamp(end_card_focus_, 0, int(std::size(kPromptStyles)) - 1) : -1;
    const std::string value = shown_i >= 0 ? kPromptStyles[shown_i].value : chosen;
    const std::string style = value == "auto" ? automatic : value;
    const float pw = rx1 - rx0, ph = 300 * s, r = 10 * s, pad = 22 * s;
    const ImVec2 p0(rx0, ry), p1(rx1, ry + ph);
    dl->AddRectFilled(ImVec2(p0.x, p0.y + 5 * s), ImVec2(p1.x, p1.y + 5 * s), IM_COL32(0, 0, 0, 50), r);
    dl->AddRectFilled(p0, p1, ui::WithAlpha(IM_COL32(14, 16, 20, 255), 0.82f), r);
    dl->AddRect(p0, p1, ui::WithAlpha(color::kText, 0.12f), r, 0, 1.0f * s);
    float py = p0.y + pad;
    ui::DrawTracked(dl, f.display, 12 * s, ImVec2(p0.x + pad, py), color::kAccent, Upper(T("Preview")), 2.6f * s);
    py += 22 * s;
    const std::string name = value == "auto" ? F("Automatic ({0})", {T(std::string(PromptStyleName(automatic)))})
                                             : std::string(T(PromptStyleName(value)));
    dl->AddText(f.display, 24 * s, ImVec2(p0.x + pad, py), color::kText, name.c_str());
    py += 40 * s;
    // The face buttons big, and the shoulder buttons and triggers beside them.
    const float art_h = 116 * s;
    DrawFaceButtons(dl, style, ImVec2(p0.x + pad, py), ImVec2(p0.x + pad + (style == "keyboard" ? 300 * s : art_h), py + art_h), 1.0f);
    if (style != "keyboard") {
      float sx = p0.x + pad + art_h + 40 * s;
      const float gh = 34 * s;
      for (const char* b : {"lb", "rb", "lt", "rt"}) {
        const float w = DrawStyleGlyph(dl, style, b, 0, 0, gh, false);
        if (sx + w > p1.x - pad) break;
        DrawStyleGlyph(dl, style, b, sx + w * 0.5f, py + art_h * 0.5f, gh);
        sx += w + 14 * s;
      }
    }
    py += art_h + 22 * s;
    DrawGlyphLine(dl, "Press {0} to select and {1} to go back.", style, {"a", "b"}, p0.x + pad, py, 16 * s, 28 * s,
                  color::kText);
    py += 38 * s;
    DrawGlyphLine(dl, "In play, Kong grabs with {0} and jumps with {1}.", style, {"x", "b"}, p0.x + pad, py, 16 * s,
                  28 * s, color::kTextDim);
    (void)pw;

    // Under the panel: what the focused card does, and that everything can be remapped later.
    const std::string about =
        value == "auto"       ? T(std::string("Shows the buttons of the controller you are using, and changes when you "
                                              "switch controllers."))
        : value == "keyboard" ? T(std::string("Plays with a keyboard and mouse: the game's hints show the keys and the "
                                              "mouse turns the camera."))
                              : F("Always shows the {0} buttons.", {T(std::string(PromptStyleName(value)))});
    float dy = p1.y + 22 * s;
    dl->AddText(f.text, 15 * s, ImVec2(rx0, dy), color::kTextDim, about.c_str(), nullptr, pw);
    dy += ui::TextSize(f.text, 15 * s, about, pw).y + 14 * s;
    const std::string remap =
        T(std::string("Controller buttons and keyboard keys can be remapped any time in Settings > Controls."));
    dl->AddText(f.text, 14.5f * s, ImVec2(rx0, dy), color::kTextFaint, remap.c_str(), nullptr, pw);

#if defined(KK_DEV_TOOLS)
    {
      static std::string logged;
      const std::string now = (end_on_cards_ ? "card " + std::string(kPromptStyles[std::clamp(end_card_focus_, 0, 6)].name)
                                             : "menu " + std::to_string(end_menu_focus_)) +
                              " chosen " + chosen + " preview " + style;
      if (now != logged) {
        logged = now;
        REXLOG_INFO("KK dev: setup step 9 focus {}", now);
      }
    }
#endif
  }

  void BackFromPrompts() {
    setup_prompts_ = false;
    setup_graphics_ = true;
    GfxGo(kGfxPages - 1);
    REXLOG_INFO("KK: setup: back from the button prompts to the graphics pages");
  }

  void FinishSetupPrompts() {
    setup_prompts_ = false;
#if defined(KK_DEV_TOOLS)
    if (dev_setup_ == kPromptsStage) dev_setup_ = -1;
#endif
    REXLOG_INFO("KK: setup: button prompts done: kk_button_prompts={} mnk_mode={}", Get("kk_button_prompts"),
                Get("mnk_mode"));
    page_ = kHome;
    home_focus_ = 0;
    if (!InSetup()) Status(InstalledMessage());
  }
