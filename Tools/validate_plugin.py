#!/usr/bin/env python3
"""Static/package checks that do not require Unreal Engine or UnrealBuildTool."""
from __future__ import annotations

import json
import math
import random
import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
SOURCE_ROOT = ROOT / "Source"


def require(condition: bool, message: str) -> None:
    if not condition:
        raise AssertionError(message)


def quantize(value: float, minimum: float, maximum: float, max_q: int) -> int:
    alpha = min(max((value - minimum) / (maximum - minimum), 0.0), 1.0)
    return int(math.floor(alpha * max_q + 0.5))


def dequantize(value: int, minimum: float, maximum: float, max_q: int) -> float:
    return minimum + (maximum - minimum) * (value / max_q)


def unwind_degrees(value: float) -> float:
    value = math.fmod(value, 360.0)
    if value > 180.0:
        value -= 360.0
    elif value < -180.0:
        value += 360.0
    return value


def pack(style: int, yaw: float, pitch: float, recoil: float) -> int:
    style_q = min(max(style, 0), 255)
    yaw_q = quantize(unwind_degrees(yaw), -180.0, 180.0, 4095)
    pitch_q = quantize(pitch, -90.0, 90.0, 255)
    recoil_q = quantize(recoil, 0.0, 1.0, 15)
    return style_q | (yaw_q << 8) | (pitch_q << 20) | (recoil_q << 28)


def unpack(value: int) -> tuple[int, float, float, float]:
    value &= 0xFFFFFFFF
    return (
        value & 255,
        dequantize((value >> 8) & 4095, -180.0, 180.0, 4095),
        dequantize((value >> 20) & 255, -90.0, 90.0, 255),
        dequantize((value >> 28) & 15, 0.0, 1.0, 15),
    )


def rotate_axis(vector: tuple[float, float, float], axis: tuple[float, float, float], radians: float) -> tuple[float, float, float]:
    ax, ay, az = axis
    length = math.sqrt(ax * ax + ay * ay + az * az)
    ax, ay, az = ax / length, ay / length, az / length
    x, y, z = vector
    sine, cosine = math.sin(radians), math.cos(radians)
    cross = (ay * z - az * y, az * x - ax * z, ax * y - ay * x)
    dot = ax * x + ay * y + az * z
    return (
        x * cosine + cross[0] * sine + ax * dot * (1.0 - cosine),
        y * cosine + cross[1] * sine + ay * dot * (1.0 - cosine),
        z * cosine + cross[2] * sine + az * dot * (1.0 - cosine),
    )


def check_descriptor_and_modules() -> None:
    descriptor_path = ROOT / "MassBattleSingleTurret.uplugin"
    descriptor = json.loads(descriptor_path.read_text(encoding="utf-8"))
    expected = {"MassBattleSingleTurretRuntime", "MassBattleSingleTurretEditor"}
    modules = descriptor.get("Modules", [])
    require({item["Name"] for item in modules} == expected, "Unexpected .uplugin module list")
    require(descriptor.get("CanContainContent") is True, "CanContainContent must be true for project-side HLSL/assets")
    require(descriptor.get("Version", 0) >= 20000, "Descriptor version must include the direct AgentConfig beta contract")
    for module in modules:
        module_name = module["Name"]
        build_file = SOURCE_ROOT / module_name / f"{module_name}.Build.cs"
        require(build_file.exists(), f"Missing {build_file.relative_to(ROOT)}")


def check_generated_include_order() -> None:
    for header in SOURCE_ROOT.rglob("*.h"):
        include_lines = [line.strip() for line in header.read_text(encoding="utf-8-sig").splitlines() if line.strip().startswith("#include")]
        generated = [line for line in include_lines if ".generated.h" in line]
        if generated:
            require(len(generated) == 1, f"Multiple generated includes in {header.relative_to(ROOT)}")
            require(include_lines[-1] == generated[0], f"Generated include must be last in {header.relative_to(ROOT)}")


def strip_cpp_comments_and_literals(text: str) -> str:
    pattern = re.compile(r'//.*?$|/\*.*?\*/|"(?:\\.|[^"\\])*"|\'(?:\\.|[^\'\\])*\'', re.M | re.S)
    return pattern.sub("", text)


def check_balanced_delimiters() -> None:
    for path in [*SOURCE_ROOT.rglob("*.h"), *SOURCE_ROOT.rglob("*.cpp")]:
        text = strip_cpp_comments_and_literals(path.read_text(encoding="utf-8-sig"))
        for opening, closing in (("{", "}"), ("(", ")"), ("[", "]")):
            depth = 0
            for char in text:
                if char == opening:
                    depth += 1
                elif char == closing:
                    depth -= 1
                    require(depth >= 0, f"Unbalanced {opening}{closing} in {path.relative_to(ROOT)}")
            require(depth == 0, f"Unbalanced {opening}{closing} in {path.relative_to(ROOT)}")


