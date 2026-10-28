// Included in coop_objects_game.cpp's private namespace. Native contracts are
// scripts40_ode.c/scripts41_ode.c and scripts39_btode.c/scripts40_btode.c.
// This adapter deliberately covers detach endpoints, not remove-all roots,
// arbitrary completion callbacks, personal pickup scripts or linked transforms.
uint32_t Physics(const uint8_t* b,uint32_t actor) {
  return PoseAddress(actor,32) && (Load32(b,actor)&0x10000000) ? Ptr(b,Ptr(b,actor,20,12),8,0x78) : 0;
}
bool PersonalOrHuman(const uint8_t* b,uint32_t actor) {
  if (actor==Load32(b,0x82CCBF7C)) return true;
  const auto ai=Ptr(b,Ptr(b,actor,24,8),4,88);
  for (const auto fn:{0x826718B8u,0x8264EC58u,0x8262A7A0u,0x82644A60u,
      0x8251C540u,0x8251BA60u,0x8251B800u,0x824F35E0u,0x824F3820u,
      0x8257C560u,0x8258E228u,0x8258C5F8u,0x825CE2F0u}) if (Track(b,ai,fn)) return true;
  return false;
}
bool UnitBody(const uint8_t* b,uint32_t actor) {
  const auto m=Ptr(b,actor,12,64),ph=Physics(b,actor);
  if (!m || !ph || !Ptr(b,ph,0x70,32) || b[ph+13]==4 || !(b[ph+14]&1) ||
      (Load32(b,actor)&0x00400025) || PersonalOrHuman(b,actor)) return false;
  std::array<float,9> basis;
  for (unsigned row=0;row<3;++row) for (unsigned c=0;c<3;++c) basis[3*row+c]=LoadF(b,m+16*row+4*c);
  return objects::RigidSupportBasis(basis);
}
#if defined(KK_DEV_TOOLS)
uint64_t DevSupportSource() {
  static const uint64_t source=[] {
    const auto value=std::getenv("KK_DEV_OBJECT_COLLAPSE_SOURCE"); if (!value || !*value) return uint64_t(0);
    char* end=nullptr; const auto id=std::strtoull(value,&end,16);
    return end!=value && !*end && id ? uint64_t(id) : UINT64_MAX;
  }();
  return source;
}
unsigned DevSupportPart() {
  static const unsigned part=[] {
    const auto value=std::getenv("KK_DEV_OBJECT_COLLAPSE_PART"); if (!value || !*value) return 0u;
    char* end=nullptr; const auto index=std::strtoul(value,&end,10);
    return end!=value && !*end && index<32 ? unsigned(index) : 32u;
  }();
  return part;
}
// Read-only native group relationships. Pickup children2..6 are NOT physical
// support identities. No relationship is inferred from matching positions.
void SupportRelations(const uint8_t* b,uint32_t actor,const char* family) {
  const auto host=SessionFlowSnapshot().host;
  const auto link=Ptr(b,actor,20,16),pose=Ptr(b,link,12,88);
  const auto group=(Load32(b,actor)&2) ? Ptr(b,link,0,0x58) : 0;
  const auto list=Ptr(b,Ptr(b,group,8,4),0,28);
  const auto begin=list ? Load32(b,list+16) : 0,end=list ? Load32(b,list+4) : 0;
  const auto swap=Ptr(b,group,0x54,0x84);
  REXLOG_INFO("KK dev object support relation: role={} family={} root={:08X} type={:08X} parent={:08X} group={:08X} physics={:08X} native-owner={:08X}",
      host ? "host" : "guest",family,actor,Load32(b,actor),pose ? Load32(b,pose+80) : 0,group,Physics(b,actor),PoseAddress(actor,36) ? Load32(b,actor+32) : 0);
  if (!list || b[list+20]!=0x1E || !PoseAddress(begin) || end<begin || end-begin>64*8 || (end-begin)%8 || !PoseAddress(begin,end-begin)) return;
  for (unsigned n=0;n<(end-begin)/8;++n) {
    const auto child=Load32(b,begin+8*n); if (!PoseAddress(child,32)) continue;
    const auto child_pose=Ptr(b,Ptr(b,child,20,16),12,88);
    REXLOG_INFO("KK dev object support relation: role={} family={} root={:08X} slot={} child={:08X} type={:08X} flags={:08X} parent={:08X} physics={:08X} native-owner={:08X}",
        host ? "host" : "guest",family,actor,swap ? unsigned(b[swap+4+n]) : n,child,Load32(b,child),Load32(b,child+4),
        child_pose ? Load32(b,child_pose+80) : 0,Physics(b,child),PoseAddress(child,36) ? Load32(b,child+32) : 0);
  }
}
#endif
void PrepareSupport(const uint8_t* b,Binding& o) {
  auto& s=o.support;
  s.definition=Ptr(b,Ptr(b,o.ai,0,4),0,8);
  const bool tree=o.kind==ObjectKind::Tree;
  const auto base=tree ? 4u : 20u;
  bool terminated=false;
  for (unsigned i=0;i<=32;++i) {
    const auto actor=Load32(b,o.vars+base+4*i);
    if (!actor) { terminated=true; break; }
    if (i==32 || !PoseAddress(actor,32)) break;
    s.parts.push_back(actor); s.all.push_back(actor);
  }
  for (const auto offset:tree ? std::vector<unsigned>{0x7C4,0x7F0} : std::vector<unsigned>{2500,2504,2548})
    if (const auto actor=Load32(b,o.vars+offset)) {
      if (!PoseAddress(actor,32)) return;
      s.all.push_back(actor); // Include duplicate aliases; their slots are authored too.
    }
  const auto mode=Load32(b,o.vars+(tree ? 0x52C : 0));
  s.eligible=terminated && !s.parts.empty() && mode>=1 && mode<=3 &&
      !(Load32(b,o.actor)&0x400001) &&
      (tree || (!Load32(b,o.vars+2496) && !Load32(b,o.vars+2256) && !Load32(b,o.vars+3368)));
  // C105: a member without a rigid unit ODE body (no physics record, a scaled
  // mesh, a disabled or attached piece) no longer withdraws its whole root. The
  // host still owns the root's trigger and End, the guest replays them through
  // its own native Wait and End, and only that member keeps its own native
  // motion instead of host poses. Personal pickups and humans still refuse.
  for (const auto actor:s.all) {
    const auto member=objects::ClassifySupportMember(PersonalOrHuman(b,actor),Physics(b,actor)!=0,UnitBody(b,actor));
    if (member==objects::SupportMember::Refuse) s.eligible=false;
    else if (member==objects::SupportMember::Local) s.local.insert(actor);
  }
  // A host can first bind after a native loader has already advanced the root.
  // Retain a generic collapse plus the final physical outcome in that case.
  if (Track(b,o.ai,tree ? 0x826E7BF0 : 0x826F1E08)) s.phase=1;
  if (Track(b,o.ai,tree ? 0x826D0770 : 0x826F4948)) s.phase=2;
  if (tree && Load32(b,o.vars+0x930) && !Track(b,o.ai,0x826E6390) &&
      !Track(b,o.ai,0x826E78F0) && !Track(b,o.ai,0x826E7BF0)) s.phase=2;
  if (s.phase) s.event.trigger=1;
  if (Trace()) REXLOG_INFO("KK dev object support profile: role={} id={:016X} family={} mode={} parts={} members={} eligible={} local={}",
      state.host ? "host" : "guest",o.source,tree ? "tree" : "ode",mode,s.parts.size(),s.all.size(),s.eligible,s.local.size());
  // The contract fields that decide eligibility, so that an excluded root
  // names its exact condition (C99 could not capture this for level 101).
  if (Trace()) REXLOG_INFO("KK dev object support contract: role={} id={:016X} terminated={} type={:08X} remove-all={:08X} watch={:08X} partner={:08X} refs={:08X}/{:08X}/{:08X}",
      state.host ? "host" : "guest",o.source,terminated,Load32(b,o.actor),
      tree ? 0u : Load32(b,o.vars+2496),tree ? 0u : Load32(b,o.vars+2256),tree ? 0u : Load32(b,o.vars+3368),
      Load32(b,o.vars+(tree ? 0x7C4 : 2500)),Load32(b,o.vars+(tree ? 0x7F0 : 2504)),tree ? 0u : Load32(b,o.vars+2548));
#if defined(KK_DEV_TOOLS)
  if (Trace()) REXLOG_INFO("KK dev object support activation contract: role={} id={:016X} push={} strength={} throw={:08X} speed={} end-delay={}",
      state.host ? "host" : "guest",o.source,Load32(b,o.vars+(tree ? 0x794 : 2436)),LoadF(b,o.vars+(tree ? 0x798 : 2440)),
      Load32(b,o.vars+(tree ? 0x7F0 : 2548)),LoadF(b,o.vars+(tree ? 0x7F4 : 2552)),LoadF(b,o.vars+(tree ? 0x7BC : 2480)));
  if (Trace()) for (unsigned i=0;i<s.all.size();++i) {
    const auto actor=s.all[i],matrix=Ptr(b,actor,12,64),physics=Physics(b,actor);
    REXLOG_INFO("KK dev object support member: role={} root={:016X} actor={:08X} slot={} part={:08X} pos=({:.3f},{:.3f},{:.3f}) physics={:08X} flags={} unit={}",
        state.host ? "host" : "guest",o.source,o.actor,i,actor,
        matrix ? LoadF(b,matrix+48) : 0,matrix ? LoadF(b,matrix+52) : 0,matrix ? LoadF(b,matrix+56) : 0,
        physics,physics ? unsigned(b[physics+14]) : 0,UnitBody(b,actor));
  }
#endif
}
bool BodyValid(const uint8_t* b,const State::Body& p) {
  return PoseAddress(p.actor,32) && Ptr(b,p.actor,12,64)==p.matrix && Ptr(b,p.actor,20,12)==p.link &&
      Physics(b,p.actor)==p.physics && objects::BodyHandle([&](uint32_t a){return Load32(b,a);},p.physics).has_value() && !PersonalOrHuman(b,p.actor);
}
bool RefreshBody(const uint8_t* b,State::Body& p) {
  if (!BodyValid(b,p)) return false;
  const auto body=*objects::BodyHandle([&](uint32_t a){return Load32(b,a);},p.physics);
  if (p.body!=body) { p.body=body; p.saved_body=false; p.driven=false; }
  return true;
}
void FreezeSupport(const uint8_t* b) {
  if (state.support_frozen || !state.census_started) return;
  std::vector<objects::PartRoot> roots;
  for (auto& [actor,o]:state.bindings) if (objects::SupportRoot(o.kind)) {
    if (!Valid(b,o) || Match(o.source,o.kind)!=&o || o.support.all.empty()) {
      state.support_frozen=true;
      for (auto& [other,binding]:state.bindings) if (objects::SupportRoot(binding.kind)) {
        binding.support.eligible=false; WithdrawRaft(b,binding);
      }
      return;
    }
    if (!state.host) {
      const auto* authority=state.replica.Find(o.kind,o.source);
      if (o.support.eligible && !authority) return;
      if (authority && authority->value==3) o.support.eligible=false;
    }
    roots.push_back({o.source,o.support.all});
  }
  const auto ids=objects::CanonicalParts(roots);
  if (ids.empty()) return;
  // A shared body cannot be half owned by an excluded root. Exclusion spreads
  // through the complete alias graph before any guest root or body is driven.
  bool changed=true;
  while (changed) {
    changed=false; std::set<uint32_t> excluded;
    for (const auto& [actor,o]:state.bindings) if (objects::SupportRoot(o.kind) && !o.support.eligible)
      excluded.insert(o.support.all.begin(),o.support.all.end());
    for (auto& [actor,o]:state.bindings) if (objects::SupportRoot(o.kind) && o.support.eligible)
      for (const auto part:o.support.all) if (excluded.contains(part)) { o.support.eligible=false; changed=true; break; }
  }
  if (!state.host) {
    // Match the host's complete alias contract before driving any component.
    // A private reload can use retained contracts immediately. Disjoint native
    // families that the host explicitly declined do not block supported ones.
    for (const auto& root:roots) {
      auto* o=Match(root.source,ObjectKind::Ode); if (!o) o=Match(root.source,ObjectKind::Tree);
      if (!o || !o->support.eligible) continue;
      const auto* s=state.replica.Find(o->kind,o->source);
      if (!s || s->collapse.topology!=objects::RootTopology(root,ids)) return;
    }
  }
  state.support_frozen=true;
  for (const auto& root:roots) {
    auto* o=Match(root.source,ObjectKind::Ode); if (!o) o=Match(root.source,ObjectKind::Tree);
    if (!o || !o->support.eligible) continue;
    o->support.topology=objects::RootTopology(root,ids);
    unsigned driven=0;
    for (const auto actor:root.parts) {
      // A local member keeps its own native motion; it never becomes a body
      // that receives or publishes host poses (UnitBody is per actor, so every
      // root that aliases it agrees).
      if (o->support.local.contains(actor)) continue;
      ++driven;
      auto [it,added]=state.bodies.try_emplace(actor);
      auto& p=it->second;
      if (added) {
        p.actor=actor; p.matrix=Ptr(b,actor,12,64); p.link=Ptr(b,actor,20,12); p.physics=Physics(b,actor);
        p.body=Ptr(b,p.physics,0x74,0x108); p.source=ids.at(actor).source; p.topology=ids.at(actor).topology;
        p.home={LoadF(b,p.matrix+48),LoadF(b,p.matrix+52),LoadF(b,p.matrix+56)};
      }
      if (std::find(p.roots.begin(),p.roots.end(),o->actor)==p.roots.end()) p.roots.push_back(o->actor);
    }
    if (Trace()) REXLOG_INFO("KK dev object support bound: role={} id={:016X} topology={:016X} family={} members={} driven={} local={} phase={}",
        state.host ? "host" : "guest",o->source,o->support.topology,uint8_t(o->kind),root.parts.size(),driven,
        root.parts.size()-driven,o->support.phase);
  }
  if (Trace()) REXLOG_INFO("KK dev object support bodies: role={} canonical={} roots={}",state.host ? "host" : "guest",state.bodies.size(),roots.size());
}
void InvalidateSupport(const uint8_t* b,uint32_t actor) {
  std::set<uint32_t> excluded{actor}; bool changed=true;
  while (changed) {
    changed=false;
    for (const auto& [part,p]:state.bodies) {
      if (std::none_of(p.roots.begin(),p.roots.end(),[&](uint32_t root){return excluded.contains(root);})) continue;
      for (const auto root:p.roots) changed|=excluded.insert(root).second;
    }
  }
  for (const auto root:excluded) if (auto it=state.bindings.find(root); it!=state.bindings.end()) {
    it->second.support.eligible=false; WithdrawRaft(b,it->second);
  }
}
bool SupportListValid(const uint8_t* b,const Binding& o) {
  const auto offset=o.kind==ObjectKind::Tree ? 4u : 20u;
  for (unsigned i=0;i<o.support.parts.size();++i)
    if (Load32(b,o.vars+offset+4*i)!=o.support.parts[i]) return false;
  return Load32(b,o.vars+offset+4*o.support.parts.size())==0;
}
bool GuestSupportLoading() {
  if (state.ready || !REXCVAR_GET(kk_coop_enabled) || !SessionInProcess()) return false;
  const auto flow=SessionFlowSnapshot();
  return flow.active && !flow.host && flow.policy==ChapterPolicy::Together;
}
void SupportTrigger(PPCContext& ctx,uint8_t* b,bool tree) {
  auto* o=state.ready && Shared() ? Current(b,tree ? ObjectKind::Tree : ObjectKind::Ode) : nullptr;
  auto native=tree ? __imp__sub_826D3568 : __imp__sub_826F0418;
  if (!o && GuestSupportLoading()) { ctx.r3.u64=0; return; }
  if (!o || !o->support.eligible) { native(ctx,b); return; }
  if (!SupportListValid(b,*o)) {
    InvalidateSupport(b,o->actor); native(ctx,b); return;
  }
  const unsigned offsets[3]{tree ? 0x620u : 1880u,tree ? 0x6D8u : 2064u,tree ? 0x790u : 2248u};
  const auto hit=tree ? 0x79Cu : 2444u,vector=tree ? 0x7A0u : 2448u;
  if (state.host) {
    native(ctx,b);
#if defined(KK_DEV_TOOLS)
    if (o->support.fixture && !o->support.phase) {
      static const unsigned fixture_trigger=[] {
        const auto value=std::getenv("KK_DEV_OBJECT_COLLAPSE_TRIGGER");
        return value && (*value=='2' || *value=='4') ? unsigned(*value-'0') : 1u;
      }();
      const auto part_index=DevSupportPart();
      if (fixture_trigger==2 && part_index>=o->support.parts.size()) return;
      for (unsigned i=0;i<3;++i) Store32(b,o->vars+offsets[i],bool(fixture_trigger&(1u<<i)));
      if (fixture_trigger==2 && !o->support.parts.empty()) {
        const auto part=o->support.parts[part_index],matrix=Ptr(b,part,12,64);
        if (matrix) {
          Store32(b,o->vars+hit,part);
          StoreF(b,o->vars+vector,0); StoreF(b,o->vars+vector+4,1); StoreF(b,o->vars+vector+8,-.25f);
          for (unsigned i=0;i<3;++i) StoreF(b,o->vars+vector+12+4*i,LoadF(b,matrix+48+4*i));
        }
      }
    }
#endif
    if (o->support.phase) return; // Retain the FIRST collapse input for private replay.
    auto& event=o->support.event;
    event={};
    for (unsigned i=0;i<3;++i) if (Load32(b,o->vars+offsets[i])) event.trigger|=1u<<i;
    if (event.trigger&2) {
      const auto target=Load32(b,o->vars+hit);
      const auto it=std::find(o->support.parts.begin(),o->support.parts.end(),target);
      if (it==o->support.parts.end()) { InvalidateSupport(b,o->actor); return; }
      event.part=uint8_t(it-o->support.parts.begin());
      for (unsigned i=0;i<6;++i) event.impulse[i]=LoadF(b,o->vars+vector+4*i);
    }
    return;
  }
  for (const auto offset:offsets) Store32(b,o->vars+offset,0);
  Store32(b,o->vars+hit,0);
  for (unsigned i=0;i<6;++i) StoreF(b,o->vars+vector+4*i,0);
  const auto* authority=state.replica.Find(o->kind,o->source);
  if (authority && authority->collapse.topology==o->support.topology && !o->support.phase && authority->value) {
    const auto& event=authority->collapse;
    for (unsigned i=0;i<3;++i) Store32(b,o->vars+offsets[i],bool(event.trigger&(1u<<i)));
    if (event.part<o->support.parts.size()) Store32(b,o->vars+hit,o->support.parts[event.part]);
    for (unsigned i=0;i<6;++i) StoreF(b,o->vars+vector+4*i,event.impulse[i]);
    if (event.trigger&4) Store32(b,o->vars+(tree ? 0x6E0 : 2072),0);
  }
  ctx.r3.u64=0;
}
void SupportNative(PPCContext& ctx,uint8_t* b,bool tree,unsigned phase) {
  const auto kind=tree ? ObjectKind::Tree : ObjectKind::Ode;
  auto* o=state.ready && Shared() ? Current(b,kind) : nullptr;
  auto native=tree ? (phase==0 ? __imp__sub_826E6390 : phase==1 ? __imp__sub_826E78F0 : phase==2 ? __imp__sub_826E7BF0 : __imp__sub_826D0770) :
      (phase==0 ? __imp__sub_826EA1A0 : phase==1 ? __imp__sub_826F0E10 : phase==2 ? __imp__sub_826F1E08 : __imp__sub_826F4948);
  if (!o && phase && GuestSupportLoading()) { ctx.r3.u64=0; return; }
  if (!o || !o->support.eligible) { native(ctx,b); return; }
  if (!SupportListValid(b,*o)) {
    InvalidateSupport(b,o->actor); native(ctx,b); return;
  }
  auto& s=o->support; s.tick=state.ai_tick;
  if (!phase) {
    native(ctx,b); s.initialized=true;
    const auto* terminal=state.host ? nullptr : state.replica.Find(kind,o->source);
    if (!tree && ctx.r3.u32==1 && terminal && terminal->value==2 && terminal->collapse.topology==s.topology &&
        objects::OdeCheckpointGone([&](uint32_t a){return Load32(b,a);},o->actor,o->vars,s.parts)) {
      // The native initializer already removed this checkpoint's geometry.
      // Do not replay Wait or apply a retained live pose that would unhide it.
      const bool first=s.phase!=2;
      s.phase=2; s.event=terminal->collapse;
      for (const auto part:s.parts) if (auto it=state.bodies.find(part); it!=state.bodies.end()) {
        it->second.retired=true; it->second.driven=it->second.saved_body=false;
      }
      if (first) {
        ++state.applied;
        if (Trace()) REXLOG_INFO("KK dev object collapse checkpoint endpoint: role=guest id={:016X} topology={:016X} parts={} native-removed=true",
            o->source,s.topology,s.parts.size());
#if defined(KK_DEV_TOOLS)
        state.observed_change=true;
#endif
      }
    }
#if defined(KK_DEV_TOOLS)
    if (Trace() && !tree && !state.host && s.phase!=2 && Track(b,o->ai,0x826EA1A0)) {
      static double next_trace=0;
      if (Now()>=next_trace) {
        next_trace=Now()+5;
        const auto ext=Ptr(b,o->actor,24,42);
        REXLOG_INFO("KK dev object support init waiting: id={:016X} result={} capability={}/{} flags={:08X} ext={:04X} parts={}",
            o->source,ctx.r3.u32,Load32(b,o->vars+4),Load32(b,o->vars+8),
            Load32(b,o->actor+4),ext ? Load16(b,ext+40) : 0,s.parts.size());
      }
    }
#endif
    return;
  }
  const auto* authority=state.host ? nullptr : state.replica.Find(kind,o->source);
  if (!state.host) {
    if (authority && authority->value==3) { native(ctx,b); return; }
    // Own scene readiness must never race an absent or still-waiting host.
    if (!s.topology || !authority || authority->collapse.topology!=s.topology || !authority->value) { ctx.r3.u64=0; return; }
    s.event=authority->collapse;
    if (phase==1 && s.phase==0) {
      native(ctx,b);
      // Wait can return before consuming the collapse (ODE's authored partner
      // handshake is one example). Keep retrying its own native Wait until the
      // track actually advances. A logged replay must mean native activation.
      if (!Track(b,o->ai,tree ? 0x826E7BF0 : 0x826F1E08)) return;
      s.phase=1;
      if (Trace()) REXLOG_INFO("KK dev object collapse replay: role=guest id={:016X} family={} topology={:016X} trigger={} part={}",
          o->source,uint8_t(kind),s.topology,s.event.trigger,s.event.part);
    } else if (authority->value==2 && s.phase<2) {
      if (!tree && phase==2) {
        // Calling End's body while still scheduled as Active leaves the guest
        // permanently on Active. Native Active uses 826F9F30 with this loaded
        // End program; retain that scheduler transition before its side effects.
        const auto end=objects::OdeEndProgram([&](uint32_t a){return Load32(b,a);},
            [&](uint32_t a){return Load16(b,a);},o->ai,o->actor);
        if (!end) { ctx.r3.u64=0; return; }
        ctx.r3.u64=end; __imp__sub_826F9F30(ctx,b);
        if (!Track(b,o->ai,0x826F4948)) { ctx.r3.u64=0; return; }
      }
      (tree ? __imp__sub_826D0770 : __imp__sub_826F4948)(ctx,b); s.phase=2;
      if (Trace()) REXLOG_INFO("KK dev object collapse endpoint: role=guest id={:016X} family={} topology={:016X}",o->source,uint8_t(kind),s.topology);
    } else ctx.r3.u64=0;
    // Local ODE motion may play the fall, but only the host's active manager
    // may retire pieces, apply environmental hits or start subsequent fires.
    // Otherwise a random guest fall could destroy geometry before its final
    // authoritative pose arrives, and a transform cannot rebuild that mesh.
    return;
  }
  const auto before=s.phase;
  native(ctx,b);
  if (phase==1 && Track(b,o->ai,tree ? 0x826E7BF0 : 0x826F1E08)) {
    s.phase=1; if (!s.event.trigger) s.event.trigger=1; // Native scene readiness, not an incoming strike.
  }
  if (phase==2 && !s.phase) { s.phase=1; s.event.trigger=1; }
  if (phase==3) { s.phase=2; if (!s.event.trigger) s.event.trigger=1; }
  if (Trace() && s.phase!=before) REXLOG_INFO("KK dev object collapse native: role=host id={:016X} family={} topology={:016X} phase={} trigger={} part={}",
      o->source,uint8_t(kind),s.topology,s.phase,s.event.trigger,s.event.part);
  if (s.topology) Observe(b,*o);
}
bool ReadBody(const uint8_t* b,const State::Body& p,SupportBodyState& s) {
  if (!BodyValid(b,p)) return false;
  s.topology=p.topology; s.flags=Load32(b,p.actor+4)&0x00620003;
  for (unsigned i=0;i<3;++i) {
    s.pose[i]=LoadF(b,p.matrix+48+4*i);
    s.pose[3+i]=-LoadF(b,p.matrix+16+4*i); s.pose[6+i]=LoadF(b,p.matrix+32+4*i);
  }
  ObjectState check{state.generation,state.level,1,p.source,ObjectKind::SupportBody,1}; check.body=s;
  return ValidObjectState(check);
}
bool BodyQuiet(const uint8_t* b,const State::Body& p) {
  if (!(b[p.physics+14]&4) || !p.body || (Load32(b,p.body+0x18)&4)) return true;
  for (const auto offset:{0xE8u,0xF8u}) {
    const float speed=std::hypot(LoadF(b,p.body+offset),LoadF(b,p.body+offset+4),LoadF(b,p.body+offset+8));
    if (!std::isfinite(speed) || speed>.08f) return false;
  }
  return true;
}
bool BodyActivated(const State::Body& p,bool guest) {
  bool active=false;
  for (const auto actor:p.roots) {
    const auto it=state.bindings.find(actor);
    if (it==state.bindings.end() || !it->second.support.eligible || !it->second.support.topology) return false;
    const auto& o=it->second;
    if (guest) {
      const auto* s=state.replica.Find(o.kind,o.source);
      active|=s && s->collapse.topology==o.support.topology && (s->value==1 || s->value==2) && o.support.phase;
    } else active|=o.support.phase>0;
  }
  return active;
}
void PublishBodies(const uint8_t* b) {
  const auto now=Now();
  for (auto& [actor,p]:state.bodies) {
    if (p.retired) continue;
    if (!BodyActivated(p,false)) continue;
    if (!RefreshBody(b,p)) {
      for (const auto root:p.roots) InvalidateSupport(b,root);
      continue;
    }
    SupportBodyState sample;
    if (!ReadBody(b,p,sample)) { for (const auto root:p.roots) InvalidateSupport(b,root); continue; }
    const auto differs=[](const SupportBodyState& a,const SupportBodyState& c) {
      if (a.flags!=c.flags || std::hypot(a.pose[0]-c.pose[0],a.pose[1]-c.pose[1],a.pose[2]-c.pose[2])>.02f) return true;
      for (unsigned n=3;n<9;++n) if (std::abs(a.pose[n]-c.pose[n])>.01f) return true;
      return false;
    };
    const bool removed=(sample.flags&0x00600000)==0x00600000;
    if (!removed && !BodyQuiet(b,p)) { p.quiet_since=0; p.candidate=sample; }
    else if (!p.quiet_since || differs(sample,p.candidate)) { p.quiet_since=now; p.candidate=sample; }
    const uint32_t phase=removed || (p.quiet_since && now-p.quiet_since>=1) ? 1u : 2u;
    // Mode1/3 pieces can stay awake indefinitely. Retain a bounded live pose
    // too, otherwise a private loader has only the intact home transform and
    // independent guest physics can diverge throughout the visible collapse.
    if ((phase==2 && now<p.next_pose) || (p.sent && p.final.value==phase && !differs(p.final.body,sample))) continue;
    if (state.number==UINT32_MAX || (!state.outcomes.contains(p.source) && state.outcomes.size()>=objects::kCensus)) continue;
    ObjectState s{state.generation,state.level,++state.number,p.source,ObjectKind::SupportBody,phase}; s.body=sample;
    state.pending[s.source]=state.outcomes[s.source]=s; p.sent=true; p.final=s;
    p.next_pose=now+.1; // At most ten changed poses per second per canonical body.
    p.retired=removed; // Retain values only before native sweep frees the component.
    if (Trace()) REXLOG_INFO("KK dev object body {}: role=host id={:016X} topology={:016X} aliases={} number={} pos=({:.3f},{:.3f},{:.3f}) flags={:08X} forward=({:.4f},{:.4f},{:.4f}) up=({:.4f},{:.4f},{:.4f})",
        phase==1 ? "settled" : "moving",p.source,p.topology,p.roots.size(),s.number,sample.pose[0],sample.pose[1],sample.pose[2],sample.flags,
        sample.pose[3],sample.pose[4],sample.pose[5],sample.pose[6],sample.pose[7],sample.pose[8]);
  }
}
void ApplyBodies(PPCContext& ctx,uint8_t* b) {
  for (auto& [actor,p]:state.bodies) {
    if (p.retired) continue;
    if (!RefreshBody(b,p) || !BodyActivated(p,true)) continue;
    const auto* s=state.replica.Find(ObjectKind::SupportBody,p.source);
    if (!s || s->body.topology!=p.topology) continue;
    if (p.body && !p.saved_body) { p.saved_body=true; p.saved_disabled=(Load32(b,p.body+0x18)&4)!=0; }
    bool same=p.driven && (Load32(b,actor+4)&0x00620003)==s->body.flags;
    for (unsigned i=0;i<3 && same;++i) same=std::abs(LoadF(b,p.matrix+48+4*i)-s->body.pose[i])<.0001f &&
        std::abs(-LoadF(b,p.matrix+16+4*i)-s->body.pose[3+i])<.0001f && std::abs(LoadF(b,p.matrix+32+4*i)-s->body.pose[6+i])<.0001f;
    if (same && (!p.body || (Load32(b,p.body+0x18)&4))) { p.final=*s; continue; }
    const auto sp=ctx.r1.u32; if ((sp&15) || sp<0x40014000 || sp>=0xA0000000) continue;
    const PPCContext saved=ctx; uint8_t scratch[16384]; const auto start=sp-sizeof(scratch);
    std::memcpy(scratch,b+start,sizeof(scratch)); ctx.r1.u64=start+12288;
    std::memset(b+ctx.r1.u32,0,4096); Store32(b,ctx.r1.u32,sp);
    const auto out=ctx.r1.u32+256;
    for (unsigned n=0;n<9;++n) StoreF(b,out+16*(n/3)+4*(n%3),s->body.pose[n]);
    ctx.r3.u64=actor; ctx.r4.u64=out; __imp__sub_827117E0(ctx,b);
    ctx.r3.u64=actor; ctx.r4.u64=out+16; ctx.r5.u64=out+32; __imp__sub_82711328(ctx,b);
    // Host motion and settled outcomes own only these canonical guest bodies.
    // The native transform helper moves
    // their own ODE geom and broadphase bounds through the authored rest matrix.
    ctx.r3.u64=actor; __imp__sub_82710130(ctx,b);
    ctx.r3.u64=actor; ctx.r4.u64=s->body.flags&0x00620000; ctx.r5.u64=(~s->body.flags)&0x00620000;
    __imp__sub_8270FB68(ctx,b);
    Store32(b,actor+4,(Load32(b,actor+4)&~3u)|(s->body.flags&3));
    if ((s->body.flags&0x00600000)==0x00600000) for (const auto root:p.roots) {
      const auto it=state.bindings.find(root); if (it==state.bindings.end()) continue;
      const auto& o=it->second;
      for (unsigned n=0;n<o.support.parts.size();++n) if (o.support.parts[n]==actor)
        Store32(b,o.vars+(o.kind==ObjectKind::Tree ? 0x4A8 : 1208)+4*n,5);
    }
    if (p.body) { ctx.r3.u64=p.body; __imp__sub_82744978(ctx,b); }
    std::memcpy(b+start,scratch,sizeof(scratch)); ctx=saved;
    if (!p.driven || p.final.number!=s->number) {
      if (Trace()) REXLOG_INFO("KK dev object body applied: role=guest id={:016X} topology={:016X} aliases={} number={} phase={} pos=({:.3f},{:.3f},{:.3f}) flags={:08X} forward=({:.4f},{:.4f},{:.4f}) up=({:.4f},{:.4f},{:.4f})",
          p.source,p.topology,p.roots.size(),s->number,s->value,LoadF(b,p.matrix+48),LoadF(b,p.matrix+52),LoadF(b,p.matrix+56),Load32(b,actor+4)&0x00620003,
          -LoadF(b,p.matrix+16),-LoadF(b,p.matrix+20),-LoadF(b,p.matrix+24),LoadF(b,p.matrix+32),LoadF(b,p.matrix+36),LoadF(b,p.matrix+40));
      ++state.applied;
    }
    p.driven=true; p.final=*s;
    if ((s->body.flags&0x00600000)==0x00600000) { p.retired=true; p.driven=p.saved_body=false; }
#if defined(KK_DEV_TOOLS)
    state.observed_change=true;
#endif
  }
}
void ReleaseBodies(PPCContext& ctx,uint8_t* b,bool all=true) {
  for (auto& [actor,p]:state.bodies) {
    if (!p.driven || (!all && BodyActivated(p,true))) continue;
    if (!p.retired && p.saved_body && !p.saved_disabled && BodyValid(b,p) && Load32(b,p.physics+0x74)==p.body) {
      const PPCContext saved=ctx; ctx.r3.u64=p.body; __imp__sub_82744988(ctx,b); ctx=saved;
    }
    p.driven=p.saved_body=false;
  }
}
void ReplaySupports(PPCContext& ctx,uint8_t* b) {
  FreezeSupport(b);
  if (!state.host) ReleaseBodies(ctx,b,false);
#if defined(KK_DEV_TOOLS)
  static const double fixture_at=[] {
    const auto value=std::getenv("KK_DEV_OBJECT_COLLAPSE"); if (!value || !*value) return -1.0;
    char* end=nullptr; const auto t=std::strtod(value,&end);
    return end!=value && !*end && std::isfinite(t) && t>=0 && t<=300 ? t : -1.0;
  }();
  static const unsigned fixture_mode=[] {
    const auto value=std::getenv("KK_DEV_OBJECT_COLLAPSE_MODE"); return value && *value=='3' ? 3u : value && *value=='1' ? 1u : 2u;
  }();
  static uint64_t selected=0;
  if (state.host && !selected && fixture_at>=0 && kk::DevSecondsSinceMenu()>=fixture_at) {
    Binding* choice=nullptr;
    for (auto& [actor,o]:state.bindings) if (objects::SupportRoot(o.kind) && o.support.topology &&
        (!DevSupportSource() || o.source==DevSupportSource()) &&
        Load32(b,o.vars+(o.kind==ObjectKind::Tree ? 0x52C : 0))==fixture_mode && Valid(b,o) &&
        (!choice || o.source<choice->source)) choice=&o;
    if (choice) {
      choice->support.fixture=true; selected=choice->source;
      REXLOG_INFO("KK dev object support fixture: role=host id={:016X} mode={} native-trigger-fixture=true",choice->source,fixture_mode);
    }
  }
  // C105 guest break fixture: the guest sends its OWN part hit request, the same
  // message ObjectsEnqueued sends for a real guest strike. The host applies it
  // as a native hit and owns the collapse; the guest never collapses locally.
  static bool guest_sent=false;
  if (!state.host && !guest_sent && fixture_at>=0 && DevSupportSource() && kk::DevSecondsSinceMenu()>=fixture_at) {
    auto* o=Match(DevSupportSource(),ObjectKind::Ode); if (!o) o=Match(DevSupportSource(),ObjectKind::Tree);
    const auto part=DevSupportPart();
    if (o && Valid(b,*o) && o->support.eligible && o->support.topology && part<o->support.parts.size()) {
      unsigned flags=0x00400400,magnitude=11; char tail=0;
      if (const auto value=std::getenv("KK_DEV_OBJECT_COLLAPSE_HIT"); value && *value &&
          (std::sscanf(value,"%x,%u%c",&flags,&magnitude,&tail)!=2 || (flags&~kObjectHitFlags) || magnitude>100000)) { guest_sent=true; return; }
      guest_sent=Request(*o,flags,float(magnitude),.5f,uint8_t(part));
      REXLOG_INFO("KK dev object support guest hit fixture: role=guest id={:016X} part={} flags={:08X} magnitude={} sent={}",
          o->source,part,flags,magnitude,guest_sent);
    }
  }
#endif
  for (auto& [actor,o]:state.bindings) {
    if (!objects::SupportRoot(o.kind) || !o.support.topology || !Valid(b,o) || Match(o.source,o.kind)!=&o) continue;
    if (state.host) {
      while (!o.pending_hits.empty()) {
        const auto hit=o.pending_hits.front();
        if (Now()>hit.until) { o.pending_hits.pop_front(); ++state.rejected; continue; }
        if (hit.raw.collapse.part>=o.support.parts.size() || !NativeHit(ctx,b,o,o.actor,o.ai,hit.raw.value,uint32_t(hit.raw.position),hit.raw.seconds,o.support.parts[hit.raw.collapse.part])) break;
        o.pending_hits.pop_front(); ++state.applied; o.support.intent_until=Now()+2;
        if (Trace()) REXLOG_INFO("KK dev object support partner hit applied: role=host id={:016X} part={} flags={:08X} magnitude={}",
            o.source,unsigned(hit.raw.collapse.part),hit.raw.value,uint32_t(hit.raw.position));
      }
      if (!o.support.fixture && !o.support.phase && Now()>o.support.intent_until) continue;
    }
    const auto* s=state.replica.Find(o.kind,o.source);
    if (!state.host && s && s->collapse.topology==o.support.topology && (s->value==1 || s->value==2)) {
      // Active's tracked-member notification is semantic progression, not a
      // guest physics observation. Replay it even after the native End track
      // has cleared itself, including on a private native reload.
      const auto ext=Ptr(b,o.actor,24,42);
      Store32(b,o.vars+(o.kind==ObjectKind::Tree ? 0x554 : 0x68C),s->position==1);
      if (ext) Store16(b,ext+40,(Load16(b,ext+40)&~0x100u)|(s->position==1 ? 0x100u : 0));
    }
    if ((!state.host && (!s || !s->value || s->collapse.topology!=o.support.topology)) ||
        o.support.tick==state.ai_tick || o.support.phase==2) continue;
    // Dormant and privately reloaded roots replay through THEIR native state
    // block and dispatcher. Never inherit another actor's current track index.
    const auto first=Load16(b,o.ai+80),last=Load16(b,o.ai+82);
    if (first>last || last>=5) continue;
    for (unsigned slot=first;slot<=last;++slot) {
      const auto block=Ptr(b,o.ai,(slot+6)*4,24); if (!block) continue;
      const auto fn=Load32(b,block+20);
      if (fn!=(o.kind==ObjectKind::Tree ? 0x826E6390u : 0x826EA1A0u) &&
          fn!=(o.kind==ObjectKind::Tree ? 0x826E78F0u : 0x826F0E10u) &&
          fn!=(o.kind==ObjectKind::Tree ? 0x826E7BF0u : 0x826F1E08u) &&
          fn!=(o.kind==ObjectKind::Tree ? 0x826D0770u : 0x826F4948u)) continue;
      const auto sp=ctx.r1.u32; if ((sp&15) || sp<0x40014000 || sp>=0xA0000000) break;
      const PPCContext saved=ctx; uint8_t scratch[16384]; const auto start=sp-sizeof(scratch);
      std::memcpy(scratch,b+start,sizeof(scratch)); ctx.r1.u64=start+12288;
      std::memset(b+ctx.r1.u32,0,4096); Store32(b,ctx.r1.u32,sp);
      constexpr uint32_t globals[]{0x82CCBF58,0x82CCBF5C,0x82CCBF60,0x82CCBF64,0x82CCBF68,0x82CCBF6C,0x82CCBF70};
      uint32_t values[7]; for (unsigned i=0;i<7;++i) values[i]=Load32(b,globals[i]);
      Store32(b,globals[0],slot); Store32(b,globals[3],o.ai); Store32(b,globals[4],o.ai);
      Store32(b,globals[5],o.actor); Store32(b,globals[6],o.actor);
      ctx.r3.u64=block; ctx.r4.u64=o.ai+(slot+1)*4; ctx.r5.u64=0; __imp__sub_8271CCB8(ctx,b);
      for (unsigned i=0;i<7;++i) Store32(b,globals[i],values[i]);
      std::memcpy(b+start,scratch,sizeof(scratch)); ctx=saved;
      break;
    }
  }
}
#if defined(KK_DEV_TOOLS)
// A single explicit native physical push, for modes whose authored trigger
// only enables dynamics. This is developer staging, never a gameplay fallback.
void SupportPushFixture(PPCContext& ctx,uint8_t* b) {
  static const double at=[] {
    const auto value=std::getenv("KK_DEV_OBJECT_PUSH"); if (!value || !*value) return -1.0;
    char* end=nullptr; const auto t=std::strtod(value,&end);
    return end!=value && !*end && std::isfinite(t) && t>=0 && t<=300 ? t : -1.0;
  }();
  static bool done=false;
  if (done || !state.host || at<0 || kk::DevSecondsSinceMenu()<at || !DevSupportSource()) return;
  auto* o=Match(DevSupportSource(),ObjectKind::Ode); if (!o) o=Match(DevSupportSource(),ObjectKind::Tree);
  const auto part=DevSupportPart();
  if (!o || !Valid(b,*o) || !o->support.eligible || !o->support.topology || !o->support.phase || part>=o->support.parts.size()) return;
  const auto actor=o->support.parts[part],matrix=Ptr(b,actor,12,64);
  const auto body=state.bodies.find(actor);
  if (!matrix || body==state.bodies.end() || !RefreshBody(b,body->second) || !body->second.body) return;
  std::array<float,3> force{20000,0,0}; char tail=0;
  if (const auto value=std::getenv("KK_DEV_OBJECT_PUSH_FORCE"); value && *value)
    if (std::sscanf(value,"%f,%f,%f%c",&force[0],&force[1],&force[2],&tail)!=3) { done=true; return; }
  const auto length=std::hypot(force[0],force[1],force[2]);
  if (!std::isfinite(length) || length<.01f || length>1000000) { done=true; return; }
  const auto sp=ctx.r1.u32; if ((sp&15) || sp<0x40014000 || sp>=0xA0000000) return;
  const PPCContext saved=ctx; uint8_t scratch[16384]; const auto start=sp-sizeof(scratch);
  std::memcpy(scratch,b+start,sizeof(scratch)); ctx.r1.u64=start+12288;
  std::memset(b+ctx.r1.u32,0,4096); Store32(b,ctx.r1.u32,sp);
  const auto vectors=ctx.r1.u32+256;
  for (unsigned i=0;i<3;++i) {
    StoreF(b,vectors+4*i,force[i]); StoreF(b,vectors+16+4*i,LoadF(b,matrix+48+4*i));
  }
  ctx.r3.u64=actor; ctx.r4.u64=vectors; ctx.r5.u64=vectors+16; __imp__sub_82408648(ctx,b);
  std::memcpy(b+start,scratch,sizeof(scratch)); ctx=saved; done=true;
  REXLOG_INFO("KK dev object physical push fixture: role=host id={:016X} part={} force=({:.3f},{:.3f},{:.3f}) native-82408648=true authored-parameters-unchanged=true",
      o->source,part,force[0],force[1],force[2]);
}
#endif
void SupportPhysics(PPCContext& ctx,uint8_t* b) {
  const auto world=ctx.r3.u32;
  const bool driven=state.ready && !state.host && Shared() && world==state.world;
  if (driven) ApplyBodies(ctx,b);
  __imp__sub_82860678(ctx,b);
  if (driven) ApplyBodies(ctx,b); // A local collision may wake one owned body.
}
