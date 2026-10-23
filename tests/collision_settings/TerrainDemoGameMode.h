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
 virtual void Tick(float) override;
private:
 UPROPERTY() TObjectPtr<class ADynamicTerrainActor> Terrain;
 UPROPERTY() TObjectPtr<class ACameraActor> Camera;
 UPROPERTY() TObjectPtr<class UMaterial> MaterialA;
 UPROPERTY() TObjectPtr<class UMaterial> MaterialB;
 UPROPERTY() TObjectPtr<class UPhysicalMaterial> SurfaceA;
 UPROPERTY() TObjectPtr<class UPhysicalMaterial> SurfaceB;
 UPROPERTY() TObjectPtr<class UPhysicalMaterial> Override;
 double Started=0;
 int32 Phase=0,OriginalCooks=0,OriginalUpdates=0;
 int64 OriginalGeneration=0;
 bool bInherited=false,bPendingEdited=false,bWithoutRecenter=false;
 bool PendingCook() const;
 bool SettingsMatch() const;
 bool SurfaceIs(UPhysicalMaterial*,const FVector& Position=FVector::ZeroVector) const;
 void Finish(bool,const TCHAR*);
};
