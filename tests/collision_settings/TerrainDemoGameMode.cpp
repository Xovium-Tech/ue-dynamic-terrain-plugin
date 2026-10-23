#include "TerrainDemoGameMode.h"
#include "DynamicTerrainActor.h"
#include "ProceduralMeshComponent.h"
#include "PhysicalMaterials/PhysicalMaterial.h"
#include "PhysicsEngine/BodySetup.h"
#include "Materials/Material.h"
#include "Materials/MaterialInstanceDynamic.h"
#include "Camera/CameraActor.h"
#include "Engine/World.h"
#include "HAL/PlatformMisc.h"
#include "Misc/CommandLine.h"
#include "Misc/Parse.h"
#include "UObject/UnrealType.h"

ATerrainDemoGameMode::ATerrainDemoGameMode(){PrimaryActorTick.bCanEverTick=true;DefaultPawnClass=nullptr;}
void ATerrainDemoGameMode::BeginPlay(){
 Super::BeginPlay();Started=FPlatformTime::Seconds();
 bInherited=FParse::Param(FCommandLine::Get(),TEXT("InheritedPhysicalMaterial"));
 SurfaceA=NewObject<UPhysicalMaterial>(this);SurfaceA->Friction=.23f;
 SurfaceB=NewObject<UPhysicalMaterial>(this);SurfaceB->Friction=.71f;
 Override=NewObject<UPhysicalMaterial>(this);Override->Friction=.91f;
 MaterialA=NewObject<UMaterial>(this);MaterialA->PhysMaterial=SurfaceA;
 MaterialB=NewObject<UMaterial>(this);MaterialB->PhysMaterial=SurfaceB;
 Camera=GetWorld()->SpawnActor<ACameraActor>(FVector(0,0,50000),FRotator::ZeroRotator);
 Terrain=GetWorld()->SpawnActor<ADynamicTerrainActor>();
 FString Config;FParse::Value(FCommandLine::Get(),TEXT("TerrainConfig="),Config);
 if(!Terrain->LoadSharedConfig(Config)){Finish(false,TEXT("invalid config"));return;}
 Terrain->CellsPerTile=128;Terrain->TextureSize=512;Terrain->ImageryDetailZoom=17;
 Terrain->TerrainMaterial=MaterialA;Terrain->FollowActor=Camera;Terrain->RefreshTerrain();
}
bool ATerrainDemoGameMode::PendingCook() const {
 const auto* Queue=FindFProperty<FArrayProperty>(UProceduralMeshComponent::StaticClass(),TEXT("AsyncBodySetupQueue"));
 const auto* Inner=Queue?CastField<FObjectPropertyBase>(Queue->Inner):nullptr;
 if(!Queue||!Inner)return false;
 TArray<UProceduralMeshComponent*> Components;Terrain->GetComponents(Components);
 for(auto* C:Components)if(C!=Terrain->Terrain){
  FScriptArrayHelper Items(Queue,Queue->ContainerPtrToValuePtr<void>(C));
  for(int32 I=0;I<Items.Num();++I){auto* B=Cast<UBodySetup>(Inner->GetObjectPropertyValue(Items.GetRawPtr(I)));
   if(B&&B->CurrentCookHelper&&!B->bCreatedPhysicsMeshes)return true;
  }
 }
 return false;
}
bool ATerrainDemoGameMode::SettingsMatch() const {
 TArray<UProceduralMeshComponent*> Components;Terrain->GetComponents(Components);int32 Count=0;
 for(auto* C:Components)if(C!=Terrain->Terrain){++Count;
  if(C->BodyInstance.GetPhysMaterialOverride()!=Terrain->Terrain->BodyInstance.GetPhysMaterialOverride()||
     C->GetCollisionObjectType()!=Terrain->Terrain->GetCollisionObjectType()||
     C->GetCollisionResponseToChannels()!=Terrain->Terrain->GetCollisionResponseToChannels())return false;
 }
 return Count==2;
}
bool ATerrainDemoGameMode::SurfaceIs(UPhysicalMaterial* Expected,const FVector& P) const {
 FHitResult Hit;if(!Terrain->TraceGround(P,Hit))return false;
 auto* Component=Hit.GetComponent();if(!Component)return false;
 FCollisionQueryParams Query(SCENE_QUERY_STAT(TerrainSettings),true);Query.bReturnPhysicalMaterial=true;Query.bReturnFaceIndex=true;
 return Component->LineTraceComponent(Hit,FVector(P.X,P.Y,100000),FVector(P.X,P.Y,-100000),Query)&&Hit.PhysMaterial.Get()==Expected;
}
void ATerrainDemoGameMode::Finish(bool Success,const TCHAR* Reason){
 UE_LOG(LogTemp,Display,TEXT("TERRAIN_SETTINGS passed=%d inherited=%d pending_settings=%d without_recenter=%d generation=%lld cooks=%d reason=%s"),
  int(Success),int(bInherited),int(bPendingEdited),int(bWithoutRecenter),Terrain?Terrain->ActiveGeneration:0,Terrain?Terrain->CollisionCooks:0,Reason);
 FPlatformMisc::RequestExitWithStatus(false,Success?0:4);Phase=99;
}
void ATerrainDemoGameMode::Tick(float Dt){
 Super::Tick(Dt);if(Phase==99||!Terrain)return;
 if(FPlatformTime::Seconds()-Started>60){Finish(false,*FString::Printf(TEXT("timeout in phase%d"),Phase));return;}
 if(Phase==0){
  if(!bPendingEdited&&PendingCook()){
   Terrain->Terrain->SetPhysMaterialOverride(Override);Terrain->Terrain->SetCollisionObjectType(ECC_WorldDynamic);
   Terrain->Terrain->SetCollisionResponseToChannel(ECC_Visibility,ECR_Ignore);bPendingEdited=true;
  }
  if(Terrain->IsTerrainReady()){
   if(!bPendingEdited||!SettingsMatch()||!SurfaceIs(Override)){Finish(false,TEXT("settings changed during cook were lost at activation"));return;}
   OriginalGeneration=Terrain->ActiveGeneration;OriginalCooks=Terrain->CollisionCooks;OriginalUpdates=Terrain->TextureUpdates;
   Terrain->Terrain->SetPhysMaterialOverride(nullptr);Terrain->Terrain->SetCollisionObjectType(ECC_WorldStatic);
   Terrain->Terrain->SetCollisionResponseToAllChannels(ECR_Block);Phase=1;
  }
 }else if(Phase==1&&SettingsMatch()&&SurfaceIs(SurfaceA)){
  if(bInherited)MaterialA->PhysMaterial=SurfaceB;else Terrain->TerrainMaterial=MaterialB;
  Phase=2;
 }else if(Phase==2&&Terrain->TextureUpdates>OriginalUpdates){
  FHitResult Hit;if(!Terrain->TraceGround(FVector::ZeroVector,Hit)){Finish(false,TEXT("ground lost during material update"));return;}
  auto* Collider=Cast<UProceduralMeshComponent>(Hit.GetComponent());bool Verified=false;
  for(int32 I=0;I<Terrain->Terrain->GetNumSections();++I){
   auto* Visual=Cast<UMaterialInstanceDynamic>(Terrain->Terrain->GetMaterial(I));
   if(Visual&&Visual->Parent==(bInherited?MaterialA.Get():MaterialB.Get())){
    if(Collider->GetMaterial(I)!=Visual){Finish(false,TEXT("active collider retained old material instance"));return;}
    const auto* Section=Terrain->Terrain->GetProcMeshSection(I);
    if(Section&&SurfaceIs(SurfaceB,Terrain->GetActorTransform().TransformPosition(Section->SectionLocalBox.GetCenter())))Verified=true;
   }
  }
  if(!Verified)return;
  Terrain->Terrain->SetPhysMaterialOverride(Override);Terrain->Terrain->SetCollisionObjectType(ECC_WorldDynamic);
  Terrain->Terrain->SetCollisionResponseToChannel(ECC_Visibility,ECR_Ignore);
  Terrain->Terrain->SetCollisionResponseToChannel(ECC_Pawn,ECR_Overlap);Phase=3;
 }else if(Phase==3&&SettingsMatch()&&SurfaceIs(Override)){
  Terrain->Terrain->SetCollisionResponseToChannel(ECC_PhysicsBody,ECR_Ignore);Phase=4;
 }else if(Phase==4&&SettingsMatch()){
  FHitResult Hit;if(Terrain->TraceGround(FVector::ZeroVector,Hit)){Finish(false,TEXT("runtime response edit did not affect active ground"));return;}
  Terrain->Terrain->SetCollisionResponseToChannel(ECC_PhysicsBody,ECR_Block);Terrain->SetActorEnableCollision(false);Phase=5;
 }else if(Phase==5&&SettingsMatch()){
  FHitResult Hit;if(Terrain->TraceGround(FVector::ZeroVector,Hit)){Finish(false,TEXT("owner collision disable ignored"));return;}
  Terrain->SetActorEnableCollision(true);Phase=6;
 }else if(Phase==6&&SurfaceIs(Override)){
  Terrain->Terrain->SetPhysMaterialOverride(nullptr);Terrain->Terrain->SetCollisionObjectType(ECC_WorldStatic);
  Terrain->Terrain->SetCollisionResponseToAllChannels(ECR_Block);Phase=7;
 }else if(Phase==7&&SettingsMatch()){
  // Parent changes apply to refined pages only, as in the original actor.
  FHitResult Hit;if(!Terrain->TraceGround(FVector::ZeroVector,Hit))return;
  bWithoutRecenter=Terrain->ActiveGeneration==OriginalGeneration&&Terrain->CollisionCooks==OriginalCooks;
  if(!bWithoutRecenter){Finish(false,TEXT("runtime settings required a new generation"));return;}
  Terrain->SetActorEnableCollision(false);Camera->SetActorLocation(FVector(Terrain->RadiusM*.65*100,0,50000));Phase=8;
 }else if(Phase==8){
  FHitResult Hit;if(Terrain->TraceGround(Camera->GetActorLocation(),Hit)){Finish(false,TEXT("owner disable lost during recenter"));return;}
  if(Terrain->CollisionCooks>OriginalCooks&&!PendingCook()){Terrain->SetActorEnableCollision(true);Phase=9;}
 }else if(Phase==9&&Terrain->ActiveGeneration>OriginalGeneration){
  FHitResult Hit;Finish(SettingsMatch()&&Terrain->TraceGround(Camera->GetActorLocation(),Hit),TEXT("material/settings propagation and disabled-owner recenter"));
 }
}
