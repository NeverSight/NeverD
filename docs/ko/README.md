**언어**: [English](../README.md) | [简体中文](../zh-CN/README.md) | [繁體中文](../zh-TW/README.md) | [日本語](../ja/README.md) | [한국어](README.md) | [Français](../fr/README.md) | [Deutsch](../de/README.md) | [Español](../es/README.md) | [Italiano](../it/README.md) | [Русский](../ru/README.md) | [العربية](../ar/README.md)

<!-- i18n-source: 6241c9252ec89317b9c488c7e2b6b8c7bd60329dfdd734df27e1a7a7139b1eff -->

[← NeverD 프로젝트](project.md)

# NeverD 문서

프로젝트 개요·빌드·CLI는 저장소 README에 있습니다. 기여자용 설계와 테스트 자료를 여기에 모았습니다.

**모바일 지원(실험적 CLI):** `neverd mobile`은 [Android](android.md) APK, DEX, smali에서 Java를, [iOS](ios.md) IPA, `.app`, Mach-O에서 네이티브 C 및 지원되는 Objective-C/Swift 소스를 복원합니다. JSON 보고서는 복원 결과와 범위를 설명합니다. [모바일 개요](mobile.md)부터 읽고 플랫폼 가이드에서 명령과 제한을 확인하세요.

영어 가이드는 `docs/` 바로 아래에 있습니다. 번역은 `ar/`, `de/`, `es/`, `fr/`, `it/`, `ja/`, `ko/`, `ru/`, `zh-CN/`, `zh-TW/`로 나뉩니다. 각 언어 디렉터리는 문서 색인 `README.md`, 프로젝트 개요 `project.md`, 주제별 가이드, `CONTRIBUTING.md`, `ATTRIBUTION.md`, `roadmap.md`를 포함합니다. 공용 이미지는 `assets/`에 있습니다.

| 문서 | 설명 |
|------|------|
| [프로젝트 설명（한국어）](project.md) | 개요, 빠른 시작, 빌드, SDK, CLI |
| [기여 가이드](CONTRIBUTING.md) | 개발 환경, 빌드 프로필, 워크플로, 스타일, PR 요구 사항 |
| [아키텍처](architecture.md) | IR 경로, 구성 요소 경계, strict lifting, 지원 깊이, 수정 위치 |
| [오프라인 Web 분석 (영문)](../web-analysis.md) | C++ 산출물·소스·바인딩·맵 검사, 메타데이터 정책, SDK 및 현재 검증 범위 |
| [Bun 독립 실행 파일 프로필 (영문)](../web-bun-profile.md) | 고정 ELF 추출, 원본 증거 범위, 소스 디코딩 및 고정 테스트 자료의 출처 |
| [ASAR 추출 프로필 (영문)](../web-asar-profile.md) | 내부·외부 파일 캡처 연결, 무결성 상태, 멤버 소비자 및 네이티브 Unicode 의존성 |
| [Electron 증거 프로필 (영문)](../web-electron-profile.md) | 캡처한 진입 경로, 매니페스트·소스 범위, 창·브리지 증거 및 IPC 채널 후보 |
| [HTML 소스 프로필 (영문)](../web-html-profile.md) | 유한 범위의 C++ 스크립트 목록, 캡처한 로컬 참조 및 원본 인라인 소스 앵커 |
| [테스트](testing.md) | 테스트 스위트, 생성 fixture, Unicorn 왕복, 증분 명령 |
| [데스크톱 워크벤치 (영문)](../gui.md) | 고전적인 디스어셈블러 레이아웃, 별도 작업 프로세스, 프로젝트 데이터베이스, 현지화와 MCP 연결 |
| [라이브러리 인식 (영문)](../library-recognition.md) | 근거에 따른 STL, ATL/MFC, COM, libc 식별, 프로필 및 복원 가능한 C 코드 접기 |
| [데스크톱 검증 기록 (영문)](../gui-qualification.md) | 측정된 GUI 증거, 패키징 범위와 남은 플랫폼 검증 |
| [인터프리터 소스 복원](interpreter-recovery.md) | 실험적 x64 인터프리터 특수화, HighC/LLVMC 출력, 실행 전제, 근거와 제한; 중첩 루프 증명 후보; 명시적 탐색 예산과 버전별 C API; 정확한 네이티브에서 LLVM으로의 증명 API |
| [Windows 예외 재구성](windows-exception-reconstruction.md) | SEH/C++ 지원 표, IR 계약, 네이티브 patch 규칙 및 PE 검증 |
| [CPU 실행과 게스트 환경](emulation.md) | 백엔드 선택, 게스트 환경, 네이티브 검증과 현재 제한 |
| [CPU 실행](cpu-execution.md) | 구성, 기능 조회, 백엔드 가용성, 형식화된 결과 |
| [비트벡터 증명 백엔드](solver.md) | 선택적 Z3 증명, 증명 게이트 합성, 독립 검사, query 내보내기 |
| [게스트 프로세스 에뮬레이션](process-emulation.md) | Linux ELF 프로필, 시작, 서비스, 제한, 테스트 |
| [macOS/iOS 프로세스 환경](darwin-emulation.md) | Mach-O 시작, 기기와 시뮬레이터 구분, Darwin 서비스와 페이지 규칙 |
| [macOS HVF](macos-hvf.md) | 호스트와 동일한 ISA의 하드웨어 실행, 서명 권한, 패키징과 검증 |
| [Windows 드라이버 에뮬레이션](driver-emulation.md) | 제한된 x64 WDM/KMDF 수명 주기, 요청, 하드웨어 시나리오, SEH, PnP 하위 집합, 백엔드 선택 및 제한 |
| [메모리 안전성 감사와 헌트](memory-safety.md) | 힙 수명과 복사 오버플로 분석: 형식별 신원 계약, 싱크/소스 카탈로그, 판정, 예산, JSON 스키마 |
| [네이티브 플러그인](plugins.md) | 순수 C descriptor ABI, callback과 event, build/link workflow, discovery 및 호환성 규칙 |
| [Python 플러그인](python-plugins.md) | 플러그인 작성, 세션·이벤트 API, 격리, 테스트 및 배포 |
| [모바일 지원 개요](mobile.md) | Android / iOS 입력, 소스 출력, CLI 흐름 및 제한 |
| [Android Java 복구](android.md) | APK(multidex)·DEX·smali 파일/디렉터리 → Java. CLI 흐름, 실행 환경, 옵션, JSON 보고서, 오류 처리 및 검증 제한 |
| [iOS 소스 복원](ios.md) | IPA·`.app`·Mach-O → 네이티브 C 및 지원되는 Objective-C / Swift 소스. 입력 선택, 메서드 본문과 배치, CLI/export, JSON 커버리지 보고서, 제한 및 실행 검증 |
| [EVM 디컴파일](evm.md) | 입력, hardfork, 단계별 IR, C/LLVM host ABI, Solidity 복구 및 제한 |
| [Solana SBF 디컴파일](sbf.md) | SBF v0-v4, LLVM IR, C/Rust 출력, 검증 및 알려진 제한 사항 |
| [로드맵](roadmap.md) | 상태: native format, EVM, Solana SBF 구현 완료 |
| 다국어 문서 | 위의 언어 링크에서 각 언어의 색인과 프로젝트 개요를 열 수 있습니다 |
| [패킹된 실행 파일 언패킹](unpack.md) | 유한한 게스트 프로세스 관찰을 통한 패킹된 PE32+ 이미지 복구: 진입점 규칙, 재구성된 이미지, 식별, 제한 |
