#pragma once
#include "common.h"

namespace dds {

struct Config {
    // general
    bool enabled = true;
    bool debugLog = false;

    // bar rendering
    bool  barShow = true;
    float barHeight = 6.0f;
    float barGap = 2.0f;
    float barXOffset = 0.0f;
    float barYOffset = 0.0f;
    float barWidthScale = 1.0f;
    bool  barAlwaysShow = false;      // false = hide the bar when shield is 0
    float fillR = 0.30f, fillG = 0.75f, fillB = 1.00f, fillA = 1.00f;
    float backR = 0.04f, backG = 0.08f, backB = 0.14f, backA = 0.85f;
    bool  barBorder = true;
    float borderR = 0.75f, borderG = 0.92f, borderB = 1.00f, borderA = 0.9f;

    // UI coordinate space (Darkest Dungeon lays out its UI for 1920x1080 and
    // scales it to the real framebuffer).
    bool  autoScale = true;
    bool  barStretch = false;
    float uiWidth = 1920.0f;
    float uiHeight = 1080.0f;
    float barScaleMul = 1.0f;
    bool  barUseLayoutOffset = false;   // add the layout health_bar_offset.x (usually 50)

    // tooltip + calibration
    bool tooltip = true;
    bool calibrate = false;           // arrow keys adjust the bar live, F10 saves

    // shield rules
    float defaultMax = 0.0f;
    float startFlat = 0.0f;
    float startHpRatio = 0.0f;
    float maxHpRatio = 0.0f;
    float mirrorHeal = 1.0f;
    int   classIdOffset = 8;     // ActorClass id string offset (verified layout)
    int   actionOffset = 0xAC8;  // Actor quest-action enum (not animation)
    int   barHoldMs = 120;       // keep drawing this long after the last bar call
    bool  hideWhenCursor = false;// hide the overlay while the game shows the mouse cursor
    bool  hideInPanels = true;   // hide while a UI panel (hero sheet etc.) is open
    bool  classShield = true;    // read shield: from .info (Hero+0x1320 class pointer)
    bool  hideActing = true;     // hide every bar while some actor is acting
    int   actingValues[32] = { 7, 9, 12 };
    int   actingValueCount = 3;  // action values treated as "performing a skill"
    int   actionAllow[32] = {};
    int   actionAllowCount = 0;  // empty = show for every action
    float regenPerRound = 0.0f;
    float decayPerRound = 0.0f;

    bool Load(const std::wstring& path);
    bool Save(const std::wstring& path) const;
};

extern Config g_cfg;

} // namespace dds


