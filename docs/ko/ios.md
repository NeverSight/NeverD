**Languages**: [English](../ios.md) | [简体中文](../zh-CN/ios.md) | [繁體中文](../zh-TW/ios.md) | [日本語](../ja/ios.md) | [한국어](ios.md) | [Français](../fr/ios.md) | [Deutsch](../de/ios.md) | [Español](../es/ios.md) | [Italiano](../it/ios.md) | [Русский](../ru/ios.md) | [العربية](../ar/ios.md)

# iOS 네이티브 코드와 소스 복원

[← 문서 색인](README.md) · [모바일 개요](../mobile.md)

`neverd mobile`은 IPA, `.app`, Mach-O를 받아 네이티브 C, 런타임 메타데이터와 지원되는 네이티브 본문에서 실험적으로 복원한 Objective-C `.m`, Swift `.swift`를 출력합니다. 게시된 결과에도 복원하지 못한 메서드가 포함될 수 있으므로 사용 전에 범위 보고서를 확인하세요. 모바일 컨테이너는 CLI 기능이며 네이티브 C SDK는 선택한 Mach-O를 별도로 로드합니다.

컴파일 과정에서 주석, 서식, 식별자와 소스 언어 구조가 사라집니다. 이 작업은 소스 표현을 재구성하며 원문을 되찾거나 임의 애플리케이션의 동작 동등성을 인증하지 않습니다. 분석 대상 애플리케이션을 실행하지도 않습니다.

## 시작과 의존성

```sh
cmake --build build --target neverd
neverd mobile App.ipa -o recovered-ios
neverd mobile App.app -o recovered-arm64 --arch=arm64
neverd mobile executable -o metadata --metadata-only
neverd mobile App.app -o recovered-framework --artifact Frameworks/Example.framework/Example
```

C++20을 지원하는 도구 체인으로 `neverd` 대상을 빌드합니다. 모바일 작업 흐름은 네이티브 CLI에 포함되며 Python 인터프리터를 호출하지 않습니다. 배포할 때 해당 빌드에 필요한 네이티브 라이브러리를 함께 제공하세요. macOS에서 생성된 Apple 언어 소스를 독립적으로 컴파일하려면 Apple Clang, SDK 및 Swift 도구 체인이 필요합니다. 이는 정적 분석과 별도의 요구 사항이며 선택적 외부 도구는 자동으로 다운로드하지 않습니다.

Swift 서명 복원은 `--swift-demangle PATH`, `NEVERD_SWIFT_DEMANGLE`, PATH의 `swift-demangle` 순서로 도구를 선택합니다. macOS에서는 마지막으로 시간 제한이 있는 `xcrun --find swift-demangle`을 시도합니다. 명시적으로 설정한 도구가 없으면 실패하며, 자동 검색 실패 시 미분류 심볼을 유지하고 `unavailable`을 보고합니다. Swift 심볼이 없는 입력에는 demangler가 필요 없습니다. `--metadata-only`는 네이티브 백엔드와 demangler를 모두 호출하지 않습니다.

```sh
neverd mobile App.ipa -o recovered-swift \
  --swift-demangle /path/to/swift-demangle --timeout=600 --json
```

## 입력과 선택

IPA에는 최상위 `Payload/*.app`이 정확히 하나 있어야 합니다. IPA와 `.app`의 주 실행 파일은 `Info.plist`의 `CFBundleExecutable`로 결정합니다. 두 형식 모두 `--artifact`는 해당 앱 번들 기준 상대 경로이며 내장 실행 파일 하나만 선택합니다. 모든 프레임워크나 확장을 재귀 분석하는 옵션은 아닙니다. 원시 Mach-O 입력은 `--artifact`를 허용하지 않습니다.

Fat 바이너리에서 `--arch=auto`는 arm64, arm, x86_64, i386 순으로 우선합니다. 없거나 지원하지 않는 슬라이스는 명시적으로 실패합니다. 현재 소스 언어 출력은 arm64/x86_64를 대상으로 하므로 다른 계열 선택이 Objective-C/Swift 지원을 뜻하지 않습니다. 선택한 슬라이스가 `cryptid != 0`이면 거부합니다. 이미 복호화되어 읽을 수 있는 입력을 제공하세요. 아카이브와 디렉터리 입력은 위험한 경로, 심볼릭 링크, 특수 파일, 충돌 항목을 거부합니다.