def check_packing_contract() -> None:
    cpp = (SOURCE_ROOT / "MassBattleSingleTurretRuntime/Public/MBSTSingleTurretTypes.h").read_text(encoding="utf-8")
    hlsl = (ROOT / "Shaders/Private/MBSTSingleTurret.ush").read_text(encoding="utf-8")

    cpp_values = {"StyleBits": 8, "YawBits": 12, "PitchBits": 8, "RecoilBits": 4}
    for name, expected in cpp_values.items():
        match = re.search(rf"{name}\s*=\s*(\d+)u", cpp)
        require(bool(match) and int(match.group(1)) == expected, f"C++ packing constant {name} mismatch")

    hlsl_values = {
        "MBST_STYLE_SHIFT": 0,
        "MBST_YAW_SHIFT": 8,
        "MBST_PITCH_SHIFT": 20,
        "MBST_RECOIL_SHIFT": 28,
        "MBST_STYLE_MASK": 255,
        "MBST_YAW_MASK": 4095,
        "MBST_PITCH_MASK": 255,
        "MBST_RECOIL_MASK": 15,
    }
    for name, expected in hlsl_values.items():
        match = re.search(rf"#define\s+{name}\s+(\d+)u", hlsl)
        require(bool(match) and int(match.group(1)) == expected, f"HLSL packing constant {name} mismatch")

    random.seed(20260715)
    max_yaw_error = 180.0 / 4095.0 + 1e-6
    max_pitch_error = 90.0 / 255.0 + 1e-6
    max_recoil_error = 0.5 / 15.0 + 1e-6
    for _ in range(10_000):
        style = random.randint(0, 255)
        yaw = random.uniform(-179.99, 179.99)
        pitch = random.uniform(-90.0, 90.0)
        recoil = random.random()
        decoded_style, decoded_yaw, decoded_pitch, decoded_recoil = unpack(pack(style, yaw, pitch, recoil))
        require(decoded_style == style, "Style round-trip mismatch")
        require(abs(decoded_yaw - yaw) <= max_yaw_error, "Yaw quantization error exceeds half-step")
        require(abs(decoded_pitch - pitch) <= max_pitch_error, "Pitch quantization error exceeds half-step")
        require(abs(decoded_recoil - recoil) <= max_recoil_error, "Recoil quantization error exceeds half-step")


def check_rotation_reference() -> None:
    rotated = rotate_axis((1.0, 0.0, 0.0), (0.0, 0.0, 1.0), math.pi * 0.5)
    error = max(abs(rotated[0]), abs(rotated[1] - 1.0), abs(rotated[2]))
    require(error < 1e-6, "Rodrigues +X to +Y reference test failed")


def check_source_contract() -> None:
    processor = (SOURCE_ROOT / "MassBattleSingleTurretRuntime/Private/MBSTSingleTurretProcessor.cpp").read_text(encoding="utf-8")
    blueprint = (SOURCE_ROOT / "MassBattleSingleTurretRuntime/Private/MBSTSingleTurretBlueprintLibrary.cpp").read_text(encoding="utf-8")
    editor = (SOURCE_ROOT / "MassBattleSingleTurretEditor/Private/MBSTSingleTurretEditorLibrary.cpp").read_text(encoding="utf-8")
    require("ProcessingPhase = EMassProcessingPhase::StartPhysics" in processor, "Logic pack must run before MassBattle's manual renderer tick group")
    require("IsSubFrameScheduled(ESubFrame::Subtick3)" in processor, "Logic pack must be gated to the framework's Subtick3 sample")
    require(".None<FMBSTMobileFireTag>()" in processor, "MobileFire must not pay the generic pack scan")
    require("Styles[EntityIndex].Index = MBSTPacking::SanitizeAndPack" in processor, "Canonical StyleArray packing write is missing")
    require("RemoveSharedFragment<FMBSTSingleTurretShared>(ClonedTemplate)" in blueprint
            and "ClonedTemplate.AddSharedFragment(" in blueprint
            and "GetOrCreateSharedFragment(Shared)" in blueprint,
            "Template shared layout must be replaced through the current MassAPI contract")
    require("DataAsset->SkeletalMesh =" in editor and "DataAsset->StaticMesh =" in editor, "VAT mesh assignment is missing")
    require("PaintVertexMask(BodyMesh" in editor and "PaintVertexMask(TurretMesh" in editor, "Articulation mask generation is missing")
    require("CreateSingleTurretAgentConfigFromTemplate(" in editor, "Actor conversion does not create a turret AgentConfig")
    require("AgentConfig->ExtraData.Tags" in editor, "AgentConfig turret Tag injection is missing")
    require("AgentConfig->ExtraData.Fragments" in editor, "AgentConfig turret State injection is missing")
    require("AgentConfig->ExtraData.MutableSharedFragments" in editor, "AgentConfig turret Shared layout injection is missing")


def check_docs_links() -> None:
    markdown_files = [ROOT / "README.md", ROOT / "README_ZH.md", *sorted((ROOT / "Docs").glob("*.md"))]
    for document in markdown_files:
        require(document.exists(), f"Missing documentation file {document.relative_to(ROOT)}")
        text = document.read_text(encoding="utf-8")
        for target in re.findall(r"\[[^\]]+\]\(([^)]+)\)", text):
            if "://" in target or target.startswith("#"):
                continue
            target_path = (document.parent / target).resolve()
            require(target_path.exists(), f"Broken link {target} in {document.relative_to(ROOT)}")


def main() -> int:
    checks = (
        check_descriptor_and_modules,
        check_generated_include_order,
        check_balanced_delimiters,
        check_packing_contract,
        check_rotation_reference,
        check_source_contract,
        check_docs_links,
    )
    for check in checks:
        check()
        print(f"PASS {check.__name__}")
    print("PASS all plugin-independent checks")
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except AssertionError as error:
        print(f"FAIL {error}", file=sys.stderr)
        raise SystemExit(1)
