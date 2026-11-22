# SPDX-License-Identifier: GPL-3.0-or-later
# ARBS-W (B2 D8): APEX haptics bridge + ARBS Core, Windows only. Included from app.pro (win32).
# ARBS Core sources live outside this repository at <ARBS-APEX>/core/arbs-core (GPL-3.0-or-later). Distributing a
# build requires publishing both this fork and the Core sources (GPL-3.0, CLAUDE.md rule 5).
# R0 (D-R0-8): the ARBS_CORE_DIR environment variable, when set, overrides the default location (standalone clone).
ARBS_CORE_DIR = $$(ARBS_CORE_DIR)
isEmpty(ARBS_CORE_DIR): ARBS_CORE_DIR = $$PWD/../../../../core/arbs-core
ARBS_CORE_DIR = $$clean_path($$ARBS_CORE_DIR)
!exists($$ARBS_CORE_DIR/include/arbs/engine.h) {
    error("ARBS Core not found at $$ARBS_CORE_DIR")
}
message(ARBS-W: APEX haptics bridge enabled - ARBS Core at $$ARBS_CORE_DIR)

DEFINES += ARBS_W
# B4 [잠정 D-B4-3] (ARBS-W b2f): edition name for the first log line, from the process-local environment variable set by
# client-w/scripts/arbs-w-build.bat (derived from the output zip name). Empty or unusual characters -> "dev".
ARBS_W_EDITION = $$(ARBS_W_EDITION)
!contains(ARBS_W_EDITION, "^[A-Za-z0-9._-]+$"): ARBS_W_EDITION = dev
message(ARBS-W: edition $$ARBS_W_EDITION)
DEFINES += ARBS_W_EDITION_STR=\\\"$$ARBS_W_EDITION\\\"
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