## 옵션과 자원 제한

| 옵션 | 기본값 | 의미 |
|------|--------|------|
| `-o DIRECTORY` | 필수 | 입력 디렉터리 바깥의 새 출력 디렉터리. 기존 출력은 보존 |
| `--platform=auto\|ios` | `auto` | 플랫폼 추론 또는 iOS 선택 |
| `--arch=auto\|arm64\|arm\|x86_64\|i386` | `auto` | Mach-O 슬라이스 하나 선택 |
| `--artifact PATH` | 주 실행 파일 | 앱 기준 실행 파일 상대 경로 |
| `--metadata-only` | 꺼짐 | 소스 복원이나 도구 호출 없이 메타데이터 읽기 |
| `--max-func N` | `0` | 네이티브 함수 제한. 0은 발견한 모든 함수이며 메타데이터 모드에서는 무시 |
| `--swift-demangle PATH` | 환경/PATH/도구 체인 | Swift 서명 demangler |
| `--timeout N` | `300` | 전체 분석의 양수 시간 예산(초). 자식 프로세스는 남은 예산 사용 |
| `--max-files N` | `20000` | 양의 항목 예산. Swift 심볼 목록도 제한 대상 |
| `--max-bytes N` | `2147483648` | 입력, 압축 해제 데이터, 최종 출력의 양의 바이트 예산 |
| `--json` | 꺼짐 | 버전이 있는 보고서를 JSON으로 출력 |

작업 영역을 감시하며 임시 입력과 중간 출력에 설정한 항목/바이트 예산의 최대 3배를 허용합니다. 로그는 프로세스마다 16 MiB, Swift 서명 JSON은 추가로 32 MiB까지입니다. 이는 자원 제어이며 프로세스 격리가 아닙니다. 시간 제한을 늘려도 다른 제한은 유지됩니다. `--max-func`로 제외되었어도 메타데이터 목록에 있는 메서드는 미복원 항목으로 남습니다.

## Objective-C 소스와 런타임 구조

네이티브 로더는 런타임 메서드 기록, 실행 가능한 IMP 주소, 지원되는 타입 인코딩을 명시적인 소스 ABI 위치에 연결합니다. 고정 스칼라/포인터 바인딩은 숨겨진 `self`/`_cmd`, 사용하지 않는 인자, 독립적인 정수/부동소수점 레지스터 뱅크와 지원되는 스택 위치를 유지합니다. float/double 비트 재해석과 수치 변환은 구분합니다. 타입 힌트는 소스 출력 입력일 뿐 인증된 ABI 증거나 실행 코드 패치 권한이 아닙니다.

`sources/objc.m`은 실제 복원한 문장을 `@implementation` 본문에 넣고 필요한 C 도우미와 타입이 연결된 호출을 보존합니다. 호출 대상에는 지원되는 소스 바인딩이 필요하며 알 수 없는 대상과 불완전한 의존성 그룹은 미복원입니다. 정의 누락, 실행 불가능한 주소, 충돌 인코딩, 미지원 ABI, 불완전한 디코딩, 거부된 IR은 선언이 있다는 이유만으로 복원 완료가 되지 않습니다.

클래스 메타데이터는 상위 클래스, 인스턴스 시작/크기, 오프셋·폭·정렬을 검증한 스칼라/포인터 ivar를 유지하며 필요한 패딩을 선언에 삽입합니다. 필요한 인스턴스 배치를 알 수 없는 메서드는 미복원입니다. 카테고리는 클래스/카테고리/주소 식별자와 구현을 분리하며 클래스·카테고리 목록에서 완전히 같은 중복 기록은 한 번만 셉니다. 지원되는 외부 카테고리는 기존 Foundation 클래스 선언을 이용합니다. 알려지지 않은 외부 헤더는 누락 의존성으로 보고하며 대체 클래스 배치를 만들어 내지 않습니다.

