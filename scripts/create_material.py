"""Run with Unreal Editor Python, or -ExecutePythonScript=<this file>."""
import unreal
path = "/DynamicTerrain/M_Terrain"
unreal.AssetRegistryHelpers.get_asset_registry().wait_for_completion()
if unreal.load_asset(path) is None:
    material = unreal.AssetToolsHelpers.get_asset_tools().create_asset("M_Terrain", "/DynamicTerrain", unreal.Material, unreal.MaterialFactoryNew())
    sample = unreal.MaterialEditingLibrary.create_material_expression(material, unreal.MaterialExpressionTextureSampleParameter2D)
    sample.set_editor_property("parameter_name", "TerrainTexture")
    sample.set_editor_property("texture", unreal.load_asset("/Engine/EngineResources/WhiteSquareTexture"))
    unreal.MaterialEditingLibrary.connect_material_property(sample, "RGB", unreal.MaterialProperty.MP_BASE_COLOR)
    roughness = unreal.MaterialEditingLibrary.create_material_expression(material, unreal.MaterialExpressionConstant)
    roughness.set_editor_property("r", 0.95)
    unreal.MaterialEditingLibrary.connect_material_property(roughness, "", unreal.MaterialProperty.MP_ROUGHNESS)
    unreal.MaterialEditingLibrary.recompile_material(material)
    unreal.EditorAssetLibrary.save_loaded_asset(material)
