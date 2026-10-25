  // ------------------------------------------------------ Play Online Co-op ---
  // C81: the co-op pages under Home (PlayView::kCoop), in the Play pages' look, the scene playing behind.
  // Their rows and actions are the Social drawer's (CoopPageRows, SocialPanel::PageActivate); the drawer
  // stays the quick view and its Host and Join buttons open these pages.
  using CoopPage = coop::CoopPage;

  bool CoopEnabled() { return GetBool("kk_coop_enabled"); }

  void OpenCoop(CoopPage page) {
    if (!CoopEnabled()) return;
    GoTo(kHome);
    status_focus_ = false;
    ReadSaves();
    play_ = PlayView::kCoop;
    CoopGo(page);
  }

  void CoopGo(CoopPage page) {
    if (page == CoopPage::Join && coop_page_ != CoopPage::Join) social_.PrepareDirect(2);
    if (page == CoopPage::HostHow) social_.PrepareDirect(1);
    if (page != coop_page_) {
      coop_lists_[int(page)] = {};
      REXLOG_INFO("KK co-op pages: {}", CoopPageName(page));
    }
    coop_page_ = page;
  }

  static const char* CoopPageName(CoopPage page) {
    switch (page) {
      case CoopPage::Hub: return "Play Online Co-op";
      case CoopPage::Host: return "Host a game";
      case CoopPage::HostNew: return "New game";
      case CoopPage::HostLoad: return "Load game";
      case CoopPage::HostHow: return "Invite";
      case CoopPage::Join: return "Join a game";
      case CoopPage::Lobby: return "Lobby";
    }
    return "";
  }

  // Every frame: the chapter or save the host picked before the lobby existed goes into the lobby as soon as
  // it can, and a game Steam started from an invite (+connect_lobby) goes to the lobby page after setup.
  void CoopTick() {
    if (!CoopEnabled()) return;
    const auto model = coop::Social().Snapshot();
    if (coop_choice_ && model.lobby && model.owner == model.self && !model.start_requested) {
      if (coop::SelectSessionStart(*coop_choice_)) {
        REXLOG_INFO("KK co-op pages: the host's choice went into the new lobby");
        coop_choice_.reset();
      }
    }
    if (!model.lobby && model.status != coop::SocialStatus::Creating && model.status != coop::SocialStatus::Joining &&
        coop_choice_ && play_ != PlayView::kCoop)
      coop_choice_.reset();  // the host left the pages without a lobby
    if (!coop_startup_checked_ && !InSetup() && !ShaderScreen() && !loop_ && !MoviesPending()) {
      coop_startup_checked_ = true;
      if (const coop::SocialId lobby = coop::StartupInvite(model)) {
        REXLOG_INFO("KK co-op pages: started by a Steam invite; joining it on the lobby page");
        OpenCoop(CoopPage::Lobby);
        coop::Social().Act({coop::SocialAction::Accept, lobby});
      }
    }
  }

  std::string CoopChoiceText(const coop::LobbyChoice& choice) {
    if (!choice.valid()) return T("Not chosen yet");
    const std::string name = T(ChapterName(choice.chapter));
    return choice.continue_game ? std::string(T("Load game")) + ": " + name : name;
  }
  std::string CoopRequestText(const play::StartRequest& r) {
    if (r.kind == play::StartKind::kContinue && SlotUsed(r.slot))
      return std::string(T("Load game")) + ": " + SaveName(saves_[size_t(r.slot)]) + "  " KK_DOT "  " +
             T(ChapterName(ContinueChapter(saves_[size_t(r.slot)])));
    return T(ChapterName(r.chapter));
  }
  static const char* PolicyName(int chapter) {
    const auto policy = coop::kChapterRules[size_t(chapter)].policy;
    return policy == coop::ChapterPolicy::Together ? "Together"
           : policy == coop::ChapterPolicy::SoloRegroup ? "Separate then regroup" : "Watch";
  }

  // The host's choice: before a lobby it is kept for Invite; in a lobby (Change) it replaces the lobby's.
  void CoopChoose(const play::StartRequest& request) {
    const auto model = coop::Social().Snapshot();
    if (model.lobby && model.owner == model.self) {
      if (coop::SelectSessionStart(request)) {
        Status("Chapter changed. Both players must be ready again.");
        CoopGo(CoopPage::Lobby);
      } else {
        ui::Cue(ui::Sound::kDeny);
        Status("Could not select chapter");
      }
      return;
    }
    coop_choice_ = request;
    REXLOG_INFO("KK co-op pages: host picked {} {}", request.kind == play::StartKind::kContinue ? "save slot" : "chapter",
                request.kind == play::StartKind::kContinue ? request.slot : request.chapter);
    CoopGo(CoopPage::HostHow);
  }

  void CoopActivate(const coop::SocialRow& row) {
    using K = coop::SocialRowKind;
    auto& service = coop::Social();
    const auto model = service.Snapshot();
    switch (row.key.kind) {
      case K::CoopHost: coop_choice_.reset(); CoopGo(CoopPage::Host); return;
      case K::CoopJoin: CoopGo(CoopPage::Join); return;
      case K::HostNewGame: CoopGo(CoopPage::HostNew); return;
      case K::HostLoadGame: CoopGo(CoopPage::HostLoad); return;
      case K::ChangeChoice: CoopGo(CoopPage::Host); return;
      case K::AddSteam: OpenSteamPrompt(); return;
      case K::JoinFriend: {
        const auto command = coop::CoopRowCommand(row, model, coop_page_);
        if (!command || !service.Act(*command)) {
          ui::Cue(ui::Sound::kDeny);
          Status("Could not join lobby");
        }
        return;
      }
      case K::DirectAddress: case K::DirectPort: case K::DirectCode: case K::DirectHost: case K::DirectJoin:
        social_.PageActivate(row, service, true, coop_page_ == CoopPage::Join ? 2 : 1);
        return;
      case K::Leave:
        if (!model.lobby) {  // cancel a lobby still being created or joined
          service.Act({coop::SocialAction::Leave});
          CoopGo(CoopPage::Hub);
          return;
        }
        social_.PageActivate(row, service, false, 0);
        return;
      default:
        social_.PageActivate(row, service, false, 0);  // Connect to Steam, Host with Steam friends, invites, Ready, Start
        return;
    }
  }

  static const char* CoopRowHelp(coop::SocialRowKind kind) {
    using K = coop::SocialRowKind;
    switch (kind) {
      case K::CoopHost: return "Choose a new game or one of your saves, then invite a friend.";
      case K::CoopJoin: return "Accept a Steam invite, join a friend who is playing now, or enter the host's code.";
      case K::ConnectSteam: return "Starts Steam for this game. Steam friends and invites need it.";
      case K::AddSteam: return "Adds King Kong to your Steam library, so friends can join you from Steam.";
      case K::HostNewGame: return "Choose a chapter to play together.";
      case K::HostLoadGame: return "Continue one of your saves together. Your partner plays it with you.";
      case K::Create: return "Opens a lobby only your Steam friends can join. Then invite a friend.";
      case K::DirectPort: return "The UDP port your partner connects to. Forward it on your router.";
      case K::DirectCode: return "Your partner needs this 6 character code to join.";
      case K::DirectHost: return "Opens your game for your partner on this port.";
      case K::DirectAddress: return "The host's IP address.";
      case K::DirectJoin: return "Connects to the host with this IP address, port and code.";
      case K::JoinFriend: return "Joins this friend's lobby.";
      case K::ChangeChoice: return "Choose another chapter or save. Both players must be ready again.";
      case K::Ready: return "Both players choose Ready, then the host starts.";
      case K::Start: return "Both games restart into the chapter and connect again.";
      case K::Leave: return "Leaves this lobby.";
      case K::OverlayInvite: return "Opens Steam's invite window.";
      default: return "";
    }
  }

  std::vector<Item> CoopItems(const std::vector<coop::SocialRow>& rows, const coop::SocialModel& model) {
    using K = coop::SocialRowKind;
    std::vector<Item> items;
    for (const auto& row : rows) {
      const auto kind = row.key.kind;
      Item it;
      it.type = kind == K::Section ? ItemType::kSection : kind == K::Text ? ItemType::kText
                : kind == K::Member ? ItemType::kInfo : ItemType::kAction;
      it.label = row.label;
      it.description = CoopRowHelp(kind);
      it.disabled = !row.enabled && it.type == ItemType::kAction;
      it.focus_subtitle_color = color::kOnFocus;
      if (kind == K::Text && !row.detail.empty()) {
        it.type = ItemType::kInfo; it.tall = true; it.literal_subtitle = true;
        it.subtitle = row.key.id == 13 ? std::string(T(row.detail)) : row.detail;
      }
      if (kind == K::DirectAddress || kind == K::DirectPort || kind == K::DirectCode) {
        it.tall = true; it.literal_subtitle = true;
        it.subtitle = kind == K::DirectAddress ? social_.direct_address()
                      : kind == K::DirectPort ? social_.direct_port() : social_.direct_code();
        if (it.subtitle.empty()) it.subtitle = T("Not entered yet");
      }
      if (kind == K::CoopHost || kind == K::CoopJoin || kind == K::HostNewGame || kind == K::HostLoadGame ||
          kind == K::Create) {
        it.tall = true;
        it.subtitle = kind == K::CoopHost ? "New game or Load game, then invite"
                      : kind == K::CoopJoin ? "Invites, friends playing now, or an IP code"
                      : kind == K::HostNewGame ? "Choose a chapter"
                      : kind == K::HostLoadGame ? "Choose one of your saves" : "Friends only Steam lobby";
      }
      if (kind == K::ChangeChoice) {
        it.tall = true; it.literal_subtitle = true;
        it.subtitle = CoopChoiceText(model.choice);
        if (model.owner != model.self) it.type = ItemType::kInfo, it.disabled = false, it.description.clear();
        else it.value_text = "Change";
      }
      if (kind == K::Ready) {  // the drawer names the toggle by what it does; here: Ready, with the current state
        const bool ready = row.label == "Not ready";
        it.label = "Ready";
        it.value_text = ready ? "Ready" : "Not ready";
        it.value_color = ready ? color::kGood : color::kTextDim;
      }
      if (kind == K::Member) {
        it.tall = true; it.literal_label = true; it.literal_subtitle = true;
        it.value_text = row.detail;
        it.value_color = row.detail == "Ready" ? color::kGood : color::kTextDim;
        if (row.key.id == model.owner) it.subtitle = T("Host");
        if (row.key.id == model.self) it.subtitle += (it.subtitle.empty() ? "" : " / ") + std::string(T("You"));
      }
      if (kind == K::Friend || kind == K::JoinFriend) {
        it.tall = true; it.literal_label = true;
        it.subtitle = row.detail;
        it.value_text = kind == K::Friend ? "Invite" : "Join";
        it.value_action = kind == K::Friend ? Action::kDefault : Action::kAccept;
        it.value_color = color::kText;
      }
      if (kind == K::Accept || kind == K::Decline) {
        it.tall = true;
        it.subtitle = row.detail.empty() ? "Friend" : row.detail;
        it.literal_subtitle = !row.detail.empty();
      }
      if (it.type == ItemType::kAction) it.on_activate = [this, row] { CoopActivate(row); };
      items.push_back(std::move(it));
    }
    return items;
  }

  // New game (a chapter) and Load game (a save) for the host.
  std::vector<Item> CoopChapterItems() {
    std::vector<Item> items;
    row_data_.clear();
    const auto& chapters = play::Chapters();
    constexpr size_t kGroup = 7;
    for (size_t i = 0; i < chapters.size(); ++i) {
      const int entry = chapters[i].entry;
      if (i % kGroup == 0) {
        const size_t last = std::min(chapters.size(), i + kGroup);
        items.push_back(Section(F("Chapters {0} to {1}", {std::to_string(i + 1), std::to_string(last)})));
        row_data_.push_back(-1);
      }
      Item it;
      it.type = ItemType::kAction;
      it.label = ChapterName(entry);
      it.value_text = PolicyName(entry);
      it.value_color = color::kTextDim;
      it.on_activate = [this, entry] {
        int slot = ChapterSlot();
        if (slot < 0) slot = std::max(0, FirstEmptySlot());
        CoopChoose({play::StartKind::kChapter, slot, entry, ""});
      };
      items.push_back(std::move(it));
      row_data_.push_back(entry);
    }
    return items;
  }
  std::vector<Item> CoopSaveItems() {
    std::vector<Item> items;
    row_data_.clear();
    items.push_back(Section("Saves"));
    row_data_.push_back(-1);
    for (const play::SaveSlot& sv : saves_) {
      if (!sv.exists) continue;
      Item it = SlotItem(sv);
      it.description = "Play this save together. Your partner plays it with you.";
      const int slot = sv.index;
      it.on_activate = [this, slot] { CoopChoose({play::StartKind::kContinue, slot, -1, ""}); };
      items.push_back(std::move(it));
      row_data_.push_back(slot);
    }
    if (items.size() == 1) items.push_back(Section("No saves yet."));
    return items;
  }

  // The status line at the top of the right side: what is going on, or what to do here.
  std::string CoopStatusLine(const coop::SocialModel& model, bool* warn) {
    *warn = false;
    if (coop_page_ == CoopPage::Lobby) {
      const auto st = coop::CoopLobbyStatus(model, social_.invited());
      *warn = !model.last_error.empty();
      if (const auto n = social_.notice(); !n.empty() && model.last_error.empty()) return T(n);
      if (std::string_view(st.key) == "Versions don't match: {0}") return F(st.key, {T(model.last_error)});
      return std::string_view(st.key).find("{0}") != std::string_view::npos
                 ? F(st.key, {st.name.empty() ? std::string(T("your partner")) : st.name})
                 : std::string(T(st.key));
    }
    if (const auto n = social_.notice(); !n.empty()) return *warn = true, std::string(T(n));
    if (!model.last_error.empty() && !model.direct) {
      *warn = true;
      const std::string_view key = coop::SocialStatusKey(model);
      return key == "Versions don't match: {0}" ? F(key.data(), {T(model.last_error)}) : std::string(T(key.data()));
    }
    switch (coop_page_) {
      case CoopPage::Hub: return T("Host a game for a friend, or join one.");
      case CoopPage::Host: return T(model.lobby ? "Choose another chapter or save." : "Choose what to play together.");
      case CoopPage::HostNew: return T("Choose the chapter to play together.");
      case CoopPage::HostLoad: return T("Choose the save to continue together.");
      case CoopPage::HostHow: return T("Host with Steam friends, or by IP with a 6 character code.");
      case CoopPage::Join: return T("Accept an invite, join a friend, or enter the host's IP and code.");
      default: return "";
    }
  }

  void DrawCoopPage(ImDrawList* dl) {
    const float s = S();
    const ui::Fonts f = ui::GetFonts();
    auto& service = coop::Social();
    const auto model = service.Snapshot();
    const CoopPage next = coop::CoopPageFor(model, coop_page_);
    if (next != coop_page_) CoopGo(next);
    ui::ListState& st = coop_lists_[int(coop_page_)];
    std::vector<coop::SocialRow> rows;
    std::vector<Item> items;
    if (coop_page_ == CoopPage::HostNew) items = CoopChapterItems();
    else if (coop_page_ == CoopPage::HostLoad) items = CoopSaveItems();
    else {
      rows = coop::CoopPageRows(model, coop_page_, {UsedSlots() > 0, coop_choice_.has_value()});
      items = CoopItems(rows, model);
      row_data_.clear();
    }
    // A page opens on its first row that can be pressed (not a greyed Steam row while Steam is off).
    if (st.focus < 0)
      for (size_t i = 0; i < items.size(); ++i)
        if (items[i].type == ItemType::kAction && !items[i].disabled) { st.focus = int(i); break; }
    const bool editing = social_.editing() && !social_.blocking();
    const bool interactive = !PageInputBlocked() && !editing;
    // X invites the focused friend, as in the drawer.
    if (interactive && !rows.empty() && st.focus >= 0 && st.focus < int(rows.size()) &&
        rows[size_t(st.focus)].key.kind == coop::SocialRowKind::Friend && input_.Pressed(Action::kDefault)) {
      input_.Consume(Action::kDefault);
      if (rows[size_t(st.focus)].enabled) { ui::Cue(ui::Sound::kSelect); CoopActivate(rows[size_t(st.focus)]); }
      else ui::Cue(ui::Sound::kDeny);
    }
    const float top = o_.y + 104 * s, bottom = o_.y + h_ - 96 * s;
    const float x0 = o_.x + Margin(), avail = w_ - Margin() * 2, list_w = std::round(avail * 0.56f);
    const Item* focused = ui::DrawList(dl, items, st, ImVec2(x0, top), ImVec2(x0 + list_w, bottom), m_, input_, interactive);
    // The right side: the status line, the chosen chapter (host pages and the lobby), then the focused row's help.
    const float rx = x0 + list_w + 76 * s, rw = o_.x + w_ - Margin() - rx;
    float y = top + 18 * s;
    bool warn = false;
    const std::string line = CoopStatusLine(model, &warn);
    if (!line.empty()) {
      dl->AddText(f.semibold, 19 * s, ImVec2(rx, y), warn ? color::kWarn : color::kText, line.c_str(), nullptr, rw);
      y += ui::TextSize(f.semibold, 19 * s, line, rw).y + 22 * s;
    }
    int entry = -1;
    std::string chosen;
    if (coop_page_ == CoopPage::HostNew && st.focus >= 0 && st.focus < int(row_data_.size())) entry = row_data_[size_t(st.focus)];
    else if (coop_page_ == CoopPage::HostLoad && st.focus >= 0 && st.focus < int(row_data_.size()) && SlotUsed(row_data_[size_t(st.focus)]))
      entry = ContinueChapter(saves_[size_t(row_data_[size_t(st.focus)])]);
    else if (coop_page_ == CoopPage::Lobby && model.choice.valid()) entry = model.choice.chapter, chosen = CoopChoiceText(model.choice);
    else if (coop_page_ == CoopPage::HostHow && coop_choice_) {
      entry = coop_choice_->kind == play::StartKind::kContinue && SlotUsed(coop_choice_->slot)
                  ? ContinueChapter(saves_[size_t(coop_choice_->slot)]) : coop_choice_->chapter;
      chosen = CoopRequestText(*coop_choice_);
    }
    if (entry >= 0 && entry < int(play::Chapters().size())) {
      float aspect = 16.0f / 9.0f;
      std::filesystem::path path;
      for (const play::ChapterInfo& c : play::Chapters()) if (c.entry == entry) path = c.picture;
      auto* picture = ChapterPicture(path, &aspect);
      const float ph = std::min(rw / std::max(0.1f, aspect), (bottom - y) * 0.42f), pw = ph * aspect;
      dl->AddRectFilled(ImVec2(rx, y), ImVec2(rx + pw, y + ph), ui::WithAlpha(color::kPanel, 0.82f));
      if (picture) dl->AddImage(ui::Tex(picture), ImVec2(rx, y), ImVec2(rx + pw, y + ph));
      dl->AddRectFilled(ImVec2(rx, y), ImVec2(rx + 3 * s, y + ph), color::kAccent);
      y += ph + 14 * s;
      const std::string name = chosen.empty() ? std::string(T(ChapterName(entry))) : chosen;
      dl->AddText(f.display, 22 * s, ImVec2(rx, y), color::kText, name.c_str(), nullptr, rw);
      y += ui::TextSize(f.display, 22 * s, name, rw).y + 8 * s;
      const std::string policy = std::string(T("Co-op")) + ": " + T(PolicyName(entry));
      dl->AddText(f.text, 15.5f * s, ImVec2(rx, y), color::kAccent, policy.c_str());
      y += 30 * s;
    }
    if (focused && coop_page_ != CoopPage::HostNew) ui::DrawDescription(dl, focused, ImVec2(rx, y), ImVec2(rx + rw, bottom), m_);
    focused_items_ = std::move(items);
    focused_ = st.focus >= 0 && st.focus < int(focused_items_.size()) ? &focused_items_[size_t(st.focus)] : nullptr;
    coop_focus_kind_ = st.focus >= 0 && st.focus < int(rows.size()) ? rows[size_t(st.focus)].key.kind : coop::SocialRowKind::Section;
    if (editing) {
      // The drawer's text editor (keyboard, controller keys, mouse), in a panel over the page.
      const float ew = std::min(490 * s, w_), ex = o_.x + (w_ - ew) * 0.5f;
      dl->AddRectFilled(o_, ImVec2(o_.x + w_, o_.y + h_), IM_COL32(0, 0, 0, 170));
      social_.DrawPageEditor(dl, ImVec2(ex, o_.y), ImVec2(ew, h_), m_, input_, glyphs_, PromptSet(), service);
    }
  }

  // B: one page back (the lobby and the first page go back to Home; the lobby itself stays open).
  void CoopBack() {
    const bool in_lobby = coop::Social().Snapshot().lobby != 0;
    switch (coop_page_) {
      case CoopPage::Hub: case CoopPage::Lobby: play_ = PlayView::kNone; return;
      case CoopPage::Host: CoopGo(in_lobby ? CoopPage::Lobby : CoopPage::Hub); return;
      case CoopPage::HostNew: case CoopPage::HostLoad: CoopGo(CoopPage::Host); return;
      case CoopPage::HostHow: CoopGo(CoopPage::Host); return;
      case CoopPage::Join: CoopGo(CoopPage::Hub); return;
    }
  }

  std::vector<ui::Prompt> CoopPrompts() const {
    if (social_.editing()) return {};
    std::vector<ui::Prompt> p;
    if (focused_ && focused_->type == ItemType::kAction && !focused_->disabled && focused_->on_activate)
      p.push_back({Action::kAccept, "Select"});
    if (coop_focus_kind_ == coop::SocialRowKind::Friend && focused_ && !focused_->disabled)
      p.push_back({Action::kDefault, "Invite"});
    p.push_back({Action::kBack, "Back"});
    return p;
  }

