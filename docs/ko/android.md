**언어**: [English](../android.md) | [简体中文](../zh-CN/android.md) | [繁體中文](../zh-TW/android.md) | [日本語](../ja/android.md) | [한국어](android.md) | [Français](../fr/android.md) | [Deutsch](../de/android.md) | [Español](../es/android.md) | [Italiano](../it/android.md) | [Русский](../ru/android.md) | [العربية](../ar/android.md)

# Android Java 복구

[← 문서 목록](README.md)

`neverd mobile`은 기본적으로 NeverD 내장 엔진을 사용해 APK, DEX, smali에서 읽기 쉬운 Java를 복원합니다. 독립적으로 구현한 판독기가 타입 정보를 갖춘 Dalvik 모델과 처리량이 제한된 Java 생성기를 공유합니다. 실험적인 CLI 기능이며 JADX와 동일한 기능이나 임의 APK의 완전한 복원을 보장하지 않습니다. 네이티브 C SDK, Python 플러그인 SDK, GUI 로더, `neverd decompile --language`는 APK 컨테이너와 Java 출력의 진입점을 제공하지 않습니다.

복구된 Java는 바이트코드를 재구성한 결과입니다. 원래 주석, 서식, 원본 소스 언어의 선택, 제거된 식별자는 되살릴 수 없습니다. Kotlin 바이트코드도 Java로 출력됩니다. 실행 성공이 의미적 동등성을 증명하거나 모든 메서드의 재컴파일을 보장하지는 않습니다. 이 과정에서 분석 대상 애플리케이션을 실행하지 않습니다.

## 빠른 시작

아래에 설명된 실행 환경을 준비한 뒤 새 출력 디렉터리를 선택합니다.

```sh
neverd mobile app.apk -o recovered-app
neverd mobile classes.dex -o recovered-dex
neverd mobile MainActivity.smali -o recovered-class
neverd mobile decoded/smali -o recovered-java
```

`recovered-app/sources/`에서 Java 파일을 읽고, `recovered-app/report.json`에서 입력 목록과 제한 사항을 확인할 수 있습니다. smali 클래스가 서로 참조한다면 디렉터리를 입력으로 사용하는 것이 좋습니다.

## 실행 환경 설정

| 구성 요소 | 요구 사항 | 선택 순서 |
|-----------|-----------|-----------|
| NeverD | C++20을 지원하는 도구 체인으로 `neverd` 대상을 빌드합니다. 모바일 작업 흐름은 네이티브 CLI에 포함되며 Python 인터프리터를 호출하지 않습니다. 배포할 때 해당 빌드에 필요한 네이티브 라이브러리를 함께 제공하세요. | `build/bin/neverd` / PATH |

기본 엔진은 C++20으로 구현되며 실행 시 Python, Java 또는 JADX가 필요하지 않습니다. DEX 035, 037–040 및 smali에서 표현할 수 있는 일반 선언과 연산을 처리합니다. DEX 041, `invoke-custom` 같은 동적 호출, 일부 초기화 경로, 알 수 없는 의미적 어노테이션이나 연산, Java로 표현할 수 없는 식별자는 명시적으로 실패합니다. 파일 형식을 받는다고 그 형식의 모든 명령과 선언을 지원하는 것은 아닙니다.

### Linux와 macOS

```sh
cmake --build build --target neverd

./build/bin/neverd mobile app.apk -o recovered-app
```

`NEVERD_JADX`와 PATH의 `jadx`는 외부 엔진을 선택하지 않습니다. 명시적인 `--jadx PATH`만 호환 어댑터를 선택하며 자동 전환은 없습니다. 공백이 있는 경로는 따옴표로 감싸세요.

### Windows PowerShell

```powershell
& .\build\bin\neverd.exe mobile .\app.apk -o .\recovered-app
```

다중 구성 빌드는 실행 파일을 `build/bin/Release/`에 배치할 수 있습니다. 해당 빌드의 일반적인 네이티브 라이브러리 배포 요구 사항을 따르세요.

## 지원 입력과 범위

