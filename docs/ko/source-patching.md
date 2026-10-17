**Languages**: [English](../source-patching.md) | [简体中文](../zh-CN/source-patching.md) | [繁體中文](../zh-TW/source-patching.md) | [日本語](../ja/source-patching.md) | [한국어](source-patching.md) | [Français](../fr/source-patching.md) | [Deutsch](../de/source-patching.md) | [Español](../es/source-patching.md) | [Italiano](../it/source-patching.md) | [Русский](../ru/source-patching.md) | [العربية](../ar/source-patching.md)

<!-- i18n-source: 1eae101089e730c29ba3fda92db515fe148fe41b758d6f5b69156252591d9b3f -->

[← Documentation Index](README.md)

# 소스 및 네이티브 오브젝트 패치

NeverD는 선택한 컴파일러로 대체 함수를 컴파일한 뒤 명시된 원래 함수 진입점에 설치합니다. 컴파일러는 C/C++ 의미를, NeverD는 주소 바인딩, 재배치, 저장 공간, 언와인드 레코드와 출력을 담당합니다. 바이너리 수정과 리프팅 후 재작성은 각 파이프라인을 유지하며 나머지 프로그램은 다시 빌드하지 않습니다.

<!-- i18n-section: compiler -->
## 컴파일러 선택

`--compiler=clang`, `--compiler=gcc`, `--compiler=neverc` 중에서 선택합니다. Clang과 일반 모드 NeverC는 LLVM IR을 생성할 수 있으며 `object`는 컴파일러의 기계어를 보존합니다. CLI는 GCC에 오브젝트를 기본으로 선택합니다. Python SDK도 GCC에 오브젝트를 기본 선택합니다. 다른 컴파일러로 조용히 대체하지 않습니다.

포함 경로, 매크로 정의 및 지원하는 소스 옵션은 `--compiler-arg` 또는 SDK의 `compiler_args`로 전달합니다. 대상과 출력 모드 재정의 및 응답 파일 토큰(`@file`)은 별도로 전달하는 옵션 값을 포함한 모든 인수 위치에서 거부됩니다. 일반적인 `-I` 경로와 `-D` 정의는 계속 지원됩니다.

예시 주소를 확인된 원래 함수 진입점으로 바꾸세요. 대체 선언은 호출 규약과 구조체 배치를 포함한 원래 기계 ABI와 일치해야 하며 주소만으로 이를 증명할 수 없습니다. C++에는 `--language=c++`와 적절한 Clang++/G++ 경로가 필요합니다. `extern "C"` 진입점을 사용하거나 정확한 맹글링 이름을 전달하세요. 추가 보조 함수는 허용됩니다. `--entry`와 `--func`가 대체 대상을 명시하며 기존 `--from-c`도 지정 주소를 따릅니다.

`artifact="object"`를 명시할 수도 있습니다.

```sh
# Clang C -> LLVM IR
neverd patch app --from-source=replacement.c --entry=replacement \
  --func=0x401140 --compiler=clang --compiler-path=/path/to/clang \
  --patch-report -o app.clang

# NeverC ordinary C -> native object
neverd patch app --from-source=replacement.c --entry=replacement \
  --func=0x401140 --compiler=neverc --compiler-path=/path/to/neverc \
  --artifact=object --patch-report -o app.neverc

# GCC -> native object (CLI default for GCC)
neverd patch app --from-source=replacement.c --entry=replacement \
  --func=0x401140 --compiler=gcc --compiler-path=/path/to/gcc \
  --patch-report -o app.gcc

# Independently compiled ELF x64 object
clang --target=x86_64-unknown-linux-gnu -fPIC -fno-lto -c replacement.c -o replacement.o
neverd patch app --from-object=replacement.o --entry=replacement \
  --func=0x401140 --patch-report -o app.object
```

<!-- i18n-section: profiles -->
## 검증된 프로파일 및 제한