#if defined(KK_DEV_TOOLS)
  // Developer input: "coop:<page>" opens a page; "coop:row:<n>" selects the focused page's n-th focusable row.
  bool CoopDevInput(const std::string& command) {
    static const std::pair<const char*, CoopPage> pages[] = {
        {"hub", CoopPage::Hub}, {"host", CoopPage::Host}, {"new", CoopPage::HostNew}, {"load", CoopPage::HostLoad},
        {"how", CoopPage::HostHow}, {"join", CoopPage::Join}, {"lobby", CoopPage::Lobby}};
    for (const auto& [name, page] : pages)
      if (command == name) { if (play_ != PlayView::kCoop) OpenCoop(page); else CoopGo(page); return true; }
    // "focus:<row>" moves the page's focus to that row (as Up and Down would; then pad:a presses it),
    // "wait-enabled:<row>" waits until that row can be pressed.
    static const std::pair<const char*, coop::SocialRowKind> kinds[] = {
        {"host-ip", coop::SocialRowKind::DirectHost}, {"join-ip", coop::SocialRowKind::DirectJoin},
        {"ready", coop::SocialRowKind::Ready}, {"start", coop::SocialRowKind::Start},
        {"leave", coop::SocialRowKind::Leave}, {"load", coop::SocialRowKind::HostLoadGame},
        {"new", coop::SocialRowKind::HostNewGame}, {"join", coop::SocialRowKind::CoopJoin}};
    for (const bool focus : {true, false}) {
      const std::string prefix = focus ? "focus:" : "wait-enabled:";
      if (command.rfind(prefix, 0) != 0 || play_ != PlayView::kCoop) continue;
      const auto model = coop::Social().Snapshot();
      const auto rows = coop::CoopPageRows(model, coop_page_, {UsedSlots() > 0, coop_choice_.has_value()});
      for (const auto& [name, kind] : kinds) {
        if (command.substr(prefix.size()) != name) continue;
        for (size_t i = 0; i < rows.size(); ++i) {
          if (rows[i].key.kind != kind) continue;
          if (focus) coop_lists_[int(coop_page_)].focus = int(i), coop_lists_[int(coop_page_)].focus_label = rows[i].label;
          return focus || rows[i].enabled;
        }
      }
      return false;
    }
    if (command.rfind("wait:", 0) == 0) {
      for (const auto& [name, page] : pages) if (command.substr(5) == name) return play_ == PlayView::kCoop && coop_page_ == page;
      return false;
    }
    return false;
  }
#endif
