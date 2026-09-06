import unreal


PACKAGE_PATH = "/PCS/Materials"
ASSET_NAME = "M_PCSDefault"
ASSET_PATH = f"{PACKAGE_PATH}/{ASSET_NAME}"


def create_default_material():
    if unreal.EditorAssetLibrary.does_asset_exist(ASSET_PATH):
        unreal.EditorAssetLibrary.delete_asset(ASSET_PATH)

    material = unreal.AssetToolsHelpers.get_asset_tools().create_asset(
        ASSET_NAME,
        PACKAGE_PATH,
        unreal.Material,
        unreal.MaterialFactoryNew(),
    )
    if material is None:
        raise RuntimeError(f"Failed to create {ASSET_PATH}")

    material.set_editor_property("material_domain", unreal.MaterialDomain.MD_SURFACE)
    material.set_editor_property("blend_mode", unreal.BlendMode.BLEND_OPAQUE)
    material.set_editor_property("shading_model", unreal.MaterialShadingModel.MSM_UNLIT)
    material.set_editor_property("two_sided", True)
    # PCS reuses UE's built-in point-cloud material usage bit because plugins
    # cannot add a new bit to EMaterialUsage without modifying the engine. Use
    # the editor API so UE also invalidates the cached shader metadata.
    unreal.MaterialEditingLibrary.set_base_material_usage(
        material,
        unreal.MaterialUsage.MATUSAGE_LIDAR_POINT_CLOUD,
        True,
    )

    vertex_color = unreal.MaterialEditingLibrary.create_material_expression(
        material,
        unreal.MaterialExpressionVertexColor,
        -300,
        0,
    )
    if not unreal.MaterialEditingLibrary.connect_material_property(
        vertex_color,
        "",  # Vertex Color exposes its float4 as the unnamed first output.
        unreal.MaterialProperty.MP_EMISSIVE_COLOR,
    ):
        raise RuntimeError("Failed to connect Vertex Color RGB to Emissive Color")
    unreal.MaterialEditingLibrary.layout_material_expressions(material)
    unreal.MaterialEditingLibrary.recompile_material(material)

    if not unreal.EditorAssetLibrary.save_loaded_asset(material, only_if_is_dirty=False):
        raise RuntimeError(f"Failed to save {ASSET_PATH}")

    unreal.log(f"Created PCS default material: {ASSET_PATH}")


create_default_material()
