# NOTICE — ARBS-W

ARBS-W is a modified version of Moonlight PC (moonlight-qt), distributed under the **GNU General Public License v3.0**.
The original [LICENSE](LICENSE) file is kept unchanged.

## Upstream and included works

| Work | Source | License |
|---|---|---|
| Moonlight PC (moonlight-qt) | `moonlight-stream/moonlight-qt`, base commit `28629dc17feb53c9195a99a34ab689c6ffb1e6e8` | GPL-3.0 |
| moonlight-common-c (submodule) | `Nonary/moonlight-common-c` (fork of `moonlight-stream/moonlight-common-c`) | GPL-3.0 |
| Other submodules and libraries | as listed in the upstream sources (`.gitmodules`, `libs/`, `setup-deps.*`) | their own licenses |
| ARBS Core (built into ARBS-W) | `arbs` repository of the ARBS project | GPL-3.0-or-later |

## ApexSenseBridge

ARBS refers to [ApexSenseBridge](https://github.com/ReynArts/ApexSenseBridge) (commit `d831575`) **for protocol facts only**
(APEX HID report byte layouts). **No ApexSenseBridge source code is copied** into this repository
(see `app/arbs/win_hid_port.cpp` and ARBS Core `include/arbs/protocol.h`).
License note: the ApexSenseBridge `LICENSE` file is GPL-3.0-or-later, while its README badge says MIT.
Either way it is compatible with this GPL-3.0 project.

## ARBS modifications

ARBS changes are the commits after the base commit above (`git log 28629dc1..HEAD`). Main areas:
`app/arbs/` (new, APEX bridge and its unit tests), `app/main.cpp`, `app/app.pro`, `app/streaming/`
(session and input hooks), `app/settings/` (ARBS settings, no mapping download at startup), `app/backend/computermanager.cpp`
(no compatibility download at startup), `app/gui/` (ARBS settings UI), `scripts/build-arch.bat`, `.gitmodules`.
Files added for publication: `README.md` (new), `README.upstream.md` (original README), `NOTICE-ARBS.md`, `.gitignore` (ARBS block).

## Obligations when distributing (GPL-3.0)

Provide the complete corresponding source under the same license (this repository **and** ARBS Core), keep copyright and
license notices, mark modifications, and add no further restrictions.

ARBS is an unofficial open-source project and is not affiliated with or endorsed by Flydigi, Moonlight, Sunshine, Apollo or Vibepollo.
