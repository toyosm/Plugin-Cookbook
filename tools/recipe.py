#!/usr/bin/env python3
# Plugin Cookbook — recipe tool.
# SPDX-License-Identifier: MIT
"""
Turn *.plugin.toml recipes into DPF plugin sources, test them offline, and build them.

    python3 tools/recipe.py new my-plugin              scaffold plugins/my-plugin.plugin.toml
    python3 tools/recipe.py check [RECIPE ...|--all]   validate recipes (schema + static DSP checks)
    python3 tools/recipe.py test  [RECIPE ...|--all]   compile the kernel natively and run the test suite
    python3 tools/recipe.py render RECIPE IN.wav OUT.wav [--set sym=value ...]
    python3 tools/recipe.py build [RECIPE ...]         build VST3 / CLAP / AU / LV2 / standalone with CMake
    python3 tools/recipe.py install                    copy built plugins to this user's plugin folders
    python3 tools/recipe.py studio                     open Plugin Studio (chat with an AI, test, build) in the browser
    python3 tools/recipe.py generate --all --out DIR   (used by CMake) emit generated sources

Requires Python 3.11+ (for tomllib) or the `tomli` package.
"""

from __future__ import annotations

import argparse
import json
import math
import os
import platform
import re
import shutil
import subprocess
import sys
from dataclasses import dataclass, field
from pathlib import Path

try:
    import tomllib  # Python 3.11+
except ModuleNotFoundError:  # pragma: no cover
    try:
        import tomli as tomllib  # type: ignore
    except ModuleNotFoundError:
        sys.exit("recipe.py needs Python 3.11+ (or `pip install tomli`). Found Python %s." % platform.python_version())

ROOT = Path(__file__).resolve().parent.parent
FRAMEWORK_INCLUDE = ROOT / "framework" / "include"
HARNESS_SRC = ROOT / "tools" / "harness" / "harness.cpp"
PLUGINS_DIR = ROOT / "plugins"
EXAMPLES_DIR = ROOT / "examples"
RECIPE_SUFFIX = ".plugin.toml"

# ----------------------------------------------------------------------------------------------------------------------
# Tables

# category -> (LV2 class, VST3 category, CLAP feature or None)
CATEGORIES = {
    "amplifier":  ("lv2:AmplifierPlugin",  "Fx|Tools",       "utility"),
    "distortion": ("lv2:DistortionPlugin", "Fx|Distortion",  "distortion"),
    "waveshaper": ("lv2:WaveshaperPlugin", "Fx|Distortion",  "distortion"),
    "simulator":  ("lv2:SimulatorPlugin",  "Fx|Distortion",  "distortion"),
    "filter":     ("lv2:FilterPlugin",     "Fx|Filter",      "filter"),
    "eq":         ("lv2:EQPlugin",         "Fx|EQ",          "equalizer"),
    "compressor": ("lv2:CompressorPlugin", "Fx|Dynamics",    "compressor"),
    "limiter":    ("lv2:LimiterPlugin",    "Fx|Dynamics",    "limiter"),
    "gate":       ("lv2:GatePlugin",       "Fx|Dynamics",    None),
    "dynamics":   ("lv2:DynamicsPlugin",   "Fx|Dynamics",    "compressor"),
    "chorus":     ("lv2:ChorusPlugin",     "Fx|Modulation",  "chorus"),
    "flanger":    ("lv2:FlangerPlugin",    "Fx|Modulation",  "flanger"),
    "phaser":     ("lv2:PhaserPlugin",     "Fx|Modulation",  "phaser"),
    "modulation": ("lv2:ModulatorPlugin",  "Fx|Modulation",  "tremolo"),
    "delay":      ("lv2:DelayPlugin",      "Fx|Delay",       "delay"),
    "reverb":     ("lv2:ReverbPlugin",     "Fx|Reverb",      "reverb"),
    "pitch":      ("lv2:PitchPlugin",      "Fx|Pitch Shift", "pitch-shifter"),
    "spatial":    ("lv2:SpatialPlugin",    "Fx|Spatial",     None),
    "utility":    ("lv2:UtilityPlugin",    "Fx|Tools",       "utility"),
}

PARAM_TYPES = ("float", "int", "bool", "choice")

ALLOWED_INCLUDES = {
    "algorithm", "array", "atomic", "cassert", "cfloat", "climits", "cmath", "complex", "cstddef", "cstdint",
    "cstdlib", "cstring", "functional", "limits", "numeric", "random", "type_traits", "utility", "vector",
    "cookbook/dsp.hpp",
}

CPP_KEYWORDS = set("""
alignas alignof and and_eq asm auto bitand bitor bool break case catch char char8_t char16_t char32_t class compl
concept const consteval constexpr constinit const_cast continue co_await co_return co_yield decltype default delete
do double dynamic_cast else enum explicit export extern false float for friend goto if inline int long mutable
namespace new noexcept not not_eq nullptr operator or or_eq private protected public register reinterpret_cast
requires return short signed sizeof static static_assert static_cast struct switch template this thread_local throw
true try typedef typeid typename union unsigned using virtual void volatile wchar_t while xor xor_eq
in out frames p params kernel sampleRate
""".split())

# Patterns that must never appear inside Kernel::process(). (comments and strings are stripped first)
FORBIDDEN_IN_PROCESS = [
    (r"\bnew\b", "heap allocation (`new`)"),
    (r"\bdelete\b", "heap deallocation (`delete`)"),
    (r"\b(malloc|calloc|realloc|free)\s*\(", "C heap allocation"),
    (r"\.\s*(push_back|emplace_back|resize|reserve|insert|assign|erase|clear|shrink_to_fit)\s*\(",
     "container operation that may allocate/free (do it in prepare())"),
    (r"\bstd::(string|map|unordered_map|set|list|deque|function|shared_ptr|make_shared|make_unique)\b",
     "allocating standard library type"),
    (r"\b(printf|fprintf|puts|std::cout|std::cerr|std::clog|fopen|fwrite|fread|std::ifstream|std::ofstream)\b",
     "I/O"),
    (r"\bstd::(mutex|lock_guard|unique_lock|scoped_lock|thread|condition_variable)\b", "locks/threads"),
    (r"\b(sleep|usleep|nanosleep|sleep_for|sleep_until)\b", "sleeping"),
    (r"\bthrow\b", "exceptions"),
]

DEFAULT_ACCENT = "#E8A33D"

# Editor controls per parameter type; the first one is the default.
CONTROLS = {
    "float": ("knob", "small-knob", "large-knob", "slider", "vslider", "number"),
    "int": ("knob", "small-knob", "large-knob", "slider", "vslider", "number", "selector"),
    "bool": ("toggle", "button", "led", "footswitch", "lever", "checkbox"),
    "choice": ("dropdown", "buttons", "radio", "selector",
               "toggle", "button", "led", "footswitch", "lever", "checkbox"),
}
SWITCH_CONTROLS = ("toggle", "button", "led", "footswitch", "lever", "checkbox")  # on/off look
SELECTOR_MAX_STEPS = 12
THEMES = ("dark", "light")
KNOB_STYLES = ("modern", "vintage", "flat", "minimal", "metal", "fluted")
KNOB_CONTROLS = ("knob", "small-knob", "large-knob")
LAYOUTS = ("flow", "free")
MAX_IMAGE_BYTES = 4_000_000

# Free layout: default control box sizes (width, height) in unscaled pixels; knobs are square.
PARAM_UI_KEYS = ("control", "knob_style", "x", "y", "size", "label", "value", "label_color", "value_color",
                 "knob_image", "knob_frames")
FREE_BOX = {"knob": (58, 58), "small-knob": (40, 40), "large-knob": (88, 88), "slider": (200, 20),
            "vslider": (24, 110), "number": (84, 22), "selector": (110, 110), "toggle": (46, 24),
            "button": (84, 30), "led": (22, 22), "footswitch": (56, 76), "lever": (28, 48), "checkbox": (22, 22),
            "dropdown": (104, 19)}
# free layout: `size` sets the width and the height follows proportionally (vslider: `size` is its height)
FREE_UNIFORM = ("selector", "led", "footswitch", "lever", "checkbox")

# Editor geometry in unscaled pixels. framework/include/cookbook/Controls.hpp and the Plugin Studio preview
# (tools/studio/static/index.html) use the same numbers and the same layout rules: keep all three in sync.
UI_PAD = 18
UI_HEADER_H = 58
UI_SUBTITLE_H = 16
UI_TITLE_H = 28
UI_GROUP_GAP = 6
UI_BOTTOM = 12
UI_MIN_WIDTH = 300
UI_CELL = {  # control -> (width, height); "buttons" and "radio" depend on their choices (cell_size)
    "knob": (118, 132), "small-knob": (92, 110), "large-knob": (150, 168), "slider": (236, 84),
    "vslider": (70, 170), "number": (118, 84), "selector": (150, 160),
    "toggle": (118, 132), "button": (118, 132), "led": (118, 84), "footswitch": (118, 132), "lever": (118, 132),
    "checkbox": (118, 84), "dropdown": (118, 132),
}

# ----------------------------------------------------------------------------------------------------------------------
# Data model


class RecipeError(Exception):
    pass


@dataclass
class Param:
    symbol: str
    name: str
    short_name: str
    type: str
    min: float
    max: float
    default: float
    unit: str
    scale: str
    smoothing_ms: float
    description: str
    choices: list[str] = field(default_factory=list)
    control: str = ""
    knob_style: str = ""   # as written in the recipe; "" = [ui].knob_style
    # free layout and images (as written; None/"" = not set)
    x: float | None = None
    y: float | None = None
    size: float | None = None
    label: bool | None = None
    value: bool | None = None
    knob_image: str = ""
    knob_frames: int = 0
    label_color: int | None = None
    value_color: int | None = None


@dataclass
class UiGroup:
    title: str
    columns: int        # 0 = [ui].columns
    params: list[int]   # parameter indices


@dataclass
class Recipe:
    path: Path
    id: str
    name: str
    brand: str
    brand_id: str
    unique_id: str
    version: tuple[int, int, int]
    description: str
    license: str
    homepage: str
    category: str
    inputs: int
    outputs: int
    clap_id: str
    uri: str
    accent: int
    columns: int
    columns_set: bool  # columns written in the recipe (otherwise 5-6 plain controls become two rows of three)
    theme: str
    background: int | None
    subtitle: str
    knob_style: str
    knob_color: int | None
    knob_ticks: bool
    layout: str
    background_image: str        # path relative to the recipe, "" = none
    image_scale: float           # image pixels per editor pixel (2 for retina artwork)
    knob_image: str
    knob_frames: int
    free_size: tuple[int, int] | None  # [ui].size
    labels: bool
    title_color: int | None
    label_color: int | None
    value_color: int | None
    images: dict[str, tuple[int, int]]  # every referenced image -> (width, height) in pixels
    groups: list[UiGroup]  # as written in the recipe; empty = one untitled group with every parameter
    params: list[Param]
    code: str
    code_path: Path
    code_line: int  # 1-based line in code_path where `code` starts
    warnings: list[str] = field(default_factory=list)

    @property
    def label(self) -> str:
        return self.id.replace("-", "_")