| 입력 | 동작 | 주요 제한 |
|------|------|-----------|
| `.apk` | 전체 ZIP을 검증한 뒤 루트의 `classes.dex`, `classes2.dex` 및 이후 번호의 DEX 파일을 함께 분석 | 코드만 처리하며 리소스나 매니페스트는 디코딩하지 않음 |
| `.dex` | 내장 판독기로 DEX 035 또는 037–040 검증 및 파싱 | DEX 041과 지원하지 않는 선언·연산은 실패하며, 이름을 바꾸거나 잘린 파일은 유효한 바이트코드가 아님 |
| `.smali` | 제공한 클래스 분석 | 참조하는 다른 클래스를 암묵적으로 불러오지 않음 |
| smali 디렉터리 | `.smali` 파일을 재귀적으로 수집하여 함께 분석 | 입력 디렉터리에 중첩 클래스와 의존하는 smali 루트들을 포함해야 함 |

`smali/`와 `smali_classes2/`를 포함하는 APK 디코딩 트리를 분석하려면 두 디렉터리의 공통 상위 디렉터리를 전달합니다. 백엔드에는 `.smali` 파일만 전달되지만, 먼저 제공된 트리 전체를 검증하고 복사하므로 관련 없는 큰 에셋도 입력 제한에 포함됩니다. 관련 smali 루트만 모은 작은 디렉터리를 사용하면 작업량을 줄일 수 있습니다.

분할 APK는 각각 별도 입력입니다. DEX를 포함하는 APK는 개별적으로 처리할 수 있지만, 이 명령은 APK 세트를 병합하지 않습니다. 리소스만 포함한 분할 APK는 루트에 DEX가 없어 실패합니다. `.aab`, `.apks`, `.xapk`, `.odex`, `.oat`, `.vdex`는 이 mobile 명령에서 허용하는 입력이 아닙니다. 백엔드가 일부 형식을 지원하더라도 이 NeverD 명령이 해당 형식을 지원하는 것은 아닙니다.

APK 리소스, `AndroidManifest.xml`, 에셋, JNI/네이티브 라이브러리, 실행 중 다운로드되는 코드는 Java로 복구되지 않습니다. 네이티브 `.so`는 별도로 추출한 뒤 `neverd decompile library.so -o library.c`를 사용하세요. 암호화되거나 패킹된 페이로드는 이 정적 분석 흐름에 전달하기 전에 일반 DEX/smali 형태로 준비되어 있어야 합니다. 언패킹, 기기 연결, 보호 우회는 수행하지 않습니다.

## 옵션과 우선순위

```sh
neverd mobile app.apk -o recovered-app --platform=android \
  --timeout=600 --max-files=30000 --max-bytes=4294967296 --json
```

| 옵션 | 기본값 | 의미 |
|------|--------|------|
| `-o DIRECTORY` | 필수 | 디렉터리 입력 바깥에 위치한 새 출력 디렉터리. 기존 출력은 덮어쓰지 않음 |
| `--platform=auto\|android` | `auto` | Android를 명시적으로 선택하거나 입력에서 플랫폼을 추론 |
| `--jadx PATH` | 미설정: 내장 엔진 | 별도로 설치한 JADX 호환 어댑터를 명시적으로 선택. 환경 변수로 선택하거나 자동 대체하지 않음 |
| `--timeout N` | `300` | 내장 분석의 양수 시간 예산. 외부 백엔드는 버전 확인을 포함한 각 프로세스의 초 단위 제한 |
| `--max-files N` | `20000` | 실제로 생성되는 디렉터리를 포함한 항목 수 상한. 양수 지정 |
| `--max-bytes N` | `2147483648` | 입력, 압축 해제 데이터, 최종 출력에 적용되는 바이트 상한. 양수 지정 |
| `--json` | 꺼짐 | 사람이 읽는 요약 대신 JSON 보고서 출력 |

기본값 이외의 `--arch`, `--artifact`, `--metadata-only`, 0이 아닌 `--max-func`는 iOS용이므로 Android에서 거부됩니다. 명시적인 `--arch=auto`는 허용됩니다. 임의의 백엔드 옵션을 전달하는 기능은 없습니다. 명시적으로 선택한 JADX 어댑터는 실행별 설정·캐시·임시 디렉터리를 격리하며 기존 환경의 백엔드 설정이나 플러그인 구성을 가져오지 않습니다.

