#include "DynamicTerrainActor.h"
#include "DynamicTerrainCollisionComponent.h"
#include "ue_px4/media.h"
#include "ProceduralMeshComponent.h"
#include "Engine/Texture2D.h"
#include "TextureResource.h"
#include "Rendering/Texture2DResource.h"
#include "ImageCore.h"
#include "Engine/World.h"
#include "Materials/MaterialInstanceDynamic.h"
#include "PhysicsEngine/BodySetup.h"
#include "PhysicalMaterials/PhysicalMaterial.h"
#include "Misc/Paths.h"
#include "HAL/PlatformProcess.h"
#include "HAL/PlatformMisc.h"
#include "Misc/CommandLine.h"
#include "Misc/Parse.h"
#include "Misc/FileHelper.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"
#include "Async/Async.h"
#include "Misc/QueuedThreadPool.h"
#include "HAL/PlatformAffinity.h"
#include "Misc/MemStack.h"
#include "RenderingThread.h"
#include "RenderCommandFence.h"
#include "Containers/Ticker.h"
#include <atomic>

struct FPreparedMip { int32 Width=0,Height=0; TArray<uint8> Bytes; };
struct FNativeImageIdentity {
    explicit FNativeImageIdentity(xov_terrain_image_identity* InValue):Value(InValue) {}
    ~FNativeImageIdentity(){xov_terrain_release_image_identity(Value);}
    FNativeImageIdentity(const FNativeImageIdentity&)=delete;
    FNativeImageIdentity& operator=(const FNativeImageIdentity&)=delete;
    xov_terrain_image_identity* Value;
};
using FImageIdentityPtr=TSharedPtr<FNativeImageIdentity,ESPMode::ThreadSafe>;
struct FPreparedPage {
    int32 Section=0,ImageryZoom=0;
    FIntVector Key;
    bool bVisualOnly=false;
    FProcMeshSection Mesh;
    TArray<FPreparedMip> Mips;
    FImageIdentityPtr ImageIdentity;
};
struct FPreparedBatch {
    uint64 Generation=0;
    uint64 CoverageRevision=0;
    bool bTextureOnly=false;
    TArray<FPreparedPage> Pages;
    TArray<FVector> CollisionVertices;
    TArray<FVector2D> CollisionUV;
    TArray<int32> CollisionIndices;
    TArray<uint16> TriangleMaterials;
};
using FPreparedPtr=TSharedPtr<FPreparedBatch,ESPMode::ThreadSafe>;
struct ADynamicTerrainActor::FState {
    struct FRetiredTexture {
        uint64 Frame=0,Bytes=0;
        bool bResourceReleased=false;
        TSharedPtr<FRenderCommandFence> Fence;
    };
    TArray<FRetiredTexture> Retired;
    FTSTicker::FDelegateHandle RetirementTicker;
    uint64 LastCleanupFrame=MAX_uint64;
    TUniquePtr<FQueuedThreadPool> PreparationPool;
    double NextPoolAttempt=0;
    bool bPoolFailureLogged=false;
    bool bExplicitlyStopped=false;
    TFuture<FPreparedPtr> Preparing;
    FPreparedPtr Pending;
    TSharedPtr<std::atomic<bool>,ESPMode::ThreadSafe> Cancel=MakeShared<std::atomic<bool>,ESPMode::ThreadSafe>(false);
    TWeakObjectPtr<UDynamicTerrainCollisionComponent> ActiveCollision,StagingCollision;
    TWeakObjectPtr<UBodySetup> PreviousCook;
    TMap<int32,FIntVector> SectionKeys;
    // Weak native ownership identities contain no decoded image payload.
    TMap<FIntVector,FImageIdentityPtr> PageImageIdentities;
    TMap<int32,TWeakObjectPtr<UPhysicalMaterial>> ActiveMaterialPhysics;
    bool bCooking=false;
    int32 CoverageUploadIndex=0;
    double LastUpdate=-1,CookStarted=0;
};
namespace {
// Admission limits apply between prepared batches. One atomic replacement can
// exceed them, but a second batch cannot add to a stalled retirement queue.
constexpr int64 RetiredTextureBudget=128ll*1024*1024;
constexpr int32 RetiredTextureCountLimit=512;
bool PrepareMipChain(const uint8* RGB,int32 Width,int32 Height,
                     const TSharedPtr<std::atomic<bool>,ESPMode::ThreadSafe>& Cancel,TArray<FPreparedMip>& Mips) {
    FImage Level(Width,Height,ERawImageFormat::BGRA8,EGammaSpace::sRGB);
    auto Pixels=Level.AsBGRA8();
    for(int32 i=0;i<Width*Height;++i) Pixels[i]=FColor(RGB[3*i],RGB[3*i+1],RGB[3*i+2],255);
    for(;;) {
        auto& Mip=Mips.AddDefaulted_GetRef();Mip.Width=Level.SizeX;Mip.Height=Level.SizeY;
        Mip.Bytes.Append(Level.RawData.GetData(),Level.RawData.Num());
        if(Level.SizeX==1&&Level.SizeY==1) break;
        if(Cancel->load()) return false;
        FImage Next;
        FImageCore::ResizeImageAllocDest(Level,Next,FMath::Max(1,Level.SizeX/2),FMath::Max(1,Level.SizeY/2),FImageCore::EResizeImageFilter::Box);
        Level=MoveTemp(Next);
    }
    return true;
}
FPreparedPtr PrepareFrame(xov_terrain_frame* Frame,const FTransform Transform,
                          const TSharedPtr<std::atomic<bool>,ESPMode::ThreadSafe>& Cancel) {
    struct FRelease { xov_terrain_frame* Value; ~FRelease(){xov_terrain_release_frame(Value);} } Release{Frame};
    xov_terrain_frame_info Info{};
    if(xov_terrain_frame_get_info(Frame,&Info)!=0 || Info.abi_version!=XOV_TERRAIN_ABI_VERSION) return {};
    auto Batch=MakeShared<FPreparedBatch,ESPMode::ThreadSafe>();
    Batch->Generation=Info.generation;Batch->bTextureOnly=Info.texture_only!=0;
    Batch->CoverageRevision=Info.coverage_revision;
    Batch->Pages.Reserve(Info.page_count);
    for(size_t Index=0;Index<Info.page_count;++Index) {
        if(Cancel->load()) return {};
        xov_terrain_frame_page Source{};
        if(xov_terrain_frame_get_page(Frame,Index,&Source)!=0) return {};
        const auto& P=Source.page;
        if(P.vertices>MAX_int32 || P.indices>MAX_int32 || P.section<0 || P.section>MAX_uint16) return {};
        auto& Page=Batch->Pages.AddDefaulted_GetRef();
        Page.Section=P.section;Page.ImageryZoom=P.imagery_zoom;Page.Key=FIntVector(Source.tile_x,Source.tile_y,Source.tile_zoom);
        Page.bVisualOnly=Source.visual_only!=0;
        const int32 Base=Batch->CollisionVertices.Num();
        Page.Mesh.ProcVertexBuffer.SetNum(P.vertices);
        for(size_t i=0;i<P.vertices;++i) {
            auto& Vertex=Page.Mesh.ProcVertexBuffer[i];
            Vertex.Position=Transform.InverseTransformPosition(FVector(P.positions[i*3]*100,P.positions[i*3+1]*100,P.positions[i*3+2]*100));
            Vertex.Normal=Transform.InverseTransformVectorNoScale(FVector(P.normals[i*3],P.normals[i*3+1],P.normals[i*3+2]));
            Vertex.UV0=FVector2D(P.uv[i*2],P.uv[i*2+1]);
            Page.Mesh.SectionLocalBox+=Vertex.Position;
            if(!Page.bVisualOnly) {Batch->CollisionVertices.Add(Vertex.Position);Batch->CollisionUV.Add(Vertex.UV0);}
        }
        Page.Mesh.ProcIndexBuffer.Reserve(P.indices);
        for(size_t i=0;i<P.indices;++i) {
            if(P.triangles[i]>=P.vertices) return {};
            if(!Page.bVisualOnly) Batch->CollisionIndices.Add(Base+P.triangles[i]);
        }
        // Match CreateMeshSection's complete triangles and duplicate-index
        // filtering on the worker, including its default vertex attributes.
        for(size_t i=0;i+2<P.indices;i+=3) {
            const auto A=P.triangles[i],B=P.triangles[i+1],C=P.triangles[i+2];
            if(A!=B&&A!=C&&B!=C) {Page.Mesh.ProcIndexBuffer.Add(A);Page.Mesh.ProcIndexBuffer.Add(B);Page.Mesh.ProcIndexBuffer.Add(C);}
        }
        if(!Page.bVisualOnly) {
            for(size_t i=0;i<P.indices/3;++i) Batch->TriangleMaterials.Add(P.section);
        }
        if(P.rgb && P.width>0 && P.height>0 && P.width<=4096 && P.height<=4096) {
            if(auto* Identity=xov_terrain_frame_acquire_image_identity(Frame,Index))
                Page.ImageIdentity=MakeShared<FNativeImageIdentity,ESPMode::ThreadSafe>(Identity);
            if(!PrepareMipChain(P.rgb,P.width,P.height,Cancel,Page.Mips)) return {};
        }
    }
    return Batch;
}
}
ADynamicTerrainActor::ADynamicTerrainActor() {
    PrimaryActorTick.bCanEverTick=true;PrimaryActorTick.TickGroup=TG_PrePhysics;
    Terrain=CreateDefaultSubobject<UProceduralMeshComponent>(TEXT("StreamedGround"));RootComponent=Terrain;
    Terrain->SetCanEverAffectNavigation(false);Terrain->SetCollisionProfileName(TEXT("BlockAll"));
    Terrain->SetCollisionEnabled(ECollisionEnabled::NoCollision);
    CollisionA=CreateDefaultSubobject<UDynamicTerrainCollisionComponent>(TEXT("TerrainCollisionA"));
    CollisionB=CreateDefaultSubobject<UDynamicTerrainCollisionComponent>(TEXT("TerrainCollisionB"));
    for(auto* Collision:{CollisionA.Get(),CollisionB.Get()}) {
        Collision->SetupAttachment(Terrain);Collision->SetCanEverAffectNavigation(false);
        Collision->bUseAsyncCooking=true;Collision->bUseComplexAsSimpleCollision=true;
        Collision->SetCollisionProfileName(TEXT("BlockAll"));Collision->SetCollisionEnabled(ECollisionEnabled::NoCollision);
        Collision->SetVisibility(false);Collision->SetHiddenInGame(true);
    }
    State=MakeShared<FState>();
}
ADynamicTerrainActor::~ADynamicTerrainActor()=default;
bool ADynamicTerrainActor::LoadSharedConfig(const FString& Path) {
    FString Text;TSharedPtr<FJsonObject> Json;
    if(!FFileHelper::LoadFileToString(Text,*Path) || !FJsonSerializer::Deserialize(TJsonReaderFactory<>::Create(Text),Json) || !Json.IsValid()) return false;
    double Schema=0,ZoomValue=0,Radius=0,Lat=0,Lon=0,Alt=0;
    FString Cache,Imagery,Elevation;
    const TSharedPtr<FJsonObject>* Origin=nullptr;
    if(!Json->TryGetNumberField(TEXT("schema"),Schema) || Schema!=1 || !Json->TryGetObjectField(TEXT("origin"),Origin) ||
       !(*Origin)->TryGetNumberField(TEXT("latitude"),Lat) || !(*Origin)->TryGetNumberField(TEXT("longitude"),Lon) ||
       !(*Origin)->TryGetNumberField(TEXT("altitude"),Alt) || !Json->TryGetNumberField(TEXT("zoom"),ZoomValue) ||
       !Json->TryGetNumberField(TEXT("radius"),Radius) || !Json->TryGetStringField(TEXT("cache"),Cache) ||
       !Json->TryGetStringField(TEXT("imagery_url"),Imagery) || !Json->TryGetStringField(TEXT("elevation_url"),Elevation)) return false;
    if(!FMath::IsFinite(Lat)||FMath::Abs(Lat)>85||!FMath::IsFinite(Lon)||FMath::Abs(Lon)>180||!FMath::IsFinite(Alt)||
       !FMath::IsFinite(ZoomValue)||ZoomValue<10||ZoomValue>17||FMath::FloorToDouble(ZoomValue)!=ZoomValue||
       !FMath::IsFinite(Radius)||Radius<1||Radius>3||FMath::FloorToDouble(Radius)!=Radius) return false;
    if(Elevation.StartsWith(TEXT("https://s3.amazonaws.com/elevation-tiles-prod/terrarium/")) && ZoomValue>15) return false;
    Latitude=Lat;Longitude=Lon;Altitude=Alt;Zoom=int32(ZoomValue);ImageryDetailZoom=Zoom;
    RadiusM=(Radius+.5)*40075016.686*FMath::Cos(FMath::DegreesToRadians(Latitude))/FMath::Pow(2.,Zoom);
    ImageryDetailRadiusM=RadiusM;CacheDirectory.Path=Cache;ImageryURL=Imagery;ElevationURL=Elevation;
    double Outer=2000000;
    Json->TryGetNumberField(TEXT("visual_outer_radius_m"),Outer);
    if(!FMath::IsFinite(Outer)||Outer<0||Outer>2000000) return false;
    OuterVisualRadiusM=Outer;
    if(FPaths::IsRelative(CacheDirectory.Path)) CacheDirectory.Path=FPaths::ConvertRelativePathToFull(FPaths::GetPath(Path),CacheDirectory.Path);
    return true;
}
FString ADynamicTerrainActor::GetTerrainCacheDirectory() const {
    FString Directory=CacheDirectory.Path;
    if(Directory.IsEmpty()) Directory=FPlatformMisc::GetEnvironmentVariable(TEXT("XOVIUM_TERRAIN_CACHE"));
    if(Directory.IsEmpty() && (!ImageryURL.IsEmpty()||!ElevationURL.IsEmpty())) Directory=FPaths::ProjectSavedDir()/TEXT("TerrainCache");
    if(Directory.IsEmpty()) Directory=FString(FPlatformProcess::UserHomeDir())/TEXT(".cache/gz_dynamic_terrain");
    if(Directory.StartsWith(TEXT("~/"))) Directory=FString(FPlatformProcess::UserHomeDir())/Directory.Mid(2);
    return FPaths::ConvertRelativePathToFull(Directory);
}
void ADynamicTerrainActor::HideFallbackSurfaces(bool DisableCollision) {
    if(!IsValid(FallbackGround)) return;
    TArray<UPrimitiveComponent*> Components;
    if(auto* Owner=FallbackGround->GetOwner()) Owner->GetComponents(Components);
    Components.AddUnique(FallbackGround.Get());
    for(auto* Component:Components) {
        if(Component==Terrain || (Component!=FallbackGround&&!Component->ComponentHasTag(FallbackSurfaceTag))) continue;
        if(!FallbackStates.ContainsByPredicate([Component](const FFallbackState& S){return S.Component.Get()==Component;}))
            FallbackStates.Add({Component,Component->IsVisible(),Component->GetCollisionEnabled()});
        Component->SetVisibility(false);if(DisableCollision) Component->SetCollisionEnabled(ECollisionEnabled::NoCollision);
    }
}
void ADynamicTerrainActor::BeginPlay() {
    Super::BeginPlay();
    if(!TerrainMaterial) TerrainMaterial=LoadObject<UMaterialInterface>(nullptr,TEXT("/DynamicTerrain/M_Terrain.M_Terrain"));
    if(bStreamTerrain) HideFallbackSurfaces(false);
}
void ADynamicTerrainActor::RefreshTerrain() {
    ReleaseTerrainResources();
    if(!bStreamTerrain||IsRunningCommandlet()) return;
    State->bExplicitlyStopped=false;
    if(xov_terrain_abi_version()!=XOV_TERRAIN_ABI_VERSION) { UE_LOG(LogTemp,Error,TEXT("DynamicTerrain native ABI mismatch."));return; }
    if(!TerrainMaterial) TerrainMaterial=LoadObject<UMaterialInterface>(nullptr,TEXT("/DynamicTerrain/M_Terrain.M_Terrain"));
    if(!TerrainMaterial) UE_LOG(LogTemp,Warning,TEXT("DynamicTerrain: run scripts/create_material.py in Unreal Editor to create the default terrain material."));
    HideFallbackSurfaces(false);
    const FString Cache=GetTerrainCacheDirectory();FTCHARToUTF8 C(*Cache),I(*ImageryURL),E(*ElevationURL);
    xov_terrain_config Config{Latitude,Longitude,Altitude,RadiusM,Zoom,CellsPerTile,C.Get(),I.Get(),E.Get()};
    const xov_terrain_quality Quality{TextureSize,ImageryDetailZoom,ImageryDetailRadiusM};
    Runtime=xov_terrain_create_with_coverage(&Config,&Quality,OuterVisualRadiusM);
    bPreviewInEditor=GetWorld()&&!GetWorld()->IsGameWorld();
    State->Cancel=MakeShared<std::atomic<bool>,ESPMode::ThreadSafe>(false);
    if(!Runtime) UE_LOG(LogTemp,Error,TEXT("DynamicTerrain configuration is invalid."));
}
void ADynamicTerrainActor::StopTerrain() {
    if(State) State->bExplicitlyStopped=true;
    ReleaseTerrainResources();
}
void ADynamicTerrainActor::ReleaseTerrainResources() {
    bPreviewInEditor=false;
    if(State) {
        State->Cancel->store(true);
        if(State->Preparing.IsValid()) {State->Preparing.Wait();State->Preparing.Consume();}
        State->Pending.Reset();
        // AsyncPool queued work cannot Abandon: drain the future before joining
        // its worker, then use a fresh pool if terrain is restarted.
        State->PreparationPool.Reset();State->NextPoolAttempt=0;State->bPoolFailureLogged=false;
        State->bCooking=false;State->SectionKeys.Empty();State->PageImageIdentities.Empty();State->ActiveMaterialPhysics.Empty();State->LastUpdate=-1;
        State->ActiveCollision.Reset();State->StagingCollision.Reset();
    }
    xov_terrain_destroy(Runtime);Runtime=nullptr;
    for(const auto& S:FallbackStates) if(auto* C=S.Component.Get()) {C->SetVisibility(S.bVisible);C->SetCollisionEnabled(S.Collision);}
    FallbackStates.Empty();
    // Clear old cooks as well: a late Chaos callback must never reactivate collision.
    for(auto* C:{CollisionA.Get(),CollisionB.Get()}) if(IsValid(C)) {
        C->ResetCollision();
    }
    if(IsValid(Terrain)) {Terrain->ClearAllMeshSections();Terrain->EmptyOverrideMaterials();}
    for(const auto& Pair:PageTextures) RetireTexture(Pair.Value.Get());
    Textures.Empty();PageTextures.Empty();PageMaterials.Empty();PageImageryZoom.Empty();
    // A stopped preview may no longer receive Actor ticks. The weak core ticker
    // finishes fenced retirement without restarting terrain or forcing GC.
    const auto RetiredCount=Diagnostics.RetiredTextures,RetiredBytes=Diagnostics.RetiredTextureBytes;
    const auto PeakBytes=Diagnostics.PeakRetiredTextureBytes;
    const auto ReleasedCount=Diagnostics.ReleasedTextures,ReleasedBytes=Diagnostics.ReleasedTextureBytes;
    Diagnostics={};Diagnostics.RetiredTextures=RetiredCount;Diagnostics.RetiredTextureBytes=RetiredBytes;
    Diagnostics.PeakRetiredTextureBytes=PeakBytes;Diagnostics.ReleasedTextures=ReleasedCount;Diagnostics.ReleasedTextureBytes=ReleasedBytes;
    LoadedPages=0;TextureUpdates=0;HighestImageryZoom=0;ActiveGeneration=0;ActiveCoverageRevision=0;CollisionCooks=0;
}
void ADynamicTerrainActor::Tick(float Dt) {
    Super::Tick(Dt);
    ProcessRetiredTextures(GFrameCounter);
    if(!bStreamTerrain) {
        // Observing disabled streaming arms the next enable, without changing
        // the authored setting when the public Stop control is used.
        State->bExplicitlyStopped=false;
        if(Runtime) ReleaseTerrainResources();
        return;
    }
    if(GetWorld()&&!GetWorld()->IsGameWorld()) {
        if(!bPreviewInEditor) {if(Runtime) ReleaseTerrainResources();return;}
        // Stop clears preview; explicitly enabling its checkbox resumes it.
        State->bExplicitlyStopped=false;
    }
    if(State->bExplicitlyStopped) return;
    if(!Runtime) RefreshTerrain();
    if(!Runtime) return;
    SynchronizeCollisionSettings();
    const double Now=FPlatformTime::Seconds();
    if(Now-State->LastUpdate>=.2) {
        State->LastUpdate=Now;
        const FVector P=FollowActor?FollowActor->GetActorLocation():GetActorLocation();
        xov_terrain_update(Runtime,P.X*.01,P.Y*.01,P.Z*.01);
        UpdateDiagnostics();
    }
    if(State->Preparing.IsValid()&&State->Preparing.IsReady()) {
        State->Pending=State->Preparing.Consume();State->CoverageUploadIndex=0;
    }
    const bool bCleanupPressure=Diagnostics.RetiredTextureBytes>RetiredTextureBudget||
        RetiredTextures.Num()>RetiredTextureCountLimit;
    if(bCleanupPressure) ++Diagnostics.CleanupBackpressureFrames;
    if(State->Pending&&!State->bCooking&&!bCleanupPressure) ApplyPrepared();
    if(State->bCooking) CommitPrepared();
    if(!State->Preparing.IsValid()&&!State->Pending&&!bCleanupPressure) {
        if(!State->PreparationPool) {
            if(Now<State->NextPoolAttempt) return;
            State->NextPoolAttempt=Now+1.0;
            TUniquePtr<FQueuedThreadPool> Pool(FQueuedThreadPool::Allocate());
            if(!Pool||!Pool->Create(1,0,FPlatformAffinity::GetTaskBPThreadPriority(),TEXT("DynamicTerrainPrepare"))) {
                if(!State->bPoolFailureLogged) UE_LOG(LogTemp,Error,TEXT("DynamicTerrain preparation worker unavailable; retaining active terrain."));
                State->bPoolFailureLogged=true;return;
            }
            State->PreparationPool=MoveTemp(Pool);State->bPoolFailureLogged=false;
        }
        if(auto* Frame=xov_terrain_acquire_frame(Runtime)) {
            const FTransform Transform=GetActorTransform();const auto Cancel=State->Cancel;
            State->Preparing=AsyncPool(*State->PreparationPool,[Frame,Transform,Cancel] {
                // Retain the scoped temporary-stack cleanup of Unreal's global
                // pool while keeping owned mesh/mip allocations on one worker.
                FMemMark Mark(FMemStack::Get());
                return PrepareFrame(Frame,Transform,Cancel);
            });
        }
    }
}
void ADynamicTerrainActor::RetireTexture(UTexture2D* Texture) {
    if(!IsValid(Texture)||RetiredTextures.Contains(Texture)) return;
    uint64 Bytes=0;
    if(const auto* Data=Texture->GetPlatformData())
        for(const auto& Mip:Data->Mips) Bytes+=Mip.BulkData.GetBulkDataSize();
    RetiredTextures.Add(Texture);
    auto& Entry=State->Retired.AddDefaulted_GetRef();Entry.Frame=GFrameCounter;Entry.Bytes=Bytes;
    Diagnostics.RetiredTextures=RetiredTextures.Num();Diagnostics.RetiredTextureBytes+=Bytes;
    Diagnostics.PeakRetiredTextureBytes=FMath::Max(Diagnostics.PeakRetiredTextureBytes,Diagnostics.RetiredTextureBytes);
    if(!State->RetirementTicker.IsValid()&&!HasAnyFlags(RF_BeginDestroyed|RF_FinishDestroyed)) {
        const TWeakObjectPtr<ADynamicTerrainActor> WeakThis(this);
        State->RetirementTicker=FTSTicker::GetCoreTicker().AddTicker(FTickerDelegate::CreateLambda([WeakThis](float) {
            auto* Actor=WeakThis.Get();if(!Actor) return false;
            Actor->ProcessRetiredTextures(GFrameCounter);
            if(!Actor->RetiredTextures.IsEmpty()) return true;
            Actor->State->RetirementTicker.Reset();return false;
        }));
    }
}
void ADynamicTerrainActor::ProcessRetiredTextures(uint64 Frame) {
    if(!State||State->LastCleanupFrame==Frame) return;
    State->LastCleanupFrame=Frame;
    const double Started=FPlatformTime::Seconds();int32 Work=0;
    for(int32 i=State->Retired.Num()-1;i>=0&&Work<8;--i) {
        auto& Entry=State->Retired[i];auto* Texture=RetiredTextures[i].Get();
        // MarkRenderStateDirty is deferred until the end of the world tick.
        // A fence inserted in the retiring tick could precede the old proxy's
        // destruction. Insert it only in a later engine frame.
        if(Frame<=Entry.Frame) continue;
        if(!Entry.Fence) {
            Entry.Fence=MakeShared<FRenderCommandFence>();Entry.Fence->BeginFence();++Work;
        } else if(Entry.Fence->IsFenceComplete()) {
            if(!Entry.bResourceReleased) {
                if(Texture->HasPendingInitOrStreaming()) continue;
                Texture->ReleaseResource();
                Entry.bResourceReleased=true;Entry.Fence->BeginFence();++Work;
            } else {
                // FTexture2DResource holds a pointer to platform mip metadata.
                // Do not remove it until resource deletion has passed its fence.
                if(auto* Data=Texture->GetPlatformData()) Data->Mips.Empty();
                Diagnostics.RetiredTextureBytes-=Entry.Bytes;
                Diagnostics.ReleasedTextureBytes+=Entry.Bytes;++Diagnostics.ReleasedTextures;
                RetiredTextures.RemoveAtSwap(i);State->Retired.RemoveAtSwap(i);
                Diagnostics.RetiredTextures=RetiredTextures.Num();++Work;
            }
        }
        if(FPlatformTime::Seconds()-Started>=.002) break;
    }
}
void ADynamicTerrainActor::SynchronizeCollisionSettings() {
    // The public visual component retains the original authored settings API.
    // Forward runtime edits before physics without recreating bodies or cooks.
    const auto* Physical=Terrain->BodyInstance.GetPhysMaterialOverride();
    const auto ObjectType=Terrain->GetCollisionObjectType();
    const auto& Responses=Terrain->GetCollisionResponseToChannels();
    for(auto* Collision:{CollisionA.Get(),CollisionB.Get()}) if(IsValid(Collision)) {
        if(Collision->BodyInstance.GetPhysMaterialOverride()!=Physical)
            Collision->SetPhysMaterialOverride(Terrain->BodyInstance.GetPhysMaterialOverride());
        if(Collision->GetCollisionObjectType()!=ObjectType) Collision->SetCollisionObjectType(ObjectType);
        if(Collision->GetCollisionResponseToChannels()!=Responses) Collision->SetCollisionResponseToChannels(Responses);
    }
}
void ADynamicTerrainActor::UpdateDiagnostics() {
    xov_terrain_diagnostics Data{};
    if(xov_terrain_get_diagnostics(Runtime,&Data)!=0) return;
    Diagnostics.Queued=Data.queued;
    Diagnostics.Active=Data.active;
    Diagnostics.PeakQueued=Data.peak_queued;
    Diagnostics.PeakActive=Data.peak_active;
    Diagnostics.BufferedBytes=Data.buffered_bytes;
    Diagnostics.CacheHits=Data.cache_hits;
    Diagnostics.Downloaded=Data.downloaded;
    Diagnostics.Failed=Data.failed;
    Diagnostics.Cancelled=Data.cancelled;
    Diagnostics.Bytes=Data.bytes;
    Diagnostics.Connections=Data.connections;
    Diagnostics.Retries=Data.retries;
    Diagnostics.DecodedDemBytes=Data.decoded_dem_bytes;
    Diagnostics.DecodedDemEntries=Data.decoded_dem_entries;
    Diagnostics.DecodedImageryBytes=Data.decoded_imagery_bytes;
    Diagnostics.DecodedImageryEntries=Data.decoded_imagery_entries;
    Diagnostics.PageCacheBytes=Data.page_cache_bytes;
    Diagnostics.PageCacheEntries=Data.page_cache_entries;
    Diagnostics.OuterMeshBytes=Data.outer_mesh_bytes;
    Diagnostics.OuterMeshEntries=Data.outer_mesh_entries;
    Diagnostics.OuterMeshLimitBytes=Data.outer_mesh_limit_bytes;
    Diagnostics.OuterMeshHits=Data.outer_mesh_hits;
    Diagnostics.OuterMeshMisses=Data.outer_mesh_misses;
    Diagnostics.PendingFrames=Data.pending_frames;
    Diagnostics.PendingTexturePages=Data.pending_texture_pages;
    Diagnostics.PendingTextureBytes=Data.pending_texture_bytes;
    Diagnostics.PreparingFrames=State->Preparing.IsValid()?1:0;
    Diagnostics.StagedFrames=State->Pending?1:0;
    Diagnostics.ResidentTextures=PageTextures.Num();
}
void ADynamicTerrainActor::ApplyPrepared() {
    auto Batch=State->Pending;if(!Batch) return;
    if(Batch->bTextureOnly) {
        auto* Collision=State->ActiveCollision.Get();bool RefreshPhysicalMaterials=false;
        if(Batch->Generation==uint64(ActiveGeneration)) for(auto& Page:Batch->Pages) {
            const FIntVector* Key=State->SectionKeys.Find(Page.Section);
            if(Key&&*Key==Page.Key) {
                ApplyTexture(Page.Section,Page.Key,Page);++TextureUpdates;
                if(Collision) {
                    auto* Material=Terrain->GetMaterial(Page.Section);
                    auto* Physical=Material?Material->GetPhysicalMaterial():nullptr;
                    const bool MaterialChanged=Collision->GetMaterial(Page.Section)!=Material;
                    if(MaterialChanged) Collision->SetMaterial(Page.Section,Material);
                    // Reused MIDs can also inherit an edited physical material.
                    // SetMaterial updates Chaos only when its pointer changes.
                    if(!MaterialChanged&&State->ActiveMaterialPhysics.FindRef(Page.Section).Get()!=Physical)
                        RefreshPhysicalMaterials=true;
                    State->ActiveMaterialPhysics.Add(Page.Section,Physical);
                }
            }
        }
        if(RefreshPhysicalMaterials) Collision->BodyInstance.UpdatePhysicalMaterials();
        State->Pending.Reset();return;
    }
    if(Batch->CollisionVertices.IsEmpty()) {State->Pending.Reset();return;}
    // The first outer coverage revision only appends visual pages. Keep the
    // exact existing collision body and upload a bounded number each tick.
    if(Batch->Generation==uint64(ActiveGeneration)&&Batch->CoverageRevision>0) {
        const double Start=FPlatformTime::Seconds();int32 Uploaded=0;
        while(State->CoverageUploadIndex<Batch->Pages.Num()&&Uploaded<8) {
            auto& Page=Batch->Pages[State->CoverageUploadIndex++];
            const auto* Existing=State->SectionKeys.Find(Page.Section);
            if(!Existing||*Existing!=Page.Key) {
                Terrain->SetProcMeshSection(Page.Section,Page.Mesh);
                State->SectionKeys.Add(Page.Section,Page.Key);++LoadedPages;
            }
            ApplyTexture(Page.Section,Page.Key,Page);++Uploaded;
            if(FPlatformTime::Seconds()-Start>=.002) break;
        }
        if(State->CoverageUploadIndex==Batch->Pages.Num()) {
            ActiveCoverageRevision=Batch->CoverageRevision;
            xov_terrain_activate(Runtime,Batch->Generation);State->Pending.Reset();
        }
        return;
    }
    auto* Staging=State->ActiveCollision.Get()==CollisionA?CollisionB.Get():CollisionA.Get();
    Staging->SetCollisionEnabled(ECollisionEnabled::NoCollision);
    State->PreviousCook=Staging->ProcMeshBodySetup;
    Staging->TriangleMaterials=Batch->TriangleMaterials;
    Staging->EmptyOverrideMaterials();Staging->MaterialSlotCount=Batch->Pages.Num();
    for(const auto& Page:Batch->Pages) Staging->SetMaterial(Page.Section,TerrainMaterial);
    Staging->SetPhysMaterialOverride(Terrain->BodyInstance.GetPhysMaterialOverride());
    Staging->SetCollisionObjectType(Terrain->GetCollisionObjectType());
    Staging->SetCollisionResponseToChannels(Terrain->GetCollisionResponseToChannels());
    Staging->CreateMeshSection_LinearColor(0,Batch->CollisionVertices,Batch->CollisionIndices,{},Batch->CollisionUV,{}, {},true);
    State->StagingCollision=Staging;State->bCooking=true;State->CookStarted=FPlatformTime::Seconds();++CollisionCooks;
}
bool ADynamicTerrainActor::CommitPrepared() {
    auto* Staging=State->StagingCollision.Get();auto Batch=State->Pending;
    if(!Staging||!Batch) return false;
    if(FPlatformTime::Seconds()-State->CookStarted>60) {
        UE_LOG(LogTemp,Warning,TEXT("DynamicTerrain cook timed out; retaining active terrain."));
        Staging->ResetCollision();
        State->bCooking=false;State->Pending.Reset();State->StagingCollision.Reset();return false;
    }
    auto* Body=Staging->ProcMeshBodySetup.Get();
    if(!Body||!Body->bCreatedPhysicsMeshes||Body->bFailedToCreatePhysicsMeshes) return false;
    if(GetWorld()->IsGameWorld()&&Body==State->PreviousCook.Get()) return false;
    const double Start=FPlatformTime::Seconds();
    // No physics simulation occurs inside this pre-physics game-thread transaction.
    Staging->SetCollisionEnabled(ECollisionEnabled::QueryAndPhysics);
    FHitResult Hit;const FVector P=FollowActor?FollowActor->GetActorLocation():GetActorLocation();
    const auto Bounds=Staging->Bounds.GetBox();
    if(!Staging->LineTraceComponent(Hit,FVector(P.X,P.Y,Bounds.Max.Z+100),FVector(P.X,P.Y,Bounds.Min.Z-100),
            FCollisionQueryParams(SCENE_QUERY_STAT(TerrainCommit),true))) {
        Staging->SetCollisionEnabled(ECollisionEnabled::NoCollision);
        if(P.X<Bounds.Min.X||P.X>Bounds.Max.X||P.Y<Bounds.Min.Y||P.Y>Bounds.Max.Y) {
            Staging->ResetCollision();
            State->bCooking=false;State->Pending.Reset();State->StagingCollision.Reset();
        }
        return false;
    }
    Terrain->ClearAllMeshSections();Terrain->EmptyOverrideMaterials();
    Staging->EmptyOverrideMaterials();State->SectionKeys.Empty();State->ActiveMaterialPhysics.Empty();LoadedPages=0;
    // Reserve every section once. The old render proxy owns independent
    // buffers until this game-thread transaction finishes. Move the prepared
    // arrays through the component's public section API, then finalize bounds,
    // collision bookkeeping and render state once with the last section.
    const int32 LastSection=Batch->Pages.Last().Section;
    Terrain->SetProcMeshSection(LastSection,FProcMeshSection());
    TSet<FIntVector> Live;
    for(auto& Page:Batch->Pages) {
        if(Page.Section==LastSection) Terrain->SetProcMeshSection(Page.Section,Page.Mesh);
        else *Terrain->GetProcMeshSection(Page.Section)=MoveTemp(Page.Mesh);
        State->SectionKeys.Add(Page.Section,Page.Key);Live.Add(Page.Key);
        ApplyTexture(Page.Section,Page.Key,Page);auto* Material=Terrain->GetMaterial(Page.Section);
        Staging->SetMaterial(Page.Section,Material);
        State->ActiveMaterialPhysics.Add(Page.Section,Material?Material->GetPhysicalMaterial():nullptr);++LoadedPages;
    }
    for(auto It=PageTextures.CreateIterator();It;++It) if(!Live.Contains(It.Key())) {RetireTexture(It.Value().Get());It.RemoveCurrent();}
    for(auto It=PageMaterials.CreateIterator();It;++It) if(!Live.Contains(It.Key())) It.RemoveCurrent();
    for(auto It=PageImageryZoom.CreateIterator();It;++It) if(!Live.Contains(It.Key())) It.RemoveCurrent();
    for(auto It=State->PageImageIdentities.CreateIterator();It;++It) if(!Live.Contains(It.Key())) It.RemoveCurrent();
    // The replacement is cooked and its ground trace has succeeded. The old
    // component no longer needs to retain triangles, Chaos data or tile MIDs.
    if(auto* Old=State->ActiveCollision.Get()) Old->ResetCollision();
    State->ActiveCollision=Staging;State->StagingCollision.Reset();State->bCooking=false;
    ActiveGeneration=Batch->Generation;xov_terrain_activate(Runtime,Batch->Generation);
    ActiveCoverageRevision=Batch->CoverageRevision;
    HideFallbackSurfaces(true);State->Pending.Reset();
    LastCommitMilliseconds=(FPlatformTime::Seconds()-Start)*1000;
    UE_LOG(LogTemp,Display,TEXT("DynamicTerrain generation=%lld pages=%d cooks=%d commit_ms=%.3f textures=%d"),ActiveGeneration,LoadedPages,CollisionCooks,LastCommitMilliseconds,PageTextures.Num());
    return true;
}
void ADynamicTerrainActor::ApplyTexture(int32 Section,const FIntVector& Key,FPreparedPage& Page) {
    if(Page.Mips.IsEmpty()||!TerrainMaterial) return;
    UTexture2D* Texture=PageTextures.FindOrAdd(Key).Get();
    // An outer snapshot can finish preparation after newer near refinement.
    // Keep the finer resident pixels, but still bind them to a new section.
    const bool bKeepResident=Texture&&PageImageryZoom.FindRef(Key)>Page.ImageryZoom;
    if(!bKeepResident) {
    const auto& Top=Page.Mips[0];
    const bool bReuse=Texture&&Texture->GetPlatformData()&&Texture->GetSizeX()==Top.Width&&Texture->GetSizeY()==Top.Height&&Texture->GetPlatformData()->Mips.Num()==Page.Mips.Num();
    const auto ResidentIdentity=State->PageImageIdentities.FindRef(Key);
    const bool bSameImage=bReuse&&Texture->GetResource()&&Page.ImageIdentity&&ResidentIdentity&&
        xov_terrain_image_identity_equal(Page.ImageIdentity->Value,ResidentIdentity->Value);
    if(bSameImage) {
        // Cached pages share an immutable native image. Rebinding their mesh or
        // material needs no CPU copy or Vulkan upload. Keep the complete mips
        // prepared above as a fallback for a missing/recreated texture.
        ++Diagnostics.ReusedImageryPages;
        for(const auto& Mip:Page.Mips) Diagnostics.SkippedTextureUploadBytes+=Mip.Bytes.Num();
    } else {
    uint64 UploadBytes=0;
    for(const auto& Mip:Page.Mips) UploadBytes+=Mip.Bytes.Num();
    if(!bReuse) {
        UTexture2D* PreviousTexture=Texture;
        Texture=UTexture2D::CreateTransient(Top.Width,Top.Height,PF_B8G8R8A8);
        if(!Texture) return;
        Texture->SRGB=true;Texture->NeverStream=true;Texture->Filter=TF_Trilinear;
        Texture->AddressX=TA_Clamp;Texture->AddressY=TA_Clamp;
#if WITH_EDITORONLY_DATA
        Texture->MipGenSettings=TMGS_LeaveExistingMips;
#endif
        for(int32 i=0;i<Page.Mips.Num();++i) {
            const auto& Source=Page.Mips[i];auto& Mips=Texture->GetPlatformData()->Mips;
            if(i>0) Mips.Add(new FTexture2DMipMap());auto& Mip=Mips[i];
            Mip.SizeX=Source.Width;Mip.SizeY=Source.Height;Mip.SizeZ=1;
            Mip.BulkData.Lock(LOCK_READ_WRITE);void* Dest=Mip.BulkData.Realloc(Source.Bytes.Num());
            FMemory::Memcpy(Dest,Source.Bytes.GetData(),Source.Bytes.Num());Mip.BulkData.Unlock();
        }
        Texture->UpdateResource();PageTextures.Add(Key,Texture);
        RetireTexture(PreviousTexture);
    } else {
        // Keep retained CPU mip data coherent for device/resource recreation.
        for(int32 i=0;i<Page.Mips.Num();++i) {
            auto& Bulk=Texture->GetPlatformData()->Mips[i].BulkData;
            void* Dest=Bulk.Lock(LOCK_READ_WRITE);
            FMemory::Memcpy(Dest,Page.Mips[i].Bytes.GetData(),Page.Mips[i].Bytes.Num());Bulk.Unlock();
        }
        // One render command owns every mip so no frame sees a partial mip chain.
        auto* Resource=static_cast<FTexture2DResource*>(Texture->GetResource());
        if(Resource) {
            auto Mips=MoveTemp(Page.Mips);
            ENQUEUE_RENDER_COMMAND(DynamicTerrainUpdateMips)([Resource,Mips=MoveTemp(Mips)](FRHICommandListImmediate& RHICmdList) {
                for(int32 i=0;i<Mips.Num();++i) {
                    const auto& Mip=Mips[i];FUpdateTextureRegion2D Region(0,0,0,0,Mip.Width,Mip.Height);
                    RHICmdList.UpdateTexture2D(Resource->GetTexture2DRHI(),i,Region,Mip.Width*4,Mip.Bytes.GetData());
                }
            });
        } else Texture->UpdateResource();
    }
    ++Diagnostics.TextureUploadPages;Diagnostics.TextureUploadBytes+=UploadBytes;
    }
    if(Page.ImageIdentity) State->PageImageIdentities.Add(Key,Page.ImageIdentity);
    else State->PageImageIdentities.Remove(Key);
    PageImageryZoom.Add(Key,Page.ImageryZoom);
    }
    auto& Material=PageMaterials.FindOrAdd(Key);
    if(!Material||Material->Parent!=TerrainMaterial) Material=UMaterialInstanceDynamic::Create(TerrainMaterial,this);
    Material->SetTextureParameterValue(TEXT("TerrainTexture"),Texture);Terrain->SetMaterial(Section,Material);
    HighestImageryZoom=FMath::Max(HighestImageryZoom,Page.ImageryZoom);
}
bool ADynamicTerrainActor::TraceGround(const FVector& P,FHitResult& Hit) const {
    auto* C=State?State->ActiveCollision.Get():nullptr;
    if(!GetActorEnableCollision()||!ActiveGeneration||!IsValid(C)||!C->IsPhysicsCollisionEnabled()||!C->IsQueryCollisionEnabled()
       ||C->GetCollisionResponseToChannel(ECC_PhysicsBody)!=ECR_Block) return false;
    const auto B=C->Bounds.GetBox();
    return C->LineTraceComponent(Hit,FVector(P.X,P.Y,B.Max.Z+100),FVector(P.X,P.Y,B.Min.Z-100),FCollisionQueryParams(SCENE_QUERY_STAT(SpawnTerrain),true));
}
void ADynamicTerrainActor::EndPlay(EEndPlayReason::Type R) {StopTerrain();Super::EndPlay(R);}
void ADynamicTerrainActor::Destroyed() {StopTerrain();Super::Destroyed();}
void ADynamicTerrainActor::BeginDestroy() {
    StopTerrain();
    if(State&&State->RetirementTicker.IsValid()) {
        FTSTicker::GetCoreTicker().RemoveTicker(State->RetirementTicker);State->RetirementTicker.Reset();
    }
    // Engine destruction fences handle any resources still awaiting cleanup.
    Super::BeginDestroy();
}

