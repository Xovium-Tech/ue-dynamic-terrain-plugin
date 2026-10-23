#include "DynamicTerrainCollisionComponent.h"
#include "PhysicsEngine/BodySetup.h"
bool UDynamicTerrainCollisionComponent::GetPhysicsTriMeshData(FTriMeshCollisionData* Data,bool All) {
    const bool Result=Super::GetPhysicsTriMeshData(Data,All);
    if(Data->MaterialIndices.Num()==TriangleMaterials.Num()) Data->MaterialIndices=TriangleMaterials;
    return Result;
}

UMaterialInterface* UDynamicTerrainCollisionComponent::GetMaterialFromCollisionFaceIndex(int32 Face,int32& Section) const {
    Section=TriangleMaterials.IsValidIndex(Face)?TriangleMaterials[Face]:0;
    return GetMaterial(Section);
}