# ----------------------------------------------------------------------------------------------------------------------
# Loading and validation


def _err(errors: list[str], msg: str) -> None:
    errors.append(msg)


def _check_keys(errors: list[str], where: str, table: dict, allowed: set[str]) -> None:
    for k in table:
        if k not in allowed:
            _err(errors, f"{where}: unknown key '{k}' (allowed: {', '.join(sorted(allowed))})")


def _slug(text: str) -> str:
    s = re.sub(r"[^a-z0-9]+", "-", text.lower()).strip("-")
    return s or "cookbook"


def _num(errors: list[str], where: str, value, kind=float):
    if isinstance(value, bool) or not isinstance(value, (int, float)):
        _err(errors, f"{where}: expected a number, got {value!r}")
        return None
    if kind is int and float(value) != int(value):
        _err(errors, f"{where}: expected an integer, got {value!r}")
        return None
    if isinstance(value, float) and (math.isnan(value) or math.isinf(value)):
        _err(errors, f"{where}: must be finite")
        return None
    return kind(value)


def _find_code_line(text: str) -> int:
    """Line (1-based) of the first line of the [dsp] code string in the TOML text."""
    lines = text.splitlines()
    in_dsp = False
    for i, line in enumerate(lines):
        stripped = line.strip()
        if stripped.startswith("[") and not stripped.startswith("[["):
            in_dsp = stripped.replace(" ", "") == "[dsp]"
            continue
        if in_dsp and re.match(r"^code\s*=\s*('''|\"\"\")", stripped):
            rest = re.sub(r"^code\s*=\s*('''|\"\"\")", "", stripped)
            # TOML trims a newline immediately following the opening delimiter
            return i + 1 if rest else i + 2
    return 1


def png_size(path: Path) -> tuple[int, int] | str:
    """(width, height) of a PNG file, or an error message."""
    if not path.exists():
        return f"{path.name} not found (put it next to the recipe, e.g. plugins/<id>/{path.name})"
    data = path.read_bytes()
    if len(data) > MAX_IMAGE_BYTES:
        return f"{path.name} is {len(data) // 1_000_000} MB; keep images under {MAX_IMAGE_BYTES // 1_000_000} MB"
    if data[:8] != b"\x89PNG\r\n\x1a\n" or data[12:16] != b"IHDR":
        return f"{path.name} is not a PNG image"
    return int.from_bytes(data[16:20], "big"), int.from_bytes(data[20:24], "big")


def load_recipe(path: Path) -> Recipe:
    path = Path(path)
    if not path.exists():
        raise RecipeError(f"{path}: file not found")
    return load_recipe_text(path.read_text(encoding="utf-8"), path)