입력, 압축 해제 데이터, 최종 출력에는 파일 수와 바이트 예산이 적용됩니다. 내장 판독기와 생성기는 처리량 및 경과 시간도 검사합니다. 외부 백엔드 작업 공간은 임시 입력과 중간 출력을 함께 보관하도록 설정한 항목 수·바이트 예산의 최대 3배까지 허용합니다. 로그는 프로세스당 16 MiB로 제한됩니다. 이는 자원 제한이며 샌드박스가 아닙니다. 한 제한을 높여도 다른 제한이 해제되지 않습니다.

## 출력 구조와 JSON 보고서

```text
recovered-app/
  sources/                       복원한 Java 패키지와 클래스
  metadata/android-methods.json  내장 엔진의 메서드 복원 현황
  report.json                    버전이 명시된 목록과 복원 한계
```

임시 입력은 삭제됩니다. 중첩 클래스가 외부 클래스의 소스 파일을 공유할 수 있으므로 Java 파일 수는 DEX 클래스 수와 다릅니다. 생성한 메서드는 Java 디스패치 루프를 사용할 수 있습니다. 원본 DEX를 실행하거나 런타임 브리지로 호출하지 않습니다.

내장 보고서는 `android_method_recovery`를 포함하고 같은 내용을 `metadata/android-methods.json`에 저장합니다. 게시 전에 `method_count = recovered_method_count + declaration_only_method_count`를 만족하고 `unrecovered_method_count`가 0이어야 합니다. 원래의 `native`, `abstract` 메서드는 `declaration-only` 상태이며 복원한 본문 수에 포함하지 않습니다. 아래는 축약한 예시이며, 복원 현황 파일에는 메서드별 목록도 있습니다.

```json
{
  "schema_version": 1,
  "status": "success",
  "platform": "android",
  "source": "app.apk",
  "input_kind": "apk",
  "backend": {
    "name": "neverd",
    "version": "1",
    "execution": "builtin"
  },
  "input_code_files": [
    "classes.dex",
    "classes2.dex"
  ],
  "dex_count": 2,
  "smali_count": 0,
  "java_source_count": 2,
  "java_sources": [
    "sources/example/Main.java",
    "sources/example/Peer.java"
  ],
  "logs": [],
  "android_method_recovery": {
    "schema_version": 1,
    "status": "recovered",
    "class_count": 2,
    "method_count": 6,
    "recovered_method_count": 5,
    "declaration_only_method_count": 1,
    "unrecovered_method_count": 0
  }
}
```

`input_kind`는 `apk`, `dex`, `smali`, `smali-directory` 중 하나입니다. `input_code_files`에는 입력 바이트코드 이름이나 smali 경로가 나열되며, `java_sources`와 `logs`는 출력 루트 기준 상대 경로입니다. `source`는 입력의 기본 이름입니다. 실제 보고서에는 추가적인 재구성 제한 사항도 포함됩니다. 결과를 다른 도구에 전달할 때도 이 정보를 보존하세요.

자동화에서는 `status`를 사용하기 전에 프로세스 종료 코드를 확인합니다. 표준 출력을 리디렉션할 때는 보고서를 새 출력 디렉터리 밖에 저장하세요.

```sh
neverd mobile app.apk -o recovered-app --json > recovery-result.json
```

네이티브 CLI는 성공 시 0, 복구 실패 시 0이 아닌 값을 반환합니다. `--json`은 처리된 실패에 `schema_version`, `status: "error"`, `error`를 포함합니다. 인수 분석, 네이티브 실행 파일이나 라이브러리 시작 실패, 중단은 stderr로만 보고될 수 있습니다. 호출자는 먼저 종료 상태를 확인해야 합니다.

## 실패 처리와 문제 해결

