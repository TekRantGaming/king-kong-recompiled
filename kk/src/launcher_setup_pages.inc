  // ------------------------------------------------------------ setup: the Settings pages ---
  // Task 332 (owner, 20 Oct, DECISIONS row A): the first-time setup's order is now game files, shaders, then the
  // FULL Settings pages Display, Graphics and Controls (SetupStage 16 to 18), button prompts (12), the HD texture
  // pack (6), the PS2 cutscenes (7), the Workshop (15) and You're all set (13). The three Settings pages are built
  // by the same code as Settings (SettingsPageItems: DisplayItems, GraphicsItems, ControlsItems, in the order of
  // settings_layout.h), with the same right-hand description panel, and Next / Back rows under them, so a setting
  // added to one of those pages shows in the setup by itself. No recommended-settings button and no pictures there.
  // Task 365: every page after the shaders until the Finish page has a row at the top right (DrawSetupTop): a progress
  // bar by page, Auto detect, Skip all (a warning pop-up, then straight to the startup movies, StartStartupMovies,
  // with every setting not chosen left as it is) and Next page.
  // Included inside class Launcher (launcher.cpp). The four graphics pages with pictures
  // (launcher_setup_graphics.inc) are only shown from the New in this update screen now.

  static constexpr int kWorkshopStage = 15, kSettingsFirstStage = 16, kSettingsSetupPages = 3;
  static constexpr Page kSetupSettingsPages[kSettingsSetupPages] = {kDisplay, kGraphics, kControls};

  bool setup_settings_ = false;  // setup steps 3 to 5: the Settings pages (SetupStage 16 to 18)
  int setup_settings_page_ = 0;
  bool setup_workshop_ = false;  // setup step 9: what the Workshop is (SetupStage 15)
  ui::ListState setup_lists_[kSettingsSetupPages * 4];

  bool SettingsSetupStage(int stage) const {
    return stage >= kSettingsFirstStage && stage < kSettingsFirstStage + kSettingsSetupPages;
  }
  int SetupSettingsPage() const { return std::clamp(SetupStage() - kSettingsFirstStage, 0, kSettingsSetupPages - 1); }
  // The first-time setup's pages that ask the player something (developer scripts press buttons on them).
  bool AskingSetupStage(int stage) const {
    return stage == 6 || stage == 7 || GraphicsSetupStage(stage) || EndSetupStage(stage) || stage == kNewStage ||
           stage == kWorkshopStage || SettingsSetupStage(stage);
  }

  void SetupSettingsGo(int page) {
    setup_settings_ = true;
    setup_settings_page_ = std::clamp(page, 0, kSettingsSetupPages - 1);
    sub_ = kNoSubPage;
#if defined(KK_DEV_TOOLS)
    if (dev_setup_ >= 0) dev_setup_ = kSettingsFirstStage + setup_settings_page_;
#endif
    REXLOG_INFO("KK: setup: Settings page {} ({})", setup_settings_page_ + 1,
                kPageNames[kSetupSettingsPages[setup_settings_page_]]);
  }

  void FinishSetupSettings() {
    setup_settings_ = false;
    sub_ = kNoSubPage;
#if defined(KK_DEV_TOOLS)
    if (SettingsSetupStage(dev_setup_)) dev_setup_ = kPromptsStage;
#endif
    LogGraphicsChoices("Settings pages done");
    REXLOG_INFO("KK: setup: Settings pages done; the button prompts next");
  }

  // The rows of a Settings page as Settings builds them (DrawSettings uses the same), with the help.
  std::vector<Item> SetupSettingsItems(int page) {
    in_setup_pages_ = true;
    std::vector<Item> items = SettingsPageItems(kSetupSettingsPages[page]);
    in_setup_pages_ = false;
    AttachHelp(items);
    if (sub_ != kNoSubPage) return items;  // Controls' Button remapping / Keyboard bindings: B goes back
    items.push_back(Section(""));
    items.push_back(ActionItem("Next", "The next page.", "",  // task 365: the page number is in the top right bar
                               [this, page] {
                                 if (page + 1 < kSettingsSetupPages) SetupSettingsGo(page + 1);
                                 else FinishSetupSettings();
                               }));
    if (page > 0) items.push_back(ActionItem("Back", "The page before.", "", [this, page] { SetupSettingsGo(page - 1); }));
    return items;
  }

  void DrawSetupSettings(ImDrawList* dl) {
    const float s = S();
    const ui::Fonts f = ui::GetFonts();
    const int page = SetupSettingsPage();
    const Page settings_page = kSetupSettingsPages[page];
    // The same frame as Settings (DrawSettings): the rows on the left, the description on the right. Above them,
    // where Settings has its tabs: where the setup is, and the page's name.
    const float top = o_.y + 104 * s, bottom = o_.y + h_ - 96 * s;
    const float x0 = o_.x + Margin();
    const float list_w = std::round((w_ - Margin() * 2) * 0.56f);
    const std::string caption =
        Upper(T("PC port")) + "  " KK_DOT "  " + Upper(T("Setup")) + "  " KK_DOT "  " + Upper(T(kPageNames[settings_page]));
    float y = o_.y + 30 * s;
    // (Task 365: where the setup is now shows in the progress bar at the top right, DrawSetupTop.)
    ui::DrawTracked(dl, f.display, 13 * s, ImVec2(x0 + 2 * s, y), color::kTextDim, caption, 3 * s);
    y += 24 * s;
    const std::string title = sub_ == kRemapPage ? T(std::string("Button remapping"))
                              : sub_ == kKeysPage ? T(std::string("Keyboard bindings"))
                                                  : T(std::string(kPageNames[settings_page]));
    dl->AddText(f.display, 30 * s, ImVec2(x0, y), color::kText, title.c_str());
    DrawSetupTop(dl);

    std::vector<Item> items = SetupSettingsItems(page);
    ui::ListState& st = setup_lists_[page * 4 + sub_];
    const Item* focused = ui::DrawList(dl, items, st, ImVec2(x0, top), ImVec2(x0 + list_w, bottom), m_, input_,
                                       !modal_.open);
    const float dx = x0 + list_w + 76 * s;
    ui::DrawDescription(dl, focused, ImVec2(dx, top + 18 * s), ImVec2(o_.x + w_ - Margin(), bottom), m_);

    // B: out of Controls' sub-page, else the page before (none before the first: the shaders are done).
    if (!modal_.open && capturing_.empty() && input_.Pressed(Action::kBack)) {
      input_.Consume(Action::kBack);
      if (sub_ != kNoSubPage) {
        ui::Cue(ui::Sound::kBack);
        sub_ = kNoSubPage;
      } else if (page > 0) {
        ui::Cue(ui::Sound::kBack);
        SetupSettingsGo(page - 1);
      } else {
        ui::Cue(ui::Sound::kDeny);
      }
    }
    // Kept for the prompts (the items live until the next frame's rebuild).
    focused_items_ = std::move(items);
    focused_ = st.focus >= 0 && st.focus < int(focused_items_.size()) ? &focused_items_[size_t(st.focus)] : nullptr;
#if defined(KK_DEV_TOOLS)
    {
      static std::string logged;
      const std::string now = std::to_string(page + 1) + " sub " + std::to_string(int(sub_)) + " row " +
                              (focused_ ? focused_->label : std::string("-"));
      if (now != logged) {
        logged = now;
        REXLOG_INFO("KK dev: setup Settings page {}", now);
      }
    }
#endif
  }

  std::vector<ui::Prompt> SetupSettingsPrompts() const {
    std::vector<ui::Prompt> p;
    if (focused_) {
      if ((focused_->type == ItemType::kChoice || focused_->type == ItemType::kSlider) && !focused_->disabled)
        p.push_back({Action::kLeft, "Change"});
      if ((focused_->type == ItemType::kAction || focused_->type == ItemType::kLink) && !focused_->disabled &&
          focused_->on_activate)
        p.push_back({Action::kAccept, "Select"});
      if (focused_->on_default && !focused_->disabled) p.push_back({Action::kDefault, "Default"});
    }
    if (sub_ != kNoSubPage || SetupSettingsPage() > 0) p.push_back({Action::kBack, "Back"});
    return p;
  }

  // --------------------------------------------------------------------- setup: the Workshop ---
  // Setup step 9 (task 332). Task 365 (owner, 23 Oct): a proper description (what the Workshop is, the texture packs
  // and mods it has, how they are downloaded and switched on, how big the packs are) and pictures. The pictures are
  // the ones the Workshop's own cards show, so nothing new ships with the port: the HD texture pack and the Cel
  // shading mod are made at run time from a still of the player's own game (WorkshopArt); the UHD, Comic Book, Cartoon
  // and Tropical Holiday packs show the picture.png at the start of their download (read once and kept in the
  // Workshop's cache, or their installed copy's), with one of the game's stills until it is there.
  struct WorkshopTile {
    const char* key;    // the Workshop card's key (TexturePackShelf::Picture) or the built in mod's id
    const char* label;  // the pack's short name (a name: not translated), or the mod's name (translated)
    bool mod;
  };
  static constexpr WorkshopTile kWorkshopTiles[] = {
      {"hd", "HD", false},          {"uhd", "UHD", false},
      {"comic", "Comic Book", false}, {"cartoon", "Cartoon", false},
      {"tropical", "Tropical Holiday", false}, {"builtin:cel-shading", "Cel shading", true}};

  rex::ui::ImmediateTexture* WorkshopTilePicture(const WorkshopTile& tile, float* aspect) {
    if (tile.mod || std::string_view(tile.key) == "hd") {
      if (auto* t = WorkshopArt(tile.key, aspect)) return t;
    } else if (workshop_) {
      if (auto* t = workshop_->TexturePackPicture(tile.key, aspect)) return t;
    }
    if (slides_.empty()) return nullptr;
    const Slide& sl = slides_[size_t(&tile - kWorkshopTiles) % slides_.size()];
    *aspect = sl.aspect;
    return sl.sharp;
  }

  // The smallest and largest texture pack download and the largest installed pack, in GB (the size warning).
  std::array<std::string, 3> TexturePackSizes() const {
    uint64_t lo = 0, hi = 0, unpacked = 0;
    for (const content::Kind k : {content::Kind::kTexturePack, content::Kind::kUhd, content::Kind::kComic,
                                  content::Kind::kCartoon, content::Kind::kTropical}) {
      const content::Info& info = content::InfoFor(k);
      if (info.zip_size == 0) continue;
      lo = lo == 0 ? info.zip_size : std::min(lo, info.zip_size);
      hi = std::max(hi, info.zip_size);
      unpacked = std::max(unpacked, info.unpacked);
    }
    return {Gb(lo), Gb(hi), Gb(unpacked)};
  }

  void DrawSetupWorkshop(ImDrawList* dl) {
    const float s = S();
    const ui::Fonts f = ui::GetFonts();
    const float x0 = o_.x + Margin(), right = o_.x + w_ - Margin();
    const float list_w = std::round(std::min(660 * s, (right - x0) * 0.5f));
    const float top = o_.y + std::max(40 * s, h_ * 0.06f);
    const std::string intro = std::string(T("The Workshop is on the main menu.")) + " " +
                              T("It has mods and texture packs for King Kong, and other projects from TRG Studios.");
    float y = DrawEndHeader(dl, "Workshop", "The Workshop", intro, x0, list_w);
    DrawSetupTop(dl);
    // What it has and how it works: a caption and a short paragraph each, then the size warning. Smaller type when
    // the column is short (1280x720 in German), so Next and Back always fit above the button prompts.
    const auto [lo, hi, unpacked] = TexturePackSizes();
    struct Block {
      std::string caption, text;
      bool warn;
    };
    const std::vector<Block> blocks = {
        {T("Texture packs"),
         T(std::string("Texture packs change how the whole game looks. HD and UHD keep the original style with much "
                       "sharper detail. Comic Book, Cartoon and Tropical Holiday give it a fun new look.")),
         false},
        {T("Mods"),
         T(std::string("Mods change how the game looks or plays, such as Cel shading, which draws it like a comic "
                       "book. You can also add your own mods; the modding guide in the Workshop shows how.")),
         false},
        {T("How it works"),
         T(std::string("Choose a pack or mod in the Workshop to see it, download it, then switch it on there. Only "
                       "one texture pack is on at a time, and you can go back to the original textures at any time.")),
         false},
        {T("Before you download"),
         F("Texture packs are big: each one is a download of {0} to {1} GB and takes about {2} GB once installed.",
           {lo, hi, unpacked}),
         true},
    };
    const float bottom = o_.y + h_ - 96 * s - 2 * 42 * s - 10 * s;
    float k = 1.0f;
    for (const float try_k : {1.0f, 0.93f, 0.87f, 0.8f}) {
      k = try_k;
      float h = 6 * s;
      for (const Block& b : blocks) h += 18 * s * k + ui::TextSize(f.text, 15 * s * k, b.text, list_w).y + 14 * s * k;
      if (y + h <= bottom) break;
    }
    y += 6 * s;
    for (const Block& b : blocks) {
      ui::DrawTracked(dl, f.display, 12 * s * k, ImVec2(x0 + 1 * s, y), b.warn ? color::kAccent : color::kTextDim,
                      Upper(b.caption), 2.4f * s * k);
      y += 18 * s * k;
      dl->AddText(f.text, 15 * s * k, ImVec2(x0, y), b.warn ? color::kText : color::kTextDim, b.text.c_str(), nullptr,
                  list_w);
      y += ui::TextSize(f.text, 15 * s * k, b.text, list_w).y + 14 * s * k;
    }
    y += 6 * s;
    if (end_page_ != kWorkshopStage) {  // the page coming up: the focus on Next
      end_page_ = kWorkshopStage;
      end_on_cards_ = false;
      end_menu_focus_ = 0;
    }
    std::vector<HomeEntry> entries;
    entries.push_back({"Next", true, [this] { FinishSetupWorkshop(); }});
    entries.push_back({"Back", true, [this] { BackFromWorkshop(); }});
    end_on_cards_ = false;  // no cards on this page: Up on Next stays there
    if (end_menu_focus_ == 0) input_.Consume(Action::kUp);
    DrawEndMenu(dl, entries, x0, y, [this] { BackFromWorkshop(); });

    // The right side: a download's strip (when one runs), then the pictures, two to a row, each with its name.
    const float rx0 = x0 + list_w + 56 * s, rx1 = right;
    float ry = top + 18 * s;  // clear of the row at the top right
    if (DrawContentStrip(dl, 1.0f, true)) ry = o_.y + std::max(84 * s, h_ * 0.12f) + 66 * s + 18 * s;
    const int n = int(std::size(kWorkshopTiles)), cols = 2, rows = (n + cols - 1) / cols;
    const float gap = 12 * s, label_h = 24 * s, r = 8 * s;
    const float room_h = o_.y + h_ - 110 * s - ry;
    float tw = (rx1 - rx0 - gap * float(cols - 1)) / float(cols);
    float th = tw * 9.0f / 16.0f;
    if (rows * (th + label_h + gap) > room_h) {  // short window: smaller tiles, the column centred
      th = std::max(40 * s, room_h / float(rows) - label_h - gap);
      tw = th * 16.0f / 9.0f;
    }
    const float gx0 = rx0 + std::max(0.0f, (rx1 - rx0) - (tw * cols + gap * (cols - 1))) * 0.5f;
    for (int i = 0; i < n; ++i) {
      const WorkshopTile& tile = kWorkshopTiles[i];
      const ImVec2 p0(gx0 + float(i % cols) * (tw + gap), ry + float(i / cols) * (th + label_h + gap));
      const ImVec2 p1(p0.x + tw, p0.y + th);
      float aspect = 16.0f / 9.0f;
      dl->AddRectFilled(ImVec2(p0.x, p0.y + 4 * s), ImVec2(p1.x, p1.y + 4 * s), IM_COL32(0, 0, 0, 50), r);
      if (auto* pic = WorkshopTilePicture(tile, &aspect)) {
        // Cropped to the tile's shape (the pictures are 16:9 or close to it).
        const float want = tw / std::max(th, 1.0f);
        float u0 = 0, u1 = 1, v0 = 0, v1 = 1;
        if (aspect > want) {
          const float u = want / aspect;
          u0 = (1 - u) * 0.5f, u1 = u0 + u;
        } else if (aspect > 0) {
          const float v = aspect / want;
          v0 = (1 - v) * 0.5f, v1 = v0 + v;
        }
        dl->AddImageRounded(ui::Tex(pic), p0, p1, ImVec2(u0, v0), ImVec2(u1, v1), IM_COL32_WHITE, r);
      } else {
        dl->AddRectFilled(p0, p1, ui::WithAlpha(IM_COL32(16, 18, 22, 255), 0.8f), r);
      }
      dl->AddRect(p0, p1, ui::WithAlpha(color::kText, 0.12f), r, 0, 1.0f * s);
      const std::string label = ui::Ellipsize(f.semibold, 14 * s, tile.mod ? std::string(T(tile.label)) : tile.label, tw);
      const std::string kind = T(tile.mod ? "Mod" : "Texture pack");
      dl->AddText(f.semibold, 14 * s, ImVec2(p0.x + 2 * s, p1.y + 5 * s), color::kText, label.c_str());
      const float lw = ui::TextSize(f.semibold, 14 * s, label).x;
      dl->AddText(f.text, 12.5f * s, ImVec2(p0.x + 2 * s + lw + 8 * s, p1.y + 6.5f * s), color::kTextFaint,
                  ui::Ellipsize(f.text, 12.5f * s, kind, std::max(0.0f, tw - lw - 10 * s)).c_str());
    }
  }

  void FinishSetupWorkshop() {
    setup_workshop_ = false;
    end_page_ = -1;
#if defined(KK_DEV_TOOLS)
    if (dev_setup_ == kWorkshopStage) dev_setup_ = kAllSetStage;
#endif
    REXLOG_INFO("KK: setup: Workshop page done");
  }

  // Back from the Workshop page: the PS2 cutscene page where it is shown, else the texture pack page.
  void BackFromWorkshop() {
    end_page_ = -1;
    if (movies::PlaybackAvailable()) setup_cutscenes_ = true;
    else setup_textures_ = true;
    setup_focus_ = 0;
#if defined(KK_DEV_TOOLS)
    if (dev_setup_ == kWorkshopStage) dev_setup_ = movies::PlaybackAvailable() ? 7 : 6;
#endif
    REXLOG_INFO("KK: setup: back from the Workshop page");
  }

  // ------------------------------------------------------------- setup: the top right row ---
  // Task 365 (owner, 23 Oct): on every page after the shaders until the Finish page (setup_progress.h): a progress bar
  // by page, then Auto detect (the Settings pages and the button prompts), Skip all and Next page, each clicked or
  // pressed (Start or P, Y or R, RB or E). The shader screen has none of them: it moves on by itself when the shaders
  // are ready. The Finish page (You're all set) has nothing there.
  static_assert(kSettingsFirstStage == setup_progress::kDisplay && kPromptsStage == setup_progress::kPrompts &&
                kWorkshopStage == setup_progress::kWorkshop && kAllSetStage == setup_progress::kFinish,
                "setup_progress.h follows SetupStage's numbers");
  double setup_next_at_ = -10;  // the last Next page (a held RB or E repeats: one page per press)

  bool SetupTopShown() const {
#if defined(KK_DEV_TOOLS)
    if (dev_setup_ >= 0) return setup_progress::For(dev_setup_, movies::PlaybackAvailable()).shown;
#endif
    if (!files_ok_ || installing_ || setup_art_ || installing_update_ || update_new_ || ShaderScreen()) return false;
    if (!(setup_settings_ || setup_prompts_ || setup_textures_ || setup_cutscenes_ || setup_workshop_)) return false;
    return setup_progress::For(SetupStage(), movies::PlaybackAvailable()).shown;
  }

  void DrawSetupTop(ImDrawList* dl) {
    if (!SetupTopShown()) return;
    const float s = S();
    const ui::Fonts f = ui::GetFonts();
    const int stage = SetupStage();
    std::vector<ui::Prompt> prompts;
    if (setup_progress::AutoDetectOffered(stage)) prompts.push_back({Action::kPlay, "Auto detect"});
    prompts.push_back({Action::kResetPage, "Skip all"});
    prompts.push_back({Action::kNextTab, "Next page"});
    const float right = o_.x + w_ - Margin(), cy = o_.y + 22 * s;
    const float pw = ui::PromptsWidth(dl, glyphs_, PromptSet(), prompts, m_);
    const Action clicked = ui::DrawPrompts(dl, glyphs_, PromptSet(), prompts, right, cy, m_, !modal_.open);
    // The progress bar by page, left of the buttons: one segment a page (the Finish page counted), the pages done
    // dimmed, this one in the accent, and "3 of 8" before it.
    const setup_progress::Bar bar = setup_progress::For(stage, movies::PlaybackAvailable());
    {
      const float seg = 16 * s, gap = 4 * s, fs = 13.5f * s;
      const float bw = seg * float(bar.count) + gap * float(bar.count - 1);
      const std::string label = F("{0} of {1}", {std::to_string(bar.index), std::to_string(bar.count)});
      float x = right - pw - 30 * s - bw;
      dl->AddText(f.text, fs, ImVec2(x - 10 * s - ui::TextSize(f.text, fs, label).x, cy - fs * 0.5f - 1 * s),
                  color::kTextDim, label.c_str());
      for (int i = 1; i <= bar.count; ++i, x += seg + gap)
        dl->AddRectFilled(ImVec2(x, cy - 2 * s), ImVec2(x + seg, cy + 2 * s),
                          i == bar.index ? color::kAccent : i < bar.index ? WithAlphaText(0.5f) : color::kHairline,
                          2 * s);
    }
    if (modal_.open || !capturing_.empty()) return;
    Action act = clicked;
    for (const ui::Prompt& p : prompts)
      if (act == Action::kCount && input_.Pressed(p.action)) {
        act = p.action;
        if (act == Action::kNextTab && ImGui::GetTime() - setup_next_at_ < 0.35) act = Action::kCount;  // held
      }
    for (const ui::Prompt& p : prompts) input_.Consume(p.action);
    input_.Consume(Action::kPrevTab);
    if (act == Action::kNextTab) {
      setup_next_at_ = ImGui::GetTime();
      SetupNextPage();
    } else if (act == Action::kResetPage) {
      ui::Cue(ui::Sound::kSelect);
      AskSkipAll();
    } else if (act == Action::kPlay) {
      AutoDetectSetup();
    }
  }

  // Next page: on to the following page; this page's settings stay as they are (the texture pack and PS2 cutscene
  // pages: nothing downloaded, as with Skip or No thanks).
  void SetupNextPage() {
    const int stage = SetupStage();
    ui::Cue(ui::Sound::kSelect);
    REXLOG_INFO("KK: setup: Next page from setup stage {} (to {})", stage,
                setup_progress::Next(stage, movies::PlaybackAvailable()));
    if (SettingsSetupStage(stage)) {
      const int page = SetupSettingsPage();
      if (page + 1 < kSettingsSetupPages) SetupSettingsGo(page + 1);
      else FinishSetupSettings();
    } else if (stage == kPromptsStage) {
      FinishSetupPrompts();
    } else if (stage == 6 || stage == 7) {
      FinishSetupContentStep(stage, "next page");
    } else if (stage == kWorkshopStage) {
      FinishSetupWorkshop();
    }
  }

  void AskSkipAll() {
    REXLOG_INFO("KK: setup: Skip all asked on setup stage {}", SetupStage());
    OpenModal("Skip all",
              T(std::string("If you do not configure your game correctly, you may have a worse experience.")) + " " +
                  T(std::string("You can change all of these settings later from Settings on the main menu.")),
              {"Cancel", "Skip all"}, 0, [this](int button) {
                if (button == 1) SkipAllSetup();
                else REXLOG_INFO("KK: setup: Skip all cancelled");
              });
  }

  // Skip all: straight to the end of the setup. Everything not chosen keeps what it has: the defaults the setup
  // started with (StartFullSetup: new installs and the setup again get the hardware recommendation's preset and
  // Remastered), the choices made on the pages already seen, no texture pack or PS2 cutscene download.
  void SkipAllSetup() {
    REXLOG_INFO("KK: setup: Skip all from setup stage {}", SetupStage());
    setup_shaders_ = false;
    setup_settings_ = setup_prompts_ = setup_textures_ = setup_cutscenes_ = setup_workshop_ = false;
    sub_ = kNoSubPage;
    if (content_watch_setup_ != 0) content_watch_setup_ = 0;
    FinishSetup();
  }

  // Auto detect (task 365): every setting of the Settings pages and the button prompts at what this PC is
  // recommended, exactly as a new install's setup starts (StartFullSetup for the setup again): the Display, Graphics
  // and Controls settings and the button prompts back to their defaults, the hardware found again (graphics card,
  // processor, memory, screen) for its tier and render resolution, Remastered (Strong GTAO, 60 FPS), and the tier's
  // named preset. Then the texture pack page, and the setup goes on from there.
  void AutoDetectSetup() {
    const int from = SetupStage();
    ui::Cue(ui::Sound::kSelect);
    REXLOG_INFO("KK: setup: Auto detect on setup stage {}", from);
    ResetForSetupAgain();
    ApplyRemastered(true);
    ApplyRecommendedPreset();
    SaveSettings(paths_.config_path);
    REXLOG_INFO("KK: setup: Auto detect chose: native_backend={} tier {} preset '{}' render {} upscaler {} / {} "
                "native_gi={} ao_strength={} kk_frame_rate={} kk_fov={} fullscreen={} kk_button_prompts={} mnk_mode={}",
                FindFlag("native_backend") ? Get("native_backend") : std::string("-"), Get("kk_hardware_tier"),
                Get("kk_graphics_preset"), Get("kk_render_quality"),
                FindFlag("native_upscaler") ? Get("native_upscaler") : std::string("-"), Get("present_effect"),
                FindFlag("native_gi") ? Get("native_gi") : std::string("-"), Get("ao_strength"), Get("kk_frame_rate"),
                Get("kk_fov"), Get("fullscreen"), Get("kk_button_prompts"), Get("mnk_mode"));
    setup_settings_ = setup_prompts_ = false;
    sub_ = kNoSubPage;
    end_page_ = -1;
    setup_textures_ = true;
    setup_cutscenes_ = movies::PlaybackAvailable();
    setup_workshop_ = true;
    setup_all_set_ = true;
    setup_focus_ = 0;
#if defined(KK_DEV_TOOLS)
    if (dev_setup_ >= 0) dev_setup_ = 6;
#endif
    Status(T(std::string("Auto detect chose the recommended settings for this PC.")));
  }
