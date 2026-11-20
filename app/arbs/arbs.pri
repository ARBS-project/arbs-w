# SPDX-License-Identifier: GPL-3.0-or-later
# ARBS-W (B2 D8): APEX haptics bridge + ARBS Core, Windows only. Included from app.pro (win32).
# ARBS Core sources live outside this repository at <ARBS-APEX>/core/arbs-core (GPL-3.0-or-later). Distributing a
# build requires publishing both this fork and the Core sources (GPL-3.0, CLAUDE.md rule 5).
ARBS_CORE_DIR = $$clean_path($$PWD/../../../../core/arbs-core)
!exists($$ARBS_CORE_DIR/include/arbs/engine.h) {
    error("ARBS Core not found at $$ARBS_CORE_DIR")
}
message(ARBS-W: APEX haptics bridge enabled - ARBS Core at $$ARBS_CORE_DIR)

DEFINES += ARBS_W
INCLUDEPATH += $$PWD $$ARBS_CORE_DIR/include

SOURCES += \
    $$ARBS_CORE_DIR/src/arrival_stats.cpp \
    $$ARBS_CORE_DIR/src/engine.cpp \
    $$ARBS_CORE_DIR/src/jitter_buffer.cpp \
    $$ARBS_CORE_DIR/src/pcm_pipeline.cpp \
    $$ARBS_CORE_DIR/src/probe_policy.cpp \
    $$ARBS_CORE_DIR/src/protocol.cpp \
    $$ARBS_CORE_DIR/src/rumble_synth.cpp \
    $$ARBS_CORE_DIR/src/scheduler.cpp \
    $$ARBS_CORE_DIR/src/session_stats.cpp \
    $$ARBS_CORE_DIR/src/trigger_bridge.cpp \
    $$ARBS_CORE_DIR/src/trigger_effect.cpp \
    $$PWD/arbs_bridge.cpp \
    $$PWD/arbs_log.cpp \
    $$PWD/arbs_logic.cpp \
    $$PWD/win_clock.cpp \
    $$PWD/win_hid_port.cpp

HEADERS += \
    $$PWD/arbs_bridge.h \
    $$PWD/arbs_log.h \
    $$PWD/arbs_logic.h \
    $$PWD/arbs_numbering.h \
    $$PWD/arbs_policy.h \
    $$PWD/arbs_settings.h \
    $$PWD/win_clock.h \
    $$PWD/win_hid_port.h

# HID enumeration (SetupDi/HidD/HidP), MMCSS (Av*)
LIBS += -lhid -lsetupapi -lavrt
