**언어**: [English](../mobile.md) | [简体中文](../zh-CN/mobile.md) | [繁體中文](../zh-TW/mobile.md) | [日本語](../ja/mobile.md) | [한국어](mobile.md) | [Français](../fr/mobile.md) | [Deutsch](../de/mobile.md) | [Español](../es/mobile.md) | [Italiano](../it/mobile.md) | [Русский](../ru/mobile.md) | [العربية](../ar/mobile.md)

# 모바일 애플리케이션 복원

[← 문서 목록](README.md) · [전체 Android 가이드](android.md) · [전체 iOS 가이드](ios.md)

`neverd mobile`은 Android APK, DEX 및 smali 입력에서 읽을 수 있는 Java를 복원합니다. iOS IPA, `.app` 및 Mach-O 입력에서는 네이티브 C를 내보내고 지원되는 Objective-C 메서드 본문을 `.m` 소스로 재구성하며, 실험적인 Swift 소스와 런타임 메타데이터도 제공합니다. 이는 실험적인 CLI 작업 흐름이며 네이티브 C SDK나 GUI 로더는 모바일 컨테이너를 받지 않습니다.

## 설정

C++20을 지원하는 환경에서 `neverd` 타깃을 빌드하세요. 모바일 작업 흐름은 네이티브 CLI에 컴파일되며 Python 인터프리터를 사용하지 않습니다. 실행 파일을 배포할 때 해당 빌드에 필요한 네이티브 라이브러리를 함께 제공하세요.

네이티브 ZIP 처리는 CRC-32와 DEFLATE에 zlib를 사용합니다. CMake는 `find_package`로 설치된 라이브러리를 우선 사용하며, 없으면 고정된 SHA256으로 검증하는 zlib 1.3.2를 다운로드하여 정적으로 빌드합니다. 이 모바일 ZIP 구현은 Windows에서 Python 보조 도구가 필요하지 않습니다. [THIRD_PARTY_NOTICES.md](../../THIRD_PARTY_NOTICES.md)의 의존성 고지를 유지하세요.

