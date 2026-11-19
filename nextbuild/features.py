from __future__ import annotations

from pathlib import Path

KNOWN_FEATURES = (
    "SSE42",
    "AVX2",
    "AVX512",
    "AVX512BW",
    "FMA",
    "VNNI",
)

DEFAULT_HOST_FEATURES = ("AVX2", "FMA")


def generate_cmake_generated_module(
    *,
    library_kind: str,
    features: set[str],
    little_endian: bool = True,
) -> str:
    """Synthesize `cmake_generated` BMI source formerly produced by CMake."""
    kind = library_kind.lower()
    is_windows = kind == "host"
    enabled = {name: (name in features) for name in KNOWN_FEATURES}
    if kind == "host":
        enabled["AVX2"] = True
        enabled["FMA"] = True

    parts: list[str] = []
    for name in KNOWN_FEATURES:
        value = "true" if enabled[name] else "false"
        parts.append(
            "\n".join(
                [
                    "    template <>",
                    f"    struct FeatureEnabled<{name}>",
                    "    {",
                    f"        static constexpr bool value = {value};",
                    "    };",
                    "",
                ]
            )
        )
    specializations = "\n".join(parts)

    return "\n".join(
        [
            "export module cmake_generated;",
            "",
            "export namespace cmake_generated",
            "{",
            "    struct PlatformWindows {};",
            "    struct PlatformLinux {};",
            "    struct PlatformApple {};",
            "    struct EndianessLittle {};",
            "    struct EndianessBig {};",
            "",
            "    struct SSE42 {};",
            "    struct AVX2 {};",
            "    struct AVX512 {};",
            "    struct AVX512BW {};",
            "    struct FMA {};",
            "    struct VNNI {};",
            "",
            f"    inline constexpr bool kIsWindowsPlatform = {'true' if is_windows else 'false'};",
            "    inline constexpr bool kIsLinuxPlatform = false;",
            "    inline constexpr bool kIsApplePlatform = false;",
            f"    inline constexpr bool kIsLittleEndian = {'true' if little_endian else 'false'};",
            f"    inline constexpr bool kIsBigEndian = {'false' if little_endian else 'true'};",
            "",
            "    template <class Feature>",
            "    struct FeatureEnabled",
            "    {",
            "        static constexpr bool value = false;",
            "    };",
            "",
            specializations.rstrip(),
            "",
            "    template <class Feature>",
            "    inline constexpr bool kIsEnabledFeature = FeatureEnabled<Feature>::value;",
            "}",
            "",
        ]
    )


def write_cmake_generated(path: Path, *, library_kind: str, features: set[str]) -> Path:
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(
        generate_cmake_generated_module(library_kind=library_kind, features=features),
        encoding="utf-8",
    )
    return path
