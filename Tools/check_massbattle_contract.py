#!/usr/bin/env python3
"""Check that an updated MassBattle source tree still exposes the integration contract used by this plugin."""
from __future__ import annotations

import argparse
import sys
from pathlib import Path


def find_file(root: Path, suffix: str) -> Path | None:
    matches = list(root.rglob(suffix))
    return matches[0] if matches else None


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("source_root", type=Path, help="Project/plugin root containing Source/MassBattle and Source/MassAPI")
    args = parser.parse_args()
    root = args.source_root.resolve()

    checks = {
        "MassBattleAgentRenderProcessor.cpp": [
            "StyleArray[InstanceId]",
            "StyleTypeList[i].Index",
            "SetNiagaraArrayInt32",
            "User.StyleArray",
            "AgentMesh",
        ],
        "MassBattleAgentRenderer.h": [
            "NiagaraSystemAsset",
            "AgentMesh",
        ],
        "MassBattleFuncLib.h": [
            "MakeTemplateDataFromDataAsset",
        ],
        "MassAPISubsystem.h": [
            "CloneTemplate",
            "SetFragment",
            "SetSharedFragment",
            "AddSharedFragment",
            "GetFragmentPtr",
            "GetSharedFragmentPtr",
        ],
    }

    failed = False
    for filename, required_tokens in checks.items():
        path = find_file(root, filename)
        if path is None:
            print(f"FAIL missing {filename}")
            failed = True
            continue
        text = path.read_text(encoding="utf-8-sig", errors="replace")
        missing = [token for token in required_tokens if token not in text]
        if missing:
            print(f"FAIL {path}: missing {', '.join(missing)}")
            failed = True
        else:
            print(f"PASS {path}")

    if failed:
        print("The updated source no longer matches the zero-intrusion contract; adapt this plugin before upgrading.")
        return 1

    print("PASS MassBattle integration contract")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
