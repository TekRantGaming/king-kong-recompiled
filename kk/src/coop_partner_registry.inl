// Included in coop_partner_body.cpp's anonymous namespace. Read-only guest
// census and provenance live here to keep the combat/driver edits separate.
bool RegistryTable(const uint8_t* b, uint32_t world, uint32_t& begin, uint32_t& end) {
  if (!Range(world, 44)) return false;
  begin = Load32(b, world + 40); end = Load32(b, world + 28);
  return end >= begin && end - begin <= 8 * 20000 && !((end - begin) & 7) && Range(begin, end - begin);
}
struct RegistryEvidence {
  bool valid = false, complete = false, pool_valid = false;
  uint32_t entries = 0, skipped = 0, budget_exhausted = 0, refs_dropped = 0;
  std::map<uint32_t, unsigned> members, scheduled;
  std::set<uint32_t> pool_claims;
  struct Reference {
    uint32_t human = 0, owner = 0, ai = 0, model = 0, offset = 0;
    uint32_t record = 0, index = ~0u, current = ~0u, callback = 0, payload_offset = ~0u;
    const char* kind = "unknown-vars-reference";
  };
  std::vector<Reference> refs;
  Clock::time_point logged{};
};
RegistryEvidence registry_evidence;

// This deliberately makes no activation calls. Neither registration nor a
// palette-preparation hook proves a dormant body is drawn. Until a native
// setter + actual body-draw audit exists, every unscheduled body is refused.
bool UnscheduledConfirmed(const Rig&) { return false; }

void RefreshRegistryEvidence(const uint8_t* b, uint32_t world) {
  const auto logged = registry_evidence.logged;
  registry_evidence = {};
  auto& e = registry_evidence;
  e.logged = logged;
  uint32_t begin, end, sb, se;
  if (!Table(b, world, sb, se)) return;
  for (uint32_t p = sb; p < se; p += 8) ++e.scheduled[Load32(b, p)];
  if (!RegistryTable(b, world, begin, end)) return;
  e.valid = true; e.complete = true; e.entries = (end - begin) / 8;
  for (uint32_t p = begin; p < end; p += 8) ++e.members[Load32(b, p)];
  std::set<uint32_t> human_models, human_candidates;
  for (const auto& [actor, obs] : Get().humans) {
    human_candidates.insert(actor);
    const auto ai = Ptr(b, Ptr(b, actor, 24, 8), 4, 88);
    if (ai == obs.rig.ai && Ptr(b, ai, 64, 8) == obs.rig.vars) {
      if (const auto model = Ptr(b, ai, 0, 4)) human_models.insert(model);
    }
  }
  for (const auto& [actor, n] : e.members) {
    const auto ai = Ptr(b, Ptr(b, actor, 24, 8), 4, 88);
    if (human_models.count(Ptr(b, ai, 0, 4))) human_candidates.insert(actor);
  }
  const auto count = Load32(b, 0x82CCBFCC);
  e.pool_valid = count <= 512;
  if (e.pool_valid) for (uint32_t i = 0; i < count; ++i) {
    const auto p = 0x82CC3100 + 16 * i;
    e.pool_claims.insert(Load32(b, p)); e.pool_claims.insert(Load32(b, p + 4));
  }
  size_t budget = 8 * 1024 * 1024;
  for (const auto& [owner, occurrences] : e.members) {
    if (!Range(owner, 32)) { ++e.skipped; e.complete = false; continue; }
    const auto ext = Load32(b, owner + 24);
    if (!ext) continue;
    if (!Range(ext, 8)) { ++e.skipped; e.complete = false; continue; }
    const auto ai = Load32(b, ext + 4);
    if (!ai) continue;
    if (!Range(ai, 88)) { ++e.skipped; e.complete = false; continue; }
    const auto size = VarsSize(b, ai), vars = Load32(b, ai + 64);
    if (size > 1024 * 1024 || (size && !Range(vars, size))) {
      ++e.skipped; e.complete = false; continue;
    }
    if (size > budget) { ++e.budget_exhausted; e.complete = false; continue; }
    budget -= size;
    Rig tracks; tracks.ai = ai;
    if (!ReadTracks(b, tracks)) { ++e.skipped; e.complete = false; }
    bool prg = false;
    const auto first = Load16(b, ai + 80), last = Load16(b, ai + 82);
    if (first <= last && last <= 4096 && last - first < 64 && Range(ai, (last + 12) * 4)) {
      for (uint32_t t = first; t <= last; ++t) {
        const auto block = Load32(b, ai + (t + 6) * 4);
        if (!block) continue;
        if (!Range(block, 24) || !Load32(b, block + 20)) { e.complete = false; continue; }
        prg |= Load32(b, block + 20) == 0x82454690;
      }
    }
    const uint32_t current = prg && size >= 19820 ? Load32(b, vars + 19812) : ~0u;
    if (prg && (size < 19820 || current >= 104)) e.complete = false;
    for (uint32_t off = 0; off + 4 <= size; off += 4) {
      const auto human = Load32(b, vars + off);
      if (human == owner || !human_candidates.count(human)) continue;
      // All resident owners protect a human, including sleeping PRGs and
      // cached outputs. Classification is diagnostic, never a lease exemption.
      script_evidence.references.try_emplace(human, owner);
      if (prg && off >= 920 && off < 920 + 104 * 180 && size >= 19820 && current < 104) {
        const uint32_t index = (off - 920) / 180;
        const uint32_t distance = index > current ? index-current : current-index;
        auto [it, added] = script_evidence.mission_distance.emplace(human, distance);
        if (!added) it->second = std::min(it->second, distance);
        if (index == current && e.scheduled.count(owner) && Load32(b, vars+924+180*index))
          script_evidence.current_references.insert(human);
      }
      if (e.refs.size() == 512) { ++e.refs_dropped; continue; }
      RegistryEvidence::Reference ref;
      ref.human = human; ref.owner = owner; ref.ai = ai;
      ref.model = Ptr(b, ai, 0, 4); ref.offset = off; ref.current = current;
      if (prg && off >= 920 && off < 920 + 104 * 180 && size >= 19820) {
        ref.index = (off - 920) / 180;
        ref.record = vars + 920 + ref.index * 180;
        ref.callback = Load32(b, ref.record + 4);
        const auto in_record = (off - 920) % 180;
        if (in_record >= 72) {
          ref.payload_offset = in_record - 72;
          if (ref.callback == 0x82449F18 && ref.payload_offset == 4) ref.kind = "input-CapaBit-actor";
          else if (ref.callback == 0x82449F18 && ref.payload_offset == 20) ref.kind = "cached-output-CapaBit-actor";
          else ref.kind = "unknown-PRG-payload-reference";
        } else ref.kind = "PRG-header-reference";
      }
      e.refs.push_back(ref);
    }
  }
}

