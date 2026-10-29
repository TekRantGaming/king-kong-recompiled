  // ------------------------------------------------------ Play Online Co-op ---
  // C81: the co-op pages under Home (PlayView::kCoop), the scene playing behind.
  // UI1 (29 Oct, Craig picked Design 1 "Clear steps", mockups\coop-ui-2026-10-29\d1-*): Home with two large
  // cards, Host and Join, a Steam invite banner and Rejoin; a step bar (choose what to play, invite a friend,
  // both ready then start) over the host pages and the lobby; Join in three columns (invites, friends playing
  // now, join with a code). Every action is still a Social row (CoopPageRows) checked against the latest
  // snapshot and run by CoopActivate / SocialPanel::PageActivate, as before. Before the pages, every time: the
  // early access warning (Craig, 29 Oct), which nothing closes for its first 5 seconds.
  using CoopPage = coop::CoopPage;

  bool CoopEnabled() { return GetBool("kk_coop_enabled"); }

  // One focusable thing on a page: a card, a row, a button or a field. Pages list them every frame.
  struct CoopTarget {
    std::string key;
    ImVec2 a, b;
    bool enabled = true;
    int group = 0;  // Join: its column (LB and RB move between them)
    std::function<void()> press;
  };
  // The rejoin record of a game that was cut off, as Home's Rejoin row shows it (read at most once a second).
  struct CoopRejoin {
    bool ok = false;
    std::string partner;
    int chapter = -1;
    long long written_ms = 0;
    double read_at = -1;
  };
  static constexpr float kCoopPi = 3.14159265f;

  void OpenCoop(CoopPage page) {
    if (!CoopEnabled()) return;
    GoTo(kHome);
    status_focus_ = false;
    ReadSaves();
    play_ = PlayView::kCoop;
    CoopGo(page);
    coop_gate_.Open(ImGui::GetTime());
    coop_gate_focus_ = "continue";
    REXLOG_INFO("KK co-op pages: early access warning shown");
  }

  void CoopGo(CoopPage page) {
    if (page == CoopPage::Join && coop_page_ != CoopPage::Join) social_.PrepareDirect(2);
    if (page == CoopPage::HostHow) social_.PrepareDirect(1);
    if (page != coop_page_) {
      coop_lists_[int(page)] = {};
      coop_focus_.erase(int(page));
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
  // it can, and a game Steam started from an invite (+connect_lobby) goes to the lobby page after setup (after
  // the early access warning: the invite is accepted when the player continues).
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
    if (play_ != PlayView::kCoop && coop_gate_.open()) coop_gate_.Close(), coop_gate_lobby_ = 0;
    if (!coop_startup_checked_ && !InSetup() && !ShaderScreen() && !loop_ && !MoviesPending()) {
      coop_startup_checked_ = true;
      if (const coop::SocialId lobby = coop::StartupInvite(model)) {
        REXLOG_INFO("KK co-op pages: started by a Steam invite; joining it on the lobby page after the warning");
        OpenCoop(CoopPage::Lobby);
        coop_gate_lobby_ = lobby;
      }
    }
  }

  std::string CoopChoiceText(const coop::LobbyChoice& choice) {
    if (!choice.valid()) return T("Not chosen yet");
    const std::string name = T(ChapterName(choice.chapter));
    return choice.continue_game ? std::string(T("Load game")) + ": " + name : name;
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

  // New game (a chapter) and Load game (a save) for the host: the launcher's lists, each chapter with its co-op
  // mode in plain words.
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
      it.value_text = coop::CoopModeName(entry);
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

  // What is going wrong, or the latest action's notice (warn), else empty. The lobby has its own line.
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
    return "";
  }

  // ------------------------------------------------------------ drawing kit ---
  // The mockups are 1920 x 1080 pixels; the launcher is laid out for 1280 x 720 and scaled with the window.
  float U(float px) const { return px * S() / 1.5f; }
  static constexpr ImU32 kCoopHair = IM_COL32(255, 255, 255, 36);

  static std::string CoopPlainVersion() {  // "2.0.0" (a beta's "-beta.17" as " beta 17": no dashes on screen)
    std::string v = KK_VERSION;
    for (char& c : v) if (c == '-') c = ' ';
    if (const size_t b = v.find(" beta."); b != std::string::npos) v[b + 5] = ' ';
    return v;
  }

  // A chapter's picture filling a box (cropped to it), darkened by `light` (1: as it is).
  void CoopPicture(ImDrawList* dl, int entry, ImVec2 a, ImVec2 b, float light = 0.8f) {
    dl->AddRectFilled(a, b, IM_COL32(20, 23, 28, 255));
    std::filesystem::path path;
    for (const play::ChapterInfo& c : play::Chapters()) if (c.entry == entry) path = c.picture;
    float aspect = 16.0f / 9.0f;
    auto* picture = ChapterPicture(path, &aspect);
    if (!picture || b.x <= a.x || b.y <= a.y) return;
    const float box = (b.x - a.x) / (b.y - a.y);
    ImVec2 uv0(0, 0), uv1(1, 1);
    if (aspect > box) {
      const float u = box / aspect;
      uv0.x = (1 - u) * 0.5f, uv1.x = uv0.x + u;
    } else {
      const float v = aspect / box;
      uv0.y = (1 - v) * 0.5f, uv1.y = uv0.y + v;
    }
    const int l = int(255 * std::clamp(light, 0.0f, 1.0f));
    dl->AddImage(ui::Tex(picture), a, b, uv0, uv1, IM_COL32(l, l, l, 255));
  }
  static int CoopChapterNumber(int entry) {
    const auto& chapters = play::Chapters();
    for (size_t i = 0; i < chapters.size(); ++i) if (chapters[i].entry == entry) return int(i) + 1;
    return entry + 1;
  }

  void CoopDashedRect(ImDrawList* dl, ImVec2 a, ImVec2 b, ImU32 col) {
    const float s = S(), dash = 6 * s, gap = 4 * s;
    auto line = [&](ImVec2 p, ImVec2 q) {
      const float len = std::max(std::abs(q.x - p.x), std::abs(q.y - p.y));
      const ImVec2 d((q.x - p.x) / std::max(len, 1.0f), (q.y - p.y) / std::max(len, 1.0f));
      for (float t = 0; t < len; t += dash + gap) {
        const float e = std::min(len, t + dash);
        dl->AddLine(ImVec2(p.x + d.x * t, p.y + d.y * t), ImVec2(p.x + d.x * e, p.y + d.y * e), col, 1.0f);
      }
    };
    line(a, ImVec2(b.x, a.y)); line(ImVec2(b.x, a.y), b); line(b, ImVec2(a.x, b.y)); line(ImVec2(a.x, b.y), a);
  }
  void CoopCheck(ImDrawList* dl, ImVec2 c, float size, ImU32 col) {
    const ImVec2 pts[] = {ImVec2(c.x - size * 0.45f, c.y), ImVec2(c.x - size * 0.12f, c.y + size * 0.32f),
                          ImVec2(c.x + size * 0.48f, c.y - size * 0.34f)};
    dl->AddPolyline(pts, 3, col, ImDrawFlags_None, std::max(1.5f, size * 0.14f));
  }
  void CoopInfoIcon(ImDrawList* dl, ImVec2 c, float r, ImU32 col) {
    const float t = std::max(1.2f, r * 0.17f);
    dl->AddCircle(c, r, col, 0, t);
    dl->AddLine(ImVec2(c.x, c.y - r * 0.45f), ImVec2(c.x, c.y + r * 0.12f), col, t);
    dl->AddCircleFilled(ImVec2(c.x, c.y + r * 0.45f), t * 0.75f, col);
  }
  void CoopLockIcon(ImDrawList* dl, ImVec2 c, float size, ImU32 col) {
    const float t = std::max(1.2f, size * 0.09f);
    dl->AddRect(ImVec2(c.x - size * 0.36f, c.y - size * 0.05f), ImVec2(c.x + size * 0.36f, c.y + size * 0.42f), col,
                size * 0.08f, 0, t);
    dl->PathArcTo(ImVec2(c.x, c.y - size * 0.12f), size * 0.2f, kCoopPi, 2 * kCoopPi);
    dl->PathStroke(col, ImDrawFlags_None, t);
    dl->AddLine(ImVec2(c.x - size * 0.2f, c.y - size * 0.12f), ImVec2(c.x - size * 0.2f, c.y - size * 0.05f), col, t);
    dl->AddLine(ImVec2(c.x + size * 0.2f, c.y - size * 0.12f), ImVec2(c.x + size * 0.2f, c.y - size * 0.05f), col, t);
  }
  void CoopPlayIcon(ImDrawList* dl, ImVec2 c, float size, ImU32 col) {
    dl->AddTriangle(ImVec2(c.x - size * 0.3f, c.y - size * 0.42f), ImVec2(c.x + size * 0.42f, c.y),
                    ImVec2(c.x - size * 0.3f, c.y + size * 0.42f), col, std::max(1.5f, size * 0.08f));
  }
  void CoopWarnIcon(ImDrawList* dl, ImVec2 c, float size, ImU32 col) {
    const float t = std::max(1.5f, size * 0.08f);
    dl->AddTriangle(ImVec2(c.x, c.y - size * 0.48f), ImVec2(c.x + size * 0.52f, c.y + size * 0.42f),
                    ImVec2(c.x - size * 0.52f, c.y + size * 0.42f), col, t);
    dl->AddLine(ImVec2(c.x, c.y - size * 0.14f), ImVec2(c.x, c.y + size * 0.14f), col, t);
    dl->AddCircleFilled(ImVec2(c.x, c.y + size * 0.27f), t * 0.7f, col);
  }
  void CoopRejoinIcon(ImDrawList* dl, ImVec2 c, float r, ImU32 col) {
    const float t = std::max(1.5f, r * 0.16f);
    dl->PathArcTo(c, r, -kCoopPi * 0.75f, kCoopPi * 1.0f);
    dl->PathStroke(col, ImDrawFlags_None, t);
    const ImVec2 e(c.x + r * std::cos(-kCoopPi * 0.75f), c.y + r * std::sin(-kCoopPi * 0.75f));
    dl->AddLine(e, ImVec2(e.x, e.y - r * 0.55f), col, t);
    dl->AddLine(e, ImVec2(e.x + r * 0.55f, e.y), col, t);
  }
  // A numbered circle: `state` 0 waiting (faint), 1 now (brass ring, glowing), 2 done (filled green with a tick),
  // 3 a plain brass ring (numbered lists).
  void CoopNumber(ImDrawList* dl, ImVec2 c, float r, const std::string& text, int state) {
    const ui::Fonts f = ui::GetFonts();
    const float t = std::max(1.5f, r * 0.1f);
    if (state == 2) {
      dl->AddCircleFilled(c, r, color::kGood);
      CoopCheck(dl, c, r * 0.95f, color::kOnFocus);
      return;
    }
    const ImU32 col = state == 1 || state == 3 ? color::kAccent : color::kTextFaint;
    if (state == 1) dl->AddCircleFilled(c, r + 4 * S(), ui::WithAlpha(color::kAccent, 0.15f));
    dl->AddCircleFilled(c, r, IM_COL32(14, 16, 20, 255));
    dl->AddCircle(c, r, col, 0, t);
    const float fs = r * 0.95f;
    const ImVec2 ts = ui::TextSize(f.display, fs, text);
    dl->AddText(f.display, fs, ImVec2(c.x - ts.x * 0.5f, c.y - ts.y * 0.5f), col, text.c_str());
  }
  // A round avatar: the Steam picture when there is one, else the name's first letter on a colour of its own.
  void CoopAvatar(ImDrawList* dl, ImVec2 c, float r, const std::string& name, rex::ui::ImmediateTexture* picture) {
    if (picture) {
      dl->AddImageRounded(ui::Tex(picture), ImVec2(c.x - r, c.y - r), ImVec2(c.x + r, c.y + r), ImVec2(0, 0),
                          ImVec2(1, 1), IM_COL32_WHITE, r);
      return;
    }
    static constexpr ImU32 kTop[] = {IM_COL32(232, 211, 166, 255), IM_COL32(169, 201, 216, 255),
                                     IM_COL32(185, 211, 165, 255), IM_COL32(216, 176, 184, 255),
                                     IM_COL32(199, 194, 216, 255)};
    static constexpr ImU32 kBottom[] = {IM_COL32(180, 135, 72, 255), IM_COL32(95, 138, 163, 255),
                                        IM_COL32(111, 154, 90, 255), IM_COL32(160, 103, 119, 255),
                                        IM_COL32(125, 117, 152, 255)};
    size_t h = 0;
    for (const char ch : name) h = h * 31 + uint8_t(ch);
    const size_t i = h % std::size(kTop);
    dl->AddCircleFilled(c, r, kBottom[i]);
    dl->AddCircleFilled(ImVec2(c.x - r * 0.12f, c.y - r * 0.12f), r * 0.82f, ui::WithAlpha(kTop[i], 0.75f));
    std::string letter = name.empty() ? "?" : name.substr(0, 1);
    if (!name.empty() && (uint8_t(name[0]) & 0x80)) {  // a UTF-8 letter: its whole sequence
      size_t n = 1;
      while (n < name.size() && (uint8_t(name[n]) & 0xC0) == 0x80) ++n;
      letter = name.substr(0, n);
    }
    letter = Upper(letter);
    const ui::Fonts f = ui::GetFonts();
    const float fs = r * 0.9f;
    const ImVec2 ts = ui::TextSize(f.display, fs, letter);
    dl->AddText(f.display, fs, ImVec2(c.x - ts.x * 0.5f, c.y - ts.y * 0.5f), color::kOnFocus, letter.c_str());
  }
  // A pill: `style` 0 brass (a mode), 1 ready (green), 2 waiting (grey). Returns its width; right-aligned when
  // `right` (x is then its right edge).
  float CoopPill(ImDrawList* dl, float x, float cy, const std::string& text, int style, bool right = false,
                 bool caps = true) {
    const ui::Fonts f = ui::GetFonts();
    const float s = S(), fs = 11.5f * s, h = U(32), pad = U(14);
    const std::string label = caps ? Upper(text) : text;
    const float tw = caps ? ui::TrackedWidth(f.semibold, fs, label, 0.8f * s) : ui::TextSize(f.semibold, fs, label).x;
    const bool tick = style == 1;
    const float w = tw + pad * 2 + (tick ? fs + 4 * s : 0.0f);
    if (right) x -= w;
    const ImU32 col = style == 0 ? color::kAccent : style == 1 ? color::kGood : color::kTextDim;
    dl->AddRectFilled(ImVec2(x, cy - h * 0.5f), ImVec2(x + w, cy + h * 0.5f),
                      style == 2 ? IM_COL32(255, 255, 255, 13) : ui::WithAlpha(col, 0.16f), h * 0.5f);
    dl->AddRect(ImVec2(x, cy - h * 0.5f), ImVec2(x + w, cy + h * 0.5f),
                style == 2 ? kCoopHair : ui::WithAlpha(col, 0.45f), h * 0.5f, 0, 1.0f);
    float tx = x + pad;
    if (tick) {
      CoopCheck(dl, ImVec2(tx + fs * 0.45f, cy), fs * 0.85f, col);
      tx += fs + 4 * s;
    }
    if (caps) ui::DrawTracked(dl, f.semibold, fs, ImVec2(tx, cy - fs * 0.55f), col, label, 0.8f * s);
    else dl->AddText(f.semibold, fs, ImVec2(tx, cy - fs * 0.55f), col, label.c_str());
    return w;
  }
  // A brass caption with a hairline after it, like the mockups' section titles.
  void CoopSection(ImDrawList* dl, float x, float y, float w, const std::string& caption,
                   const std::string& count = {}) {
    const ui::Fonts f = ui::GetFonts();
    const float s = S(), fs = 12 * s, tr = 2.6f * s;
    const std::string cap = Upper(caption);
    ui::DrawTracked(dl, f.display, fs, ImVec2(x, y), color::kAccent, cap, tr);
    float end = x + ui::TrackedWidth(f.display, fs, cap, tr) + 12 * s;
    if (!count.empty()) {
      const std::string c = Upper(count);
      ui::DrawTracked(dl, f.display, fs, ImVec2(end, y), color::kTextDim, c, 1.2f * s);
      end += ui::TrackedWidth(f.display, fs, c, 1.2f * s) + 12 * s;
    }
    if (end < x + w) dl->AddLine(ImVec2(end, std::round(y + fs * 0.55f)), ImVec2(x + w, std::round(y + fs * 0.55f)), kCoopHair);
  }
  // Wrapped text; returns its height.
  float CoopText(ImDrawList* dl, ImFont* font, float size, ImVec2 pos, ImU32 col, const std::string& text, float wrap) {
    dl->AddText(font, size, pos, col, text.c_str(), nullptr, wrap);
    return ui::TextSize(font, size, text, wrap).y;
  }
  // "Lead: rest of the line", the lead in semibold, wrapped by words to `w`. Returns the height.
  float CoopLeadText(ImDrawList* dl, ImVec2 pos, float w, const std::string& lead, const std::string& rest, float size,
                     ImU32 lead_col, ImU32 rest_col) {
    const ui::Fonts f = ui::GetFonts();
    const float line_h = size * 1.38f, space = ui::TextSize(f.text, size, " ").x;
    float x = pos.x, y = pos.y;
    if (!lead.empty()) {
      dl->AddText(f.semibold, size, pos, lead_col, lead.c_str());
      x += ui::TextSize(f.semibold, size, lead).x + space;
    }
    size_t at = 0;
    while (at < rest.size()) {
      size_t end = rest.find(' ', at);
      if (end == std::string::npos) end = rest.size();
      const std::string word = rest.substr(at, end - at);
      const float ww = ui::TextSize(f.text, size, word).x;
      if (x + ww > pos.x + w && x > pos.x) x = pos.x, y += line_h;
      dl->AddText(f.text, size, ImVec2(x, y), rest_col, word.c_str());
      x += ww + space;
      at = end + 1;
    }
    return y - pos.y + line_h;
  }
  // A focusable box: registers it and says whether it has the focus.
  bool CoopAdd(const std::string& key, ImVec2 a, ImVec2 b, bool enabled, std::function<void()> press, int group = 0) {
    coop_targets_.push_back({key, a, b, enabled, group, std::move(press)});
    const auto it = coop_focus_.find(int(coop_page_));
    return it != coop_focus_.end() && it->second == key;
  }
  // A plain button (the mockups' btn2): bone when focused, faint when it cannot be pressed.
  void CoopButton(ImDrawList* dl, const std::string& key, ImVec2 a, ImVec2 b, const std::string& label, bool enabled,
                  std::function<void()> press, Action glyph = Action::kCount, bool primary = false, int group = 0) {
    const ui::Fonts f = ui::GetFonts();
    const float s = S();
    const bool on = CoopAdd(key, a, b, enabled, std::move(press), group);
    const ImU32 fill = on ? color::kFocus : primary && enabled ? IM_COL32(255, 255, 255, 22) : IM_COL32(255, 255, 255, 10);
    dl->AddRectFilled(a, b, fill);
    if (!on) dl->AddRect(a, b, primary && enabled ? ui::WithAlpha(color::kAccent, 0.6f) : kCoopHair, 0, 0, 1.0f);
    const float fs = 14 * s, cy = (a.y + b.y) * 0.5f;
    float gw = 0;
    if (glyph != Action::kCount) gw = ui::DrawGlyph(dl, glyphs_, glyph, PromptSet(), 0, 0, 20 * s, m_, false) + 8 * s;
    const std::string text = Ellipsize(f.semibold, fs, label, std::max(1.0f, b.x - a.x - gw - 16 * s));
    const float tw = ui::TextSize(f.semibold, fs, text).x;
    float x = (a.x + b.x - tw - gw) * 0.5f;
    if (glyph != Action::kCount) {
      ui::DrawGlyph(dl, glyphs_, glyph, PromptSet(), x, cy, 20 * s, m_, true);
      x += gw;
    }
    dl->AddText(f.semibold, fs, ImVec2(x, cy - fs * 0.55f), on ? color::kOnFocus : enabled ? color::kText : color::kTextFaint,
                text.c_str());
  }
  // A text field (Direct IP address, port): its caption, the value, bone outline when focused.
  void CoopField(ImDrawList* dl, const std::string& key, float x, float& y, float w, const std::string& caption,
                 const std::string& value, bool enabled, std::function<void()> press, int group, bool dim = false) {
    const ui::Fonts f = ui::GetFonts();
    const float s = S();
    dl->AddText(f.text, 12.5f * s, ImVec2(x, y), color::kTextDim, caption.c_str());
    y += 12.5f * s + U(10);
    const ImVec2 a(x, y), b(x + w, y + U(54));
    const bool on = CoopAdd(key, a, b, enabled, std::move(press), group);
    dl->AddRectFilled(a, b, IM_COL32(0, 0, 0, 90));
    dl->AddRect(a, b, on ? color::kFocus : kCoopHair, 0, 0, on ? 2.0f * s : 1.0f);
    const float fs = 15.5f * s;
    const std::string shown = value.empty() ? std::string(T("Not entered yet")) : value;
    dl->AddText(f.text, fs, ImVec2(a.x + U(16), (a.y + b.y) * 0.5f - fs * 0.55f),
                value.empty() || dim || !enabled ? color::kTextDim : color::kText,
                Ellipsize(f.text, fs, shown, w - U(32)).c_str());
    y = b.y;
  }
  // The join code as 6 boxes; the next empty one is outlined in brass while the field has the focus.
  void CoopCodeBoxes(ImDrawList* dl, const std::string& key, float x, float& y, float w, const std::string& code,
                     bool enabled, std::function<void()> press, int group) {
    const ui::Fonts f = ui::GetFonts();
    const float s = S();
    dl->AddText(f.text, 12.5f * s, ImVec2(x, y), color::kTextDim, T("Code (6 characters)"));
    y += 12.5f * s + U(10);
    const float gap = U(10), bw = std::min(U(60), (w - gap * 5) / 6), bh = U(70);
    const ImVec2 a(x, y), b(x + bw * 6 + gap * 5, y + bh);
    const bool on = CoopAdd(key, ImVec2(a.x - 3 * s, a.y - 3 * s), ImVec2(b.x + 3 * s, b.y + 3 * s), enabled,
                            std::move(press), group);
    if (on) dl->AddRect(ImVec2(a.x - 4 * s, a.y - 4 * s), ImVec2(b.x + 4 * s, b.y + 4 * s), color::kFocus, 0, 0, 1.5f * s);
    for (int i = 0; i < 6; ++i) {
      const ImVec2 p(x + float(i) * (bw + gap), y), q(p.x + bw, p.y + bh);
      dl->AddRectFilled(p, q, IM_COL32(0, 0, 0, 90));
      const bool cur = on && i == int(std::min<size_t>(code.size(), 5));
      dl->AddRect(p, q, cur ? color::kAccent : kCoopHair, 0, 0, cur ? 2.0f * s : 1.0f);
      const std::string ch = size_t(i) < code.size() ? code.substr(size_t(i), 1) : "_";
      const float fs = size_t(i) < code.size() ? 21 * s : 15 * s;
      const ImVec2 ts = ui::TextSize(f.display, fs, ch);
      dl->AddText(f.display, fs, ImVec2((p.x + q.x - ts.x) * 0.5f, (p.y + q.y - ts.y) * 0.5f),
                  size_t(i) < code.size() ? color::kText : color::kTextFaint, ch.c_str());
    }
    y = b.y;
  }
  std::string Ellipsize(ImFont* font, float size, const std::string& text, float width) {
    return ui::Ellipsize(font, size, text, width);
  }

  // The chips at the top right: the Steam (or Direct IP) state and the version.
  void DrawCoopChips(ImDrawList* dl, const coop::SocialModel& m) {
    const ui::Fonts f = ui::GetFonts();
    const float s = S(), cy = o_.y + 44 * s, h = U(36), fs = 12 * s, pad = U(16);
    using St = coop::SocialStatus;
    std::string steam;
    ImU32 dot = color::kGood;
    if (m.lobby) steam = T(m.direct ? "Direct IP lobby" : "Steam lobby, friends only");
    else if (m.direct) steam = T("Direct IP");
    else if (m.status == St::Disabled) steam = T("Local dev link selected"), dot = color::kTextFaint;
    else if (m.status == St::Unavailable) steam = T("Steam is unavailable"), dot = color::kWarn;
    else if (m.status == St::Dormant)
      m.status_text == "Connecting to Steam" ? (steam = T("Connecting to Steam"), dot = color::kAccent)
                                             : (steam = T("Steam not connected"), dot = color::kTextFaint);
    else steam = T("Steam connected");
    const std::string version_caption = T("Version"), version = CoopPlainVersion();
    float x = o_.x + w_ - Margin();
    auto chip = [&](float w, auto&& draw) {
      x -= w;
      dl->AddRectFilled(ImVec2(x, cy - h * 0.5f), ImVec2(x + w, cy + h * 0.5f), IM_COL32(255, 255, 255, 13), h * 0.5f);
      dl->AddRect(ImVec2(x, cy - h * 0.5f), ImVec2(x + w, cy + h * 0.5f), kCoopHair, h * 0.5f, 0, 1.0f);
      draw(x + pad);
      x -= U(14);
    };
    const float vw = ui::TextSize(f.text, fs, version_caption).x + 6 * s + ui::TextSize(f.semibold, fs, version).x;
    chip(vw + pad * 2, [&](float tx) {
      dl->AddText(f.text, fs, ImVec2(tx, cy - fs * 0.55f), color::kTextDim, version_caption.c_str());
      dl->AddText(f.semibold, fs, ImVec2(tx + ui::TextSize(f.text, fs, version_caption).x + 6 * s, cy - fs * 0.55f),
                  color::kText, version.c_str());
    });
    const float sw = ui::TextSize(f.text, fs, steam).x + U(18);
    chip(sw + pad * 2, [&](float tx) {
      dl->AddCircleFilled(ImVec2(tx + U(4.5f), cy), U(4.5f), dot);
      dl->AddText(f.text, fs, ImVec2(tx + U(18), cy - fs * 0.55f), color::kTextDim, steam.c_str());
    });
  }

  // The step bar over the host pages and the lobby: 1 choose what to play, 2 invite a friend, 3 both ready.
  void DrawCoopSteps(ImDrawList* dl, int step) {
    const ui::Fonts f = ui::GetFonts();
    const float s = S(), cy = o_.y + U(172), r = U(19), fs = 14 * s;
    const float x0 = o_.x + Margin(), x1 = o_.x + w_ - Margin();
    const char* names[] = {"Choose what to play", "Invite a friend", "Both ready, then start"};
    float widths[3], total = 0;
    for (int i = 0; i < 3; ++i) total += widths[i] = r * 2 + U(14) + ui::TextSize(f.semibold, fs, T(names[i])).x;
    const float line = std::max(20 * s, (x1 - x0 - total) / 2);
    float x = x0;
    for (int i = 0; i < 3; ++i) {
      const int state = i + 1 < step ? 2 : i + 1 == step ? 1 : 0;
      CoopNumber(dl, ImVec2(x + r, cy), r, std::to_string(i + 1), state);
      dl->AddText(state == 1 ? f.semibold : f.text, fs, ImVec2(x + r * 2 + U(14), cy - fs * 0.6f),
                  state == 0 ? color::kTextDim : color::kText, T(names[i]));
      x += widths[i];
      if (i < 2) {
        dl->AddLine(ImVec2(x + U(26), cy), ImVec2(x + line - U(26), cy),
                    state == 2 ? ui::WithAlpha(color::kGood, 0.6f) : kCoopHair, 2 * s);
        x += line;
      }
    }
  }

  // A page's big title and its line under it (Home, Join, Host), and the status line under those when there is
  // something to say (an error or the latest action's notice).
  void DrawCoopIntro(ImDrawList* dl, const std::string& title, const std::string& sub, const coop::SocialModel& model) {
    const ui::Fonts f = ui::GetFonts();
    const float s = S(), x = o_.x + Margin(), w = w_ - Margin() * 2;
    dl->AddText(f.display, 30 * s, ImVec2(x, o_.y + 98 * s), color::kText, Ellipsize(f.display, 30 * s, title, w).c_str());
    dl->AddText(f.text, 14.5f * s, ImVec2(x, o_.y + 138 * s), color::kTextDim, Ellipsize(f.text, 14.5f * s, sub, w).c_str());
    bool warn = false;
    const std::string line = CoopStatusLine(model, &warn);
    if (!line.empty())
      dl->AddText(f.semibold, 13.5f * s, ImVec2(x, o_.y + 160 * s), warn ? color::kWarn : color::kText,
                  Ellipsize(f.semibold, 13.5f * s, line, w).c_str());
  }

  // A chapter's card (lobby, Invite, the host's lists): picture with its number and name over it, the co-op mode
  // and what it means. `over`: the line over the name ("Chapter 3 · New game"). Returns where its body ends.
  float DrawCoopChapterCard(ImDrawList* dl, int entry, ImVec2 a, float w, float pic_h, const std::string& over,
                            const std::string& name) {
    const ui::Fonts f = ui::GetFonts();
    const float s = S();
    const ImVec2 pb(a.x + w, a.y + pic_h);
    CoopPicture(dl, entry, a, pb, 0.85f);
    dl->AddRectFilledMultiColor(ImVec2(a.x, a.y + pic_h * 0.4f), pb, IM_COL32(14, 16, 20, 0), IM_COL32(14, 16, 20, 0),
                                IM_COL32(14, 16, 20, 245), IM_COL32(14, 16, 20, 245));
    const float name_fs = std::min(35 * s, std::max(24 * s, pic_h * 0.18f));
    const std::string shown = Ellipsize(f.display, name_fs, name, w - U(60));
    const float ny = pb.y - U(22) - name_fs * 1.05f;
    ui::DrawTracked(dl, f.display, 11 * s, ImVec2(a.x + U(30), ny - 16 * s), color::kAccent, Upper(over), 2.4f * s);
    dl->AddText(f.display, name_fs, ImVec2(a.x + U(30), ny), color::kText, shown.c_str());
    float y = pb.y + U(22);
    const float pw = CoopPill(dl, a.x + U(30), y + U(16), T(coop::CoopModeName(entry)), 0, false, false);
    (void)pw;
    y += U(32) + U(14);
    y += CoopText(dl, f.text, 13.5f * s, ImVec2(a.x + U(30), y), color::kTextDim, T(coop::CoopModeText(entry)), w - U(60));
    return y;
  }

  // ------------------------------------------------------------- navigation ---
  // Focus moves to the nearest target in the pressed direction (controller, keyboard), the mouse focuses what it
  // moves over and clicks it, A / Enter presses the focused one; LB and RB jump between groups (Join's columns).
  int CoopSpatial(const std::vector<CoopTarget>& t, int cur, Action dir) {
    const float e = 2 * S();
    const ImVec2 ca = t[size_t(cur)].a, cb = t[size_t(cur)].b;
    const ImVec2 c((ca.x + cb.x) * 0.5f, (ca.y + cb.y) * 0.5f);
    int best = -1;
    float best_score = 1e30f;
    for (size_t j = 0; j < t.size(); ++j) {
      if (int(j) == cur) continue;
      const ImVec2 ja = t[j].a, jb = t[j].b, jc((ja.x + jb.x) * 0.5f, (ja.y + jb.y) * 0.5f);
      float primary, perp;
      switch (dir) {
        case Action::kDown:
          if (ja.y < cb.y - e) continue;
          primary = ja.y - cb.y, perp = std::max(0.0f, std::max(ja.x - cb.x, ca.x - jb.x));
          break;
        case Action::kUp:
          if (jb.y > ca.y + e) continue;
          primary = ca.y - jb.y, perp = std::max(0.0f, std::max(ja.x - cb.x, ca.x - jb.x));
          break;
        case Action::kRight:
          if (ja.x < cb.x - e) continue;
          primary = ja.x - cb.x, perp = std::max(0.0f, std::max(ja.y - cb.y, ca.y - jb.y));
          break;
        default:  // kLeft
          if (jb.x > ca.x + e) continue;
          primary = ca.x - jb.x, perp = std::max(0.0f, std::max(ja.y - cb.y, ca.y - jb.y));
          break;
      }
      const bool vertical = dir == Action::kDown || dir == Action::kUp;
      const float score = std::max(0.0f, primary) + perp * 3 + 0.05f * std::abs(vertical ? jc.x - c.x : jc.y - c.y);
      if (score < best_score) best_score = score, best = int(j);
    }
    return best;
  }
  void CoopPress(const CoopTarget& t) {
    if (!t.enabled || !t.press) {
      ui::Cue(ui::Sound::kDeny);
      return;
    }
    ui::Cue(ui::Sound::kSelect);
    const auto press = t.press;  // the press may change the page and its targets
    press();
  }
  // `key`: the page's focused target (kept by key, so a row that appears above keeps the focus where it was).
  void CoopNavigate(std::string& key, bool interactive, const std::string& first = {}) {
    const auto& t = coop_targets_;
    if (t.empty()) return;
    int cur = -1;
    for (size_t i = 0; i < t.size(); ++i) if (t[i].key == key) cur = int(i);
    if (cur < 0) {
      for (size_t i = 0; i < t.size() && cur < 0; ++i) if (t[i].key == first) cur = int(i);
      for (size_t i = 0; i < t.size() && cur < 0; ++i) if (t[i].enabled) cur = int(i);
      if (cur < 0) cur = 0;
      key = t[size_t(cur)].key;
    }
    if (!interactive) return;
    for (size_t i = 0; i < t.size(); ++i) {
      if (!ImGui::IsMouseHoveringRect(t[i].a, t[i].b, false)) continue;
      if (input_.mouse_moved() && int(i) != cur) key = t[i].key, cur = int(i);
      if (ImGui::IsMouseClicked(0)) {
        key = t[i].key;
        CoopPress(t[i]);
        return;
      }
    }
    for (const Action d : {Action::kUp, Action::kDown, Action::kLeft, Action::kRight}) {
      if (!input_.Pressed(d)) continue;
      input_.Consume(d);
      const int next = CoopSpatial(t, cur, d);
      if (next >= 0) {
        cur = next, key = t[size_t(next)].key;
        ui::Cue(ui::Sound::kMove);
      }
    }
    for (const Action d : {Action::kPrevTab, Action::kNextTab}) {
      if (!input_.Pressed(d)) continue;
      input_.Consume(d);
      int groups = 0;
      for (const auto& x : t) groups = std::max(groups, x.group + 1);
      if (groups < 2) continue;
      const int g = (t[size_t(cur)].group + (d == Action::kNextTab ? 1 : groups - 1)) % groups;
      for (size_t i = 0; i < t.size(); ++i)
        if (t[i].group == g) {
          cur = int(i), key = t[i].key;
          ui::Cue(ui::Sound::kTab);
          break;
        }
    }
    if (input_.Pressed(Action::kAccept)) {
      input_.Consume(Action::kAccept);
      CoopPress(t[size_t(cur)]);
    }
  }

  // ---------------------------------------------------------------- the pages ---
  void DrawCoopPage(ImDrawList* dl) {
    auto& service = coop::Social();
    const auto model = service.Snapshot();
    social_.Tick(model);
    const CoopPage next = coop::CoopPageFor(model, coop_page_);
    if (next != coop_page_) CoopGo(next);
    // A soft shade over the scene, darker on the left where the text is (the mockups' backdrop).
    dl->AddRectFilledMultiColor(o_, ImVec2(o_.x + w_, o_.y + h_), IM_COL32(8, 10, 13, 150), IM_COL32(8, 10, 13, 90),
                                IM_COL32(8, 10, 13, 150), IM_COL32(8, 10, 13, 200));
    coop_targets_.clear();
    focused_items_.clear();
    focused_ = nullptr;
    if (coop_gate_.open()) return DrawCoopWarning(dl);
    DrawCoopChips(dl, model);
    const bool editing = social_.editing();
    const bool interactive = !PageInputBlocked() && !editing;
    switch (coop_page_) {
      case CoopPage::Hub: DrawCoopHub(dl, model, interactive); break;
      case CoopPage::Host: DrawCoopHost(dl, model, interactive); break;
      case CoopPage::HostNew: case CoopPage::HostLoad: DrawCoopList(dl, model, interactive); break;
      case CoopPage::HostHow: DrawCoopHow(dl, model, interactive); break;
      case CoopPage::Join: DrawCoopJoin(dl, model, interactive); break;
      case CoopPage::Lobby: DrawCoopLobby(dl, model, interactive); break;
    }
    if (social_.editing()) {
      // The text editor (keyboard, controller keys, mouse), in a panel over the page.
      const float ew = std::min(490 * S(), w_), ex = o_.x + (w_ - ew) * 0.5f;
      dl->AddRectFilled(o_, ImVec2(o_.x + w_, o_.y + h_), IM_COL32(0, 0, 0, 170));
      social_.DrawPageEditor(dl, ImVec2(ex, o_.y), ImVec2(ew, h_), m_, input_, glyphs_, PromptSet(), service);
    }
  }

  // The rows of a page, for the targets below (each target presses its row through CoopActivate).
  std::vector<coop::SocialRow> CoopRows(const coop::SocialModel& model, CoopPage page) {
    return coop::CoopPageRows(model, page, {UsedSlots() > 0, coop_choice_.has_value()});
  }
  static const coop::SocialRow* CoopFind(const std::vector<coop::SocialRow>& rows, coop::SocialRowKind kind,
                                         std::optional<coop::SocialId> id = {}) {
    for (const auto& r : rows) if (r.key.kind == kind && (!id || r.key.id == *id)) return &r;
    return nullptr;
  }
  std::function<void()> CoopPressRow(const coop::SocialRow& row) {
    return [this, row] { CoopActivate(row); };
  }
  const coop::SocialFriend* CoopFriend(const coop::SocialModel& model, coop::SocialId id) {
    for (const auto& f : model.friends) if (f.id == id) return &f;
    return nullptr;
  }

  // The rejoin record (read at most once a second): chapter, partner and when it was written.
  const CoopRejoin& CoopRejoinInfo() {
    const double now = ImGui::GetTime();
    if (coop_rejoin_.read_at < 0 || now - coop_rejoin_.read_at > 1.0) {
      coop_rejoin_ = {};
      coop_rejoin_.read_at = now;
      if (coop::RejoinAvailable(paths_.user_dir)) {
        coop::handover::File record;
        if (coop::ReadRejoinRecord(paths_.user_dir, record)) {
          coop_rejoin_.ok = true;
          coop_rejoin_.partner = record.partner_name;
          coop_rejoin_.chapter = int(record.plan.chapter);
          coop_rejoin_.written_ms = record.written_ms;
        }
      }
    }
    return coop_rejoin_;
  }
  void CoopRejoinNow() {
    if (coop::PrepareMenuRejoin(paths_.user_dir)) {
      play::SetStartRequest({});
      Play();
    }
  }

  // Home of the co-op pages: the invite banner, Host and Join, Rejoin, how co-op works.
  void DrawCoopHub(ImDrawList* dl, const coop::SocialModel& model, bool interactive) {
    const ui::Fonts f = ui::GetFonts();
    const float s = S();
    DrawCoopIntro(dl, T("Play the story with a friend"), T("Two players, online. One of you hosts the game, the other joins it."),
                  model);
    const auto rows = CoopRows(model, CoopPage::Hub);
    const auto join_rows = CoopRows(model, CoopPage::Join);  // its Accept rows: the invites
    const float L = o_.x + Margin(), R = o_.x + w_ - Margin(), cw = R - L;
    const float left_w = cw * 1132.0f / 1728.0f, panel_x = L + cw * 1192.0f / 1728.0f, panel_w = R - panel_x;
    float y = o_.y + U(282);
    // The invite banner: the first Steam invite waiting (Y accepts it).
    const coop::SocialRow* accept = CoopFind(join_rows, coop::SocialRowKind::Accept);
    if (accept) {
      const ImVec2 a(L, y), b(L + left_w, y + U(74));
      const bool on = CoopAdd("accept", a, b, accept->enabled, CoopPressRow(*accept));
      dl->AddRectFilled(a, b, on ? color::kFocus : ui::WithAlpha(color::kAccent, 0.10f));
      if (!on) dl->AddRect(a, b, ui::WithAlpha(color::kAccent, 0.5f), 0, 0, 1.0f);
      dl->AddRectFilled(a, ImVec2(a.x + 3 * s, b.y), color::kAccent);
      const float cy = (a.y + b.y) * 0.5f, r = U(23);
      const std::string name = accept->detail;
      CoopAvatar(dl, ImVec2(a.x + U(22) + r, cy), r, name.empty() ? "?" : name, nullptr);
      const float tx = a.x + U(22) + r * 2 + U(18);
      const std::string line = name.empty() ? std::string(T("A friend invited you to play")) : F("{0} invited you to play", {name});
      dl->AddText(f.semibold, 14 * s, ImVec2(tx, cy - 16 * s), on ? color::kOnFocus : color::kText,
                  Ellipsize(f.semibold, 14 * s, line, left_w * 0.55f).c_str());
      dl->AddText(f.text, 12 * s, ImVec2(tx, cy + 2 * s), on ? color::kOnFocusDim : color::kTextDim, T("Steam invite"));
      const std::string act = T("Accept invite");
      const float aw = ui::TextSize(f.semibold, 13.5f * s, act).x;
      dl->AddText(f.semibold, 13.5f * s, ImVec2(b.x - U(22) - aw, cy - 13.5f * s * 0.55f), on ? color::kOnFocus : color::kText,
                  act.c_str());
      const float gw = ui::DrawGlyph(dl, glyphs_, Action::kResetPage, PromptSet(), 0, 0, 22 * s, m_, false);
      ui::DrawGlyph(dl, glyphs_, Action::kResetPage, PromptSet(), b.x - U(22) - aw - 8 * s - gw, cy, 22 * s, m_, true);
      y += U(100);
    }
    // Host and Join, side by side.
    const float card_w = (left_w - U(36)) * 0.5f, card_h = U(440), pic_h = U(196);
    struct Card { const char* key; const char* tag; const char* title; const char* text; const char* checks[2]; int picture; };
    const Card cards[2] = {
        {"host", "You lead", "Host a game", "Pick a chapter or one of your saves, then invite a friend to play it with you.",
         {"Your game runs the world", "Invite a Steam friend, or share a 6 character code"}, 0},
        {"join", "Friend leads", "Join a game",
         "Play in a friend's game. Accept their invite, join them from your friends list, or type their code.",
         {"Invites and friends playing now", "Join with a code, no Steam needed"}, 10}};
    for (int i = 0; i < 2; ++i) {
      const Card& c = cards[i];
      const coop::SocialRow* row = CoopFind(rows, i == 0 ? coop::SocialRowKind::CoopHost : coop::SocialRowKind::CoopJoin);
      const bool enabled = row && row->enabled;
      const ImVec2 a(L + float(i) * (card_w + U(36)), y), b(a.x + card_w, y + card_h);
      const bool on = CoopAdd(c.key, a, b, enabled, row ? CoopPressRow(*row) : std::function<void()>{});
      if (on) dl->AddRectFilled(ImVec2(a.x - 3 * s, a.y - 3 * s), ImVec2(b.x + 3 * s, b.y + 3 * s), ui::WithAlpha(color::kFocus, 0.18f));
      dl->AddRectFilled(a, b, on ? color::kFocus : ui::WithAlpha(color::kPanel, 0.9f));
      dl->AddRect(a, b, on ? color::kFocus : kCoopHair, 0, 0, 1.0f);
      const int entry = int(play::Chapters().size()) > c.picture ? play::Chapters()[size_t(c.picture)].entry : 0;
      CoopPicture(dl, entry, ImVec2(a.x + 1, a.y + 1), ImVec2(b.x - 1, a.y + pic_h), enabled ? 0.8f : 0.45f);
      const std::string tag = Upper(T(c.tag));
      const float tw = ui::TrackedWidth(f.display, 10 * s, tag, 2 * s);
      dl->AddRectFilled(ImVec2(a.x + U(16), a.y + U(16)), ImVec2(a.x + U(16) + tw + U(24), a.y + U(16) + 10 * s + U(12)),
                        IM_COL32(8, 10, 13, 190));
      ui::DrawTracked(dl, f.display, 10 * s, ImVec2(a.x + U(28), a.y + U(22)), color::kAccent, tag, 2 * s);
      float ty = a.y + pic_h + U(24);
      const float tx = a.x + U(28), tw2 = card_w - U(56);
      const ImU32 head = on ? color::kOnFocus : enabled ? color::kText : color::kTextFaint;
      dl->AddText(f.display, 22.5f * s, ImVec2(tx, ty), head, T(c.title));
      ty += 22.5f * s + U(12);
      ty += CoopText(dl, f.text, 13.5f * s, ImVec2(tx, ty), on ? IM_COL32(80, 83, 90, 255) : color::kTextDim, T(c.text), tw2);
      ty += U(14);
      for (const char* check : c.checks) {
        CoopCheck(dl, ImVec2(tx + 6 * s, ty + 7.5f * s), 11 * s, on ? IM_COL32(154, 111, 44, 255) : color::kAccent);
        ty += CoopText(dl, f.text, 13 * s, ImVec2(tx + 20 * s, ty), head, T(check), tw2 - 20 * s) + U(8);
      }
    }
    y += card_h;
    // Rejoin the last game (the hand over record of a game that was cut off).
    if (!model.lobby && CoopRejoinInfo().ok) {
      const auto& rj = coop_rejoin_;
      y += U(30);
      const ImVec2 a(L, y), b(L + left_w, y + U(78));
      const bool on = CoopAdd("rejoin", a, b, true, [this] { CoopRejoinNow(); });
      dl->AddRectFilled(a, b, on ? color::kFocus : ui::WithAlpha(color::kPanel, 0.75f));
      if (!on) dl->AddRect(a, b, kCoopHair, 0, 0, 1.0f);
      const float cy = (a.y + b.y) * 0.5f, r = U(22);
      dl->AddCircleFilled(ImVec2(a.x + U(24) + r, cy), r, on ? IM_COL32(0, 0, 0, 20) : IM_COL32(255, 255, 255, 16));
      CoopRejoinIcon(dl, ImVec2(a.x + U(24) + r, cy), r * 0.45f, on ? IM_COL32(154, 111, 44, 255) : color::kAccent);
      const float tx = a.x + U(24) + r * 2 + U(18);
      dl->AddText(f.semibold, 14 * s, ImVec2(tx, cy - 16 * s), on ? color::kOnFocus : color::kText, T("Rejoin your last game"));
      std::string sub = rj.chapter >= 0 ? std::string(T(ChapterName(rj.chapter))) : std::string();
      if (!rj.partner.empty()) sub = sub.empty() ? rj.partner : F("{0} with {1}", {sub, rj.partner});
      if (rj.written_ms > 0) sub += (sub.empty() ? "" : "  " KK_DOT "  ") + When(rj.written_ms / 1000);
      dl->AddText(f.text, 12 * s, ImVec2(tx, cy + 2 * s), on ? color::kOnFocusDim : color::kTextDim,
                  Ellipsize(f.text, 12 * s, sub, left_w * 0.6f).c_str());
      const std::string act = T("Rejoin");
      const float aw = ui::TextSize(f.text, 13.5f * s, act).x;
      dl->AddText(f.text, 13.5f * s, ImVec2(b.x - U(24) - aw, cy - 13.5f * s * 0.55f), on ? color::kOnFocus : color::kTextDim,
                  act.c_str());
      const float gw = ui::DrawGlyph(dl, glyphs_, Action::kDefault, PromptSet(), 0, 0, 21 * s, m_, false);
      ui::DrawGlyph(dl, glyphs_, Action::kDefault, PromptSet(), b.x - U(24) - aw - 8 * s - gw, cy, 21 * s, m_, true);
    }
    // How co-op works, and good to know.
    {
      const float px = panel_x + U(32), pw = panel_w - U(64);
      float py = o_.y + U(282) + U(30);
      const float top = o_.y + U(282);
      // Measured first, drawn over its own panel.
      ImDrawListSplitter split;
      split.Split(dl, 2);
      split.SetCurrentChannel(dl, 1);
      CoopSection(dl, px, py, pw, T("How co-op works"));
      py += 12 * s + U(22);
      const char* steps[3][2] = {{"The host picks what to play", "A chapter, or one of the host's saves."},
                                 {"Invite a friend", "Through Steam, or by sharing a 6 character code."},
                                 {"Both press Ready, then start", "Both games load the chapter and you play it together."}};
      for (int i = 0; i < 3; ++i) {
        const float r = U(20);
        CoopNumber(dl, ImVec2(px + r, py + r), r, std::to_string(i + 1), 3);
        const float tx = px + r * 2 + U(18), tw = pw - r * 2 - U(18);
        dl->AddText(f.semibold, 13.5f * s, ImVec2(tx, py), color::kText, T(steps[i][0]));
        const float h = 13.5f * s + 3 * s + CoopText(dl, f.text, 12 * s, ImVec2(tx, py + 13.5f * s + 3 * s), color::kTextDim,
                                                     T(steps[i][1]), tw);
        py += std::max(r * 2, h) + U(20);
      }
      py += U(10);
      CoopSection(dl, px, py, pw, T("Good to know"));
      py += 12 * s + U(16);
      const char* facts[3][2] = {{"Kong chapters:", "each player plays their own Kong part, then you meet up again."},
                                 {"Progress:", "chapters you finish together unlock on both saves."},
                                 {"Talking:", "there is no voice chat in the game. Use Steam or Discord."}};
      for (const auto& fact : facts) {
        CoopInfoIcon(dl, ImVec2(px + U(10), py + 7 * s), U(9.5f), color::kAccent);
        py += CoopLeadText(dl, ImVec2(px + U(32), py), pw - U(32), T(fact[0]), T(fact[1]), 12 * s, color::kText,
                           color::kTextDim) + U(13);
      }
      py += U(14);
      split.SetCurrentChannel(dl, 0);
      dl->AddRectFilled(ImVec2(panel_x, top), ImVec2(panel_x + panel_w, py), ui::WithAlpha(color::kPanel, 0.85f));
      dl->AddRect(ImVec2(panel_x, top), ImVec2(panel_x + panel_w, py), kCoopHair, 0, 0, 1.0f);
      split.Merge(dl);
      // Steam: connect (only on request: it installs Steam's hooks) and Add to Steam.
      py += U(24);
      const coop::SocialRow* connect = CoopFind(rows, coop::SocialRowKind::ConnectSteam);
      const coop::SocialRow* add = CoopFind(rows, coop::SocialRowKind::AddSteam);
      const int n = (connect ? 1 : 0) + (add ? 1 : 0);
      float bx = panel_x;
      const float bw = n ? (panel_w - U(14) * float(n - 1)) / float(n) : 0;
      if (connect) {
        CoopButton(dl, "connect", ImVec2(bx, py), ImVec2(bx + bw, py + U(52)), T("Connect to Steam"), connect->enabled,
                   CoopPressRow(*connect));
        bx += bw + U(14);
      }
      if (add)
        CoopButton(dl, "add-steam", ImVec2(bx, py), ImVec2(bx + bw, py + U(52)), T("Add to Steam"), add->enabled,
                   CoopPressRow(*add));
    }
    // Y accepts the invite, X rejoins (as the prompts say).
    if (interactive && input_.Pressed(Action::kResetPage)) {
      input_.Consume(Action::kResetPage);
      if (accept) CoopPress({"accept", {}, {}, accept->enabled, 0, CoopPressRow(*accept)});
    }
    if (interactive && input_.Pressed(Action::kDefault)) {
      input_.Consume(Action::kDefault);
      if (!model.lobby && coop_rejoin_.ok) CoopPress({"rejoin", {}, {}, true, 0, [this] { CoopRejoinNow(); }});
    }
    CoopNavigate(coop_focus_[int(CoopPage::Hub)], interactive, "host");
  }

  // Host: New game or Load game (also the lobby's Change chapter).
  void DrawCoopHost(ImDrawList* dl, const coop::SocialModel& model, bool interactive) {
    const ui::Fonts f = ui::GetFonts();
    const float s = S();
    DrawCoopSteps(dl, 1);
    const auto rows = CoopRows(model, CoopPage::Host);
    const float L = o_.x + Margin(), R = o_.x + w_ - Margin(), cw = R - L;
    float y = o_.y + U(240);
    bool warn = false;
    if (const std::string line = CoopStatusLine(model, &warn); !line.empty()) {
      dl->AddText(f.semibold, 13.5f * s, ImVec2(L, y - 18 * s), warn ? color::kWarn : color::kText,
                  Ellipsize(f.semibold, 13.5f * s, line, cw).c_str());
    }
    const float card_w = (cw - U(36)) * 0.5f, card_h = U(520), pic_h = U(300);
    struct Card { const char* key; coop::SocialRowKind kind; const char* title; const char* text; int picture; };
    const Card cards[2] = {{"new", coop::SocialRowKind::HostNewGame, "New game", "Choose any chapter to play together.", 1},
                           {"load", coop::SocialRowKind::HostLoadGame, "Load game",
                            "Continue one of your saves together. Your partner plays it with you.", 20}};
    for (int i = 0; i < 2; ++i) {
      const Card& c = cards[i];
      const coop::SocialRow* row = CoopFind(rows, c.kind);
      const bool enabled = row && row->enabled;
      const ImVec2 a(L + float(i) * (card_w + U(36)), y), b(a.x + card_w, y + card_h);
      const bool on = CoopAdd(c.key, a, b, enabled, row ? CoopPressRow(*row) : std::function<void()>{});
      if (on) dl->AddRectFilled(ImVec2(a.x - 3 * s, a.y - 3 * s), ImVec2(b.x + 3 * s, b.y + 3 * s), ui::WithAlpha(color::kFocus, 0.18f));
      dl->AddRectFilled(a, b, on ? color::kFocus : ui::WithAlpha(color::kPanel, 0.9f));
      dl->AddRect(a, b, on ? color::kFocus : kCoopHair, 0, 0, 1.0f);
      const int entry = int(play::Chapters().size()) > c.picture ? play::Chapters()[size_t(c.picture)].entry : 0;
      CoopPicture(dl, entry, ImVec2(a.x + 1, a.y + 1), ImVec2(b.x - 1, a.y + pic_h), enabled ? 0.8f : 0.45f);
      float ty = a.y + pic_h + U(28);
      const float tx = a.x + U(28), tw = card_w - U(56);
      dl->AddText(f.display, 22.5f * s, ImVec2(tx, ty), on ? color::kOnFocus : enabled ? color::kText : color::kTextFaint, T(c.title));
      ty += 22.5f * s + U(12);
      const char* text = i == 1 && !enabled && UsedSlots() == 0 ? "No saves yet." : c.text;
      CoopText(dl, f.text, 13.5f * s, ImVec2(tx, ty), on ? IM_COL32(80, 83, 90, 255) : color::kTextDim, T(text), tw);
    }
    CoopNavigate(coop_focus_[int(CoopPage::Host)], interactive, "new");
  }

  // New game and Load game: the launcher's list on the left, the focused chapter's card on the right.
  void DrawCoopList(ImDrawList* dl, const coop::SocialModel& model, bool interactive) {
    const ui::Fonts f = ui::GetFonts();
    const float s = S();
    DrawCoopSteps(dl, 1);
    ui::ListState& st = coop_lists_[int(coop_page_)];
    std::vector<Item> items = coop_page_ == CoopPage::HostNew ? CoopChapterItems() : CoopSaveItems();
    if (st.focus < 0)
      for (size_t i = 0; i < items.size(); ++i)
        if (items[i].type == ItemType::kAction && !items[i].disabled) { st.focus = int(i); break; }
    const float L = o_.x + Margin(), R = o_.x + w_ - Margin(), cw = R - L;
    const float top = o_.y + U(240), bottom = o_.y + h_ - 96 * s, list_w = std::round(cw * 0.5f);
    bool warn = false;
    if (const std::string line = CoopStatusLine(model, &warn); !line.empty())
      dl->AddText(f.semibold, 13.5f * s, ImVec2(L, top - 18 * s), warn ? color::kWarn : color::kText,
                  Ellipsize(f.semibold, 13.5f * s, line, cw).c_str());
    const Item* focused = ui::DrawList(dl, items, st, ImVec2(L, top), ImVec2(L + list_w, bottom), m_, input_, interactive);
    (void)focused;
    int entry = -1;
    std::string over, name;
    if (st.focus >= 0 && st.focus < int(row_data_.size())) {
      const int data = row_data_[size_t(st.focus)];
      if (coop_page_ == CoopPage::HostNew && data >= 0) {
        entry = data;
        over = F("Chapter {0}", {std::to_string(CoopChapterNumber(entry))}) + "  " KK_DOT "  " + T("New game");
        name = T(ChapterName(entry));
      } else if (coop_page_ == CoopPage::HostLoad && SlotUsed(data)) {
        entry = ContinueChapter(saves_[size_t(data)]);
        over = F("Chapter {0}", {std::to_string(CoopChapterNumber(entry))}) + "  " KK_DOT "  " + T("Load game") + "  " KK_DOT
               "  " + SaveName(saves_[size_t(data)]);
        name = T(ChapterName(entry));
      }
    }
    if (entry >= 0) {
      const float cx = L + list_w + U(60), w = R - cx;
      const ImVec2 a(cx, top);
      const float pic_h = std::min(w * 9.0f / 16.0f, (bottom - top) * 0.55f);
      // Measure the body, then draw the panel under it.
      ImDrawListSplitter split;
      split.Split(dl, 2);
      split.SetCurrentChannel(dl, 1);
      const float end = DrawCoopChapterCard(dl, entry, a, w, pic_h, over, name) + U(26);
      split.SetCurrentChannel(dl, 0);
      dl->AddRectFilled(a, ImVec2(a.x + w, end), ui::WithAlpha(color::kPanel, 0.9f));
      dl->AddRect(a, ImVec2(a.x + w, end), kCoopHair, 0, 0, 1.0f);
      split.Merge(dl);
      dl->AddRectFilled(a, ImVec2(a.x + 3 * s, end), color::kAccent);
    }
    focused_items_ = std::move(items);
    focused_ = st.focus >= 0 && st.focus < int(focused_items_.size()) ? &focused_items_[size_t(st.focus)] : nullptr;
  }

  // Invite (the host, before the lobby exists): Steam friends, or a code by IP.
  void DrawCoopHow(ImDrawList* dl, const coop::SocialModel& model, bool interactive) {
    const ui::Fonts f = ui::GetFonts();
    const float s = S();
    DrawCoopSteps(dl, 2);
    const auto rows = CoopRows(model, CoopPage::HostHow);
    const float L = o_.x + Margin(), R = o_.x + w_ - Margin(), cw = R - L;
    const float top = o_.y + U(240), left_w = cw * 812.0f / 1728.0f, rx = L + cw * 872.0f / 1728.0f, rw = R - rx;
    bool warn = false;
    if (const std::string line = CoopStatusLine(model, &warn); !line.empty())
      dl->AddText(f.semibold, 13.5f * s, ImVec2(L, top - 18 * s), warn ? color::kWarn : color::kText,
                  Ellipsize(f.semibold, 13.5f * s, line, cw).c_str());
    // The chosen chapter or save, with Change.
    if (coop_choice_) {
      const auto& c = *coop_choice_;
      const bool load = c.kind == play::StartKind::kContinue && SlotUsed(c.slot);
      const int entry = load ? ContinueChapter(saves_[size_t(c.slot)]) : c.chapter;
      std::string over = F("Chapter {0}", {std::to_string(CoopChapterNumber(entry))}) + "  " KK_DOT "  " +
                         T(load ? "Load game" : "New game");
      if (load) over += "  " KK_DOT "  " + SaveName(saves_[size_t(c.slot)]);
      ImDrawListSplitter split;
      split.Split(dl, 2);
      split.SetCurrentChannel(dl, 1);
      float end = DrawCoopChapterCard(dl, entry, ImVec2(L, top), left_w, U(292), over, T(ChapterName(entry))) + U(22);
      CoopButton(dl, "change", ImVec2(L + U(30), end), ImVec2(L + U(30) + U(300), end + U(52)), T("Change chapter"), true,
                 [this] { CoopGo(CoopPage::Host); });
      end += U(52) + U(26);
      split.SetCurrentChannel(dl, 0);
      dl->AddRectFilled(ImVec2(L, top), ImVec2(L + left_w, end), ui::WithAlpha(color::kPanel, 0.9f));
      dl->AddRect(ImVec2(L, top), ImVec2(L + left_w, end), kCoopHair, 0, 0, 1.0f);
      split.Merge(dl);
      dl->AddRectFilled(ImVec2(L, top), ImVec2(L + 3 * s, end), color::kAccent);
    }
    // Two ways to invite.
    float y = top;
    auto panel = [&](const char* title, const char* text, auto&& body) {
      ImDrawListSplitter split;
      split.Split(dl, 2);
      split.SetCurrentChannel(dl, 1);
      const float x = rx + U(26), w = rw - U(52);
      float py = y + U(24);
      dl->AddText(f.display, 18 * s, ImVec2(x, py), color::kText, T(title));
      py += 18 * s + U(10);
      py += CoopText(dl, f.text, 12.5f * s, ImVec2(x, py), color::kTextDim, T(text), w) + U(16);
      body(x, py, w);
      py += U(24);
      split.SetCurrentChannel(dl, 0);
      dl->AddRectFilled(ImVec2(rx, y), ImVec2(rx + rw, py), ui::WithAlpha(color::kPanel, 0.9f));
      dl->AddRect(ImVec2(rx, y), ImVec2(rx + rw, py), kCoopHair, 0, 0, 1.0f);
      split.Merge(dl);
      y = py + U(18);
    };
    panel("Invite a Steam friend", "Opens a lobby only your Steam friends can join. Then invite a friend.",
          [&](float x, float& py, float w) {
            const float bw = std::min(w, U(420));
            if (const auto* connect = CoopFind(rows, coop::SocialRowKind::ConnectSteam)) {
              CoopButton(dl, "connect", ImVec2(x, py), ImVec2(x + bw, py + U(56)), T("Connect to Steam"), connect->enabled,
                         CoopPressRow(*connect));
              py += U(56);
            } else if (const auto* create = CoopFind(rows, coop::SocialRowKind::Create);
                       create && model.status != coop::SocialStatus::Unavailable && model.status != coop::SocialStatus::Disabled) {
              CoopButton(dl, "steam-host", ImVec2(x, py), ImVec2(x + bw, py + U(56)), T("Host with Steam friends"),
                         create->enabled, CoopPressRow(*create), Action::kCount, true);
              py += U(56);
            } else {
              py += CoopText(dl, f.text, 13 * s, ImVec2(x, py), color::kWarn, T("Steam is unavailable. You can connect by IP."), w);
            }
          });
    panel("Host with a code", "Your friend joins with your IP address and this code. Forward the UDP port on your router.",
          [&](float x, float& py, float w) {
            const auto* code = CoopFind(rows, coop::SocialRowKind::DirectCode);
            const auto* port = CoopFind(rows, coop::SocialRowKind::DirectPort);
            const auto* host = CoopFind(rows, coop::SocialRowKind::DirectHost);
            const float half = (w - U(30)) * 0.5f;
            float cy = py, py2 = py;
            if (code) CoopCodeBoxes(dl, "code", x, cy, half + U(80), social_.direct_code(), code->enabled, CoopPressRow(*code), 0);
            if (port)
              CoopField(dl, "port", x + half + U(110), py2, w - half - U(110), T("Port"), social_.direct_port(), port->enabled,
                        CoopPressRow(*port), 0);
            py = std::max(cy, py2) + U(22);
            if (host) {
              CoopButton(dl, "host-ip", ImVec2(x, py), ImVec2(x + std::min(w, U(420)), py + U(56)), T("Host by IP"),
                         host->enabled, CoopPressRow(*host), Action::kCount, true);
              py += U(56);
            }
          });
    CoopNavigate(coop_focus_[int(CoopPage::HostHow)], interactive, "steam-host");
  }

  // Join: invites, friends playing now, and join with a code.
  void DrawCoopJoin(ImDrawList* dl, const coop::SocialModel& model, bool interactive) {
    const ui::Fonts f = ui::GetFonts();
    const float s = S();
    DrawCoopIntro(dl, T("Join a friend's game"), T("Pick the way that suits you. All three put you in the same lobby as your friend."),
                  model);
    const auto rows = CoopRows(model, CoopPage::Join);
    const float L = o_.x + Margin(), R = o_.x + w_ - Margin(), cw = R - L, gap = U(36);
    const float col_w[3] = {cw * 548.0f / 1728.0f, cw * 548.0f / 1728.0f, cw - cw * 1096.0f / 1728.0f - gap * 2};
    const float top = o_.y + U(272), bottom = top + U(640);
    const bool steam_shown = !model.direct && model.status != coop::SocialStatus::Unavailable &&
                             model.status != coop::SocialStatus::Disabled && model.status != coop::SocialStatus::Dormant;
    float x = L;
    std::string first = "address";  // the page opens on the first invite, else on the host's IP address
    auto head = [&](int n, const char* title, const char* sub, int badge) {
      const float px = x + U(26), pw = col_w[n - 1] - U(52);
      dl->AddRectFilled(ImVec2(x, top), ImVec2(x + col_w[n - 1], bottom), ui::WithAlpha(color::kPanel, 0.9f));
      dl->AddRect(ImVec2(x, top), ImVec2(x + col_w[n - 1], bottom), kCoopHair, 0, 0, 1.0f);
      const float r = U(17), cy = top + U(26) + r;
      CoopNumber(dl, ImVec2(px + r, cy), r, std::to_string(n), 3);
      dl->AddText(f.display, 17.5f * s, ImVec2(px + r * 2 + U(14), cy - 17.5f * s * 0.6f), color::kText, T(title));
      if (badge > 0) {
        const std::string b = std::to_string(badge);
        const float bw = std::max(U(30), ui::TextSize(f.semibold, 11 * s, b).x + U(20));
        dl->AddRectFilled(ImVec2(px + pw - bw, cy - U(15)), ImVec2(px + pw, cy + U(15)), color::kAccent, U(15));
        const ImVec2 ts = ui::TextSize(f.semibold, 11 * s, b);
        dl->AddText(f.semibold, 11 * s, ImVec2(px + pw - bw * 0.5f - ts.x * 0.5f, cy - ts.y * 0.5f), color::kOnFocus, b.c_str());
      }
      return cy + r + U(8) + CoopText(dl, f.text, 12 * s, ImVec2(px, cy + r + U(8)), color::kTextDim, T(sub), pw) + U(20);
    };
    // 1: invites.
    {
      std::vector<const coop::SocialRow*> accepts;
      for (const auto& r : rows) if (r.key.kind == coop::SocialRowKind::Accept) accepts.push_back(&r);
      if (!accepts.empty()) first = "accept:" + std::to_string(accepts.front()->key.id);
      float y = head(1, "Invites", "When a friend invites you on Steam, it shows up here.", int(accepts.size()));
      const float px = x + U(26), pw = col_w[0] - U(52);
      if (const auto* connect = CoopFind(rows, coop::SocialRowKind::ConnectSteam)) {
        CoopButton(dl, "connect", ImVec2(px, y), ImVec2(px + pw, y + U(56)), T("Connect to Steam"), connect->enabled,
                   CoopPressRow(*connect), Action::kCount, false, 0);
        y += U(56) + U(12);
        y += CoopText(dl, f.text, 12 * s, ImVec2(px, y), color::kTextFaint, T("Steam invites and friends need Steam."), pw);
      } else if (!steam_shown && accepts.empty()) {
        CoopText(dl, f.text, 12.5f * s, ImVec2(px, y), color::kTextDim, T("Steam is unavailable. You can connect by IP."), pw);
      }
      size_t shown = 0;
      for (const auto* acc : accepts) {
        const coop::SocialRow* decline = CoopFind(rows, coop::SocialRowKind::Decline, acc->key.id);
        const bool first = shown == 0;
        const float card_h = (first ? U(132) : 0.0f) + U(150);
        if (y + card_h > bottom - U(60)) break;
        const ImVec2 a(px, y), b(px + pw, y + card_h);
        const std::string akey = "accept:" + std::to_string(acc->key.id), dkey = "decline:" + std::to_string(acc->key.id);
        const auto it = coop_focus_.find(int(CoopPage::Join));
        const bool on = it != coop_focus_.end() && (it->second == akey || it->second == dkey);
        dl->AddRectFilled(a, b, on ? color::kFocus : IM_COL32(255, 255, 255, 10));
        dl->AddRect(a, b, on ? color::kFocus : kCoopHair, 0, 0, 1.0f);
        float iy = a.y;
        if (first) {
          const int entry = play::Chapters().size() > 1 ? play::Chapters()[1].entry : 0;
          CoopPicture(dl, entry, ImVec2(a.x + 2, a.y + 2), ImVec2(b.x - 2, a.y + U(132)), 0.8f);
          iy += U(132);
        }
        const float r = U(24), cy = iy + U(16) + r;
        CoopAvatar(dl, ImVec2(a.x + U(18) + r, cy), r, acc->detail.empty() ? "?" : acc->detail, nullptr);
        const float tx = a.x + U(18) + r * 2 + U(14);
        const std::string who = acc->detail.empty() ? std::string(T("A friend invited you")) : F("{0} invited you", {acc->detail});
        dl->AddText(f.semibold, 13.5f * s, ImVec2(tx, cy - 14 * s), on ? color::kOnFocus : color::kText,
                    Ellipsize(f.semibold, 13.5f * s, who, b.x - tx - U(18)).c_str());
        dl->AddText(f.text, 11.5f * s, ImVec2(tx, cy + 2 * s), on ? color::kOnFocusDim : color::kTextDim, T("Steam invite"));
        const float by = cy + r + U(16), bw = (pw - U(36) - U(10)) * 0.5f;
        CoopButton(dl, akey, ImVec2(a.x + U(18), by), ImVec2(a.x + U(18) + bw, by + U(48)), T("Accept"), acc->enabled,
                   CoopPressRow(*acc), Action::kAccept, true, 0);
        if (decline)
          CoopButton(dl, dkey, ImVec2(a.x + U(28) + bw, by), ImVec2(a.x + U(28) + bw * 2, by + U(48)), T("Decline"),
                     decline->enabled, CoopPressRow(*decline), Action::kCount, false, 0);
        y = b.y + U(14);
        ++shown;
      }
      if (shown < accepts.size())
        y += CoopText(dl, f.text, 12 * s, ImVec2(px, y), color::kTextDim,
                      F("{0} more invites", {std::to_string(accepts.size() - shown)}), pw) + U(10);
      if (accepts.empty() && steam_shown) {
        y += CoopText(dl, f.text, 13 * s, ImVec2(px, y), color::kTextDim, T("No invites right now."), pw) + U(10);
      }
      if (steam_shown || !accepts.empty())
        CoopText(dl, f.text, 11.5f * s, ImVec2(px, y), color::kTextFaint,
                 T("Invites from your Steam friends appear here while the game is open."), pw);
      x += col_w[0] + gap;
    }
    // 2: friends playing now.
    {
      float y = head(2, "Friends playing now", "Steam friends who are in King Kong right now.", 0);
      const float px = x + U(26), pw = col_w[1] - U(52);
      if (!steam_shown) {
        y += CoopText(dl, f.text, 12.5f * s, ImVec2(px, y), color::kTextDim,
                      T("Connect to Steam to see friends who are playing now."), pw);
      } else {
        int n = 0;
        auto friend_row = [&](const std::string& name, const std::string& sub, const coop::SocialFriend* fr,
                              const coop::SocialRow* join, const char* state) {
          const ImVec2 a(px, y), b(px + pw, y + U(70));
          const std::string key = join ? "friend:" + std::to_string(join->key.id) : std::string();
          bool on = false;
          if (join) on = CoopAdd(key, a, b, join->enabled, CoopPressRow(*join), 1);
          dl->AddRectFilled(a, b, on ? color::kFocus : IM_COL32(255, 255, 255, 6));
          dl->AddRect(a, b, on ? color::kFocus : IM_COL32(255, 255, 255, 20), 0, 0, 1.0f);
          const float r = U(23), cy = (a.y + b.y) * 0.5f;
          CoopAvatar(dl, ImVec2(a.x + U(14) + r, cy), r, name, fr ? social_.Avatar(*fr, immediate_) : nullptr);
          const float tx = a.x + U(14) + r * 2 + U(14);
          const std::string act = T(join ? "Join" : state);
          const float aw = ui::TextSize(f.semibold, 12 * s, act).x + U(32);
          dl->AddText(f.semibold, 13 * s, ImVec2(tx, cy - 14 * s), on ? color::kOnFocus : join ? color::kText : color::kTextDim,
                      Ellipsize(f.semibold, 13 * s, name, b.x - tx - aw - U(28)).c_str());
          dl->AddText(f.text, 11.5f * s, ImVec2(tx, cy + 2 * s), on ? color::kOnFocusDim : color::kTextDim,
                      Ellipsize(f.text, 11.5f * s, T(sub), b.x - tx - aw - U(28)).c_str());
          const ImVec2 ba(b.x - U(14) - aw, cy - U(19)), bb(b.x - U(14), cy + U(19));
          if (join) dl->AddRect(ba, bb, on ? IM_COL32(0, 0, 0, 80) : kCoopHair, 0, 0, 1.0f);
          else CoopDashedRect(dl, ba, bb, kCoopHair);
          dl->AddText(join ? f.semibold : f.text, 12 * s, ImVec2(ba.x + U(16), cy - 12 * s * 0.55f),
                      on ? color::kOnFocus : join ? color::kText : color::kTextFaint, act.c_str());
          y = b.y + U(14);
          ++n;
        };
        for (const auto& r : rows) {
          if (r.key.kind != coop::SocialRowKind::JoinFriend || y + U(70) > bottom - U(90)) continue;
          friend_row(r.label, "Playing King Kong", CoopFriend(model, r.key.id), &r, nullptr);
        }
        for (const auto& fr : model.friends) {  // in the game but not hosting: shown, not joinable
          if (fr.id == model.self || !fr.in_our_game || fr.lobby || fr.state == coop::FriendState::Offline ||
              y + U(70) > bottom - U(90))
            continue;
          friend_row(fr.name, "In the game, not hosting", &fr, nullptr, "Not hosting");
        }
        if (n == 0) y += CoopText(dl, f.text, 13 * s, ImVec2(px, y), color::kTextDim, T("No friends are playing now"), pw) + U(10);
        y += U(8);
        CoopText(dl, f.text, 11.5f * s, ImVec2(px, y), color::kTextFaint,
                 T("Friend not listed? They need Steam open and the same version of King Kong. You can also join with their code."), pw);
      }
      x += col_w[1] + gap;
    }
    // 3: join with a code.
    {
      float y = head(3, "Join with a code", "For games hosted by IP. The host sees their address and code on their lobby screen.", 0);
      const float px = x + U(26), pw = col_w[2] - U(52);
      const auto* address = CoopFind(rows, coop::SocialRowKind::DirectAddress);
      const auto* code = CoopFind(rows, coop::SocialRowKind::DirectCode);
      const auto* port = CoopFind(rows, coop::SocialRowKind::DirectPort);
      const auto* join = CoopFind(rows, coop::SocialRowKind::DirectJoin);
      if (address)
        CoopField(dl, "address", px, y, pw, T("Host's IP address"), social_.direct_address(), address->enabled,
                  CoopPressRow(*address), 2);
      y += U(20);
      if (code) CoopCodeBoxes(dl, "code", px, y, pw, social_.direct_code(), code->enabled, CoopPressRow(*code), 2);
      y += U(20);
      if (port) {
        CoopField(dl, "port", px, y, pw, T("Port"), social_.direct_port(), port->enabled, CoopPressRow(*port), 2, true);
        y += U(8);
        CoopText(dl, f.text, 11.5f * s, ImVec2(px, y), color::kTextFaint, T("Leave it as it is unless the host changed it."), pw);
      }
      if (join)
        CoopButton(dl, "join-ip", ImVec2(px, bottom - U(24) - U(58)), ImVec2(px + pw, bottom - U(24)), T("Join game"),
                   join->enabled, CoopPressRow(*join), Action::kCount, true, 2);
    }
    // The note under the columns.
    CoopInfoIcon(dl, ImVec2(L + U(10), bottom + U(30) + 7 * s), U(9.5f), color::kAccent);
    CoopText(dl, f.text, 12 * s, ImVec2(L + U(32), bottom + U(30)), color::kTextDim,
             T("Both players need the same version of King Kong. If joining fails, the screen tells you why and what to try."),
             cw - U(32));
    CoopNavigate(coop_focus_[int(CoopPage::Join)], interactive, first);
  }

  // The lobby: the steps, the chapter, the players, Ready and Start.
  void DrawCoopLobby(ImDrawList* dl, const coop::SocialModel& model, bool interactive) {
    const ui::Fonts f = ui::GetFonts();
    const float s = S();
    const auto rows = CoopRows(model, CoopPage::Lobby);
    const float L = o_.x + Margin(), R = o_.x + w_ - Margin(), cw = R - L;
    bool warn = false;
    const std::string status = CoopStatusLine(model, &warn);
    if (!model.lobby) {  // the lobby is being made or joined (or Steam starts for an accepted invite)
      DrawCoopSteps(dl, coop_choice_ ? 2 : 1);
      const float w = std::min(cw, U(900)), x = o_.x + (w_ - w) * 0.5f, y = o_.y + U(330);
      dl->AddRectFilled(ImVec2(x, y), ImVec2(x + w, y + U(260)), ui::WithAlpha(color::kPanel, 0.92f));
      dl->AddRect(ImVec2(x, y), ImVec2(x + w, y + U(260)), kCoopHair, 0, 0, 1.0f);
      const std::string line = status.empty() ? std::string(T("Joining the lobby")) : status;
      const ImVec2 ts = ui::TextSize(f.display, 22 * s, line, w - U(80));
      dl->AddText(f.display, 22 * s, ImVec2(x + (w - std::min(ts.x, w - U(80))) * 0.5f, y + U(60)), warn ? color::kWarn : color::kText,
                  line.c_str(), nullptr, w - U(80));
      if (const auto* leave = CoopFind(rows, coop::SocialRowKind::Leave))
        CoopButton(dl, "leave", ImVec2(x + w * 0.5f - U(170), y + U(160)), ImVec2(x + w * 0.5f + U(170), y + U(216)),
                   T("Cancel"), leave->enabled, CoopPressRow(*leave));
      CoopNavigate(coop_focus_[int(CoopPage::Lobby)], interactive, "leave");
      return;
    }
    DrawCoopSteps(dl, coop::CoopLobbyStep(model));
    const bool host = model.owner == model.self;
    const coop::SocialMember* self = nullptr;
    const coop::SocialMember* partner = nullptr;
    for (const auto& p : model.members) (p.id == model.self ? self : partner) = &p;
    const coop::SocialMember* host_member = host ? self : partner;
    const float top = o_.y + U(240), left_w = cw * 812.0f / 1728.0f, rx = L + cw * 872.0f / 1728.0f, rw = R - rx;
    // Left: the chapter card with Change chapter, Invite with Steam and Leave lobby.
    const auto* change = CoopFind(rows, coop::SocialRowKind::ChangeChoice);
    const auto* overlay = CoopFind(rows, coop::SocialRowKind::OverlayInvite);
    const auto* leave = CoopFind(rows, coop::SocialRowKind::Leave);
    const bool inviting = host && !partner;
    float end;
    {
      ImDrawListSplitter split;
      split.Split(dl, 2);
      split.SetCurrentChannel(dl, 1);
      const float pic_h = inviting ? U(230) : U(292);
      if (model.choice.valid()) {
        const int entry = model.choice.chapter;
        const std::string over = F("Chapter {0}", {std::to_string(CoopChapterNumber(entry))}) + "  " KK_DOT "  " +
                                 T(model.choice.continue_game ? "Load game" : "New game");
        end = DrawCoopChapterCard(dl, entry, ImVec2(L, top), left_w, pic_h, over, T(ChapterName(entry))) + U(22);
      } else {
        dl->AddRectFilled(ImVec2(L, top), ImVec2(L + left_w, top + pic_h), IM_COL32(20, 23, 28, 255));
        dl->AddText(f.display, 24 * s, ImVec2(L + U(30), top + pic_h - U(22) - 26 * s), color::kTextDim, T("Not chosen yet"));
        end = top + pic_h + U(22);
        end += CoopText(dl, f.text, 13.5f * s, ImVec2(L + U(30), end), color::kTextDim,
                        T(host ? "Choose a chapter or one of your saves." : "The host chooses a chapter or a save."),
                        left_w - U(60)) + U(22);
      }
      float bx = L + U(30);
      const float bh = U(52);
      auto button = [&](const char* key, const char* label, const coop::SocialRow* row, Action glyph) {
        const float w = ui::TextSize(f.semibold, 14 * s, T(label)).x + (glyph != Action::kCount ? 28 * s : 0.0f) + U(44);
        CoopButton(dl, key, ImVec2(bx, end), ImVec2(bx + w, end + bh), T(label), row->enabled, CoopPressRow(*row), glyph);
        bx += w + U(14);
      };
      if (change && host) button("change", model.choice.valid() ? "Change chapter" : "Choose a chapter", change, Action::kDefault);
      if (overlay) button("invite", "Invite with Steam", overlay, Action::kResetPage);
      if (leave) button("leave", "Leave lobby", leave, Action::kCount);
      end += bh + U(26);
      split.SetCurrentChannel(dl, 0);
      dl->AddRectFilled(ImVec2(L, top), ImVec2(L + left_w, end), ui::WithAlpha(color::kPanel, 0.9f));
      dl->AddRect(ImVec2(L, top), ImVec2(L + left_w, end), kCoopHair, 0, 0, 1.0f);
      split.Merge(dl);
      dl->AddRectFilled(ImVec2(L, top), ImVec2(L + 3 * s, end), color::kAccent);
    }
    // Under it, while the host waits for a partner: the details to share (by IP) or Steam friends to invite.
    if (inviting) {
      float y = end + U(26);
      if (model.direct) {
        CoopSection(dl, L, y, left_w, T("Share these details"));
        y += 12 * s + U(18);
        const std::pair<const char*, std::string> lines[] = {{"IP address", model.direct_addresses},
                                                              {"Port", std::to_string(model.direct_port)},
                                                              {"Join code", model.direct_code}};
        for (const auto& [caption, value] : lines) {
          dl->AddText(f.text, 13 * s, ImVec2(L, y), color::kTextDim, T(caption));
          dl->AddText(f.semibold, 14 * s, ImVec2(L + U(200), y - 1 * s), color::kText,
                      Ellipsize(f.semibold, 14 * s, value.empty() ? std::string(T("Not entered yet")) : value, left_w - U(200)).c_str());
          y += 14 * s + U(14);
        }
      } else if (overlay) {
        CoopSection(dl, L, y, left_w, T("Invite a friend"));
        y += 12 * s + U(16);
        int n = 0;
        for (const auto& r : rows) {
          if (r.key.kind != coop::SocialRowKind::Friend || n >= 3 || y + U(64) > o_.y + h_ - 70 * s) continue;
          const ImVec2 a(L, y), b(L + left_w, y + U(60));
          const bool on = CoopAdd("friend:" + std::to_string(r.key.id), a, b, r.enabled, CoopPressRow(r));
          dl->AddRectFilled(a, b, on ? color::kFocus : IM_COL32(255, 255, 255, 6));
          dl->AddRect(a, b, on ? color::kFocus : IM_COL32(255, 255, 255, 20), 0, 0, 1.0f);
          const float rr = U(20), cy = (a.y + b.y) * 0.5f;
          const auto* fr = CoopFriend(model, r.key.id);
          CoopAvatar(dl, ImVec2(a.x + U(12) + rr, cy), rr, r.label, fr ? social_.Avatar(*fr, immediate_) : nullptr);
          const float tx = a.x + U(12) + rr * 2 + U(14);
          dl->AddText(f.semibold, 13 * s, ImVec2(tx, cy - 9 * s), on ? color::kOnFocus : r.enabled ? color::kText : color::kTextDim,
                      Ellipsize(f.semibold, 13 * s, r.label, left_w * 0.5f).c_str());
          dl->AddText(f.text, 11.5f * s, ImVec2(tx + std::min(ui::TextSize(f.semibold, 13 * s, r.label).x, left_w * 0.5f) + U(14),
                                                cy - 8 * s), on ? color::kOnFocusDim : color::kTextDim, T(r.detail).c_str());
          const std::string act = T("Invite");
          const float aw = ui::TextSize(f.semibold, 12 * s, act).x;
          dl->AddText(f.semibold, 12 * s, ImVec2(b.x - U(20) - aw, cy - 7 * s),
                      on ? color::kOnFocus : r.enabled ? color::kText : color::kTextFaint, act.c_str());
          y = b.y + U(10);
          ++n;
        }
        if (n == 0) CoopText(dl, f.text, 12.5f * s, ImVec2(L, y), color::kTextDim, T("No friends to show"), left_w);
      }
    }
    // Right: the players.
    float y = top;
    CoopSection(dl, rx, y, rw, T("Players"), F("{0} of {1}", {std::to_string(model.members.size()), "2"}));
    y += 12 * s + U(18);
    const coop::SocialMember* order[2] = {host_member, host ? partner : self};
    for (int i = 0; i < 2; ++i) {
      const ImVec2 a(rx, y), b(rx + rw, y + U(128));
      const coop::SocialMember* p = order[i];
      const float r = U(38), cy = (a.y + b.y) * 0.5f;
      if (!p) {
        dl->AddRectFilled(a, b, IM_COL32(14, 16, 20, 120));
        CoopDashedRect(dl, a, b, IM_COL32(255, 255, 255, 56));
        dl->AddCircle(ImVec2(a.x + U(28) + r, cy), r, IM_COL32(255, 255, 255, 50), 0, 1.5f * s);
        dl->AddText(f.semibold, 15 * s, ImVec2(a.x + U(28) + r * 2 + U(24), cy - 18 * s), color::kTextDim,
                    T(model.direct ? "Waiting for your partner to join" : "Waiting for a friend to join"));
        dl->AddText(f.text, 12 * s, ImVec2(a.x + U(28) + r * 2 + U(24), cy + 4 * s), color::kTextFaint,
                    T(host ? (model.direct ? "They join with your IP address, port and code." : "Invite a Steam friend to this lobby.")
                           : "Connecting to the host"));
      } else {
        dl->AddRectFilled(a, b, ui::WithAlpha(color::kPanel, 0.9f));
        dl->AddRect(a, b, p->ready ? ui::WithAlpha(color::kGood, 0.5f) : kCoopHair, 0, 0, 1.0f);
        if (p->ready) dl->AddRectFilled(a, ImVec2(a.x + 3 * s, b.y), color::kGood);
        const auto* fr = CoopFriend(model, p->id);
        CoopAvatar(dl, ImVec2(a.x + U(28) + r, cy), r, p->name, fr ? social_.Avatar(*fr, immediate_) : nullptr);
        const float tx = a.x + U(28) + r * 2 + U(24);
        dl->AddText(f.semibold, 16.5f * s, ImVec2(tx, cy - 20 * s), color::kText,
                    Ellipsize(f.semibold, 16.5f * s, p->name, rw * 0.5f).c_str());
        std::string role = T(p->id == model.owner ? "Host" : "Partner");
        if (p->id == model.self) role += "  " KK_DOT "  " + std::string(T("You"));
        ui::DrawTracked(dl, f.display, 10 * s, ImVec2(tx, cy + 6 * s), color::kAccent, Upper(role), 2 * s);
        CoopPill(dl, b.x - U(28), cy, T(p->ready ? "Ready" : "Not ready yet"), p->ready ? 1 : 2, true);
      }
      y = b.y + U(18);
    }
    // Ready, then Start (the host) or who starts (the partner).
    y = std::max(y + U(12), top + U(344));
    {
      const auto* ready = CoopFind(rows, coop::SocialRowKind::Ready);
      const ImVec2 a(rx, y), b(rx + rw, y + U(92));
      const bool me = self && self->ready;
      const bool on = ready && CoopAdd("ready", a, b, ready->enabled, CoopPressRow(*ready));
      const bool enabled = ready && ready->enabled;
      if (on) dl->AddRectFilled(ImVec2(a.x - 3 * s, a.y - 3 * s), ImVec2(b.x + 3 * s, b.y + 3 * s), ui::WithAlpha(color::kFocus, 0.16f));
      dl->AddRectFilled(a, b, on ? color::kFocus : enabled ? IM_COL32(255, 255, 255, 14) : IM_COL32(255, 255, 255, 6));
      if (!on) dl->AddRect(a, b, me ? ui::WithAlpha(color::kGood, 0.5f) : kCoopHair, 0, 0, 1.0f);
      const float cy = (a.y + b.y) * 0.5f;
      const float gw = ui::DrawGlyph(dl, glyphs_, Action::kAccept, PromptSet(), a.x + U(28), cy, 27 * s, m_, true);
      const float tx = a.x + U(28) + gw + U(20);
      dl->AddText(f.semibold, 16.5f * s, ImVec2(tx, cy - 19 * s), on ? color::kOnFocus : enabled ? color::kText : color::kTextFaint,
                  T("Ready"));
      const char* sub = !model.choice.valid() ? (host ? "Choose a chapter or save first" : "The host has not chosen what to play yet")
                        : me ? "Press again if you need more time" : "Press when you are set to play";
      dl->AddText(f.text, 12 * s, ImVec2(tx, cy + 3 * s), on ? IM_COL32(80, 83, 90, 255) : color::kTextDim, T(sub));
      if (me) {
        const std::string t = T("You are ready");
        const float tw = ui::TextSize(f.semibold, 13 * s, t).x;
        const ImU32 col = on ? IM_COL32(63, 122, 74, 255) : color::kGood;
        dl->AddText(f.semibold, 13 * s, ImVec2(b.x - U(28) - tw, cy - 13 * s * 0.55f), col, t.c_str());
        CoopCheck(dl, ImVec2(b.x - U(28) - tw - 10 * s, cy), 12 * s, col);
      }
      y = b.y + U(16);
    }
    {
      const ImVec2 a(rx, y), b(rx + rw, y + U(92));
      const float cy = (a.y + b.y) * 0.5f;
      if (host) {
        const auto* start = CoopFind(rows, coop::SocialRowKind::Start);
        const bool enabled = start && start->enabled;
        const bool on = start && CoopAdd("start", a, b, enabled, CoopPressRow(*start));
        const auto lock = coop::CoopStartLock(model);
        if (on && enabled) dl->AddRectFilled(a, b, color::kFocus);
        else if (enabled) dl->AddRectFilled(a, b, ui::WithAlpha(color::kAccent, 0.14f)), dl->AddRect(a, b, color::kAccent, 0, 0, 1.0f);
        else {
          if (on) dl->AddRect(a, b, color::kFocus, 0, 0, 1.5f * s);
          else CoopDashedRect(dl, a, b, IM_COL32(255, 255, 255, 56));
        }
        const ImU32 head = on && enabled ? color::kOnFocus : enabled ? color::kText : color::kTextFaint;
        CoopPlayIcon(dl, ImVec2(a.x + U(28) + 11 * s, cy), 22 * s, head);
        const float tx = a.x + U(28) + 22 * s + U(20);
        dl->AddText(f.semibold, 16.5f * s, ImVec2(tx, cy - 19 * s), head, T("Start game"));
        const std::string why = !lock.key[0] ? std::string(T("Both players are ready"))
                                : std::string_view(lock.key).find("{0}") != std::string_view::npos
                                    ? F(lock.key, {lock.name.empty() ? std::string(T("your partner")) : lock.name})
                                    : std::string(T(lock.key));
        dl->AddText(f.text, 12 * s, ImVec2(tx, cy + 3 * s), on && enabled ? IM_COL32(80, 83, 90, 255) : color::kTextDim,
                    Ellipsize(f.text, 12 * s, why, rw - (tx - a.x) - U(70)).c_str());
        if (!enabled) CoopLockIcon(dl, ImVec2(b.x - U(28) - 9 * s, cy), 18 * s, color::kTextFaint);
      } else {
        CoopDashedRect(dl, a, b, IM_COL32(255, 255, 255, 40));
        CoopPlayIcon(dl, ImVec2(a.x + U(28) + 11 * s, cy), 22 * s, color::kTextFaint);
        const float tx = a.x + U(28) + 22 * s + U(20);
        dl->AddText(f.semibold, 16.5f * s, ImVec2(tx, cy - 19 * s), color::kTextDim, T("The host starts the game"));
        const std::string who = host_member ? host_member->name : std::string(T("the host"));
        dl->AddText(f.text, 12 * s, ImVec2(tx, cy + 3 * s), color::kTextDim,
                    Ellipsize(f.text, 12 * s, model.start_requested ? std::string(T("Starting co-op...")) : F("Waiting for {0} to start", {who}),
                              rw - (tx - a.x) - U(28)).c_str());
      }
      y = b.y + U(18);
    }
    // The status (an error or a notice), then what happens at Start.
    if (warn && !status.empty()) {
      y += CoopText(dl, f.semibold, 13 * s, ImVec2(rx, y), color::kWarn, status, rw) + U(8);
    }
    const std::string chapter = model.choice.valid() ? std::string(T(ChapterName(model.choice.chapter))) : std::string(T("the chapter"));
    CoopInfoIcon(dl, ImVec2(rx + U(10), y + 7 * s), U(9.5f), color::kAccent);
    CoopText(dl, f.text, 12 * s, ImVec2(rx + U(32), y), color::kTextDim,
             F("When the host starts, both games load {0} and connect again. It takes a few seconds. If the host changes the "
               "chapter, both players press Ready again.", {chapter}),
             rw - U(32));
    // X changes the chapter (the host), Y opens Steam's invite (as the prompts say).
    if (interactive && input_.Pressed(Action::kDefault)) {
      input_.Consume(Action::kDefault);
      if (change && host) CoopPress({"change", {}, {}, change->enabled, 0, CoopPressRow(*change)});
    }
    if (interactive && input_.Pressed(Action::kResetPage)) {
      input_.Consume(Action::kResetPage);
      if (overlay) CoopPress({"invite", {}, {}, overlay->enabled, 0, CoopPressRow(*overlay)});
    }
    CoopNavigate(coop_focus_[int(CoopPage::Lobby)], interactive, "ready");
  }

  // ------------------------------------------------------ early access warning ---
  // Craig (29 Oct): every time PLAY ONLINE CO-OP opens (and before a Steam invite start opens the pages), full
  // screen; for 5 seconds nothing closes it (a countdown on Continue). Then A / Enter / a click on Continue goes on to
  // the pages and B / Esc goes back to the main menu.
  std::filesystem::path CoopLogsFolder() const {
    std::filesystem::path log = rex::cvar::GetFlagByName("log_file");
    if (log.empty()) return rex::filesystem::GetExecutableFolder() / "logs";
    if (log.is_relative()) log = rex::filesystem::GetExecutableFolder() / log;
    return log.parent_path();
  }
  void CoopWarningContinue() {
    coop_gate_.Close();
    REXLOG_INFO("KK co-op pages: early access warning: Continue");
    if (const coop::SocialId lobby = coop_gate_lobby_) {
      coop_gate_lobby_ = 0;
      CoopGo(CoopPage::Lobby);
      coop::Social().Act({coop::SocialAction::Accept, lobby});
    }
  }
  void CoopWarningBack() {
    coop_gate_.Close();
    coop_gate_lobby_ = 0;
    REXLOG_INFO("KK co-op pages: early access warning: back to the main menu");
    play_ = PlayView::kNone;
  }
  void DrawCoopWarning(ImDrawList* dl) {
    const ui::Fonts f = ui::GetFonts();
    const float s = S();
    const double now = ImGui::GetTime();
    const bool ready = coop_gate_.Ready(now);
    const bool interactive = ready && !PageInputBlocked();
    if (!ready) input_.ConsumeAll();  // nothing closes it yet
    dl->AddRectFilled(o_, ImVec2(o_.x + w_, o_.y + h_), ui::WithAlpha(color::kBase, 0.93f));
    const float pw = std::min(U(1260), w_ - Margin() * 2), px = o_.x + (w_ - pw) * 0.5f, pad = U(48);
    const float tx = px + pad, tw = pw - pad * 2;
    // Laid out first on the top channel, its panel drawn under it once its height is known.
    ImDrawListSplitter split;
    split.Split(dl, 2);
    split.SetCurrentChannel(dl, 1);
    const float est_h = U(900);
    const float top = o_.y + std::max(U(40), (h_ - 60 * s - est_h) * 0.5f);
    float y = top + U(44);
    CoopWarnIcon(dl, ImVec2(tx + 13 * s, y + 12 * s), 26 * s, color::kWarn);
    ui::DrawTracked(dl, f.display, 12 * s, ImVec2(tx + 40 * s, y + 5 * s), color::kWarn,
                    Upper(std::string(T("Early access")) + "  " KK_DOT "  " + T("Alpha")), 2.6f * s);
    y += 26 * s + U(20);
    y += CoopText(dl, f.display, 28 * s, ImVec2(tx, y), color::kText, T("Online co-op is in early access"), tw) + U(16);
    y += CoopText(dl, f.text, 14.5f * s, ImVec2(tx, y), color::kTextDim,
                  T("Online co-op is at an alpha stage. You will run into random issues, and you may not be able to progress "
                    "the game."), tw) + U(10);
    y += CoopText(dl, f.text, 14.5f * s, ImVec2(tx, y), color::kTextDim,
                  T("For a smooth experience, play the single player game. Play online co-op to try it early and to help us "
                    "by reporting bugs."), tw) + U(28);
    CoopSection(dl, tx, y, tw, T("Known issues"));
    y += 12 * s + U(18);
    static const char* const kIssues[] = {"Desync between players", "Random teleports out of the map", "Random guest reloads",
                                          "Mission state can get stuck", "NPCs not in sync", "Some chapters fail to load",
                                          "Not every chapter has a player model for the guest"};
    const float col = (tw - U(30)) * 0.5f;
    float cy[2] = {y, y};
    for (size_t i = 0; i < std::size(kIssues); ++i) {
      const int c = i < 4 ? 0 : 1;
      const float ix = tx + float(c) * (col + U(30));
      dl->AddCircleFilled(ImVec2(ix + 4 * s, cy[c] + 9 * s), 3 * s, color::kWarn);
      cy[c] += CoopText(dl, f.text, 14 * s, ImVec2(ix + 16 * s, cy[c]), color::kText, T(kIssues[i]), col - 16 * s) + U(10);
    }
    y = std::max(cy[0], cy[1]) + U(18);
    CoopSection(dl, tx, y, tw, T("Report a bug"));
    y += 12 * s + U(16);
    y += CoopText(dl, f.text, 14 * s, ImVec2(tx, y), color::kTextDim, T("Tell us on the Discord and send your logs folder."), tw) + U(16);
    const float bh = U(56), bw = std::min(U(330), (tw - U(14)) * 0.5f);
    coop_targets_.clear();
    // The warning's targets live under its own focus key (CoopAdd reads the page's).
    const CoopPage page = coop_page_;
    std::string& focus = coop_gate_focus_;
    auto add = [&](const std::string& key, ImVec2 a, ImVec2 b, std::function<void()> press) {
      coop_targets_.push_back({key, a, b, ready, 0, std::move(press)});
      return focus == key;
    };
    auto button = [&](const std::string& key, ImVec2 a, ImVec2 b, const std::string& label, Action glyph, bool primary,
                      std::function<void()> press) {
      const bool on = add(key, a, b, std::move(press)) && ready;
      dl->AddRectFilled(a, b, on ? color::kFocus : primary && ready ? ui::WithAlpha(color::kAccent, 0.2f) : IM_COL32(255, 255, 255, 10));
      if (!on) dl->AddRect(a, b, primary && ready ? color::kAccent : kCoopHair, 0, 0, 1.0f);
      const float fs = 14.5f * s, cyb = (a.y + b.y) * 0.5f;
      const float gw = glyph != Action::kCount ? ui::DrawGlyph(dl, glyphs_, glyph, PromptSet(), 0, 0, 22 * s, m_, false) + 8 * s : 0;
      const float lw = ui::TextSize(f.semibold, fs, label).x;
      float x = (a.x + b.x - lw - gw) * 0.5f;
      if (gw > 0 && ready) ui::DrawGlyph(dl, glyphs_, glyph, PromptSet(), x, cyb, 22 * s, m_, true);
      if (gw > 0 && ready) x += gw;
      else if (gw > 0) x += gw * 0.5f;
      dl->AddText(f.semibold, fs, ImVec2(x, cyb - fs * 0.55f), on ? color::kOnFocus : ready ? color::kText : color::kTextFaint,
                  label.c_str());
    };
    button("discord", ImVec2(tx, y), ImVec2(tx + bw, y + bh), T("Join the Discord"), Action::kCount, false, [] { OpenUrl(kDiscordUrl); });
    button("logs", ImVec2(tx + bw + U(14), y), ImVec2(tx + bw * 2 + U(14), y + bh), T("Open logs folder"), Action::kCount, false,
           [this] { OpenInExplorer(CoopLogsFolder()); });
    y += bh + U(40);
    dl->AddLine(ImVec2(tx, y), ImVec2(tx + tw, y), kCoopHair);
    y += U(28);
    const int left = coop_gate_.SecondsLeft(now);
    const float cw = std::min(U(400), (tw - U(14)) * 0.5f);
    button("back", ImVec2(tx + tw - cw * 2 - U(14), y), ImVec2(tx + tw - cw - U(14), y + bh + U(8)), T("Back to main menu"),
           Action::kBack, false, [this] { CoopWarningBack(); });
    const ImVec2 ca(tx + tw - cw, y), cb(tx + tw, y + bh + U(8));
    button("continue", ca, cb, left > 0 ? F("Continue ({0})", {std::to_string(left)}) : std::string(T("Continue")),
           Action::kAccept, true, [this] { CoopWarningContinue(); });
    if (!ready) {  // the countdown, as a brass bar filling along Continue's bottom edge
      const float k = float(std::clamp((now - coop_gate_.opened_at()) / coop::EarlyAccessGate::kSeconds, 0.0, 1.0));
      dl->AddRectFilled(ImVec2(ca.x, cb.y - 3 * s), ImVec2(ca.x + (cb.x - ca.x) * k, cb.y), color::kAccent);
    }
    y = cb.y + U(44);
    split.SetCurrentChannel(dl, 0);
    dl->AddRectFilled(ImVec2(px, top), ImVec2(px + pw, y), color::kPanel);
    dl->AddRect(ImVec2(px, top), ImVec2(px + pw, y), kCoopHair, 0, 0, 1.0f);
    dl->AddRectFilled(ImVec2(px, top), ImVec2(px + pw, top + 4 * s), color::kWarn);
    split.Merge(dl);
    (void)page;
    if (!ready) return;
    CoopNavigate(focus, interactive, "continue");
    if (!coop_gate_.open()) return;  // a press above closed it
    if (interactive && input_.Pressed(Action::kBack)) {
      input_.Consume(Action::kBack);
      ui::Cue(ui::Sound::kBack);
      CoopWarningBack();
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

  // Home's PLAY ONLINE CO-OP: how many Steam invites are waiting ("1 invite"), else empty.
  std::string CoopInviteBadge() {
    const size_t n = coop::Social().Snapshot().pending_invites.size();
    if (!n) return {};
    return n == 1 ? std::string(T("1 invite")) : F("{0} invites", {std::to_string(n)});
  }

  bool CoopTargetEnabled(const std::string& key) const {
    for (const auto& t : coop_targets_) if (t.key == key) return t.enabled;
    return false;
  }
  std::vector<ui::Prompt> CoopPrompts() const {
    if (social_.editing()) return {};
    if (coop_gate_.open()) {
      if (!coop_gate_.Ready(ImGui::GetTime())) return {};
      return {{Action::kAccept, "Select"}, {Action::kBack, "Back"}};
    }
    std::vector<ui::Prompt> p;
    if (coop_page_ == CoopPage::Join) p.push_back({Action::kNextTab, "Switch"});
    p.push_back({Action::kAccept, "Select"});
    if (coop_page_ == CoopPage::Hub) {
      if (CoopTargetEnabled("accept")) p.push_back({Action::kResetPage, "Accept invite"});
      if (CoopTargetEnabled("rejoin")) p.push_back({Action::kDefault, "Rejoin"});
    }
    if (coop_page_ == CoopPage::Lobby) {
      if (CoopTargetEnabled("change")) p.push_back({Action::kDefault, "Change chapter"});
      if (CoopTargetEnabled("invite")) p.push_back({Action::kResetPage, "Invite"});
    }
    p.push_back({Action::kBack, "Back"});
    return p;
  }

#if defined(KK_DEV_TOOLS)
  // Developer input: "coop:<page>" opens a page (from outside the pages: after the early access warning);
  // "coop:wait:<page>" waits until that page shows (accepting the warning once its countdown is over);
  // "coop:warning-ok" accepts the warning once it can be closed, "coop:wait:warning-ready" waits for that;
  // "coop:focus:<target>" moves the focus to a target of the page shown (as the directions would; then pad:a
  // presses it), "coop:wait-enabled:<target>" waits until it can be pressed. Targets: hub: accept, host, join,
  // rejoin, connect, add-steam; host: new, load; how: change, connect, steam-host, code, port, host-ip; join:
  // accept:<id>, decline:<id>, friend:<id>, connect, address, code, port, join-ip; lobby: change, invite, leave,
  // friend:<id>, ready, start.
  bool CoopDevInput(const std::string& command) {
    static const std::pair<const char*, CoopPage> pages[] = {
        {"hub", CoopPage::Hub}, {"host", CoopPage::Host}, {"new", CoopPage::HostNew}, {"load", CoopPage::HostLoad},
        {"how", CoopPage::HostHow}, {"join", CoopPage::Join}, {"lobby", CoopPage::Lobby}};
    for (const auto& [name, page] : pages)
      if (command == name) { if (play_ != PlayView::kCoop) OpenCoop(page); else CoopGo(page); return true; }
    if (command == "warning-ok" || command == "wait:warning-ready") {
      if (play_ != PlayView::kCoop || !coop_gate_.Ready(ImGui::GetTime())) return false;
      if (command == "warning-ok") {
        REXLOG_INFO("KK dev: early access warning accepted");
        CoopWarningContinue();
      }
      return true;
    }
    for (const bool focus : {true, false}) {
      const std::string prefix = focus ? "focus:" : "wait-enabled:";
      if (command.rfind(prefix, 0) != 0) continue;
      if (play_ != PlayView::kCoop || coop_gate_.open()) return false;
      const std::string name = command.substr(prefix.size());
      // Old names (C81 scripts): "join" on Home is its Join card, "new" and "load" the Host page's cards.
      for (const auto& t : coop_targets_) {
        if (t.key != name) continue;
        if (focus) coop_focus_[int(coop_page_)] = name;
        return focus || t.enabled;
      }
      return false;
    }
    if (command.rfind("wait:", 0) == 0) {
      for (const auto& [name, page] : pages) {
        if (command.substr(5) != name) continue;
        if (play_ == PlayView::kCoop && coop_gate_.open() && coop_gate_.Ready(ImGui::GetTime())) {
          REXLOG_INFO("KK dev: early access warning accepted (waiting for a co-op page)");
          CoopWarningContinue();
        }
        return play_ == PlayView::kCoop && !coop_gate_.open() && coop_page_ == page;
      }
      return false;
    }
    return false;
  }
#endif
