**언어**: [English](android.md) | [简体中文](android.zh-CN.md) | [繁體中文](android.zh-TW.md) | [日本語](android.ja.md) | [한국어](android.ko.md) | [Français](android.fr.md) | [Deutsch](android.de.md) | [Español](android.es.md) | [Italiano](android.it.md) | [Русский](android.ru.md) | [العربية](android.ar.md)

# Android Java 복구

[← 문서 목록](README.ko.md)

`neverd mobile`은 별도로 설치한 JADX 백엔드를 통해 APK, DEX, smali 입력에서 읽기 쉬운 Java를 복구합니다. 바이트코드를 검증해 작업 영역에 복사하고, 관련 클래스를 함께 분석한 다음, 생성된 출력을 검사하여 소스 디렉터리와 기계가 읽을 수 있는 보고서를 게시합니다. 이는 실험적인 CLI 기능입니다. APK 컨테이너와 Java 출력은 네이티브 C SDK, Python 플러그인 SDK, GUI 로더 또는 `neverd decompile --language`에서 제공하지 않습니다.

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
| NeverD | `neverd` 타깃을 빌드하고 실행 파일과 같은 위치의 `mobile/` 디렉터리도 함께 배포 | `build/bin/neverd` 또는 PATH의 실행 파일 |
| Python | Python 3.10 이상. 내장 플러그인 호스트의 Python 환경과는 별개 | `--python`, `NEVERD_PYTHON`, PATH의 `python3`/`python` 순서 |
| Java 백엔드 | 표준 DEX 및 smali 입력 플러그인을 포함한 JADX 1.5.6 이상 | `--jadx`, `NEVERD_JADX`, PATH의 `jadx` 순서 |
| Java 실행 환경 | Java 11 이상. 컴파일 및 실행 검증에는 JDK 필요 | `JAVA_HOME` 또는 PATH의 Java |