기본 엔진은 C++20으로 구현되어 Python, Java 또는 JADX 런타임이 필요하지 않습니다. 명시적인 `--jadx PATH`만 별도로 설치된 호환 어댑터를 선택합니다. `NEVERD_JADX`와 PATH는 이를 자동으로 선택하지 않으며 자동 대체 경로도 없습니다. 선택적 어댑터에는 표준 DEX/smali 입력 플러그인을 갖춘 JADX 1.5.6+와 Java 11+가 필요합니다. 보고서는 실제 `jadx` 엔진과 버전을 명시합니다. 설치 및 의존성 라이선스는 [Android 가이드](android.md#선택-사항인-jadx-호환-어댑터)에 설명되어 있습니다.

## Android

Java 복원 없이 클래스 목록을 빠르게 얻으려면
`neverd mobile app.apk --list-classes`를 사용하고, 필요하면
`--class-prefix com.example` 또는 `--json`을 추가하세요.
[목록 계약](android.md#빠른-클래스-목록-조회)은 순서, 제한 및 선택 페이로드 검증을 설명합니다.
쿼리 모드에는 출력 디렉터리가 필요하지 않으며, 선택적 `-o`는 새 파일을 지정합니다.

직접 바이트코드 참조에는
`neverd mobile app.apk --find-refs string --query 'example' --json`을 사용하세요.
[참조 쿼리 계약](android.md#코드-참조-쿼리)은 type, method, field 피연산자,
리터럴/정확 일치, 각 발생 위치, UTF-16 보존 및 코드 검증 범위도 다룹니다.
`--json`이 없으면 JSON Lines를 출력합니다. `-o`가 새 파일을 지정하는 동작도 같습니다.

다음 예시와 출력은 복원 작업을 설명합니다.

```sh
neverd mobile app.apk -o recovered-app
neverd mobile classes.dex -o recovered-dex
neverd mobile MainActivity.smali -o recovered-class
neverd mobile decoded/smali -o recovered-java
```

APK 루트의 모든 `classes.dex`, `classes2.dex` 및 이후 번호가 붙은 DEX 파일을 함께 분석합니다. smali 디렉터리는 재귀적으로 검색하며 중첩 및 형제 클래스를 포함한 모든 클래스를 한 번의 호출로 분석합니다. 서로 참조하는 클래스를 복원할 때는 디렉터리를 사용하세요. 단일 smali 파일은 해당 클래스만 제공합니다.

내장 복원 출력에는 `sources/`, `metadata/android-methods.json`, `report.json`이 포함되며 `backend: {"name": "neverd", "version": "1", "execution": "builtin"}`을 기록합니다. 보고서는 `android_method_recovery`를 포함합니다. `method_count = recovered_method_count + declaration_only_method_count`를 만족하며, 공개 전에 `unrecovered_method_count`가 0이어야 합니다. 원래의 `native`/`abstract` 선언은 복원된 본문과 별도로 계산합니다. 명시적으로 선택한 외부 어댑터는 자체 백엔드 로그를 보관합니다. APK 리소스, 매니페스트, 네이티브 라이브러리 및 동적으로 로드되는 코드는 이 Java 경로의 범위 밖이며, 네이티브 라이브러리는 `neverd decompile`로 따로 분석할 수 있습니다.

내장 복원 판독기는 독립적으로 구현한 타입이 있는 Dalvik 모델과 처리량이 제한된 Java 생성기를 공유하며, 표현 가능한 일반 DEX 035/037–040 및 smali 코드를 처리합니다. DEX 041, `invoke-custom` 같은 동적 호출, 일부 초기화 경로, 알 수 없는 연산 및 Java로 표현할 수 없는 식별자는 명시적으로 실패합니다. 생성된 Java는 디스패치 루프를 사용할 수 있지만 원래 DEX를 실행하거나 런타임 연결을 통해 호출하지 않습니다. 원래 주석, 서식 및 제거된 이름은 복원할 수 없습니다. 실험적 엔진은 JADX와의 기능 동등성, 의미적 동등성 또는 임의 APK의 완전한 복원을 약속하지 않습니다.

## iOS

[전체 iOS 가이드](ios.md)는 IPA, `.app`, Mach-O 선택, 설정, 모든 CLI 옵션, 소스 스키마, 검사 범위 및 검증을 설명합니다.

```sh
neverd mobile App.ipa -o recovered-ios
neverd mobile App.app -o recovered-arm64 --arch=arm64
neverd mobile App.app -o framework-analysis --artifact Frameworks/Example.framework/Example
neverd mobile executable -o metadata --metadata-only
```

각 실행은 실행 파일 하나를 선택합니다. `--artifact`는 IPA와 `.app` 모두에서 애플리케이션 번들 기준 상대 경로입니다. Fat 선택은 arm64, arm, x86_64, i386 순서로 우선하며, 소스 언어 투영은 arm64/x86_64를 대상으로 합니다. 선택된 슬라이스가 암호화되어 있으면 거부합니다. 실험적 네이티브 경로는 C와 지원되는 Objective-C 메서드 본문을 생성하며, 스칼라/포인터, 부동소수점, 혼합 및 스택 ABI 바인딩을 포함합니다. 런타임 클래스/ivar 배치와 별도 카테고리는 검증된 경우 유지하며, 해결되지 않은 배치, 시그니처, 호출 및 기타 의존성은 명시적인 누락으로 남습니다.

Swift 복원은 NeverD LLVM 포크의 `LLVMSwiftDemangle`을 사용하여 C++ 프로세스 안에서 시그니처를 분류합니다. 외부 디맹글러나 툴체인 탐색 명령을 시작하지 않으며 NeverD를 빌드하거나 실행할 때 설치된 Swift 컴파일러가 필요하지 않습니다. 포크 소스 빌드와 대응하는 LLVM 패키지가 이 구성 요소를 포함하며, NeverD는 별도의 Swift 소스 의존성을 가져오지 않습니다. 시그니처 목록은 `demangler: {"name": "llvm-swift-demangle", "execution": "builtin", "version": "6.3.3"}`을 기록합니다. 지원되는 시그니처를 네이티브 진입점과 ABI 위치에 연결한 다음 실제 `.swift` 함수, 클래스 메서드/이니셜라이저 및 고정 배치 구조체 메서드를 생성합니다. generic/resilient, async/throwing, 지원하지 않는 런타임 생성 callable 형태 및 불완전한 소스 의존성 그룹은 복원되지 않은 상태로 남습니다.

일반 출력에는 `sources/native.c`, 선택적인 `sources/objc.m`와 `sources/swift.swift`, 선언과 런타임 메타데이터, 메서드/시그니처 검사 범위 JSON, 로그, `artifacts/selected.macho` 및 `report.json`이 포함됩니다. 외부 Swift 툴체인 탐색이나 디맹글링 로그는 없습니다. 생성 소스는 원래 바이너리를 복원용 연결 대상으로 호출하지 않습니다. Swift의 `source_units`는 타입 선언과 메서드를 묶으며, 독립 메서드 행을 이어 붙여 클래스를 재구성해서는 안 됩니다. 바깥쪽 `status: "success"`는 출력 공개를 뜻하며 완전한 메서드 복원 범위나 의미적 동등성을 뜻하지 않습니다.

`--metadata-only`는 네이티브 소스 내보내기와 시그니처 디맹글링을 모두 실행하지 않으며 소스나 메서드 검사 범위를 출력하지 않습니다. 모든 모드는 네이티브 로더의 해석된 Objective-C 메타데이터를 사용합니다. Swift 메타데이터는 범위가 제한된 네이티브 이미지 읽기를 사용하며, 지원하지 않는 fixup, 재배치 가능한 배치 또는 참조에 대해서는 부분 진단을 보존합니다. `--max-func`는 네이티브 함수 복원을 제한하며 메타데이터 전용 모드에서는 무시합니다. 네이티브 함수 본문이 없으면 일반 실행은 실패합니다. 임시로 압축 해제한 입력은 제거합니다.

원래 주석, 서식, 제거된 식별자 및 컴파일로 잃어버린 소스 구조는 정확히 재구성할 수 없습니다. 출력을 사용하기 전에 각 메서드의 복원 상태와 이유, Swift의 callable/non-callable/unclassified 개별 집계 및 문서의 제한 사항을 읽으세요.

## 제한과 실패

복원에서 `-o`는 디렉터리 입력 바깥의 새 디렉터리를 지정해야 합니다. 쿼리 모드는 대신 선택적인 새 출력 파일을 받습니다. 기존 출력은 절대 덮어쓰지 않습니다. 복원 작업은 임시 영역에서 수행되며 복원과 출력 검증이 성공한 뒤에만 공개합니다. 쿼리 결과는 선택된 모든 DEX가 성공할 때까지 버퍼에 보관합니다. 네이티브 CLI 실행이 성공하면 0을 반환합니다. 복원 실패는 0이 아닌 값을 반환하며, `--json`은 처리된 실패를 `schema_version`, `status: "error"`, `error`로 보고합니다. 인수 파싱, 네이티브 실행 파일이나 라이브러리 시작 실패 및 중단은 대신 stderr에 보고될 수 있습니다. 소비자는 먼저 종료 상태를 확인해야 합니다.

기본 제한은 20,000개 항목, 입력/압축 해제 데이터 또는 최종 출력 데이터 2 GiB, 내장 Android/iOS 분석 또는 명시적 JADX 프로세스 각각에 대해 300초입니다. iOS 자식 프로세스에는 전체 분석 예산의 남은 시간을 제공합니다. 내장 판독기와 생성기도 제한된 처리량 예산을 적용합니다. `--max-files`, `--max-bytes`, `--timeout`으로 이 양수 제한을 조정합니다. 백엔드 실행 중 임시 작업 영역을 감시하며, 준비된 입력과 중간 출력이 함께 존재할 수 있도록 항목/바이트 제한의 최대 3배까지 허용합니다. 진단은 프로세스당 16 MiB로 제한합니다.

복원 중 APK 준비 단계는 루트의 `classes.dex`, `classes2.dex` 및 이후 번호가 붙은 DEX 파일만 기록합니다. 그래도 모든 ZIP 멤버는 헤더/범위 검사, 압축 해제, 길이 및 CRC 검증을 거치며 아카이브 항목 수와 압축 해제 바이트 제한에 포함됩니다. 기록하지 않는 리소스는 `res/-A.xml`과 `res/-a.xml`처럼 대소문자를 구분하는 서로 다른 이름을 가질 수 있습니다. 정확히 중복된 ZIP 이름과 파일/디렉터리 식별 충돌은 여전히 오류이며, 이식 가능한 파일 시스템의 대소문자 충돌 검사는 실제로 기록하는 멤버에 적용합니다. IPA 입력을 포함한 전체 추출은 이런 출력 충돌을 계속 거부합니다. 아카이브 전체에서 경로 탈출, 링크, 특수 파일 및 암호화된 ZIP 항목을 거부합니다. 디렉터리 입력도 심볼릭 링크와 특수 파일을 거부합니다.

이 제한은 견고성을 위한 제어이며 타사 백엔드 코드의 샌드박스가 아닙니다. 명시적 JADX 및 네이티브 소스 내보내기 명령은 로컬 자식 프로세스로 실행됩니다. 실패한 준비 출력은 제거합니다. 0이 아닌 백엔드 종료에는 길이가 제한된 진단 끝부분을 포함합니다. 백엔드 시간 초과 시 시간 초과 메시지를 보존하고, 캡처된 로그 텍스트가 있으면 길이가 제한된 끝부분을 추가합니다. 시작 실패와 예산 위반은 각각의 오류 메시지를 유지합니다.

## 검증

Python은 아래 개발 테스트 하네스에서만 사용하며, 내장 모바일 복원은 네이티브 C++20 CLI에서 실행됩니다.

```sh
cmake --build build --target check-neverd-mobile
ctest --test-dir build -L NeverDMobileTests --output-on-failure
python3 scripts/test_mobile_android_internal.py --d8 PATH --neverd build/bin/neverd
python3 scripts/test_mobile_android_backend.py --jadx /opt/jadx/bin/jadx --neverd build/bin/neverd
```

구성 요소 테스트는 파싱, 안전하지 않은 컨테이너, 백엔드 실패, 출력 정리, 아키텍처 선택 및 기존 출력 보존을 검증합니다. 빌드된 CLI를 호출하는 테스트는 `NEVERD_BUILD_DIR`을 사용합니다. 내장 Android 실행기는 JDK(`java`, `javac`)와 D8으로 독립 DEX/APK fixture를 만든 다음 복원된 Java를 컴파일하고 실행합니다. 이는 검증 의존성이며 내장 복원의 요구 사항이 아닙니다. 사례를 검증된 것으로 취급하기 전에 현재 빌드로 실행하고 결과를 확인하세요. 별도의 호환성 실행기에는 추가로 JADX가 필요하며 fixture 성공은 임의 애플리케이션 복원을 증명하지 않습니다.

Apple Clang, 해당 SDK 및 빌드된 NeverD가 있는 macOS에서 실제 Objective-C 실행 비교를 수행합니다.

```sh
python3 scripts/test_mobile_ios_backend.py --neverd build/bin/neverd
cmake --build build --target check-neverd-mobile-ios
```

실행기는 자체 Objective-C fixture를 빌드하고 메서드 구현을 복원한 뒤, 복원된 `.m`만 동일한 독립 호출 하네스와 연결합니다. 22개 메서드 fixture는 정수 경계, 분기, 루프, 포인터 읽기/쓰기, 숨겨진 인수와 사용되지 않는 인수, float/double의 동일성 비트, 혼합 매개변수 및 스택 위치를 다루는 141개 관측 결과를 비교합니다. 사례를 검증된 것으로 취급하려면 현재 빌드의 통과 결과가 필요합니다. 요청한 모든 아키텍처와 fixup 변형이 완료되어야 하며, 기본 범위는 arm64/x86_64 × classic/default입니다. 변형이 없거나 호스트에서 실행할 수 없는 아키텍처는 실패이며 건너뛰기를 허용하지 않습니다. 이 fixture 증거는 임의 iOS 프로그램에 대한 완전성을 확립하지 않습니다. `NeverDMobileIOSBackend`는 주 CI 테스트 프로필을 포함하여 macOS의 CTest에 등록됩니다.

독립 Swift 복원 실행기는 `python3 scripts/test_mobile_swift_backend.py --neverd build/bin/neverd`입니다. 원래 바이너리를 연결하지 않고 생성 Swift와 하네스를 다시 컴파일하며, 지원하지 않는 callable과 동작 차이는 실패입니다. 검사 범위의 의미와 보존되는 실패 증거는 [iOS 가이드](ios.md)를 참조하세요.

[Mobile Real Applications 작업 흐름](../../.github/workflows/mobile-real-apps.yml)은 [corpus manifest](../../scripts/mobile_real_apps.json)에 고정된 공개 애플리케이션을 사용합니다. 수락 관문은 각 APK의 모든 DEX와 각 완전한 iOS 번들의 모든 Mach-O에 대한 독립 목록, 원본과 생성 소스의 독립 재빌드, 동작 비교를 요구합니다. 누락된 단계, 알 수 없는 목록 범위 또는 빠진 필수 사례는 관문을 실패시킵니다. 실제 앱 재컴파일과 동작 단계는 아직 불완전하므로 지원은 Experimental 표시를 유지합니다. 하네스 보호 테스트가 통과해도 실제 앱 성공을 확립하지는 않습니다.

Android 사례는 Android SDK 선언만 사용하여 목록에 있는 전체 생성 Java를 javac와 D8으로 컴파일하려고 시도합니다. 원래 애플리케이션 바이트코드, 의존성 구현 및 대체 스텁으로 복원의 누락을 채울 수 없습니다. 부분 복원과 컴파일러 오류는 증거에 남으며, 컴파일이 성공해도 독립 검증, 완전한 APK 재구성 및 ART 동작 비교가 필요합니다. iOS 오라클은 디스크의 Objective-C 메서드 기록을 Apple 도구 출력과 대조하고 class, metaclass, category, list 및 ordinal 식별 정보를 보존합니다. 해결되지 않은 포인터나 생략된 슬롯이 있으면 목록은 알 수 없는 상태로 남습니다. 별도의 기기 및 시뮬레이터 SDK 선언 목록은 프레임워크 가져오기 작업을 지원하지만, 헤더를 인스턴스 배치나 동작의 증거로 취급하지 않습니다.
