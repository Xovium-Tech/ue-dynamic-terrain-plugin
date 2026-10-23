#include "TerrainDemoGameMode.h"
#include "DynamicTerrainActor.h"
#include "ProceduralMeshComponent.h"
#include "PhysicsEngine/BodySetup.h"
#include "Camera/CameraActor.h"
#include "Engine/World.h"
#include "HAL/PlatformMisc.h"
#include "Misc/CommandLine.h"
#include "Misc/Parse.h"
#include "UObject/UnrealType.h"
#include "UObject/GarbageCollection.h"
#include "Materials/Material.h"
#include "Materials/MaterialInstanceDynamic.h"

ATerrainDemoGameMode::ATerrainDemoGameMode(){PrimaryActorTick.bCanEverTick=true;DefaultPawnClass=nullptr;}
void ATerrainDemoGameMode::BeginPlay(){
 Super::BeginPlay();Started=PhaseAt=FPlatformTime::Seconds();
 FParse::Value(FCommandLine::Get(),TEXT("TerrainConfig="),Config);
 FParse::Value(FCommandLine::Get(),TEXT("TerrainSeconds="),Deadline);
 Camera=GetWorld()->SpawnActor<ACameraActor>(FVector(0,0,50000),FRotator(-70,20,0));
 SpawnTerrain();
}
void ATerrainDemoGameMode::SpawnTerrain(){
 Terrain=GetWorld()->SpawnActor<ADynamicTerrainActor>();
 if(!Terrain->LoadSharedConfig(Config)){Finish(false,TEXT("invalid config"));return;}
 // Dense geometry makes the genuine asynchronous cook observable on fast hosts.
 // This fixture does not measure rendering or replace the full-quality demo.
 Terrain->CellsPerTile=128;Terrain->RadiusM=1200;Terrain->TextureSize=256;
 Terrain->ImageryDetailZoom=Terrain->Zoom;Terrain->FollowActor=Camera;Terrain->RefreshTerrain();
}
bool ATerrainDemoGameMode::HasPendingCook() const {
 if(!Terrain)return false;
 // Read the engine's transient UPROPERTY queue; never modify it. This proves an
 // actual Chaos cook is in progress, rather than inferring it from a frame delay.
 const auto* Queue=FindFProperty<FArrayProperty>(UProceduralMeshComponent::StaticClass(),TEXT("AsyncBodySetupQueue"));
 const auto* Inner=Queue?CastField<FObjectPropertyBase>(Queue->Inner):nullptr;
 if(!Queue||!Inner)return false;
 TArray<UProceduralMeshComponent*> Components;Terrain->GetComponents(Components);
 for(auto* Component:Components){
  if(Component==Terrain->Terrain)continue;
  FScriptArrayHelper Items(Queue,Queue->ContainerPtrToValuePtr<void>(Component));
  for(int32 Index=0;Index<Items.Num();++Index){
   auto* Body=Cast<UBodySetup>(Inner->GetObjectPropertyValue(Items.GetRawPtr(Index)));
   if(Body&&Body->CurrentCookHelper&&!Body->bCreatedPhysicsMeshes)return true;
  }
 }
 return false;
}
void ATerrainDemoGameMode::Finish(bool Success,const TCHAR* Reason){
 UE_LOG(LogTemp,Display,TEXT("TERRAIN_LIFECYCLE passed=%d stop_pending=%d destroy_pending=%d old_ground_preserved=%d material_parent_updated=%d reason=%s"),
   int(Success),int(bStopPending),int(bDestroyPending),int(bGroundPreserved),int(bMaterialParentUpdated),Reason);
 UE_LOG(LogTemp,Display,TEXT("TERRAIN_SOAK complete ready=%d generations=%lld"),int(Success),Terrain?Terrain->ActiveGeneration:0);
 FPlatformMisc::RequestExitWithStatus(false,Success?0:4);Phase=99;
}
void ATerrainDemoGameMode::Tick(float Dt){
 Super::Tick(Dt);if(Phase==99)return;const double Now=FPlatformTime::Seconds();
 if(Now-Started>Deadline){Finish(false,TEXT("timed out waiting for lifecycle phase"));return;}
 if(Phase==0&&HasPendingCook()){
  bStopPending=true;Terrain->StopTerrain();
  if(Terrain->IsTerrainReady()){Finish(false,TEXT("Stop retained ready state"));return;}
  Phase=1;PhaseAt=Now;UE_LOG(LogTemp,Display,TEXT("TERRAIN_LIFECYCLE stopped pending cook"));
 }else if(Phase==1&&Now-PhaseAt>.5){
  CollectGarbage(RF_NoFlags);Terrain->RefreshTerrain();Phase=2;
 }else if(Phase==2&&Terrain->IsTerrainReady()){
  FHitResult Hit;if(!Terrain->TraceGround(Camera->GetActorLocation(),Hit)){Finish(false,TEXT("restart has no ground"));return;}
  Terrain->TerrainMaterial=UMaterial::GetDefaultMaterial(MD_Surface);
  Camera->SetActorLocation(FVector(Terrain->RadiusM*.65*100,0,50000));Phase=3;
 }else if(Phase==3&&Terrain->ActiveGeneration>1){
  bMaterialParentUpdated=true;
  for(int32 Index=0;Index<Terrain->Terrain->GetNumSections();++Index){
   auto* Material=Cast<UMaterialInstanceDynamic>(Terrain->Terrain->GetMaterial(Index));
   bMaterialParentUpdated=bMaterialParentUpdated&&Material&&Material->Parent==Terrain->TerrainMaterial;
  }
  if(!bMaterialParentUpdated){Finish(false,TEXT("reused MID retained old material parent"));return;}
  Camera->SetActorLocation(FVector(Terrain->RadiusM*.65*100,Terrain->RadiusM*.65*100,50000));Phase=4;
 }else if(Phase==4&&HasPendingCook()){
  FHitResult Hit;bGroundPreserved=Terrain->TraceGround(Camera->GetActorLocation(),Hit);
  if(!bGroundPreserved){Finish(false,TEXT("old ground missing during replacement cook"));return;}
  bDestroyPending=true;Terrain->Destroy();Terrain=nullptr;Phase=5;PhaseAt=Now;
  UE_LOG(LogTemp,Display,TEXT("TERRAIN_LIFECYCLE destroyed actor during pending replacement cook"));
 }else if(Phase==5&&Now-PhaseAt>.5){
  CollectGarbage(RF_NoFlags);SpawnTerrain();Phase=6;
 }else if(Phase==6&&Terrain->IsTerrainReady()){
  FHitResult Hit;const bool Contact=Terrain->TraceGround(Camera->GetActorLocation(),Hit);
  Finish(Contact&&bStopPending&&bDestroyPending&&bGroundPreserved&&bMaterialParentUpdated,TEXT("restart after Stop and EndPlay"));
 }
}
