// Included beside the C104 native probes. All memory access is on the game thread.
void_rescue::History void_history;
struct VoidScope {
  uint64_t epoch=0;
  uint32_t generation=0,hero=0,vars=0,checkpoint=0;
  bool operator==(const VoidScope&) const = default;
} void_scope;
bool void_fallback_logged=false;

// Extra queries must not replace the native ray result or leave pointers into
// our temporary stack in the shared query record.
struct VoidQueryScope {
  uint8_t* b; uint32_t world;
  std::array<uint8_t,0x44> query,deferred;
  std::array<uint8_t,0x40> hit;
  uint32_t counters[5],contact;
  static constexpr uint32_t addresses[]={0x82CCABF4,0x82CCAC00,0x82CCAC04,0x82CCAC08,0x82CCAC0C};
  VoidQueryScope(uint8_t* base,uint32_t w):b(base),world(w) {
    std::memcpy(query.data(),b+0x82977230,query.size());
    std::memcpy(deferred.data(),b+world+0x294,deferred.size());
    std::memcpy(hit.data(),b+world+0x16268,hit.size());
    for (unsigned i=0;i<5;++i) counters[i]=Load32(b,addresses[i]);
    contact=Load32(b,0x828D2A74);
  }
  ~VoidQueryScope() {
    std::memcpy(b+0x82977230,query.data(),query.size());
    std::memcpy(b+world+0x294,deferred.data(),deferred.size());
    std::memcpy(b+world+0x16268,hit.data(),hit.size());
    for (unsigned i=0;i<5;++i) Store32(b,addresses[i],counters[i]);
    Store32(b,0x828D2A74,contact);
  }
};
bool VoidStaticFloor(const uint8_t* b,uint32_t floor) {
  // Terrain mesh only. Refuse collision items, physics bodies, AI objects,
  // destroyed or inactive surfaces, even if a raft or breakable is at rest.
  if (!PoseAddress(floor,32) || !(Load32(b,floor)&0x4000) ||
      (Load32(b,floor)&0x10000000) || (Load32(b,floor+4)&0x00600000) ||
      !Ptr(b,floor,12,64)) return false;
  const auto ext=Load32(b,floor+24);
  return !ext || (PoseAddress(ext,8) && !Load32(b,ext+4));
}
uint32_t VoidVelocity(const uint8_t* b,uint32_t hero) {
  const auto motion=Ptr(b,Ptr(b,hero,20,8),4,0x28);
  return Ptr(b,Ptr(b,motion,0x24,4),0,12);
}
std::array<uint32_t,16> VoidTransform(const uint8_t* b,uint32_t floor) {
  std::array<uint32_t,16> out{};
  const auto m=Ptr(b,floor,12,64);
  if (m) for (unsigned i=0;i<16;++i) out[i]=Load32(b,m+4*i);
  return out;
}
bool VoidCheckSpot(PPCContext& ctx,uint8_t* b,Scratch& scratch,uint32_t world,
    const HumanStatus& h,uint32_t body,void_rescue::Spot& spot,bool recheck) {
  float normal=0;
  const auto& p=spot.position;
  const auto floor=NativeFloor(ctx,b,scratch,world,h.hero,body,p.x,p.y,p.z+.5f,1.f,normal);
  const auto surface=Load32(b,world+0x16268);
  if (!floor || !VoidStaticFloor(b,surface) || std::abs(floor->z-(p.z-.1f))>.25f) return false;
  if (recheck && (surface!=spot.floor || VoidTransform(b,surface)!=spot.transform)) return false;
  // A short sight ray also keeps the landing capsule away from walls. The
  // nav reference is the cell Jack actually stood on, retained with the spot.
  const ShotVector anchor{p.x+.5f*std::cos(spot.facing),p.y+.5f*std::sin(spot.facing),floor->z};
  const auto probe=ProbeSpot(ctx,b,scratch,world,h.hero,body,{p.x,p.y,floor->z},anchor);
  if (revive::Judge(probe,floor->z,spot.nav)!=revive::Verdict::Ok) return false;
  spot.floor=surface; spot.transform=VoidTransform(b,surface);
  spot.position.z=floor->z+.1f;
  return true;
}
#if defined(KK_DEV_TOOLS)
void VoidDevFall(PPCContext& ctx,uint8_t* b,const HumanStatus& h,const ShotVector& at,double now) {
  static const double due=[] { const auto s=std::getenv("KK_DEV_VOID_FALL"); return s ? std::atof(s) : -1.; }();
  static const unsigned total=[] { const auto s=std::getenv("KK_DEV_VOID_COUNT"); return s ? unsigned(std::clamp(std::atoi(s),1,5)) : 1u; }();
  static unsigned count=0; static double next=0;
  if (due<0 || count>=total || kk::DevSecondsSinceMenu()<due || now<next) return;
  // Move only the local Jack beyond the authored world. Do not set the void
  // flag or health: the native floor query and gravity must confirm the fall.
  const ShotVector outside{at.x+2000,at.y+2000,at.z-.5f};
  NativeTeleport(ctx,b,h.hero,outside,0);
  ResetGround(b,h.vars,at,false);
  if (std::getenv("KK_DEV_VOID_NO_SPOTS")) void_history.ClearSpots();
  ++count; next=now+4;
  REXLOG_INFO("COOP VOID dev fall role={} level={} count={} from=({:.3f},{:.3f},{:.3f}) no-spots={}",
      Get().link.role==Role::Host ? "host":"guest",Get().identity.level,count,at.x,at.y,at.z,
      std::getenv("KK_DEV_VOID_NO_SPOTS")!=nullptr);
}
#endif
void VoidBeforeGround(PPCContext& ctx,uint8_t* b) {
  const auto view=SessionFlowSnapshot();
  const auto link=EnemyLinkSnapshot();
  const bool enabled=REXCVAR_GET(kk_coop_enabled) && (link.SessionConnected() || (view.host && view.reconnecting)) && view.active &&
      view.phase==FlowPhase::Playing && view.policy==ChapterPolicy::Together;
  if (!enabled) { void_history.ClearSpots(); void_scope={}; return; }
  const auto h=ReadHumanStatus(b);
  if (!h || Load32(b,0x82CCBF6C)!=h->hero || !JackReady(b) || !PoseAddress(h->vars,0x3CC0) ||
      !Scratch::CanUse(ctx) || !ControllerSafe(b,h->hero)) return;
  const auto world=Load32(b,0x828E8388),matrix=Ptr(b,h->hero,12,64);
  if (!PoseAddress(world,0x16300) || !matrix) return;
  const VoidScope scope{link.epoch,view.world.generation,h->hero,h->vars,view.checkpoint};
  if (scope!=void_scope) { void_history.ClearSpots(); void_scope=scope; void_fallback_logged=false; }
  const auto at=Vector(b,matrix+48);
  const auto now=Now();
  void_rescue::Gate gate;
  gate.coop=enabled; gate.jack=true; gate.playing=true;
  gate.cinematic=observer::InCutscene(b) || observer::Active();
  gate.dead=h->condition==PlayerCondition::Dead;
  const auto bank=Load32(b,0x82CC87F8);
  gate.scripted=Load32(b,h->vars+0x337C)!=0 || !PoseAddress(bank,0x1D8C) || Load32(b,bank+0x1D88)!=0;
  gate.native_void=Load32(b,h->vars+kVoid)!=0;
  gate.immune=Load32(b,h->vars+0x3CBC)!=0;
  gate.fall=LoadF(b,h->vars+kLastGround+8)-at.z;
  if (!void_rescue::Eligible(gate)) { if (gate.dead) void_history.ClearSpots(); return; }
  if (!gate.native_void && !void_history.Due(now)) {
#if defined(KK_DEV_TOOLS)
    VoidDevFall(ctx,b,*h,at,now);
#endif
    return;
  }
  VoidQueryScope query(b,world);
  Scratch scratch(ctx,b);
  ctx.r3.u64=h->hero; ctx.r4.u64=world; ctx.r5.u64=0x40;
  __imp__sub_82808310(ctx,b); gate.grounded=ctx.r3.u32!=0;
  const auto body=kk::PartnerBodyCombatActor(b);
  if (void_rescue::Confirmed(gate)) {
    const auto velocity=VoidVelocity(b,h->hero);
    const auto spot=velocity ? void_history.Find(gate,now,[&](const void_rescue::Spot& stored) {
      auto test=stored; return VoidCheckSpot(ctx,b,scratch,world,*h,body,test,true);
    }) : std::nullopt;
    if (spot) {
      NativeTeleport(ctx,b,h->hero,spot->position,spot->facing);
      for (unsigned i=0;i<3;++i) StoreF(b,velocity+4*i,0);
      ResetGround(b,h->vars,spot->position,true);
      StoreF(b,h->vars+kAirTime,0); StoreF(b,h->vars+13268,0);
      void_history.Rescued(now); void_guard={}; void_fallback_logged=false;
      REXLOG_INFO("COOP VOID rescue role={} level={} from=({:.3f},{:.3f},{:.3f}) to=({:.3f},{:.3f},{:.3f}) age={:.3f} fall={:.3f} health={:.3f}",
          link.role==Role::Host ? "host":"guest",view.world.level,at.x,at.y,at.z,
          spot->position.x,spot->position.y,spot->position.z,now-spot->time,gate.fall,h->value);
    } else if (!void_fallback_logged) {
      void_fallback_logged=true;
      REXLOG_INFO("COOP VOID native path role={} level={} fall={:.3f} reason={}",link.role==Role::Host ? "host":"guest",
          view.world.level,gate.fall,void_history.Allowed(now) ? "no checked spot":"rescue limit");
    }
    return;
  }
  if (gate.grounded && !gate.native_void && void_history.Due(now)) {
    void_history.Sampled(now); void_fallback_logged=false;
    void_rescue::Spot spot; spot.position=at;
    spot.facing=std::atan2(-LoadF(b,matrix+20),-LoadF(b,matrix+16));
    spot.nav=NativeNav(ctx,b,scratch,world,at);
    const bool accepted=VoidCheckSpot(ctx,b,scratch,world,*h,body,spot,false);
    if (accepted) void_history.Remember(spot,now);
#if defined(KK_DEV_TOOLS)
    static unsigned samples=0;
    if (std::getenv("KK_DEV_VOID_FALL") && samples++<8)
      REXLOG_INFO("COOP VOID sample role={} level={} accepted={} at=({:.3f},{:.3f},{:.3f}) floor={:08X} nav={} velocity={:08X}",
          link.role==Role::Host ? "host":"guest",view.world.level,accepted,at.x,at.y,at.z,spot.floor,int(spot.nav),VoidVelocity(b,h->hero));
#endif
  }
#if defined(KK_DEV_TOOLS)
  VoidDevFall(ctx,b,*h,at,now);
#endif
}
