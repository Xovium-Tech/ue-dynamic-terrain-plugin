#include "PX4TerrainStartupTestMode.h"
#include "PX4Terrain.h"
#include "PX4Plane.h"
#include "EngineUtils.h"
#include "HAL/PlatformMisc.h"
#include "HAL/PlatformTime.h"
#include "Misc/CommandLine.h"
#include "Misc/Parse.h"

void APX4TerrainStartupTestMode::BeginPlay(){
 bExpectTimeout=FParse::Param(FCommandLine::Get(),TEXT("PX4TerrainStartupExpectTimeout"));
 Started=FPlatformTime::Seconds();Super::BeginPlay();BeginPlaySeconds=FPlatformTime::Seconds()-Started;LastReport=Started;
 UE_LOG(LogTemp,Display,TEXT("TERRAIN_STARTUP begin_play_seconds=%.6f"),BeginPlaySeconds);
}
void APX4TerrainStartupTestMode::Finish(bool Success,const TCHAR* Reason){
 bFinished=true;
 UE_LOG(LogTemp,Display,TEXT("TERRAIN_STARTUP complete passed=%d waiting_ticks=%llu elapsed=%.6f begin_play_seconds=%.6f reason=%s"),
  int(Success),WaitingTicks,FPlatformTime::Seconds()-Started,BeginPlaySeconds,Reason);
 FPlatformMisc::RequestExitWithStatus(false,Success?0:4);
}
void APX4TerrainStartupTestMode::Tick(float DeltaSeconds){
 if(bFinished)return;
 APX4Terrain* Terrain=nullptr;for(TActorIterator<APX4Terrain> It(GetWorld());It;++It){Terrain=*It;break;}
 FHitResult Hit;const bool Ground=Terrain&&Terrain->TraceGround(FVector::ZeroVector,Hit);
 int32 Before=0;for(TActorIterator<APX4Plane> It(GetWorld());It;++It)++Before;
 if(!Ground){++WaitingTicks;if(Before){Finish(false,TEXT("vehicle existed before terrain collision"));return;}}
 if(FPlatformTime::Seconds()-LastReport>=5){
  LastReport=FPlatformTime::Seconds();
  UE_LOG(LogTemp,Display,TEXT("TERRAIN_STARTUP waiting elapsed=%.6f ticks=%llu vehicles=%d ground=%d"),LastReport-Started,WaitingTicks,Before,int(Ground));
 }
 Super::Tick(DeltaSeconds);
 int32 After=0;for(TActorIterator<APX4Plane> It(GetWorld());It;++It)++After;
 if(After){
  Finish(!bExpectTimeout&&Ground&&Before==0&&After==1&&WaitingTicks>100&&BeginPlaySeconds<2&&FPlatformTime::Seconds()-Started>4,
   TEXT("real production vehicle spawned only after ground; ticks continued during delayed downloads"));
 }else if(FPlatformTime::Seconds()-Started>(bExpectTimeout?130:45)){Finish(false,TEXT("vehicle did not spawn or production deadline did not fire"));}
}