지원하는 Objective-C Block 호출에는 숨겨진 Block 객체와 모든 인수 및 반환값의 전달 위치를 포함하는 완전한 고정 스칼라 호출 ABI가 필요합니다. 런타임 인코딩 `@?`를 `id`로 넓히는 것은 선언에만 적용되며 호출 원형을 제공하지 않습니다. 전역 Block 참조는 공유 객체의 동일성을 유지합니다. 지원하는 동기 스칼라 캡처는 네이티브 캡처 저장소와 호출 흐름이 입증되어야 합니다. 이스케이프 또는 비동기 캡처, 모델에 없는 객체/byref 소유권, copy/dispose 도우미와 알 수 없는 레이아웃은 미복원으로 남습니다.

런타임 정보 복원은 제한적입니다. 완전한 프로퍼티·프로토콜, 원래 소유권 주석, 임의 집합형, 가변 인자 꼬리, 예외 의존 본문, 모델에 없는 Block/캡처 배치를 보장하지 않습니다. 런타임 인코딩은 고정 인자만 설명하며 원 선언에 생략 부호가 없었다고 증명할 수 없습니다. 체인 포인터는 로더가 해당 슬롯을 해결한 경우에만 사용하며 나머지 형식에는 진단을 남깁니다.

## Swift 소스와 저장 배치

구조화된 demangler 출력은 호출 가능한 서명과 호출 불가능한 메타데이터를 분리합니다. 지원되는 서명은 본문 출력 전에 선택한 바이너리의 심볼, 진입점, 명시적인 기계 ABI와 연결합니다. Swift 수신자는 Swift ABI를 따르며 Objective-C 숨김 인자로 대체하지 않습니다. 사용자가 제공한 서명 파일도 검증해야 하는 힌트입니다.

실험적 출력기는 지원되는 자유 함수, 클래스 메서드, 지정 초기화 메서드, 고정 배치 struct 메서드를 구성하며 지원되는 mutating 수신자 형태를 포함합니다. 클래스/struct 선언과 저장 필드는 복원된 배치 메타데이터가 필요합니다. 필요한 선언과 본문이 지원되는 완전한 의존성 그룹일 때만 네이티브 호출을 출력합니다. 소스 단위는 선언과 메서드를 함께 포함하며 원래 바이너리를 호출하는 브리지를 만들지 않습니다.

지원하는 Swift getter/setter 본문은 네이티브 구현에서 복원하여 프로퍼티로 조립합니다. 비공개 backing storage는 확인된 필드 레이아웃을 유지하며 초기화 함수와 다른 메서드도 같은 저장소 이름을 사용합니다. 프로퍼티 선언이나 필드 기록만으로 접근자 본문이 복원되었다고 판단하지 않습니다.

지원하는 할당 초기화 함수, 단순 소멸자/해제 함수, 타입 메타데이터 접근자와 `_modify`/resume 진입점은 출력된 타입 단위로 투영할 수 있습니다. 각 항목에는 전체 네이티브 흐름과 효과에 대한 제한된 증명, 실제 복원된 컨텍스트/초기화 함수/프로퍼티 의존성, 관련 본문의 예외 처리 및 IR 검사가 필요합니다. 할당 함수의 쓰기는 실제 초기화 함수와 일치해야 하고 `_modify`는 정확한 가변 필드와 재개 진입점을 연결해야 합니다. 런타임 메타데이터 호출은 복원된 타입 안에서 모델링된 의미를 유지합니다. 이 항목들은 컴파일러 소스 투영으로 명시되며, 각각 복원된 일반 메서드 본문이나 원래 소스 텍스트를 뜻하지 않습니다.

제네릭 또는 resilient 배치, async/throwing 함수, 알 수 없는 호출 규약, 미지원 accessor/allocator/thunk, 불완전한 초기화와 연결되지 않은 네이티브/런타임 의존성은 개별 `unrecovered`로 남습니다. 맹글링된 심볼이나 명목 타입 이름만으로 메서드 복원이 되지는 않습니다. 제거된 심볼과 미분류 demangler 노드는 범위를 불완전하거나 알 수 없게 만듭니다.