def load_recipe_text(text: str, path: Path) -> Recipe:
    """Validates recipe `text`; `path` is used for messages, the file-name check and [dsp].file."""
    path = Path(path)
    try:
        data = tomllib.loads(text)
    except tomllib.TOMLDecodeError as e:
        raise RecipeError(f"{path}: TOML syntax error: {e}")

    errors: list[str] = []
    warnings: list[str] = []
    _check_keys(errors, "top level", data, {"plugin", "ui", "param", "dsp"})

    plugin = data.get("plugin")
    if not isinstance(plugin, dict):
        raise RecipeError(f"{path}: missing [plugin] table")
    _check_keys(errors, "[plugin]", plugin, {
        "id", "name", "brand", "brand_id", "unique_id", "version", "description", "license", "homepage",
        "category", "inputs", "outputs", "clap_id", "uri"})

    def req(key):
        if key not in plugin:
            _err(errors, f"[plugin]: missing required key '{key}'")
            return ""
        v = plugin[key]
        if not isinstance(v, str):
            _err(errors, f"[plugin].{key}: expected a string")
            return ""
        return v

    pid = req("id")
    if pid and not re.fullmatch(r"[a-z][a-z0-9-]{1,40}", pid):
        _err(errors, f"[plugin].id '{pid}': use 2-41 chars of lowercase letters, digits and dashes, starting with a letter")
    if pid and path.name.endswith(RECIPE_SUFFIX) and path.name[: -len(RECIPE_SUFFIX)] != pid:
        _err(errors, f"file name must be '{pid}{RECIPE_SUFFIX}' to match [plugin].id")

    name = req("name")
    if name and len(name) > 40:
        _err(errors, "[plugin].name: keep it under 40 characters")
    brand = req("brand")
    if brand and len(brand) > 40:
        _err(errors, "[plugin].brand: keep it under 40 characters")

    brand_id = req("brand_id")
    if brand_id and not (re.fullmatch(r"[A-Za-z][A-Za-z0-9]{3}", brand_id) and re.search(r"[^a-z]", brand_id)):
        _err(errors, f"[plugin].brand_id '{brand_id}': exactly 4 letters/digits, starting with a letter, "
                     "with at least one uppercase letter or digit (Apple reserves all-lowercase codes)")
    unique_id = req("unique_id")
    if unique_id and not re.fullmatch(r"[A-Za-z][A-Za-z0-9]{3}", unique_id):
        _err(errors, f"[plugin].unique_id '{unique_id}': exactly 4 letters/digits, starting with a letter")

    description = req("description")
    category = req("category")
    if category and category not in CATEGORIES:
        _err(errors, f"[plugin].category '{category}': must be one of {', '.join(CATEGORIES)}")

    version_s = plugin.get("version", "1.0.0")
    m = re.fullmatch(r"(\d+)\.(\d+)\.(\d+)", str(version_s))
    if not m:
        _err(errors, f"[plugin].version '{version_s}': use MAJOR.MINOR.PATCH, e.g. \"1.0.0\"")
        version = (1, 0, 0)
    else:
        version = tuple(int(x) for x in m.groups())  # type: ignore

    io = {}
    for key in ("inputs", "outputs"):
        v = plugin.get(key)
        if v is None:
            _err(errors, f"[plugin]: missing required key '{key}' (1 = mono, 2 = stereo)")
            io[key] = 1
        elif v not in (1, 2) or isinstance(v, bool):
            _err(errors, f"[plugin].{key}: must be 1 (mono) or 2 (stereo)")
            io[key] = 1
        else:
            io[key] = int(v)

    license_ = plugin.get("license", "MIT")
    homepage = plugin.get("homepage", "")
    brand_slug = _slug(brand)
    clap_id = plugin.get("clap_id") or f"org.plugincookbook.{brand_slug}.{pid}"
    if not re.fullmatch(r"[a-z0-9]+(\.[a-z0-9-]+)+", clap_id):
        _err(errors, f"[plugin].clap_id '{clap_id}': reverse-DNS form, e.g. com.yourname.{pid}")
    uri = plugin.get("uri") or f"urn:plugin-cookbook:{brand_slug}:{pid}"

    # [ui]
    ui = data.get("ui", {})
    if not isinstance(ui, dict):
        _err(errors, "[ui] must be a table")
        ui = {}
    _check_keys(errors, "[ui]", ui, {"accent", "columns", "theme", "background", "subtitle", "knob_style",
                                     "knob_color", "knob_ticks", "layout", "background_image", "image_scale",
                                     "knob_image", "knob_frames", "size", "labels", "title_color", "label_color",
                                     "value_color", "group"})
    accent_s = ui.get("accent", DEFAULT_ACCENT)
    if not isinstance(accent_s, str) or not re.fullmatch(r"#[0-9A-Fa-f]{6}", accent_s):
        _err(errors, f"[ui].accent '{accent_s}': use a hex color like \"#E8A33D\"")
        accent_s = DEFAULT_ACCENT
    accent = int(accent_s[1:], 16)
    columns = ui.get("columns", 4)
    if not isinstance(columns, int) or isinstance(columns, bool) or not 1 <= columns <= 8:
        _err(errors, "[ui].columns: integer between 1 and 8")
        columns = 4
    theme = ui.get("theme", "dark")
    if theme not in THEMES:
        _err(errors, f"[ui].theme: \"dark\" or \"light\"")
        theme = "dark"
    background = None
    if "background" in ui:
        bg = ui["background"]
        if not isinstance(bg, str) or not re.fullmatch(r"#[0-9A-Fa-f]{6}", bg):
            _err(errors, f"[ui].background '{bg}': use a hex color like \"#202124\"")
        else:
            background = int(bg[1:], 16)
    subtitle = ui.get("subtitle", "")
    if not isinstance(subtitle, str) or len(subtitle) > 60:
        _err(errors, "[ui].subtitle: a string of at most 60 characters")
        subtitle = ""
    knob_style = ui.get("knob_style", "modern")
    if knob_style not in KNOB_STYLES:
        _err(errors, f"[ui].knob_style: one of {', '.join(repr(k) for k in KNOB_STYLES)}")
        knob_style = "modern"
    knob_color = None
    if "knob_color" in ui:
        kc = ui["knob_color"]
        if not isinstance(kc, str) or not re.fullmatch(r"#[0-9A-Fa-f]{6}", kc):
            _err(errors, f"[ui].knob_color '{kc}': use a hex color like \"#D9CBA8\"")
        else:
            knob_color = int(kc[1:], 16)
    knob_ticks = ui.get("knob_ticks", False)
    if not isinstance(knob_ticks, bool):
        _err(errors, "[ui].knob_ticks: true or false")
        knob_ticks = False
    images: dict[str, tuple[int, int]] = {}

    def image(where: str, value) -> str:
        """Validates an image path (relative to the recipe) and records its size."""
        if not isinstance(value, str) or not value:
            _err(errors, f"{where}: a path like \"{pid or 'my-plugin'}/knob.png\", relative to the recipe")
            return ""
        rel = Path(value)
        if rel.is_absolute() or ".." in rel.parts:
            _err(errors, f"{where}: '{value}' must be inside the recipe's folder (no absolute paths or '..')")
            return ""
        if value not in images:
            size = png_size(path.parent / rel)
            if isinstance(size, str):
                _err(errors, f"{where}: {size}")
                return ""
            images[value] = size
        return value

    layout = ui.get("layout", "flow")
    if layout not in LAYOUTS:
        _err(errors, "[ui].layout: \"flow\" (rows, the default) or \"free\" (every control at its own x, y)")
        layout = "flow"
    background_image = image("[ui].background_image", ui["background_image"]) if "background_image" in ui else ""
    image_scale = ui.get("image_scale", 1)
    if isinstance(image_scale, bool) or not isinstance(image_scale, (int, float)) or not 0.5 <= image_scale <= 4:
        _err(errors, "[ui].image_scale: a number from 0.5 to 4 (2 = artwork drawn at twice the size, for retina)")
        image_scale = 1
    image_scale = float(image_scale)
    knob_image = image("[ui].knob_image", ui["knob_image"]) if "knob_image" in ui else ""
    knob_frames = ui.get("knob_frames", 1)
    if not isinstance(knob_frames, int) or isinstance(knob_frames, bool) or not 1 <= knob_frames <= 256:
        _err(errors, "[ui].knob_frames: 1 (one image that rotates) or the number of frames in a filmstrip (2-256)")
        knob_frames = 1
    free_size = None
    if "size" in ui:
        sz = ui["size"]
        if (not isinstance(sz, list) or len(sz) != 2 or not all(isinstance(v, int) and not isinstance(v, bool)
                                                                  and 120 <= v <= 2400 for v in sz)):
            _err(errors, "[ui].size: [width, height] in pixels, each 120-2400")
        else:
            free_size = (sz[0], sz[1])
    def color(where: str, table: dict, key: str) -> int | None:
        if key not in table:
            return None
        v = table[key]
        if not isinstance(v, str) or not re.fullmatch(r"#[0-9A-Fa-f]{6}", v):
            _err(errors, f"{where}.{key} '{v}': use a hex color like \"#F2E6D0\"")
            return None
        return int(v[1:], 16)

    title_color = color("[ui]", ui, "title_color")
    label_color = color("[ui]", ui, "label_color")
    value_color = color("[ui]", ui, "value_color")
    labels = ui.get("labels", True)
    if not isinstance(labels, bool):
        _err(errors, "[ui].labels: true or false")
        labels = True
    if layout == "free" and not free_size and not background_image:
        _err(errors, "[ui]: layout = \"free\" needs background_image or size = [width, height]")

    raw_groups = ui.get("group", [])
    if layout == "free" and raw_groups:
        warnings.append("[[ui.group]] is ignored with layout = \"free\"")
    if not isinstance(raw_groups, list):
        _err(errors, "[ui] groups must be declared as [[ui.group]] tables")
        raw_groups = []

    # [[param]]
    raw_params = data.get("param", [])
    if not isinstance(raw_params, list):
        _err(errors, "parameters must be declared as [[param]] tables")
        raw_params = []
    if len(raw_params) > 32:
        _err(errors, "more than 32 parameters: simplify the plugin")
    params: list[Param] = []
    seen: set[str] = set()
    for idx, rp in enumerate(raw_params):
        where = f"[[param]] #{idx + 1}"
        if not isinstance(rp, dict):
            _err(errors, f"{where}: must be a table")
            continue
        _check_keys(errors, where, rp, {"symbol", "name", "short_name", "type", "min", "max", "default", "unit",
                                         "scale", "smoothing_ms", "description", "choices", "control",
                                         "knob_style", "x", "y", "size", "label", "value", "knob_image",
                                         "knob_frames", "label_color", "value_color"})
        sym = rp.get("symbol", "")
        where = f"[[param]] '{sym or idx + 1}'"
        if not isinstance(sym, str) or not re.fullmatch(r"[a-z][a-z0-9_]{0,31}", sym):
            _err(errors, f"{where}: symbol must be lowercase letters/digits/underscores, starting with a letter")
        elif sym in CPP_KEYWORDS:
            _err(errors, f"{where}: symbol '{sym}' is reserved, pick another")
        elif sym in seen:
            _err(errors, f"{where}: duplicate symbol")
        seen.add(sym)

        pname = rp.get("name", "")
        if not isinstance(pname, str) or not pname:
            _err(errors, f"{where}: missing 'name'")
            pname = sym
        short = rp.get("short_name", "")
        if not isinstance(short, str) or len(short) > 16:
            _err(errors, f"{where}: short_name must be a string of at most 16 characters")
            short = ""
        if not short and len(pname) > 16:
            warnings.append(f"{where}: name is long; add short_name (<= 16 chars) for small UIs")

        ptype = rp.get("type", "float")
        if ptype not in PARAM_TYPES:
            _err(errors, f"{where}: type must be one of {', '.join(PARAM_TYPES)}")
            ptype = "float"
        unit = rp.get("unit", "")
        if not isinstance(unit, str) or len(unit) > 8:
            _err(errors, f"{where}: unit must be a short string like \"dB\", \"Hz\", \"ms\", \"%\"")
            unit = ""
        scale = rp.get("scale", "linear")
        if scale not in ("linear", "log"):
            _err(errors, f"{where}: scale must be \"linear\" or \"log\"")
            scale = "linear"
        desc = rp.get("description", "")
        if not isinstance(desc, str):
            _err(errors, f"{where}: description must be a string")
            desc = ""
        choices: list[str] = []

        if ptype == "choice":
            choices = rp.get("choices", [])
            if not isinstance(choices, list) or len(choices) < 2 or not all(isinstance(c, str) and c for c in choices):
                _err(errors, f"{where}: choice parameters need choices = [\"A\", \"B\", ...] (2 or more)")
                choices = ["A", "B"]
            if len(set(choices)) != len(choices):
                _err(errors, f"{where}: duplicate choice labels")
            for k in ("min", "max", "scale", "unit"):
                if k in rp:
                    _err(errors, f"{where}: '{k}' is not used for choice parameters")
            pmin, pmax = 0.0, float(len(choices) - 1)
            d = rp.get("default", 0)
            if isinstance(d, str):
                if d not in choices:
                    _err(errors, f"{where}: default '{d}' is not one of the choices")
                    d = 0
                else:
                    d = choices.index(d)
            dv = _num(errors, f"{where}.default", d, int)
            pdef = float(dv if dv is not None else 0)
            smoothing = 0.0
        elif ptype == "bool":
            for k in ("min", "max", "scale", "unit", "choices"):
                if k in rp:
                    _err(errors, f"{where}: '{k}' is not used for bool parameters")
            d = rp.get("default", False)
            if not isinstance(d, bool):
                _err(errors, f"{where}: bool default must be true or false")
                d = False
            pmin, pmax, pdef = 0.0, 1.0, 1.0 if d else 0.0
            smoothing = 0.0
        else:
            kind = int if ptype == "int" else float
            if "choices" in rp:
                _err(errors, f"{where}: 'choices' is only for choice parameters")
            for k in ("min", "max", "default"):
                if k not in rp:
                    _err(errors, f"{where}: missing '{k}'")
            pmin = _num(errors, f"{where}.min", rp.get("min", 0), kind)
            pmax = _num(errors, f"{where}.max", rp.get("max", 1), kind)
            pdef = _num(errors, f"{where}.default", rp.get("default", 0), kind)
            pmin = float(pmin if pmin is not None else 0)
            pmax = float(pmax if pmax is not None else 1)
            pdef = float(pdef if pdef is not None else pmin)
            if pmax <= pmin:
                _err(errors, f"{where}: max must be greater than min")
            elif not pmin <= pdef <= pmax:
                _err(errors, f"{where}: default {pdef:g} is outside [{pmin:g}, {pmax:g}]")
            if scale == "log" and pmin <= 0:
                _err(errors, f"{where}: scale = \"log\" needs min > 0")
            if ptype == "int" and scale == "log":
                _err(errors, f"{where}: log scale is only for float parameters")
            if ptype == "int" and pmax - pmin > 10000:
                _err(errors, f"{where}: int range too large")
            if ptype == "float" and scale == "linear" and unit == "Hz" and pmin > 0 and pmax / pmin >= 20:
                warnings.append(f"{where}: frequency spans {pmax / pmin:.0f}x — consider scale = \"log\"")
            smoothing = rp.get("smoothing_ms", 20.0 if ptype == "float" else 0.0)
            if ptype == "int" and "smoothing_ms" in rp:
                _err(errors, f"{where}: smoothing_ms is only for float parameters")
            sv = _num(errors, f"{where}.smoothing_ms", smoothing)
            smoothing = float(sv if sv is not None else 20.0)
            if not 0 <= smoothing <= 2000:
                _err(errors, f"{where}: smoothing_ms must be within 0..2000")

        control = rp.get("control", CONTROLS[ptype][0])
        if control not in CONTROLS[ptype]:
            _err(errors, f"{where}: control for a {ptype} parameter must be one of "
                         f"{', '.join(repr(c) for c in CONTROLS[ptype])}")
            control = CONTROLS[ptype][0]
        if ptype == "choice" and control in SWITCH_CONTROLS and len(choices) != 2:
            _err(errors, f"{where}: '{control}' works for a choice with exactly 2 options (off = the first)")
            control = CONTROLS[ptype][0]
        steps = len(choices) if ptype == "choice" else int(pmax - pmin) + 1 if ptype == "int" else 0
        if control == "selector" and not 2 <= steps <= SELECTOR_MAX_STEPS:
            _err(errors, f"{where}: a selector shows every position; use it for 2-{SELECTOR_MAX_STEPS} steps")
            control = CONTROLS[ptype][0]

        pknob = rp.get("knob_style", "")
        if pknob and pknob not in KNOB_STYLES:
            _err(errors, f"{where}: knob_style must be one of {', '.join(repr(k) for k in KNOB_STYLES)}")
            pknob = ""
        elif pknob and control not in KNOB_CONTROLS:
            _err(errors, f"{where}: knob_style only applies to knob controls")
            pknob = ""

        prm = Param(sym, pname, short, ptype, pmin, pmax, pdef, unit, scale, smoothing, desc, choices, control,
                    pknob)
        for key in ("x", "y", "size"):
            if key in rp:
                v = _num(errors, f"{where}.{key}", rp[key])
                if v is not None and not (0 <= v <= 4000 if key != "size" else 8 <= v <= 1000):
                    _err(errors, f"{where}.{key}: out of range")
                setattr(prm, key, v)
        if layout == "free" and (prm.x is None or prm.y is None):
            _err(errors, f"{where}: layout = \"free\" needs x and y (the top-left of the control, in pixels)")
        elif layout == "flow" and ("x" in rp or "y" in rp):
            warnings.append(f"{where}: x/y are only used with layout = \"free\"")
        for key in ("label", "value"):
            if key in rp:
                if not isinstance(rp[key], bool):
                    _err(errors, f"{where}.{key}: true or false")
                else:
                    setattr(prm, key, rp[key])
        prm.label_color = color(where, rp, "label_color")
        prm.value_color = color(where, rp, "value_color")
        if "knob_image" in rp:
            if control not in KNOB_CONTROLS:
                _err(errors, f"{where}: knob_image only applies to knob controls")
            else:
                prm.knob_image = image(f"{where}.knob_image", rp["knob_image"])
        if "knob_frames" in rp:
            kf = rp["knob_frames"]
            if not isinstance(kf, int) or isinstance(kf, bool) or not 1 <= kf <= 256:
                _err(errors, f"{where}.knob_frames: 1 (rotating image) or the filmstrip's frame count (2-256)")
            else:
                prm.knob_frames = kf
        params.append(prm)

    # [[ui.group]] (needs the parameter symbols)
    groups: list[UiGroup] = []
    index_of = {p.symbol: i for i, p in enumerate(params)}
    grouped: set[str] = set()
    for gi, rg in enumerate(raw_groups):
        where = f"[[ui.group]] #{gi + 1}"
        if not isinstance(rg, dict):
            _err(errors, f"{where}: must be a table")
            continue
        _check_keys(errors, where, rg, {"title", "params", "columns"})
        title = rg.get("title", "")
        if not isinstance(title, str) or len(title) > 40:
            _err(errors, f"{where}: title must be a string of at most 40 characters")
            title = ""
        gcols = rg.get("columns", 0)
        if "columns" in rg and (not isinstance(gcols, int) or isinstance(gcols, bool) or not 1 <= gcols <= 8):
            _err(errors, f"{where}: columns must be an integer between 1 and 8")
            gcols = 0
        members = rg.get("params", [])
        if not isinstance(members, list) or not members or not all(isinstance(m, str) for m in members):
            _err(errors, f"{where}: params = [\"symbol\", ...] listing at least one parameter")
            members = []
        idxs = []
        for m in members:
            if m not in index_of:
                _err(errors, f"{where}: no parameter with symbol '{m}'")
            elif m in grouped:
                _err(errors, f"{where}: parameter '{m}' is already in another group")
            else:
                grouped.add(m)
                idxs.append(index_of[m])
        groups.append(UiGroup(title, gcols, idxs))
    if groups:
        missing = [p.symbol for p in params if p.symbol not in grouped]
        if missing:
            warnings.append(f"[[ui.group]]: {', '.join(missing)} not in any group; shown at the end")

    # [dsp]
    dsp = data.get("dsp")
    code = ""
    code_path = path
    code_line = 1
    if not isinstance(dsp, dict):
        _err(errors, "missing [dsp] table with the kernel code")
    else:
        _check_keys(errors, "[dsp]", dsp, {"code", "file"})
        if ("code" in dsp) == ("file" in dsp):
            _err(errors, "[dsp]: provide exactly one of `code` (inline C++) or `file` (path to a .hpp)")
        elif "code" in dsp:
            code = dsp["code"]
            code_line = _find_code_line(text)
        else:
            code_path = (path.parent / dsp["file"]).resolve()
            if not code_path.exists():
                _err(errors, f"[dsp].file: {code_path} not found")
            else:
                code = code_path.read_text(encoding="utf-8")
        if not isinstance(code, str):
            _err(errors, "[dsp].code must be a string")
            code = ""

    recipe = Recipe(path=path, id=pid, name=name, brand=brand, brand_id=brand_id, unique_id=unique_id,
                    version=version, description=description, license=license_, homepage=homepage,
                    category=category, inputs=io["inputs"], outputs=io["outputs"], clap_id=clap_id, uri=uri,
                    accent=accent, columns=columns, columns_set="columns" in ui, theme=theme, background=background, subtitle=subtitle,
                    knob_style=knob_style, knob_color=knob_color, knob_ticks=knob_ticks, layout=layout,
                    background_image=background_image, image_scale=image_scale, knob_image=knob_image,
                    knob_frames=knob_frames, free_size=free_size, labels=labels, title_color=title_color,
                    label_color=label_color, value_color=value_color, images=images,
                    groups=groups, params=params, code=code, code_path=code_path,
                    code_line=code_line, warnings=warnings)

    if code:
        static_check_code(recipe, errors, warnings)
    for p in params:
        img = p.knob_image or (knob_image if p.control in KNOB_CONTROLS else "")
        frames = p.knob_frames or knob_frames
        if img and img in images and frames > 1:
            w, h = images[img]
            if (h % frames if h >= w else w % frames) != 0:
                _err(errors, f"[[param]] '{p.symbol}': {img} is {w}x{h}, which doesn't split into {frames} frames")

    if errors:
        raise RecipeError(f"{path}:\n  - " + "\n  - ".join(errors))
    return recipe


