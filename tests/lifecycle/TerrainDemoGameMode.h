#pragma once
#include "CoreMinimal.h"
#include "GameFramework/GameModeBase.h"
#include "TerrainDemoGameMode.generated.h"
UCLASS()
class ATerrainDemoGameMode : public AGameModeBase {
 GENERATED_BODY()
public:
 ATerrainDemoGameMode();
 virtual void BeginPlay() override;
 virtual void Tick(float Dt) override;
private:
 UPROPERTY() TObjectPtr<class ADynamicTerrainActor> Terrain;
 UPROPERTY() TObjectPtr<class ACameraActor> Camera;
 FString Config;
 double Started=0,Deadline=120,PhaseAt=0;
 int32 Phase=0;
 bool bStopPending=false,bDestroyPending=false,bGroundPreserved=false,bMaterialParentUpdated=false;
 void SpawnTerrain();
 bool HasPendingCook() const;
 void Finish(bool Success,const TCHAR* Reason);
};
