  // ------------------------------------------------------------------ setup graphics pages ---
  // Setup steps 5 to 8 (codex/state/setup-flow.md, job J4): four pages after the PS2 cutscene step with the
  // graphics options the original game did not have, in the launcher's game-asset look (AssetSetup): the logo,
  // a heading, the options as Settings rows (the same rows and settings as Settings > Graphics and Display,
  // built by the same code: one source of truth), the graphics API and the upscaler as cards, and on the right
  // a close-up of the game with the option off (Original, left) and on (New, right) and a slider between them
  // (dragged with the mouse, or LT and RT). Every page has "Use recommended" and says the options can be
  // changed later in Settings. Included inside class Launcher (launcher.cpp).
  //
  // The pictures are made at run time from stills of the player's own copy of the game (setup_compare.h): nothing
  // game-derived is in git or in the release package (owner, 18 Oct; the shipped folder of screenshots is gone).
  // Options whose effect shows on a still get a made pair; the rest show one still and say the game shows them.

  static constexpr int kGfxFirstStage = 8, kGfxPages = 4;
  static constexpr const char* kGfxTitles[kGfxPages] = {"Display", "Upscaling and sharpness", "Lighting and effects",
                                                        "Look"};
  static constexpr const char* kGfxIntros[kGfxPages] = {
      "Choose how the game fills your screen and how smoothly it runs.",
      "Choose how the picture is scaled up to your screen and how sharp it looks.",
      "New lighting effects made for this PC port.",
      "Keep the new look, or play the game as it looked on the Xbox 360."};
  static constexpr const char* kChangeLater = "You can change this any time in Settings.";
  static constexpr const char* kAbove30Note =
      "King Kong was made to run at 30 FPS, and above that some character animations can look wrong, such as the "
      "crew rowing the boat at the start.";

  bool setup_graphics_ = false;  // setup steps 5-8 (SetupStage 8 to 11)
  int gfx_page_ = 0;
  bool gfx_entered_ = false;     // the settings were logged as the pages first came up
  bool gfx_from_new_ = false;    // opened from New in this update: Next on the last page and Back on the first go back to it
  ui::ListState gfx_lists_[kGfxPages];
  bool gfx_on_cards_ = false;    // the page's cards (graphics API, upscaler) have the focus
  int gfx_card_focus_ = 0;
  int gfx_cards_page_ = -1;      // the page the card focus was set up for
  float gfx_split_ = 0.5f;       // the comparison's slider, 0 (all New) to 1 (all Original)
  bool gfx_dragging_ = false;
  std::string gfx_last_key_;     // the close-up shown while an action row has the focus
  struct CompareArt {
    rex::ui::ImmediateTexture* original = nullptr;  // a pair, with the slider
    rex::ui::ImmediateTexture* fresh = nullptr;
    rex::ui::ImmediateTexture* still = nullptr;     // or one picture from the game
    const char* left = "Original";
    const char* right = "New";
    const char* note = nullptr;  // what the picture shows (English; drawn translated)
  };
  std::map<std::string, CompareArt> compare_art_;  // made from the game files (setup_compare.h); cleared with the art
  std::vector<art::Image> setup_stills_;
  bool setup_stills_loaded_ = false;

  bool GraphicsSetupStage(int stage) const { return stage >= kGfxFirstStage && stage < kGfxFirstStage + kGfxPages; }
  int GfxPage() const { return std::clamp(SetupStage() - kGfxFirstStage, 0, kGfxPages - 1); }

  void GfxGo(int page) {
    gfx_page_ = page;
    gfx_split_ = 0.5f;
    gfx_dragging_ = false;
    gfx_cards_page_ = -1;
#if defined(KK_DEV_TOOLS)
    if (dev_setup_ >= 0) dev_setup_ = kGfxFirstStage + page;
#endif
    REXLOG_INFO("KK: setup: graphics page {} ({})", page + 1, kGfxTitles[page]);
  }

  // A Settings row, moved out of a page Settings built, with the setup's one plain sentence.
  static bool TakeRow(std::vector<Item>& from, std::string_view label, const char* sentence, std::vector<Item>& to) {
    for (Item& it : from) {
      if (it.type == ItemType::kSection || it.label != label) continue;
      it.description = sentence;
      to.push_back(std::move(it));
      return true;
    }
    return false;
  }

  static Item SetupToggle(const char* label, const char* sentence, bool on, std::function<void(bool)> set,
                          bool default_on) {
    Item it;
    it.type = ItemType::kChoice;
    it.label = label;
    it.description = sentence;
    it.options = {"Off", "On"};
    it.index = on ? 1 : 0;
    it.default_text = default_on ? "On" : "Off";
    it.on_choice = [set](int i) { set(i == 1); };
    it.on_default = [set, default_on] { set(default_on); };
    return it;
  }

  static std::string FlagDefault(const char* name) {
    const auto* flag = FindFlag(name);
    return flag ? flag->default_value : std::string();
  }

  // Frame rate cap: 30, 60, 120, 144, 165, Match my screen, Unlimited (kk_frame_rate, as Display's Frame rate).
  Item SetupFrameRateItem() {
    int screen_hz = 0;
    const auto mode = ExclusiveFullscreen() ? ParseDisplayMode(Get("kk_fullscreen_mode"))
                                            : ParseDisplayMode(DesktopModeValue());
    if (mode) screen_hz = int(std::lround(mode->refresh));
    std::vector<Option> opts;
    bool matched = false;
    for (int fps : {30, 60, 120, 144, 165}) {
      if (!matched && screen_hz > 30 && screen_hz < fps) {  // the screen's own rate, in its place
        opts.push_back({F("Match my screen ({0} FPS)", {std::to_string(screen_hz)}), std::to_string(screen_hz)});
        matched = true;
      }
      std::string label = fps == 30 ? "30 FPS (Xbox 360)" : std::to_string(fps) + " FPS";
      if (fps == screen_hz) {
        label = F("Match my screen ({0} FPS)", {std::to_string(fps)});
        matched = true;
      }
      opts.push_back({label, std::to_string(fps)});
    }
    if (!matched && screen_hz > 165)
      opts.push_back({F("Match my screen ({0} FPS)", {std::to_string(screen_hz)}), std::to_string(screen_hz)});
    opts.push_back({"Unlimited", "0"});
    const std::string cur = Get("kk_frame_rate");
    bool listed = false;
    for (const auto& o : opts) listed = listed || o.value == cur;
    if (!listed && !cur.empty()) opts.push_back({F("{0} FPS (settings file)", {cur}), cur});
    Item it = CvarChoice("Frame rate", "The most pictures the game draws each second.", "kk_frame_rate", opts);
    it.default_text = "60 FPS";  // Remastered's (the owner's choice); Original keeps 30
    it.on_default = [] { SetInt("kk_frame_rate", 60); };
    if (GetInt("kk_frame_rate", 30) != 30) it.facts.push_back(kAbove30Note);
    return std::move(Disable(it, GetBool("kk_original_look"), kSetByOriginal));
  }

  // The rows of a page (the cards are separate: GfxCards).
  std::vector<Item> SetupGraphicsItems(int page) {
    const bool original = GetBool("kk_original_look");
    std::vector<Item> rows;
    if (page == 0) {
      std::vector<Item> display = DisplayItems();
      TakeRow(display, "Window mode", "How the game fills your screen: in a window, borderless or fullscreen.", rows);
      if (ExclusiveFullscreen()) {
        if (TakeRow(display, "Resolution", "The size your screen switches to while the game runs.", rows))
          rows.back().facts.insert(rows.back().facts.begin(), "The game is drawn at up to this size.");
        TakeRow(display, "Refresh rate", "How many times a second your screen refreshes while the game runs.", rows);
      } else if (GetBool("fullscreen")) {  // Borderless: the desktop's own mode
        // Task 363: the desktop's size or an ultrawide picture size (BorderlessResolutionItem).
        const auto desktop = ParseDisplayMode(DesktopModeValue());
        if (TakeRow(display, "Resolution",
                    "The size of the game's picture: your desktop's, or a wider one shown with black bars above and "
                    "below.",
                    rows))
          rows.back().facts.insert(rows.back().facts.begin(), "The game is drawn at up to this size.");
        Item hz;
        hz.type = ItemType::kInfo;
        hz.label = "Refresh rate";
        hz.description = "Borderless uses your desktop's refresh rate.";
        hz.value_text = desktop ? desktop->RateLabel() : std::string();
        rows.push_back(std::move(hz));
      } else if (TakeRow(display, "Window size", "The size of the game's window.", rows)) {
        rows.back().facts.insert(rows.back().facts.begin(), "The game is drawn at up to this size.");
      }
      rows.push_back(SetupFrameRateItem());
    } else if (page == 1) {
      std::vector<Item> graphics = GraphicsItems();
      if (GfxCards(1).empty())
        TakeRow(graphics, "Upscaler", "Scales the picture up to your screen and sharpens it.", rows);
      const std::string temporal = CurrentTemporalUpscaler();
      if (TakeRow(graphics, "Upscaler mode",
                  temporal.empty() ? "How many pixels the game draws before they are scaled up to your screen."
                                   : "How many pixels the game draws before the upscaler rebuilds the picture.",
                  rows) && !temporal.empty()) {
        // The sizes it draws at and rebuilds to, for each mode ("1280 x 720 to 1920 x 1080").
        const auto [out_w, out_h] = cb_.output_size ? cb_.output_size() : std::pair<int, int>{1280, 720};
        const auto [ow, oh] = UpscalerOutputFor(out_w, out_h, PictureAspect(out_w, out_h));  // ultrawide's shape too
        Item& it = rows.back();
        it.facts.clear();
        for (size_t m = 0; m < std::size(kTemporalModes) && m < it.options.size(); ++m) {
          const double r = kTemporalModes[m].ratio;
          const std::string sizes = F("{0} to {1}", {Size(int(std::lround(ow / r)), int(std::lround(oh / r))), Size(ow, oh)});
          it.facts.push_back(T(std::string(kTemporalModes[m].label)) + ": " + sizes);
          it.options[m] = std::string(T(kTemporalModes[m].label)) + "  " KK_DOT "  " + Size(int(std::lround(ow / r)), int(std::lround(oh / r)));
        }
      } else {
        TakeRow(graphics, "Render quality", "How sharply the game is drawn compared with your screen.", rows);
      }
      {
        const std::string aa_default = FlagDefault("swap_post_effect");
        Item it = SetupToggle("SMAA", "Smooths jagged edges after the picture is drawn.", Get("swap_post_effect") == "smaa",
                              [](bool on) { Set("swap_post_effect", on ? "smaa" : "none"); }, aa_default == "smaa");
        Disable(it, !temporal.empty() && !original, "The upscaler smooths edges itself, so SMAA is not used with it.");
        rows.push_back(std::move(Disable(it, original, kSetByOriginal)));
      }
      TakeRow(graphics, "Sharpness", "Makes fine detail crisper.", rows);
      {
        const std::string def = FlagDefault("anisotropic_override");
        Item it = SetupToggle("16x texture filtering", "Keeps the ground and distant textures sharp at steep angles.",
                              Get("anisotropic_override") == "5",
                              [def](bool on) { Set("anisotropic_override", on ? "5" : def == "5" || def.empty() ? "-1" : def); },
                              def == "5");
        rows.push_back(std::move(Disable(it, original, kSetByOriginal)));
      }
      // (No HDR row since Beta 14, owner 21 Oct: HDR is experimental, in the F8 graphics panel only.)
    } else if (page == 2) {
      std::vector<Item> graphics = GraphicsItems();
      TakeRow(graphics, "Global illumination", "Sunlight bounces off surfaces and softly lights the shade beside them.",
              rows);
      TakeRow(graphics, "Ambient occlusion", "Soft shading where surfaces meet, in corners and under rocks and grass.",
              rows);
      TakeRow(graphics, "Shadows", "Sharper, softer edged shadows than the game's own.", rows);
      TakeRow(graphics, "Water reflections", "The water reflects the rocks, trees and the ship around it.", rows);
      TakeRow(graphics, "Light shafts", "Sunlight shines through the mist and fog in visible shafts.", rows);
    } else {
      rows.push_back(SetupToggle("Original look", "Plays the game exactly as it looked on the Xbox 360.", original,
                                 [](bool on) {
                                   SetOriginalLook(on);
                                   REXLOG_INFO("KK: setup: Original look {}", on ? "on" : "off");
                                 }, false));
    }

    rows.push_back(Section(""));
    if (page == 3) {
      rows.push_back(ActionItem("Use recommended for my PC",
                                "Sets every graphics option to what suits your PC, with the new look.", "Apply",
                                [this] { ApplyGfxRecommended(-1); }));
    } else {
      Item rec = ActionItem("Use recommended", "Sets the options on this page to what suits your PC.", "Apply",
                            [this, page] { ApplyGfxRecommended(page); });
      rows.push_back(std::move(Disable(rec, original, kSetByOriginal)));
    }
    const bool last = page == kGfxPages - 1;
    rows.push_back(ActionItem(last && gfx_from_new_ ? "Done" : "Next",
                              !last ? "The next page." : gfx_from_new_ ? "Back to New in this update." : "Finishes the graphics setup.",
                              last ? "" : F("{0} of {1}", {std::to_string(page + 2), std::to_string(kGfxPages)}),
                              [this, page] {
                                if (page + 1 < kGfxPages) GfxGo(page + 1);
                                else FinishSetupGraphics();
                              }));
    if (page > 0)
      rows.push_back(ActionItem("Back", "The page before.", "", [this, page] { GfxGo(page - 1); }));
    else if (gfx_from_new_)
      rows.push_back(ActionItem("Back", "Back to New in this update.", "", [this] { BackToNewScreen(); }));
    return rows;
  }

  // "Use recommended": a page's options back to what suits this PC (the hardware recommendation's defaults and
  // Remastered's 60 FPS and Strong GTAO); -1: all of them and the new look (page 4's "for my PC"). The window
  // mode and resolution are the player's screen and stay.
  void ApplyGfxRecommended(int page) {
    const bool all = page < 0;
    if (all) ApplyRemastered();  // the new look, the hardware recommendation, 60 FPS, Strong GTAO
    if (all || page == 0) {
      SetInt("kk_frame_rate", 60);
      if (Get("native_backend") != "auto" && FindFlag("native_backend")) {
        startup::ManualChoice();
        Reset("native_backend");
        ApplyUpscalerDefaults();
      }
    }
    if (all || page == 1) {
      Reset("present_effect");
      if (HasTemporalSetting()) Reset("native_upscaler");
      for (const char* name : {"native_upscaler_quality", "kk_render_quality", "swap_post_effect", "native_sharpness",
                               "anisotropic_override"})
        if (FindFlag(name)) Reset(name);
    }
    if (all || page == 2) {
      for (const char* name : {"native_gi", "native_gi_distance", "ao_mode", "native_shadow_quality",
                               "native_water_reflections", "native_volumetrics"})
        if (FindFlag(name)) Reset(name);
      SetInt("ao_strength", 3);  // Strong: Remastered's
    }
    ++g_changes;
    REXLOG_INFO("KK: setup: recommended settings for {}", all ? std::string("every graphics page")
                                                              : "graphics page " + std::to_string(page + 1));
    Status(all ? "The recommended settings for your PC are set." : "The recommended settings for this page are set.");
  }

  void FinishSetupGraphics() {
    if (gfx_from_new_) return BackToNewScreen();
    setup_graphics_ = false;
    gfx_entered_ = false;
    setup_prompts_ = true;  // then setup step 9, the button prompts (launcher_setup_end.inc)
#if defined(KK_DEV_TOOLS)
    if (GraphicsSetupStage(dev_setup_)) dev_setup_ = kPromptsStage;
#endif
    LogGraphicsChoices("graphics pages done");
    page_ = kHome;
    home_focus_ = 0;
    if (!InSetup()) Status(InstalledMessage());
  }

  static void LogGraphicsChoices(const char* when) {
    std::string line;
    for (const char* name : {"fullscreen", "kk_fullscreen_mode", "kk_frame_rate", "native_backend", "native_upscaler",
                             "native_upscaler_quality", "present_effect", "kk_render_quality", "swap_post_effect",
                             "native_sharpness", "anisotropic_override", "present_hdr", "native_gi", "ao_mode",
                             "ao_strength", "native_shadow_quality", "native_water_reflections", "native_volumetrics",
                             "kk_original_look"})
      if (FindFlag(name)) line += std::string(line.empty() ? "" : " ") + name + "=" + Get(name);
    REXLOG_INFO("KK: setup: {}: {}", when, line);
  }

  // ----------------------------------------------------------------------------------------- cards ---
  struct GfxCard {
    std::string label, caption, text;  // English; text: the description panel's sentence
    bool enabled = true, selected = false;
    std::function<void()> pick;
  };

  std::vector<GfxCard> GfxCards(int page) const {
    std::vector<GfxCard> cards;
    if (page == 0) {
      if (!NativeRendererAvailable() || !UsingNativeRenderer() || !FindFlag("native_backend")) return cards;
      const std::string backend = Get("native_backend");
#if !defined(__linux__)
      // Task 350a (owner, 21 Oct): DirectX 12 is the recommended API on Windows, Vulkan the other choice and the
      // fallback when DirectX 12 cannot start.
      // Task 368: on Intel integrated graphics Vulkan is the recommended one (and the default).
      const bool intel = IntelIntegratedGraphics();
      GfxCard dx;
      dx.label = "DirectX 12";
      dx.caption = intel ? "Also available" : "Recommended";
      dx.text = intel ? "Also available. On this graphics card Vulkan is recommended."
                      : "The recommended graphics API on Windows. If it cannot start, the game uses Vulkan.";
      dx.selected = backend == "d3d12";
      dx.pick = [] {
        if (Get("native_backend") == "d3d12") return;
        startup::ManualChoice();
        Set("native_backend", "d3d12");
        ApplyUpscalerDefaults();
      };
      cards.push_back(std::move(dx));
#endif
      GfxCard vk;
      vk.label = "Vulkan";
#if defined(__linux__)
      vk.caption = "Recommended";
      vk.text = "The recommended graphics API. It works on every graphics card the game supports.";
#else
      vk.caption = intel ? "Recommended" : "If DirectX 12 has problems";
      vk.text = intel ? "The recommended graphics API. It works on every graphics card the game supports."
                      : "Works on every graphics card the game supports. Try it if DirectX 12 gives you problems.";
#endif
      vk.selected = backend == "auto" || backend == "vulkan";
      vk.pick = [] {
        if (Get("native_backend") != "d3d12") return;  // Auto is Vulkan
        startup::ManualChoice();
        Set("native_backend", "vulkan");
        ApplyUpscalerDefaults();
      };
      cards.push_back(std::move(vk));
      return cards;
    }
    if (page != 1 || !HasTemporalSetting() || !UsingNativeRenderer() || GetBool("kk_original_look")) return cards;
    const OfferedUpscalers offered = OfferedTemporalUpscalers();
    const std::string temporal = CurrentTemporalUpscaler(), best = DefaultTemporalUpscaler();
    auto pick_temporal = [](std::string value) {
      return [value] {
        Set("present_effect", "bilinear");
        SetTemporalUpscaler(value.c_str());
        Set("kk_render_quality", "native");  // the temporal upscalers take the picture at the screen's size
      };
    };
    auto card = [&](const char* label, std::string value, bool runs, const char* text) {
      GfxCard c;
      c.label = label;
      c.text = text;
      // Before the answer is in, the vendor's default can be picked (the game falls back to FSR 3.1 itself).
      c.enabled = offered.known ? runs : value == best;
      c.selected = temporal == value || (value == "fsr3" && temporal == "fsr4") || (value == "fsr4" && temporal == "fsr3");
      c.caption = !offered.known && !c.enabled ? "Checking"
                  : !c.enabled               ? "Not on this graphics card"
                  : value == best || (value.rfind("fsr", 0) == 0 && best.rfind("fsr", 0) == 0) ? "Recommended"
                  : value == "fsr4"          ? "FSR 4"
                  : value == "fsr3"          ? "FSR 3.1"
                                             : "Supported";
      c.pick = pick_temporal(value);
      cards.push_back(std::move(c));
    };
    card("NVIDIA DLSS", "dlss", offered.dlss, "Rebuilds a sharp picture from fewer pixels on NVIDIA RTX graphics cards.");
    card("AMD FSR", offered.fsr4 ? "fsr4" : "fsr3", offered.fsr3 || offered.fsr4,
         "Rebuilds a sharp picture from fewer pixels and runs on most graphics cards.");
    card("Intel XeSS", "xess", offered.xess, "Rebuilds a sharp picture from fewer pixels, best on Intel graphics cards.");
    GfxCard off;
    off.label = "Off";
    off.caption = "Plain scaling";
    off.text = "No upscaler: the game is drawn at up to your screen's size and scaled plainly.";
    off.selected = temporal.empty();
    off.pick = [] {
      Set("present_effect", "bilinear");
      SetTemporalUpscaler("off");
    };
    cards.push_back(std::move(off));
    return cards;
  }

  // The cards in a row of their own; returns the height used. Left and Right move between them, A or a click
  // picks one; Down goes on to the rows.
  float DrawGfxCards(ImDrawList* dl, std::vector<GfxCard>& cards, int page, float x0, float y, float width) {
    const float s = S();
    const ui::Fonts f = ui::GetFonts();
    const int n = int(cards.size());
    if (gfx_cards_page_ != page) {  // a page coming up: the focus on its cards, on the chosen one
      gfx_cards_page_ = page;
      gfx_on_cards_ = true;
      gfx_card_focus_ = 0;
      for (int i = 0; i < n; ++i)
        if (cards[size_t(i)].selected) gfx_card_focus_ = i;
    }
    gfx_card_focus_ = std::clamp(gfx_card_focus_, 0, n - 1);
    const char* caption = page == 0 ? "Graphics API" : "Upscaler";
    ui::DrawTracked(dl, f.display, 12 * s, ImVec2(x0 + 2 * s, y), color::kAccent, Upper(T(caption)), 2.6f * s);
    y += 24 * s;
    const float gap = 12 * s, ch = 78 * s, r = 8 * s;
    const float cw = (width - gap * float(n - 1)) / float(n);
    auto pick = [&](int i) {
      GfxCard& c = cards[size_t(i)];
      if (!c.enabled) return ui::Cue(ui::Sound::kDeny);
      ui::Cue(ui::Sound::kSelect);
      c.pick();
      REXLOG_INFO("KK: setup: {} {}", caption, c.label);
    };
    if (gfx_on_cards_ && !modal_.open) {
      const int before = gfx_card_focus_;
      if (input_.Pressed(Action::kLeft)) gfx_card_focus_ = std::max(0, gfx_card_focus_ - 1);
      if (input_.Pressed(Action::kRight)) gfx_card_focus_ = std::min(n - 1, gfx_card_focus_ + 1);
      if (gfx_card_focus_ != before) ui::Cue(ui::Sound::kMove);
      if (input_.Pressed(Action::kAccept)) pick(gfx_card_focus_);
      if (input_.Pressed(Action::kDown)) {
        gfx_on_cards_ = false;
        gfx_lists_[page].focus = -1;  // the first row
        gfx_lists_[page].focus_label.clear();
        ui::Cue(ui::Sound::kMove);
      }
      for (Action a : {Action::kLeft, Action::kRight, Action::kAccept, Action::kDown, Action::kUp}) input_.Consume(a);
    }
    for (int i = 0; i < n; ++i) {
      GfxCard& c = cards[size_t(i)];
      const ImVec2 p0(x0 + float(i) * (cw + gap), y), p1(p0.x + cw, y + ch);
      const bool hovered = !modal_.open && ImGui::IsMouseHoveringRect(p0, p1, false);
      if (hovered && (input_.mouse_moved() || ImGui::IsMouseClicked(0))) {
        if (!gfx_on_cards_ || gfx_card_focus_ != i) ui::Cue(ui::Sound::kMove);
        gfx_on_cards_ = true;
        gfx_card_focus_ = i;
      }
      if (hovered && ImGui::IsMouseClicked(0)) pick(i);
      const bool focused = gfx_on_cards_ && gfx_card_focus_ == i;
      dl->AddRectFilled(ImVec2(p0.x, p0.y + 4 * s), ImVec2(p1.x, p1.y + 4 * s), IM_COL32(0, 0, 0, 40), r);
      dl->AddRectFilled(p0, p1, ui::WithAlpha(c.selected ? IM_COL32(34, 37, 44, 255) : IM_COL32(16, 18, 22, 255),
                                              c.selected || focused ? 0.86f : 0.62f), r);
      dl->AddRect(p0, p1, ui::WithAlpha(c.selected ? color::kAccent : color::kText, c.selected ? 0.9f : 0.10f), r, 0,
                  (c.selected ? 1.5f : 1.0f) * s);
      if (c.selected) dl->AddRectFilled(ImVec2(p0.x + r, p0.y), ImVec2(p1.x - r, p0.y + 3 * s), color::kAccent);
      if (focused)
        dl->AddRect(ImVec2(p0.x - 3 * s, p0.y - 3 * s), ImVec2(p1.x + 3 * s, p1.y + 3 * s),
                    ui::WithAlpha(color::kFocus, 0.9f), r + 3 * s, 0, 2.0f * s);
      const float pad = 14 * s, room = cw - pad * 2;
      const std::string label = ui::Ellipsize(f.semibold, 18 * s, T(c.label), room);
      dl->AddText(f.semibold, 18 * s, ImVec2(p0.x + pad, p0.y + 16 * s), c.enabled ? color::kText : color::kTextFaint,
                  label.c_str());
      const std::string sub = ui::Ellipsize(f.text, 13 * s, T(c.caption), room - (c.selected ? 16 * s : 0));
      dl->AddText(f.text, 13 * s, ImVec2(p0.x + pad, p0.y + 44 * s),
                  !c.enabled ? color::kTextFaint : c.selected ? color::kAccent : color::kTextDim, sub.c_str());
      if (c.selected)  // the chosen card's mark
        dl->AddCircleFilled(ImVec2(p1.x - pad - 4 * s, p0.y + 25 * s), 5 * s, color::kAccent, 16);
    }
    return 24 * s + ch + 22 * s;
  }

  // ------------------------------------------------------------------------------- the close-ups ---
  // Which close-up goes with a row (or card row): "" for options that change how the game runs, not how it looks.
  static std::string CompareKey(std::string_view label) {
    static const std::pair<const char*, const char*> kKeys[] = {
        {"Resolution", "resolution"}, {"Window size", "resolution"}, {"Upscaler", "upscaler"},
        {"Upscaler mode", "upscaler"}, {"Render quality", "upscaler"}, {"SMAA", "smaa"},
        {"Sharpness", "sharpening"}, {"16x texture filtering", "filtering"}, {"Global illumination", "gi"},
        {"Ambient occlusion", "ao"}, {"Shadows", "shadows"}, {"Water reflections", "water"},
        {"Light shafts", "shafts"}, {"Original look", "look"}};
    for (const auto& [l, key] : kKeys)
      if (label == l) return key;
    return {};
  }

  // The stills the pictures are made from, all from the player's own game files (nothing ships with the port):
  // the movie stills the launcher extracted (Windows), else the chapter select's pictures, else the capture of
  // the title screen from the first play.
  const std::vector<art::Image>& SetupStills() {
    if (!setup_stills_loaded_) {
      setup_stills_loaded_ = true;
      if (art::HasLauncherArt(ArtDir())) setup_stills_ = art::LoadLauncherArt(ArtDir()).backdrops;
      for (uint32_t i = 0; setup_stills_.empty() && i < art::kChapterNames; ++i)
        for (uint32_t j = i; j < art::kChapterNames && setup_stills_.size() < 6; ++j)
          if (art::Image img = art::LoadImage(art::ChapterPicturePath(ArtDir(), int(j))); img && img.width >= 128)
            setup_stills_.push_back(std::move(img));
      if (setup_stills_.empty())
        if (art::Image img = art::LoadImage(art::TitleCapturePath(paths_.user_dir)); img && img.width >= 64)
          setup_stills_.push_back(std::move(img));
      REXLOG_INFO("KK: setup: {} stills from the game files for the graphics pages", setup_stills_.size());
    }
    return setup_stills_;
  }

  // The picture for a key, made once: a pair (Original / New, with the slider) where an option's effect can be
  // shown on a still (setup_compare.h), else the still alone with a note.
  const CompareArt& CompareFor(const std::string& key) {
    auto it = compare_art_.find(key);
    if (it != compare_art_.end()) return it->second;
    CompareArt art;
    const std::vector<art::Image>& stills = SetupStills();
    static const char* const kOrder[] = {"resolution", "upscaler", "sharpening", "smaa", "filtering", "gi",
                                         "ao", "shadows", "water", "shafts", "look"};
    size_t index = 0;
    for (size_t i = 0; i < std::size(kOrder); ++i)
      if (key == kOrder[i]) index = i;
    if (!stills.empty()) {
      const auto t0 = std::chrono::steady_clock::now();
      constexpr int kW = 768, kH = 432;
      const art::Image still = compare::Fit(stills[index % stills.size()], kW, kH);
      art::Image a, b;
      if (key == "resolution") {
        a = compare::FewerPixels(still, 2.0f);
        b = still;
        art.left = "Fewer pixels";
        art.right = "More pixels";
        art.note = "Made from a still of your game: drawn with fewer pixels on the left, more on the right.";
      } else if (key == "upscaler") {
        a = compare::FewerPixels(still, 2.0f);
        b = still;
        art.left = "Plain scaling";
        art.right = "Upscaler";
        art.note = "Made from a still of your game: half the pixels scaled up plainly on the left, the full "
                   "picture an upscaler rebuilds on the right.";
      } else if (key == "sharpening") {
        a = still;
        b = compare::Sharpen(still, 1.2f);
        art.left = "Off";
        art.right = "On";
        art.note = "Made from a still of your game: as drawn on the left, sharpened on the right.";
      } else if (key == "smaa") {
        a = compare::Edges(stills[index % stills.size()], kW, kH, 6, 3, false);
        b = compare::Edges(stills[index % stills.size()], kW, kH, 6, 3, true);
        art.left = "Off";
        art.right = "On";
        art.note = "Made from a still of your game, enlarged: jagged edges on the left, smoothed edges on the right.";
      } else if (key == "filtering") {
        const art::Image floor = compare::Fit(stills[index % stills.size()], 512, 288);
        a = compare::Ground(floor, kW, kH, false);
        b = compare::Ground(floor, kW, kH, true);
        art.left = "Off";
        art.right = "On";
        art.note = "Made from a still of your game laid on a floor: the far floor blurs on the left and stays sharp "
                   "with 16x filtering on the right.";
      } else {
        art.still = MakeTexture(still);
        art.note = "A picture from your copy of the game. This option's effect only shows in the game itself, as "
                   "described below.";
      }
      if (a && b) {
        art.original = MakeTexture(a);
        art.fresh = MakeTexture(b);
      }
      REXLOG_INFO("KK: setup: picture {} made from the game files ({} ms)", key,
                  std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - t0).count());
    } else {
      REXLOG_INFO("KK: setup: picture {}: no stills from the game files yet", key);
    }
    return compare_art_.emplace(key, art).first->second;
  }

  // The close-up between p0 and p1: Original on the left, New on the right, the slider between them; or one
  // picture from the game; and the note saying what it shows.
  void DrawCompare(ImDrawList* dl, const std::string& key, ImVec2 p0, ImVec2 p1) {
    const float s = S();
    const ui::Fonts f = ui::GetFonts();
    const float r = 10 * s, w = p1.x - p0.x, h = p1.y - p0.y;
    dl->AddRectFilled(ImVec2(p0.x, p0.y + 5 * s), ImVec2(p1.x, p1.y + 5 * s), IM_COL32(0, 0, 0, 50), r);
    dl->AddRectFilled(p0, p1, ui::WithAlpha(IM_COL32(14, 16, 20, 255), 0.82f), r);
    auto centred = [&](const std::string& title, const std::string& body) {
      const float wrap = std::min(w - 60 * s, 460 * s);
      const float th = ui::TextSize(f.display, 20 * s, title, wrap).y, bh = ui::TextSize(f.text, 14.5f * s, body, wrap).y;
      float ty = p0.y + (h - th - 12 * s - bh) * 0.5f;
      const float tx = p0.x + (w - wrap) * 0.5f;
      dl->AddText(f.display, 20 * s, ImVec2(tx, ty), color::kTextDim, title.c_str(), nullptr, wrap);
      ty += th + 12 * s;
      dl->AddText(f.text, 14.5f * s, ImVec2(tx, ty), color::kTextFaint, body.c_str(), nullptr, wrap);
    };
    auto tag = [&](const char* english, bool left) {
      const std::string t = Upper(T(english));
      const float tw = ui::TrackedWidth(f.display, 12 * s, t, 2.4f * s), pad = 9 * s;
      const float tx = left ? p0.x + 12 * s : p1.x - 12 * s - tw - pad * 2;
      dl->AddRectFilled(ImVec2(tx, p0.y + 12 * s), ImVec2(tx + tw + pad * 2, p0.y + 12 * s + 26 * s),
                        IM_COL32(8, 10, 13, 190), 4 * s);
      ui::DrawTracked(dl, f.display, 12 * s, ImVec2(tx + pad, p0.y + 19 * s), left ? color::kTextDim : color::kAccent,
                      t, 2.4f * s);
    };
    const CompareArt* art = key.empty() ? nullptr : &CompareFor(key);
    if (key.empty()) {
      centred(T(std::string("Nothing to compare")),
              T(std::string("This option changes how the game runs, not how it looks.")));
    } else if (art->still) {
      dl->AddImageRounded(ui::Tex(art->still), p0, p1, ImVec2(0, 0), ImVec2(1, 1), IM_COL32_WHITE, r);
      tag("From your game", true);
    } else if (!art->original) {
      centred(T(std::string("No picture yet")),
              T(std::string("Pictures from your copy of the game show here once its files are installed.")));
    } else {
      // The slider: dragged with the mouse (from a click on the picture), or LT and RT (DrawSetupGraphics).
      const ImGuiIO& io = ImGui::GetIO();
      const bool over = !modal_.open && ImGui::IsMouseHoveringRect(p0, p1, false);
      if (over && ImGui::IsMouseClicked(0)) gfx_dragging_ = true;
      if (gfx_dragging_ && !ImGui::IsMouseDown(0)) gfx_dragging_ = false;
      if (gfx_dragging_) gfx_split_ = std::clamp((io.MousePos.x - p0.x) / std::max(1.0f, w), 0.0f, 1.0f);
      const float sx = std::round(p0.x + w * gfx_split_);
      dl->AddImage(ui::Tex(art->original), p0, ImVec2(sx, p1.y), ImVec2(0, 0), ImVec2(gfx_split_, 1));
      dl->AddImage(ui::Tex(art->fresh), ImVec2(sx, p0.y), p1, ImVec2(gfx_split_, 0), ImVec2(1, 1));
      dl->AddRectFilled(ImVec2(sx - 1 * s, p0.y), ImVec2(sx + 1 * s, p1.y), color::kFocus);
      const float cy = p0.y + h * 0.5f, kr = 15 * s;
      dl->AddCircleFilled(ImVec2(sx, cy), kr, color::kFocus, 24);
      const float aw = 5 * s, ah = 5 * s;
      dl->AddTriangleFilled(ImVec2(sx - 3 * s - aw, cy), ImVec2(sx - 3 * s, cy - ah), ImVec2(sx - 3 * s, cy + ah), color::kOnFocus);
      dl->AddTriangleFilled(ImVec2(sx + 3 * s + aw, cy), ImVec2(sx + 3 * s, cy - ah), ImVec2(sx + 3 * s, cy + ah), color::kOnFocus);
      tag(art->left, true);
      tag(art->right, false);
    }
    if (art && art->note) {  // what the picture shows, along its bottom
      const std::string note = T(std::string(art->note));
      const float pad = 12 * s, wrap = w - pad * 2;
      const float nh = ui::TextSize(f.text, 13 * s, note, wrap).y;
      dl->AddRectFilled(ImVec2(p0.x, p1.y - nh - pad * 2), p1, IM_COL32(8, 10, 13, 200), r, ImDrawFlags_RoundCornersBottom);
      dl->AddText(f.text, 13 * s, ImVec2(p0.x + pad, p1.y - nh - pad), color::kTextDim, note.c_str(), nullptr, wrap);
    }
    dl->AddRect(p0, p1, ui::WithAlpha(color::kText, 0.12f), r, 0, 1.0f * s);
  }

  // ------------------------------------------------------------------------------------ the page ---
  void DrawSetupGraphics(ImDrawList* dl) {
    const float s = S();
    const ui::Fonts f = ui::GetFonts();
    const int page = GfxPage();
    if (!gfx_entered_) {
      gfx_entered_ = true;
      // New installs are already Remastered (StartFullSetup, job J6), and the Original look is a switch on the
      // last page; a player who comes from the New in this update screen sees their own settings.
      LogGraphicsChoices(gfx_from_new_ ? "graphics pages start (from New in this update)" : "graphics pages start");
    }
    const float x0 = o_.x + Margin(), right = o_.x + w_ - Margin();
    const float list_w = std::round(std::min(660 * s, (right - x0) * 0.5f));
    const float bottom = o_.y + h_ - 92 * s;
    float y = o_.y + std::max(40 * s, h_ * 0.06f);
    const float top = y;

    // The logo, where the step is, the heading and what the page is for.
    if (logo_) {
      const float lh = 72 * s, lw = lh * logo_aspect_;
      dl->AddImage(ui::Tex(logo_), ImVec2(x0 - 6 * s, y - 8 * s), ImVec2(x0 - 6 * s + lw, y - 8 * s + lh));
      y += lh;
    }
    const std::string caption = Upper(T("PC port")) + "  " KK_DOT "  " + Upper(T("Setup")) + "  " KK_DOT "  " +
                                Upper(F("Graphics {0} of {1}", {std::to_string(page + 1), std::to_string(kGfxPages)}));
    ui::DrawTracked(dl, f.display, 13 * s, ImVec2(x0 + 2 * s, y), color::kTextDim, caption, 3 * s);
    {
      float px = x0 + 2 * s + ui::TrackedWidth(f.display, 13 * s, caption, 3 * s) + 18 * s;
      for (int i = 0; i < kGfxPages; ++i, px += 26 * s)
        dl->AddRectFilled(ImVec2(px, y + 5 * s), ImVec2(px + 20 * s, y + 8 * s),
                          i == page ? color::kAccent : i < page ? WithAlphaText(0.45f) : color::kHairline);
    }
    y += 30 * s;
    dl->AddText(f.display, 30 * s, ImVec2(x0, y), color::kText, T(kGfxTitles[page]));
    y += 44 * s;
    const std::string intro = std::string(T(kGfxIntros[page])) + " " + T(kChangeLater);
    dl->AddText(f.text, 15 * s, ImVec2(x0, y), color::kTextDim, intro.c_str(), nullptr, list_w);
    y += ui::TextSize(f.text, 15 * s, intro, list_w).y + 20 * s;

    // The comparison's slider from the controller (LT and RT) or Page Up / Page Down, before the list (which
    // would scroll with them).
    if (!modal_.open) {
      const float dt = std::min(ImGui::GetIO().DeltaTime, 0.1f);
      float dir = 0;
      if (input_.HeldFor(Action::kPageUp) > 0) dir -= dt * 0.9f;
      if (input_.HeldFor(Action::kPageDown) > 0) dir += dt * 0.9f;
      if (input_.Pressed(Action::kPageUp)) dir -= 0.05f;
      if (input_.Pressed(Action::kPageDown)) dir += 0.05f;
      if (dir != 0) gfx_split_ = std::clamp(gfx_split_ + dir, 0.0f, 1.0f);
      input_.Consume(Action::kPageUp);
      input_.Consume(Action::kPageDown);
    }

#if defined(KK_DEV_TOOLS)
    // Developer: which presses reach the page, and whether a test injected them (a controller in another
    // program's hands also reaches the launcher).
    for (Action a : {Action::kAccept, Action::kBack, Action::kLeft, Action::kRight, Action::kUp, Action::kDown})
      if (input_.Pressed(a) && !modal_.open)
        REXLOG_INFO("KK dev: setup graphics page {} press {} ({})", page + 1, int(a), input_.injected() ? "injected" : "not injected");
#endif
    std::vector<Item> items = SetupGraphicsItems(page);
    std::vector<GfxCard> cards = GfxCards(page);
    if (cards.empty()) gfx_on_cards_ = false;
    ui::ListState& st = gfx_lists_[page];
    if (!cards.empty()) {
      // Up from the first row goes to the cards.
      if (!gfx_on_cards_ && !modal_.open && input_.Pressed(Action::kUp)) {
        int first = -1;
        for (int i = 0; i < int(items.size()) && first < 0; ++i)
          if (items[size_t(i)].type != ItemType::kSection) first = i;
        if (st.focus <= first) {
          input_.Consume(Action::kUp);
          gfx_on_cards_ = true;
          ui::Cue(ui::Sound::kMove);
        }
      }
      y += DrawGfxCards(dl, cards, page, x0, y, list_w);
    }
    const ImVec2 l0(x0, y), l1(x0 + list_w, bottom);
    if (gfx_on_cards_ && !modal_.open && input_.mouse_moved() && ImGui::IsMouseHoveringRect(l0, l1, false))
      gfx_on_cards_ = false;
    const Item* focused = ui::DrawList(dl, items, st, l0, l1, m_, input_, !modal_.open && !gfx_on_cards_);

    // B: the page before (the first page has none in the setup: the steps before it are done; from the New in
    // this update screen it goes back there).
    if (!modal_.open && input_.Pressed(Action::kBack)) {
      input_.Consume(Action::kBack);
      if (page > 0) {
        ui::Cue(ui::Sound::kBack);
        GfxGo(page - 1);
      } else if (gfx_from_new_) {
        ui::Cue(ui::Sound::kBack);
        BackToNewScreen();
      } else {
        ui::Cue(ui::Sound::kDeny);
      }
    }

    // The right side: a download's strip (when one runs), the close-up, and the focused option's sentence.
    const float rx0 = x0 + list_w + 56 * s, rx1 = right;
    float ry = top;
    if (DrawContentStrip(dl, 1.0f, true)) ry = o_.y + std::max(84 * s, h_ * 0.12f) + 66 * s + 18 * s;
    Item card_item;
    const Item* described = focused;
    std::string key;
    if (gfx_on_cards_ && !cards.empty()) {
      const GfxCard& c = cards[size_t(std::clamp(gfx_card_focus_, 0, int(cards.size()) - 1))];
      card_item.type = ItemType::kInfo;
      card_item.label = c.label;
      card_item.description = c.text;
      if (page == 0) card_item.facts.push_back("Applies when the game starts.");
      if (page == 1 && !OfferedTemporalUpscalers().known)
        card_item.facts.push_back("Checking which other upscalers your graphics card runs...");
      if (!c.enabled) {
        card_item.disabled = true;
        card_item.disabled_note = "Your graphics card does not run this upscaler.";
      }
      described = &card_item;
      key = page == 0 ? std::string() : std::string("upscaler");
    } else if (focused) {
      key = focused->type == ItemType::kAction ? gfx_last_key_ : CompareKey(focused->label);
    }
    if (!(focused && focused->type == ItemType::kAction && !gfx_on_cards_)) gfx_last_key_ = key;
    const float pw = rx1 - rx0;
    float ph = std::min(pw * 9.0f / 16.0f, bottom - ry - 190 * s);
    ph = std::max(ph, 120 * s);
    const float cw = std::min(pw, ph * 16.0f / 9.0f);
    DrawCompare(dl, key, ImVec2(rx0, ry), ImVec2(rx0 + cw, ry + ph));
    ui::DrawDescription(dl, described, ImVec2(rx0, ry + ph + 22 * s), ImVec2(rx1, bottom), m_);

    // Kept for the prompts and the developer steps (the items live until the next frame's rebuild).
    focused_items_ = std::move(items);
    focused_ = gfx_on_cards_ || st.focus < 0 || st.focus >= int(focused_items_.size()) ? nullptr
                                                                                       : &focused_items_[size_t(st.focus)];
#if defined(KK_DEV_TOOLS)
    {
      static std::string logged;
      const std::string now = std::to_string(page) + (gfx_on_cards_ ? " card " + std::to_string(gfx_card_focus_)
                                                                    : " row " + (focused_ ? focused_->label : std::string("-"))) +
                              " key " + key;
      if (now != logged) {
        logged = now;
        REXLOG_INFO("KK dev: setup graphics page {} focus {}", page + 1, now.substr(now.find(' ') + 1));
      }
    }
#endif
  }

  std::vector<ui::Prompt> GfxPrompts() const {
    std::vector<ui::Prompt> p;
    if (gfx_on_cards_) {
      p.push_back({Action::kAccept, "Select"});
    } else if (focused_) {
      if ((focused_->type == ItemType::kChoice || focused_->type == ItemType::kSlider) && !focused_->disabled)
        p.push_back({Action::kLeft, "Change"});
      if ((focused_->type == ItemType::kAction || focused_->type == ItemType::kLink) && !focused_->disabled)
        p.push_back({Action::kAccept, "Select"});
    }
    p.push_back({Action::kPageUp, "Compare"});
    if (GfxPage() > 0 || gfx_from_new_) p.push_back({Action::kBack, "Back"});
    return p;
  }
