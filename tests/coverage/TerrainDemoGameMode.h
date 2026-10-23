#pragma once
#include "GameFramework/GameModeBase.h"
#include "TerrainDemoGameMode.generated.h"
class ADynamicTerrainActor;
class ASceneCapture2D;
class UTextureRenderTarget2D;
UCLASS()
class ATerrainDemoGameMode: public AGameModeBase {
 GENERATED_BODY()
public:
 ATerrainDemoGameMode();
 void BeginPlay() override;
 void Tick(float Delta) override;
private:
 void Capture();
 bool CheckPreparedMeshParity();
 void Finish(bool Pass,const TCHAR* Reason);
 UPROPERTY() TObjectPtr<ADynamicTerrainActor> Terrain;
 UPROPERTY() TArray<TObjectPtr<ASceneCapture2D>> Cameras;
 UPROPERTY() TArray<TObjectPtr<UTextureRenderTarget2D>> Targets;
 double Started=0,NextCapture=0;
 FString Output;
 int32 Step=0,Images=0,Missing=0;uint64 Checked=0;bool Done=false;
};
