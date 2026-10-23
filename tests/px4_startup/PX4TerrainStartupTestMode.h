#pragma once
#include "CoreMinimal.h"
#include "PX4GameMode.h"
#include "PX4TerrainStartupTestMode.generated.h"

// Fixture-only subclass: production startup and vehicle spawning run unchanged.
UCLASS()
class APX4TerrainStartupTestMode : public APX4GameMode {
 GENERATED_BODY()
public:
 virtual void Tick(float DeltaSeconds) override;
protected:
 virtual void BeginPlay() override;
private:
 double Started=0,BeginPlaySeconds=0,LastReport=0;
 uint64 WaitingTicks=0;
 bool bFinished=false,bExpectTimeout=false;
 void Finish(bool Success,const TCHAR* Reason);
};