결과 게시는 트랜잭션 방식으로 이루어집니다. 기존 출력은 보존하고 실패한 임시 출력은 삭제합니다. 지원하지 않는 연산, 해결하지 못한 레지스터 흐름, 표현할 수 없는 선언, 잘못된 예외 처리, 예산 소진은 내장 실행을 실패하게 하며 누락된 본문을 게시하지 않습니다. 외부 어댑터도 0이 아닌 종료, 로그의 어셈블·디컴파일 오류, 중복 클래스 누락, 불완전한 코드 표시, 빈 Java 파일과 Java 출력 누락을 거부합니다. 복원 성공은 의미적 동등성의 증명이 아닙니다.

| 증상 | 조치 |
|------|------|
| 지원하지 않는 DEX·명령·선언·초기화 | 명시적 진단과 지원 범위를 확인. 별도의 호환 어댑터를 의도적으로 선택할 때만 `--jadx PATH` 사용 |
| 유효하지 않은 입력 또는 중복 클래스 | 입력 바이트코드나 클래스 구성을 수정. 지원하지 않는 본문은 조용히 생략되지 않음 |
| 시간 또는 자원 예산 초과 | 입력을 줄이거나 가용 자원에 맞게 `--timeout`, `--max-files`, `--max-bytes` 조정 |
| 출력이 이미 존재 | 새로운 출력 디렉터리 선택 |

## 선택 사항인 JADX 호환 어댑터

`--jadx PATH`는 내장 구현이 아닌 외부 JADX를 선택합니다. 표준 DEX/smali 입력 플러그인이 포함된 JADX 1.5.6 이상과 Java 11 이상을 설치하세요. 완전한 [JADX 배포본](https://github.com/skylot/jadx/releases/tag/v1.5.6)을 받고 `bin/`, `lib/` 구조를 유지하며 재배포 시 동봉된 의존성 라이선스를 보존하세요. 자동 다운로드는 없습니다. 어댑터 보고서는 실제 `jadx` 엔진과 감지한 버전을 기록하며 내장 메서드 복원 현황을 제공한다고 주장하지 않습니다.

Windows에서는 배포본의 `.bat`/`.cmd` 실행기 또는 `lib/jadx-*-all.jar`를 지정하세요. NeverD는 배포본 JAR을 찾아 Java를 직접 호출하므로 애플리케이션 경로가 명령 셸에 들어가지 않습니다. Java는 `JAVA_HOME` 또는 PATH로 선택합니다. 어댑터 실행이 성공하면 `logs/jadx-version.log`, `logs/jadx.log`를 보관하고 실패한 임시 디렉터리와 로그는 삭제합니다. 백엔드가 0이 아닌 값으로 종료할 때만 길이가 제한된 로그 끝부분이 오류에 포함됩니다. 실행 실패, 시간 초과, 예산 초과는 별도의 진단을 제공합니다.

```sh
neverd mobile app.apk -o recovered-jadx --jadx /opt/jadx/bin/jadx
python3 scripts/test_mobile_android_backend.py --jadx /opt/jadx/bin/jadx --neverd build/bin/neverd
```

## 검증과 지원 수준

Python은 아래 개발 테스트 스크립트에만 사용합니다. 내장 모바일 복구는 네이티브 C++20 CLI에서 실행됩니다.

```sh
cmake --build build --target check-neverd-mobile
ctest --test-dir build -L NeverDMobileTests --output-on-failure
python3 scripts/test_mobile_android_internal.py --d8 PATH --neverd build/bin/neverd
```

컴포넌트와 CLI 테스트는 파싱, 출력 계약, 실패 시 정리를 확인합니다. 내장 실행 비교 러너는 JDK(`java`, `javac`)와 D8로 독립 DEX/APK 예제를 만들고 복원한 Java를 컴파일·실행합니다. 이는 테스트 의존성이며 내장 복원의 실행 요구 사항이 아닙니다. 현재 빌드로 실행하고 결과를 확인한 뒤에만 해당 사례를 검증했다고 판단하세요. 별도의 호환성 러너는 추가로 JADX가 필요하며 외부 어댑터를 검증합니다. 예제 성공은 임의 애플리케이션의 완전한 복원을 입증하지 않습니다.

관련 iOS 작업 흐름은 [모바일 개요](../mobile.md)를 참고하세요.
