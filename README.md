# ARBS-W — Windows client (APEX Remote Bridge STACKON)

> **Unofficial open-source project.** ARBS is not affiliated with, endorsed by, or sponsored by Flydigi.
> It is also not affiliated with the Moonlight, Sunshine, Apollo or Vibepollo projects.
> All product names and trademarks belong to their respective owners. This repository contains no Flydigi logos or images.

## What is this?

ARBS-W is a fork of [Moonlight PC (moonlight-qt)](https://github.com/moonlight-stream/moonlight-qt) for **Windows** that
lets a **Flydigi APEX 6 Pro** controller connected to the client PC feel the host game's DualSense output
(standard rumble, adaptive-trigger effects and PCM HD haptics) while streaming from an ARBS-H host.

ARBS consists of four repositories in the `arbs-project` organisation:

| Repository | Role |
|---|---|
| `arbs` | Project overview, user guide, download notes, license notices, ARBS Core sources |
| `arbs-h` | Host (fork of Vibepollo 2.18.9) |
| `arbs-w` | Windows client (this repository, fork of moonlight-qt) |
| `arbs-a` | Android client (fork of moonlight-android) |

## Based on

- `moonlight-stream/moonlight-qt` commit `28629dc1` (after v6.2.0). The original history is kept; ARBS commits follow it.
- `moonlight-common-c` submodule points to `Nonary/moonlight-common-c` (needed for the DualSense haptics stream).
- The original README is kept as [README.upstream.md](README.upstream.md).

## Changes from upstream (summary)

- APEX haptics bridge (`app/arbs/`): rumble, trigger effects and PCM haptics forwarded to the APEX over HID (Windows only).
- **Building requires ARBS Core** (GPL-3.0-or-later, published in the `arbs` repository). By default the build looks for it
  at `../../core/arbs-core` relative to this repository; set the `ARBS_CORE_DIR` environment variable to use another location.
- Discord Rich Presence is excluded from the build. The update check is off by default.
- No moonlight-stream.org requests at startup: the compatibility list and gamepad-mapping downloads are disabled
  (saved values and the built-in `gamecontrollerdb.txt` are still used).

Status: **experimental**. What has actually been confirmed on real devices is listed in the `arbs` repository README.

## License

GPL-3.0 — see [LICENSE](LICENSE). Third-party and upstream notices: [NOTICE-ARBS.md](NOTICE-ARBS.md).
Security issues: please use the private reporting described in the `arbs` repository `SECURITY.md`.

---

## 한국어 요약

ARBS-W는 moonlight-qt(커밋 `28629dc1`, v6.2.0 이후)를 바탕으로 한 **Windows 전용** 클라이언트 포크로, 클라이언트 PC에 연결한
Flydigi APEX 6 Pro에서 호스트 게임의 DualSense 출력(일반 진동·트리거 효과·PCM HD 햅틱)을 느끼게 합니다.
빌드에는 ARBS Core(`arbs` 저장소, `ARBS_CORE_DIR` 환경 변수로 위치 지정)가 필요합니다. Discord 연동 제외, 업데이트 확인 기본 꺼짐,
시작 때 moonlight-stream.org 조회(호환성·게임패드 매핑) 꺼짐. 라이선스 GPL-3.0. 상태: 실험적.
**Flydigi와 관련 없는 비공식 오픈소스 프로젝트**이며 Moonlight·Sunshine·Apollo·Vibepollo 프로젝트와도 관련이 없습니다.
