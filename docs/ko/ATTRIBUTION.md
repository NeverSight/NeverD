**언어**: [English](../../ATTRIBUTION.md) | [简体中文](../zh-CN/ATTRIBUTION.md) | [繁體中文](../zh-TW/ATTRIBUTION.md) | [日本語](../ja/ATTRIBUTION.md) | [한국어](ATTRIBUTION.md) | [Français](../fr/ATTRIBUTION.md) | [Deutsch](../de/ATTRIBUTION.md) | [Español](../es/ATTRIBUTION.md) | [Italiano](../it/ATTRIBUTION.md) | [Русский](../ru/ATTRIBUTION.md) | [العربية](../ar/ATTRIBUTION.md)

# 저작자 표시와 인용

이 페이지는 [영문 안내](../../ATTRIBUTION.md)를 번역한 것입니다. [LICENSE](../../LICENSE)의 조항이 적용됩니다.

NeverD는 **NeverD 기여자들**이 개발합니다. 소스 저장소는
[NeverSight/NeverD](https://github.com/NeverSight/NeverD)입니다.

## 코드 재사용 시 라이선스 의무

NeverD의 자체 저작물에는
[GNU AGPL 버전 3만](../../LICENSE) 적용됩니다. 라이선스가 적용되는 복제본이나
개작물을 배포할 때는 [NOTICE](../../NOTICE)의 프로젝트 고지를 포함하여,
해당하는 저작권, 라이선스 및 보증 관련 고지를 보존해야 합니다.
개별 저작자의 고지도 모두 보존해야 합니다. 라이선스가 적용되는 소스를 수정했다면,
변경 내용과 그 관련 날짜를 명시하는 눈에 띄는 고지를 포함해야 합니다.

이러한 의무는 라이선스가 적용되는 자료를 수작업으로 재사용하거나, AI 어시스턴트 또는
대규모 언어 모델(LLM)로 복제·개작하거나, LLVM IR, 컴파일, 디컴파일 또는 다른 프로그래밍
언어를 통해 변환하는 경우에도 적용됩니다. 이름, 형식, 언어 또는 도구를 바꾸는 것만으로는
의무가 사라지지 않습니다. NeverD 자료를 재사용할 때는 그 출처가 NeverD임을 명시하세요.
AI 모델이나 LLVM만 표시해서는 해당 출처를 밝힌 것이 아닙니다.

소스 배포물과 배포하는 바이너리에 대응하는 소스에 라이선스 전문과 해당 고지를 함께
포함하세요. 바이너리와 네트워크 서비스에는 AGPL 제6조와 제13조의 해당 규정도 준수해야
합니다. 인용, 링크 또는 감사 표시만으로는 AGPL의 라이선스, 변경 고지 또는 소스 제공
요건을 **대체할 수 없습니다**.

이 안내는 기존 라이선스를 설명하며, 제7조에 따른 제한이나 추가 조항을 만들지 않습니다.
공식 조항은 [LICENSE](../../LICENSE), 특히 제0조, 제2조, 제4–6조 및 제13조에 있으며,
[자유 소프트웨어 재단](https://www.gnu.org/licenses/agpl-3.0.html)에서도 확인할 수 있습니다.

## 출처를 추적할 수 있도록 기록하기

재사용한 부분마다 원본 파일 또는 심벌, 정확한 커밋 또는 릴리스, 간단한 변경 설명을
코드 옆이나 프로젝트 고지에 기록할 것을 권장합니다. 인용이 계속 같은 소스를 가리키도록
전체 커밋 해시가 포함된 GitHub 영구 링크를 사용하세요.
이러한 추가 출처 정보는 인용에 관한 권장 사항이며, 추가 라이선스 조건이 아닙니다.

예를 들어, 대괄호 안의 항목을 실제 출처 정보로 바꾸세요.

```text
This project includes material from NeverD.
Copyright (C) 2026 NeverD contributors (https://github.com/NeverSight/NeverD)
License: GNU Affero General Public License, version 3 only (AGPL-3.0-only).
Original source: https://github.com/NeverSight/NeverD/blob/[full-commit]/[path]
Changes: [description], [YYYY-MM-DD].
The applicable license and notices are included with this distribution.
```

AI를 활용하는 작업 흐름에서는 선택한 소스 컨텍스트와 함께 이 출처 정보를 보관하고,
공개하는 모든 라이선스 적용 대상 코드에도 전달하세요. 공유하기 전에 결과 코드와 그 고지를
검토하세요. 라이선스가 적용되는 NeverD 소스를 포함한 데이터셋의 경우, 해당 소스를 배포할 때
해당 고지와 라이선스 정보를 보존하세요.

## 연구, 참고 및 출력

NeverD를 사용하거나 그 구현을 참고하는 논문, 문서, 벤치마크 및 프로젝트에서는 NeverD를
인용해 주세요. [CITATION.cff](../../CITATION.cff)는 기계가 읽을 수 있는 소프트웨어 인용
메타데이터를 제공하며, 다음 일반 텍스트 인용문에 실제 사용한 버전이나 커밋을 기재하여
사용할 수도 있습니다.

```text
NeverD contributors. NeverD: Binary analysis and decompilation engine.
https://github.com/NeverSight/NeverD. Version or commit: [revision used].
```

단순히 아이디어나 알고리즘을 연구했다고 해서 독립적인 구현에 NeverD의 라이선스가 자동으로
적용되는 것은 아닙니다. 다른 사람의 프로그램에 NeverD를 실행하는 것 역시 출력에 AGPL을
자동으로 적용하지 않습니다. 제2조에 따라, 출력은 그 내용이 라이선스 적용 대상 저작물을
구성하는 경우에만 적용 대상이 됩니다. AI 출력에도 같은 구분이 적용됩니다. NeverD로
학습하거나 NeverD를 읽었다고 해서 모든 모델 출력이 자동으로 라이선스 적용 대상 저작물이
되는 것은 아닙니다. 라이선스 적용 대상 자료를 포함하지 않는 이러한 사용에는 새로운
라이선스 조건을 부과하는 대신, 학술 및 공학 관행으로서 인용을 요청합니다.

## 제3자 자료 및 이전 복제본

LLVM, Capstone, Unicorn 등의 구성 요소에는 각자의 라이선스가 계속 적용됩니다.
해당 소스의 고지, [THIRD_PARTY_NOTICES.md](../../THIRD_PARTY_NOTICES.md), 그리고
[테스트 코퍼스 라이선스](https://github.com/NeverSight/testbins/blob/9d9362d2cdfe0b4b0347bd0e12b3b8fac67d3a5b/LICENSE)를 포함한
디렉터리별 라이선스를 확인하세요. 이러한 자료를 재사용할 때는 원래의 제3자 저작자 표시를
보존하고 해당 라이선스를 준수하세요. 이 안내는 제3자 자료의 라이선스를 변경하거나
이전 복제본에 대해 이미 부여된 허가를 철회하지 않습니다.