def _strip_comments_and_strings(code: str) -> str:
    out = []
    i, n = 0, len(code)
    while i < n:
        c = code[i]
        if code.startswith("//", i):
            j = code.find("\n", i)
            i = n if j < 0 else j
        elif code.startswith("/*", i):
            j = code.find("*/", i + 2)
            seg = code[i: (n if j < 0 else j + 2)]
            out.append("\n" * seg.count("\n"))
            i = n if j < 0 else j + 2
        elif c in "\"'":
            j = i + 1
            while j < n and code[j] != c:
                j += 2 if code[j] == "\\" else 1
            out.append(c + c)
            i = j + 1
        else:
            out.append(c)
            i += 1
    return "".join(out)


def _function_body(code: str, name: str) -> str | None:
    m = re.search(r"\b" + name + r"\s*\([^;{]*\)\s*(const\s*)?(noexcept\s*)?(override\s*)?\{", code)
    if not m:
        return None
    depth, i = 1, m.end()
    while i < len(code) and depth:
        if code[i] == "{":
            depth += 1
        elif code[i] == "}":
            depth -= 1
        i += 1
    return code[m.end(): i - 1]


def static_check_code(r: Recipe, errors: list[str], warnings: list[str]) -> None:
    clean = _strip_comments_and_strings(r.code)

    if not re.search(r"\b(class|struct)\s+Kernel\b", clean):
        errors.append("[dsp]: the code must define `class Kernel` (or `struct Kernel`)")

    for line in r.code.splitlines():
        m = re.match(r"\s*#\s*include\s*[<\"]([^>\"]+)[>\"]", line)
        if m and m.group(1) not in ALLOWED_INCLUDES:
            errors.append(f"[dsp]: #include <{m.group(1)}> is not allowed "
                          f"(allowed: {', '.join(sorted(ALLOWED_INCLUDES))})")
        elif re.match(r"\s*#\s*(define|pragma|if|ifdef|ifndef)\b", line):
            warnings.append(f"[dsp]: preprocessor directive '{line.strip()}' — prefer constexpr constants")

    body = _function_body(clean, "process")
    if body is None:
        errors.append("[dsp]: could not find the body of Kernel::process(...)")
    else:
        for pat, what in FORBIDDEN_IN_PROCESS:
            m = re.search(pat, body)
            if m:
                errors.append(f"[dsp]: process() contains {what}: `{m.group(0).strip()}` — not real-time safe")

    for which, count in (("in", r.inputs), ("out", r.outputs)):
        for m in re.finditer(r"\b" + which + r"\s*\[\s*(\d+)\s*\]", clean):
            if int(m.group(1)) >= count:
                errors.append(f"[dsp]: `{m.group(0)}` but the plugin declares {count} "
                              f"{'input' if which == 'in' else 'output'}{'s' if count > 1 else ''}")

    for p in r.params:
        if p.symbol and not re.search(r"\b" + re.escape(p.symbol) + r"\b", clean):
            warnings.append(f"parameter '{p.symbol}' is never used by the kernel")


def has_latency(r: Recipe) -> bool:
    return bool(re.search(r"\blatency\s*\(\s*\)\s*const", _strip_comments_and_strings(r.code)))


# ----------------------------------------------------------------------------------------------------------------------
# Code generation


def c_str(s: str) -> str:
    return '"' + s.replace("\\", "\\\\").replace('"', '\\"').replace("\n", "\\n") + '"'


def c_float(v: float) -> str:
    s = repr(float(v))
    if "e" not in s and "." not in s:
        s += ".0"
    return s + "f"


def default_columns(r: Recipe) -> int:
    """Columns for groups that don't set their own. Five or six plain knobs read better as two rows of three."""
    n = max(1, len(r.params))
    cols = min(r.columns, n)
    if not r.groups and not r.columns_set and n in (5, 6) and r.columns >= 4:
        cols = 3
    return cols


def effective_groups(r: Recipe) -> list[UiGroup]:
    """The groups the editor draws: the recipe's, plus an untitled group for any parameter left out."""
    groups = list(r.groups)
    used = {i for g in groups for i in g.params}
    rest = [i for i in range(len(r.params)) if i not in used]
    if rest or not groups:
        groups.append(UiGroup("", 0, rest))
    return groups


def cell_size(p: Param) -> tuple[int, int]:
    if p.control == "buttons":
        seg = max(len(c) for c in p.choices) * 7 + 16
        return min(472, max(118, 16 + len(p.choices) * seg)), 84
    if p.control == "radio":
        return 118, 24 + 22 * len(p.choices) + 12
    return UI_CELL[p.control]


def layout_rows(r: Recipe, g: UiGroup, avail: float) -> list[list[int]]:
    """Greedy rows: a row ends at the group's column limit or when the next control doesn't fit."""
    cols = g.columns or default_columns(r)
    rows: list[list[int]] = []
    row: list[int] = []
    row_w = 0
    for i in g.params:
        w = cell_size(r.params[i])[0]
        if row and (len(row) >= cols or row_w + w > avail):
            rows.append(row)
            row, row_w = [], 0
        row.append(i)
        row_w += w
    if row:
        rows.append(row)
    return rows


def knob_image_of(r: Recipe, p: Param) -> tuple[str, int]:
    """The image a knob is drawn with ("" = drawn by code) and its frame count."""
    if p.control not in KNOB_CONTROLS:
        return "", 1
    return (p.knob_image or r.knob_image), (p.knob_frames or r.knob_frames)


