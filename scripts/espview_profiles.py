#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""ESPView M7-G -- canonical profile whitelist + label table (single source of truth).

Every ESP32 firmware build / flash / harness profile is described here:
the six profile attributes the project tracks for every profile
(Transport / OLED / Test transport switch / Console / Wi-Fi / Display)
plus the exact whitelisted Kconfig keys the profile is allowed to force.

Whitelist policy (M7-G):
  * Only names in PROFILES (or ALIASES) may be passed to -b / --profile.
  * Profiles never touch credential Kconfig keys (SSID / PASSWORD / PSK /
    secret / token are rejected by assertion -- see FORBIDDEN_KEY_PARTS).
  * Console is NONE for every profile (CONFIG_ESP_CONSOLE_NONE=y).
  * Kconfig choice groups are handled explicitly so a profile cannot drift
    (e.g. mode_c accidentally keeping OLED=y or TCP=y).

This module performs no file I/O and never reads esp32/sdkconfig.
"""
from __future__ import annotations

# Six profile attributes, in fixed order (machine-parseable output uses them).
ATTR_NAMES = ("transport", "oled", "test_switch", "console", "wifi", "display")

# M8-A7（A7-5）：支持的 IDF target（profile 与 target 正交；见 DESIGN AS.2 D8）。
# 新增 target 必须先加入本白名单；profile 键不绑定 target。
TARGETS = ("esp32", "esp32s3")


def validate_target(name):
    """Return True if name is a whitelisted IDF target."""
    return name in TARGETS


# M8-A7（A7-6）：历史实验 profile（M7-G G1 OLED×RF 矩阵；证据保留，不删除）。
# 不进 CI 默认矩阵 / 一等发布；仍可经 workflow_dispatch 或 --apply 显式构建。
HISTORICAL_PROFILES = frozenset({"g1_a", "g1_b", "g1_c", "g1_d"})


def is_historical(name):
    """Return True if the (canonical) profile is a historical experiment."""
    return canonical_name(name) in HISTORICAL_PROFILES

# Kconfig choice groups: setting one member to "y" unsets the other members.
CHOICE_GROUPS = (
    ("CONFIG_ESPVIEW_TRANSPORT_UART", "CONFIG_ESPVIEW_TRANSPORT_TCP"),
    ("CONFIG_ESPVIEW_APP_LVGL", "CONFIG_ESPVIEW_APP_TESTPATTERN"),
)

# Hard guard: no profile-controlled key may look like a credential key.
FORBIDDEN_KEY_PARTS = ("SSID", "PASSWORD", "PSK", "TOKEN", "SECRET")

# Whitelisted profiles. attrs order == ATTR_NAMES order.
PROFILES = {
    "uart": {
        "label": "UART development baseline (OLED + LVGL + F12 test hooks, RF off at boot)",
        "attrs": ("UART", "ON", "ON", "NONE", "OFF", "LVGL"),
        "config": {
            "CONFIG_ESPVIEW_TRANSPORT_UART": "y",
            "CONFIG_ESPVIEW_OLED_ENABLE": "y",
            "CONFIG_ESPVIEW_TEST_TRANSPORT_SWITCH": "y",
            "CONFIG_ESP_CONSOLE_NONE": "y",
            "CONFIG_ESP_WIFI_ENABLED": "n",
            "CONFIG_ESPVIEW_APP_LVGL": "y",
        },
    },
    "tcp": {
        "label": "TCP production (Wi-Fi STA + TCP client, OLED + LVGL, no test hooks)",
        "attrs": ("TCP", "ON", "OFF", "NONE", "ON", "LVGL"),
        "config": {
            "CONFIG_ESPVIEW_TRANSPORT_TCP": "y",
            "CONFIG_ESPVIEW_OLED_ENABLE": "y",
            "CONFIG_ESPVIEW_TEST_TRANSPORT_SWITCH": "n",
            "CONFIG_ESP_CONSOLE_NONE": "y",
            "CONFIG_ESP_WIFI_ENABLED": "y",
            "CONFIG_ESPVIEW_APP_LVGL": "y",
        },
    },
    "oled": {
        "label": "OLED focus (UART + OLED + RF on for Wi-Fi scan experiments)",
        "attrs": ("UART", "ON", "OFF", "NONE", "ON", "LVGL"),
        "config": {
            "CONFIG_ESPVIEW_TRANSPORT_UART": "y",
            "CONFIG_ESPVIEW_OLED_ENABLE": "y",
            "CONFIG_ESPVIEW_TEST_TRANSPORT_SWITCH": "n",
            "CONFIG_ESP_CONSOLE_NONE": "y",
            "CONFIG_ESP_WIFI_ENABLED": "y",
            "CONFIG_ESPVIEW_APP_LVGL": "y",
        },
    },
    "oled-off": {
        "label": "OLED disabled comparison (UART + no OLED + RF on for scan experiments)",
        "attrs": ("UART", "OFF", "OFF", "NONE", "ON", "LVGL"),
        "config": {
            "CONFIG_ESPVIEW_TRANSPORT_UART": "y",
            "CONFIG_ESPVIEW_OLED_ENABLE": "n",
            "CONFIG_ESPVIEW_TEST_TRANSPORT_SWITCH": "n",
            "CONFIG_ESP_CONSOLE_NONE": "y",
            "CONFIG_ESP_WIFI_ENABLED": "y",
            "CONFIG_ESPVIEW_APP_LVGL": "y",
        },
    },
    "diagnostic": {
        "label": "Diagnostic (UART + OLED + TestPattern deterministic frames, RF off)",
        "attrs": ("UART", "ON", "OFF", "NONE", "OFF", "TestPattern"),
        "config": {
            "CONFIG_ESPVIEW_TRANSPORT_UART": "y",
            "CONFIG_ESPVIEW_OLED_ENABLE": "y",
            "CONFIG_ESPVIEW_TEST_TRANSPORT_SWITCH": "n",
            "CONFIG_ESP_CONSOLE_NONE": "y",
            "CONFIG_ESP_WIFI_ENABLED": "n",
            "CONFIG_ESPVIEW_APP_TESTPATTERN": "y",
        },
    },
    # M7-G G1 harness profiles (task-book section 6): OLED x RF matrix.
    "g1_a": {
        "label": "G1 A: OLED OFF + RF OFF",
        "attrs": ("UART", "OFF", "OFF", "NONE", "OFF", "LVGL"),
        "config": {
            "CONFIG_ESPVIEW_TRANSPORT_UART": "y",
            "CONFIG_ESPVIEW_OLED_ENABLE": "n",
            "CONFIG_ESPVIEW_TEST_TRANSPORT_SWITCH": "n",
            "CONFIG_ESP_CONSOLE_NONE": "y",
            "CONFIG_ESP_WIFI_ENABLED": "n",
            "CONFIG_ESPVIEW_APP_LVGL": "y",
        },
    },
    "g1_b": {
        "label": "G1 B: OLED ON + RF OFF",
        "attrs": ("UART", "ON", "OFF", "NONE", "OFF", "LVGL"),
        "config": {
            "CONFIG_ESPVIEW_TRANSPORT_UART": "y",
            "CONFIG_ESPVIEW_OLED_ENABLE": "y",
            "CONFIG_ESPVIEW_TEST_TRANSPORT_SWITCH": "n",
            "CONFIG_ESP_CONSOLE_NONE": "y",
            "CONFIG_ESP_WIFI_ENABLED": "n",
            "CONFIG_ESPVIEW_APP_LVGL": "y",
        },
    },
    "g1_c": {
        "label": "G1 C: OLED OFF + RF ON",
        "attrs": ("UART", "OFF", "OFF", "NONE", "ON", "LVGL"),
        "config": {
            "CONFIG_ESPVIEW_TRANSPORT_UART": "y",
            "CONFIG_ESPVIEW_OLED_ENABLE": "n",
            "CONFIG_ESPVIEW_TEST_TRANSPORT_SWITCH": "n",
            "CONFIG_ESP_CONSOLE_NONE": "y",
            "CONFIG_ESP_WIFI_ENABLED": "y",
            "CONFIG_ESPVIEW_APP_LVGL": "y",
        },
    },
    "g1_d": {
        "label": "G1 D: OLED ON + RF ON",
        "attrs": ("UART", "ON", "OFF", "NONE", "ON", "LVGL"),
        "config": {
            "CONFIG_ESPVIEW_TRANSPORT_UART": "y",
            "CONFIG_ESPVIEW_OLED_ENABLE": "y",
            "CONFIG_ESPVIEW_TEST_TRANSPORT_SWITCH": "n",
            "CONFIG_ESP_CONSOLE_NONE": "y",
            "CONFIG_ESP_WIFI_ENABLED": "y",
            "CONFIG_ESPVIEW_APP_LVGL": "y",
        },
    },
}

# Legacy alias: uart_hw == uart attributes, but keeps its historic build dir
# name (esp32/build/uart_hw) so existing build artifacts stay usable.
ALIASES = {
    "uart_hw": "uart",
}


def canonical_name(name):
    """Map aliases to the canonical whitelisted profile name."""
    return ALIASES.get(name, name)


def get_profile(name):
    """Return (canonical_name, spec) or (None, None) for unknown names."""
    base = canonical_name(name)
    spec = PROFILES.get(base)
    if spec is None:
        return None, None
    return base, spec


def profile_line(name):
    """Machine-parseable '# profile <name> attr=... label=...' summary line."""
    base, spec = get_profile(name)
    if spec is None:
        return None
    parts = ["# profile %s" % base]
    for attr, value in zip(ATTR_NAMES, spec["attrs"]):
        parts.append("%s=%s" % (attr, value))
    label = spec["label"]
    if is_historical(base):
        label += " [historical]"
    parts.append("label=%s" % label)
    return " ".join(parts)


def config_keys():
    """All whitelisted Kconfig keys across every profile (for validation)."""
    keys = set()
    for spec in PROFILES.values():
        keys.update(spec["config"])
    return keys


def validate_table():
    """Sanity-check the table: attribute length, key values, credential guard."""
    for name, spec in PROFILES.items():
        assert len(spec["attrs"]) == len(ATTR_NAMES), name
        for key, value in spec["config"].items():
            assert value in ("y", "n"), (name, key, value)
            assert key.startswith("CONFIG_"), (name, key)
            assert not any(part in key for part in FORBIDDEN_KEY_PARTS), \
                (name, key)
    for alias, target in ALIASES.items():
        assert target in PROFILES, (alias, target)
    assert HISTORICAL_PROFILES.issubset(PROFILES), \
        "historical profiles must stay whitelisted"
    return True


if __name__ == "__main__":
    validate_table()
    print("espview_profiles: table OK (%d profiles, %d aliases, %d targets, "
          "%d historical)"
          % (len(PROFILES), len(ALIASES), len(TARGETS),
             len(HISTORICAL_PROFILES)))