## 출력과 범위

```text
recovered-ios/
  artifacts/selected.macho
  sources/native.c
  sources/objc.m
  sources/swift.swift
  metadata/objc.h
  metadata/objc.json
  metadata/objc-methods.json
  metadata/swift.json
  metadata/swift-signatures.json
  metadata/swift-methods.json
  logs/
  report.json
```

소스 언어 파일은 코드를 출력할 수 있을 때만 존재합니다. `objc.json`은 클래스, 카테고리, ivar, 원시 메서드 인코딩을 저장하고 `objc.h`는 지원되는 선언을 담습니다. `swift.json`은 명목 타입과 맹글링된 심볼을 포함합니다. 서명/메서드 JSON은 분류, 생략, 이유, 개수를 유지합니다. 로그에는 네이티브 진단과 사용된 경우 Swift 도구 탐색·demangling·네이티브 Swift 내보내기 진단이 들어갑니다. `report.json`의 경로는 해당 디렉터리 기준입니다. 선택한 바이너리는 분석 산출물이며 생성 소스에 복원 브리지로 링크하지 않습니다.

임시 패키지 복사본과 중간 JSON을 삭제합니다. 일반 실행에서 네이티브 본문이 없으면 메타데이터가 있어도 실패합니다. 메타데이터 모드는 선택 파일, `objc.h`, `objc.json`, `swift.json`, `report.json`만 만들며 소스와 서명/메서드 범위 파일은 없습니다. `native_function_count`, `objc_method_recovery`, `swift_method_recovery`는 `null`. 모든 모드는 네이티브 로더가 해석한 Objective-C 메타데이터를 사용합니다. Swift 메타데이터는 범위를 확인한 네이티브 이미지 읽기를 사용하며 지원하지 않는 fixup, 재배치 가능 레이아웃 또는 참조는 부분 분석 진단을 유지합니다.

다음 축약 예시는 의도적으로 부분 복원을 보여 줍니다.

```json
{
  "schema_version": 1,
  "status": "success",
  "platform": "ios",
  "architecture": "arm64",
  "objc_method_recovery": {
    "status": "partial",
    "method_count": 3,
    "recovered_method_count": 2,
    "unrecovered_method_count": 1
  },
  "swift_method_recovery": {
    "status": "partial",
    "coverage_status": "partial",
    "method_count": 4,
    "recovered_method_count": 2,
    "source_body_method_count": 1,
    "compiler_projection_method_count": 1,
    "unrecovered_method_count": 2,
    "metadata_symbol_count": 5,
    "unclassified_symbol_count": 1
  }
}
```

바깥쪽 `status: "success"`는 검증한 출력을 게시했다는 뜻입니다. `recovered`, `partial`, `unrecovered`, `no-methods`는 발견한 목록에 대한 상태이며 의미 동등성이나 원 프로그램 완전성이 아닙니다. 각 미복원 메서드에는 이유가 있습니다. Objective-C의 `recovered`는 완전한 런타임 메타데이터도 요구합니다. 빈 목록이 메서드가 없었다는 증거는 아닙니다.

Swift `coverage_status`는 분류된 호출 가능 항목만 셉니다. 전체 Swift `status`는 알 수 없는 심볼도 고려하여 `unavailable`, `unclassified`, `unsupported-architecture`, `no-symbols`일 수 있습니다. 호출 불가능 메타데이터는 `non_method_symbols`에 `not-callable`, 알 수 없는 심볼은 `unclassified`로 저장합니다. `types`, `type_metadata_count`, `source_type_count`는 타입 메타데이터와 출력 타입 단위를 별도로 세며 메서드 수를 부풀리는 데 사용하면 안 됩니다.