def free_box(r: Recipe, p: Param) -> tuple[float, float]:
    """Control box (width, height) in the free layout, before scaling."""
    if p.control in KNOB_CONTROLS:
        img, frames = knob_image_of(r, p)
        d = FREE_BOX[p.control][0]
        if img:
            w, h = r.images[img]
            frame_w = w if h >= w else w / frames
            d = frame_w / r.image_scale
        d = p.size or d
        return d, d
    if p.control == "buttons":
        return p.size or cell_size(p)[0] - 16, 28
    if p.control == "radio":
        return p.size or 100, 22 * len(p.choices)
    w, h = FREE_BOX[p.control]
    if p.control == "toggle" or not p.size:
        return w, h
    if p.control == "vslider":
        return w, p.size
    if p.control in FREE_UNIFORM:
        return p.size, p.size * h / w
    return p.size, h


def free_base_size(r: Recipe) -> tuple[int, int]:
    if r.free_size:
        return r.free_size
    w, h = r.images[r.background_image]
    return round(w / r.image_scale), round(h / r.image_scale)


def ui_size(r: Recipe) -> tuple[int, int, int]:
    if r.layout == "free":
        w, h = free_base_size(r)
        return w, h, default_columns(r)
    groups = effective_groups(r)
    header = UI_HEADER_H + (UI_SUBTITLE_H if r.subtitle else 0)
    natural = max((sum(cell_size(r.params[i])[0] for i in row)
                   for g in groups for row in layout_rows(r, g, float("inf"))), default=0)
    width = max(UI_MIN_WIDTH, natural + 2 * UI_PAD)
    height = header + 8
    for g in groups:
        if not g.params:
            continue
        if g.title:
            height += UI_TITLE_H
        for row in layout_rows(r, g, width - 2 * UI_PAD):
            height += max(cell_size(r.params[i])[1] for i in row)
        height += UI_GROUP_GAP
    height += UI_BOTTOM - UI_GROUP_GAP
    return width, height, default_columns(r)


# ----------------------------------------------------------------------------------------------------------------------
# Editor layout as data (used by Plugin Studio's interface editor)


def _hex(v: int | None) -> str | None:
    return f"#{v:06X}" if v is not None else None


def ui_model(r: Recipe) -> dict:
    width, height, cols = ui_size(r)
    return {
        "id": r.id,
        "name": r.name,
        "brand": r.brand,
        "width": width,
        "height": height,
        "default_columns": cols,
        "ui": {
            "accent": f"#{r.accent:06X}",
            "columns": r.columns,
            "columns_set": r.columns_set,
            "theme": r.theme,
            "background": f"#{r.background:06X}" if r.background is not None else None,
            "subtitle": r.subtitle,
            "knob_style": r.knob_style,
            "knob_color": f"#{r.knob_color:06X}" if r.knob_color is not None else None,
            "knob_ticks": r.knob_ticks,
            "layout": r.layout,
            "background_image": r.background_image,
            "image_scale": r.image_scale,
            "knob_image": r.knob_image,
            "knob_frames": r.knob_frames,
            "size": list(r.free_size) if r.free_size else None,
            "labels": r.labels,
            "title_color": _hex(r.title_color),
            "label_color": _hex(r.label_color),
            "value_color": _hex(r.value_color),
        },
        "images": {k: list(v) for k, v in r.images.items()},
        "groups": [{"title": g.title, "columns": g.columns, "params": [r.params[i].symbol for i in g.params]}
                   for g in effective_groups(r) if g.params],
        "params": [{"symbol": p.symbol, "name": p.name, "short_name": p.short_name, "type": p.type, "min": p.min,
                    "max": p.max, "default": p.default, "unit": p.unit, "scale": p.scale, "choices": p.choices,
                    "control": p.control, "knob_style": p.knob_style, "description": p.description,
                    "x": p.x, "y": p.y, "size": p.size, "label": p.label, "value": p.value,
                    "knob_image": p.knob_image, "knob_frames": p.knob_frames,
                    "label_color": _hex(p.label_color), "value_color": _hex(p.value_color)}
                   for p in r.params],
        "controls": CONTROLS,
        "switch_controls": SWITCH_CONTROLS,
        "knob_styles": KNOB_STYLES,
        "geometry": {"pad": UI_PAD, "header": UI_HEADER_H, "subtitle": UI_SUBTITLE_H, "title": UI_TITLE_H,
                     "gap": UI_GROUP_GAP, "bottom": UI_BOTTOM, "min_width": UI_MIN_WIDTH, "cells": UI_CELL,
                     "free_box": FREE_BOX},
    }


_TABLE_RE = re.compile(r"^\s*(\[\[?)\s*([A-Za-z0-9_.\- ]+?)\s*\]\]?\s*(#.*)?$")


def _toml_tables(lines: list[str]) -> list[tuple[int, str]]:
    """(line index, table name) of every table header, skipping the insides of multi-line strings."""
    out = []
    inside = None
    for i, line in enumerate(lines):
        if inside:
            if line.count(inside) % 2 == 1:
                inside = None
            continue
        m = _TABLE_RE.match(line)
        if m:
            out.append((i, ("[[" if m.group(1) == "[[" else "[") + m.group(2).replace(" ", "")))
            continue
        for delim in ("\'\'\'", '"""'):
            if line.count(delim) % 2 == 1:
                inside = delim
                break
    return out


def _toml_str(v: str) -> str:
    return json.dumps(v, ensure_ascii=False)


def apply_ui(text: str, ui: dict) -> str:
    """Rewrites the [ui] table, the [[ui.group]] tables and each parameter's `control` key from `ui`
    (the shape ui_model() returns), leaving everything else in the recipe untouched."""
    lines = text.split("\n")

    # 1. drop [ui] and [[ui.group]]
    tables = _toml_tables(lines)
    keep = [True] * len(lines)
    for n, (start, name) in enumerate(tables):
        if name in ("[ui", "[[ui.group"):
            end = tables[n + 1][0] if n + 1 < len(tables) else len(lines)
            # comments right above the next table belong to it
            while end - 1 > start and (not lines[end - 1].strip() or lines[end - 1].lstrip().startswith("#")):
                end -= 1
            for k in range(start, end):
                keep[k] = False
    lines = [l for l, k in zip(lines, keep) if k]

    # 2. per-parameter keys
    u = ui.get("ui", {})
    free = u.get("layout") == "free"
    wanted_by_symbol: dict[str, dict] = {}
    for prm in ui.get("params", []):
        ptype = prm.get("type", "float")
        control = prm.get("control") or CONTROLS.get(ptype, ("",))[0]
        is_knob = control in KNOB_CONTROLS
        num = lambda v: None if v is None else (int(round(v)) if abs(v - round(v)) < 1e-6 else round(float(v), 1))
        wanted_by_symbol[prm["symbol"]] = {
            "control": control if control != CONTROLS.get(ptype, ("",))[0] else None,
            "knob_style": (prm.get("knob_style") or None) if is_knob else None,
            "x": num(prm.get("x")) if free else None,
            "y": num(prm.get("y")) if free else None,
            "size": num(prm.get("size")) if free and prm.get("size") else None,
            "label": prm.get("label"),
            "value": prm.get("value"),
            "label_color": prm.get("label_color") or None,
            "value_color": prm.get("value_color") or None,
            "knob_image": (prm.get("knob_image") or None) if is_knob else None,
            "knob_frames": (prm.get("knob_frames") or None) if is_knob else None,
        }

    def toml_value(v) -> str:
        if isinstance(v, bool):
            return "true" if v else "false"
        if isinstance(v, (int, float)):
            return str(v)
        return _toml_str(v)

    tables = _toml_tables(lines)
    edits: list[tuple[int, int, str | None]] = []  # (line, 0 = replace/delete or 1.. = insert order, text)
    for n, (start, name) in enumerate(tables):
        if name != "[[param":
            continue
        end = tables[n + 1][0] if n + 1 < len(tables) else len(lines)
        sym = None
        key_width = 0
        found: dict[str, int] = {}
        last_key = start
        for k in range(start + 1, end):
            m = re.match(r'^(\s*)([A-Za-z_]+)(\s*)=\s*(.*)$', lines[k])
            if not m:
                continue
            last_key = k
            key, value = m.group(2), m.group(4)
            if key == "symbol":
                sym = (re.match(r'"([^"]*)"', value) or [None, None])[1]
                key_width = len(m.group(2)) + len(m.group(3))
            elif key in PARAM_UI_KEYS:
                found[key] = k
        wanted = wanted_by_symbol.get(sym)
        if wanted is None:
            continue
        order = 0
        for key in PARAM_UI_KEYS:
            v = wanted.get(key)
            new = f"{key.ljust(max(key_width, len(key) + 1))}= {toml_value(v)}" if v is not None else None
            if key in found:
                edits.append((found[key], 0, new))
            elif new:
                order += 1
                edits.append((last_key + 1, order, new))
    for idx, mode, new in sorted(edits, key=lambda e: (e[0], e[1]), reverse=True):
        if mode == 0:
            if new is None:
                del lines[idx]
            else:
                lines[idx] = new
        else:
            lines.insert(idx, new)

    # 3. the new [ui] block, before the first parameter
    entries = [("accent", _toml_str(u.get("accent", DEFAULT_ACCENT)))]
    if u.get("columns_set") or int(u.get("columns", 4)) != 4:
        entries.append(("columns", str(int(u.get("columns", 4)))))
    if u.get("theme", "dark") != "dark":
        entries.append(("theme", _toml_str(u["theme"])))
    if u.get("background"):
        entries.append(("background", _toml_str(u["background"])))
    if u.get("subtitle"):
        entries.append(("subtitle", _toml_str(u["subtitle"])))
    if u.get("knob_style", "modern") != "modern":
        entries.append(("knob_style", _toml_str(u["knob_style"])))
    if u.get("knob_color"):
        entries.append(("knob_color", _toml_str(u["knob_color"])))
    if u.get("knob_ticks"):
        entries.append(("knob_ticks", "true"))
    if u.get("layout", "flow") != "flow":
        entries.append(("layout", _toml_str(u["layout"])))
    if u.get("background_image"):
        entries.append(("background_image", _toml_str(u["background_image"])))
    if u.get("image_scale", 1) != 1:
        sc = float(u["image_scale"])
        entries.append(("image_scale", str(int(sc)) if sc.is_integer() else str(sc)))
    if u.get("knob_image"):
        entries.append(("knob_image", _toml_str(u["knob_image"])))
        if int(u.get("knob_frames", 1)) != 1:
            entries.append(("knob_frames", str(int(u["knob_frames"]))))
    if u.get("size") and (u.get("layout") == "free"):
        entries.append(("size", f"[{int(u['size'][0])}, {int(u['size'][1])}]"))
    if u.get("labels") is False:
        entries.append(("labels", "false"))
    for key in ("title_color", "label_color", "value_color"):
        if u.get(key):
            entries.append((key, _toml_str(u[key])))
    width = max(len(k) for k, _ in entries)
    block = ["[ui]"] + [f"{k.ljust(width)} = {v}" for k, v in entries] + [""]

    groups = [g for g in ui.get("groups", []) if g.get("params")]
    order = [p["symbol"] for p in ui.get("params", [])]
    trivial = (len(groups) == 1 and not groups[0].get("title") and not groups[0].get("columns")
               and (not order or groups[0]["params"] == order))
    if groups and not trivial and u.get("layout") != "free":
        for g in groups:
            block.append("[[ui.group]]")
            if g.get("title"):
                block.append(f'title   = {_toml_str(g["title"])}')
            block.append("params  = [" + ", ".join(_toml_str(p) for p in g["params"]) + "]")
            if g.get("columns"):
                block.append(f'columns = {int(g["columns"])}')
            block.append("")

    tables = _toml_tables(lines)
    at = next((i for i, name in tables if name == "[[param"), None)
    if at is None:
        at = next((i for i, name in tables if name == "[dsp"), len(lines))
    # keep comment lines that sit right above the first parameter attached to it
    while at > 0 and lines[at - 1].lstrip().startswith("#"):
        at -= 1
    if at > 0 and lines[at - 1].strip():
        block.insert(0, "")
    lines[at:at] = block

    out = "\n".join(lines)
    return re.sub(r"\n{3,}", "\n\n", out)


