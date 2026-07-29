"""Force-delete only the generated demo tank assets so automation can rebuild them.

Run through UnrealEditor-Cmd. Source MassBattle/MassBattleFrame assets are never
modified; this touches only generated assets owned by MassBattleSingleTurret.
"""

import unreal


ASSET_PATHS = (
    "/MassBattleSingleTurret/Demo/Tank/Tank_SingleTurret_AgentConfig",
    "/MassBattleSingleTurret/Demo/Tank/Tank_SingleTurret_SingleTurret",
    "/MassBattleSingleTurret/Demo/Tank/Tank_SingleTurret",
)


def main():
    asset_subsystem = unreal.get_editor_subsystem(unreal.EditorAssetSubsystem)
    for asset_path in ASSET_PATHS:
        if unreal.EditorAssetLibrary.does_asset_exist(asset_path):
            deleted = asset_subsystem.delete_asset(asset_path)
            unreal.log_warning(f"MBST_RESET asset={asset_path} deleted={deleted}")
            if not deleted:
                raise RuntimeError(f"Could not delete generated demo asset: {asset_path}")
        else:
            unreal.log_warning(f"MBST_RESET asset={asset_path} already_missing=True")


main()
