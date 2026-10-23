#include "TerrainDemoGameMode.h"
#include "DynamicTerrainActor.h"
#include "Engine/SceneCapture2D.h"
#include "Components/SceneCaptureComponent2D.h"
#include "Engine/TextureRenderTarget2D.h"
#include "Engine/World.h"
#include "ProceduralMeshComponent.h"
#include "ImageUtils.h"
#include "Misc/FileHelper.h"
#include "Misc/CommandLine.h"
#include "Misc/Parse.h"
#include "HAL/PlatformMisc.h"
#include "RenderingThread.h"
#include "Engine/DirectionalLight.h"
#include "Components/DirectionalLightComponent.h"
ATerrainDemoGameMode::ATerrainDemoGameMode(){PrimaryActorTick.bCanEverTick=true;DefaultPawnClass=nullptr;}
void ATerrainDemoGameMode::BeginPlay(){
 Super::BeginPlay();Started=FPlatformTime::Seconds();FString Config;
 FParse::Value(FCommandLine::Get(),TEXT("TerrainConfig="),Config);
 FParse::Value(FCommandLine::Get(),TEXT("CoverageOutput="),Output);
 Terrain=GetWorld()->SpawnActor<ADynamicTerrainActor>();
 if(!Terrain->LoadSharedConfig(Config)){Finish(false,TEXT("config"));return;}
 Terrain->OuterVisualRadiusM=2000000;Terrain->RefreshTerrain();
 auto* Sun=GetWorld()->SpawnActor<ADirectionalLight>(FVector(0,0,100000),FRotator(-90,0,0));
 Sun->GetLightComponent()->SetMobility(EComponentMobility::Movable);Sun->GetLightComponent()->SetIntensity(3);
 for(int i=0;i<2;++i){
  auto* Camera=GetWorld()->SpawnActor<ASceneCapture2D>();Cameras.Add(Camera);
  auto* Target=NewObject<UTextureRenderTarget2D>(this);Targets.Add(Target);
  Target->RenderTargetFormat=RTF_RGBA8;Target->ClearColor=FLinearColor(1,0,1);Target->InitAutoFormat(1280,960);
  auto* Capture=Camera->GetCaptureComponent2D();Capture->TextureTarget=Target;
  Capture->FOVAngle=FMath::RadiansToDegrees(1.74);Capture->CaptureSource=ESceneCaptureSource::SCS_FinalColorLDR;
  Capture->bCaptureEveryFrame=false;Capture->bCaptureOnMovement=false;
  Capture->ShowFlags.SetAtmosphere(false);Capture->ShowFlags.SetFog(false);
  Capture->ShowFlags.SetAntiAliasing(false);Capture->ShowFlags.SetMotionBlur(false);
  Capture->PostProcessSettings.bOverride_AutoExposureMinBrightness=true;Capture->PostProcessSettings.AutoExposureMinBrightness=1;
  Capture->PostProcessSettings.bOverride_AutoExposureMaxBrightness=true;Capture->PostProcessSettings.AutoExposureMaxBrightness=1;
 }
 Terrain->FollowActor=Cameras[0];
}
void ATerrainDemoGameMode::Finish(bool Pass,const TCHAR* Reason){
 Done=true;UE_LOG(LogTemp,Display,TEXT("TERRAIN_COVERAGE passed=%d images=%d checked=%llu missing=%d cooks=%d generation=%lld revision=%lld reason=%s"),
 int(Pass),Images,Checked,Missing,Terrain?Terrain->CollisionCooks:0,Terrain?Terrain->ActiveGeneration:0,Terrain?Terrain->ActiveCoverageRevision:0,Reason);
 FPlatformMisc::RequestExitWithStatus(false,Pass?0:7);
}
void ATerrainDemoGameMode::Tick(float Dt){
 Super::Tick(Dt);if(Done)return;const double Now=FPlatformTime::Seconds();
 if(Now-Started>180){Finish(false,TEXT("timeout"));return;}
 if(!Terrain->IsDistantTerrainReady()||Now<NextCapture)return;
 if(Step==8){
  // A camera translation requests a full replacement, while ground must remain.
  for(auto Camera:Cameras)Camera->SetActorLocation(FVector(170000,0,3000000));
  if(Terrain->ActiveGeneration<2){FHitResult Hit;if(!Terrain->TraceGround(FVector(170000,0,0),Hit)){Finish(false,TEXT("collision gap"));}return;}
 }
 if((Step==0||Step==8)&&!CheckPreparedMeshParity()){Finish(false,TEXT("prepared mesh parity"));return;}
 Capture();++Step;NextCapture=Now+.15;
 if(Step==16)Finish(Missing==0&&Images==32&&Terrain->CollisionCooks==2,TEXT("two cameras, 30km, turns, recenter, one near cook per generation"));
}
bool ATerrainDemoGameMode::CheckPreparedMeshParity(){
 auto* Reference=NewObject<UProceduralMeshComponent>(this);uint64 Vertices=0;
 for(int32 Section=0;Section<Terrain->Terrain->GetNumSections();++Section){
  const auto* Actual=Terrain->Terrain->GetProcMeshSection(Section);if(!Actual)return false;
  TArray<FVector> Positions,Normals;TArray<FVector2D> UV;TArray<int32> Indices;
  for(const auto& V:Actual->ProcVertexBuffer){Positions.Add(V.Position);Normals.Add(V.Normal);UV.Add(V.UV0);}
  for(auto Index:Actual->ProcIndexBuffer)Indices.Add(Index);
  Reference->CreateMeshSection_LinearColor(0,Positions,Indices,Normals,UV,{}, {},false);
  const auto* Expected=Reference->GetProcMeshSection(0);
  if(Expected->ProcIndexBuffer!=Actual->ProcIndexBuffer||Expected->SectionLocalBox!=Actual->SectionLocalBox||
     Expected->bEnableCollision!=Actual->bEnableCollision||Expected->bSectionVisible!=Actual->bSectionVisible||
     Expected->ProcVertexBuffer.Num()!=Actual->ProcVertexBuffer.Num())return false;
  for(int32 i=0;i<Actual->ProcVertexBuffer.Num();++i){
   const auto& A=Actual->ProcVertexBuffer[i];const auto& B=Expected->ProcVertexBuffer[i];
   if(A.Position!=B.Position||A.Normal!=B.Normal||A.UV0!=B.UV0||A.UV1!=B.UV1||A.UV2!=B.UV2||A.UV3!=B.UV3||
      A.Color!=B.Color||A.Tangent.TangentX!=B.Tangent.TangentX||A.Tangent.bFlipTangentY!=B.Tangent.bFlipTangentY)return false;
  }
  Vertices+=Actual->ProcVertexBuffer.Num();
 }
 UE_LOG(LogTemp,Display,TEXT("TERRAIN_PREPARED_MESH_PARITY generation=%lld sections=%d vertices=%llu exact=1"),Terrain->ActiveGeneration,Terrain->Terrain->GetNumSections(),Vertices);
 return true;
}
void ATerrainDemoGameMode::Capture(){
 const double Roll[]={0,55,-55,70,-70,25,-25,0};
 const double Yaw=(Step%4)*90.;const double East=Step>=8?170000.:0.;
 for(int Cam=0;Cam<2;++Cam){
  const FQuat Body=FRotator(0,Yaw,Roll[Step%8]).Quaternion();
  const FVector Origin=FVector(East,0,3000000)+Body.RotateVector(Cam==0?FVector(85,0,15):FVector(0,0,-10));
  const FRotator Rotation=(Body*FRotator(Cam==0?0:-90,0,0).Quaternion()).Rotator();
  Cameras[Cam]->SetActorLocationAndRotation(Origin,Rotation);
  auto* Capture=Cameras[Cam]->GetCaptureComponent2D();Capture->CaptureScene();FlushRenderingCommands();
  TArray<FColor> Pixels;Targets[Cam]->GameThread_GetRenderTargetResource()->ReadPixels(Pixels);
  const int W=Targets[Cam]->SizeX,H=Targets[Cam]->SizeY;const double Tx=FMath::Tan(.87),Ty=Tx*H/W;
  if(Pixels.Num()!=W*H){Finish(false,TEXT("readback"));return;}
  uint64 Valid=0;int32 Bad=0;
  for(int Y=2;Y<H-2;++Y)for(int X=2;X<W-2;++X){
   const FVector Ray=Rotation.RotateVector(FVector(1,(2.*(X+.5)/W-1)*Tx,(1-2.*(Y+.5)/H)*Ty));
   if(Ray.Z>=-.01)continue;const double T=-Origin.Z/Ray.Z;
   const FVector Hit=Origin+Ray*T;if(FMath::Abs(Hit.X)>100000000||FMath::Abs(Hit.Y)>100000000)continue;
   ++Valid;const auto& P=Pixels[Y*W+X];if(P.G<=P.R+15||P.G<=P.B+15)++Bad;
  }
  Checked+=Valid;Missing+=Bad;++Images;
  TArray64<uint8> PNG;FImageUtils::PNGCompressImageArray(W,H,Pixels,PNG);
  FFileHelper::SaveArrayToFile(PNG,*FString::Printf(TEXT("%s/%02d-%s.png"),*Output,Step,Cam==0?TEXT("front"):TEXT("bottom")));
  UE_LOG(LogTemp,Display,TEXT("TERRAIN_COVERAGE_IMAGE step=%d camera=%d valid=%llu missing=%d"),Step,Cam,Valid,Bad);
 }
}
