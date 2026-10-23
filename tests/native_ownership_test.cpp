// Observe weak ownership of the actual bridge implementation without adding
// test/debug entry points to the public C ABI.
#include "../native/src/terrain.cpp"
#include <cassert>
#include <iostream>

namespace {
struct Payload {
    std::shared_ptr<dt::TerrainSnapshot> snapshot;
    std::weak_ptr<const dt::TerrainSnapshot> weakSnapshot;
    std::weak_ptr<const dt::MeshData> weakMesh;
    std::weak_ptr<const dt::ImageData> weakImage;
};
Payload makePayload(uint64_t generation, uint64_t revision) {
    Payload result;
    result.snapshot=std::make_shared<dt::TerrainSnapshot>();
    result.snapshot->generation=generation;
    result.snapshot->coverageRevision=revision;
    result.snapshot->centerTile={123,456,14};
    auto mesh=std::make_shared<dt::MeshData>();
    mesh->positions={{1,2,3},{4,5,6},{7,8,9}};
    mesh->normals={{0,0,1},{0,0,1},{0,0,1}};
    mesh->texCoords={{0,0},{1,0},{0,1}};
    mesh->indices={0,1,2};
    auto image=std::make_shared<dt::ImageData>();
    image->width=2;image->height=2;image->rgb.assign(12,73);
    dt::TerrainPage page;
    page.key=result.snapshot->centerTile;page.mesh=mesh;page.texture=image;page.imageryZoom=14;
    result.snapshot->pages.push_back(std::move(page));
    result.weakSnapshot=result.snapshot;result.weakMesh=mesh;result.weakImage=image;
    return result;
}
void checkBorrowedFrame(xov_terrain_frame* frame,uint64_t generation,uint64_t revision) {
    xov_terrain_frame_info info{};
    assert(xov_terrain_frame_get_info(frame,&info)==0);
    assert(info.generation==generation&&info.coverage_revision==revision&&info.page_count==1);
    xov_terrain_frame_page page{};
    assert(xov_terrain_frame_get_page(frame,0,&page)==0);
    assert(page.page.vertices==3&&page.page.indices==3&&page.page.positions[8]==9);
    assert(page.page.rgb[11]==73&&page.page.triangles[2]==2);
}
void checkImageIdentity() {
    using Identity=std::unique_ptr<xov_terrain_image_identity,
        decltype(&xov_terrain_release_image_identity)>;
    auto acquire=[](const xov_terrain_frame* frame,size_t index) {
        return Identity(xov_terrain_frame_acquire_image_identity(frame,index),
                        xov_terrain_release_image_identity);
    };
    assert(!acquire(nullptr,0));
    assert(!xov_terrain_image_identity_equal(nullptr,nullptr));
    xov_terrain_release_image_identity(nullptr);
    auto original=makePayload(10,0);
    xov_terrain_frame first;first.snapshot=original.snapshot;
    auto token=acquire(&first,0);assert(token);
    assert(!acquire(&first,1)&&!acquire(&first,size_t(-1)));
    assert(!xov_terrain_image_identity_equal(token.get(),nullptr));
    auto same=acquire(&first,0);
    assert(xov_terrain_image_identity_equal(token.get(),same.get()));

    // The actual cache carry path shares one const image across generations.
    xov_terrain_frame carried;
    auto next=std::make_shared<dt::TerrainSnapshot>(*original.snapshot);
    next->generation=11;carried.snapshot=next;
    auto carriedToken=acquire(&carried,0);
    assert(xov_terrain_image_identity_equal(token.get(),carriedToken.get()));

    // Equal dimensions, zoom and every RGB byte are insufficient identity.
    auto replacement=makePayload(12,0);
    xov_terrain_frame different;different.snapshot=replacement.snapshot;
    assert(replacement.snapshot->pages[0].texture->rgb==original.snapshot->pages[0].texture->rgb);
    auto differentToken=acquire(&different,0);
    assert(!xov_terrain_image_identity_equal(token.get(),differentToken.get()));

    // Texture-only frames expose the same published owner through the same API.
    xov_terrain_frame refinement;
    dt::TexturePageUpdate update;update.texture=original.snapshot->pages[0].texture;
    refinement.textures.push_back(update);update.texture.reset();
    auto refinementToken=acquire(&refinement,0);
    assert(xov_terrain_image_identity_equal(token.get(),refinementToken.get()));
    assert(!acquire(&refinement,1));
    refinement.textures[0].texture.reset();assert(!acquire(&refinement,0));
    refinement.textures[0].texture=std::make_shared<dt::ImageData>();
    assert(!acquire(&refinement,0));
    refinement.textures[0].texture.reset();

    // No strong image owner is retained by any identity token.
    original.snapshot.reset();first.snapshot.reset();carried.snapshot.reset();next.reset();
    assert(original.weakImage.expired());
    assert(xov_terrain_image_identity_equal(token.get(),same.get()));
    assert(xov_terrain_image_identity_equal(token.get(),carriedToken.get()));
    assert(!xov_terrain_image_identity_equal(token.get(),differentToken.get()));

    // Defensive support for aliases: a shared control block alone is not enough.
    struct Pair {dt::ImageData first,second;};
    auto pair=std::make_shared<Pair>();
    for(auto* image:{&pair->first,&pair->second}) {
        image->width=1;image->height=1;image->rgb={1,2,3};
    }
    xov_terrain_frame aliases;
    dt::TexturePageUpdate a,b;
    a.texture=std::shared_ptr<const dt::ImageData>(pair,&pair->first);
    b.texture=std::shared_ptr<const dt::ImageData>(pair,&pair->second);
    aliases.textures={a,b};
    auto aliasA=acquire(&aliases,0),aliasB=acquire(&aliases,1);
    assert(!xov_terrain_image_identity_equal(aliasA.get(),aliasB.get()));

    // Same pointee address with a distinct owner also must not compare equal.
    dt::ImageData reused;reused.width=1;reused.height=1;reused.rgb={1,2,3};
    aliases.textures[0].texture=std::shared_ptr<const dt::ImageData>(&reused,[](const dt::ImageData*){});
    auto oldAddress=acquire(&aliases,0);
    aliases.textures[0].texture.reset();assert(oldAddress->owner.expired());
    aliases.textures[0].texture=std::shared_ptr<const dt::ImageData>(&reused,[](const dt::ImageData*){});
    auto newAddress=acquire(&aliases,0);
    assert(!xov_terrain_image_identity_equal(oldAddress.get(),newAddress.get()));
}
}
int main() {
    checkImageIdentity();
    xov_terrain_config cfg{35.7,-120.7,300,500,14,8,"/tmp/terrain-ownership-no-downloads","",""};
    xov_terrain_quality quality{256,14,500};
    auto terrain=std::make_unique<Terrain>(cfg,quality);

    // A frame released by the worker must remain staged until the host commits;
    // afterwards, an otherwise unreferenced payload must be released immediately.
    auto first=makePayload(7,0);
    terrain->QueueSnapshot(first.snapshot);first.snapshot.reset();
    auto* frame=xov_terrain_acquire_frame(terrain.get());assert(frame);
    checkBorrowedFrame(frame,7,0);xov_terrain_release_frame(frame);
    assert(!first.weakSnapshot.expired());
    assert(xov_terrain_activate(terrain.get(),8)==-1);
    assert(!first.weakSnapshot.expired());
    assert(xov_terrain_activate(terrain.get(),7)==0);
    assert(first.weakSnapshot.expired()&&first.weakMesh.expired()&&first.weakImage.expired());
    assert(terrain->ActiveGeneration()==7&&terrain->ActiveCoverageRevision()==0);
    assert(terrain->HasActiveTerrain()&&terrain->ActiveCenterTile()==dt::TileKey({123,456,14}));

    // A same-generation coverage append updates readiness metadata, while an
    // acquired frame still owns its exact payload across activation and teardown.
    auto expanded=makePayload(7,1);
    terrain->QueueSnapshot(expanded.snapshot);expanded.snapshot.reset();
    frame=xov_terrain_acquire_frame(terrain.get());assert(frame);
    auto* retainedIdentity=xov_terrain_frame_acquire_image_identity(frame,0);
    auto* retainedIdentityCopy=xov_terrain_frame_acquire_image_identity(frame,0);
    assert(retainedIdentity&&retainedIdentityCopy);
    assert(xov_terrain_activate(terrain.get(),7)==0);
    assert(terrain->ActiveCoverageRevision()==1&&!expanded.weakSnapshot.expired());
    checkBorrowedFrame(frame,7,1);
    terrain.reset();
    checkBorrowedFrame(frame,7,1);
    xov_terrain_release_frame(frame);
    assert(expanded.weakSnapshot.expired()&&expanded.weakMesh.expired()&&expanded.weakImage.expired());
    assert(xov_terrain_image_identity_equal(retainedIdentity,retainedIdentityCopy));
    xov_terrain_release_image_identity(retainedIdentity);
    xov_terrain_release_image_identity(retainedIdentityCopy);
    std::cout<<"Native activation and weak image identity release RGB payload; frame/runtime lifetimes and owner equality preserved\n";
}