def gen_plugin_info(r: Recipe, with_ui: bool) -> str:
    lv2, vst3, clap = CATEGORIES[r.category]
    io = "Stereo" if r.outputs == 2 else "Mono"
    clap_features = ['"audio-effect"'] + ([f'"{clap}"'] if clap else []) + ['"stereo"' if r.outputs == 2 else '"mono"']
    width, height, cols = ui_size(r)
    uid = ",".join(f"'{c}'" for c in r.unique_id)
    lines = [
        "// Generated by tools/recipe.py from " + r.path.name + " — do not edit.",
        "#pragma once",
        "",
        f"#define DISTRHO_PLUGIN_BRAND {c_str(r.brand)}",
        f"#define DISTRHO_PLUGIN_NAME {c_str(r.name)}",
        f"#define DISTRHO_PLUGIN_URI {c_str(r.uri)}",
        f"#define DISTRHO_PLUGIN_CLAP_ID {c_str(r.clap_id)}",
        f"#define DISTRHO_PLUGIN_BRAND_ID {r.brand_id}",
        f"#define DISTRHO_PLUGIN_UNIQUE_ID {r.unique_id}",
        "",
        f"#define DISTRHO_PLUGIN_HAS_UI {1 if with_ui else 0}",
        "#define DISTRHO_PLUGIN_IS_RT_SAFE 1",
        f"#define DISTRHO_PLUGIN_NUM_INPUTS {r.inputs}",
        f"#define DISTRHO_PLUGIN_NUM_OUTPUTS {r.outputs}",
        f"#define DISTRHO_PLUGIN_WANT_LATENCY {1 if has_latency(r) else 0}",
        "",
        f"#define DISTRHO_PLUGIN_LV2_CATEGORY {c_str(lv2)}",
        f"#define DISTRHO_PLUGIN_VST3_CATEGORIES {c_str(vst3 + '|' + io)}",
        f"#define DISTRHO_PLUGIN_CLAP_FEATURES {', '.join(clap_features)}",
        "",
        f"#define COOKBOOK_LABEL {c_str(r.label)}",
        f"#define COOKBOOK_DESCRIPTION {c_str(r.description)}",
        f"#define COOKBOOK_MAKER {c_str(r.brand)}",
        f"#define COOKBOOK_HOMEPAGE {c_str(r.homepage)}",
        f"#define COOKBOOK_LICENSE {c_str(r.license)}",
        f"#define COOKBOOK_VERSION_MAJOR {r.version[0]}",
        f"#define COOKBOOK_VERSION_MINOR {r.version[1]}",
        f"#define COOKBOOK_VERSION_MICRO {r.version[2]}",
        f"#define COOKBOOK_UNIQUE_ID_CHARS {uid}",
    ]
    if with_ui:
        lines += [
            "",
            "#define DISTRHO_UI_USE_CUSTOM 1",
            '#define DISTRHO_UI_CUSTOM_INCLUDE_PATH "DearImGui.hpp"',
            "#define DISTRHO_UI_CUSTOM_WIDGET_TYPE DGL_NAMESPACE::ImGuiTopLevelWidget",
            f"#define DISTRHO_UI_DEFAULT_WIDTH {width}",
            f"#define DISTRHO_UI_DEFAULT_HEIGHT {height}",
            "#define DISTRHO_UI_USER_RESIZABLE 1",
            f"#define COOKBOOK_UI_ACCENT 0x{r.accent:06X}",
            f"#define COOKBOOK_UI_COLUMNS {cols}",
            f"#define COOKBOOK_UI_LIGHT {1 if r.theme == 'light' else 0}",
            f"#define COOKBOOK_UI_BACKGROUND {('0x%06X' % r.background) if r.background is not None else -1}",
            f"#define COOKBOOK_UI_SUBTITLE {c_str(r.subtitle)}",
        ]
    return "\n".join(lines) + "\n"


def gen_meta(r: Recipe) -> str:
    n = len(r.params)
    out = [
        "// Generated by tools/recipe.py from " + r.path.name + " — do not edit.",
        "#pragma once",
        "",
        "#include <cstdint>",
        '#include "cookbook/ParamInfo.hpp"',
        "",
        "namespace recipe {",
        "",
        f"constexpr uint32_t kNumInputs = {r.inputs};",
        f"constexpr uint32_t kNumOutputs = {r.outputs};",
        "",
    ]
    for p in r.params:
        if p.type == "choice":
            out.append(f"constexpr const char* kChoices_{p.symbol}[] = {{ {', '.join(c_str(c) for c in p.choices)} }};")
    if any(p.type == "choice" for p in r.params):
        out.append("")

    out.append("// Parameter values handed to Kernel::process() every block.")
    out.append("struct Params")
    out.append("{")
    for p in r.params:
        if p.type == "float":
            smooth = f"smoothed {p.smoothing_ms:g} ms" if p.smoothing_ms > 0 else "not smoothed"
            unit = f" {p.unit}" if p.unit else ""
            out.append(f"    const float* {p.symbol} = nullptr; // {p.name}: one value per sample, "
                       f"{p.min:g}..{p.max:g}{unit} ({p.scale}, {smooth})")
        elif p.type == "int":
            out.append(f"    int {p.symbol} = 0; // {p.name}: {int(p.min)}..{int(p.max)}")
        elif p.type == "bool":
            out.append(f"    bool {p.symbol} = false; // {p.name}")
        else:
            labels = ", ".join(f"{i} = {c}" for i, c in enumerate(p.choices))
            out.append(f"    int {p.symbol} = 0; // {p.name}: {labels}")
    out.append("};")
    out.append("")

    out.append("struct Meta")
    out.append("{")
    out.append(f"    static constexpr uint32_t kNumInputs = {r.inputs};")
    out.append(f"    static constexpr uint32_t kNumOutputs = {r.outputs};")
    out.append(f"    static constexpr uint32_t kNumParams = {n};")
    out.append("    using Params = recipe::Params;")
    out.append("")
    out.append(f"    static constexpr cookbook::ParamInfo kParams[{max(1, n)}] = {{")
    if n == 0:
        out.append('        { "none", "none", "", "", "", cookbook::ParamType::Float, cookbook::Scale::Linear, '
                   "0.f, 1.f, 0.f, 0.f, nullptr, 0, cookbook::Control::Knob, cookbook::KnobStyle::Modern },")
    for p in r.params:
        ptype = {"float": "Float", "int": "Int", "bool": "Bool", "choice": "Choice"}[p.type]
        scale = "Log" if p.scale == "log" else "Linear"
        choices = f"kChoices_{p.symbol}, {len(p.choices)}" if p.type == "choice" else "nullptr, 0"
        control = "".join(w.capitalize() for w in p.control.split("-"))
        knob = (p.knob_style or r.knob_style).capitalize()
        out.append(f"        {{ {c_str(p.symbol)}, {c_str(p.name)}, {c_str(p.short_name)}, {c_str(p.unit)}, "
                   f"{c_str(p.description)}, cookbook::ParamType::{ptype}, cookbook::Scale::{scale}, "
                   f"{c_float(p.min)}, {c_float(p.max)}, {c_float(p.default)}, {c_float(p.smoothing_ms)}, {choices}, "
                   f"cookbook::Control::{control}, cookbook::KnobStyle::{knob} }},")
    out.append("    };")
    out.append("")
    groups = [g for g in effective_groups(r) if g.params] or [UiGroup("", 0, [0])]
    for gi, g in enumerate(groups):
        out.append(f"    static constexpr uint32_t kGroup{gi}[{len(g.params)}] = {{ {', '.join(map(str, g.params))} }};")
    out.append(f"    static constexpr uint32_t kNumGroups = {len(groups) if n else 0};")
    knob_color = f"0x{r.knob_color:06X}" if r.knob_color is not None else "-1"
    out.append(f"    static constexpr int64_t kKnobColor = {knob_color}; // knob body color, -1 = the style's own")
    out.append(f"    static constexpr bool kKnobTicks = {'true' if r.knob_ticks else 'false'};")
    cc = lambda v: f"0x{v:06X}" if v is not None else "-1"
    out.append(f"    // text colors (0xRRGGBB, -1 = the theme's): plugin name, labels (names, group titles, brand), values")
    out.append(f"    static constexpr int32_t kTitleColor = {cc(r.title_color)}, kLabelColor = {cc(r.label_color)}, "
               f"kValueColor = {cc(r.value_color)};")
    out.append("")
    images = sorted(r.images)
    base_w, base_h = free_base_size(r) if r.layout == "free" else ui_size(r)[:2]
    out.append(f"    static constexpr bool kFreeLayout = {'true' if r.layout == 'free' else 'false'};")
    out.append(f"    static constexpr float kBaseWidth = {c_float(base_w)}, kBaseHeight = {c_float(base_h)};")
    out.append(f"    static constexpr float kImageScale = {c_float(r.image_scale)};")
    bg = images.index(r.background_image) if r.background_image else -1
    out.append(f"    static constexpr int32_t kBackgroundImage = {bg}; // index into recipe::kAssets, -1 = none")
    out.append("    // free layout boxes (x, y, w, h), label/value visibility, knob image index and frames")
    out.append(f"    static constexpr cookbook::UiParam kUiParams[{max(1, n)}] = {{")
    if n == 0:
        out.append("        { 0.f, 0.f, 0.f, 0.f, false, false, -1, 1, -1, -1 },")
    for p in r.params:
        img, frames = knob_image_of(r, p)
        w, h = free_box(r, p) if r.layout == "free" else (0.0, 0.0)
        show_label = p.label if p.label is not None else r.labels
        show_value = p.value if p.value is not None else r.labels
        out.append(f"        {{ {c_float(p.x or 0)}, {c_float(p.y or 0)}, {c_float(w)}, {c_float(h)}, "
                   f"{'true' if show_label else 'false'}, {'true' if show_value else 'false'}, "
                   f"{images.index(img) if img else -1}, {frames}, "
                   f"{cc(p.label_color if p.label_color is not None else r.label_color)}, "
                   f"{cc(p.value_color if p.value_color is not None else r.value_color)} }}, // {p.symbol}")
    out.append("    };")
    out.append(f"    static constexpr cookbook::UiGroup kGroups[{len(groups)}] = {{")
    for gi, g in enumerate(groups):
        out.append(f"        {{ {c_str(g.title)}, {g.columns or default_columns(r)}, kGroup{gi}, {len(g.params)} }},")
    out.append("    };")
    out.append("")
    out.append("    // floatBuffers: per-sample buffers of the float parameters, in declaration order.")
    out.append("    // values: current (target) value of every parameter, by parameter index.")
    out.append("    static Params bind(const float* const* floatBuffers, const float* values) noexcept")
    out.append("    {")
    out.append("        Params p;")
    slot = 0
    for i, prm in enumerate(r.params):
        if prm.type == "float":
            out.append(f"        p.{prm.symbol} = floatBuffers[{slot}];")
            slot += 1
        elif prm.type == "bool":
            out.append(f"        p.{prm.symbol} = values[{i}] > 0.5f;")
        else:
            out.append(f"        p.{prm.symbol} = static_cast<int>(values[{i}] + 0.5f);")
    if n == 0:
        out.append("        (void)floatBuffers; (void)values;")
    out.append("        return p;")
    out.append("    }")
    out.append("};")
    out.append("")
    out.append("} // namespace recipe")
    return "\n".join(out) + "\n"


