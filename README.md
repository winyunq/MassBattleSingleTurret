# MassBattle Single Turret

Independent UE 5.8 plugin that converts an arbitrary component-based Actor into a directly spawnable single-entity MassBattle turret unit.

The conversion entry point always creates a new AgentConfig and injects exactly one `FMBSTSingleTurretTag`, `FMBSTSingleTurretState`, and `FMBSTSingleTurretShared`. An optional normal AgentConfig is copied only as a template and is never modified in place. Normal units that were not created through this conversion path remain untouched and are excluded from the turret processor query.

The authoring component exists only to identify the turret pivot, optional barrel pivot, muzzle, and VAT driver during conversion. Runtime units use one Mass entity, one Niagara particle, one merged StaticMesh, and no Actor component, host entity, or turret child Agent.

The included tank demo is fully wired under `/MassBattleSingleTurret/Demo/Tank`: generated mesh, layout, AgentConfig, Renderer Blueprint, Niagara system, plugin material, and material instances. It was derived from the MassBattleFrame demo tank without modifying MassBattleFrame.

This design removes structural overhead; it is not literally compute-free. Tagged turret units still pay for a compact per-entity state update, one packed 32-bit style value, Niagara transfer, and vertex articulation. See [README_ZH.md](README_ZH.md) and [Docs/04_Performance_ZH.md](Docs/04_Performance_ZH.md).
