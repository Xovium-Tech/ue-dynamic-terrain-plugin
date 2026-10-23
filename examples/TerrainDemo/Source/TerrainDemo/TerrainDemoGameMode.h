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
 double Started=0,LastReport=0,LastFrame=0,Duration=0,NextMove=0;
 int32 Step=0,Frames=0;
 double FrameSum=0,FrameMax=0;
 int64 LastGeneration=0;
 bool bScreenshotRequested=false;
 FString ScreenshotPath;
 TArray<uint64> FrameHistogram;
 uint64 MeasuredFrames=0;
 uint64 ContactChecks=0,ContactFailures=0;
 bool bEverReady=false;
 double FramePercentile(double Fraction) const;
};