def gen_kernel(r: Recipe) -> str:
    hoisted: list[str] = []
    body_lines = []
    for line in r.code.splitlines():
        if re.match(r"\s*#\s*include\b", line):
            hoisted.append(line.strip())
            body_lines.append("")  # keep line numbers aligned
        else:
            body_lines.append(line)
    src = r.code_path.resolve().as_posix()
    out = [
        "// Generated by tools/recipe.py from " + r.path.name + " — do not edit.",
        "#pragma once",
        "",
        "#include <algorithm>",
        "#include <array>",
        "#include <cmath>",
        "#include <cstdint>",
        "#include <cstring>",
        "#include <vector>",
        *sorted(set(hoisted)),
        "",
        '#include "RecipeMeta.hpp"',
        '#include "cookbook/dsp.hpp"',
        "",
        "#if defined(__GNUC__)",
        "# pragma GCC diagnostic push",
        '# pragma GCC diagnostic ignored "-Wunused-parameter"',
        "#endif",
        "",
        "namespace recipe {",
        "using namespace cookbook;",
        "",
        f'#line {r.code_line} "{src}"',
        *body_lines,
        "",
        "} // namespace recipe",
        "",
        "#if defined(__GNUC__)",
        "# pragma GCC diagnostic pop",
        "#endif",
    ]
    return "\n".join(out) + "\n"


def write_if_changed(path: Path, content: str) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    if path.exists() and path.read_text(encoding="utf-8") == content:
        return
    path.write_text(content, encoding="utf-8")


def gen_assets(r: Recipe) -> str:
    """The recipe's images as byte arrays (PNG, decoded at runtime). Only the editor includes this."""
    out = ["// Generated by tools/recipe.py from " + r.path.name + " — do not edit.", "#pragma once", "",
           '#include "cookbook/ParamInfo.hpp"', "", "namespace recipe {", ""]
    names = sorted(r.images)
    for i, name in enumerate(names):
        data = (r.path.parent / name).read_bytes()
        out.append(f"// {name}")
        out.append(f"static const unsigned char kAssetData{i}[{len(data)}] = {{")
        for k in range(0, len(data), 32):
            out.append("    " + ",".join(str(b) for b in data[k:k + 32]) + ",")
        out.append("};")
    out.append("")
    out.append(f"static constexpr uint32_t kNumAssets = {len(names)};")
    out.append(f"static const cookbook::Asset kAssets[{max(1, len(names))}] = {{")
    if not names:
        out.append('    { "", nullptr, 0 },')
    for i, name in enumerate(names):
        out.append(f"    {{ {c_str(name)}, kAssetData{i}, sizeof(kAssetData{i}) }},")
    out.append("};")
    out.append("")
    out.append("} // namespace recipe")
    return "\n".join(out) + "\n"


def generate(r: Recipe, out_dir: Path, with_ui: bool = True) -> Path:
    d = out_dir / r.id
    write_if_changed(d / "DistrhoPluginInfo.h", gen_plugin_info(r, with_ui))
    write_if_changed(d / "RecipeMeta.hpp", gen_meta(r))
    write_if_changed(d / "RecipeKernel.hpp", gen_kernel(r))
    write_if_changed(d / "RecipeAssets.hpp", gen_assets(r))
    return d


# ----------------------------------------------------------------------------------------------------------------------
# Recipe discovery


def default_recipes() -> list[Path]:
    mine = sorted(PLUGINS_DIR.glob("*" + RECIPE_SUFFIX)) if PLUGINS_DIR.exists() else []
    return mine if mine else sorted(EXAMPLES_DIR.glob("*" + RECIPE_SUFFIX))


def resolve_recipes(args_recipes: list[str], use_all: bool) -> list[Path]:
    if use_all or not args_recipes:
        paths = default_recipes()
        if not paths:
            sys.exit("no recipes found in plugins/ or examples/")
        return paths
    paths = []
    for a in args_recipes:
        p = Path(a)
        if not p.exists():
            for cand in (PLUGINS_DIR / a, PLUGINS_DIR / (a + RECIPE_SUFFIX), EXAMPLES_DIR / (a + RECIPE_SUFFIX)):
                if cand.exists():
                    p = cand
                    break
        paths.append(p)
    return paths


def load_all(paths: list[Path]) -> tuple[list[Recipe], list[str]]:
    recipes, errors = [], []
    for p in paths:
        try:
            recipes.append(load_recipe(p))
        except RecipeError as e:
            errors.append(str(e))
    ids: dict[str, Path] = {}
    codes: dict[tuple[str, str], Path] = {}
    for r in recipes:
        if r.id in ids:
            errors.append(f"{r.path}: id '{r.id}' already used by {ids[r.id]}")
        ids[r.id] = r.path
        key = (r.brand_id, r.unique_id)
        if key in codes:
            errors.append(f"{r.path}: brand_id/unique_id {key} already used by {codes[key]}")
        codes[key] = r.path
    return recipes, errors


# ----------------------------------------------------------------------------------------------------------------------
# Native test harness


def find_cxx() -> list[str] | None:
    env = os.environ.get("CXX")
    if env:
        return env.split()
    for c in ("c++", "g++", "clang++"):
        if shutil.which(c):
            return [c]
    if shutil.which("cl"):
        return ["cl"]
    return None


def build_harness(r: Recipe, work: Path, sanitize: bool = False) -> Path:
    gen = generate(r, work / "generated", with_ui=False)
    cxx = find_cxx()
    if not cxx:
        sys.exit("no C++ compiler found (set CXX)")
    exe = work / r.id / ("harness.exe" if os.name == "nt" else "harness")
    exe.parent.mkdir(parents=True, exist_ok=True)
    if cxx[0].lower().endswith("cl") or cxx[0].lower().endswith("cl.exe"):
        cmd = cxx + ["/nologo", "/std:c++17", "/O2", "/EHsc", f"/I{gen}", f"/I{FRAMEWORK_INCLUDE}",
                     str(HARNESS_SRC), f"/Fe:{exe}", f"/Fo:{exe.parent}\\"]
    else:
        cmd = cxx + ["-std=c++17", "-O2", "-Wall", "-Wextra", "-Wno-unused-parameter",
                     f"-I{gen}", f"-I{FRAMEWORK_INCLUDE}", str(HARNESS_SRC), "-o", str(exe)]
        if sanitize:  # debug info only here: on macOS -g also runs dsymutil, which slows every test
            cmd += ["-g", "-fsanitize=address,undefined", "-fno-omit-frame-pointer", "-DCOOKBOOK_NO_ALLOC_HOOK"]
        if platform.system() == "Linux":
            cmd += ["-lm"]
    res = subprocess.run(cmd, capture_output=True, text=True)
    if res.returncode != 0:
        print(res.stdout + res.stderr, file=sys.stderr)
        raise RecipeError(f"{r.path}: kernel failed to compile (errors above point into the recipe file)")
    warn = (res.stdout + res.stderr).strip()
    if warn:
        print(warn, file=sys.stderr)
    return exe


SYMBOLS = {"pass": "PASS", "warn": "WARN", "fail": "FAIL", "info": "INFO"}


def run_tests(r: Recipe, work: Path, quick: bool, sanitize: bool) -> bool:
    exe = build_harness(r, work, sanitize)
    cmd = [str(exe), "test"] + (["--quick"] if quick else [])
    res = subprocess.run(cmd, capture_output=True, text=True)
    ok = res.returncode == 0
    results = []
    for line in res.stdout.splitlines():
        line = line.strip()
        if line.startswith("{"):
            try:
                results.append(json.loads(line))
            except json.JSONDecodeError:
                pass
    print(f"\n== {r.name} ({r.path.name})")
    for w in r.warnings:
        print(f"  WARN  static: {w}")
    for t in results:
        print(f"  {SYMBOLS.get(t['status'], t['status']):4}  {t['test']}: {t['detail']}")
    sanitizer = "Sanitizer" in res.stderr or "runtime error:" in res.stderr
    crashed = res.returncode not in (0, 1) or sanitizer
    if crashed:
        last = results[-1]["test"] if results else "start"
        what = "memory error detected by the sanitizer" if sanitizer else f"crashed (exit code {res.returncode})"
        print(f"  FAIL  harness {what} after test '{last}'")
        tail = "\n".join(res.stderr.strip().splitlines()[-40:])
        if tail:
            print("        " + tail.replace("\n", "\n        "))
        if not sanitize:
            print("        hint: re-run with --sanitize to find the out-of-bounds access (usually a buffer sized "
                  "too small in prepare() or an index past its end)")
        ok = False
    elif res.returncode != 0 and not results:
        print(res.stdout + res.stderr)
    return ok


