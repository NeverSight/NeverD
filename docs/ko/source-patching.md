**Languages**: [English](../source-patching.md) | [简体中文](../zh-CN/source-patching.md) | [繁體中文](../zh-TW/source-patching.md) | [日本語](../ja/source-patching.md) | [한국어](source-patching.md) | [Français](../fr/source-patching.md) | [Deutsch](../de/source-patching.md) | [Español](../es/source-patching.md) | [Italiano](../it/source-patching.md) | [Русский](../ru/source-patching.md) | [العربية](../ar/source-patching.md)

<!-- i18n-source: 3ceb56e2abf86f594f437a16b09a1888650cb49d4cf06860711e73458c52c68d -->

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
| Clang 22.1.x | LLVM IR | ELF x64/ARM64 C/C++; Mach-O x64/ARM64 C/C++; PE x86/x64/ARM64 C; PE x64/ARM64 C++ FH3 |
| Clang 22.1.6 | 네이티브 오브젝트 | ELF x64/ARM64 C/C++; PE x86/x64/ARM64 C |
| GCC/G++ 13.3.0 | 네이티브 오브젝트 | 호환되는 Itanium 런타임의 ELF x64/ARM64 C/C++ |
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
## 원래 참조 및 저장 공간

바인딩은 외부 선언에 대응합니다. `function`에는 확인된 원래 함수/임포트 스텁, `data`에는 쓰기 가능한 저장 공간, `const`에는 const IR 선언이 필요합니다. 오브젝트 데이터 바인딩은 0이 아닌 범위가 필요합니다. 저장 경계를 넘거나 RELRO에 쓰기 권한을 부여할 수 없습니다. 대체되는 원래 함수 본체로의 콜백은 거부됩니다. 이 프로파일에는 원래 프롤로그를 저장하는 콜백 트램펄린이 없습니다.

새 코드는 RX, 변경 가능한 상태는 RW로 유지됩니다. 기존 임포트만 재사용하며 GOT/IAT 슬롯과 호출 가능한 스텁을 구분합니다. Mach-O에는 `-Wl,-headerpad,0x400` 같은 로드 명령 여유 공간, PE에는 섹션 헤더 여유 공간이 필요합니다. 공간이 부족하면 실패합니다. ELF `.data.rel.ro`와 원래 CET/IBT 보호에는 추가 계약이 필요하여 현재는 거부합니다.

```sh
neverd patch app --from-source=replacement.c --entry=replacement --func=0x401140 \
  --bind=function:original_helper=0x401190 \
  --bind=data:shared_counter=0x404020:4 \
  --bind=const:lookup_table=0x402040:64 \
  --patch-report -o app.bound
```

Mach-O 언와인드 테이블 확장에는 인접한 파일 및 가상 공간의 여유도 필요합니다. 헤더 여유 공간만으로 확보되지 않습니다. 검증 입력은 두 요구 사항을 별도로 충족합니다.

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
    compiler="gcc", compiler_path="/path/to/gcc", artifact="object",
    bindings={"shared_counter": ("data", original_counter_address, 4)},
)
```

<!-- i18n-section: verification -->
## 빌드 및 검증

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

이전 변환 프로필의 204회 관측은 과거 기록이며 현재 C 전용 계약의 근거가 아닙니다. 현재 세 NeverC 경로는 C를 사용하고 Clang C++ FH3는 별도로 검증합니다. macOS x64는 Rosetta 2를 사용하며 Unicorn/QEMU CPU 결과는 네이티브 OS와 언와인더 검증을 대신하지 않습니다.