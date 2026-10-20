  // ------------------------------------------------------------ setup: the Settings pages ---
  // Task 332 (owner, 20 Oct, DECISIONS row A): the first-time setup's order is now game files, shaders, then the
  // FULL Settings pages Display, Graphics and Controls (SetupStage 16 to 18), button prompts (12), the HD texture
  // pack (6), the PS2 cutscenes (7), the Workshop (15) and You're all set (13). The three Settings pages are built
  // by the same code as Settings (SettingsPageItems: DisplayItems, GraphicsItems, ControlsItems, in the order of
  // settings_layout.h), with the same right-hand description panel, and Next / Back rows under them, so a setting
  // added to one of those pages shows in the setup by itself. No recommended-settings button and no pictures there.
  // Every page after the game files has "Skip all" at the top (Y, R or a click): a pop-up, then straight to the
  // startup movies (StartStartupMovies), with every setting not chosen left as it is.
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
    const bool last = page == kSettingsSetupPages - 1;
    items.push_back(ActionItem("Next", "The next page.",
                               last ? "" : F("{0} of {1}", {std::to_string(page + 2), std::to_string(kSettingsSetupPages)}),
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
    ui::DrawTracked(dl, f.display, 13 * s, ImVec2(x0 + 2 * s, y), color::kTextDim, caption, 3 * s);
    {
      float px = x0 + 2 * s + ui::TrackedWidth(f.display, 13 * s, caption, 3 * s) + 18 * s;
      for (int i = 0; i < kSettingsSetupPages; ++i, px += 26 * s)
        dl->AddRectFilled(ImVec2(px, y + 5 * s), ImVec2(px + 20 * s, y + 8 * s),
                          i == page ? color::kAccent : i < page ? WithAlphaText(0.45f) : color::kHairline);
    }
    y += 24 * s;
    const std::string title = sub_ == kRemapPage ? T(std::string("Button remapping"))
                              : sub_ == kKeysPage ? T(std::string("Keyboard bindings"))
                                                  : T(std::string(kPageNames[settings_page]));
    dl->AddText(f.display, 30 * s, ImVec2(x0, y), color::kText, title.c_str());
    DrawSkipAll(dl);

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
  // Setup step 9 (task 332): what the Workshop on the main menu is, in a few plain words.
  void DrawSetupWorkshop(ImDrawList* dl) {
    const float s = S();
    const float x0 = o_.x + Margin(), right = o_.x + w_ - Margin();
    const float list_w = std::round(std::min(660 * s, (right - x0) * 0.5f));
    const float top = o_.y + std::max(40 * s, h_ * 0.06f);
    const std::string intro = std::string(T("The Workshop is on the main menu.")) + " " +
                              T("It has mods and texture packs for King Kong, and other projects from TRG Studios.");
    float y = DrawEndHeader(dl, "Workshop", "The Workshop", intro, x0, list_w);
    y += 6 * s;
    DrawSkipAll(dl);
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
    // The right side: a download's strip (when one runs), and one of the game's stills, framed.
    float ry = top;
    if (DrawContentStrip(dl, 1.0f, true)) ry = o_.y + std::max(84 * s, h_ * 0.12f) + 66 * s + 18 * s;
    DrawSetupStill(dl, x0 + list_w + 56 * s, right, ry);
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

  // ---------------------------------------------------------------------------- setup: Skip all ---
  // Every page of the first-time setup after the game files (the shader screen and every page after it).
  bool SkipAllShown() const {
#if defined(KK_DEV_TOOLS)
    if (dev_setup_ >= 0)
      return dev_setup_ == 4 || (AskingSetupStage(dev_setup_) && dev_setup_ != kNewStage && !GraphicsSetupStage(dev_setup_));
#endif
    if (!files_ok_ || installing_ || setup_art_ || installing_update_ || update_shaders_ || update_new_) return false;
    if (rebuild_shaders_screen_ && !setup_shaders_) return false;
    return setup_shaders_ || setup_settings_ || setup_prompts_ || setup_textures_ || setup_cutscenes_ ||
           setup_workshop_ || setup_all_set_;
  }

  // "Skip all" at the top right, above the pages' own panels: its button picture and name, clicked or pressed (Y / R).
  void DrawSkipAll(ImDrawList* dl) {
    if (!SkipAllShown()) return;
    const float s = S();
    const Action clicked = ui::DrawPrompts(dl, glyphs_, PromptSet(), {{Action::kResetPage, "Skip all"}},
                                           o_.x + w_ - Margin(), o_.y + 20 * s, m_, !modal_.open);
    if (modal_.open || !capturing_.empty()) return;
    if (clicked == Action::kResetPage || input_.Pressed(Action::kResetPage)) {
      input_.Consume(Action::kResetPage);
      ui::Cue(ui::Sound::kSelect);
      REXLOG_INFO("KK: setup: Skip all asked on setup stage {}", SetupStage());
      OpenModal("Skip all", T(std::string("You can change all of these settings later from Settings on the main menu.")),
                {"OK"}, -1, [this](int) { SkipAllSetup(); });
    }
  }

  // Straight to the end of the setup: the shaders go on in the background if they are still being prepared, and
  // everything not chosen keeps what it has (new installs: the Remastered defaults StartFullSetup set).
  void SkipAllSetup() {
    REXLOG_INFO("KK: setup: Skip all from setup stage {}{}", SetupStage(),
                setup_shaders_ && native_pack_.Busy() ? " (the shaders carry on in the background)" : "");
    setup_shaders_ = false;
    setup_settings_ = setup_prompts_ = setup_textures_ = setup_cutscenes_ = setup_workshop_ = false;
    sub_ = kNoSubPage;
    if (content_watch_setup_ != 0) content_watch_setup_ = 0;
    FinishSetup();
  }
