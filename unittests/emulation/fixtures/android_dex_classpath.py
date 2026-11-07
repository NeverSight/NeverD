#!/usr/bin/env python3
"""Generate independent DEX035 class-capacity inputs for native JNI tests."""
from pathlib import Path
import sys

sys.path.insert(0, str(Path(__file__).resolve().parents[3] / 'scripts'))
from benchmark_mobile_inventory import make_dex
from android_jni_registration_dex import registration_dex
from android_jni_native_dex import native_calls_dex
from android_jni_resume_dex import resume_dex

output = Path(sys.argv[1])
output.mkdir(parents=True, exist_ok=True)
for count, index in [(17, 0), (2048, 0), (2048, 1), (2049, 1), (4096, 0), (4097, 0)]:
    (output / f'classes-{count}-{index}.dex').write_bytes(make_dex(count, dex_index=index))

(output / 'registration.dex').write_bytes(registration_dex())
(output / 'native-calls.dex').write_bytes(native_calls_dex())

(output / 'java-resume.dex').write_bytes(resume_dex())

(output / 'java-binder.dex').write_bytes(resume_dex(binder_only=True))
(output / 'java-binder-dispatch.dex').write_bytes(resume_dex(dispatch=True))
