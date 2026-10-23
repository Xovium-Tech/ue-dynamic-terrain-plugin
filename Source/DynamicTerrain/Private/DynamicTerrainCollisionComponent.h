#pragma once
#include "ProceduralMeshComponent.h"
#include "DynamicTerrainCollisionComponent.generated.h"
// Physics-only: no scene proxy, GPU vertex buffers or ray tracing resources.
UCLASS()
class UDynamicTerrainCollisionComponent : public UProceduralMeshComponent {
    GENERATED_BODY()
public:
    // Empty synchronously to drop UProceduralMeshComponent's async result queue.
    // A late cook callback can then no longer publish a cancelled generation.
    void ResetCollision() {
        SetCollisionEnabled(ECollisionEnabled::NoCollision);
        const bool WasAsync=bUseAsyncCooking;
        bUseAsyncCooking=false;ClearAllMeshSections();bUseAsyncCooking=WasAsync;
        TriangleMaterials.Empty();MaterialSlotCount=0;EmptyOverrideMaterials();
    }
    TArray<uint16> TriangleMaterials;
    int32 MaterialSlotCount=0;
    virtual int32 GetNumMaterials() const override { return MaterialSlotCount; }
    virtual FPrimitiveSceneProxy* CreateSceneProxy() override { return nullptr; }
    virtual UMaterialInterface* GetMaterialFromCollisionFaceIndex(int32 Face,int32& Section) const override;
    virtual bool GetPhysicsTriMeshData(FTriMeshCollisionData* Data,bool All) override;
};
