#pragma once
#include "coop_protocol.h"

#if defined(KK_DEV_TOOLS)
namespace kk::coop {
// Synthetic values only. Shared by the optional SDK probe and SDK-free tests,
// never by a live session. Exercise maximum variable payloads as well as all IDs.
inline Message SteamProbeMessage(MessageType type) {
  Message m; m.type=type; m.sequence=uint32_t(type); m.epoch=12345;
  m.hello={kProtocolVersion,std::string(128,'v'),std::string(128,'b'),std::string(128,'m')};
  m.display_name=std::string(kMaxDisplayNameBytes,'P');
  m.reason=RejectReason::Protocol;
  m.pose={1,101,1,2,3,0,0};
  const EnemyWorld world{101,1,1};
  const EnemyState enemy{{123,0,EnemyFamily::Raptor},1,2,3,0,50};
  m.enemies={world,1,{}};
  for (size_t i=0;i<kMaxEnemyBatch;++i) {
    auto e=enemy; e.id.source+=i; m.enemies.enemies.push_back(e);
  }
  m.enemy_event={world,EnemyEventKind::Spawned,enemy};
  m.shot.world=world; m.shot.number=1; m.shot.weapon=3; m.shot.time=1; m.shot.direction={1,0,0};
  for (size_t i=0;i<kMaxShotHits;++i) m.shot.hits.push_back({{123+i,0,EnemyFamily::Raptor},{1,2,3},0});
  m.cheats={0x125}; m.inventory={world,1,2,{0,999,0,0}};
  m.start.chapter=1; m.start.level=101; m.start.start=1;
  m.save_chunk={0,std::vector<uint8_t>(kSaveChunkBytes,0xa5)};
  m.flow={1,101,1,0};
  m.damage={world,11,1,enemy.id,16388,6,{1,0,0},1};
  m.player_status={world,11,2,1,PlayerCondition::Hurt};
  m.ownership={world,OwnershipPhase::Prepare,1,EnemyOwner::Guest,enemy};
  m.life={m.flow,11,2,1};
  m.camera.generation=1; m.camera.level=101; m.camera.cut=1; m.camera.time=1;
  m.story={1,101,1,123,456,1,kStoryRecords,StoryPhase::Armed};
  m.digest.generation=1; m.digest.level=101; m.digest.sweep=1;
  m.digest.enemy_world={101,0,1};
  m.digest.script_count=kDigestScriptsPerPage; m.digest.enemy_count=kDigestEnemiesPerPage;
  for (size_t i=0;i<kDigestScriptsPerPage;++i) m.digest.scripts.push_back({123+i,456,1,3,5});
  for (size_t i=0;i<kDigestEnemiesPerPage;++i) m.digest.enemies.push_back({{123+i,0,EnemyFamily::Raptor},EnemyOwner::Host,0,50,0});
  return m;
}
}  // namespace kk::coop
#endif