#if WITH_DEV_AUTOMATION_TESTS
#include "Misc/AutomationTest.h"
#include "Misc/SecureHash.h"
#include "HAL/FileManager.h"
#include "UObject/GarbageCollection.h"
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FTerrainImageryIdentity,"Xovium.DynamicTerrain.ImageryIdentity",EAutomationTestFlags::EditorContext|EAutomationTestFlags::EngineFilter)
bool FTerrainImageryIdentity::RunTest(const FString&) {
    FString FixtureURL;
    if(!FParse::Value(FCommandLine::Get(),TEXT("TerrainIdentityFixtureURL="),FixtureURL)||
       !FixtureURL.StartsWith(TEXT("http://127.0.0.1:"))) {
        AddError(TEXT("ImageryIdentity requires the loopback fixture supplied by xov-tests/unreal-memory-cleanup/run_actor_automation.py"));
        return false;
    }
    const FString Cache=FPaths::CreateTempFilename(*FPaths::ProjectSavedDir(),TEXT("TerrainIdentity"),TEXT(""));
    struct FCacheCleanup {FString Path;~FCacheCleanup(){IFileManager::Get().DeleteDirectory(*Path,false,true);}} CacheCleanup{Cache};
    struct FNativeOwner {void* Value=nullptr;~FNativeOwner(){xov_terrain_destroy(Value);}} NativeA,NativeB;
    const auto Cancel=MakeShared<std::atomic<bool>,ESPMode::ThreadSafe>(false);
    FImageIdentityPtr SeparateIdentity;
    auto Fixture=[&](const TCHAR* Name,FNativeOwner& Owner,bool KeepSeparateIdentity) -> FPreparedPtr {
        const FString FixtureCache=Cache/TEXT("cache-")+Name;
        const FString Imagery=FixtureURL/TEXT("identity-")+Name+TEXT("/{z}/{x}/{y}.png");
        const FString Elevation=FixtureURL/TEXT("elevation/{z}/{x}/{y}.png");
        FTCHARToUTF8 C(*FixtureCache),I(*Imagery),E(*Elevation);
        const xov_terrain_config Config{35.7162542,-120.7615102,300,100,15,8,C.Get(),I.Get(),E.Get()};
        const xov_terrain_quality Quality{256,15,100};
        Owner.Value=xov_terrain_create_with_quality(&Config,&Quality);
        if(!Owner.Value){AddError(TEXT("Could not create native identity fixture runtime"));return {};}
        xov_terrain_update(Owner.Value,0,0,0);
        const double Deadline=FPlatformTime::Seconds()+20;
        while(FPlatformTime::Seconds()<Deadline) {
            if(auto* Frame=xov_terrain_acquire_frame(Owner.Value)) {
                if(KeepSeparateIdentity) {
                    if(auto* Identity=xov_terrain_frame_acquire_image_identity(Frame,0))
                        SeparateIdentity=MakeShared<FNativeImageIdentity,ESPMode::ThreadSafe>(Identity);
                }
                return PrepareFrame(Frame,FTransform::Identity,Cancel);
            }
            FPlatformProcess::Sleep(.005f);
        }
        AddError(TEXT("Timed out waiting for local native image fixture"));return {};
    };
    auto BatchA=Fixture(TEXT("a"),NativeA,true);
    auto BatchB=Fixture(TEXT("b"),NativeB,false);
    if(!TestTrue(TEXT("Both native fixtures returned pages"),BatchA&&BatchB&&!BatchA->Pages.IsEmpty()&&!BatchB->Pages.IsEmpty())) return false;
    const auto& SourceA=BatchA->Pages[0];const auto& SourceB=BatchB->Pages[0];
    if(!TestTrue(TEXT("Native image identities acquired"),SourceA.ImageIdentity&&SourceB.ImageIdentity&&SeparateIdentity)) return false;
    TestTrue(TEXT("Separate tokens describe one native owner"),xov_terrain_image_identity_equal(SourceA.ImageIdentity->Value,SeparateIdentity->Value)!=0);
    TestFalse(TEXT("Independent providers have distinct native owners"),xov_terrain_image_identity_equal(SourceA.ImageIdentity->Value,SourceB.ImageIdentity->Value)!=0);
    TestEqual(TEXT("Replacement uses same tile"),SourceA.Key,SourceB.Key);
    TestEqual(TEXT("Replacement uses same imagery zoom"),SourceA.ImageryZoom,SourceB.ImageryZoom);
    auto PreparedHashes=[](const FPreparedPage& Page) {
        TArray<FString> Hashes;for(const auto& Mip:Page.Mips) Hashes.Add(FMD5::HashBytes(Mip.Bytes.GetData(),Mip.Bytes.Num()));return Hashes;
    };
    const auto ExpectedA=PreparedHashes(SourceA),ExpectedB=PreparedHashes(SourceB);
    if(!TestTrue(TEXT("Real decoded fixture pixels differ"),!ExpectedA.IsEmpty()&&ExpectedA!=ExpectedB)) return false;
    auto* Actor=NewObject<ADynamicTerrainActor>();
    struct FActorCleanup {ADynamicTerrainActor* Value;~FActorCleanup(){Value->StopTerrain();FlushRenderingCommands();}} Cleanup{Actor};
    Actor->TerrainMaterial=LoadObject<UMaterialInterface>(nullptr,TEXT("/DynamicTerrain/M_Terrain.M_Terrain"));
    if(!TestNotNull(TEXT("Default material"),Actor->TerrainMaterial.Get())) return false;
    const FIntVector Key=SourceA.Key;
    auto CheckPixels=[&](const TArray<FString>& Expected) {
        auto* Texture=Actor->PageTextures.FindRef(Key).Get();
        if(!TestNotNull(TEXT("Resident texture"),Texture)||!TestNotNull(TEXT("Resident texture data"),Texture->GetPlatformData())) return;
        TestEqual(TEXT("Full mip chain remains resident"),Texture->GetPlatformData()->Mips.Num(),Expected.Num());
        int32 Index=0;for(auto& Mip:Texture->GetPlatformData()->Mips) {
            const auto* Bytes=static_cast<const uint8*>(Mip.BulkData.LockReadOnly());
            const FString Actual=FMD5::HashBytes(Bytes,Mip.BulkData.GetBulkDataSize());Mip.BulkData.Unlock();
            if(Expected.IsValidIndex(Index)) TestEqual(FString::Printf(TEXT("Mip %d exact pixels"),Index),Actual,Expected[Index]);++Index;
        }
    };
    auto Initial=SourceA;Actor->ApplyTexture(0,Key,Initial);FlushRenderingCommands();
    auto* Texture=Actor->PageTextures.FindRef(Key).Get();
    if(!TestNotNull(TEXT("Initial native image texture"),Texture)) return false;
    CheckPixels(ExpectedA);
    TestEqual(TEXT("Initial upload counted once"),Actor->Diagnostics.TextureUploadPages,int64(1));
    const int64 InitialBytes=Actor->Diagnostics.TextureUploadBytes;
    auto* Material=Actor->PageMaterials.FindRef(Key).Get();Actor->Terrain->EmptyOverrideMaterials();
    auto Same=SourceA;Same.ImageIdentity=SeparateIdentity;Actor->ApplyTexture(7,Key,Same);FlushRenderingCommands();
    TestEqual(TEXT("Same-owner CPU/RHI upload skipped"),Actor->Diagnostics.TextureUploadPages,int64(1));
    TestEqual(TEXT("Skipped upload adds no bytes"),Actor->Diagnostics.TextureUploadBytes,InitialBytes);
    TestEqual(TEXT("Same-owner reuse counted"),Actor->Diagnostics.ReusedImageryPages,int64(1));
    TestTrue(TEXT("Same texture and material rebound"),Actor->PageTextures.FindRef(Key)==Texture&&Actor->Terrain->GetMaterial(7)==Material);CheckPixels(ExpectedA);
    auto* Parent=LoadObject<UMaterialInterface>(nullptr,TEXT("/Engine/EngineMaterials/WorldGridMaterial.WorldGridMaterial"));
    if(!TestNotNull(TEXT("Replacement parent material"),Parent)) return false;
    Actor->TerrainMaterial=Parent;auto Reparent=SourceA;Actor->ApplyTexture(7,Key,Reparent);
    TestTrue(TEXT("Identity reuse still updates material parent"),Actor->PageMaterials.FindRef(Key)->Parent==Parent&&Actor->Terrain->GetMaterial(7)==Actor->PageMaterials.FindRef(Key));
    TestEqual(TEXT("Material change uploads no imagery"),Actor->Diagnostics.TextureUploadPages,int64(1));
    auto Different=SourceB;Actor->ApplyTexture(7,Key,Different);FlushRenderingCommands();
    TestEqual(TEXT("Different owner at same zoom is uploaded"),Actor->Diagnostics.TextureUploadPages,int64(2));
    TestTrue(TEXT("Same-size replacement reuses texture"),Actor->PageTextures.FindRef(Key)==Texture);CheckPixels(ExpectedB);
    Texture->ReleaseResource();FlushRenderingCommands();
    TestNull(TEXT("Fixture releases native rendering resource"),Texture->GetResource());
    auto MissingResource=SourceB;Actor->ApplyTexture(7,Key,MissingResource);FlushRenderingCommands();
    TestNotNull(TEXT("Same-owner missing resource recreated"),Texture->GetResource());
    TestEqual(TEXT("Missing resource takes upload fallback"),Actor->Diagnostics.TextureUploadPages,int64(3));CheckPixels(ExpectedB);
    Actor->PageTextures.Remove(Key);Actor->RetireTexture(Texture);
    auto MissingTexture=SourceB;Actor->ApplyTexture(7,Key,MissingTexture);FlushRenderingCommands();
    TestTrue(TEXT("Missing texture creates replacement despite remembered identity"),Actor->PageTextures.FindRef(Key)!=Texture);
    TestEqual(TEXT("Missing texture takes upload fallback"),Actor->Diagnostics.TextureUploadPages,int64(4));CheckPixels(ExpectedB);
    auto NoIdentity=SourceB;NoIdentity.ImageIdentity.Reset();Actor->ApplyTexture(7,Key,NoIdentity);FlushRenderingCommands();
    TestEqual(TEXT("Unknown identity conservatively uploads"),Actor->Diagnostics.TextureUploadPages,int64(5));
    TestEqual(TEXT("Unknown identity clears remembered token"),Actor->State->PageImageIdentities.Num(),0);CheckPixels(ExpectedB);
    auto Remember=SourceB;Actor->ApplyTexture(7,Key,Remember);FlushRenderingCommands();
    TestEqual(TEXT("Resident identity retained only per active key"),Actor->State->PageImageIdentities.Num(),1);
    Actor->StopTerrain();
    TestEqual(TEXT("Stop clears identity metadata"),Actor->State->PageImageIdentities.Num(),0);
    TestEqual(TEXT("Stop clears active texture owners"),Actor->PageTextures.Num(),0);
    TestEqual(TEXT("Stop clears active materials"),Actor->PageMaterials.Num(),0);
    return true;
}
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FTerrainStreamingControls,"Xovium.DynamicTerrain.StreamingControls",EAutomationTestFlags::EditorContext|EAutomationTestFlags::EngineFilter)
bool FTerrainStreamingControls::RunTest(const FString&) {
    if(!TestFalse(TEXT("Controls require the editor runtime, not a commandlet"),IsRunningCommandlet())) return false;
    const bool DefaultStreaming=GetDefault<ADynamicTerrainActor>()->bStreamTerrain;
    // Real native runtimes with isolated caches and loopback-only providers.
    // These tests inspect control lifetime, without waiting for any terrain
    // payload; the rendered lifecycle fixture covers outstanding preparation.
    const FString Cache=FPaths::CreateTempFilename(*FPaths::ProjectSavedDir(),TEXT("TerrainControls"),TEXT(""));
    for(const auto WorldType:{EWorldType::Game,EWorldType::EditorPreview}) {
        auto* World=UWorld::CreateWorld(WorldType,false);
        if(!TestNotNull(TEXT("Private control-test world"),World)) return false;
        struct FCleanup {
            UWorld* World;ADynamicTerrainActor* Actor=nullptr;FString Cache;
            ~FCleanup(){if(Actor) Actor->StopTerrain();World->DestroyWorld(false);IFileManager::Get().DeleteDirectory(*Cache,false,true);}
        } Cleanup{World,nullptr,Cache};
        auto* Actor=World->SpawnActor<ADynamicTerrainActor>();Cleanup.Actor=Actor;
        if(!TestNotNull(TEXT("Private control-test actor"),Actor)) return false;
        Actor->CacheDirectory.Path=Cache;Actor->RadiusM=100;Actor->CellsPerTile=8;
        Actor->TextureSize=256;Actor->ImageryDetailZoom=Actor->Zoom;
        Actor->ImageryURL=TEXT("http://127.0.0.1:1/control-imagery/{z}/{x}/{y}.png");
        Actor->ElevationURL=TEXT("http://127.0.0.1:1/control-elevation/{z}/{x}/{y}.png");
        auto Tick=[&]{Actor->Tick(1.f/60);};
        auto CheckStopped=[&] {
            TestNull(TEXT("Stopped actor has no native runtime"),Actor->Runtime);
            TestFalse(TEXT("Stopped actor is not ready"),Actor->IsTerrainReady());
            TestEqual(TEXT("Stopped actor has no pages"),Actor->LoadedPages,0);
            TestFalse(TEXT("Stopped actor owns no preparation future"),Actor->State->Preparing.IsValid());
            TestFalse(TEXT("Stopped actor owns no preparation worker"),Actor->State->PreparationPool.IsValid());
        };

        Actor->bStreamTerrain=false;Actor->RefreshTerrain();
        TestFalse(TEXT("Refresh preserves explicitly disabled streaming"),Actor->bStreamTerrain);
        TestFalse(TEXT("Disabled Refresh does not start editor preview"),Actor->bPreviewInEditor);
        for(int Step=0;Step<4;++Step){Tick();CheckStopped();}

        Actor->bStreamTerrain=true;
        if(WorldType==EWorldType::Game) Tick();else Actor->RefreshTerrain();
        TestNotNull(TEXT("Enabled game starts automatically; editor Refresh starts preview"),Actor->Runtime);
        Actor->StopTerrain();
        TestTrue(TEXT("Stop preserves the authored streaming flag"),Actor->bStreamTerrain);
        TestFalse(TEXT("Stop disables viewport-only ticks"),Actor->ShouldTickIfViewportsOnly());
        for(int Step=0;Step<8;++Step){Tick();CheckStopped();}

        Actor->RefreshTerrain();
        TestNotNull(TEXT("Explicit Refresh resumes stopped terrain"),Actor->Runtime);
        if(WorldType!=EWorldType::Game) TestTrue(TEXT("Editor Refresh enables viewport ticks"),Actor->ShouldTickIfViewportsOnly());
        Actor->StopTerrain();
        if(WorldType==EWorldType::Game) {
            Actor->bStreamTerrain=false;Tick();CheckStopped();
            Actor->bStreamTerrain=true;Tick();
            TestNotNull(TEXT("An observed disable/enable cycle resumes game streaming"),Actor->Runtime);
        } else {
            Actor->bPreviewInEditor=true;Tick();
            TestNotNull(TEXT("Explicit preview checkbox resumes stopped editor terrain"),Actor->Runtime);
        }
        Actor->bStreamTerrain=false;Tick();CheckStopped();
        TestFalse(TEXT("Automatic cleanup preserves disabled streaming"),Actor->bStreamTerrain);
    }
    TestEqual(TEXT("Controls leave class defaults unchanged"),GetDefault<ADynamicTerrainActor>()->bStreamTerrain,DefaultStreaming);
    return true;
}
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FTerrainTextureRetirement,"Xovium.DynamicTerrain.TextureRetirement",EAutomationTestFlags::EditorContext|EAutomationTestFlags::EngineFilter)
bool FTerrainTextureRetirement::RunTest(const FString&) {
    auto* Actor=NewObject<ADynamicTerrainActor>();
    Actor->TerrainMaterial=LoadObject<UMaterialInterface>(nullptr,TEXT("/DynamicTerrain/M_Terrain.M_Terrain"));
    if(!TestNotNull(TEXT("Default terrain material"),Actor->TerrainMaterial.Get())) return false;
    const FIntVector Key(12,23,15);
    const auto Cancel=MakeShared<std::atomic<bool>,ESPMode::ThreadSafe>(false);
    auto Page=[&](int Size,int Zoom,uint8 Color) {
        FPreparedPage Result;Result.ImageryZoom=Zoom;TArray<uint8> RGB;RGB.Init(Color,Size*Size*3);
        PrepareMipChain(RGB.GetData(),Size,Size,Cancel,Result.Mips);return Result;
    };
    auto Initial=Page(256,15,70);Actor->ApplyTexture(0,Key,Initial);
    auto* Retiring=Actor->PageTextures.FindRef(Key).Get();
    auto Refined=Page(512,16,190);Actor->ApplyTexture(0,Key,Refined);
    auto* Current=Actor->PageTextures.FindRef(Key).Get();
    TestTrue(TEXT("Resolution replacement creates distinct texture"),Retiring!=Current);
    TestEqual(TEXT("Superseded texture queued"),Actor->RetiredTextures.Num(),1);
    TestTrue(TEXT("Retiring CPU mips still present before render fence"),!Retiring->GetPlatformData()->Mips.IsEmpty());
    Actor->ProcessRetiredTextures(GFrameCounter);
    TestFalse(TEXT("Retirement fence waits until a later engine frame"),Actor->State->Retired[0].Fence.IsValid());

    auto* Parent=LoadObject<UMaterialInterface>(nullptr,TEXT("/Engine/EngineMaterials/WorldGridMaterial.WorldGridMaterial"));
    if(!TestNotNull(TEXT("Alternate material parent"),Parent)) return false;
    Actor->TerrainMaterial=Parent;
    auto Rebind=Page(512,16,190);Actor->ApplyTexture(0,Key,Rebind);
    TestTrue(TEXT("Material parent replacement preserves active texture"),Actor->PageTextures.FindRef(Key)==Current);
    TestTrue(TEXT("New material parent is bound"),Actor->PageMaterials.FindRef(Key)->Parent==Parent);
    TArray<FString> Before;
    for(auto& Mip:Current->GetPlatformData()->Mips) {
        const auto* Data=static_cast<const uint8*>(Mip.BulkData.LockReadOnly());
        Before.Add(FMD5::HashBytes(Data,Mip.BulkData.GetBulkDataSize()));Mip.BulkData.Unlock();
    }
    // These unregistered test components have no scene proxy. Advancing the
    // injected cleanup frame exercises both fences without mutating engine time
    // or invoking GC. Rendered lifecycle fixtures cover registered components.
    const uint64 Frame=GFrameCounter;
    for(uint64 Step=1;Step<=4;++Step) {
        FlushRenderingCommands();Actor->ProcessRetiredTextures(Frame+Step);
    }
    TestTrue(TEXT("Retired UObject remains valid without GC"),IsValid(Retiring));
    TestTrue(TEXT("Retired GPU resource released"),Retiring->GetResource()==nullptr);
    TestTrue(TEXT("Retired CPU mip storage freed without GC"),Retiring->GetPlatformData()->Mips.IsEmpty());
    TestEqual(TEXT("Retirement queue drained"),Actor->RetiredTextures.Num(),0);
    TestEqual(TEXT("Retirement byte counter drained"),Actor->Diagnostics.RetiredTextureBytes,int64(0));
    TestEqual(TEXT("Released texture counter"),Actor->Diagnostics.ReleasedTextures,int64(1));
    int32 Index=0;
    for(auto& Mip:Current->GetPlatformData()->Mips) {
        const auto* Data=static_cast<const uint8*>(Mip.BulkData.LockReadOnly());
        TestEqual(TEXT("Active mip unchanged during retirement"),FMD5::HashBytes(Data,Mip.BulkData.GetBulkDataSize()),Before[Index++]);Mip.BulkData.Unlock();
    }
    TestEqual(TEXT("Entire active mip chain preserved"),Index,Before.Num());
    Actor->StopTerrain();
    TestEqual(TEXT("Stop queues active texture"),Actor->RetiredTextures.Num(),1);
    Actor->ProcessRetiredTextures(Frame+5);
    TestTrue(TEXT("Stop has outstanding render fence"),Actor->State->Retired[0].Fence.IsValid());
    const TWeakObjectPtr<UTexture2D> WeakTexture(Current);
    Actor->ConditionalBeginDestroy();
    TestFalse(TEXT("Destroy removes weak cleanup ticker"),Actor->State->RetirementTicker.IsValid());
    TestTrue(TEXT("Destroy delegates still-pending mip cleanup to UTexture lifecycle"),!Current->GetPlatformData()->Mips.IsEmpty());
    // Destruction only: exercise Unreal's own fences/GC after cancelling the
    // actor ticker. No collection is used in the preceding retirement proof.
    FlushRenderingCommands();CollectGarbage(RF_NoFlags,true);FlushRenderingCommands();
    TestFalse(TEXT("Pending retired texture safely collected after actor destruction"),WeakTexture.IsValid());
    return true;
}
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FTerrainResidentImagery,"Xovium.DynamicTerrain.ResidentImagery",EAutomationTestFlags::EditorContext|EAutomationTestFlags::EngineFilter)
bool FTerrainResidentImagery::RunTest(const FString&) {
    auto* Actor=NewObject<ADynamicTerrainActor>();
    Actor->TerrainMaterial=LoadObject<UMaterialInterface>(nullptr,TEXT("/DynamicTerrain/M_Terrain.M_Terrain"));
    if(!TestNotNull(TEXT("Default terrain material"),Actor->TerrainMaterial.Get())) return false;
    const FIntVector Key(12,23,15);
    const auto Cancel=MakeShared<std::atomic<bool>,ESPMode::ThreadSafe>(false);
    auto Page=[&](int Zoom,uint8 Color) {
        FPreparedPage Result;Result.ImageryZoom=Zoom;TArray<uint8> RGB;RGB.Init(Color,256*256*3);
        PrepareMipChain(RGB.GetData(),256,256,Cancel,Result.Mips);return Result;
    };
    auto Fine=Page(17,170);Actor->ApplyTexture(0,Key,Fine);
    auto* Texture=Actor->PageTextures.FindRef(Key).Get();
    TArray<FString> Before;
    for(auto& Mip:Texture->GetPlatformData()->Mips) {
        const auto* Data=static_cast<const uint8*>(Mip.BulkData.LockReadOnly());
        Before.Add(FMD5::HashBytes(Data,Mip.BulkData.GetBulkDataSize()));Mip.BulkData.Unlock();
    }
    auto Stale=Page(15,30);Actor->Terrain->EmptyOverrideMaterials();Actor->ApplyTexture(3,Key,Stale);
    TestTrue(TEXT("Texture object retained"),Texture==Actor->PageTextures.FindRef(Key));
    TestEqual(TEXT("Resident detail retained"),Actor->PageImageryZoom.FindRef(Key),17);
    TestNotNull(TEXT("Resident material rebound to replacement section"),Actor->Terrain->GetMaterial(3));
    int Index=0;
    for(auto& Mip:Texture->GetPlatformData()->Mips) {
        const auto* Data=static_cast<const uint8*>(Mip.BulkData.LockReadOnly());
        TestEqual(TEXT("Every resident mip unchanged"),FMD5::HashBytes(Data,Mip.BulkData.GetBulkDataSize()),Before[Index++]);Mip.BulkData.Unlock();
    }
    auto Newer=Page(18,220);Actor->ApplyTexture(3,Key,Newer);
    TestEqual(TEXT("New refinement accepted"),Actor->PageImageryZoom.FindRef(Key),18);
    FlushRenderingCommands();Actor->StopTerrain();
    TestEqual(TEXT("Residency cleared on stop"),Actor->PageImageryZoom.Num(),0);
    return true;
}
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FTerrainMipParity,"Xovium.DynamicTerrain.MipParity",EAutomationTestFlags::EditorContext|EAutomationTestFlags::EngineFilter)
bool FTerrainMipParity::RunTest(const FString&) {
    for(const int32 Size:{256,512,2048}) {
        TArray<uint8> RGB;RGB.SetNumUninitialized(Size*Size*3);
        for(int32 y=0;y<Size;++y) for(int32 x=0;x<Size;++x) {
            const int32 i=(y*Size+x)*3;RGB[i]=(x*17+y*3)%256;RGB[i+1]=(x*5+y*19)%256;RGB[i+2]=(x^y)%256;
        }
        const auto Cancel=MakeShared<std::atomic<bool>,ESPMode::ThreadSafe>(false);
        auto Future=Async(EAsyncExecution::ThreadPool,[RGB,Size,Cancel] {
            TArray<FPreparedMip> Mips;PrepareMipChain(RGB.GetData(),Size,Size,Cancel,Mips);return Mips;
        });
        auto Actual=Future.Consume();
        // Reference is the pre-optimization APX4Terrain::ApplyPage algorithm,
        // executed synchronously using this engine's actual sRGB FImage code.
        FImage Level(Size,Size,ERawImageFormat::BGRA8,EGammaSpace::sRGB);
        auto Pixels=Level.AsBGRA8();
        for(int32 i=0;i<Size*Size;++i) Pixels[i]=FColor(RGB[3*i],RGB[3*i+1],RGB[3*i+2],255);
        int32 Index=0;
        for(;;) {
            if(!Actual.IsValidIndex(Index)) {AddError(TEXT("Worker omitted a mip"));return false;}
            const auto& Mip=Actual[Index++];
            TestEqual(TEXT("Width"),Mip.Width,Level.SizeX);TestEqual(TEXT("Height"),Mip.Height,Level.SizeY);
            TestEqual(TEXT("Byte count"),int64(Mip.Bytes.Num()),int64(Level.RawData.Num()));
            const FString ExpectedHash=FMD5::HashBytes(Level.RawData.GetData(),Level.RawData.Num());
            const FString ActualHash=FMD5::HashBytes(Mip.Bytes.GetData(),Mip.Bytes.Num());
            TestEqual(TEXT("Complete mip byte hash"),ActualHash,ExpectedHash);
            AddInfo(FString::Printf(TEXT("size=%d mip=%d dimensions=%dx%d md5=%s"),Size,Index-1,Mip.Width,Mip.Height,*ActualHash));
            if(Level.SizeX==1&&Level.SizeY==1) break;
            FImage Next;
            FImageCore::ResizeImageAllocDest(Level,Next,FMath::Max(1,Level.SizeX/2),FMath::Max(1,Level.SizeY/2),FImageCore::EResizeImageFilter::Box);
            Level=MoveTemp(Next);
        }
        TestEqual(TEXT("Complete mip chain count"),Actual.Num(),Index);
    }
    return true;
}
#endif