# ----------------------------------------------------------------------------------------------------------------------
# Commands


def cmd_check(args) -> int:
    recipes, errors = load_all(resolve_recipes(args.recipes, args.all))
    for r in recipes:
        lv2, vst3, _ = CATEGORIES[r.category]
        w, h, _ = ui_size(r)
        print(f"OK    {r.path.name}: '{r.name}' by {r.brand} — {r.inputs} in / {r.outputs} out, "
              f"{len(r.params)} params, {vst3}, UI {w}x{h}{', reports latency' if has_latency(r) else ''}")
        for wmsg in r.warnings:
            print(f"      warning: {wmsg}")
    for e in errors:
        print("ERROR " + e)
    return 1 if errors else 0


def cmd_generate(args) -> int:
    paths = resolve_recipes(args.recipes, args.all)
    recipes, errors = load_all(paths)
    if errors:
        for e in errors:
            print("ERROR " + e, file=sys.stderr)
        return 1
    out = Path(args.out)
    for r in recipes:
        generate(r, out, with_ui=not args.no_ui)
    listing = ["# Generated by tools/recipe.py — do not edit.", "set(COOKBOOK_PLUGIN_IDS"]
    listing += [f"  {r.id}" for r in recipes]
    listing += [")", "set(COOKBOOK_RECIPE_FILES"]
    listing += [f'  "{r.path.resolve().as_posix()}"' for r in recipes]
    listing += [f'  "{r.code_path.resolve().as_posix()}"' for r in recipes if r.code_path != r.path]
    listing += [f'  "{(r.path.parent / img).resolve().as_posix()}"' for r in recipes for img in sorted(r.images)]
    listing += [")"]
    for r in recipes:
        cm = lambda v: '"' + v.replace("\\", "\\\\").replace('"', '\\"') + '"'
        listing += [f"set(COOKBOOK_NAME_{r.id} {cm(r.name)})",
                    f"set(COOKBOOK_BUNDLE_ID_{r.id} {cm(r.clap_id)})",
                    f"set(COOKBOOK_VERSION_{r.id} \"{'.'.join(map(str, r.version))}\")"]
    write_if_changed(out / "plugins.cmake", "\n".join(listing) + "\n")
    for r in recipes:
        for w in r.warnings:
            print(f"warning: {r.path.name}: {w}", file=sys.stderr)
    return 0


def cmd_test(args) -> int:
    recipes, errors = load_all(resolve_recipes(args.recipes, args.all))
    for e in errors:
        print("ERROR " + e)
    work = ROOT / "build" / "cookbook-test"
    ok = not errors
    for r in recipes:
        try:
            ok = run_tests(r, work, args.quick, args.sanitize) and ok
        except RecipeError as e:
            print("ERROR " + str(e))
            ok = False
    print("\nAll tests passed." if ok else "\nSome tests FAILED.")
    return 0 if ok else 1


def cmd_render(args) -> int:
    r = load_recipe(resolve_recipes([args.recipe], False)[0])
    exe = build_harness(r, ROOT / "build" / "cookbook-test")
    cmd = [str(exe), "render", args.input, args.output]
    for s in args.set or []:
        cmd += ["--set", s]
    if args.block:
        cmd += ["--block", str(args.block)]
    return subprocess.run(cmd).returncode


def cmd_new(args) -> int:
    pid = args.id
    if not re.fullmatch(r"[a-z][a-z0-9-]{1,40}", pid):
        sys.exit("id: lowercase letters, digits and dashes, starting with a letter")
    dest = PLUGINS_DIR / (pid + RECIPE_SUFFIX)
    if dest.exists():
        sys.exit(f"{dest} already exists")
    name = " ".join(w.capitalize() for w in pid.split("-"))
    uid = (re.sub(r"[^A-Za-z0-9]", "", name) + "Xxxx")[:4]
    uid = uid[0].upper() + uid[1:] if uid[0].isalpha() else "P" + uid[1:]
    PLUGINS_DIR.mkdir(exist_ok=True)
    dest.write_text(TEMPLATE.format(id=pid, name=name, uid=uid), encoding="utf-8")
    print(f"created {dest.relative_to(ROOT)} — edit it, then: python3 tools/recipe.py test {pid}")
    return 0


def cmd_build(args) -> int:
    build = ROOT / "build"
    cfg = ["cmake", "-S", str(ROOT), "-B", str(build), "-DCMAKE_BUILD_TYPE=Release"]
    if args.recipes:
        files = ";".join(str(p.resolve()) for p in resolve_recipes(args.recipes, False))
        cfg.append(f"-DCOOKBOOK_RECIPES={files}")
    else:
        cfg.append("-DCOOKBOOK_RECIPES=")
    if args.formats:
        cfg.append(f"-DCOOKBOOK_FORMATS={args.formats.replace(',', ';')}")
    if args.no_ui:
        cfg.append("-DCOOKBOOK_UI=OFF")
    if subprocess.run(cfg).returncode != 0:
        return 1
    jobs = str(os.cpu_count() or 2)
    if subprocess.run(["cmake", "--build", str(build), "--config", "Release", "--parallel", jobs]).returncode != 0:
        return 1
    print("\nBuilt into", build / "bin")
    for p in sorted((build / "bin").iterdir()):
        print("  ", p.name)
    return 0


def plugin_dirs() -> dict[str, Path]:
    home = Path.home()
    system = platform.system()
    if system == "Darwin":
        base = home / "Library" / "Audio" / "Plug-Ins"
        return {".vst3": base / "VST3", ".clap": base / "CLAP", ".component": base / "Components", ".lv2": base / "LV2"}
    if system == "Windows":
        local = Path(os.environ.get("LOCALAPPDATA", home / "AppData" / "Local")) / "Programs" / "Common"
        appdata = Path(os.environ.get("APPDATA", home / "AppData" / "Roaming"))
        return {".vst3": local / "VST3", ".clap": local / "CLAP", ".lv2": appdata / "LV2"}
    return {".vst3": home / ".vst3", ".clap": home / ".clap", ".lv2": home / ".lv2"}


def cmd_install(args) -> int:
    binp = ROOT / "build" / "bin"
    if not binp.exists():
        sys.exit("nothing built yet: run `python3 tools/recipe.py build` first")
    dirs = plugin_dirs()
    for item in sorted(binp.iterdir()):
        dest_dir = dirs.get(item.suffix)
        if not dest_dir:
            continue
        dest_dir.mkdir(parents=True, exist_ok=True)
        dest = dest_dir / item.name
        if dest.exists():
            shutil.rmtree(dest) if dest.is_dir() else dest.unlink()
        (shutil.copytree if item.is_dir() else shutil.copy2)(item, dest)
        print(f"installed {item.name} -> {dest_dir}")
    if platform.system() == "Darwin":
        print("note: AudioUnits may need `killall -9 AudioComponentRegistrar` before hosts see them")
    return 0


def cmd_studio(args) -> int:
    sys.path.insert(0, str(ROOT / "tools" / "studio"))
    import server  # type: ignore

    return server.serve(args.port, not args.no_browser)


TEMPLATE = '''# Plugin Cookbook recipe — see prompts/plugin-from-idea.md for the full format.

[plugin]
id          = "{id}"
name        = "{name}"
brand       = "Plugin Cookbook"
brand_id    = "Ckbk"            # 4 chars, your maker code (same for all your plugins)
unique_id   = "{uid}"            # 4 chars, unique among your plugins
version     = "1.0.0"
description = "Describe what it does."
category    = "utility"
inputs      = 1
outputs     = 1

[[param]]
symbol      = "gain"
name        = "Gain"
min         = -24.0
max         = 24.0
default     = 0.0
unit        = "dB"
description = "Output level."

[dsp]
code = \'\'\'
class Kernel {{
public:
    void prepare(double sampleRate, uint32_t maxBlockSize) {{ }}
    void reset() {{ }}

    void process(const float* const* in, float* const* out, uint32_t frames, const Params& p)
    {{
        for (uint32_t i = 0; i < frames; ++i)
            out[0][i] = in[0][i] * dbToGain(p.gain[i]);
    }}
}};
\'\'\'
'''


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = ap.add_subparsers(dest="cmd", required=True)

    p = sub.add_parser("check", help="validate recipes")
    p.add_argument("recipes", nargs="*")
    p.add_argument("--all", action="store_true")
    p.set_defaults(func=cmd_check)

    p = sub.add_parser("generate", help="emit generated sources (used by CMake)")
    p.add_argument("recipes", nargs="*")
    p.add_argument("--all", action="store_true")
    p.add_argument("--out", required=True)
    p.add_argument("--no-ui", action="store_true")
    p.set_defaults(func=cmd_generate)

    p = sub.add_parser("test", help="compile kernels natively and run the test suite")
    p.add_argument("recipes", nargs="*")
    p.add_argument("--all", action="store_true")
    p.add_argument("--quick", action="store_true", help="fewer sample rates and parameter sets")
    p.add_argument("--sanitize", action="store_true", help="build with AddressSanitizer + UBSan")
    p.set_defaults(func=cmd_test)

    p = sub.add_parser("render", help="process a WAV file offline")
    p.add_argument("recipe")
    p.add_argument("input")
    p.add_argument("output")
    p.add_argument("--set", action="append", metavar="SYMBOL=VALUE")
    p.add_argument("--block", type=int, default=0)
    p.set_defaults(func=cmd_render)

    p = sub.add_parser("new", help="scaffold a new recipe in plugins/")
    p.add_argument("id")
    p.set_defaults(func=cmd_new)

    p = sub.add_parser("build", help="build plugins with CMake (default: all recipes)")
    p.add_argument("recipes", nargs="*")
    p.add_argument("--formats", help="comma list: standalone,lv2,vst3,clap,au,jack (default: all but jack)")
    p.add_argument("--no-ui", action="store_true")
    p.set_defaults(func=cmd_build)

    p = sub.add_parser("install", help="copy built plugins to this user's plugin folders")
    p.set_defaults(func=cmd_install)

    p = sub.add_parser("studio", help="open Plugin Studio in the browser")
    p.add_argument("--port", type=int, default=8765)
    p.add_argument("--no-browser", action="store_true")
    p.set_defaults(func=cmd_studio)

    args = ap.parse_args()
    try:
        return args.func(args)
    except RecipeError as e:
        print("ERROR " + str(e), file=sys.stderr)
        return 1


if __name__ == "__main__":
    sys.exit(main())