복원된 Swift 행의 `source_representation`은 `native-method-body` 또는 `compiler-generated-from-type`입니다. 컴파일러 투영에는 `compiler_projection_kind`와 `compiler_projection_evidence`도 보존합니다. `source_body_method_count`는 복원된 네이티브 메서드 본문 수, `compiler_projection_method_count`는 증명된 컴파일러 투영 수이며 합계는 `recovered_method_count`와 같습니다. 컴파일러 진입점도 `method_count` 분모에 남고 정확한 식별 정보가 대응하는 하나의 `type` 소스 단위에 포함되어야 합니다. 타입 메타데이터나 의존성 이름만으로 복원 수를 늘리지 않습니다. 네이티브 일괄 JSON의 컴파일러 행과 타입 단위에는 `source`가 있지만, mobile의 `source_units`는 `source` 없이 설명만 보존하며 전체 소스는 `sources/swift.swift`에 저장됩니다.

Swift 배치 `source_units`는 `{kind, module, name, source, method_entries, method_identities}`를 기록합니다. kind는 `function` 또는 `type`, identity는 `{entry, mangled_symbol}`입니다. 다른 심볼은 같은 진입점을 공유하면서 별도의 ABI 출력을 유지할 수 있습니다. 각 복원 identity는 정확히 한 번 포함되고 미복원 identity는 포함되지 않아야 합니다. `method_entries`는 `method_identities`의 진입점을 순서대로 투영한 목록과 일치하며 중복 주소를 허용합니다. 완전히 같은 중복 identity를 조용히 합치면 안 됩니다. 배치 `source`는 각 단위 소스와 줄바꿈을 순서대로 연결한 값입니다. Mobile은 전체 소스를 `sources/swift.swift`에, 단위 설명을 범위 JSON에 보관합니다. 개별 메서드 `source`는 검사 용도이며 단순 연결로 클래스 선언을 올바르게 복원할 수 없습니다.

## 직접 네이티브 내보내기와 SDK

```sh
neverd export recovered-ios/artifacts/selected.macho \
  --format=objc-methods --max-func=20 -o objc-batch.json
neverd export recovered-ios/artifacts/selected.macho \
  --format=swift-methods \
  --source-signatures=recovered-ios/metadata/swift-signatures.json -o swift-batch.json
```

Swift 내보내기는 demangler를 사용하는 일반 mobile 실행에서 만든 구조화 서명 목록을 받습니다. Objective-C 배치 JSON에는 `native_source`, `native_function_count`, `objc_metadata`와 메서드별 C 소스, 함수 이름, 반환 타입, 인자가 들어 있습니다. Mobile은 `.m` 생성 전에 선언·본문·배치를 추가 검증하므로 최종 범위가 C 배치보다 작을 수 있습니다. 네이티브 내보내기가 성공해도 복원 메서드가 하나도 없을 수 있습니다.

Mach-O를 이미 로드한 세션에서 `neverd_objc_methods_json(session, max_functions)`, `neverd_swift_methods_json(session, signatures_json, max_functions)`가 보고서를 반환합니다. 0은 발견한 모든 함수입니다. 성공 문자열은 `neverd_free_string`으로 해제합니다. `NULL`은 실패이며 세션 오류에 이유가 있습니다. 이 API는 IPA/`.app` 컨테이너를 로드하지 않습니다.

## 검증과 문제 해결

macOS에서 `BUILD_TESTING`을 활성화한 빌드는 `check-neverd-mobile-ios`를 제공하며, CTest로 세 네이티브 복원 테스트 모음을 실행합니다.

Python은 아래 개발 테스트 스크립트에만 사용합니다. 내장 모바일 복구는 네이티브 C++20 CLI에서 실행됩니다.

```sh
cmake --build build --target check-neverd-mobile-ios
ctest --test-dir build -L NeverDMobileTests --output-on-failure
python3 scripts/test_mobile_ios_backend.py --neverd build/bin/neverd
python3 scripts/test_mobile_ios_calls_backend.py --neverd build/bin/neverd
python3 scripts/test_mobile_swift_backend.py --neverd build/bin/neverd
```

