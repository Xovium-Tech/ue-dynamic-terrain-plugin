"""Unreal Python fixture: save with old APX4Terrain, verify with its new subclass.

Run with TERRAIN_COMPAT_MODE=save or verify and TERRAIN_COMPAT_REPORT=/path.json.
Copy Content/TerrainCompatibility.umap and Content/PM_TerrainCompatibility.uasset
from the old isolated project to the new compatibility project between runs.
"""
import json
import os
from pathlib import Path
import traceback
import unreal

MAP = '/Game/TerrainCompatibility'
PHYSICAL = '/Game/PM_TerrainCompatibility'
MATERIAL = '/Game/Worlds/M_Terrain.M_Terrain'
SETTINGS = {'latitude': 35.7162542, 'longitude': -120.7615102, 'altitude': 321.25,
            'radius_m': 1234.5, 'zoom': 15, 'cells_per_tile': 48, 'texture_size': 512,
            'imagery_detail_zoom': 16, 'imagery_detail_radius_m': 750.5,
            'stream_terrain': False, 'preview_in_editor': False}


def enum_value(enum, *names):
    for name in names:
        if hasattr(enum, name):
            return getattr(enum, name)
    raise ValueError(f'Enum value missing: {names}')


def run():
    mode = os.environ['TERRAIN_COMPAT_MODE']
    actors = unreal.get_editor_subsystem(unreal.EditorActorSubsystem)
    levels = unreal.get_editor_subsystem(unreal.LevelEditorSubsystem)
    dynamic = enum_value(unreal.CollisionChannel, 'WORLD_DYNAMIC', 'ECC_WORLD_DYNAMIC')
    visibility = enum_value(unreal.CollisionChannel, 'VISIBILITY', 'ECC_VISIBILITY')
    ignore = enum_value(unreal.CollisionResponseType, 'IGNORE', 'ECR_IGNORE')
    if mode == 'save':
        assert levels.new_level(MAP)
        actor = actors.spawn_actor_from_class(unreal.load_class(None, '/Script/PX4Unreal.PX4Terrain'), unreal.Vector())
        actor.set_actor_label('TerrainCompatibilityActor')
        for name, value in SETTINGS.items():
            actor.set_editor_property(name, value)
        follow = actors.spawn_actor_from_class(unreal.CameraActor, unreal.Vector(123, 456, 789))
        follow.set_actor_label('TerrainCompatibilityFollow')
        fallback = actors.spawn_actor_from_class(unreal.StaticMeshActor, unreal.Vector(0, 0, -100))
        fallback.set_actor_label('TerrainCompatibilityFallback')
        fallback.static_mesh_component.set_static_mesh(unreal.load_asset('/Engine/BasicShapes/Cube.Cube'))
        actor.set_editor_property('follow_actor', follow)
        actor.set_editor_property('fallback_ground', fallback.static_mesh_component)
        material = unreal.load_asset(MATERIAL)
        assert material
        actor.set_editor_property('terrain_material', material)
        component = actor.get_editor_property('terrain')
        component.set_material(0, material)
        component.set_collision_object_type(dynamic)
        component.set_collision_response_to_channel(visibility, ignore)
        physical = unreal.load_asset(PHYSICAL)
        if not physical:
            physical = unreal.AssetToolsHelpers.get_asset_tools().create_asset(
                'PM_TerrainCompatibility', '/Game', unreal.PhysicalMaterial, unreal.PhysicalMaterialFactoryNew())
        physical.set_editor_property('friction', 0.71)
        physical.set_editor_property('restitution', 0.23)
        component.set_phys_material_override(physical)
        actor.set_actor_enable_collision(False)
        assert unreal.EditorAssetLibrary.save_asset(PHYSICAL, False)
        world = unreal.get_editor_subsystem(unreal.UnrealEditorSubsystem).get_editor_world()
        assert unreal.EditorLoadingAndSavingUtils.save_map(world, MAP)
    elif mode == 'verify':
        assert levels.load_level(MAP)
    else:
        raise ValueError(mode)
    actor = next(a for a in actors.get_all_level_actors() if a.get_actor_label() == 'TerrainCompatibilityActor')
    assert actor.get_class().get_path_name() == '/Script/PX4Unreal.PX4Terrain'
    for name, expected in SETTINGS.items():
        actual = actor.get_editor_property(name)
        assert abs(actual - expected) < 1e-8 if isinstance(expected, float) else actual == expected, (name, actual, expected)
    component = actor.get_editor_property('terrain')
    assert actor.get_editor_property('root_component') == component
    assert component.get_name() == 'StreamedGround'
    assert component.get_owner() == actor
    assert actor.get_editor_property('follow_actor').get_actor_label() == 'TerrainCompatibilityFollow'
    assert actor.get_editor_property('fallback_ground').get_owner().get_actor_label() == 'TerrainCompatibilityFallback'
    assert actor.get_editor_property('terrain_material').get_path_name() == MATERIAL
    assert component.get_material(0).get_path_name() == MATERIAL
    assert component.get_collision_object_type() == dynamic
    assert component.get_collision_response_to_channel(visibility) == ignore
    physical = component.get_editor_property('body_instance').get_editor_property('phys_material_override')
    assert physical.get_path_name() == PHYSICAL + '.PM_TerrainCompatibility'
    assert abs(physical.get_editor_property('friction') - 0.71) < 1e-6
    assert abs(physical.get_editor_property('restitution') - 0.23) < 1e-6
    assert not actor.get_actor_enable_collision()
    return {'passed': True, 'mode': mode, 'class': actor.get_class().get_path_name(),
            'settings': SETTINGS, 'component': component.get_path_name(),
            'material': MATERIAL, 'physical_material': physical.get_path_name(),
            'follow_and_fallback_references': True, 'custom_collision_channels': True,
            'owner_collision_disabled': True}


try:
    result = run()
except Exception:
    result = {'passed': False, 'error': traceback.format_exc()}
Path(os.environ['TERRAIN_COMPAT_REPORT']).write_text(json.dumps(result, indent=2) + '\n')
if not result['passed']:
    raise RuntimeError(result['error'])
