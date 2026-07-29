"""Wire the generated demo AgentConfig to the plugin renderer and Niagara system."""

import unreal


CONFIG_PATH = "/MassBattleSingleTurret/Demo/Tank/Tank_SingleTurret_AgentConfig"
LAYOUT_PATH = "/MassBattleSingleTurret/Demo/Tank/Tank_SingleTurret_SingleTurret"
RENDERER_PATH = "/MassBattleSingleTurret/Demo/Tank/Renderer_Tank_SingleTurret"
NIAGARA_PATH = "/MassBattleSingleTurret/Demo/Tank/NS_Tank_SingleTurret"


def _message_result(result):
    if isinstance(result, tuple):
        return bool(result[0]), str(result[1])
    if isinstance(result, str):
        return True, result
    return bool(result), str(result)


def main():
    config = unreal.load_asset(CONFIG_PATH)
    layout = unreal.load_asset(LAYOUT_PATH)
    renderer_blueprint = unreal.load_asset(RENDERER_PATH)
    renderer_class = unreal.EditorAssetLibrary.load_blueprint_class(RENDERER_PATH)
    niagara = unreal.load_asset(NIAGARA_PATH)
    if not all((config, layout, renderer_blueprint, renderer_class, niagara)):
        raise RuntimeError("Demo config/layout/renderer/Niagara asset is missing")

    configured, message = _message_result(
        unreal.MBSTSingleTurretEditorLibrary.configure_mass_battle_renderer_class(
            renderer_class,
            layout,
            niagara,
        )
    )
    unreal.log_warning(f"MBST_RENDERER class_configured={configured} message={message}")
    if not configured:
        raise RuntimeError(message)

    configured, message = _message_result(
        unreal.MBSTSingleTurretEditorLibrary.configure_agent_config_renderer(
            config,
            renderer_class,
        )
    )
    unreal.log_warning(f"MBST_RENDERER config_configured={configured} message={message}")
    if not configured:
        raise RuntimeError(message)

    configured, message = _message_result(
        unreal.MBSTSingleTurretEditorLibrary.ensure_niagara_style_array(niagara)
    )
    unreal.log_warning(f"MBST_RENDERER niagara_configured={configured} message={message}")
    if not configured:
        raise RuntimeError(message)

    asset_subsystem = unreal.get_editor_subsystem(unreal.EditorAssetSubsystem)
    if not asset_subsystem.save_loaded_assets(
        [config, renderer_blueprint, niagara],
        False,
    ):
        raise RuntimeError("Could not save the demo renderer contract")


main()