NeverD는 의존성을 자동으로 다운로드하지 않습니다. 전체 [JADX 배포 패키지](https://github.com/skylot/jadx/releases/tag/v1.5.6)를 받아 `bin/`과 `lib/` 디렉터리 구조를 유지하고, 재배포할 때는 동봉된 라이선스 문서도 보존하세요. 검증된 백엔드 버전은 1.5.6이며, 이후 버전도 같은 CLI 계약을 충족해야 합니다. 이 의존성 설정은 NeverD LLVM 파이프라인 빌드와 별도로 진행합니다.

### Linux와 macOS

```sh
cmake --build build --target neverd
python3 --version
java -version
/opt/jadx/bin/jadx --version

./build/bin/neverd mobile app.apk -o recovered-app \
  --python python3 --jadx /opt/jadx/bin/jadx
```

반복해서 사용한다면 `NEVERD_JADX=/opt/jadx/bin/jadx`를 설정하고, 필요에 따라 `NEVERD_PYTHON`을 인터프리터 경로로 지정합니다. 아직 Java를 사용할 수 없다면 `JAVA_HOME`을 JDK 설치 디렉터리로 설정하세요. 공백이 포함된 경로는 따옴표로 감싸야 합니다.

### Windows PowerShell

```powershell
$env:JAVA_HOME = 'C:\Tools\jdk'
& .\build\bin\neverd.exe mobile .\app.apk -o .\recovered-app `
  --python 'C:\Tools\Python\python.exe' `
  --jadx 'C:\Tools\jadx\bin\jadx.bat'
```

백엔드의 `.bat`/`.cmd` 경로에서 배포 패키지에 있는 단일 `lib/jadx-*-all.jar`를 찾아 NeverD가 Java를 직접 호출합니다. 이 JAR를 `--jadx`에 직접 전달할 수도 있습니다. 애플리케이션 경로를 명령 셸에 삽입하지 않습니다. 다중 구성 빌드에서는 실행 파일이 `build/bin/Release/`에 놓일 수 있습니다. 같은 위치의 `mobile/` 디렉터리 없이 실행 파일만 이동하면 도우미가 없다는 오류가 발생합니다.

## 지원 입력과 범위

| 입력 | 동작 | 주요 제한 |
|------|------|-----------|
| `.apk` | 전체 ZIP을 검증한 뒤 루트의 `classes.dex`, `classes2.dex` 및 이후 번호의 DEX 파일을 함께 분석 | 코드만 처리하며 리소스나 매니페스트는 디코딩하지 않음 |
| `.dex` | DEX 매직 값을 검증한 뒤 백엔드가 내용을 디코딩 | 확장자를 바꾸거나 잘린 파일은 유효한 바이트코드가 아님 |
| `.smali` | 제공한 클래스 분석 | 참조하는 다른 클래스를 암묵적으로 불러오지 않음 |
| smali 디렉터리 | `.smali` 파일을 재귀적으로 수집하여 함께 분석 | 입력 디렉터리에 중첩 클래스와 의존하는 smali 루트들을 포함해야 함 |

`smali/`와 `smali_classes2/`를 포함하는 APK 디코딩 트리를 분석하려면 두 디렉터리의 공통 상위 디렉터리를 전달합니다. 백엔드에는 `.smali` 파일만 전달되지만, 먼저 제공된 트리 전체를 검증하고 복사하므로 관련 없는 큰 에셋도 입력 제한에 포함됩니다. 관련 smali 루트만 모은 작은 디렉터리를 사용하면 작업량을 줄일 수 있습니다.

분할 APK는 각각 별도 입력입니다. DEX를 포함하는 APK는 개별적으로 처리할 수 있지만, 이 명령은 APK 세트를 병합하지 않습니다. 리소스만 포함한 분할 APK는 루트에 DEX가 없어 실패합니다. `.aab`, `.apks`, `.xapk`, `.odex`, `.oat`, `.vdex`는 이 mobile 명령에서 허용하는 입력이 아닙니다. 백엔드가 일부 형식을 지원하더라도 이 NeverD 명령이 해당 형식을 지원하는 것은 아닙니다.

APK 리소스, `AndroidManifest.xml`, 에셋, JNI/네이티브 라이브러리, 실행 중 다운로드되는 코드는 Java로 복구되지 않습니다. 네이티브 `.so`는 별도로 추출한 뒤 `neverd decompile library.so -o library.c`를 사용하세요. 암호화되거나 패킹된 페이로드는 이 정적 분석 흐름에 전달하기 전에 일반 DEX/smali 형태로 준비되어 있어야 합니다. 언패킹, 기기 연결, 보호 우회는 수행하지 않습니다.

## 옵션과 우선순위

```sh
neverd mobile app.apk -o recovered-app --platform=android \
  --jadx /opt/jadx/bin/jadx --timeout=600 \
  --max-files=30000 --max-bytes=4294967296 --json
```

| 옵션 | 기본값 | 의미 |
|------|--------|------|
| `-o DIRECTORY` | 필수 | 디렉터리 입력 바깥에 위치한 새 출력 디렉터리. 기존 출력은 덮어쓰지 않음 |
| `--platform=auto\|android` | `auto` | Android를 명시적으로 선택하거나 입력에서 플랫폼을 추론 |
| `--jadx PATH` | 환경 변수/PATH | 백엔드 실행기 또는 배포 JAR. 명시한 옵션이 우선 |
| `--python PATH` | 환경 변수/PATH | 동봉된 도우미를 실행할 인터프리터. 명시한 옵션이 우선 |
| `--timeout N` | `300` | 버전 확인을 포함한 백엔드 프로세스별 제한 시간. 초 단위의 양수 |
| `--max-files N` | `20000` | 실제로 생성되는 디렉터리를 포함한 항목 수 상한. 양수 지정 |
| `--max-bytes N` | `2147483648` | 입력, 압축 해제 데이터, 최종 출력에 적용되는 바이트 상한. 양수 지정 |
| `--json` | 꺼짐 | 사람이 읽는 요약 대신 JSON 보고서 출력 |

`--arch`의 기본값이 아닌 값, `--artifact`, `--metadata-only`, 0이 아닌 `--max-func`는 iOS용이므로 Android에서는 거부됩니다. `--arch=auto`를 명시적으로 지정하는 것은 허용됩니다. 임의의 백엔드 옵션을 그대로 전달하는 기능은 없습니다. 백엔드 설정, 캐시, 임시 디렉터리는 실행마다 격리되며 기존 백엔드 설정이나 플러그인 설정을 가져오지 않습니다.

이 제한은 리소스 제어 수단이며 백엔드 프로세스를 위한 샌드박스가 아닙니다. 작업 영역도 모니터링되며, 입력·압축 해제 데이터·출력을 위한 공간으로 설정한 항목 수/바이트 예산의 최대 3배까지 허용합니다. 로그는 프로세스마다 16 MiB로 제한됩니다. 큰 입력에는 더 많은 Java 힙이나 더 긴 제한 시간이 필요할 수 있습니다. 한 제한을 늘려도 다른 제한이 해제되지는 않습니다.

## 출력 구조와 JSON 보고서

```text
recovered-app/
  sources/                 복구된 Java 패키지와 클래스
  logs/jadx-version.log    백엔드 버전 확인
  logs/jadx.log            백엔드 진단 정보
  report.json              버전이 지정된 목록과 복구 제한 사항
```

임시 복사본과 백엔드 캐시는 삭제됩니다. Java 파일의 구체적인 이름과 수는 백엔드 재구성 결과에 따라 달라지며, 중첩 클래스가 외부 클래스와 소스 파일 하나를 공유할 수 있습니다. 따라서 Java 소스 파일 수와 DEX 클래스 수는 같지 않습니다.

다음은 일부 내용을 생략한 보고서 예시입니다.

```json
{
  "schema_version": 1,
  "status": "success",
  "platform": "android",
  "source": "app.apk",
  "input_kind": "apk",
  "backend": {"name": "jadx", "version": "1.5.6"},
  "input_code_files": ["classes.dex", "classes2.dex"],
  "dex_count": 2,
  "smali_count": 0,
  "java_source_count": 2,
  "java_sources": ["sources/example/Main.java", "sources/example/Peer.java"],
  "logs": ["logs/jadx-version.log", "logs/jadx.log"],
  "limitations": ["Java is reconstructed from bytecode; original comments, formatting, and stripped names cannot be restored."]
}
```

`input_kind`는 `apk`, `dex`, `smali`, `smali-directory` 중 하나입니다. `input_code_files`에는 입력 바이트코드 이름이나 smali 경로가 나열되며, `java_sources`와 `logs`는 출력 루트 기준 상대 경로입니다. `source`는 입력의 기본 이름입니다. 실제 보고서에는 추가적인 재구성 제한 사항도 포함됩니다. 결과를 다른 도구에 전달할 때도 이 정보를 보존하세요.

자동화에서는 `status`를 사용하기 전에 프로세스 종료 코드를 확인합니다. 표준 출력을 리디렉션할 때는 보고서를 새 출력 디렉터리 밖에 저장하세요.

```sh
neverd mobile app.apk -o recovered-app --json > recovery-result.json
```

도우미 실행이 성공하면 0을, 복구에 실패하면 0이 아닌 값을 반환합니다. 도우미가 시작된 뒤에는 `--json`이 `schema_version`, `status: "error"`, `error`를 포함하는 오류 객체를 생성합니다. 네이티브 인자 파싱, Python 누락이나 3.10 미만 버전, 도우미 누락은 그보다 앞서 실패하여 JSON 대신 stderr로 보고될 수 있습니다. 실행 중단도 stderr로 보고될 수 있으므로 사용 측에서 이러한 경우를 처리해야 합니다.

## 실패 처리와 문제 해결

결과 게시는 트랜잭션 방식으로 이루어집니다. 기존 출력은 보존하고 실패한 작업 영역의 출력은 삭제합니다. 백엔드의 0이 아닌 종료 코드, 로그에 기록된 어셈블/디컴파일 오류, 중복 클래스 때문에 생긴 누락, 명시적인 불완전 코드 표시, 빈 Java 파일, Java가 전혀 생성되지 않은 결과는 모두 명령 실패로 처리됩니다. 백엔드의 성공 보고만으로 메서드별 정확성을 독립적으로 증명할 수는 없습니다.

| 증상 | 조치 |
|------|------|
| Python/도우미 없음 | Python 3.10 이상을 설치하거나 선택하고, `mobile/` 디렉터리를 NeverD 실행 파일과 같은 위치에 유지 |
| 백엔드 실행 불가 또는 지원하지 않는 버전 | `--jadx`, 전체 배포 패키지 구조, Java, 백엔드 최소 버전 확인 |
| 잘못된 DEX 헤더/루트 DEX 없음 | 실제 입력 형식을 확인하고 코드가 있는 APK, 일반 DEX 또는 smali 사용 |
| smali 파일 없음 | Java 소스나 에셋만 있는 트리가 아닌, `.smali` 파일이 포함된 디렉터리 지정 |
| 중복 클래스 또는 부분 복구 | 중복된 입력 정의를 제거하거나 관련 바이트코드 집합을 나누어 분석. 불완전한 결과를 받아들이지 말고 잘못된 smali 수정 |
| 시간 초과/바이트 또는 항목 수 초과 | 관련된 작은 입력으로 범위를 줄이거나 해당 제한을 의도적으로 상향 |
| 안전하지 않은 아카이브 경로나 링크 | 경로 탐색 이름, 링크, 특수 파일, 충돌 경로가 없는 일반적이고 이식 가능한 입력으로 다시 구성 |
| 출력이 이미 존재함 | 다른 출력 디렉터리를 선택하고 이전에 성공한 결과 디렉터리는 재사용하지 않음 |

성공한 실행에서는 백엔드 로그가 보존됩니다. 실패한 작업 디렉터리는 로그를 포함해 삭제됩니다. 백엔드가 0이 아닌 코드로 종료하면 오류에 길이가 제한된 진단 로그 끝부분이 포함되며, 시간 초과와 리소스 예산 오류에는 각각의 메시지가 표시됩니다. 백엔드별 문제를 조사하려면 분리한 입력과 별도 진단 디렉터리를 사용해 백엔드 자체 CLI에서 재현하세요. 실패 전에 일부 Java가 생성되었다는 이유만으로 성공했다고 판단해서는 안 됩니다.

## 검증과 지원 수준

```sh
cmake --build build --target check-neverd-mobile
NEVERD_BUILD_DIR=build python3 -m unittest discover -s scripts/tests -p 'test_mobile_*.py' -v
python3 scripts/test_mobile_android_backend.py --jadx /opt/jadx/bin/jadx --neverd build/bin/neverd
```

첫 두 명령은 mobile 구성 요소와 빌드된 CLI의 계약을 검증합니다. 플랫폼별 테스트 픽스처 요구 사항에 따라 일부 테스트가 명시적으로 건너뛰어질 수 있습니다. 실제 백엔드 테스트 실행기에는 JDK(`java`와 `javac`)도 필요합니다. 단일 smali, 클래스 간 참조/중첩 smali, DEX, 실제 multidex APK 사례를 만든 뒤 복구된 Java를 컴파일하고 실행합니다. 분기, 반복문, 배열, 예외 처리, 클래스 참조, 잘못된 입력, 중복 클래스 누락을 검증합니다. 이는 해당 픽스처에 대한 검증 근거이며 임의의 애플리케이션을 완전히 복구한다는 보장은 아닙니다.

[Mobile Decompilation 워크플로](../.github/workflows/mobile.yml)는 Linux, macOS, Windows에서 Python 3.10 및 3.13으로 구성 요소 테스트를 실행하고, Linux에서 체크섬으로 버전을 고정한 실제 Android 백엔드 작업도 수행합니다. 별도의 iOS 흐름과 현재 제한 사항은 [mobile 개요](mobile.md)(영어)를 참조하세요.