macOS의 Objective-C 검증 스크립트는 원본 샘플을 컴파일하고 `.m`을 복원한 뒤 생성 소스만 독립 호출 하네스와 링크합니다. 스칼라 스크립트는 정수 경계, 분기, 루프, 포인터 읽기/쓰기, 숨겨진 인수, float/double 비트 동일성, 혼합 인수와 스택 인수를 검사합니다. 호출 스크립트는 메시지 디스패치, 상속, Category, 인스턴스 변수 저장소, 네이티브 도우미와 Block 호출/캡처/공유 동일성도 검사합니다. 호출 샘플은 arm64/x86_64 × classic/default 조합마다 21/21 메서드 복구와 134/134 독립 예상 결과 일치를 요구합니다. 현재 네이티브 CLI 빌드로 검증하세요.

엄격한 Swift 스크립트는 사용자 선언 22개, getter/setter 진입점 3개, 컴파일러가 생성한 호출 가능 진입점 7개를 검사하며 어떤 항목도 목록에서 빠질 수 없습니다. 변형마다 원본 프로그램의 독립 예상 결과 855개를 확인합니다. 생성된 `.swift`와 하네스를 독립적으로 컴파일하며 원본 dylib, 모듈, 브리지 또는 수동 대체 선언을 사용하지 않습니다. 스칼라/네이티브 호출, 클래스 초기화와 저장소, 구조체 값 전달/mutating 메서드, 부동소수점과 스택 인수, 포인터 및 루프를 포함합니다. 네이티브 C++20 CLI 인수 조건은 arm64/x86_64 × classic/default 네 조합을 건너뛰지 않고 통과하는 것입니다. 조합마다 네이티브 본문 25개와 컴파일러 투영 7개를 복원하고 호출 가능한 식별 정보 32개를 보존해야 하며, 원본과 독립적으로 컴파일한 생성 Swift가 각각 855/855 예상 결과 검사에 통과해야 합니다. 이 결과는 해당 샘플에 한정되며 임의 애플리케이션이나 원래 소스 텍스트의 복원을 보장하지 않습니다. 스크립트는 커버리지 누락, 소스 컴파일 실패와 동작 불일치를 거부합니다.

세 스크립트 모두 `--arch all|arm64|x86_64`, `--fixups both|classic|default`, `--timeout N`, `--work-dir NEW_DIRECTORY`를 지원합니다. `--setup-only`는 원본만 검증하며 복원은 테스트하지 않습니다. 호스트가 실행할 수 없는 아키텍처는 허용되는 경우 명시적으로 건너뛰며, 건너뛰기는 통과가 아닙니다. 보존된 실패 산출물로 소스 커버리지 누락, 컴파일 오류와 동작 차이를 구분하고 검증된 지원을 주장하기 전에 현재 테스트 출력을 확인하십시오.

게시는 트랜잭션 방식입니다. 새 디렉터리를 선택하고 종료 상태부터 확인하며 리디렉션 JSON은 그 밖에 저장하세요. 실패 시 임시 출력을 삭제하고 기존 결과를 보존합니다. 백엔드의 비정상 종료에는 제한된 로그 끝부분이 포함되며 시간 초과와 예산 초과는 별도 메시지입니다. 네이티브 CLI는 성공 시 0, 복구 실패 시 0이 아닌 값을 반환합니다. `--json`은 처리된 실패에 `schema_version`, `status: "error"`, `error`를 포함합니다. 인수 분석, 네이티브 실행 파일이나 라이브러리 시작 실패, 중단은 stderr로만 보고될 수 있습니다. 호출자는 먼저 종료 상태를 확인해야 합니다.

암호화된 슬라이스에는 읽을 수 있는 입력을, 아키텍처 부재에는 사용 가능한 슬라이스 확인을, Swift 도구 부재에는 실제 demangler 선택을 적용하세요. 생략된 메서드의 정확한 이유와 메타데이터 진단을 확인합니다. `--max-func`를 늘리는 것은 개수 제한으로 제외된 함수에만 유효합니다. 배치·서명·외부 헤더·예외·ABI 지원 누락은 구현이나 추가 유효 메타데이터가 필요하며 완전 복원이라는 주장으로 해결되지 않습니다. 배포할 때 적용되는 의존성 라이선스 고지를 보존하세요.