[NeverC producer: 182fd2c5dc](https://github.com/NeverSight/NeverC/commit/182fd2c5dc)

**NeverC C와 hosted DynCode**

NeverC는 x64/ARM64 ELF, PE, Mach-O에서 순수 C 소스만 받습니다. NeverD는 컴파일러를 실행하기 전에 NeverC와 `--language=c++`의 조합을 거부하며 암시적 변환이나 컴파일러 대체를 하지 않습니다. C++ 및 예외 패치는 Clang 또는 검증된 ELF 객체 프로필에 맞는 GCC를 사용합니다. hosted C 프레임은 실제 언와인드 레코드를 통해 주변 C++ 프로그램이 던지고 처리하는 예외를 전달할 수 있지만 NeverC 자체의 C++ 지원을 뜻하지는 않습니다.

`dyncode`: `--compile-mode=dyncode`는 NeverC `-fdyncode-hosted`를 사용하며 소스, 하나의 명시적 진입점, 네이티브 객체와 기존 심볼 바인딩 없음이 필요합니다. CLI와 Python은 이 모드와 GCC에 객체를 기본 선택합니다. 객체·최종 페이로드 해시, 링크된 모든 코드 바이트와 진입점을 검사하고 실제 CFI/SEH 또는 완전한 native compact 기록으로 언와인드를 보장합니다. 가변/TLS 상태, 암시적 syscall/PEB/heap/string 변환, 미해결 참조와 재배열 코드는 거부합니다. 데이터·콜백 포인터는 인수로 전달할 수 있습니다.

```sh
neverd patch app --from-source=replacement.cpp --language=c++ \
  --compiler=clang --compiler-path=/path/to/clang++ \
  --entry=replacement --func=0x401140 --patch-report -o app.cpp
neverd patch app --from-source=replacement.c --compiler=neverc \
  --compiler-path=/path/to/neverc --compile-mode=dyncode \
  --entry=replacement --func=0x401140 --patch-report -o app.dyncode
```

Clang LLVM IR 경로는 Windows ARM64 C++ FH3를 지원하며 xdata, 부모 프레임 ABI와 funclet/FuncInfo 소유권을 확인합니다. 객체 언어 처리기는 지원하지 않습니다. `check_source_patch_cpp_runtime.py`는 x64/ARM64 네이티브 Windows에서 아홉 예외 시나리오를 실행하며 CPU와 OS 언와인더 증거를 구분합니다.

| 컴파일러 | 산출물 | 검증된 OS 실행 프로파일 |
|---|---|---|
| Clang 22.1.x | LLVM IR | ELF x64/ARM64 C/C++; Mach-O x64/ARM64 C/C++; PE x86/x64/ARM64 C; PE x64/ARM64 C++ FH3; ELF x86 C (`compatibility`, `qemu-user`); ELF ARM32 C (ARM/Thumb, `compatibility`, `qemu-user`) |
| Clang 22.1.6 | 네이티브 오브젝트 | ELF x64/ARM64 C/C++; PE x86/x64/ARM64 C; Mach-O x64/ARM64 C; ELF x86 C (`compatibility`, `qemu-user`); ELF ARM32 C (ARM/Thumb, `compatibility`, `qemu-user`) |
| GCC/G++ 13.3.0 | 네이티브 오브젝트 | 호환되는 Itanium 런타임의 ELF x64/ARM64 C/C++; ELF x86 C (`compatibility`, `qemu-user`); ELF ARM32 C (ARM/Thumb, `compatibility`, `qemu-user`) |
| NeverC 3389.1.5 일반 모드 | LLVM IR | ELF x64 C; Mach-O x64/ARM64 C |
| NeverC 3389.1.5 일반 모드 | 네이티브 오브젝트 | ELF x64 C |
| NeverC 3389.1.5 C | LLVM IR / 네이티브 객체 | ELF / PE / Mach-O x64/ARM64: 순수 C 소스 |
| NeverC 3389.1.5 `hosted-v1` | 네이티브 객체 + 봉인된 페이로드 | ELF / PE / Mach-O x64/ARM64: C 소스와 C 프레임을 통한 예외 전달 |

위 표는 검증된 OS 실행 구성이며 모든 컴파일러 버전이나 옵션을 보장하지 않습니다. ELF, PE, Mach-O 네이티브 설치기는 네 아키텍처를 모두 받아들입니다. GCC는 일치하는 ELF 드라이버가 필요하며 LLVM IR을 생성할 수 없습니다. 알 수 없는 재배치, TLS, 로드 시 전역 생성자, LTO, common/IFUNC 심볼, 미해결 가져오기는 명확히 실패합니다. JSON 기록에는 컴파일러, 인수, 바인딩, 설치 주소가 포함되며 네이티브 객체에는 SHA-256도 기록합니다.

네이티브 객체 기록의 `target` 필드는 로드한 이미지의 ABI에서 결정하며 ARM ELF 부동소수점 플래그와 iOS/watchOS 플랫폼 구분도 포함합니다. 컴파일된 객체와 외부 객체에 같은 식별 규칙을 적용합니다.

CPU 행렬은 열두 조합 모두에서 소스, 외부 IR, 네이티브 객체 설치 성공을 요구합니다. ARMv7k watchOS와 ARMv7 iOS, 가변／읽기 전용 상태, 원본 부동소수점 및 스택 인수 바인딩, 로드 주소 변경 후 진입점 실행도 확인합니다. CPU 실행만으로 OS 로더, C++ 언와인더 또는 임의 ABI를 인증할 수 없습니다. 위 OS 구성을 기준으로 판단해야 합니다.

| Format | `x86` | `x64` | `ARM32` | `ARM64` |
|---|---|---|---|---|
| `ELF` | `CPU` | `CPU` | `CPU` | `CPU` |
| `PE` | `CPU` | `CPU` | `CPU` | `CPU` |
| `Mach-O` | `CPU` | `CPU` | `CPU` | `CPU` |

`CPU`는 소스／IR／객체 설치, 재로드, 정확한 함수 실행을 뜻합니다. ARM ELF는 이미지에서 EABI5, ARM/Thumb 상태, 부동소수점 ABI를 선택하고 완전히 정렬된 `EHABI` 색인을 등록합니다. PE는 x64/ARM 런타임 행과 `HIGHLOW`/`DIR64`/`MOV32T` 기본 주소 재배치를 유지하며 x86은 컴파일러 등록 코드를 사용합니다. Mach-O는 CPU 하위 유형에 따라 watchOS DWARF와 iOS `SjLj`를 구분합니다. 소스 ARM/Thumb 진입점은 상대 분기로 주소 변경을 견딥니다.

네이티브 객체는 자체의 경계가 있는 함수 심볼, 보조／섹션 정의, 언와인드 기록으로 범위를 확인합니다. MC 소유자 ID를 만들거나 IR을 재컴파일하지 않습니다. COFF 함수 범위는 정확한 보조 길이, 단일 함수 섹션(`-ffunction-sections`) 또는 디코딩된 네이티브 런타임 함수 기록으로 확인합니다. 다음 심볼로 길이를 추측하지 않습니다. watchOS 어댑터는 실제 기록을 얻도록 리프 함수 프레임을 유지합니다. 네이티브 COFF/Mach-O 언어 처리기는 검증된 객체 구성에 포함되지 않으며 C++ 예외에는 검증된 IR 경로를 사용해야 합니다. 고정 exidx 등록부를 가진 정적 ARM 언와인더, 등록되지 않은 Mach-O 절대 포인터와 미지원 RELRO 저장소는 거부합니다.

GCC C 객체도 ELF x86, ARM32, ARM64에 설치하고 Linux QEMU user mode에서 실행했습니다. 어댑터는 이미지에서 ARM/Thumb을 선택하며 GNU property는 ELF 포인터 너비로 정렬합니다.

<!-- i18n-section: references -->
## 원래 구현 콜백, 배치 확장과 새 의존성

바인딩은 외부 선언을 지정합니다. `function`은 원래 함수나 임포트 스텁을 인증하고, `data`는 쓰기 가능한 저장소, `const`는 const IR 선언을 요구합니다. 객체 데이터 바인딩에는 0이 아닌 범위가 필요하며 저장소 경계를 넘거나 RELRO 쓰기 권한을 부여할 수 없습니다. 교체하는 함수의 원래 본문을 호출하려면 `original`을 명시합니다. 공개 진입점은 교체 함수를 호출하고, 저장된 진입점은 완전한 명령 단위로 재배치한 접두부를 실행한 뒤 원래 본문으로 이어집니다. 소스, 외부 IR, 네이티브 객체는 부동소수점·스택 인자와 여러 콜백을 포함해 같은 계약을 사용합니다.

콜백에는 인증된 함수 경계, 명령 모드, 복귀 연결 주소와 언와인드 증거가 필요합니다. 알 수 없는 명령, 안전하지 않은 PC 상대 참조, 복사한 접두부 내부로의 분기, 미지원 로더 재배치, 중첩 콜백, 모델링하지 않은 핸들러 프롤로그는 게시 전에 거부합니다. ABI에 맞는 선언은 호출자가 제공해야 합니다. 영수증의 `original_callbacks`에 저장된 진입점을 기록합니다.

PE32/PE32+는 파일 범위를 이동하고 모델링된 디버그·심볼·인증서·오버레이 포인터를 갱신하여 꽉 찬 섹션 헤더 표를 확장하면서 원래 RVA를 유지할 수 있습니다. Mach-O x64/ARM64 PIE는 인접한 page-zero 보호 영역과 마지막 linkedit 세그먼트가 조건을 충족하면 검사된 헤더 접두부 확장으로 명령·언와인드 저장소를 확보하고 원래 코드·데이터 주소를 유지합니다. 이 프로필은 `-Wl,-headerpad,0x400`이나 인접한 빈 언와인드 페이지를 요구하지 않습니다. 명시적 헤더 주소 참조, 알 수 없는 로드 명령, 코드·데이터 소유권 혼합, 잘못된 범위와 미지원 배치는 거부합니다. 설치 전에 헤더 공간과 언와인드 용량을 함께 계획합니다.

새 코드는 RX, 가변 상태는 RW로 유지합니다. 명시적 `--dependency` 요청은 ELF 동적 메타데이터, PE 임포트 설명자/IAT 또는 Mach-O dylib/bind 메타데이터로 함수·데이터를 추가합니다. 기존 임포트 식별·서수·버전 기록을 유지하고 슬롯과 호출 스텁을 구별합니다. 대상 OS 로더에 맞는 라이브러리 이름·경로를 선택해야 합니다. 아래 예는 ELF 공유 라이브러리 이름을 사용합니다. 누락된 라이브러리나 심볼은 실행 시 실제 로더 오류로 확인합니다. TLS, IFUNC, 새 심볼 버전 계약, 로드 시 생성자와 모델링하지 않은 재배치는 미지원이며 ELF `.data.rel.ro`와 CET/IBT도 추가 계약이 필요합니다. 네이티브 COFF 객체는 새 함수·데이터 임포트를 `__declspec(dllimport)`로 선언하여 재배치가 IAT를 식별해야 합니다. 일반 직접 외부 참조는 거부합니다.

```sh
# ELF example: the original target is at 0x401140.
neverd patch app --from-source=replacement.c --entry=replacement --func=0x401140 \
  --bind=original:saved_target=0x401140 \
  --bind=function:original_helper=0x401190 \
  --bind=data:shared_counter=0x404020:4 \
  --bind=const:lookup_table=0x402040:64 \
  --dependency=function:extra_function=libpatch_extra.so \
  --dependency=data:extra_state=libpatch_extra.so \
  --patch-report -o app.bound
```

구조체 크기를 검사하는 추가형 의존성 옵션은 소스, 외부 IR과 네이티브 객체에 공통 적용됩니다. 영수증은 `added_imports`를 포함하고 후보를 다시 로드한 뒤 모든 콜백·임포트·저장소 권한·설치 바이트를 재검사합니다. 실패하면 입력과 기존 대상을 보존합니다.

<!-- i18n-section: examples -->
## 실행 가능한 예제

[Windows/macOS/Linux 전체 예제](../../examples/source-patch/README.md) 에는 원본 프로그램, 교체 함수, 의존 라이브러리 소스와 빌드 명령, 실제 호스트에서 실행되는 Python SDK 플러그인이 있습니다. Python이 활성화된 NeverD 빌드를 사용하여 저장소 루트에서 새 출력 디렉터리로 실행합니다.

```sh
# Linux / macOS
python3 examples/source-patch/run.py --neverd build-example-host/bin/neverd --clang clang --output build-example-native
```

```powershell
# Windows: Visual Studio developer PowerShell
python examples/source-patch/run.py --neverd build-example-host/bin/neverd.exe --clang clang --output build-example-native
```

기본적으로 두 경로를 모두 실행합니다. `--api cli` 또는 `--api sdk`로 하나를 선택할 수 있습니다. 대상 주소는 자동으로 찾습니다. 입력 `7`에서 원본은 `8`, 두 패치는 모두 `36`을 출력합니다. 드라이버는 실제 실행, 영수증 및 라이브러리 누락 시 로드 실패를 검사하고 전체 빌드와 patch 명령을 `commands.json`에 저장합니다.

<!-- i18n-section: exceptions -->
## C++ 예외

ELF는 컴파일러 FDE/LSDA, Mach-O는 compact unwind/DWARF를 사용합니다. PE x64/ARM64는 컴파일러 FH3 테이블을 허용하며 원래 프로그램에 호환 MSVC 런타임과 타입 식별자가 필요합니다. type-info 데이터는 명시적 바인딩이 필요할 수 있습니다. FH4, 사용자 정의 personality, SEH 소스 및 활성 CFG/EHCont/XFG 모드는 거부하며 원래 보호 플래그는 유지합니다.

IBT 요구가 있는 ELF x86/x64 원본은 현재 거부합니다. 소스 설치기는 아직 ENDBR을 보존하는 트램펄린을 제공하지 않습니다.

실행 사례는 정상 반환, 내부 catch, 원래 호출자의 새 예외 포착, 원래 피호출 함수의 예외 전파, 정확히 한 번의 RAII 정리, 재던지기, noexcept 종료와 변경하지 않은 함수를 검증합니다. 원래 호출 지점은 예외 전파를 이미 허용해야 합니다. 원래 컴파일러가 예외를 던지지 않는 함수 주위에서 제거한 catch 경로는 패치로 복원할 수 없습니다.

<!-- i18n-section: publication -->
## SDK 및 출력

추가 방식 C API는 `neverd_patch_source`와 `neverd_source_patch_options_v1`이며 `neverd_patch_source_report`가 기록을 반환합니다. 오브젝트 입력은 바이트 길이를 전달합니다. 기존 `neverd_patch_from_c`도 사용할 수 있습니다.

입력을 스냅샷으로 저장하고 목적지 옆에 후보를 생성한 다음 다시 로드하여 요청된 모든 진입점을 확인하고 배타적으로 출력합니다. 기존 파일이나 심볼릭 링크 별칭은 덮어쓰지 않습니다. 매핑 하나가 누락되어도 전체 요청이 실패합니다. `--mode=inplace`는 거부됩니다. 세션 IR 변환은 네이티브 오브젝트에 적용할 수 없습니다. Mach-O는 서명을 엄격히 확인하고 재서명하며 PE 서명을 조용히 제거하지 않습니다.

```python
report = session.patch_source(
    replacement_source, "app.patched",
    mappings={"replacement": original_address},
    compiler="clang", compiler_path="/path/to/clang", artifact="object",
    bindings={
        "saved_target": ("original", original_address),
        "shared_counter": ("data", original_counter_address, 4),
    },
    dependencies={
        "extra_function": ("libpatch_extra.so", "function"),
        "extra_state": ("libpatch_extra.so", "data"),
    },
)
```

<!-- i18n-section: verification -->
## 빌드 및 검증

선택한 여섯 네이티브 OS/ISA 작업은 실패·건너뛰기 없이 통과했습니다. GoogleTest 실행 3669회, 네이티브 프로세스 관측 1008회(기능 504회, C++ 예외 504회), 실제 로더 거부 12회, 이미지 96개, 게시 영수증 84개입니다.

| 호스트 | 소스 | Harness | CI 작업 | 테스트 | 관측 / 거부 |
|---|---|---|---|---|---|
| Linux x64 | [61fb1d3e759](https://github.com/NeverSight/NeverD/commit/61fb1d3e759f6796d23341782e7f4d624295f27c) | [197126208b6](https://github.com/gmh5225/test_mac_intel/commit/197126208b665bec0db5c6dc74278931300229af) | [116845056048](https://github.com/gmh5225/test_mac_intel/actions/runs/38861651227/job/116845056048) | 626 | 168 / 2 |
| Linux ARM64 | [61fb1d3e759](https://github.com/NeverSight/NeverD/commit/61fb1d3e759f6796d23341782e7f4d624295f27c) | [197126208b6](https://github.com/gmh5225/test_mac_intel/commit/197126208b665bec0db5c6dc74278931300229af) | [116845055863](https://github.com/gmh5225/test_mac_intel/actions/runs/38861651227/job/116845055863) | 600 | 168 / 2 |
| macOS Intel | [b7a180c7ac3](https://github.com/NeverSight/NeverD/commit/b7a180c7ac35fc2f05be3568a86104d42fc32800) | [cba0906a114](https://github.com/gmh5225/test_mac_intel/commit/cba0906a11461d6e29bbe68f9337515fe084b610) | [117185095063](https://github.com/gmh5225/test_mac_intel/actions/runs/38963782346/job/117185095063) | 624 | 168 / 2 |
| macOS ARM64 | [61fb1d3e759](https://github.com/NeverSight/NeverD/commit/61fb1d3e759f6796d23341782e7f4d624295f27c) | [f6311a611e0](https://github.com/gmh5225/test_mac_intel/commit/f6311a611e095bcffb7537c7ae3b40c826d1e2e1) | [116879179194](https://github.com/gmh5225/test_mac_intel/actions/runs/38872352550/job/116879179194) | 624 | 168 / 2 |
| Windows x64 | [76ce83e138d](https://github.com/NeverSight/NeverD/commit/76ce83e138de639b8d75206c78bd5ccbb9cfd8dd) | [a42b117681f](https://github.com/gmh5225/test_mac_intel/commit/a42b117681f81d922e09d98ccb1a1c4525055947) | [116908799030](https://github.com/gmh5225/test_mac_intel/actions/runs/38881414688/job/116908799030) | 598 | 168 / 2 |
| Windows ARM64 | [76ce83e138d](https://github.com/NeverSight/NeverD/commit/76ce83e138de639b8d75206c78bd5ccbb9cfd8dd) | [cba0906a114](https://github.com/gmh5225/test_mac_intel/commit/cba0906a11461d6e29bbe68f9337515fe084b610) | [117014878954](https://github.com/gmh5225/test_mac_intel/actions/runs/38913475295/job/117014878954) | 597 | 168 / 2 |

각 산출물은 아래 표의 정확한 소스와 harness 커밋만 검증합니다. 이후 dev 통합과 문서 커밋에는 별도의 로컬·문서 검사를 적용합니다. 과거 실패·취소 작업은 실패로 보존합니다. 과거의 보충 관측 216회, PE ARM32 및 구형 32비트 Apple CPU/호환 계층 결과는 새 기능의 네이티브 하드웨어나 C++ 예외 증거가 아닙니다.

Windows ARM64 검증은 고정 서브모듈과 검증된 LLVM-MinGW 도구 체인으로 실제 빌드한 네이티브 ARM64 Unicorn C DLL, MSVC 임포트 라이브러리, 명시적 CMake 대상 재정의를 사용합니다. 격리된 빌드 복사본은 비 SEH 점프 함수 쌍, 명령 캐시 동기화를 포함한 ARM64 페이지 커밋 콜백, Windows LLP64용 64비트 즉시값 마스크라는 세 호스트 수정을 인증합니다. 서브모듈 소스와 리비전은 그대로입니다. 이는 CPU 행렬의 테스트 호스트를 검증하며 기본 MSVC ARM64 JIT 지원을 입증하지 않습니다. 다섯 호스트 복구 검사는 패치 프로세스의 네이티브 실행 및 C++ 예외 증거와 별도로 기록합니다.

`NEVERD_SOURCE_PATCH_TEST_CLANG_EXECUTABLE`로 호환 컴파일러를 선택하세요(Clang 22.1.6 검증). 통합 빌드는 빌드 디렉터리의 LLVM 복사본만 수정하고 서브모듈과 공유 사전 빌드 캐시는 유지합니다. r3/r4의 x64 PLT 버그는 IR 패치를 거부하게 할 수 있습니다. 네이티브 오브젝트는 해당 MC 경로를 거치지 않습니다. `NEVERD_TEST_REQUIRE_FIXED_LLVM=1`은 수정된 정확한 PLT 생성을 요구합니다.

`NeverDSourcePatchTests`는 네이티브 C/API/오브젝트/예외를, `NeverDSourcePatchMatrixTests`는 Unicorn으로 모든 조합을 검사합니다. `NEVERD_TEST_WINDOWS_CRT_LIB`, `NEVERD_TEST_WINDOWS_UCRT_LIB`, `NEVERD_TEST_WINDOWS_SDK_LIB`를 설정하면 `SourcePatchPE`가 C++ 시나리오 7개를 실행합니다. Windows 이외 호스트에는 `NEVERD_TEST_WINDOWS_RUNNER`도 필요합니다. `NEVERD_TEST_REQUIRE_WINDOWS_RUNTIME=1`은 입력 누락을 실패로 처리합니다. Windows 전용 워크플로는 네이티브 실행을 요구하고 건너뛴 사례를 거부합니다.

개발 Mac에서 Linux 검증은 Docker와 명시적 Linux 로더를, PE 검증은 Wine 11을 사용합니다. Mach-O ARM64는 Mac 자체에서 실행합니다. Windows 네이티브 CI 결과는 별도로 확인해야 하며 작업 구성만으로 실행 성공을 의미하지 않습니다. 기록, 기존 실패 및 에뮬레이터 제한은 [상세 계약과 검증 기록](../source-patching-details.md)을 참조하세요.

```sh
cmake -S . -B build-source -G Ninja -DCMAKE_BUILD_TYPE=Release \
  -DBUILD_TESTING=ON -DNEVERD_LLVM_PREBUILT=OFF \
  -DNEVERD_ENABLE_SEMANTIC_TESTS=ON \
  -DNEVERD_SOURCE_PATCH_TEST_CLANG_EXECUTABLE=/path/to/clang
cmake --build build-source --target NeverDSourcePatchTests \
  NeverDSourcePatchMatrixTests --parallel 4
ctest --test-dir build-source -L \
  '^(NeverDSourcePatchTests|NeverDSourcePatchMatrixTests)$' \
  --output-on-failure --parallel 4
```

LLVM 빌드 복사본은 Mach-O x64 GOT 데이터 참조도 수정합니다. Clang의 타입 catch 테이블은 4바이트 재배치 필드 끝을 기준으로 합니다. r3/r4는 4바이트 어긋나며 출력 전 검증에서 수정되지 않은 결과를 거부합니다. 개발 Mac의 x64 Mach-O 사례는 Rosetta 2로, ARM64 사례는 직접 실행합니다.

Windows x64 네이티브 검증 [2ed7e78fc](https://github.com/NeverSight/NeverD/actions/runs/38307461805)에서 PE/객체/trampoline 전용 테스트 16개와 CPU 대상 행렬 테스트 143개가 건너뛰기 없이 모두 통과했습니다. FH3 테스트는 C++ 실행 시나리오 7가지를 검증했으며 대상 행렬은 Unicorn을 사용합니다. 이 실행은 Clang 22.1.6, 네이티브 MSVC 및 Windows SDK, 통합 LLVM을 사용했습니다.

Windows 워크플로는 x86, x64, ARM64용 PE 교체 바이너리 27개도 생성합니다. `scripts/check_source_patch_pe_runtime.py`는 소스 IR, 컴파일된 객체, 외부 객체 경로에서 비리프 호출, 변경 가능한 포인터 상태, 부동소수점 및 스택 인수를 검사합니다. 원본과 각 교체 결과를 세 번씩 실행하여 아키텍처마다 프로세스 실행 30회를 관측합니다. x86/x64는 Windows x64에서, ARM64는 `windows-11-arm`에서 실행합니다. 검증기는 네이티브 호스트, 커밋, 파일 해시의 일치와 전체 사례 목록을 요구하며 외부 실행기의 결과는 별도로 기록합니다. 이 프로파일은 ARM64 C++ 예외 지원을 입증하지 않습니다.

행렬의 선택적 컴파일러는 `NEVERD_TEST_MATRIX_NEVERC`로 지정합니다. NeverC 3389.1.5는 x64/ARM64와 ELF, PE, Mach-O 조합의 66개 검사를 모두 통과했으며 IR 및 네이티브 객체 설치와 CPU 실행을 확인했습니다. 이 컴파일러 버전은 x86과 ARM32를 지원하지 않으므로 해당 대상에는 Clang을 사용하며 ELF에는 호환 GCC 드라이버도 사용할 수 있습니다. NeverC의 PE 컴파일 시 어댑터는 `-fno-autolink`를 추가하여 다른 CRT를 링크하지 않고 기존 이미지에서 참조를 해결합니다. `.drectve` 링크 계약이 있는 외부 객체는 계속 거부됩니다.

Windows 네이티브 프로세스 검증 [a4a5ae3cb](https://github.com/NeverSight/NeverD/actions/runs/38350079817)에서 소스 IR, 컴파일된 객체, 외부 객체 경로를 포함한 관측 90회가 모두 통과했습니다. x86, x64, ARM64 각각 30회이며, x86/x64는 Windows x64에서, ARM64는 동일한 해시 검증 이미지를 사용해 `windows-11-arm`에서 실행했습니다. 같은 실행에서 PE/객체/trampoline 전용 검사 16개와 Unicorn 대상 행렬 검사 143개도 건너뛰기 없이 모두 통과했습니다. FH3 사례는 x64 C++ 실행 시나리오 7가지도 계속 검증했습니다.

네이티브 검증은 NeverC `182fd2c5dc`, 독립 원본 및 현재 NeverD 설치기를 사용합니다. `check_source_patch_hosted_pe_runtime.py`는 Windows 아키텍처마다 C IR/객체/hosted 결과와 두 C 프레임의 예외 전달을 36회 관측합니다. `check_source_patch_elf_runtime.py`는 Linux 아키텍처마다 C, hosted 전달과 아홉 Clang C++ 예외 시나리오를 120회 관측합니다. 영수증, 해시, 실제 호스트 아키텍처가 일치해야 하며 CI 호스트에서 NeverC 실행 파일을 실행했다는 뜻은 아닙니다. Linux x64와 ARM64에서 Clang IR, Clang 네이티브 객체, GCC 네이티브 객체 각각의 아홉 예외 시나리오를 검증합니다. NeverC는 계속 C 전용입니다.

이전 변환 프로필의 204회 관측은 과거 기록이며 현재 C 전용 계약의 근거가 아닙니다. 현재 세 NeverC 경로는 C를 사용하고 Clang C++ FH3는 별도로 검증합니다. 개발용 Apple Silicon Mac에서 macOS x64는 Rosetta 2를 사용하며 Unicorn/QEMU CPU 결과는 네이티브 OS와 언와인더 검증을 대신하지 않습니다.

AArch64 명령어 재배치에는 명시적인 RELA 가수가 필요합니다. 암시적 REL 명령어 가수는 게시 전에 거부되며 명령어 비트 필드를 일반 데이터로 해석하지 않습니다. 지원되는 PREL64 데이터 재배치는 REL과 RELA 모두에서 전체 부호 있는 가수를 유지합니다.

C 전용 생성기 검증 [cc91afb3b](https://github.com/NeverSight/NeverD/actions/runs/38591930213)은 네이티브 프로세스 관측 312회를 모두 통과했습니다. Windows x64와 ARM64는 각각 36회, Linux x64와 ARM64는 각각 120회입니다. Linux는 Clang IR, Clang 네이티브 객체, GCC 네이티브 객체의 아홉 예외 시나리오를 각각 검증하며 NeverC의 IR, 객체, hosted DynCode 입력은 모두 C입니다.

설치기를 다시 빌드한 검증 [cc91afb3b](https://github.com/NeverSight/NeverD/actions/runs/38591928600)은 PE/객체 검사 19개, CPU 매트릭스 143개 전체, Windows/Linux 네이티브 프로세스 관측 474회를 통과했습니다. Windows x86 프로세스는 Windows x64 호스트에서, x64와 ARM64는 일치하는 호스트에서 실행했습니다. 별도 [Linux 검증](https://github.com/NeverSight/NeverD/actions/runs/38585759996)은 기존 108개와 추가 LLVM-C 3개, Clang/GCC C++ 3개를 모두 통과했으며 실패와 건너뛰기는 없습니다.

[통합 검증 0a6f6d768](https://github.com/NeverSight/NeverD/actions/runs/38627694097)은 Windows/Linux의 x64 및 ARM64 네이티브 환경에서 312회의 프로세스 관측을 모두 재검증했습니다. 최종 이미지 30개는 이전에 검증한 바이트와 일치하며, 소스 해시와 C 전용 출력 영수증을 갱신하고 독립적으로 확인했습니다.

[0a6f6d768의 확장 Linux 검증](https://github.com/NeverSight/NeverD/actions/runs/38626866257)은 원래 108개, 전체 Swift 55개, CPU 대상 143개, 네이티브 세션 117개, LLVM-C 3개, Clang/GCC C++ 3개를 합친 429개 검사를 실패나 건너뜀 없이 실행했습니다. 이 통합 버전에서 전체 로컬 바이너리 패치 회귀 22,961개도 모두 통과했습니다. 네이티브 프로세스 수와 CPU 에뮬레이션 검증 범위는 구분하여 기록합니다.

[0a6f6d768의 Windows 설치기 재빌드 검증](https://github.com/NeverSight/NeverD/actions/runs/38627581732)에서 집중 검사 182개와 네이티브 프로세스 실행 474회가 모두 통과했으며 실패나 건너뛰기가 없었습니다. 독립적인 산출물 감사에서 각 이미지의 해시, 아키텍처, 시나리오와 반복 횟수를 확인했습니다. 이후 ABI/언와인드 수정을 통합한 `e1cac949f`에서도 전체 바이너리 패치 테스트 22,961개가 다시 모두 통과했으며 실패나 건너뛰기가 없었습니다. NeverC는 순수 C만 처리하며 C++ 대체 코드는 Clang/GCC를 사용합니다.

<!-- i18n-section: runtime-coverage -->
## macOS 및 32비트 프로세스 검증

프로세스 이미지 워크플로는 macOS ARM64(`macos-15`)와 Intel x64(`macos-15-intel`)의 네이티브 실행을 요구합니다. 각 아키텍처에서 96회 관측하며 Clang C 소스/객체 설치, 쓰기 가능한 포인터 상태, 부동소수점/스택 인수, 9가지 Clang C++ 예외 시나리오, 기존 NeverC 순수 C의 IR/객체/hosted 출력을 확인합니다. 실행 전에 Mach-O 서명을 엄격하게 검사하고 설치된 hosted 코드와 컴파일러 페이로드의 바이트 단위 일치를 요구합니다.

같은 워크플로에서 Windows x64의 Windows x86 관측 30회, Linux x64의 Linux x86 관측 48회, QEMU 사용자 모드의 Linux ARM32 관측 96회를 요구합니다. ARM32는 ARM과 Thumb에 절반씩 배정하며 Linux 검사에는 해당 대상의 GCC 객체도 포함합니다. 각 시나리오는 세 번 반복합니다. 보고서는 `native`, `compatibility`, `rosetta`, `qemu-user`를 구분하고 대상 및 실제 호스트 아키텍처를 함께 기록합니다. Rosetta는 Intel 네이티브 실행 요건을 충족하지 않습니다.

전체 14개 CPU 대상 프로필은 계속 283개 소스/IR/객체 행렬로 검증합니다. PE ARM32, Mach-O i386, ARMv7 iOS, ARMv7k watchOS는 현재 CPU 검증 범위이며 네이티브 OS 로더나 C++ 언와인더의 검증을 뜻하지 않습니다. NeverC는 x64/ARM64 순수 C 전용으로 유지합니다. 32비트 생성에는 Clang과 해당 ELF GCC 드라이버를 사용합니다. 검증기는 NeverC를 빌드하거나 수정하지 않습니다.

Windows x64와 ARM64에서도 각각 Clang C++ FH3 관측 36회를 실행합니다.

Mach-O 쓰기 가능 포인터는 실행 중 초기화합니다. 정적 주소 초기화는 아직 지원하지 않는 로드 시 재배치 등록이 필요하므로 명시적 거부 회귀 테스트로 확인합니다. ELF 크기 0 COMDAT 함수는 서명이 실행 가능 섹션의 시작에 있는 유일한 함수이고 CFI가 섹션 전체와 정확히 일치할 때만 허용합니다. 내부 오프셋, 공유, 패딩, 그룹 없음, CFI 누락은 계속 거부합니다.

ELF ARM과 Thumb는 별도 프로필입니다. 소스 생성기에 `-marm` 또는 `-mthumb`를 명시하며 네이티브 함수 바인딩은 원래 명령 상태 비트를 유지합니다. 32비트 ELF 프로세스는 실제 동적 재배치가 있는 PIE 이미지를 사용합니다. ARM32 QEMU 실행은 glibc의 명시적 `--loader`와 sysroot를 기록합니다. QEMU AUXV 계산이 이동된 프로그램 헤더를 Linux 커널과 같은 방식으로 처리하지 않기 때문입니다. 이 경로는 QEMU에서 glibc 로딩과 실행을 검증하며 ARM32 실기기나 네이티브 커널 로더의 증거가 아닙니다.

이 패치된 ELF 이미지에서는 Linux x86과 ARM32의 `qemu-user` 경로 모두 대상 아키텍처에 맞는 sysroot 안의 glibc `--loader`를 명시해야 합니다. Linux x86의 `compatibility` 경로는 Linux x64 호스트 커널의 32비트 호환 기능을 사용하며 QEMU 실행과 별도로 기록됩니다. 이는 네이티브 32비트 x86 호스트에서 실행되었다는 증거가 아닙니다.

[`fcee81c9a5e2f20a71090ade23ae833175f64c59` 프로세스 검증](https://github.com/NeverSight/NeverD/actions/runs/38694434082)은 모든 750회 관측에 통과했습니다. 일치하는 네이티브 호스트에서 576회, x86 호환 모드에서 78회, QEMU 사용자 모드의 ARM/Thumb에서 96회 실행했습니다. 독립적인 산출물 감사에서 126개 이미지의 해시, 실행 환경, 전체 시나리오 목록을 확인했습니다. [Windows 재빌드 검증](https://github.com/NeverSight/NeverD/actions/runs/38694628242)은 전용 검사 194개와 Windows/Linux 프로세스 관측 474회에 통과했습니다. [Linux 재빌드 검증](https://github.com/NeverSight/NeverD/actions/runs/38694638358)은 기존 실패 62개와 건너뛴 46개를 모두 포함한 444개 검사에 실패나 건너뛰기 없이 통과했습니다. 위의 하드웨어 및 로더 검증 범위는 그대로 유지됩니다. NeverC는 순수 C 전용이며 저장소를 변경하지 않았습니다.

추가 [ARM/Thumb 검증](https://github.com/gmh5225/test_mac_intel/actions/runs/38750504263)은 `ubuntu-24.04-arm`에서 `fcee81c9a5e2f20a71090ade23ae833175f64c59`의 C 테스트 이미지 32개를 실행하여 96회 관측 모두 통과했습니다(ARM과 Thumb 각각 48회). Linux 커널은 QEMU나 명시적인 로더 명령 없이 `/lib/ld-linux-armhf.so.3`을 통해 각 이미지를 직접 로드했습니다. 이는 ARM64 호스트의 `compatibility`이며 네이티브 32비트 호스트 검증은 아닙니다. 테스트 하네스 커밋 `3ad17ac9608d6bd9f5a4d44953ec375524e5730b`와 이미지 커밋은 별도로 기록됩니다. 독립적인 산출물 감사로 이미지 해시, 게시 영수증 30개 전체, 모든 시나리오와 반복 횟수, 그리고 일치하는 `binfmt_misc` 인터프리터 규칙이 없음을 확인했습니다.

별도의[호스팅 runner 조사](https://github.com/gmh5225/test_mac_intel/actions/runs/38749138719)에서 제공된 Windows 11 ARM64 runner는 ARM32 프로그램 실행을 `WinError 216`으로 거부했습니다. macOS 15.7.9와 Xcode 16.4 환경에서는 제공된 SDK로 macOS i386 또는 iOS ARMv7 프로그램을 링크할 수 없었습니다. watchOS ARMv7k 프로그램은 링크에 성공했지만 실제 기기 실행 증거는 확보되지 않았습니다. 조사 작업의 성공은 해당 플랫폼의 검증 완료를 뜻하지 않으며 네이티브 하드웨어 검증은 아직 남아 있습니다. NeverC는 빌드하거나 변경하지 않았습니다.

추가 [개인 Actions Linux 검증](https://github.com/gmh5225/test_mac_intel/actions/runs/38773830274)에서는 32비트 Linux 커널과 glibc를 갖춘 완전한 QEMU TCG ARM32 게스트 안에서 ARM/Thumb 실행 96회가 통과했고, 같은 게스트의 Wine에서 PE ARM32 실행 30회도 통과했습니다. 게스트 커널이 ELF 이미지를 직접 로드합니다. 이는 시스템 전체 에뮬레이션이며 명시적 로더를 사용하는 `qemu-user` 경로와 구분합니다. ARM64 호스트 호환 계층에서 수행한 Wine 레지스트리 설치는 별도로 기록하며 패치 실행 횟수에는 포함하지 않습니다. 게스트의 Wine 초기화와 모든 측정 실행은 원래 제한 시간 안에 성공해야 합니다.

[Apple 호환성 검증](https://github.com/gmh5225/test_mac_intel/actions/runs/38771214358)에서는 Linux의 Darling에서 Mach-O i386 실행 30회, Intel macOS의 touchHLE에서 ARMv7/ARMv7k 실행 60회가 통과했습니다. 원본과 소스 IR, 컴파일 객체, 외부 객체를 사용한 각 교체 결과를 세 번씩 실행합니다. 독립 감사는 이 추가 실행들의 이미지 72개, 패치 게시 기록 66개, 실행 216회 전체와 GitHub 산출물 크기, SHA256, 정확한 커밋, 전체 사례 목록을 확인했습니다. Linux 입력은 계속 `fcee81c9a5`에 고정되며 PE 입력은 `a542f97ba`, Mach-O 입력은 `8f5f06762`에서 생성했습니다. 검증 스크립트 커밋은 연결된 실행에 별도로 기록합니다.

Mach-O 수정은 이미지 전체의 ARM/Thumb 혼합 정보와 별개로 선택된 Thumb 함수를 인증하고, 증거가 있는 Thumb 내보내기 포인터와 ARM32 언와인드 코드 주소를 정규화합니다. 데이터 포인터나 범위 길이는 바꾸지 않습니다. 통합 LLVM 소스 빌드는 로컬에서 CPU 매트릭스 검사 283개와 예외 계약 검사 258개를 모두 통과했으며 실패나 건너뛰기가 없습니다. 구형 Mach-O 테스트는 기록된 오픈 소스 링커와 예약된 언와인드 공간을 사용합니다. i386에는 올바르게 정렬된 시작 진입점과 64 KiB 페이지 제로 세그먼트가 있습니다.

이 결과는 명시한 에뮬레이터와 호환 계층에서의 C 코드 실행을 입증합니다. Wine은 Microsoft Windows가 아니며 Darling은 macOS가 아닙니다. iOS HLE 안의 ARMv7k 실행은 watchOS, WatchKit 또는 Apple 실기기 검증이 아닙니다. 구형 Apple SDK나 네이티브 32비트 하드웨어 지원을 추론하지 않으며 C++ 예외 언와인드 검증으로도 간주하지 않습니다. 기존 네이티브 및 호환 모드 증거는 계속 분리해 기록합니다. NeverC는 빌드하거나 변경하지 않았습니다.