#if defined(KK_DEV_TOOLS)
void LogRegistry(const uint8_t* b) {
  auto& s = Get(); auto& e = registry_evidence;
  const auto now = Clock::now();
  // A census can contain hundreds of reference lines. Keep normal co-op smoke
  // logs long enough to retain session/load evidence; explicit scans keep the
  // original one-second cadence for body investigations.
  static const bool detailed = [] {
    const char* puppet = std::getenv("KK_DEV_PUPPET");
    const char* registry = std::getenv("KK_DEV_PUPPET_REGISTRY");
    return (puppet && (std::string(puppet) == "scan" || std::string(puppet) == "1")) ||
        (registry && std::string(registry) == "1");
  }();
  if (e.logged != Clock::time_point{} && now - e.logged < std::chrono::seconds(detailed ? 1 : 30)) return;
  e.logged = now;
  REXLOG_INFO("PUPPET registry epoch={} level={} world={:08X} valid={} entries={} owners-complete={} skipped={} budget-exhausted={} refs={} refs-dropped={} pool-valid={} human-observations={} absence-is-not-global-proof=true",
      s.epoch, s.level, s.world, e.valid, e.entries, e.complete, e.skipped, e.budget_exhausted,
      e.refs.size(), e.refs_dropped, e.pool_valid, s.humans.size());
  std::set<uint32_t> human_models, spawner_models;
  for (const auto& [actor, obs] : s.humans) {
    const auto ai = Ptr(b, Ptr(b, actor, 24, 8), 4, 88);
    if (ai == obs.rig.ai && Ptr(b, ai, 64, 8) == obs.rig.vars) {
      if (const auto model = Ptr(b, ai, 0, 4)) human_models.insert(model);
    }
  }
  for (const auto& [actor, obs] : s.spawners) {
    const auto ai = Ptr(b, Ptr(b, actor, 24, 8), 4, 88);
    if (ai == obs.ai && Ptr(b, ai, 64, 8380) == obs.vars) {
      if (const auto model = Ptr(b, ai, 0, 4)) spawner_models.insert(model);
    }
  }
  unsigned logged_humans = 0, logged_other = 0, logged_spawners = 0;
  for (const auto& [actor, n] : e.members) {
    if (!Range(actor, 32)) continue;
    const auto observation = s.humans.find(actor);
    const auto spawner = s.spawners.find(actor);
    // Registry membership does not prove a human animation/AI layout. In the
    // raft chapter, decoding an unrelated actor followed an unmapped pointer.
    if (observation == s.humans.end() && spawner == s.spawners.end()) {
      ++logged_other; continue;
    }
    const auto ai = Ptr(b, Ptr(b, actor, 24, 8), 4, 88);
    const auto current_vars = Ptr(b, ai, 64, 8);
    if (!ai || !current_vars ||
        (observation != s.humans.end() && (ai != observation->second.rig.ai || current_vars != observation->second.rig.vars)) ||
        (spawner != s.spawners.end() && (ai != spawner->second.ai || current_vars != spawner->second.vars))) {
      ++logged_other; continue;
    }
    const auto model = Ptr(b, ai, 0, 4);
    const bool h_model = human_models.count(model) != 0, spawner_model = spawner_models.count(model) != 0;
    const auto vars_bytes = VarsSize(b, ai);
    const auto vars = Ptr(b, ai, 64, 1780);
    if (spawner != s.spawners.end() && vars == spawner->second.vars &&
        vars_bytes >= 1780 && vars_bytes <= 1024 * 1024 && vars) {
      if (logged_spawners++ < 128) for (uint32_t off : {1592u, 1776u})
        REXLOG_INFO("PUPPET registry spawner owner={:08X} ai={:08X} model={:08X} vars-offset={} source={:08X} init-observed={} registered=true scheduled={}",
            actor, ai, model, off, Load32(b, vars + off), true, e.scheduled.count(actor) != 0);
    }
    if (observation == s.humans.end()) continue;
    if (logged_humans++ >= 256) continue;
    Rig r;
    const bool intact = ReadRig(b, actor, r);
    const bool tracks_valid = intact;
    const auto ext = Ptr(b, actor, 24, 48);
    const bool initialized = observation != s.humans.end() && r.ai == observation->second.rig.ai &&
        r.vars == observation->second.rig.vars && (!observation->second.bound || Same(r, observation->second.rig));
    REXLOG_INFO("PUPPET registry human epoch={} actor={:08X} ai={:08X} model={:08X} observed-H-model={} observed-spawner-model={} vars={:08X} vars-bytes={} init-observed={} init-generation={} intact={} registry-count={} scheduled-count={} dead={} cinematic={} follow={} grabbed={} Ann={} flags={:08X}/{:08X} lifecycle={:02X} importance={} ext46={:04X} pool-claimed={} movement-and-draw-confirmed={} xyz={} tracks-valid={} tracks={}",
        s.epoch, actor, r.ai, model, h_model, spawner_model, r.vars, VarsSize(b, r.ai), initialized,
        initialized ? observation->second.generation : 0, intact, n, e.scheduled.count(actor) ? e.scheduled.at(actor) : 0,
        r.dead_track, r.cinematic, r.following, r.grabbed, r.ann, Load32(b, actor), Load32(b, actor + 4),
        b[actor + 31], b[actor + 28], ext ? Load16(b, ext + 46) : 0, e.pool_claims.count(actor) != 0,
        UnscheduledConfirmed(r), intact ? XYZ(r.pos) : "unavailable", tracks_valid, r.tracks);
  }
  for (const auto& r : e.refs) REXLOG_INFO("PUPPET registry reference human={:08X} owner={:08X} owner-scheduled={} ai={:08X} model={:08X} vars-offset={} PRG-record={:08X} record-index={} current-index={} callback={:08X} payload-offset={} provenance={} protected=true reentry-and-same-dispatch-not-exempt=true",
      r.human, r.owner, e.scheduled.count(r.owner) != 0, r.ai, r.model, r.offset,
      r.record, r.index, r.current, r.callback, r.payload_offset, r.kind);
  const auto count = Load32(b, 0x82CCBFCC);
  if (e.pool_valid) for (uint32_t i = 0; i < count; ++i) {
    const auto p = 0x82CC3100 + 16 * i, source = Load32(b, p), copy = Load32(b, p + 4);
    REXLOG_INFO("PUPPET registry pool record={} source={:08X} instance={:08X} field8={:08X} in-use={} source-init={} instance-init={}",
        i, source, copy, Load32(b, p + 8), Load32(b, p + 12), s.humans.count(source) != 0, s.humans.count(copy) != 0);
  }
  REXLOG_INFO("PUPPET registry limits human-lines={} human-lines-dropped={} unproved-layouts-skipped={} spawner-lines-dropped={} unscheduled-policy=skip-without-native-movement-and-body-draw-proof activation=none",
      std::min(logged_humans, 256u), logged_humans > 256 ? logged_humans - 256 : 0,
      logged_other,
      logged_spawners > 128 ? logged_spawners - 128 : 0);
}
#endif
