#include "TerrainDemoGameMode.h"
#include "DynamicTerrainActor.h"
#include "Camera/CameraActor.h"
#include "Components/DirectionalLightComponent.h"
#include "UnrealClient.h"
#include "Engine/DirectionalLight.h"
#include "Engine/World.h"
#include "GameFramework/PlayerController.h"
#include "HAL/PlatformMisc.h"
#include "HAL/PlatformMemory.h"
#include "Misc/CommandLine.h"
#include "Misc/Parse.h"
ATerrainDemoGameMode::ATerrainDemoGameMode(){PrimaryActorTick.bCanEverTick=true;DefaultPawnClass=nullptr;}
void ATerrainDemoGameMode::BeginPlay(){
 Super::BeginPlay();Started=FPlatformTime::Seconds();LastReport=LastFrame=Started;NextMove=Started+20;
 FrameHistogram.SetNumZeroed(100001); // 0.01 ms buckets; final bucket means >=1000 ms.
 FParse::Value(FCommandLine::Get(),TEXT("TerrainSeconds="),Duration);
 FParse::Value(FCommandLine::Get(),TEXT("TerrainScreenshot="),ScreenshotPath);
 Terrain=GetWorld()->SpawnActor<ADynamicTerrainActor>();
 FString Config;
 if(FParse::Value(FCommandLine::Get(),TEXT("TerrainConfig="),Config)&&!Terrain->LoadSharedConfig(Config)){
  UE_LOG(LogTemp,Error,TEXT("TERRAIN_SOAK invalid config"));FPlatformMisc::RequestExitWithStatus(false,2);return;
 }
 FParse::Value(FCommandLine::Get(),TEXT("TerrainDetailZoom="),Terrain->ImageryDetailZoom);
 Camera=GetWorld()->SpawnActor<ACameraActor>(FVector(0,0,50000),FRotator(-70,20,0));
 Terrain->FollowActor=Camera;
 auto* Sun=GetWorld()->SpawnActor<ADirectionalLight>(FVector(0,0,50000),FRotator(-50,-30,0));
 Sun->GetLightComponent()->SetMobility(EComponentMobility::Movable);Sun->GetLightComponent()->SetIntensity(3.0f);
 if(auto* Player=GetWorld()->GetFirstPlayerController())Player->SetViewTarget(Camera);
 Terrain->RefreshTerrain();
}
void ATerrainDemoGameMode::Tick(float Dt){
 Super::Tick(Dt);if(!Terrain||!Camera)return;
 const double Now=FPlatformTime::Seconds();const double FrameSeconds=Now-LastFrame;LastFrame=Now;
 ++Frames;FrameSum+=FrameSeconds;FrameMax=FMath::Max(FrameMax,FrameSeconds);
 if(Now-Started>=15){++FrameHistogram[FMath::Clamp(int32(FMath::CeilToDouble(FrameSeconds*100000)),0,100000)];++MeasuredFrames;}
 // Check the current covered position before this fixture moves its camera.
 bEverReady=bEverReady||Terrain->IsTerrainReady();
 if(bEverReady){FHitResult Ground;++ContactChecks;if(!Terrain->TraceGround(Camera->GetActorLocation(),Ground)){
  ++ContactFailures;if(ContactFailures<=5)UE_LOG(LogTemp,Error,TEXT("TERRAIN_SOAK missing collision elapsed=%.3f generation=%lld"),Now-Started,Terrain->ActiveGeneration);
 }}
 if(!bScreenshotRequested&&!ScreenshotPath.IsEmpty()&&Terrain->IsTerrainReady()&&Now-Started>FMath::Min(10.0,Duration*.5)){
  FScreenshotRequest::RequestScreenshot(ScreenshotPath,false,false);bScreenshotRequested=true;
 }
 if(Now>=NextMove&&Terrain->IsTerrainReady()){
  ++Step;NextMove=Now+20;
  const double Distance=Terrain->RadiusM*.65*100;
  const int32 Route=Step%4;
  Camera->SetActorLocation(FVector((Step/4)*Distance+(Route==1||Route==2?Distance:0),Route>=2?Distance:0,50000));
 }
 if(Now-LastReport>=5){
  const auto Memory=FPlatformMemory::GetStats();FHitResult Hit;
  const bool Contact=Terrain->TraceGround(Camera->GetActorLocation(),Hit);
  UE_LOG(LogTemp,Display,TEXT("TERRAIN_SOAK elapsed=%.1f generation=%lld pages=%d cooks=%d updates=%d rss=%llu mean_ms=%.3f max_ms=%.3f contact=%d commit_ms=%.3f"),
    Now-Started,Terrain->ActiveGeneration,Terrain->LoadedPages,Terrain->CollisionCooks,Terrain->TextureUpdates,
    uint64(Memory.UsedPhysical),Frames?FrameSum/Frames*1000:0,FrameMax*1000,int(Contact),Terrain->LastCommitMilliseconds);
  UE_LOG(LogTemp,Display,TEXT("TERRAIN_PERCENTILES elapsed=%.1f samples=%llu p50_ms=%.2f p95_ms=%.2f p99_ms=%.2f resolution_ms=0.01 warmup_seconds=15"),
    Now-Started,MeasuredFrames,FramePercentile(.50),FramePercentile(.95),FramePercentile(.99));
  const auto& D=Terrain->Diagnostics;
  UE_LOG(LogTemp,Display,TEXT("TERRAIN_DIAGNOSTICS elapsed=%.1f queued=%lld active=%lld peak_queued=%lld peak_active=%lld buffered_bytes=%lld cache_hits=%lld downloaded=%lld failed=%lld cancelled=%lld bytes=%lld connections=%lld retries=%lld decoded_dem_bytes=%lld decoded_dem_entries=%lld decoded_imagery_bytes=%lld decoded_imagery_entries=%lld page_cache_bytes=%lld page_cache_entries=%lld pending_frames=%lld pending_texture_pages=%lld pending_texture_bytes=%lld preparing_frames=%lld staged_frames=%lld resident_textures=%lld"),Now-Started,D.Queued,D.Active,D.PeakQueued,D.PeakActive,D.BufferedBytes,D.CacheHits,D.Downloaded,D.Failed,D.Cancelled,D.Bytes,D.Connections,D.Retries,D.DecodedDemBytes,D.DecodedDemEntries,D.DecodedImageryBytes,D.DecodedImageryEntries,D.PageCacheBytes,D.PageCacheEntries,D.PendingFrames,D.PendingTexturePages,D.PendingTextureBytes,D.PreparingFrames,D.StagedFrames,D.ResidentTextures);
  Frames=0;FrameSum=FrameMax=0;LastReport=Now;
 }
 if(Duration>0&&Now-Started>=Duration){
  const bool Ready=Terrain->IsTerrainReady()&&ContactFailures==0;
  UE_LOG(LogTemp,Display,TEXT("TERRAIN_SOAK complete ready=%d generations=%lld samples=%llu p95_ms=%.2f p99_ms=%.2f contact_checks=%llu contact_failures=%llu"),
    int(Ready),Terrain->ActiveGeneration,MeasuredFrames,FramePercentile(.95),FramePercentile(.99),ContactChecks,ContactFailures);
  FPlatformMisc::RequestExitWithStatus(false,Ready?0:3);
 }
}
double ATerrainDemoGameMode::FramePercentile(double Fraction) const {
 if(!MeasuredFrames)return 0;
 const uint64 Required=FMath::CeilToDouble(MeasuredFrames*Fraction);uint64 Count=0;
 for(int32 Index=0;Index<FrameHistogram.Num();++Index){Count+=FrameHistogram[Index];if(Count>=Required)return Index*.01;}
 return 1000;
}
