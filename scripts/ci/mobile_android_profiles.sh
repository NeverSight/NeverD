#!/usr/bin/env bash
# CI profile catalog. The shard runner expands these commands without executing them.
# Keep one unique --work-dir per profile. The runner supplies both JDKs and records
# the selected JDK, group, deadline and complete argv for every profile.

profile_group "Verify original synthetic method names and frozen dependency calls" 21 1200
python scripts/test_mobile_android_synthetic_method_names.py \
  --neverd build-mobile/bin/neverd --d8 "$NEVERD_D8" --java-home "$JAVA_HOME" \
  --kotlin-lib "$RUNNER_TEMP/neverd-kotlin-2.0.21/kotlinc/lib" \
  --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
  --work-dir "$RUNNER_TEMP/android-synthetic-method-names"
python scripts/test_mobile_android_synthetic_method_names.py --dependency-box \
  --neverd build-mobile/bin/neverd --d8 "$NEVERD_D8" --java-home "$JAVA_HOME" \
  --kotlin-lib "$RUNNER_TEMP/neverd-kotlin-2.0.21/kotlinc/lib" \
  --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
  --work-dir "$RUNNER_TEMP/android-synthetic-method-names-sdk"

profile_group "Verify ordinary public static JVM names" 21 900
python scripts/test_mobile_android_synthetic_method_names.py --ordinary-static \
  --neverd build-mobile/bin/neverd --d8 "$NEVERD_D8" --java-home "$JAVA_HOME" \
  --kotlin-lib "$RUNNER_TEMP/neverd-kotlin-2.0.21/kotlinc/lib" \
  --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
  --work-dir "$RUNNER_TEMP/android-synthetic-method-names-ordinary"
python scripts/test_mobile_android_synthetic_method_names.py --ordinary-static --dependency-box \
  --neverd build-mobile/bin/neverd --d8 "$NEVERD_D8" --java-home "$JAVA_HOME" \
  --kotlin-lib "$RUNNER_TEMP/neverd-kotlin-2.0.21/kotlinc/lib" \
  --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
  --work-dir "$RUNNER_TEMP/android-synthetic-method-names-ordinary-sdk"

profile_group "Recover and independently execute Android sources through C++ CLI" 21 1200
python scripts/test_mobile_android_internal.py \
  --neverd build-mobile/bin/neverd --d8 "$NEVERD_D8" \
  --work-dir "$RUNNER_TEMP/android-native" --timeout 120

profile_group "Preserve Android SDK SuppressLint through source recovery" 21 600
python scripts/test_mobile_android_suppress_lint.py \
  --neverd build-mobile/bin/neverd --d8 "$NEVERD_D8" \
  --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
  --work-dir "$RUNNER_TEMP/android-suppress-lint"

profile_group "Preserve generic metadata through source recovery" 21 600
python scripts/test_mobile_android_generic_bounds.py \
  --neverd build-mobile/bin/neverd \
  --work-dir "$RUNNER_TEMP/android-generic-bounds"
python scripts/test_mobile_android_wildcard_inheritance.py \
  --neverd build-mobile/bin/neverd --java-home "$JAVA_HOME" --d8 "$NEVERD_D8" \
  --work-dir "$RUNNER_TEMP/android-wildcard-inheritance"
python scripts/test_mobile_android_checked_erasure.py \
  --neverd build-mobile/bin/neverd \
  --work-dir "$RUNNER_TEMP/android-checked-erasure"
python scripts/test_mobile_android_return_erasure.py \
  --neverd build-mobile/bin/neverd --java-home "$JAVA_HOME" --d8 "$NEVERD_D8" \
  --work-dir "$RUNNER_TEMP/android-checked-return-erasure"
python scripts/test_mobile_android_direct_erasure.py \
  --neverd build-mobile/bin/neverd --java-home "$JAVA_HOME" --d8 "$NEVERD_D8" \
  --work-dir "$RUNNER_TEMP/android-direct-erasure"
python scripts/test_mobile_android_checked_erasure.py --interface-parameter \
  --neverd build-mobile/bin/neverd \
  --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
  --work-dir "$RUNNER_TEMP/android-checked-interface-erasure"
python scripts/test_mobile_android_checked_erasure.py --static-carrier \
  --neverd build-mobile/bin/neverd \
  --work-dir "$RUNNER_TEMP/android-static-carrier"
python scripts/test_mobile_android_checked_erasure.py --marked-carrier \
  --neverd build-mobile/bin/neverd \
  --work-dir "$RUNNER_TEMP/android-marked-carrier"
python scripts/test_mobile_android_bounded_carrier.py \
  --neverd build-mobile/bin/neverd --java-home "$JAVA_HOME" --d8 "$NEVERD_D8" \
  --work-dir "$RUNNER_TEMP/android-bounded-carrier"
python scripts/test_mobile_android_typed_adapters.py \
  --neverd build-mobile/bin/neverd \
  --work-dir "$RUNNER_TEMP/android-typed-adapters"

profile_group "Preserve Android annotation values and dependency declarations" 21 600
python scripts/test_mobile_android_annotation_values.py \
  --neverd build-mobile/bin/neverd --d8 "$NEVERD_D8" \
  --work-dir "$RUNNER_TEMP/android-values"
python scripts/test_mobile_android_annotation_values.py --initializers \
  --neverd build-mobile/bin/neverd --d8 "$NEVERD_D8" \
  --work-dir "$RUNNER_TEMP/android-annotation-initializers"
python scripts/test_mobile_android_annotation_values.py --targets \
  --neverd build-mobile/bin/neverd --d8 "$NEVERD_D8" \
  --work-dir "$RUNNER_TEMP/android-annotation-targets"
python scripts/test_mobile_android_classpath.py \
  --neverd build-mobile/bin/neverd --d8 "$NEVERD_D8" \
  --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
  --work-dir "$RUNNER_TEMP/android-classpath"

profile_group "Regenerate verified Android generic bridges and anonymous creators" 21 600
python scripts/test_mobile_android_bridges.py \
  --neverd build-mobile/bin/neverd --d8 "$NEVERD_D8" \
  --work-dir "$RUNNER_TEMP/android-bridges"
python scripts/test_mobile_android_bridges.py --anonymous \
  --neverd build-mobile/bin/neverd --d8 "$NEVERD_D8" \
  --work-dir "$RUNNER_TEMP/android-anonymous"
python scripts/test_mobile_android_bridges.py --marker \
  --neverd build-mobile/bin/neverd --d8 "$NEVERD_D8" \
  --work-dir "$RUNNER_TEMP/android-anonymous-marker"
python scripts/test_mobile_android_bridges.py --marker-callback \
  --neverd build-mobile/bin/neverd --d8 "$NEVERD_D8" \
  --work-dir "$RUNNER_TEMP/android-anonymous-marker-callback"
python scripts/test_mobile_android_bridges.py --anonymous-base \
  --neverd build-mobile/bin/neverd --d8 "$NEVERD_D8" \
  --work-dir "$RUNNER_TEMP/android-anonymous-base"
python scripts/test_mobile_android_marker_inheritance.py \
  --neverd build-mobile/bin/neverd --d8 "$NEVERD_D8" \
  --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
  --work-dir "$RUNNER_TEMP/android-marker-inheritance"
python scripts/test_mobile_android_named_scoped_unit_bridges.py \
  --neverd build-mobile/bin/neverd --d8 "$NEVERD_D8" \
  --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
  --work-dir "$RUNNER_TEMP/android-named-scoped-unit-bridges"
python scripts/test_mobile_android_named_scoped_unit_bridges.py --dependency-parents \
  --neverd build-mobile/bin/neverd --d8 "$NEVERD_D8" \
  --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
  --work-dir "$RUNNER_TEMP/android-named-scoped-unit-declarations"
python scripts/test_mobile_android_void_unit_bridges.py \
  --neverd build-mobile/bin/neverd --d8 "$NEVERD_D8" \
  --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
  --work-dir "$RUNNER_TEMP/android-void-unit-bridges"
python scripts/test_mobile_android_parameterized_bridges.py \
  --neverd build-mobile/bin/neverd --d8 "$NEVERD_D8" \
  --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
  --work-dir "$RUNNER_TEMP/android-parameterized-bridges"
python scripts/test_mobile_android_integer_unit_bridges.py --boolean \
  --neverd build-mobile/bin/neverd --d8 "$NEVERD_D8" \
  --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
  --work-dir "$RUNNER_TEMP/android-boolean-unit-bridges"
python scripts/test_mobile_android_integer_unit_bridges.py \
  --neverd build-mobile/bin/neverd --d8 "$NEVERD_D8" \
  --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
  --work-dir "$RUNNER_TEMP/android-integer-unit-bridges"
python scripts/test_mobile_android_value_carrier_bridges.py \
  --neverd build-mobile/bin/neverd --d8 "$NEVERD_D8" \
  --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
  --work-dir "$RUNNER_TEMP/android-value-carrier-bridges"
python scripts/test_mobile_android_value_carrier_bridges.py --mixed \
  --neverd build-mobile/bin/neverd --d8 "$NEVERD_D8" \
  --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
  --work-dir "$RUNNER_TEMP/android-mixed-value-carrier-bridges"
python scripts/test_mobile_android_mixed_final_carrier_parameters.py \
  --neverd build-mobile/bin/neverd --d8 "$NEVERD_D8" \
  --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
  --work-dir "$RUNNER_TEMP/android-mixed-final-carrier-parameters"
python scripts/test_mobile_android_value_carrier_bridges.py --generic-result \
  --neverd build-mobile/bin/neverd --d8 "$NEVERD_D8" \
  --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
  --work-dir "$RUNNER_TEMP/android-generic-result-value-carrier-bridges"
python scripts/test_mobile_android_value_carrier_bridges.py --boxed-integer \
  --neverd build-mobile/bin/neverd --d8 "$NEVERD_D8" \
  --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
  --work-dir "$RUNNER_TEMP/android-boxed-integer-reference-bridges"
python scripts/test_mobile_android_value_carrier_bridges.py --result-only \
  --neverd build-mobile/bin/neverd --d8 "$NEVERD_D8" \
  --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
  --work-dir "$RUNNER_TEMP/android-result-only-carrier-bridges"
python scripts/test_mobile_android_reference_only_carrier_bridges.py \
  --neverd build-mobile/bin/neverd --d8 "$NEVERD_D8" \
  --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
  --work-dir "$RUNNER_TEMP/android-reference-only-carrier-bridges"
python scripts/test_mobile_android_contained_consumer_parameters.py \
  --neverd build-mobile/bin/neverd --d8 "$NEVERD_D8" \
  --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
  --work-dir "$RUNNER_TEMP/android-contained-consumer-parameters"
python scripts/test_mobile_android_contained_consumer_parameters.py --unbounded \
  --neverd build-mobile/bin/neverd --d8 "$NEVERD_D8" \
  --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
  --work-dir "$RUNNER_TEMP/android-unbounded-consumer-parameters"
python scripts/test_mobile_android_inherited_contained_consumers.py \
  --neverd build-mobile/bin/neverd --d8 "$NEVERD_D8" \
  --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
  --work-dir "$RUNNER_TEMP/android-inherited-contained-consumers"
python scripts/test_mobile_android_inherited_contained_consumers.py --dependency-root \
  --neverd build-mobile/bin/neverd --d8 "$NEVERD_D8" \
  --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
  --work-dir "$RUNNER_TEMP/android-inherited-contained-consumer-declarations"
python scripts/test_mobile_android_object_lower_parameter.py \
  --neverd build-mobile/bin/neverd --d8 "$NEVERD_D8" \
  --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
  --work-dir "$RUNNER_TEMP/android-object-lower-parameter"
python scripts/test_mobile_android_value_carrier_bridges.py --generic-input \
  --neverd build-mobile/bin/neverd --d8 "$NEVERD_D8" \
  --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
  --work-dir "$RUNNER_TEMP/android-generic-input-carrier-bridges"
python scripts/test_mobile_android_value_carrier_bridges.py --zero-arity \
  --neverd build-mobile/bin/neverd --d8 "$NEVERD_D8" \
  --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
  --work-dir "$RUNNER_TEMP/android-zero-arity-carrier-bridges"
python scripts/test_mobile_android_value_carrier_bridges.py --standard-long \
  --neverd build-mobile/bin/neverd --d8 "$NEVERD_D8" \
  --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
  --work-dir "$RUNNER_TEMP/android-standard-long-reference-bridges"
python scripts/test_mobile_android_value_carrier_bridges.py --standard-double \
  --neverd build-mobile/bin/neverd --d8 "$NEVERD_D8" \
  --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
  --work-dir "$RUNNER_TEMP/android-standard-double-reference-bridges"
python scripts/test_mobile_android_shared_long_bridges.py \
  --neverd build-mobile/bin/neverd --d8 "$NEVERD_D8" \
  --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
  --work-dir "$RUNNER_TEMP/android-shared-long-reference-bridges"
python scripts/test_mobile_android_character_getter_carriers.py \
  --neverd build-mobile/bin/neverd --d8 "$NEVERD_D8" \
  --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
  --work-dir "$RUNNER_TEMP/android-character-getter-carriers"
python scripts/test_mobile_android_character_getter_carriers.py --dependency-root \
  --neverd build-mobile/bin/neverd --d8 "$NEVERD_D8" \
  --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
  --work-dir "$RUNNER_TEMP/android-character-getter-carriers-declarations"
python scripts/test_mobile_android_value_carrier_bridges.py --standard-float \
  --neverd build-mobile/bin/neverd --d8 "$NEVERD_D8" \
  --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
  --work-dir "$RUNNER_TEMP/android-standard-float-reference-bridges"
python scripts/test_mobile_android_named_setter_bridges.py \
  --neverd build-mobile/bin/neverd --d8 "$NEVERD_D8" \
  --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
  --work-dir "$RUNNER_TEMP/android-named-generic-setter-bridges"
python scripts/test_mobile_android_final_parameter_variance.py \
  --neverd build-mobile/bin/neverd --d8 "$NEVERD_D8" \
  --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
  --work-dir "$RUNNER_TEMP/android-final-parameter-variance"
python scripts/test_mobile_android_final_comparator_parameters.py \
  --neverd build-mobile/bin/neverd --d8 "$NEVERD_D8" \
  --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
  --work-dir "$RUNNER_TEMP/android-final-comparator-parameters"
python scripts/test_mobile_android_final_parameter_covariant_result.py \
  --neverd build-mobile/bin/neverd --d8 "$NEVERD_D8" \
  --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
  --work-dir "$RUNNER_TEMP/android-final-parameter-covariant-result"
python scripts/test_mobile_android_value_carrier_bridges.py --named-primitive \
  --neverd build-mobile/bin/neverd --d8 "$NEVERD_D8" \
  --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
  --work-dir "$RUNNER_TEMP/android-named-primitive-reference-bridges"
python scripts/test_mobile_android_value_carrier_bridges.py --primitive-array-result \
  --neverd build-mobile/bin/neverd --d8 "$NEVERD_D8" \
  --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
  --work-dir "$RUNNER_TEMP/android-primitive-array-result-bridges"
python scripts/test_mobile_android_value_carrier_bridges.py --reference-carrier \
  --neverd build-mobile/bin/neverd --d8 "$NEVERD_D8" \
  --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
  --work-dir "$RUNNER_TEMP/android-reference-carrier-bridges"
python scripts/test_mobile_android_value_carrier_bridges.py --array-unit \
  --neverd build-mobile/bin/neverd --d8 "$NEVERD_D8" \
  --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
  --work-dir "$RUNNER_TEMP/android-array-carrier-unit-bridges"
python scripts/test_mobile_android_value_carrier_bridges.py --scalar-unit \
  --neverd build-mobile/bin/neverd --d8 "$NEVERD_D8" \
  --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
  --work-dir "$RUNNER_TEMP/android-scalar-carrier-unit-bridges"
python scripts/test_mobile_android_reference_result_boxing.py \
  --neverd build-mobile/bin/neverd --d8 "$NEVERD_D8" \
  --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
  --work-dir "$RUNNER_TEMP/android-reference-result-boxing"
python scripts/test_mobile_android_source_interface_defaults.py \
  --neverd build-mobile/bin/neverd --d8 "$NEVERD_D8" \
  --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
  --work-dir "$RUNNER_TEMP/android-source-interface-defaults"
python scripts/test_mobile_android_source_interface_defaults.py --raw-parent \
  --neverd build-mobile/bin/neverd --d8 "$NEVERD_D8" \
  --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
  --work-dir "$RUNNER_TEMP/android-source-raw-interface-parent"
python scripts/test_mobile_android_upcast_result_bridges.py \
  --neverd build-mobile/bin/neverd --d8 "$NEVERD_D8" \
  --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
  --work-dir "$RUNNER_TEMP/android-upcast-result-bridges"
python scripts/test_mobile_android_upcast_result_bridges.py --generic-getter \
  --neverd build-mobile/bin/neverd --d8 "$NEVERD_D8" \
  --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
  --work-dir "$RUNNER_TEMP/android-covariant-reference-getters"
python scripts/test_mobile_android_upcast_result_bridges.py --ancestor-chain \
  --neverd build-mobile/bin/neverd --d8 "$NEVERD_D8" \
  --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
  --work-dir "$RUNNER_TEMP/android-chained-covariant-getters"
python scripts/test_mobile_android_bounded_covariant_defaults.py \
  --neverd build-mobile/bin/neverd --d8 "$NEVERD_D8" \
  --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
  --work-dir "$RUNNER_TEMP/android-bounded-covariant-defaults"
python scripts/test_mobile_android_bounded_covariant_methods.py \
  --neverd build-mobile/bin/neverd --d8 "$NEVERD_D8" \
  --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
  --work-dir "$RUNNER_TEMP/android-bounded-covariant-methods"
python scripts/test_mobile_android_bounded_covariant_methods.py --omit-interface-bridge \
  --neverd build-mobile/bin/neverd --d8 "$NEVERD_D8" \
  --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
  --work-dir "$RUNNER_TEMP/android-omitted-interface-bridge"
python scripts/test_mobile_android_nominal_interface_returns.py \
  --neverd build-mobile/bin/neverd --d8 "$NEVERD_D8" \
  --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
  --work-dir "$RUNNER_TEMP/android-nominal-interface-returns"
python scripts/test_mobile_android_substituted_interface_returns.py \
  --neverd build-mobile/bin/neverd --d8 "$NEVERD_D8" \
  --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
  --work-dir "$RUNNER_TEMP/android-substituted-interface-returns"
python scripts/test_mobile_android_source_default_returns.py \
  --neverd build-mobile/bin/neverd --d8 "$NEVERD_D8" \
  --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
  --work-dir "$RUNNER_TEMP/android-source-default-returns"
python scripts/test_mobile_android_source_default_returns.py --default-override \
  --neverd build-mobile/bin/neverd --d8 "$NEVERD_D8" \
  --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
  --work-dir "$RUNNER_TEMP/android-source-default-override"
python scripts/test_mobile_android_source_default_returns.py --disjoint-class-roots \
  --neverd build-mobile/bin/neverd --d8 "$NEVERD_D8" \
  --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
  --work-dir "$RUNNER_TEMP/android-disjoint-class-default-roots"
python scripts/test_mobile_android_source_primitive_void_bridges.py \
  --neverd build-mobile/bin/neverd --d8 "$NEVERD_D8" \
  --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
  --work-dir "$RUNNER_TEMP/android-source-primitive-void-bridges"
python scripts/test_mobile_android_indexed_covariant_returns.py \
  --neverd build-mobile/bin/neverd --d8 "$NEVERD_D8" \
  --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
  --work-dir "$RUNNER_TEMP/android-indexed-covariant-returns"
python scripts/test_mobile_android_indexed_covariant_returns.py --omit-interface-bridge \
  --neverd build-mobile/bin/neverd --d8 "$NEVERD_D8" \
  --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
  --work-dir "$RUNNER_TEMP/android-omitted-indexed-interface-bridge"
python scripts/test_mobile_android_indexed_covariant_returns.py --interface-default \
  --neverd build-mobile/bin/neverd --d8 "$NEVERD_D8" \
  --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
  --work-dir "$RUNNER_TEMP/android-indexed-covariant-defaults"
python scripts/test_mobile_android_indexed_covariant_returns.py --inherited-default \
  --neverd build-mobile/bin/neverd --d8 "$NEVERD_D8" \
  --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
  --work-dir "$RUNNER_TEMP/android-inherited-indexed-defaults"
python scripts/test_mobile_android_indexed_covariant_returns.py --reference-input \
  --neverd build-mobile/bin/neverd --d8 "$NEVERD_D8" \
  --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
  --work-dir "$RUNNER_TEMP/android-reference-covariant-returns"
python scripts/test_mobile_android_indexed_covariant_returns.py --reference-input --omit-interface-bridge \
  --neverd build-mobile/bin/neverd --d8 "$NEVERD_D8" \
  --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
  --work-dir "$RUNNER_TEMP/android-omitted-reference-interface-bridge"
python scripts/test_mobile_android_indexed_covariant_returns.py --getter-chain \
  --neverd build-mobile/bin/neverd --d8 "$NEVERD_D8" \
  --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
  --work-dir "$RUNNER_TEMP/android-covariant-getter-chains"
python scripts/test_mobile_android_indexed_covariant_returns.py --deferred-class --reference-input \
  --neverd build-mobile/bin/neverd --d8 "$NEVERD_D8" \
  --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
  --work-dir "$RUNNER_TEMP/android-deferred-covariant-interfaces"
python scripts/test_mobile_android_indexed_covariant_returns.py --deferred-class --reference-input --omit-interface-bridge \
  --neverd build-mobile/bin/neverd --d8 "$NEVERD_D8" \
  --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
  --work-dir "$RUNNER_TEMP/android-deferred-omitted-covariant-entry"
python scripts/test_mobile_android_indexed_covariant_returns.py --inherited-entry \
  --neverd build-mobile/bin/neverd --d8 "$NEVERD_D8" \
  --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
  --work-dir "$RUNNER_TEMP/android-inherited-covariant-entries"
python scripts/test_mobile_android_indexed_covariant_returns.py --inherited-bridge \
  --neverd build-mobile/bin/neverd --d8 "$NEVERD_D8" \
  --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
  --work-dir "$RUNNER_TEMP/android-inherited-erased-covariant-entries"
python scripts/test_mobile_android_indexed_covariant_returns.py --class-bridge-override \
  --neverd build-mobile/bin/neverd --d8 "$NEVERD_D8" \
  --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
  --work-dir "$RUNNER_TEMP/android-class-covariant-bridge-overrides"
python scripts/test_mobile_android_private_producer_return_slots.py \
  --neverd build-mobile/bin/neverd --d8 "$NEVERD_D8" \
  --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
  --work-dir "$RUNNER_TEMP/android-private-producer-return-slots"
python scripts/test_mobile_android_annotated_bridge_targets.py \
  --neverd build-mobile/bin/neverd --d8 "$NEVERD_D8" \
  --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
  --work-dir "$RUNNER_TEMP/android-annotated-bridge-targets"
for provider in source dependency; do
  for flags in bridge synthetic; do
    character_args=()
    if [ "$provider" = dependency ]; then character_args+=(--dependency-provider); fi
    if [ "$flags" = synthetic ]; then character_args+=(--synthetic); fi
    python scripts/test_mobile_android_indexed_character_bridges.py "${character_args[@]}" \
      --neverd build-mobile/bin/neverd --d8 "$NEVERD_D8" \
      --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
      --work-dir "$RUNNER_TEMP/android-indexed-character-$provider-$flags"
  done
done
for provider in source dependency; do
  for flags in bridge synthetic; do
    default_args=()
    if [ "$provider" = dependency ]; then default_args+=(--dependency-provider); fi
    if [ "$flags" = synthetic ]; then default_args+=(--synthetic); fi
    python scripts/test_mobile_android_interface_super_getters.py "${default_args[@]}" \
      --neverd build-mobile/bin/neverd --d8 "$NEVERD_D8" \
      --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
      --work-dir "$RUNNER_TEMP/android-interface-super-getter-$provider-$flags"
  done
done
python scripts/test_mobile_android_plain_boolean_forwarding.py \
  --neverd build-mobile/bin/neverd --d8 "$NEVERD_D8" \
  --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
  --work-dir "$RUNNER_TEMP/android-plain-boolean-forwarding"
python scripts/test_mobile_android_value_carrier_bridges.py --reference-carrier --parameterized-reference \
  --neverd build-mobile/bin/neverd --d8 "$NEVERD_D8" \
  --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
  --work-dir "$RUNNER_TEMP/android-closed-reference-carrier"
python scripts/test_mobile_android_value_carrier_bridges.py --array-unit --parameterized-reference \
  --neverd build-mobile/bin/neverd --d8 "$NEVERD_D8" \
  --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
  --work-dir "$RUNNER_TEMP/android-closed-array-unit-carrier"
python scripts/test_mobile_android_value_carrier_bridges.py --reference-carrier --parameterized-reference --open-reference \
  --neverd build-mobile/bin/neverd --d8 "$NEVERD_D8" \
  --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
  --work-dir "$RUNNER_TEMP/android-scoped-reference-carrier"
python scripts/test_mobile_android_value_carrier_bridges.py --array-unit --parameterized-reference --open-reference \
  --neverd build-mobile/bin/neverd --d8 "$NEVERD_D8" \
  --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
  --work-dir "$RUNNER_TEMP/android-scoped-array-unit-carrier"
python scripts/test_mobile_android_numeric_unit_bridges.py --profile double \
  --neverd build-mobile/bin/neverd --d8 "$NEVERD_D8" \
  --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
  --work-dir "$RUNNER_TEMP/android-double-unit-carriers"
python scripts/test_mobile_android_numeric_unit_bridges.py --profile long \
  --neverd build-mobile/bin/neverd --d8 "$NEVERD_D8" \
  --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
  --work-dir "$RUNNER_TEMP/android-long-unit-carriers"
python scripts/test_mobile_android_numeric_unit_bridges.py --profile float \
  --neverd build-mobile/bin/neverd --d8 "$NEVERD_D8" \
  --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
  --work-dir "$RUNNER_TEMP/android-float-unit-carriers"
python scripts/test_mobile_android_inherited_integer_carriers.py --ranged \
  --neverd build-mobile/bin/neverd --d8 "$NEVERD_D8" \
  --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
  --work-dir "$RUNNER_TEMP/android-inherited-ranged-integer-carriers"
python scripts/test_mobile_android_inherited_integer_carriers.py \
  --neverd build-mobile/bin/neverd --d8 "$NEVERD_D8" \
  --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
  --work-dir "$RUNNER_TEMP/android-inherited-integer-carriers"
python scripts/test_mobile_android_boxed_primitive_getters.py \
  --neverd build-mobile/bin/neverd --d8 "$NEVERD_D8" \
  --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
  --work-dir "$RUNNER_TEMP/android-boxed-primitive-getters"
python scripts/test_mobile_android_nominal_element_getters.py \
  --neverd build-mobile/bin/neverd --d8 "$NEVERD_D8" \
  --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
  --work-dir "$RUNNER_TEMP/android-nominal-element-getters"
python scripts/test_mobile_android_original_raw_results.py \
  --neverd build-mobile/bin/neverd --d8 "$NEVERD_D8" \
  --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
  --work-dir "$RUNNER_TEMP/android-original-raw-results"
python scripts/test_mobile_android_original_raw_parameters.py \
  --neverd build-mobile/bin/neverd --d8 "$NEVERD_D8" \
  --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
  --work-dir "$RUNNER_TEMP/android-original-raw-parameters"
python scripts/test_mobile_android_original_raw_parameters.py --dependency-interface \
  --neverd build-mobile/bin/neverd --d8 "$NEVERD_D8" \
  --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
  --work-dir "$RUNNER_TEMP/android-original-raw-parameter-declarations"
python scripts/test_mobile_android_original_raw_parameters.py --scoped-nominal-parameters \
  --neverd build-mobile/bin/neverd --d8 "$NEVERD_D8" \
  --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
  --work-dir "$RUNNER_TEMP/android-scoped-raw-parameters"
python scripts/test_mobile_android_original_raw_parameters.py --scoped-nominal-parameters --dependency-interface \
  --neverd build-mobile/bin/neverd --d8 "$NEVERD_D8" \
  --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
  --work-dir "$RUNNER_TEMP/android-scoped-raw-parameter-declarations"
python scripts/test_mobile_android_original_raw_parameters.py --dependency-interface --dependency-method-formals \
  --neverd build-mobile/bin/neverd --d8 "$NEVERD_D8" \
  --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
  --work-dir "$RUNNER_TEMP/android-dependency-method-formals"
python scripts/test_mobile_android_original_raw_parameters.py --dependency-interface --dependency-method-formals --raw-dependency-defaults \
  --neverd build-mobile/bin/neverd --d8 "$NEVERD_D8" \
  --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
  --work-dir "$RUNNER_TEMP/android-raw-dependency-defaults"
python scripts/test_mobile_android_raw_owned_interface.py \
  --neverd build-mobile/bin/neverd --d8 "$NEVERD_D8" \
  --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
  --work-dir "$RUNNER_TEMP/android-raw-owned-interface"
python scripts/test_mobile_android_method_owned_parameter_bridges.py \
  --neverd build-mobile/bin/neverd --d8 "$NEVERD_D8" \
  --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
  --work-dir "$RUNNER_TEMP/android-method-owned-parameter-bridges"
python scripts/test_mobile_android_method_owned_parameter_bridges.py --dependency-interface \
  --neverd build-mobile/bin/neverd --d8 "$NEVERD_D8" \
  --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
  --work-dir "$RUNNER_TEMP/android-method-owned-parameter-bridge-declarations"
python scripts/test_mobile_android_scoped_wildcard_defaults.py \
  --neverd build-mobile/bin/neverd --d8 "$NEVERD_D8" \
  --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
  --work-dir "$RUNNER_TEMP/android-scoped-wildcard-defaults"
python scripts/test_mobile_android_scoped_wildcard_defaults.py --dependency-root \
  --neverd build-mobile/bin/neverd --d8 "$NEVERD_D8" \
  --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
  --work-dir "$RUNNER_TEMP/android-scoped-wildcard-default-declarations"
python scripts/test_mobile_android_scoped_default_parameters.py \
  --neverd build-mobile/bin/neverd --d8 "$NEVERD_D8" \
  --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
  --work-dir "$RUNNER_TEMP/android-scoped-default-parameters"
python scripts/test_mobile_android_scoped_default_parameters.py --dependency-root \
  --neverd build-mobile/bin/neverd --d8 "$NEVERD_D8" \
  --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
  --work-dir "$RUNNER_TEMP/android-scoped-default-parameter-declarations"
python scripts/test_mobile_android_specialized_interface_covariance.py \
  --neverd build-mobile/bin/neverd --d8 "$NEVERD_D8" \
  --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
  --work-dir "$RUNNER_TEMP/android-specialized-interface-covariance"
python scripts/test_mobile_android_specialized_interface_covariance.py --dependency-interface \
  --neverd build-mobile/bin/neverd --d8 "$NEVERD_D8" \
  --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
  --work-dir "$RUNNER_TEMP/android-specialized-interface-covariance-declarations"
python scripts/test_mobile_android_raw_self_interface_default.py \
  --neverd build-mobile/bin/neverd --d8 "$NEVERD_D8" \
  --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
  --work-dir "$RUNNER_TEMP/android-raw-self-interface-default"
python scripts/test_mobile_android_raw_self_interface_default.py --inherited-consumer \
  --neverd build-mobile/bin/neverd --d8 "$NEVERD_D8" \
  --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
  --work-dir "$RUNNER_TEMP/android-inherited-raw-self-default"
python scripts/test_mobile_android_erased_super_constructor.py \
  --neverd build-mobile/bin/neverd --d8 "$NEVERD_D8" --java-home "$JAVA_HOME" \
  --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
  --work-dir "$RUNNER_TEMP/android-erased-super-constructor"
python scripts/test_mobile_android_erased_super_constructor.py --dependency-provider \
  --neverd build-mobile/bin/neverd --d8 "$NEVERD_D8" --java-home "$JAVA_HOME" \
  --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
  --work-dir "$RUNNER_TEMP/android-erased-super-constructor-sdk"
python scripts/test_mobile_android_checked_static_super.py \
  --neverd build-mobile/bin/neverd --d8 "$NEVERD_D8" --java-home "$JAVA_HOME" \
  --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
  --work-dir "$RUNNER_TEMP/android-checked-static-super"
python scripts/test_mobile_android_checked_static_super.py --dependency-parent \
  --neverd build-mobile/bin/neverd --d8 "$NEVERD_D8" --java-home "$JAVA_HOME" \
  --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
  --work-dir "$RUNNER_TEMP/android-checked-static-super-sdk"
python scripts/test_mobile_android_static_super_accessors.py \
  --neverd build-mobile/bin/neverd --d8 "$NEVERD_D8" --java-home "$JAVA_HOME" \
  --kotlin-lib "$RUNNER_TEMP/neverd-kotlin-2.0.21/kotlinc/lib" \
  --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
  --work-dir "$RUNNER_TEMP/android-static-super-accessors"
python scripts/test_mobile_android_static_super_accessors.py --dependency-parent \
  --neverd build-mobile/bin/neverd --d8 "$NEVERD_D8" --java-home "$JAVA_HOME" \
  --kotlin-lib "$RUNNER_TEMP/neverd-kotlin-2.0.21/kotlinc/lib" \
  --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
  --work-dir "$RUNNER_TEMP/android-static-super-accessors-sdk"
python scripts/test_mobile_android_intersection_void_invocations.py \
  --neverd build-mobile/bin/neverd --d8 "$NEVERD_D8" --java-home "$JAVA_HOME" \
  --kotlin-lib "$RUNNER_TEMP/neverd-kotlin-2.0.21/kotlinc/lib" \
  --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
  --work-dir "$RUNNER_TEMP/android-intersection-void-invocations"
python scripts/test_mobile_android_intersection_void_invocations.py --dependency-provider \
  --neverd build-mobile/bin/neverd --d8 "$NEVERD_D8" --java-home "$JAVA_HOME" \
  --kotlin-lib "$RUNNER_TEMP/neverd-kotlin-2.0.21/kotlinc/lib" \
  --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
  --work-dir "$RUNNER_TEMP/android-intersection-void-invocations-sdk"
python scripts/test_mobile_android_erased_boolean_invocations.py \
  --neverd build-mobile/bin/neverd --d8 "$NEVERD_D8" --java-home "$JAVA_HOME" \
  --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
  --work-dir "$RUNNER_TEMP/android-erased-boolean-invocations"
python scripts/test_mobile_android_erased_boolean_invocations.py --dependency-provider \
  --neverd build-mobile/bin/neverd --d8 "$NEVERD_D8" --java-home "$JAVA_HOME" \
  --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
  --work-dir "$RUNNER_TEMP/android-erased-boolean-invocations-sdk"
python scripts/test_mobile_android_erased_pair_invocations.py \
  --neverd build-mobile/bin/neverd --d8 "$NEVERD_D8" --java-home "$JAVA_HOME" \
  --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
  --work-dir "$RUNNER_TEMP/android-erased-pair-invocations"
python scripts/test_mobile_android_erased_pair_invocations.py --dependency-provider \
  --neverd build-mobile/bin/neverd --d8 "$NEVERD_D8" --java-home "$JAVA_HOME" \
  --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
  --work-dir "$RUNNER_TEMP/android-erased-pair-invocations-sdk"
python scripts/test_mobile_android_erased_factory_invocations.py \
  --neverd build-mobile/bin/neverd --d8 "$NEVERD_D8" --java-home "$JAVA_HOME" \
  --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
  --work-dir "$RUNNER_TEMP/android-erased-factory-invocations"
python scripts/test_mobile_android_erased_factory_invocations.py --dependency-provider \
  --neverd build-mobile/bin/neverd --d8 "$NEVERD_D8" --java-home "$JAVA_HOME" \
  --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
  --work-dir "$RUNNER_TEMP/android-erased-factory-invocations-sdk"
python scripts/test_mobile_android_allocation_field_before_nested.py \
  --neverd build-mobile/bin/neverd --d8 "$NEVERD_D8" --java-home "$JAVA_HOME" \
  --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
  --work-dir "$RUNNER_TEMP/android-allocation-field-before-nested"
python scripts/test_mobile_android_allocation_field_before_nested.py --dependency-provider \
  --neverd build-mobile/bin/neverd --d8 "$NEVERD_D8" --java-home "$JAVA_HOME" \
  --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
  --work-dir "$RUNNER_TEMP/android-allocation-field-before-nested-sdk"
python scripts/test_mobile_android_allocation_field_prefix.py \
  --neverd build-mobile/bin/neverd --d8 "$NEVERD_D8" --java-home "$JAVA_HOME" \
  --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
  --work-dir "$RUNNER_TEMP/android-allocation-field-prefix"
python scripts/test_mobile_android_allocation_field_prefix.py --dependency-provider \
  --neverd build-mobile/bin/neverd --d8 "$NEVERD_D8" --java-home "$JAVA_HOME" \
  --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
  --work-dir "$RUNNER_TEMP/android-allocation-field-prefix-sdk"
python scripts/test_mobile_android_conditional_allocation.py \
  --neverd build-mobile/bin/neverd --d8 "$NEVERD_D8" --java-home "$JAVA_HOME" \
  --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
  --work-dir "$RUNNER_TEMP/android-conditional-allocation"
python scripts/test_mobile_android_conditional_allocation.py --dependency-provider \
  --neverd build-mobile/bin/neverd --d8 "$NEVERD_D8" --java-home "$JAVA_HOME" \
  --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
  --work-dir "$RUNNER_TEMP/android-conditional-allocation-sdk"
python scripts/test_mobile_android_delegation_postludes.py \
  --neverd build-mobile/bin/neverd --d8 "$NEVERD_D8" --java-home "$JAVA_HOME" \
  --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
  --work-dir "$RUNNER_TEMP/android-delegation-postludes"
python scripts/test_mobile_android_delegation_postludes.py --dependency-trace \
  --neverd build-mobile/bin/neverd --d8 "$NEVERD_D8" --java-home "$JAVA_HOME" \
  --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
  --work-dir "$RUNNER_TEMP/android-delegation-postludes-sdk"
python scripts/test_mobile_android_final_throw_regions.py \
  --neverd build-mobile/bin/neverd --d8 "$NEVERD_D8" --java-home "$JAVA_HOME" \
  --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
  --work-dir "$RUNNER_TEMP/android-final-throw-regions"
python scripts/test_mobile_android_final_throw_regions.py --dependency-trace \
  --neverd build-mobile/bin/neverd --d8 "$NEVERD_D8" --java-home "$JAVA_HOME" \
  --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
  --work-dir "$RUNNER_TEMP/android-final-throw-regions-sdk"
python scripts/test_mobile_android_null_constructor_arguments.py \
  --neverd build-mobile/bin/neverd --d8 "$NEVERD_D8" --java-home "$JAVA_HOME" \
  --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
  --work-dir "$RUNNER_TEMP/android-null-constructor-arguments"
python scripts/test_mobile_android_null_constructor_arguments.py --dependency-provider \
  --neverd build-mobile/bin/neverd --d8 "$NEVERD_D8" --java-home "$JAVA_HOME" \
  --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
  --work-dir "$RUNNER_TEMP/android-null-constructor-arguments-sdk"
python scripts/test_mobile_android_encoded_final_defaults.py \
  --neverd build-mobile/bin/neverd --d8 "$NEVERD_D8" --java-home "$JAVA_HOME" \
  --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
  --work-dir "$RUNNER_TEMP/android-encoded-final-defaults"
python scripts/test_mobile_android_encoded_final_defaults.py --dependency-trace \
  --neverd build-mobile/bin/neverd --d8 "$NEVERD_D8" --java-home "$JAVA_HOME" \
  --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
  --work-dir "$RUNNER_TEMP/android-encoded-final-defaults-sdk"
python scripts/test_mobile_android_unwritten_static_finals.py \
  --neverd build-mobile/bin/neverd --d8 "$NEVERD_D8" --java-home "$JAVA_HOME" \
  --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
  --work-dir "$RUNNER_TEMP/android-unwritten-static-finals"
python scripts/test_mobile_android_unwritten_static_finals.py --dependency-trace \
  --neverd build-mobile/bin/neverd --d8 "$NEVERD_D8" --java-home "$JAVA_HOME" \
  --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
  --work-dir "$RUNNER_TEMP/android-unwritten-static-finals-sdk"
python scripts/test_mobile_android_mixed_carrier_invocations.py \
  --neverd build-mobile/bin/neverd --d8 "$NEVERD_D8" --java-home "$JAVA_HOME" \
  --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
  --work-dir "$RUNNER_TEMP/android-mixed-carrier-invocations"
python scripts/test_mobile_android_mixed_carrier_invocations.py --dependency-provider \
  --neverd build-mobile/bin/neverd --d8 "$NEVERD_D8" --java-home "$JAVA_HOME" \
  --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
  --work-dir "$RUNNER_TEMP/android-mixed-carrier-invocations-sdk"
python scripts/test_mobile_android_mapped_carrier_invocations.py \
  --neverd build-mobile/bin/neverd --d8 "$NEVERD_D8" --java-home "$JAVA_HOME" \
  --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
  --work-dir "$RUNNER_TEMP/android-mapped-carrier-invocations"
python scripts/test_mobile_android_mapped_carrier_invocations.py --dependency-provider \
  --neverd build-mobile/bin/neverd --d8 "$NEVERD_D8" --java-home "$JAVA_HOME" \
  --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
  --work-dir "$RUNNER_TEMP/android-mapped-carrier-invocations-sdk"
python scripts/test_mobile_android_seeded_carrier_invocations.py \
  --neverd build-mobile/bin/neverd --d8 "$NEVERD_D8" --java-home "$JAVA_HOME" \
  --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
  --work-dir "$RUNNER_TEMP/android-seeded-carrier-invocations"
python scripts/test_mobile_android_seeded_carrier_invocations.py --dependency-provider \
  --neverd build-mobile/bin/neverd --d8 "$NEVERD_D8" --java-home "$JAVA_HOME" \
  --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
  --work-dir "$RUNNER_TEMP/android-seeded-carrier-invocations-sdk"
python scripts/test_mobile_android_nested_carrier_invocations.py \
  --neverd build-mobile/bin/neverd --d8 "$NEVERD_D8" --java-home "$JAVA_HOME" \
  --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
  --work-dir "$RUNNER_TEMP/android-nested-carrier-invocations"
python scripts/test_mobile_android_nested_carrier_invocations.py --dependency-provider \
  --neverd build-mobile/bin/neverd --d8 "$NEVERD_D8" --java-home "$JAVA_HOME" \
  --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
  --work-dir "$RUNNER_TEMP/android-nested-carrier-invocations-sdk"
python scripts/test_mobile_android_raw_interface_parameter_bridges.py \
  --neverd build-mobile/bin/neverd --d8 "$NEVERD_D8" --java-home "$JAVA_HOME" \
  --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
  --work-dir "$RUNNER_TEMP/android-raw-interface-parameter-bridges"
python scripts/test_mobile_android_raw_interface_parameter_bridges.py --dependency-provider \
  --neverd build-mobile/bin/neverd --d8 "$NEVERD_D8" --java-home "$JAVA_HOME" \
  --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
  --work-dir "$RUNNER_TEMP/android-raw-interface-parameter-bridges-sdk"
python scripts/test_mobile_android_protected_allocation.py \
  --neverd build-mobile/bin/neverd --d8 "$NEVERD_D8" --java-home "$JAVA_HOME" \
  --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
  --work-dir "$RUNNER_TEMP/android-protected-allocation"
python scripts/test_mobile_android_protected_allocation.py --dependency-provider \
  --neverd build-mobile/bin/neverd --d8 "$NEVERD_D8" --java-home "$JAVA_HOME" \
  --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
  --work-dir "$RUNNER_TEMP/android-protected-allocation-sdk"
python scripts/test_mobile_android_protected_allocation.py --handler-effects \
  --neverd build-mobile/bin/neverd --d8 "$NEVERD_D8" --java-home "$JAVA_HOME" \
  --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
  --work-dir "$RUNNER_TEMP/android-protected-allocation-handler-effects"
python scripts/test_mobile_android_protected_allocation.py --handler-effects --dependency-provider \
  --neverd build-mobile/bin/neverd --d8 "$NEVERD_D8" --java-home "$JAVA_HOME" \
  --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
  --work-dir "$RUNNER_TEMP/android-protected-allocation-handler-effects-sdk"
python scripts/test_mobile_android_allocation_statements.py \
  --neverd build-mobile/bin/neverd --d8 "$NEVERD_D8" \
  --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
  --work-dir "$RUNNER_TEMP/android-allocation-statements"
python scripts/test_mobile_android_allocation_statements.py --dependency-provider \
  --neverd build-mobile/bin/neverd --d8 "$NEVERD_D8" \
  --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
  --work-dir "$RUNNER_TEMP/android-allocation-statements-sdk"
python scripts/test_mobile_android_monitor_regions.py \
  --neverd build-mobile/bin/neverd --d8 "$NEVERD_D8" \
  --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
  --work-dir "$RUNNER_TEMP/android-monitor-regions"
python scripts/test_mobile_android_monitor_regions.py --dependency-provider \
  --neverd build-mobile/bin/neverd --d8 "$NEVERD_D8" \
  --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
  --work-dir "$RUNNER_TEMP/android-monitor-regions-sdk"
python scripts/test_mobile_android_monitor_regions.py --shared-return \
  --neverd build-mobile/bin/neverd --d8 "$NEVERD_D8" --java-home "$JAVA_HOME" \
  --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
  --work-dir "$RUNNER_TEMP/android-monitor-regions-shared-return"
python scripts/test_mobile_android_monitor_regions.py --shared-return --dependency-provider \
  --neverd build-mobile/bin/neverd --d8 "$NEVERD_D8" --java-home "$JAVA_HOME" \
  --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
  --work-dir "$RUNNER_TEMP/android-monitor-regions-shared-return-sdk"
python scripts/test_mobile_android_monitor_sparse_cleanup.py \
  --neverd build-mobile/bin/neverd --d8 "$NEVERD_D8" --java-home "$JAVA_HOME" \
  --kotlin-lib "$RUNNER_TEMP/neverd-kotlin-2.0.21/kotlinc/lib" \
  --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
  --work-dir "$RUNNER_TEMP/android-monitor-sparse-cleanup"
python scripts/test_mobile_android_monitor_sparse_cleanup.py --dependency-provider \
  --neverd build-mobile/bin/neverd --d8 "$NEVERD_D8" --java-home "$JAVA_HOME" \
  --kotlin-lib "$RUNNER_TEMP/neverd-kotlin-2.0.21/kotlinc/lib" \
  --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
  --work-dir "$RUNNER_TEMP/android-monitor-sparse-cleanup-sdk"
python scripts/test_mobile_android_monitor_sparse_cleanup.py --post-release \
  --neverd build-mobile/bin/neverd --d8 "$NEVERD_D8" --java-home "$JAVA_HOME" \
  --kotlin-lib "$RUNNER_TEMP/neverd-kotlin-2.0.21/kotlinc/lib" \
  --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
  --work-dir "$RUNNER_TEMP/android-monitor-post-release"
python scripts/test_mobile_android_monitor_sparse_cleanup.py --post-release --dependency-provider \
  --neverd build-mobile/bin/neverd --d8 "$NEVERD_D8" --java-home "$JAVA_HOME" \
  --kotlin-lib "$RUNNER_TEMP/neverd-kotlin-2.0.21/kotlinc/lib" \
  --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
  --work-dir "$RUNNER_TEMP/android-monitor-post-release-sdk"
python scripts/test_mobile_android_monitor_sparse_cleanup.py --multiple-release \
  --neverd build-mobile/bin/neverd --d8 "$NEVERD_D8" --java-home "$JAVA_HOME" \
  --kotlin-lib "$RUNNER_TEMP/neverd-kotlin-2.0.21/kotlinc/lib" \
  --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
  --work-dir "$RUNNER_TEMP/android-monitor-multiple-release"
python scripts/test_mobile_android_monitor_sparse_cleanup.py --multiple-release --dependency-provider \
  --neverd build-mobile/bin/neverd --d8 "$NEVERD_D8" --java-home "$JAVA_HOME" \
  --kotlin-lib "$RUNNER_TEMP/neverd-kotlin-2.0.21/kotlinc/lib" \
  --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
  --work-dir "$RUNNER_TEMP/android-monitor-multiple-release-sdk"
python scripts/test_mobile_android_erased_callback_invocations.py \
  --neverd build-mobile/bin/neverd --d8 "$NEVERD_D8" \
  --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
  --work-dir "$RUNNER_TEMP/android-erased-callback-invocations"
python scripts/test_mobile_android_erased_callback_invocations.py --dependency-provider \
  --neverd build-mobile/bin/neverd --d8 "$NEVERD_D8" \
  --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
  --work-dir "$RUNNER_TEMP/android-erased-callback-invocations-sdk"
python scripts/test_mobile_android_interface_initializers.py \
  --neverd build-mobile/bin/neverd --d8 "$NEVERD_D8" \
  --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
  --work-dir "$RUNNER_TEMP/android-interface-initializers"
python scripts/test_mobile_android_interface_initializers.py --dependency-provider \
  --neverd build-mobile/bin/neverd --d8 "$NEVERD_D8" \
  --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
  --work-dir "$RUNNER_TEMP/android-interface-initializers-sdk"
python scripts/test_mobile_android_nested_allocations.py \
  --neverd build-mobile/bin/neverd --d8 "$NEVERD_D8" \
  --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
  --work-dir "$RUNNER_TEMP/android-nested-allocations"
python scripts/test_mobile_android_nested_allocations.py --dependency-provider \
  --neverd build-mobile/bin/neverd --d8 "$NEVERD_D8" \
  --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
  --work-dir "$RUNNER_TEMP/android-nested-allocations-sdk"
python scripts/test_mobile_android_allocation_static_calls.py \
  --neverd build-mobile/bin/neverd --d8 "$NEVERD_D8" \
  --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
  --work-dir "$RUNNER_TEMP/android-allocation-static-calls"
python scripts/test_mobile_android_allocation_static_calls.py --dependency-provider \
  --neverd build-mobile/bin/neverd --d8 "$NEVERD_D8" \
  --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
  --work-dir "$RUNNER_TEMP/android-allocation-static-calls-sdk"
python scripts/test_mobile_android_allocation_static_calls.py --generic-provider \
  --neverd build-mobile/bin/neverd --d8 "$NEVERD_D8" \
  --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
  --work-dir "$RUNNER_TEMP/android-allocation-static-calls-generic"
python scripts/test_mobile_android_allocation_static_calls.py --generic-provider --dependency-provider \
  --neverd build-mobile/bin/neverd --d8 "$NEVERD_D8" \
  --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
  --work-dir "$RUNNER_TEMP/android-allocation-static-calls-generic-sdk"
python scripts/test_mobile_android_erased_result_invocations.py \
  --neverd build-mobile/bin/neverd --d8 "$NEVERD_D8" \
  --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
  --work-dir "$RUNNER_TEMP/android-erased-result-invocations"
python scripts/test_mobile_android_erased_result_invocations.py --dependency-provider \
  --neverd build-mobile/bin/neverd --d8 "$NEVERD_D8" \
  --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
  --work-dir "$RUNNER_TEMP/android-erased-result-invocations-sdk"
python scripts/test_mobile_android_nominal_result_invocations.py \
  --neverd build-mobile/bin/neverd --d8 "$NEVERD_D8" \
  --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
  --work-dir "$RUNNER_TEMP/android-nominal-result-invocations"
python scripts/test_mobile_android_nominal_result_invocations.py --dependency-provider \
  --neverd build-mobile/bin/neverd --d8 "$NEVERD_D8" \
  --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
  --work-dir "$RUNNER_TEMP/android-nominal-result-invocations-sdk"
python scripts/test_mobile_android_object_root_covariance.py \
  --neverd build-mobile/bin/neverd --d8 "$NEVERD_D8" \
  --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
  --work-dir "$RUNNER_TEMP/android-object-root-covariance"
python scripts/test_mobile_android_object_root_covariance.py --omit-original-exceptions \
  --neverd build-mobile/bin/neverd --d8 "$NEVERD_D8" \
  --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
  --work-dir "$RUNNER_TEMP/android-object-root-covariance-empty-exceptions"
python scripts/test_mobile_android_nearer_generic_class_covariance.py \
  --neverd build-mobile/bin/neverd --d8 "$NEVERD_D8" \
  --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
  --work-dir "$RUNNER_TEMP/android-nearer-generic-class-covariance"
python scripts/test_mobile_android_original_raw_parameters.py --dependency-interface --interface-fields \
  --neverd build-mobile/bin/neverd --d8 "$NEVERD_D8" \
  --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
  --work-dir "$RUNNER_TEMP/android-plain-dependency-interface-fields"
python scripts/test_mobile_android_original_raw_parameters.py --dependency-interface --dependency-parent-chain \
  --neverd build-mobile/bin/neverd --d8 "$NEVERD_D8" \
  --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
  --work-dir "$RUNNER_TEMP/android-fixed-dependency-class-prefix"
python scripts/test_mobile_android_original_raw_parameters.py --dependency-interface --dependency-parent-chain --abstract-fixed-prefix \
  --neverd build-mobile/bin/neverd --d8 "$NEVERD_D8" \
  --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
  --work-dir "$RUNNER_TEMP/android-abstract-fixed-dependency-prefix"
python scripts/test_mobile_android_original_raw_parameters.py --dependency-interface --abstract-dependency-parent \
  --neverd build-mobile/bin/neverd --d8 "$NEVERD_D8" \
  --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
  --work-dir "$RUNNER_TEMP/android-abstract-dependency-parent"
python scripts/test_mobile_android_plain_sdk_return_slots.py --interface-covariance \
  --neverd build-mobile/bin/neverd --d8 "$NEVERD_D8" \
  --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
  --work-dir "$RUNNER_TEMP/android-sdk-parent-interface-covariance"
python scripts/test_mobile_android_plain_sdk_return_slots.py \
  --neverd build-mobile/bin/neverd --d8 "$NEVERD_D8" \
  --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
  --work-dir "$RUNNER_TEMP/android-plain-sdk-return-slots"
python scripts/test_mobile_android_original_raw_parameters.py --dependency-interface --abstract-dependency-parent --deferred-abstract-child \
  --neverd build-mobile/bin/neverd --d8 "$NEVERD_D8" \
  --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
  --work-dir "$RUNNER_TEMP/android-plain-deferred-abstract"
python scripts/test_mobile_android_original_raw_parameters.py --dependency-interface --abstract-dependency-parent --plain-class-covariance \
  --neverd build-mobile/bin/neverd --d8 "$NEVERD_D8" \
  --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
  --work-dir "$RUNNER_TEMP/android-plain-class-covariance"
python scripts/test_mobile_android_original_raw_parameters.py --dependency-interface --abstract-dependency-parent --plain-class-covariance --covariant-siblings \
  --neverd build-mobile/bin/neverd --d8 "$NEVERD_D8" \
  --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
  --work-dir "$RUNNER_TEMP/android-covariant-class-siblings"
python scripts/test_mobile_android_abstract_class_covariance.py \
  --neverd build-mobile/bin/neverd --d8 "$NEVERD_D8" \
  --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
  --work-dir "$RUNNER_TEMP/android-abstract-class-covariance"
python scripts/test_mobile_android_abstract_class_covariance.py --dependency-root \
  --neverd build-mobile/bin/neverd --d8 "$NEVERD_D8" \
  --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
  --work-dir "$RUNNER_TEMP/android-abstract-class-covariance-sdk"
python scripts/test_mobile_android_inherited_final_covariance.py \
  --neverd build-mobile/bin/neverd --d8 "$NEVERD_D8" \
  --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
  --work-dir "$RUNNER_TEMP/android-inherited-final-covariance"
python scripts/test_mobile_android_inherited_final_covariance.py --dependency-provider \
  --neverd build-mobile/bin/neverd --d8 "$NEVERD_D8" \
  --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
  --work-dir "$RUNNER_TEMP/android-inherited-final-covariance-sdk"
python scripts/test_mobile_android_inherited_checked_results.py \
  --neverd build-mobile/bin/neverd --d8 "$NEVERD_D8" \
  --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
  --work-dir "$RUNNER_TEMP/android-inherited-checked-results"
python scripts/test_mobile_android_inherited_checked_results.py --dependency-provider \
  --neverd build-mobile/bin/neverd --d8 "$NEVERD_D8" \
  --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
  --work-dir "$RUNNER_TEMP/android-inherited-checked-results-sdk"
python scripts/test_mobile_android_inherited_self_carrier.py \
  --neverd build-mobile/bin/neverd --d8 "$NEVERD_D8" \
  --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
  --work-dir "$RUNNER_TEMP/android-inherited-self-carrier"
python scripts/test_mobile_android_inherited_self_carrier.py --dependency-root \
  --neverd build-mobile/bin/neverd --d8 "$NEVERD_D8" \
  --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
  --work-dir "$RUNNER_TEMP/android-inherited-self-carrier-sdk"
python scripts/test_mobile_android_fixed_parameterized_methods.py \
  --neverd build-mobile/bin/neverd --d8 "$NEVERD_D8" \
  --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
  --work-dir "$RUNNER_TEMP/android-fixed-parameterized-methods"
python scripts/test_mobile_android_nongeneric_nominal_bridges.py --source-default \
  --neverd build-mobile/bin/neverd --d8 "$NEVERD_D8" \
  --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
  --work-dir "$RUNNER_TEMP/android-plain-source-default-covariance"
python scripts/test_mobile_android_nongeneric_nominal_bridges.py \
  --neverd build-mobile/bin/neverd --d8 "$NEVERD_D8" \
  --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
  --work-dir "$RUNNER_TEMP/android-nongeneric-nominal-bridges"
python scripts/test_mobile_android_method_formal_interface_fields.py \
  --neverd build-mobile/bin/neverd --d8 "$NEVERD_D8" \
  --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
  --work-dir "$RUNNER_TEMP/android-method-formal-interface-fields"
python scripts/test_mobile_android_method_formal_interface_fields.py --bridge-implementation \
  --neverd build-mobile/bin/neverd --d8 "$NEVERD_D8" \
  --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
  --work-dir "$RUNNER_TEMP/android-method-formal-implementation-bridge"
python scripts/test_mobile_android_method_formal_interface_fields.py --bridge-implementation --fixed-implementations \
  --neverd build-mobile/bin/neverd --d8 "$NEVERD_D8" \
  --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
  --work-dir "$RUNNER_TEMP/android-complete-contract-implementation-bridges"
python scripts/test_mobile_android_nonreturning_integer_alias.py \
  --neverd build-mobile/bin/neverd --d8 "$NEVERD_D8" \
  --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
  --work-dir "$RUNNER_TEMP/android-nonreturning-integer-alias"
python scripts/test_mobile_android_nonreturning_boolean_results.py \
  --neverd build-mobile/bin/neverd --d8 "$NEVERD_D8" \
  --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
  --work-dir "$RUNNER_TEMP/android-nonreturning-boolean-results"
python scripts/test_mobile_android_short_comparison_carrier.py \
  --neverd build-mobile/bin/neverd --d8 "$NEVERD_D8" \
  --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
  --work-dir "$RUNNER_TEMP/android-short-comparison-carrier"
python scripts/test_mobile_android_short_comparison_carrier.py --float \
  --neverd build-mobile/bin/neverd --d8 "$NEVERD_D8" \
  --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
  --work-dir "$RUNNER_TEMP/android-float-comparison-carrier"
python scripts/test_mobile_android_short_comparison_carrier.py --long \
  --neverd build-mobile/bin/neverd --d8 "$NEVERD_D8" \
  --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
  --work-dir "$RUNNER_TEMP/android-long-comparison-carrier"
python scripts/test_mobile_android_final_getter_return_slots.py \
  --neverd build-mobile/bin/neverd --d8 "$NEVERD_D8" \
  --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
  --work-dir "$RUNNER_TEMP/android-final-getter-return-slots"
python scripts/test_mobile_android_superclass_getter_results.py \
  --neverd build-mobile/bin/neverd --d8 "$NEVERD_D8" \
  --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
  --work-dir "$RUNNER_TEMP/android-superclass-getter-results"
python scripts/test_mobile_android_superclass_getter_results.py --integer-getter \
  --neverd build-mobile/bin/neverd --d8 "$NEVERD_D8" \
  --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
  --work-dir "$RUNNER_TEMP/android-superclass-integer-getters"
python scripts/test_mobile_android_abstract_integer_getter_bridge.py \
  --neverd build-mobile/bin/neverd --d8 "$NEVERD_D8" \
  --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
  --work-dir "$RUNNER_TEMP/android-abstract-integer-getter-bridge"
python scripts/test_mobile_android_abstract_integer_getter_bridge.py --dependency-interface \
  --neverd build-mobile/bin/neverd --d8 "$NEVERD_D8" \
  --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
  --work-dir "$RUNNER_TEMP/android-external-abstract-integer-getter-bridge"
python scripts/test_mobile_android_abstract_integer_getter_bridge.py --class-ancestor \
  --neverd build-mobile/bin/neverd --d8 "$NEVERD_D8" \
  --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
  --work-dir "$RUNNER_TEMP/android-integer-source-class-ancestor"
python scripts/test_mobile_android_abstract_integer_getter_bridge.py --dependency-ancestor --dependency-interface \
  --neverd build-mobile/bin/neverd --d8 "$NEVERD_D8" \
  --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
  --work-dir "$RUNNER_TEMP/android-integer-dependency-class-ancestor"
python scripts/test_mobile_android_open_inheritance.py \
  --neverd build-mobile/bin/neverd --d8 "$NEVERD_D8" \
  --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
  --work-dir "$RUNNER_TEMP/android-open-inheritance"
python scripts/test_mobile_android_parameterized_unit_bridges.py \
  --neverd build-mobile/bin/neverd --d8 "$NEVERD_D8" \
  --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
  --work-dir "$RUNNER_TEMP/android-parameterized-unit-bridges"
python scripts/test_mobile_android_nongeneric_ancestors.py \
  --neverd build-mobile/bin/neverd --d8 "$NEVERD_D8" \
  --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
  --work-dir "$RUNNER_TEMP/android-nongeneric-ancestors"
python scripts/test_mobile_android_nongeneric_ancestors.py --generic-interface \
  --neverd build-mobile/bin/neverd --d8 "$NEVERD_D8" \
  --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
  --work-dir "$RUNNER_TEMP/android-mixed-generic-ancestors"
python scripts/test_mobile_android_nongeneric_ancestors.py --abstract-parent \
  --neverd build-mobile/bin/neverd --d8 "$NEVERD_D8" \
  --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
  --work-dir "$RUNNER_TEMP/android-abstract-nongeneric-ancestors"
python scripts/test_mobile_android_nongeneric_ancestors.py --abstract-parent --generic-interface \
  --neverd build-mobile/bin/neverd --d8 "$NEVERD_D8" \
  --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
  --work-dir "$RUNNER_TEMP/android-abstract-mixed-generic-ancestors"
python scripts/test_mobile_android_cross_package_ancestors.py \
  --neverd build-mobile/bin/neverd --d8 "$NEVERD_D8" \
  --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
  --work-dir "$RUNNER_TEMP/android-cross-package-ancestors"
python scripts/test_mobile_android_cross_package_ancestors.py --dependency-parent \
  --neverd build-mobile/bin/neverd --d8 "$NEVERD_D8" \
  --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
  --work-dir "$RUNNER_TEMP/android-cross-package-ancestor-declarations"
python scripts/test_mobile_android_source_class_bridge_chain.py \
  --neverd build-mobile/bin/neverd --d8 "$NEVERD_D8" \
  --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
  --work-dir "$RUNNER_TEMP/android-source-class-bridge-chain"
python scripts/test_mobile_android_source_class_bridge_chain.py --dependency-root \
  --neverd build-mobile/bin/neverd --d8 "$NEVERD_D8" \
  --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
  --work-dir "$RUNNER_TEMP/android-source-class-bridge-declarations"
python scripts/test_mobile_android_source_class_bridge_chain.py --inherited-members \
  --neverd build-mobile/bin/neverd --d8 "$NEVERD_D8" \
  --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
  --work-dir "$RUNNER_TEMP/android-inherited-typed-provider"
python scripts/test_mobile_android_source_class_bridge_chain.py --inherited-members --dependency-root \
  --neverd build-mobile/bin/neverd --d8 "$NEVERD_D8" \
  --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
  --work-dir "$RUNNER_TEMP/android-inherited-typed-provider-declarations"
python scripts/test_mobile_android_boolean_result_bridges.py \
  --neverd build-mobile/bin/neverd --d8 "$NEVERD_D8" \
  --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
  --work-dir "$RUNNER_TEMP/android-boolean-result-bridges"
python scripts/test_mobile_android_boolean_result_bridges.py --dependency-interface \
  --neverd build-mobile/bin/neverd --d8 "$NEVERD_D8" \
  --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
  --work-dir "$RUNNER_TEMP/android-boolean-result-bridge-declarations"
python scripts/test_mobile_android_abstract_plain_chain.py \
  --neverd build-mobile/bin/neverd --d8 "$NEVERD_D8" \
  --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
  --work-dir "$RUNNER_TEMP/android-abstract-plain-chain-implicit"
python scripts/test_mobile_android_abstract_plain_chain.py --dependency-interface \
  --neverd build-mobile/bin/neverd --d8 "$NEVERD_D8" \
  --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
  --work-dir "$RUNNER_TEMP/android-abstract-plain-chain-implicit-declarations"
python scripts/test_mobile_android_abstract_plain_chain.py --redeclare \
  --neverd build-mobile/bin/neverd --d8 "$NEVERD_D8" \
  --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
  --work-dir "$RUNNER_TEMP/android-abstract-plain-chain-redeclared"
python scripts/test_mobile_android_abstract_plain_chain.py --redeclare --dependency-interface \
  --neverd build-mobile/bin/neverd --d8 "$NEVERD_D8" \
  --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
  --work-dir "$RUNNER_TEMP/android-abstract-plain-chain-redeclared-declarations"
python scripts/test_mobile_android_deferred_plain_callbacks.py \
  --neverd build-mobile/bin/neverd --d8 "$NEVERD_D8" \
  --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
  --work-dir "$RUNNER_TEMP/android-deferred-plain-callbacks"
python scripts/test_mobile_android_deferred_plain_callbacks.py --dependency-interface \
  --neverd build-mobile/bin/neverd --d8 "$NEVERD_D8" \
  --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
  --work-dir "$RUNNER_TEMP/android-deferred-plain-callback-declarations"
python scripts/test_mobile_android_fixed_throws_empty_overrides.py \
  --neverd build-mobile/bin/neverd --d8 "$NEVERD_D8" \
  --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
  --work-dir "$RUNNER_TEMP/android-fixed-throws-empty-overrides"
python scripts/test_mobile_android_fixed_throws_empty_overrides.py --dependency-interface \
  --neverd build-mobile/bin/neverd --d8 "$NEVERD_D8" \
  --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
  --work-dir "$RUNNER_TEMP/android-fixed-throws-empty-override-declarations"
python scripts/test_mobile_android_inherited_generic_empty_throws.py \
  --neverd build-mobile/bin/neverd --d8 "$NEVERD_D8" \
  --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
  --work-dir "$RUNNER_TEMP/android-inherited-generic-empty-throws"
python scripts/test_mobile_android_inherited_generic_empty_throws.py --dependency-interface \
  --neverd build-mobile/bin/neverd --d8 "$NEVERD_D8" \
  --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
  --work-dir "$RUNNER_TEMP/android-inherited-generic-empty-throw-declarations"
python scripts/test_mobile_android_inherited_generic_empty_throws.py --parameterized-return \
  --neverd build-mobile/bin/neverd --d8 "$NEVERD_D8" \
  --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
  --work-dir "$RUNNER_TEMP/android-parameterized-raw-returns"
python scripts/test_mobile_android_inherited_generic_empty_throws.py --dependency-interface --parameterized-return \
  --neverd build-mobile/bin/neverd --d8 "$NEVERD_D8" \
  --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
  --work-dir "$RUNNER_TEMP/android-parameterized-raw-return-declarations"
python scripts/test_mobile_android_inherited_generic_empty_throws.py --parameterized-throws \
  --neverd build-mobile/bin/neverd --d8 "$NEVERD_D8" \
  --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
  --work-dir "$RUNNER_TEMP/android-parameterized-fixed-throws"
python scripts/test_mobile_android_inherited_generic_empty_throws.py --dependency-interface --parameterized-throws \
  --neverd build-mobile/bin/neverd --d8 "$NEVERD_D8" \
  --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
  --work-dir "$RUNNER_TEMP/android-parameterized-fixed-throw-declarations"
python scripts/test_mobile_android_scoped_unchecked_implementations.py \
  --neverd build-mobile/bin/neverd --d8 "$NEVERD_D8" \
  --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
  --work-dir "$RUNNER_TEMP/android-scoped-unchecked-implementations"
python scripts/test_mobile_android_scoped_unchecked_implementations.py --dependency-root \
  --neverd build-mobile/bin/neverd --d8 "$NEVERD_D8" \
  --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
  --work-dir "$RUNNER_TEMP/android-scoped-unchecked-implementation-declarations"
python scripts/test_mobile_android_erased_terminal_consumers.py \
  --neverd build-mobile/bin/neverd --d8 "$NEVERD_D8" \
  --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
  --work-dir "$RUNNER_TEMP/android-erased-terminal-consumers"
python scripts/test_mobile_android_erased_terminal_consumers.py --dependency-root \
  --neverd build-mobile/bin/neverd --d8 "$NEVERD_D8" \
  --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
  --work-dir "$RUNNER_TEMP/android-erased-terminal-consumer-declarations"
python scripts/test_mobile_android_guarded_scalar_parameters.py \
  --neverd build-mobile/bin/neverd --d8 "$NEVERD_D8" \
  --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
  --work-dir "$RUNNER_TEMP/android-guarded-scalar-parameters"
python scripts/test_mobile_android_guarded_scalar_parameters.py --dependency-root \
  --neverd build-mobile/bin/neverd --d8 "$NEVERD_D8" \
  --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
  --work-dir "$RUNNER_TEMP/android-guarded-scalar-parameter-declarations"
python scripts/test_mobile_android_named_scalar_getters.py \
  --neverd build-mobile/bin/neverd --d8 "$NEVERD_D8" \
  --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
  --work-dir "$RUNNER_TEMP/android-named-scalar-getters"
python scripts/test_mobile_android_named_scalar_getters.py --dependency-root \
  --neverd build-mobile/bin/neverd --d8 "$NEVERD_D8" \
  --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
  --work-dir "$RUNNER_TEMP/android-named-scalar-getter-declarations"
python scripts/test_mobile_android_nominal_boxed_results.py \
  --neverd build-mobile/bin/neverd --d8 "$NEVERD_D8" \
  --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
  --work-dir "$RUNNER_TEMP/android-nominal-boxed-results"
python scripts/test_mobile_android_nominal_boxed_results.py --dependency-parent \
  --neverd build-mobile/bin/neverd --d8 "$NEVERD_D8" \
  --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
  --work-dir "$RUNNER_TEMP/android-nominal-boxed-results-parent"
python scripts/test_mobile_android_boxed_generic_interface_results.py \
  --neverd build-mobile/bin/neverd --d8 "$NEVERD_D8" \
  --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
  --work-dir "$RUNNER_TEMP/android-boxed-generic-interface-results"
python scripts/test_mobile_android_boxed_generic_interface_results.py --dependency-root \
  --neverd build-mobile/bin/neverd --d8 "$NEVERD_D8" \
  --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
  --work-dir "$RUNNER_TEMP/android-boxed-generic-interface-result-declarations"
python scripts/test_mobile_android_boxed_generic_interface_results.py --abstract-owner \
  --neverd build-mobile/bin/neverd --d8 "$NEVERD_D8" \
  --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
  --work-dir "$RUNNER_TEMP/android-abstract-boxed-generic-results"
python scripts/test_mobile_android_boxed_generic_interface_results.py --abstract-owner --dependency-root \
  --neverd build-mobile/bin/neverd --d8 "$NEVERD_D8" \
  --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
  --work-dir "$RUNNER_TEMP/android-abstract-boxed-generic-result-declarations"
python scripts/test_mobile_android_boxed_generic_interface_results.py --abstract-owner --deferred-plain-slot \
  --neverd build-mobile/bin/neverd --d8 "$NEVERD_D8" \
  --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
  --work-dir "$RUNNER_TEMP/android-abstract-boxed-deferred-slots"
python scripts/test_mobile_android_boxed_generic_interface_results.py --abstract-owner --deferred-plain-slot --dependency-root \
  --neverd build-mobile/bin/neverd --d8 "$NEVERD_D8" \
  --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
  --work-dir "$RUNNER_TEMP/android-abstract-boxed-deferred-slot-declarations"
python scripts/test_mobile_android_final_void_return_slots.py \
  --neverd build-mobile/bin/neverd --d8 "$NEVERD_D8" \
  --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
  --work-dir "$RUNNER_TEMP/android-final-void-return-slots"
python scripts/test_mobile_android_final_void_return_slots.py --dependency-parent \
  --neverd build-mobile/bin/neverd --d8 "$NEVERD_D8" \
  --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
  --work-dir "$RUNNER_TEMP/android-final-void-return-slot-declarations"
python scripts/test_mobile_android_boxed_superclass_results.py \
  --neverd build-mobile/bin/neverd --d8 "$NEVERD_D8" \
  --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
  --work-dir "$RUNNER_TEMP/android-boxed-superclass-results"
python scripts/test_mobile_android_boxed_superclass_results.py --dependency-parent \
  --neverd build-mobile/bin/neverd --d8 "$NEVERD_D8" \
  --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
  --work-dir "$RUNNER_TEMP/android-boxed-superclass-result-declarations"
python scripts/test_mobile_android_inherited_wildcard_parameters.py \
  --neverd build-mobile/bin/neverd --d8 "$NEVERD_D8" \
  --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
  --work-dir "$RUNNER_TEMP/android-inherited-wildcard-parameters"
python scripts/test_mobile_android_inherited_wildcard_parameters.py --dependency-parent \
  --neverd build-mobile/bin/neverd --d8 "$NEVERD_D8" \
  --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
  --work-dir "$RUNNER_TEMP/android-inherited-wildcard-parameter-declarations"
python scripts/test_mobile_android_raw_method_formal_members.py \
  --neverd build-mobile/bin/neverd --d8 "$NEVERD_D8" \
  --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
  --work-dir "$RUNNER_TEMP/android-raw-method-formal-members"
python scripts/test_mobile_android_raw_method_formal_members.py --dependency-root \
  --neverd build-mobile/bin/neverd --d8 "$NEVERD_D8" \
  --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
  --work-dir "$RUNNER_TEMP/android-raw-method-formal-member-declarations"
python scripts/test_mobile_android_raw_constant_parameters.py \
  --neverd build-mobile/bin/neverd --d8 "$NEVERD_D8" \
  --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
  --work-dir "$RUNNER_TEMP/android-raw-constant-parameters"
python scripts/test_mobile_android_raw_constant_parameters.py --dependency-root \
  --neverd build-mobile/bin/neverd --d8 "$NEVERD_D8" \
  --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
  --work-dir "$RUNNER_TEMP/android-raw-constant-parameter-declarations"
python scripts/test_mobile_android_checked_inlined_interfaces.py \
  --neverd build-mobile/bin/neverd --d8 "$NEVERD_D8" \
  --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
  --work-dir "$RUNNER_TEMP/android-checked-inlined-interfaces"
python scripts/test_mobile_android_checked_inlined_interfaces.py --dependency-root \
  --neverd build-mobile/bin/neverd --d8 "$NEVERD_D8" \
  --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
  --work-dir "$RUNNER_TEMP/android-checked-inlined-interface-declarations"
python scripts/test_mobile_android_checked_inlined_interfaces.py --recursive-bound \
  --neverd build-mobile/bin/neverd --d8 "$NEVERD_D8" \
  --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
  --work-dir "$RUNNER_TEMP/android-checked-inlined-recursive-bounds"
python scripts/test_mobile_android_checked_inlined_interfaces.py --recursive-bound --dependency-root \
  --neverd build-mobile/bin/neverd --d8 "$NEVERD_D8" \
  --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
  --work-dir "$RUNNER_TEMP/android-checked-inlined-recursive-bound-declarations"
python scripts/test_mobile_android_package_generic_covariance.py \
  --neverd build-mobile/bin/neverd --d8 "$NEVERD_D8" \
  --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
  --work-dir "$RUNNER_TEMP/android-package-generic-covariance"
python scripts/test_mobile_android_package_generic_covariance.py --dependency-root \
  --neverd build-mobile/bin/neverd --d8 "$NEVERD_D8" \
  --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
  --work-dir "$RUNNER_TEMP/android-package-generic-covariance-declarations"
python scripts/test_mobile_android_public_superclass_access_bridges.py \
  --neverd build-mobile/bin/neverd --d8 "$NEVERD_D8" \
  --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
  --work-dir "$RUNNER_TEMP/android-public-superclass-access-bridges"
python scripts/test_mobile_android_public_superclass_access_bridges.py --dependency-root \
  --neverd build-mobile/bin/neverd --d8 "$NEVERD_D8" \
  --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
  --work-dir "$RUNNER_TEMP/android-public-superclass-access-declarations"
python scripts/test_mobile_android_scoped_raw_interface_parameters.py \
  --neverd build-mobile/bin/neverd --d8 "$NEVERD_D8" \
  --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
  --work-dir "$RUNNER_TEMP/android-scoped-raw-interface-parameters"
python scripts/test_mobile_android_scoped_raw_interface_parameters.py --dependency-interfaces \
  --neverd build-mobile/bin/neverd --d8 "$NEVERD_D8" \
  --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
  --work-dir "$RUNNER_TEMP/android-scoped-raw-interface-declarations"
python scripts/test_mobile_android_scoped_raw_class_parameters.py \
  --neverd build-mobile/bin/neverd --d8 "$NEVERD_D8" \
  --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
  --work-dir "$RUNNER_TEMP/android-scoped-raw-class-parameters"
python scripts/test_mobile_android_scoped_raw_class_parameters.py --dependency-root \
  --neverd build-mobile/bin/neverd --d8 "$NEVERD_D8" \
  --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
  --work-dir "$RUNNER_TEMP/android-scoped-raw-class-parameter-declarations"
python scripts/test_mobile_android_scoped_plain_ancestors.py \
  --neverd build-mobile/bin/neverd --d8 "$NEVERD_D8" \
  --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
  --work-dir "$RUNNER_TEMP/android-scoped-plain-ancestors"
python scripts/test_mobile_android_scoped_plain_ancestors.py --dependency-root \
  --neverd build-mobile/bin/neverd --d8 "$NEVERD_D8" \
  --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
  --work-dir "$RUNNER_TEMP/android-scoped-plain-ancestor-declarations"
python scripts/test_mobile_android_primitive_bridge_copies.py \
  --neverd build-mobile/bin/neverd --d8 "$NEVERD_D8" \
  --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
  --work-dir "$RUNNER_TEMP/android-primitive-bridge-copies"
python scripts/test_mobile_android_primitive_bridge_copies.py --dependency-root \
  --neverd build-mobile/bin/neverd --d8 "$NEVERD_D8" \
  --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
  --work-dir "$RUNNER_TEMP/android-primitive-bridge-copy-declarations"
python scripts/test_mobile_android_parameter_return_covariance.py \
  --neverd build-mobile/bin/neverd --d8 "$NEVERD_D8" \
  --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
  --work-dir "$RUNNER_TEMP/android-parameter-return-covariance"
python scripts/test_mobile_android_parameter_return_covariance.py --dependency-root \
  --neverd build-mobile/bin/neverd --d8 "$NEVERD_D8" \
  --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
  --work-dir "$RUNNER_TEMP/android-parameter-return-covariance-declarations"
python scripts/test_mobile_android_parameter_return_covariance.py --deprecated-target \
  --neverd build-mobile/bin/neverd --d8 "$NEVERD_D8" \
  --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
  --work-dir "$RUNNER_TEMP/android-deprecated-parameter-return-covariance"
python scripts/test_mobile_android_parameter_return_covariance.py --deprecated-target --dependency-root \
  --neverd build-mobile/bin/neverd --d8 "$NEVERD_D8" \
  --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
  --work-dir "$RUNNER_TEMP/android-deprecated-parameter-return-covariance-declarations"
python scripts/test_mobile_android_fixed_exception_defaults.py \
  --neverd build-mobile/bin/neverd --d8 "$NEVERD_D8" \
  --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
  --work-dir "$RUNNER_TEMP/android-fixed-exception-defaults"
python scripts/test_mobile_android_fixed_exception_defaults.py --dependency-root \
  --neverd build-mobile/bin/neverd --d8 "$NEVERD_D8" \
  --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
  --work-dir "$RUNNER_TEMP/android-fixed-exception-default-declarations"
python scripts/test_mobile_android_integer_static_comparisons.py --copied-receiver --dependency-helper \
  --neverd build-mobile/bin/neverd --d8 "$NEVERD_D8" \
  --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
  --work-dir "$RUNNER_TEMP/android-copied-static-receiver-helper"
python scripts/test_mobile_android_integer_static_comparisons.py --copied-receiver --reference-result --inherited-predicates \
  --neverd build-mobile/bin/neverd --d8 "$NEVERD_D8" \
  --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
  --work-dir "$RUNNER_TEMP/android-copied-static-receiver-combined"
python scripts/test_mobile_android_integer_static_comparisons.py --copied-receiver --reference-result --inherited-predicates --dependency-parent \
  --neverd build-mobile/bin/neverd --d8 "$NEVERD_D8" \
  --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
  --work-dir "$RUNNER_TEMP/android-copied-static-receiver-combined-parent"
python scripts/test_mobile_android_integer_static_comparisons.py --omit-interface-bridge \
  --neverd build-mobile/bin/neverd --d8 "$NEVERD_D8" \
  --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
  --work-dir "$RUNNER_TEMP/android-absent-parameter-interface"
python scripts/test_mobile_android_integer_static_comparisons.py --omit-interface-bridge --dependency-helper \
  --neverd build-mobile/bin/neverd --d8 "$NEVERD_D8" \
  --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
  --work-dir "$RUNNER_TEMP/android-absent-parameter-interface-helper"
python scripts/test_mobile_android_integer_static_comparisons.py --inherited-predicates \
  --neverd build-mobile/bin/neverd --d8 "$NEVERD_D8" \
  --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
  --work-dir "$RUNNER_TEMP/android-inherited-static-predicates"
python scripts/test_mobile_android_integer_static_comparisons.py --inherited-predicates --dependency-parent \
  --neverd build-mobile/bin/neverd --d8 "$NEVERD_D8" \
  --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
  --work-dir "$RUNNER_TEMP/android-inherited-static-predicates-parent"
python scripts/test_mobile_android_integer_static_comparisons.py --reference-result \
  --neverd build-mobile/bin/neverd --d8 "$NEVERD_D8" \
  --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
  --work-dir "$RUNNER_TEMP/android-fixed-reference-static"
python scripts/test_mobile_android_integer_static_comparisons.py --reference-result --dependency-parent \
  --neverd build-mobile/bin/neverd --d8 "$NEVERD_D8" \
  --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
  --work-dir "$RUNNER_TEMP/android-fixed-reference-static-parent"
python scripts/test_mobile_android_integer_static_comparisons.py \
  --neverd build-mobile/bin/neverd --d8 "$NEVERD_D8" \
  --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
  --work-dir "$RUNNER_TEMP/android-integer-static-comparisons"
python scripts/test_mobile_android_integer_static_comparisons.py --dependency-helper \
  --neverd build-mobile/bin/neverd --d8 "$NEVERD_D8" \
  --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
  --work-dir "$RUNNER_TEMP/android-integer-static-comparisons-helper"
python scripts/test_mobile_android_inherited_contract_implementations.py \
  --neverd build-mobile/bin/neverd --d8 "$NEVERD_D8" \
  --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
  --work-dir "$RUNNER_TEMP/android-inherited-contract-implementations"
python scripts/test_mobile_android_inherited_contract_implementations.py --synthetic \
  --neverd build-mobile/bin/neverd --d8 "$NEVERD_D8" \
  --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
  --work-dir "$RUNNER_TEMP/android-synthetic-inherited-contract-implementations"
python scripts/test_mobile_android_inherited_contract_implementations.py --dependency-root \
  --neverd build-mobile/bin/neverd --d8 "$NEVERD_D8" \
  --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
  --work-dir "$RUNNER_TEMP/android-inherited-contract-implementation-declarations"
python scripts/test_mobile_android_inherited_contract_implementations.py --synthetic --dependency-root \
  --neverd build-mobile/bin/neverd --d8 "$NEVERD_D8" \
  --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
  --work-dir "$RUNNER_TEMP/android-synthetic-inherited-contract-implementation-declarations"
python scripts/test_mobile_android_class_formal_implementation_bridges.py \
  --neverd build-mobile/bin/neverd --d8 "$NEVERD_D8" \
  --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
  --work-dir "$RUNNER_TEMP/android-class-formal-implementation-bridges"
python scripts/test_mobile_android_class_formal_implementation_bridges.py --dependency-root \
  --neverd build-mobile/bin/neverd --d8 "$NEVERD_D8" \
  --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
  --work-dir "$RUNNER_TEMP/android-class-formal-implementation-bridge-declarations"
python scripts/test_mobile_android_comparable_carrier_predicates.py \
  --neverd build-mobile/bin/neverd --d8 "$NEVERD_D8" \
  --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
  --work-dir "$RUNNER_TEMP/android-comparable-carrier-predicates"
python scripts/test_mobile_android_comparable_carrier_predicates.py --dependency-root \
  --neverd build-mobile/bin/neverd --d8 "$NEVERD_D8" \
  --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
  --work-dir "$RUNNER_TEMP/android-comparable-carrier-predicate-declarations"
python scripts/test_mobile_android_comparable_carrier_predicates.py --narrow \
  --neverd build-mobile/bin/neverd --d8 "$NEVERD_D8" \
  --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
  --work-dir "$RUNNER_TEMP/android-comparable-narrow-carrier-predicates"
python scripts/test_mobile_android_comparable_carrier_predicates.py --narrow --dependency-root \
  --neverd build-mobile/bin/neverd --d8 "$NEVERD_D8" \
  --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
  --work-dir "$RUNNER_TEMP/android-comparable-narrow-carrier-predicate-declarations"
python scripts/test_mobile_android_comparable_carrier_predicates.py --inlined-carrier \
  --neverd build-mobile/bin/neverd --d8 "$NEVERD_D8" \
  --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
  --work-dir "$RUNNER_TEMP/android-projected-carrier-predicates"
python scripts/test_mobile_android_comparable_carrier_predicates.py --inlined-carrier --dependency-root \
  --neverd build-mobile/bin/neverd --d8 "$NEVERD_D8" \
  --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
  --work-dir "$RUNNER_TEMP/android-projected-carrier-predicate-declarations"
python scripts/test_mobile_android_comparable_carrier_predicates.py --inlined-carrier --narrow \
  --neverd build-mobile/bin/neverd --d8 "$NEVERD_D8" \
  --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
  --work-dir "$RUNNER_TEMP/android-projected-narrow-carrier-predicates"
python scripts/test_mobile_android_comparable_carrier_predicates.py --inlined-carrier --narrow --dependency-root \
  --neverd build-mobile/bin/neverd --d8 "$NEVERD_D8" \
  --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
  --work-dir "$RUNNER_TEMP/android-projected-narrow-carrier-predicate-declarations"
python scripts/test_mobile_android_comparable_scalar_predicates.py \
  --neverd build-mobile/bin/neverd --d8 "$NEVERD_D8" \
  --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
  --work-dir "$RUNNER_TEMP/android-comparable-scalar-predicates"
python scripts/test_mobile_android_comparable_scalar_predicates.py --dependency-root \
  --neverd build-mobile/bin/neverd --d8 "$NEVERD_D8" \
  --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
  --work-dir "$RUNNER_TEMP/android-comparable-scalar-predicate-declarations"
python scripts/test_mobile_android_comparable_scalar_predicates.py --numeric \
  --neverd build-mobile/bin/neverd --d8 "$NEVERD_D8" \
  --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
  --work-dir "$RUNNER_TEMP/android-comparable-numeric-predicates"
python scripts/test_mobile_android_comparable_scalar_predicates.py --numeric --dependency-root \
  --neverd build-mobile/bin/neverd --d8 "$NEVERD_D8" \
  --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
  --work-dir "$RUNNER_TEMP/android-comparable-numeric-predicate-declarations"
python scripts/test_mobile_android_raw_generic_ancestor_returns.py \
  --neverd build-mobile/bin/neverd --d8 "$NEVERD_D8" \
  --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
  --work-dir "$RUNNER_TEMP/android-raw-generic-ancestor-returns"
python scripts/test_mobile_android_raw_generic_ancestor_returns.py --dependency-root \
  --neverd build-mobile/bin/neverd --d8 "$NEVERD_D8" \
  --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
  --work-dir "$RUNNER_TEMP/android-raw-generic-ancestor-return-declarations"
python scripts/test_mobile_android_raw_generic_ancestor_returns.py --covariant-target \
  --neverd build-mobile/bin/neverd --d8 "$NEVERD_D8" \
  --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
  --work-dir "$RUNNER_TEMP/android-raw-generic-covariant-returns"
python scripts/test_mobile_android_raw_generic_ancestor_returns.py --covariant-target --dependency-root \
  --neverd build-mobile/bin/neverd --d8 "$NEVERD_D8" \
  --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
  --work-dir "$RUNNER_TEMP/android-raw-generic-covariant-return-declarations"
python scripts/test_mobile_android_protected_scalar_array_targets.py \
  --neverd build-mobile/bin/neverd --d8 "$NEVERD_D8" \
  --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
  --work-dir "$RUNNER_TEMP/android-protected-scalar-array-targets"
python scripts/test_mobile_android_protected_scalar_array_targets.py --dependency-parent \
  --neverd build-mobile/bin/neverd --d8 "$NEVERD_D8" \
  --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
  --work-dir "$RUNNER_TEMP/android-protected-scalar-array-target-declarations"
python scripts/test_mobile_android_protected_void_consumers.py \
  --neverd build-mobile/bin/neverd --d8 "$NEVERD_D8" \
  --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
  --work-dir "$RUNNER_TEMP/android-protected-void-consumers"
python scripts/test_mobile_android_protected_void_consumers.py --dependency-parent \
  --neverd build-mobile/bin/neverd --d8 "$NEVERD_D8" \
  --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
  --work-dir "$RUNNER_TEMP/android-protected-void-consumer-declarations"
python scripts/test_mobile_android_empty_original_bridge_throws.py \
  --neverd build-mobile/bin/neverd --d8 "$NEVERD_D8" \
  --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
  --work-dir "$RUNNER_TEMP/android-empty-original-bridge-throws"
python scripts/test_mobile_android_empty_original_bridge_throws.py --dependency-parent \
  --neverd build-mobile/bin/neverd --d8 "$NEVERD_D8" \
  --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
  --work-dir "$RUNNER_TEMP/android-empty-original-bridge-throw-declarations"
python scripts/test_mobile_android_fixed_exception_covariance.py \
  --neverd build-mobile/bin/neverd --d8 "$NEVERD_D8" \
  --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
  --work-dir "$RUNNER_TEMP/android-fixed-exception-covariance"
python scripts/test_mobile_android_fixed_exception_covariance.py --dependency-root \
  --neverd build-mobile/bin/neverd --d8 "$NEVERD_D8" \
  --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
  --work-dir "$RUNNER_TEMP/android-fixed-exception-covariance-declarations"
python scripts/test_mobile_android_unchecked_covariant_parameters.py \
  --neverd build-mobile/bin/neverd --d8 "$NEVERD_D8" \
  --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
  --work-dir "$RUNNER_TEMP/android-unchecked-covariant-parameters"
python scripts/test_mobile_android_unchecked_covariant_parameters.py --dependency-root \
  --neverd build-mobile/bin/neverd --d8 "$NEVERD_D8" \
  --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
  --work-dir "$RUNNER_TEMP/android-unchecked-covariant-parameter-declarations"
python scripts/test_mobile_android_scoped_unchecked_exception_narrowing.py \
  --neverd build-mobile/bin/neverd --d8 "$NEVERD_D8" \
  --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
  --work-dir "$RUNNER_TEMP/android-scoped-unchecked-exception-narrowing"
python scripts/test_mobile_android_scoped_unchecked_exception_narrowing.py --dependency-root \
  --neverd build-mobile/bin/neverd --d8 "$NEVERD_D8" \
  --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
  --work-dir "$RUNNER_TEMP/android-scoped-unchecked-exception-narrowing-declarations"
python scripts/test_mobile_android_plain_fixed_exceptions.py --exception-subsets \
  --neverd build-mobile/bin/neverd --d8 "$NEVERD_D8" \
  --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
  --work-dir "$RUNNER_TEMP/android-fixed-exception-subsets"
python scripts/test_mobile_android_plain_fixed_exceptions.py --exception-subsets --dependency-root \
  --neverd build-mobile/bin/neverd --d8 "$NEVERD_D8" \
  --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
  --work-dir "$RUNNER_TEMP/android-fixed-exception-subset-declarations"
python scripts/test_mobile_android_plain_fixed_exceptions.py \
  --neverd build-mobile/bin/neverd --d8 "$NEVERD_D8" \
  --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
  --work-dir "$RUNNER_TEMP/android-plain-fixed-exceptions"
python scripts/test_mobile_android_plain_fixed_exceptions.py --dependency-root \
  --neverd build-mobile/bin/neverd --d8 "$NEVERD_D8" \
  --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
  --work-dir "$RUNNER_TEMP/android-plain-fixed-exception-declarations"
python scripts/test_mobile_android_recursive_unchecked_callbacks.py \
  --neverd build-mobile/bin/neverd --d8 "$NEVERD_D8" \
  --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
  --work-dir "$RUNNER_TEMP/android-recursive-unchecked-callbacks"
python scripts/test_mobile_android_recursive_unchecked_callbacks.py --dependency-root \
  --neverd build-mobile/bin/neverd --d8 "$NEVERD_D8" \
  --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
  --work-dir "$RUNNER_TEMP/android-recursive-unchecked-callback-declarations"
python scripts/test_mobile_android_unchecked_bridge_target_throws.py \
  --neverd build-mobile/bin/neverd --d8 "$NEVERD_D8" \
  --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
  --work-dir "$RUNNER_TEMP/android-unchecked-bridge-target-throws"
python scripts/test_mobile_android_unchecked_bridge_target_throws.py --dependency-interface \
  --neverd build-mobile/bin/neverd --d8 "$NEVERD_D8" \
  --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
  --work-dir "$RUNNER_TEMP/android-unchecked-bridge-target-throw-declarations"
python scripts/test_mobile_android_unchecked_bridge_target_throws.py --dependency-interface --default-override \
  --neverd build-mobile/bin/neverd --d8 "$NEVERD_D8" \
  --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
  --work-dir "$RUNNER_TEMP/android-unchecked-default-override-declarations"
python scripts/test_mobile_android_class_formal_object_returns.py \
  --neverd build-mobile/bin/neverd --d8 "$NEVERD_D8" \
  --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
  --work-dir "$RUNNER_TEMP/android-class-formal-object-returns"
python scripts/test_mobile_android_class_formal_object_returns.py --dependency-interface \
  --neverd build-mobile/bin/neverd --d8 "$NEVERD_D8" \
  --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
  --work-dir "$RUNNER_TEMP/android-class-formal-object-return-declarations"
python scripts/test_mobile_android_bounded_formal_object_contracts.py \
  --neverd build-mobile/bin/neverd --d8 "$NEVERD_D8" \
  --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
  --work-dir "$RUNNER_TEMP/android-bounded-formal-object-contracts"
python scripts/test_mobile_android_bounded_formal_object_contracts.py --dependency-root \
  --neverd build-mobile/bin/neverd --d8 "$NEVERD_D8" \
  --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
  --work-dir "$RUNNER_TEMP/android-bounded-formal-object-declarations"
python scripts/test_mobile_android_bounded_formal_object_contracts.py --dependency-root --default-type \
  --neverd build-mobile/bin/neverd --d8 "$NEVERD_D8" \
  --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
  --work-dir "$RUNNER_TEMP/android-bounded-formal-default-type-declarations"
python scripts/test_mobile_android_scoped_source_defaults.py \
  --neverd build-mobile/bin/neverd --d8 "$NEVERD_D8" \
  --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
  --work-dir "$RUNNER_TEMP/android-scoped-source-defaults"
python scripts/test_mobile_android_scoped_source_defaults.py --dependency-interfaces \
  --neverd build-mobile/bin/neverd --d8 "$NEVERD_D8" \
  --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
  --work-dir "$RUNNER_TEMP/android-scoped-source-default-declarations"
python scripts/test_mobile_android_source_default_parameter_bridges.py \
  --neverd build-mobile/bin/neverd --d8 "$NEVERD_D8" \
  --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
  --work-dir "$RUNNER_TEMP/android-source-default-parameter-bridges"
python scripts/test_mobile_android_source_default_parameter_bridges.py --dependency-root \
  --neverd build-mobile/bin/neverd --d8 "$NEVERD_D8" \
  --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
  --work-dir "$RUNNER_TEMP/android-source-default-parameter-bridge-declarations"
python scripts/test_mobile_android_interface_ancestors.py \
  --neverd build-mobile/bin/neverd --d8 "$NEVERD_D8" \
  --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
  --work-dir "$RUNNER_TEMP/android-interface-ancestors"
python scripts/test_mobile_android_interface_ancestors.py --dependency-interfaces \
  --neverd build-mobile/bin/neverd --d8 "$NEVERD_D8" \
  --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
  --work-dir "$RUNNER_TEMP/android-external-interface-ancestors"
python scripts/test_mobile_android_interface_ancestors.py --marker-chain \
  --neverd build-mobile/bin/neverd --d8 "$NEVERD_D8" \
  --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
  --work-dir "$RUNNER_TEMP/android-marker-interface-ancestors"
python scripts/test_mobile_android_interface_ancestors.py --marker-chain --dependency-interfaces \
  --neverd build-mobile/bin/neverd --d8 "$NEVERD_D8" \
  --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
  --work-dir "$RUNNER_TEMP/android-external-marker-interface-ancestors"
python scripts/test_mobile_android_plain_interface_ancestors.py \
  --neverd build-mobile/bin/neverd --d8 "$NEVERD_D8" \
  --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
  --work-dir "$RUNNER_TEMP/android-plain-interface-ancestors"
python scripts/test_mobile_android_plain_interface_ancestors.py --dependency-interfaces \
  --neverd build-mobile/bin/neverd --d8 "$NEVERD_D8" \
  --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
  --work-dir "$RUNNER_TEMP/android-external-plain-interface-ancestors"
python scripts/test_mobile_android_public_protected.py \
  --neverd build-mobile/bin/neverd --d8 "$NEVERD_D8" \
  --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
  --work-dir "$RUNNER_TEMP/android-public-protected"
python scripts/test_mobile_android_closed_getter_forwarders.py \
  --neverd build-mobile/bin/neverd --java-home "$JAVA_HOME" --d8 "$NEVERD_D8" \
  --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
  --work-dir "$RUNNER_TEMP/android-closed-getter-forwarders"
python scripts/test_mobile_android_closed_getter_forwarders.py --dependency-parents \
  --neverd build-mobile/bin/neverd --java-home "$JAVA_HOME" --d8 "$NEVERD_D8" \
  --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
  --work-dir "$RUNNER_TEMP/android-closed-getter-forwarder-declarations"
python scripts/test_mobile_android_plain_nonreturning_void.py \
  --neverd build-mobile/bin/neverd --java-home "$JAVA_HOME" --d8 "$NEVERD_D8" \
  --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
  --work-dir "$RUNNER_TEMP/android-plain-nonreturning-void"
python scripts/test_mobile_android_plain_nonreturning_void.py --dependency-parents \
  --neverd build-mobile/bin/neverd --java-home "$JAVA_HOME" --d8 "$NEVERD_D8" \
  --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
  --work-dir "$RUNNER_TEMP/android-plain-nonreturning-void-declarations"
python scripts/test_mobile_android_protected_generic_results.py \
  --neverd build-mobile/bin/neverd --d8 "$NEVERD_D8" \
  --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
  --work-dir "$RUNNER_TEMP/android-protected-generic-results"
python scripts/test_mobile_android_protected_generic_results.py --dependency-parent \
  --neverd build-mobile/bin/neverd --d8 "$NEVERD_D8" \
  --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
  --work-dir "$RUNNER_TEMP/android-protected-generic-result-declarations"
python scripts/test_mobile_android_protected_array_results.py \
  --neverd build-mobile/bin/neverd --d8 "$NEVERD_D8" \
  --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
  --work-dir "$RUNNER_TEMP/android-protected-array-results"
python scripts/test_mobile_android_protected_array_results.py --dependency-parent \
  --neverd build-mobile/bin/neverd --d8 "$NEVERD_D8" \
  --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
  --work-dir "$RUNNER_TEMP/android-protected-array-result-declarations"
python scripts/test_mobile_android_abstract_bounded_parameters.py \
  --neverd build-mobile/bin/neverd --d8 "$NEVERD_D8" \
  --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
  --work-dir "$RUNNER_TEMP/android-abstract-bounded-parameters"
python scripts/test_mobile_android_abstract_bounded_parameters.py --dependency-root \
  --neverd build-mobile/bin/neverd --d8 "$NEVERD_D8" \
  --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
  --work-dir "$RUNNER_TEMP/android-abstract-bounded-parameters-sdk"
python scripts/test_mobile_android_abstract_bounded_parameters.py --concrete-parent \
  --neverd build-mobile/bin/neverd --d8 "$NEVERD_D8" \
  --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
  --work-dir "$RUNNER_TEMP/android-abstract-concrete-bounded"
python scripts/test_mobile_android_abstract_bounded_parameters.py --concrete-parent --dependency-root \
  --neverd build-mobile/bin/neverd --d8 "$NEVERD_D8" \
  --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
  --work-dir "$RUNNER_TEMP/android-abstract-concrete-bounded-sdk"
python scripts/test_mobile_android_protected_scoped_arrays.py \
  --neverd build-mobile/bin/neverd --d8 "$NEVERD_D8" \
  --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
  --work-dir "$RUNNER_TEMP/android-protected-scoped-arrays"
python scripts/test_mobile_android_protected_scoped_arrays.py --dependency-parent \
  --neverd build-mobile/bin/neverd --d8 "$NEVERD_D8" \
  --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
  --work-dir "$RUNNER_TEMP/android-protected-scoped-arrays-sdk"
python scripts/test_mobile_android_protected_scoped_array_returns.py \
  --neverd build-mobile/bin/neverd --d8 "$NEVERD_D8" \
  --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
  --work-dir "$RUNNER_TEMP/android-protected-scoped-array-returns"
python scripts/test_mobile_android_protected_scoped_array_returns.py --dependency-parent \
  --neverd build-mobile/bin/neverd --d8 "$NEVERD_D8" \
  --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
  --work-dir "$RUNNER_TEMP/android-protected-scoped-array-returns-sdk"
python scripts/test_mobile_android_boxed_array_superclass_results.py \
  --neverd build-mobile/bin/neverd --d8 "$NEVERD_D8" \
  --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
  --work-dir "$RUNNER_TEMP/android-boxed-array-superclass"
python scripts/test_mobile_android_boxed_array_superclass_results.py --dependency-parent \
  --neverd build-mobile/bin/neverd --d8 "$NEVERD_D8" \
  --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
  --work-dir "$RUNNER_TEMP/android-boxed-array-superclass-sdk"
python scripts/test_mobile_android_bound_boxed_superclass_results.py \
  --neverd build-mobile/bin/neverd --d8 "$NEVERD_D8" \
  --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
  --work-dir "$RUNNER_TEMP/android-bound-boxed-superclass"
python scripts/test_mobile_android_bound_boxed_superclass_results.py --dependency-parent \
  --neverd build-mobile/bin/neverd --d8 "$NEVERD_D8" \
  --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
  --work-dir "$RUNNER_TEMP/android-bound-boxed-superclass-sdk"
python scripts/test_mobile_android_multiparam_boxed_superclass_results.py \
  --neverd build-mobile/bin/neverd --d8 "$NEVERD_D8" \
  --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
  --work-dir "$RUNNER_TEMP/android-multiparam-boxed-superclass"
python scripts/test_mobile_android_multiparam_boxed_superclass_results.py --dependency-parent \
  --neverd build-mobile/bin/neverd --d8 "$NEVERD_D8" \
  --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
  --work-dir "$RUNNER_TEMP/android-multiparam-boxed-superclass-sdk"
python scripts/test_mobile_android_array_carrier_superclass.py \
  --neverd build-mobile/bin/neverd --d8 "$NEVERD_D8" \
  --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
  --work-dir "$RUNNER_TEMP/android-array-carrier-superclass"
python scripts/test_mobile_android_array_carrier_superclass.py --dependency-parent \
  --neverd build-mobile/bin/neverd --d8 "$NEVERD_D8" \
  --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
  --work-dir "$RUNNER_TEMP/android-array-carrier-superclass-sdk"
python scripts/test_mobile_android_protected_fixed_results.py \
  --neverd build-mobile/bin/neverd --d8 "$NEVERD_D8" \
  --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
  --work-dir "$RUNNER_TEMP/android-protected-fixed-results"
python scripts/test_mobile_android_protected_fixed_results.py --dependency-parent \
  --neverd build-mobile/bin/neverd --d8 "$NEVERD_D8" \
  --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
  --work-dir "$RUNNER_TEMP/android-protected-fixed-results-declarations"
python scripts/test_mobile_android_protected_open_getters.py \
  --neverd build-mobile/bin/neverd --d8 "$NEVERD_D8" \
  --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
  --work-dir "$RUNNER_TEMP/android-protected-open-getters"
python scripts/test_mobile_android_protected_open_getters.py --dependency-parent \
  --neverd build-mobile/bin/neverd --d8 "$NEVERD_D8" \
  --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
  --work-dir "$RUNNER_TEMP/android-protected-open-getter-declarations"
python scripts/test_mobile_android_protected_closed_getter.py \
  --neverd build-mobile/bin/neverd --d8 "$NEVERD_D8" \
  --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
  --work-dir "$RUNNER_TEMP/android-protected-closed-getter"
python scripts/test_mobile_android_public_bounded_self_getter.py \
  --neverd build-mobile/bin/neverd --d8 "$NEVERD_D8" \
  --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
  --work-dir "$RUNNER_TEMP/android-public-bounded-self-getter"
python scripts/test_mobile_android_public_bounded_self_getter.py --dependency-parent \
  --neverd build-mobile/bin/neverd --d8 "$NEVERD_D8" \
  --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
  --work-dir "$RUNNER_TEMP/android-public-bounded-self-getter-dependency"
python scripts/test_mobile_android_inherited_exception_bridges.py \
  --neverd build-mobile/bin/neverd --d8 "$NEVERD_D8" \
  --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
  --work-dir "$RUNNER_TEMP/android-inherited-exception-bridges"
python scripts/test_mobile_android_inherited_exception_bridges.py --dependency-parent \
  --neverd build-mobile/bin/neverd --d8 "$NEVERD_D8" \
  --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
  --work-dir "$RUNNER_TEMP/android-inherited-exception-bridges-dependency"
python scripts/test_mobile_android_source_interface_parent.py \
  --neverd build-mobile/bin/neverd --d8 "$NEVERD_D8" \
  --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
  --work-dir "$RUNNER_TEMP/android-source-interface-parent"
python scripts/test_mobile_android_source_generic_chain.py \
  --neverd build-mobile/bin/neverd --d8 "$NEVERD_D8" \
  --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
  --work-dir "$RUNNER_TEMP/android-source-generic-chain"
python scripts/test_mobile_android_deferred_abstract_interfaces.py \
  --neverd build-mobile/bin/neverd --d8 "$NEVERD_D8" \
  --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
  --work-dir "$RUNNER_TEMP/android-deferred-abstract-interfaces"
python scripts/test_mobile_android_deferred_abstract_interfaces.py --source-interfaces \
  --neverd build-mobile/bin/neverd --d8 "$NEVERD_D8" \
  --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
  --work-dir "$RUNNER_TEMP/android-deferred-source-interfaces"
python scripts/test_mobile_android_deferred_abstract_interfaces.py --source-interfaces --class-ancestor \
  --neverd build-mobile/bin/neverd --d8 "$NEVERD_D8" \
  --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
  --work-dir "$RUNNER_TEMP/android-deferred-source-class-ancestor"
python scripts/test_mobile_android_deferred_abstract_interfaces.py --dependency-ancestor \
  --neverd build-mobile/bin/neverd --d8 "$NEVERD_D8" \
  --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
  --work-dir "$RUNNER_TEMP/android-deferred-external-class-ancestor"
python scripts/test_mobile_android_deferred_abstract_interfaces.py --fixed-throws \
  --neverd build-mobile/bin/neverd --d8 "$NEVERD_D8" \
  --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
  --work-dir "$RUNNER_TEMP/android-deferred-closed-throws"
python scripts/test_mobile_android_deferred_abstract_interfaces.py --fixed-throws --source-interfaces \
  --neverd build-mobile/bin/neverd --d8 "$NEVERD_D8" \
  --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
  --work-dir "$RUNNER_TEMP/android-deferred-closed-source-throws"
python scripts/test_mobile_android_deferred_abstract_interfaces.py --fixed-throws --source-interfaces --class-ancestor \
  --neverd build-mobile/bin/neverd --d8 "$NEVERD_D8" \
  --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
  --work-dir "$RUNNER_TEMP/android-deferred-closed-source-ancestor"
python scripts/test_mobile_android_deferred_abstract_interfaces.py --fixed-throws --dependency-ancestor \
  --neverd build-mobile/bin/neverd --d8 "$NEVERD_D8" \
  --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
  --work-dir "$RUNNER_TEMP/android-deferred-closed-external-ancestor"
python scripts/test_mobile_android_ordered_sdk_set.py \
  --neverd build-mobile/bin/neverd --d8 "$NEVERD_D8" \
  --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
  --work-dir "$RUNNER_TEMP/android-ordered-sdk-set"
python scripts/test_mobile_android_sdk_abstract_interface_roots.py \
  --neverd build-mobile/bin/neverd --d8 "$NEVERD_D8" \
  --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
  --work-dir "$RUNNER_TEMP/android-sdk-abstract-interface-roots"
python scripts/test_mobile_android_external_generic_chain.py \
  --neverd build-mobile/bin/neverd --d8 "$NEVERD_D8" \
  --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
  --work-dir "$RUNNER_TEMP/android-external-generic-chain"
python scripts/test_mobile_android_external_owned_ancestors.py \
  --neverd build-mobile/bin/neverd --d8 "$NEVERD_D8" \
  --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
  --work-dir "$RUNNER_TEMP/android-external-owned-ancestors"
python scripts/test_mobile_android_marker_default_overrides.py \
  --neverd build-mobile/bin/neverd --d8 "$NEVERD_D8" \
  --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
  --work-dir "$RUNNER_TEMP/android-marker-default-overrides"
python scripts/test_mobile_android_method_formals.py --nominal-bounds \
  --neverd build-mobile/bin/neverd --d8 "$NEVERD_D8" \
  --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
  --work-dir "$RUNNER_TEMP/android-nominal-method-formals"
python scripts/test_mobile_android_method_formals.py --interface-bounds \
  --neverd build-mobile/bin/neverd --d8 "$NEVERD_D8" \
  --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
  --work-dir "$RUNNER_TEMP/android-interface-method-formals"
python scripts/test_mobile_android_method_formals.py --recursive-interface-bounds \
  --neverd build-mobile/bin/neverd --d8 "$NEVERD_D8" \
  --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
  --work-dir "$RUNNER_TEMP/android-recursive-interface-method-formals"
python scripts/test_mobile_android_class_dependent_bounds.py \
  --neverd build-mobile/bin/neverd --d8 "$NEVERD_D8" \
  --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
  --work-dir "$RUNNER_TEMP/android-class-dependent-bounds"
python scripts/test_mobile_android_class_dependent_bounds.py --interface-slot \
  --neverd build-mobile/bin/neverd --d8 "$NEVERD_D8" \
  --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
  --work-dir "$RUNNER_TEMP/android-class-dependent-bound-positions"
python scripts/test_mobile_android_class_dependent_bounds.py --dependency-provider \
  --neverd build-mobile/bin/neverd --d8 "$NEVERD_D8" \
  --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
  --work-dir "$RUNNER_TEMP/android-class-dependent-bounds-provider"
python scripts/test_mobile_android_class_dependent_bounds.py --interface-slot --dependency-provider \
  --neverd build-mobile/bin/neverd --d8 "$NEVERD_D8" \
  --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
  --work-dir "$RUNNER_TEMP/android-class-dependent-bound-positions-provider"
python scripts/test_mobile_android_inherited_dependent_bounds.py \
  --neverd build-mobile/bin/neverd --d8 "$NEVERD_D8" \
  --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
  --work-dir "$RUNNER_TEMP/android-inherited-dependent-bounds"
python scripts/test_mobile_android_inherited_dependent_bounds.py --interface-slot \
  --neverd build-mobile/bin/neverd --d8 "$NEVERD_D8" \
  --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
  --work-dir "$RUNNER_TEMP/android-inherited-dependent-bound-positions"
python scripts/test_mobile_android_inherited_dependent_bounds.py --dependency-provider \
  --neverd build-mobile/bin/neverd --d8 "$NEVERD_D8" \
  --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
  --work-dir "$RUNNER_TEMP/android-inherited-dependent-provider"
python scripts/test_mobile_android_inherited_dependent_bounds.py --interface-slot --dependency-provider \
  --neverd build-mobile/bin/neverd --d8 "$NEVERD_D8" \
  --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
  --work-dir "$RUNNER_TEMP/android-inherited-dependent-provider-positions"
for primitive in boolean byte char short int long float double; do
  python scripts/test_mobile_android_inherited_scalar_results.py --primitive "$primitive" \
    --neverd build-mobile/bin/neverd --d8 "$NEVERD_D8" \
    --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
    --work-dir "$RUNNER_TEMP/android-$primitive-scalar-results"
  python scripts/test_mobile_android_inherited_scalar_results.py --primitive "$primitive" --dependency-parents \
    --neverd build-mobile/bin/neverd --d8 "$NEVERD_D8" \
    --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
    --work-dir "$RUNNER_TEMP/android-$primitive-scalar-results-declarations"
done
python scripts/test_mobile_android_inherited_scalar_consumers.py --primitive int \
  --neverd build-mobile/bin/neverd --d8 "$NEVERD_D8" \
  --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
  --work-dir "$RUNNER_TEMP/android-int-scalar-consumers"
python scripts/test_mobile_android_inherited_scalar_consumers.py --primitive int --dependency-parents \
  --neverd build-mobile/bin/neverd --d8 "$NEVERD_D8" \
  --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
  --work-dir "$RUNNER_TEMP/android-int-scalar-consumers-declarations"
python scripts/test_mobile_android_inherited_scalar_consumers.py --primitive long \
  --neverd build-mobile/bin/neverd --d8 "$NEVERD_D8" \
  --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
  --work-dir "$RUNNER_TEMP/android-long-scalar-consumers"
python scripts/test_mobile_android_inherited_scalar_consumers.py --primitive long --dependency-parents \
  --neverd build-mobile/bin/neverd --d8 "$NEVERD_D8" \
  --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
  --work-dir "$RUNNER_TEMP/android-long-scalar-consumers-declarations"
python scripts/test_mobile_android_narrow_number_consumers.py --primitive byte \
  --neverd build-mobile/bin/neverd --d8 "$NEVERD_D8" \
  --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
  --work-dir "$RUNNER_TEMP/android-byte-number-consumers"
python scripts/test_mobile_android_narrow_number_consumers.py --primitive byte --dependency-parents \
  --neverd build-mobile/bin/neverd --d8 "$NEVERD_D8" \
  --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
  --work-dir "$RUNNER_TEMP/android-byte-number-consumers-declarations"
python scripts/test_mobile_android_narrow_number_consumers.py --primitive short \
  --neverd build-mobile/bin/neverd --d8 "$NEVERD_D8" \
  --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
  --work-dir "$RUNNER_TEMP/android-short-number-consumers"
python scripts/test_mobile_android_narrow_number_consumers.py --primitive short --dependency-parents \
  --neverd build-mobile/bin/neverd --d8 "$NEVERD_D8" \
  --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
  --work-dir "$RUNNER_TEMP/android-short-number-consumers-declarations"
python scripts/test_mobile_android_protected_array_reference.py \
  --neverd build-mobile/bin/neverd --d8 "$NEVERD_D8" \
  --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
  --work-dir "$RUNNER_TEMP/android-protected-array-reference"
python scripts/test_mobile_android_protected_array_reference.py --dependency-parent \
  --neverd build-mobile/bin/neverd --d8 "$NEVERD_D8" \
  --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
  --work-dir "$RUNNER_TEMP/android-protected-array-reference-declarations"
python scripts/test_mobile_android_inherited_final_protected.py \
  --neverd build-mobile/bin/neverd --d8 "$NEVERD_D8" \
  --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
  --work-dir "$RUNNER_TEMP/android-inherited-final-protected"
python scripts/test_mobile_android_inherited_final_protected.py --dependency-parent \
  --neverd build-mobile/bin/neverd --d8 "$NEVERD_D8" \
  --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
  --work-dir "$RUNNER_TEMP/android-inherited-final-protected-declarations"
python scripts/test_mobile_android_protected_mixed_arrays.py \
  --neverd build-mobile/bin/neverd --d8 "$NEVERD_D8" \
  --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
  --work-dir "$RUNNER_TEMP/android-protected-mixed-arrays"
python scripts/test_mobile_android_protected_mixed_arrays.py --dependency-parent \
  --neverd build-mobile/bin/neverd --d8 "$NEVERD_D8" \
  --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
  --work-dir "$RUNNER_TEMP/android-protected-mixed-arrays-declarations"
python scripts/test_mobile_android_protected_mixed_consumers.py \
  --neverd build-mobile/bin/neverd --d8 "$NEVERD_D8" \
  --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
  --work-dir "$RUNNER_TEMP/android-protected-mixed-consumers"
python scripts/test_mobile_android_protected_mixed_consumers.py --dependency-parent \
  --neverd build-mobile/bin/neverd --d8 "$NEVERD_D8" \
  --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
  --work-dir "$RUNNER_TEMP/android-protected-mixed-consumers-declarations"
python scripts/test_mobile_android_protected_array_getters.py \
  --neverd build-mobile/bin/neverd --d8 "$NEVERD_D8" \
  --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
  --work-dir "$RUNNER_TEMP/android-protected-array-getters"
python scripts/test_mobile_android_protected_array_getters.py --dependency-parent \
  --neverd build-mobile/bin/neverd --d8 "$NEVERD_D8" \
  --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
  --work-dir "$RUNNER_TEMP/android-protected-array-getters-declarations"
python scripts/test_mobile_android_inherited_protected_provider.py \
  --neverd build-mobile/bin/neverd --d8 "$NEVERD_D8" \
  --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
  --work-dir "$RUNNER_TEMP/android-inherited-protected-provider"
python scripts/test_mobile_android_inherited_protected_provider.py --dependency-parent \
  --neverd build-mobile/bin/neverd --d8 "$NEVERD_D8" \
  --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
  --work-dir "$RUNNER_TEMP/android-inherited-protected-provider-declarations"
python scripts/test_mobile_android_protected_scoped_results.py \
  --neverd build-mobile/bin/neverd --d8 "$NEVERD_D8" \
  --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
  --work-dir "$RUNNER_TEMP/android-protected-scoped-results"
python scripts/test_mobile_android_protected_scoped_results.py --dependency-parent \
  --neverd build-mobile/bin/neverd --d8 "$NEVERD_D8" \
  --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
  --work-dir "$RUNNER_TEMP/android-protected-scoped-results-declarations"
python scripts/test_mobile_android_protected_bounded_scoped.py \
  --neverd build-mobile/bin/neverd --d8 "$NEVERD_D8" \
  --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
  --work-dir "$RUNNER_TEMP/android-protected-bounded-scoped-interface"
python scripts/test_mobile_android_protected_bounded_scoped.py --dependency-parent \
  --neverd build-mobile/bin/neverd --d8 "$NEVERD_D8" \
  --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
  --work-dir "$RUNNER_TEMP/android-protected-bounded-scoped-interface-declarations"
python scripts/test_mobile_android_protected_bounded_scoped.py --class-bound \
  --neverd build-mobile/bin/neverd --d8 "$NEVERD_D8" \
  --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
  --work-dir "$RUNNER_TEMP/android-protected-bounded-scoped-class"
python scripts/test_mobile_android_protected_bounded_scoped.py --class-bound --dependency-parent \
  --neverd build-mobile/bin/neverd --d8 "$NEVERD_D8" \
  --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
  --work-dir "$RUNNER_TEMP/android-protected-bounded-scoped-class-declarations"
python scripts/test_mobile_android_protected_scoped_void.py \
  --neverd build-mobile/bin/neverd --d8 "$NEVERD_D8" \
  --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
  --work-dir "$RUNNER_TEMP/android-protected-scoped-void"
python scripts/test_mobile_android_protected_scoped_void.py --dependency-parent \
  --neverd build-mobile/bin/neverd --d8 "$NEVERD_D8" \
  --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
  --work-dir "$RUNNER_TEMP/android-protected-scoped-void-declarations"
python scripts/test_mobile_android_inherited_wide_consumers.py \
  --neverd build-mobile/bin/neverd --d8 "$NEVERD_D8" \
  --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
  --work-dir "$RUNNER_TEMP/android-inherited-wide-consumers"
python scripts/test_mobile_android_inherited_wide_consumers.py --dependency-parents \
  --neverd build-mobile/bin/neverd --d8 "$NEVERD_D8" \
  --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
  --work-dir "$RUNNER_TEMP/android-inherited-wide-consumers-declarations"
python scripts/test_mobile_android_specialized_dependent_bounds.py \
  --neverd build-mobile/bin/neverd --d8 "$NEVERD_D8" \
  --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
  --work-dir "$RUNNER_TEMP/android-specialized-dependent-bounds"
python scripts/test_mobile_android_specialized_dependent_bounds.py --interface-slot \
  --neverd build-mobile/bin/neverd --d8 "$NEVERD_D8" \
  --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
  --work-dir "$RUNNER_TEMP/android-specialized-dependent-bounds-positions"
python scripts/test_mobile_android_specialized_dependent_bounds.py --dependency-parents \
  --neverd build-mobile/bin/neverd --d8 "$NEVERD_D8" \
  --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
  --work-dir "$RUNNER_TEMP/android-specialized-dependent-bounds-declarations"
python scripts/test_mobile_android_specialized_dependent_bounds.py --dependency-parents --interface-slot \
  --neverd build-mobile/bin/neverd --d8 "$NEVERD_D8" \
  --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
  --work-dir "$RUNNER_TEMP/android-specialized-dependent-bounds-declaration-positions"
python scripts/test_mobile_android_dependent_method_bounds.py \
  --neverd build-mobile/bin/neverd --d8 "$NEVERD_D8" \
  --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
  --work-dir "$RUNNER_TEMP/android-dependent-method-bounds"
python scripts/test_mobile_android_dependent_method_bounds.py --interface-slot \
  --neverd build-mobile/bin/neverd --d8 "$NEVERD_D8" \
  --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
  --work-dir "$RUNNER_TEMP/android-dependent-method-bound-positions"
python scripts/test_mobile_android_dependent_method_bounds.py --dependency-root \
  --neverd build-mobile/bin/neverd --d8 "$NEVERD_D8" \
  --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
  --work-dir "$RUNNER_TEMP/android-dependent-method-bounds-declarations"
python scripts/test_mobile_android_dependent_method_bounds.py --interface-slot --dependency-root \
  --neverd build-mobile/bin/neverd --d8 "$NEVERD_D8" \
  --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
  --work-dir "$RUNNER_TEMP/android-dependent-method-bound-positions-declarations"
python scripts/test_mobile_android_method_formals.py --recursive-class-bounds \
  --neverd build-mobile/bin/neverd --d8 "$NEVERD_D8" \
  --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
  --work-dir "$RUNNER_TEMP/android-recursive-class-method-formals"
python scripts/test_mobile_android_recursive_enum_defaults.py \
  --neverd build-mobile/bin/neverd --d8 "$NEVERD_D8" \
  --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
  --work-dir "$RUNNER_TEMP/android-recursive-enum-defaults"
python scripts/test_mobile_android_recursive_enum_defaults.py --dependency-interface \
  --neverd build-mobile/bin/neverd --d8 "$NEVERD_D8" \
  --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
  --work-dir "$RUNNER_TEMP/android-recursive-enum-default-declarations"
python scripts/test_mobile_android_recursive_enum_defaults.py --nested-ancestor \
  --neverd build-mobile/bin/neverd --d8 "$NEVERD_D8" \
  --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
  --work-dir "$RUNNER_TEMP/android-nested-default-ancestors"
python scripts/test_mobile_android_recursive_enum_defaults.py --nested-ancestor --dependency-ancestor \
  --neverd build-mobile/bin/neverd --d8 "$NEVERD_D8" \
  --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
  --work-dir "$RUNNER_TEMP/android-nested-default-ancestor-declarations"
python scripts/test_mobile_android_recursive_enum_defaults.py --nested-ancestor --ancestor-chain \
  --neverd build-mobile/bin/neverd --d8 "$NEVERD_D8" \
  --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
  --work-dir "$RUNNER_TEMP/android-plain-default-class-chains"
python scripts/test_mobile_android_recursive_enum_defaults.py --nested-ancestor --ancestor-chain --dependency-ancestor \
  --neverd build-mobile/bin/neverd --d8 "$NEVERD_D8" \
  --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
  --work-dir "$RUNNER_TEMP/android-plain-default-class-chain-declarations"
python scripts/test_mobile_android_recursive_enum_defaults.py --nested-ancestor --ancestor-chain --ancestor-throws \
  --neverd build-mobile/bin/neverd --d8 "$NEVERD_D8" \
  --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
  --work-dir "$RUNNER_TEMP/android-complete-ancestor-throws"
python scripts/test_mobile_android_recursive_enum_defaults.py --nested-ancestor --ancestor-chain --ancestor-throws --dependency-ancestor \
  --neverd build-mobile/bin/neverd --d8 "$NEVERD_D8" \
  --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
  --work-dir "$RUNNER_TEMP/android-complete-ancestor-throws-declarations"
python scripts/test_mobile_android_method_formals.py \
  --neverd build-mobile/bin/neverd --d8 "$NEVERD_D8" \
  --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
  --work-dir "$RUNNER_TEMP/android-method-formals"
python scripts/test_mobile_android_external_default_chain.py \
  --neverd build-mobile/bin/neverd --d8 "$NEVERD_D8" \
  --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
  --work-dir "$RUNNER_TEMP/android-external-default-chain"
python scripts/test_mobile_android_external_default_chain.py --class-ancestor \
  --neverd build-mobile/bin/neverd --d8 "$NEVERD_D8" \
  --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
  --work-dir "$RUNNER_TEMP/android-class-external-defaults"
python scripts/test_mobile_android_external_default_chain.py --class-interface-branches \
  --neverd build-mobile/bin/neverd --d8 "$NEVERD_D8" \
  --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
  --work-dir "$RUNNER_TEMP/android-class-interface-default-branches"
python scripts/test_mobile_android_external_default_chain.py --class-interface-branches --plain-default-override \
  --neverd build-mobile/bin/neverd --d8 "$NEVERD_D8" \
  --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
  --work-dir "$RUNNER_TEMP/android-plain-class-default-overrides"
python scripts/test_mobile_android_mixed_external_defaults.py \
  --neverd build-mobile/bin/neverd --d8 "$NEVERD_D8" \
  --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
  --work-dir "$RUNNER_TEMP/android-mixed-external-defaults"
python scripts/test_mobile_android_mixed_external_defaults.py --class-ancestor \
  --neverd build-mobile/bin/neverd --d8 "$NEVERD_D8" \
  --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
  --work-dir "$RUNNER_TEMP/android-transitive-class-default-roots"
python scripts/test_mobile_android_plain_dependency_defaults.py \
  --neverd build-mobile/bin/neverd --d8 "$NEVERD_D8" \
  --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
  --work-dir "$RUNNER_TEMP/android-plain-dependency-defaults"
python scripts/test_mobile_android_plain_dependency_defaults.py --dependency-parent \
  --neverd build-mobile/bin/neverd --d8 "$NEVERD_D8" \
  --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
  --work-dir "$RUNNER_TEMP/android-plain-dependency-defaults-external"
python scripts/test_mobile_android_plain_dependency_defaults.py --member-signatures \
  --neverd build-mobile/bin/neverd --d8 "$NEVERD_D8" \
  --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
  --work-dir "$RUNNER_TEMP/android-plain-member-contracts"
python scripts/test_mobile_android_plain_dependency_defaults.py --member-signatures --dependency-parent \
  --neverd build-mobile/bin/neverd --d8 "$NEVERD_D8" \
  --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
  --work-dir "$RUNNER_TEMP/android-plain-member-contracts-external"
python scripts/test_mobile_android_plain_dependency_defaults.py --nonpublic-methods \
  --neverd build-mobile/bin/neverd --d8 "$NEVERD_D8" \
  --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
  --work-dir "$RUNNER_TEMP/android-plain-nonpublic-members"
python scripts/test_mobile_android_plain_dependency_defaults.py --nonpublic-methods --dependency-parent \
  --neverd build-mobile/bin/neverd --d8 "$NEVERD_D8" \
  --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
  --work-dir "$RUNNER_TEMP/android-plain-nonpublic-member-declarations"
python scripts/test_mobile_android_incompatible_return_slots.py \
  --neverd build-mobile/bin/neverd --d8 "$NEVERD_D8" \
  --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
  --work-dir "$RUNNER_TEMP/android-incompatible-return-slots"
python scripts/test_mobile_android_incompatible_return_slots.py --class-ancestor \
  --neverd build-mobile/bin/neverd --d8 "$NEVERD_D8" \
  --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
  --work-dir "$RUNNER_TEMP/android-inherited-return-slots"
python scripts/test_mobile_android_disjoint_defaults.py \
  --neverd build-mobile/bin/neverd --d8 "$NEVERD_D8" \
  --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
  --work-dir "$RUNNER_TEMP/android-disjoint-defaults"
python scripts/test_mobile_android_inherited_object.py \
  --neverd build-mobile/bin/neverd --d8 "$NEVERD_D8" \
  --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
  --work-dir "$RUNNER_TEMP/android-inherited-object"
python scripts/test_mobile_android_inherited_object.py --class-ancestor \
  --neverd build-mobile/bin/neverd --d8 "$NEVERD_D8" \
  --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
  --work-dir "$RUNNER_TEMP/android-inherited-object-class-ancestor"
python scripts/test_mobile_android_inherited_object.py --dependency-root \
  --neverd build-mobile/bin/neverd --d8 "$NEVERD_D8" \
  --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
  --work-dir "$RUNNER_TEMP/android-inherited-object-dependency-root"
python scripts/test_mobile_android_inherited_class.py \
  --neverd build-mobile/bin/neverd --d8 "$NEVERD_D8" \
  --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
  --work-dir "$RUNNER_TEMP/android-inherited-class"
python scripts/test_mobile_android_inherited_class_default.py \
  --neverd build-mobile/bin/neverd --d8 "$NEVERD_D8" \
  --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
  --work-dir "$RUNNER_TEMP/android-inherited-class-default"
python scripts/test_mobile_android_plain_covariant_contracts.py \
  --neverd build-mobile/bin/neverd --d8 "$NEVERD_D8" \
  --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
  --work-dir "$RUNNER_TEMP/android-plain-covariant-contracts"
python scripts/test_mobile_android_plain_covariant_contracts.py --parent-bridge \
  --neverd build-mobile/bin/neverd --d8 "$NEVERD_D8" \
  --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
  --work-dir "$RUNNER_TEMP/android-covariant-parent-contracts"
python scripts/test_mobile_android_fixed_scope_default_override.py \
  --neverd build-mobile/bin/neverd --d8 "$NEVERD_D8" \
  --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
  --work-dir "$RUNNER_TEMP/android-fixed-scope-default-override"
python scripts/test_mobile_android_superclass_scalar_adapters.py \
  --neverd build-mobile/bin/neverd --d8 "$NEVERD_D8" \
  --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
  --work-dir "$RUNNER_TEMP/android-superclass-scalar-adapters"
python scripts/test_mobile_android_superclass_scalar_adapters.py --dependency-root \
  --neverd build-mobile/bin/neverd --d8 "$NEVERD_D8" \
  --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
  --work-dir "$RUNNER_TEMP/android-superclass-scalar-dependency"
python scripts/test_mobile_android_checked_reference_super_adapters.py \
  --neverd build-mobile/bin/neverd --d8 "$NEVERD_D8" \
  --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
  --work-dir "$RUNNER_TEMP/android-checked-reference-super-adapters"
python scripts/test_mobile_android_checked_reference_super_adapters.py --dependency-root \
  --neverd build-mobile/bin/neverd --d8 "$NEVERD_D8" \
  --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
  --work-dir "$RUNNER_TEMP/android-checked-reference-super-dependency"
python scripts/test_mobile_android_guarded_default_super_adapters.py \
  --neverd build-mobile/bin/neverd --d8 "$NEVERD_D8" \
  --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
  --work-dir "$RUNNER_TEMP/android-guarded-default-super-adapters"
python scripts/test_mobile_android_guarded_default_super_adapters.py --dependency-root \
  --neverd build-mobile/bin/neverd --d8 "$NEVERD_D8" \
  --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
  --work-dir "$RUNNER_TEMP/android-guarded-default-super-dependency"
python scripts/test_mobile_android_raw_marker_superclasses.py \
  --neverd build-mobile/bin/neverd --d8 "$NEVERD_D8" \
  --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
  --work-dir "$RUNNER_TEMP/android-raw-marker-superclasses"
python scripts/test_mobile_android_raw_marker_superclasses.py --dependency-root \
  --neverd build-mobile/bin/neverd --d8 "$NEVERD_D8" \
  --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
  --work-dir "$RUNNER_TEMP/android-raw-marker-dependency"
python scripts/test_mobile_android_inherited_erased_parameters.py \
  --neverd build-mobile/bin/neverd --d8 "$NEVERD_D8" \
  --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
  --work-dir "$RUNNER_TEMP/android-inherited-erased-parameters"
python scripts/test_mobile_android_plain_integer_bridge.py \
  --neverd build-mobile/bin/neverd --d8 "$NEVERD_D8" \
  --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
  --work-dir "$RUNNER_TEMP/android-plain-integer-bridge"
python scripts/test_mobile_android_plain_integer_bridge.py --guarded-reference \
  --neverd build-mobile/bin/neverd --d8 "$NEVERD_D8" \
  --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
  --work-dir "$RUNNER_TEMP/android-guarded-reference-bridge"
python scripts/test_mobile_android_plain_integer_bridge.py --class-ancestor \
  --neverd build-mobile/bin/neverd --d8 "$NEVERD_D8" \
  --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
  --work-dir "$RUNNER_TEMP/android-guarded-source-class-contract"
python scripts/test_mobile_android_plain_integer_bridge.py --dependency-root \
  --neverd build-mobile/bin/neverd --d8 "$NEVERD_D8" \
  --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
  --work-dir "$RUNNER_TEMP/android-guarded-dependency-class-contract"
python scripts/test_mobile_android_plain_integer_bridge.py --inherited-guarded \
  --neverd build-mobile/bin/neverd --d8 "$NEVERD_D8" \
  --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
  --work-dir "$RUNNER_TEMP/android-inherited-guarded-source-contract"
python scripts/test_mobile_android_plain_integer_bridge.py --inherited-guarded --dependency-root \
  --neverd build-mobile/bin/neverd --d8 "$NEVERD_D8" \
  --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
  --work-dir "$RUNNER_TEMP/android-inherited-guarded-dependency-contract"
python scripts/test_mobile_android_plain_integer_bridge.py --guarded-integer \
  --neverd build-mobile/bin/neverd --d8 "$NEVERD_D8" \
  --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
  --work-dir "$RUNNER_TEMP/android-guarded-integer-bridge"
python scripts/test_mobile_android_plain_integer_bridge.py --predicate-guard \
  --neverd build-mobile/bin/neverd --d8 "$NEVERD_D8" \
  --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
  --work-dir "$RUNNER_TEMP/android-predicate-reference-bridge"
python scripts/test_mobile_android_plain_integer_bridge.py --generic-reference-getter \
  --neverd build-mobile/bin/neverd --d8 "$NEVERD_D8" \
  --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
  --work-dir "$RUNNER_TEMP/android-generic-reference-getter"
python scripts/test_mobile_android_plain_integer_bridge.py --indexed-reference-getter \
  --neverd build-mobile/bin/neverd --d8 "$NEVERD_D8" \
  --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
  --work-dir "$RUNNER_TEMP/android-indexed-reference-getter"
python scripts/test_mobile_android_plain_integer_bridge.py --indexed-abstract-ancestor \
  --neverd build-mobile/bin/neverd --d8 "$NEVERD_D8" \
  --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
  --work-dir "$RUNNER_TEMP/android-indexed-abstract-reference-ancestor"
python scripts/test_mobile_android_nested_object_lower_returns.py \
  --neverd build-mobile/bin/neverd --d8 "$NEVERD_D8" \
  --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
  --work-dir "$RUNNER_TEMP/android-nested-object-lower-returns"
python scripts/test_mobile_android_nested_object_lower_returns.py --dependency-root \
  --neverd build-mobile/bin/neverd --d8 "$NEVERD_D8" \
  --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
  --work-dir "$RUNNER_TEMP/android-nested-object-lower-return-declarations"
python scripts/test_mobile_android_bridged_nested_returns.py \
  --neverd build-mobile/bin/neverd --d8 "$NEVERD_D8" \
  --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
  --work-dir "$RUNNER_TEMP/android-bridged-nested-returns"
python scripts/test_mobile_android_bridged_nested_returns.py --dependency-parent \
  --neverd build-mobile/bin/neverd --d8 "$NEVERD_D8" \
  --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
  --work-dir "$RUNNER_TEMP/android-bridged-nested-returns-declarations"
python scripts/test_mobile_android_nested_return.py \
  --neverd build-mobile/bin/neverd --d8 "$NEVERD_D8" \
  --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
  --work-dir "$RUNNER_TEMP/android-nested-return-variance"
python scripts/test_mobile_android_nested_nominal_return.py \
  --neverd build-mobile/bin/neverd --d8 "$NEVERD_D8" \
  --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
  --work-dir "$RUNNER_TEMP/android-nested-nominal-return"
python scripts/test_mobile_android_nested_return.py --class-ancestor \
  --neverd build-mobile/bin/neverd --d8 "$NEVERD_D8" \
  --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
  --work-dir "$RUNNER_TEMP/android-nested-return-class-ancestor"
python scripts/test_mobile_android_nested_return.py --dependency-root \
  --neverd build-mobile/bin/neverd --d8 "$NEVERD_D8" \
  --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
  --work-dir "$RUNNER_TEMP/android-nested-return-dependency-root"
python scripts/test_mobile_android_throwing_bridge.py \
  --neverd build-mobile/bin/neverd --d8 "$NEVERD_D8" \
  --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
  --work-dir "$RUNNER_TEMP/android-terminal-throw-bridge"
python scripts/test_mobile_android_indexed_throwing_bridges.py \
  --neverd build-mobile/bin/neverd --d8 "$NEVERD_D8" \
  --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
  --work-dir "$RUNNER_TEMP/android-indexed-terminal-throw-bridges"
python scripts/test_mobile_android_indexed_throwing_bridges.py --plain-superclass \
  --neverd build-mobile/bin/neverd --d8 "$NEVERD_D8" \
  --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
  --work-dir "$RUNNER_TEMP/android-plain-superclass-terminal-throw-bridges"
python scripts/test_mobile_android_enums.py \
  --neverd build-mobile/bin/neverd --d8 "$NEVERD_D8" \
  --work-dir "$RUNNER_TEMP/android-enums"
python scripts/test_mobile_android_enums.py --named-constant \
  --neverd build-mobile/bin/neverd --d8 "$NEVERD_D8" \
  --work-dir "$RUNNER_TEMP/android-enum-named-constants"
python scripts/test_mobile_android_enums.py --constant-annotations \
  --neverd build-mobile/bin/neverd --d8 "$NEVERD_D8" \
  --work-dir "$RUNNER_TEMP/android-enum-constant-annotations"
python scripts/test_mobile_android_enums.py --recursive-bounds \
  --neverd build-mobile/bin/neverd --d8 "$NEVERD_D8" \
  --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
  --work-dir "$RUNNER_TEMP/android-enum-recursive-bounds"
python scripts/test_mobile_android_enum_arrays.py \
  --neverd build-mobile/bin/neverd \
  --work-dir "$RUNNER_TEMP/android-enum-arrays"
python scripts/test_mobile_android_enum_arrays.py --computed-arguments \
  --neverd build-mobile/bin/neverd \
  --work-dir "$RUNNER_TEMP/android-enum-arguments"
python scripts/test_mobile_android_enum_arrays.py --initializer-loop \
  --neverd build-mobile/bin/neverd \
  --work-dir "$RUNNER_TEMP/android-enum-loops"
python scripts/test_mobile_android_enum_arrays.py --wide-arguments \
  --neverd build-mobile/bin/neverd \
  --work-dir "$RUNNER_TEMP/android-enum-wide-arguments"
python scripts/test_mobile_android_enum_arrays.py --literal-factories \
  --neverd build-mobile/bin/neverd \
  --work-dir "$RUNNER_TEMP/android-enum-literal-factories"
python scripts/test_mobile_android_enum_arrays.py --encoded-constants \
  --neverd build-mobile/bin/neverd \
  --work-dir "$RUNNER_TEMP/android-enum-encoded-constants"
python scripts/test_mobile_android_enum_arrays.py --delegating-constructors \
  --neverd build-mobile/bin/neverd \
  --work-dir "$RUNNER_TEMP/android-enum-delegating-constructors"
python scripts/test_mobile_android_enum_arrays.py --object-arguments \
  --neverd build-mobile/bin/neverd \
  --work-dir "$RUNNER_TEMP/android-enum-object-arguments"
python scripts/test_mobile_android_enum_arrays.py --object-constructor-arguments \
  --neverd build-mobile/bin/neverd \
  --work-dir "$RUNNER_TEMP/android-enum-object-constructor-arguments"
python scripts/test_mobile_android_enum_arrays.py --conditional-constructor \
  --neverd build-mobile/bin/neverd \
  --work-dir "$RUNNER_TEMP/android-enum-conditional-constructor"
python scripts/test_mobile_android_enum_arrays.py --constructor-handlers \
  --neverd build-mobile/bin/neverd \
  --work-dir "$RUNNER_TEMP/android-enum-constructor-handlers"
python scripts/test_mobile_android_enum_arrays.py --default-constructors \
  --neverd build-mobile/bin/neverd \
  --work-dir "$RUNNER_TEMP/android-enum-default-constructors"

profile_group "Execute signed array selection inside constructor arguments" 21 300
python scripts/test_mobile_android_conditional_array_allocation.py \
  --neverd build-mobile/bin/neverd --d8 "$NEVERD_D8" --java-home "$JAVA_HOME" \
  --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
  --work-dir "$RUNNER_TEMP/android-conditional-array-allocation"
python scripts/test_mobile_android_conditional_array_allocation.py --dependency-provider --dependency-target \
  --neverd build-mobile/bin/neverd --d8 "$NEVERD_D8" --java-home "$JAVA_HOME" \
  --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
  --work-dir "$RUNNER_TEMP/android-conditional-array-allocation-sdk"

profile_group "Execute original static integers inside allocation arguments" 21 300
python scripts/test_mobile_android_allocation_static_integer.py \
  --neverd build-mobile/bin/neverd --d8 "$NEVERD_D8" --java-home "$JAVA_HOME" \
  --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
  --work-dir "$RUNNER_TEMP/android-allocation-static-integer"
python scripts/test_mobile_android_allocation_static_integer.py --dependency-provider \
  --neverd build-mobile/bin/neverd --d8 "$NEVERD_D8" --java-home "$JAVA_HOME" \
  --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
  --work-dir "$RUNNER_TEMP/android-allocation-static-integer-sdk"
python scripts/test_mobile_android_allocation_static_integer.py --field-first \
  --neverd build-mobile/bin/neverd --d8 "$NEVERD_D8" --java-home "$JAVA_HOME" \
  --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
  --work-dir "$RUNNER_TEMP/android-allocation-static-integer-first"
python scripts/test_mobile_android_allocation_static_integer.py --field-first --dependency-provider \
  --neverd build-mobile/bin/neverd --d8 "$NEVERD_D8" --java-home "$JAVA_HOME" \
  --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
  --work-dir "$RUNNER_TEMP/android-allocation-static-integer-first-sdk"

profile_group "Execute total integer arithmetic inside allocation arguments" 21 300
python scripts/test_mobile_android_allocation_integer_arithmetic.py \
  --neverd build-mobile/bin/neverd --d8 "$NEVERD_D8" --java-home "$JAVA_HOME" \
  --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
  --work-dir "$RUNNER_TEMP/android-allocation-integer-arithmetic"
python scripts/test_mobile_android_allocation_integer_arithmetic.py --dependency-target \
  --neverd build-mobile/bin/neverd --d8 "$NEVERD_D8" --java-home "$JAVA_HOME" \
  --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
  --work-dir "$RUNNER_TEMP/android-allocation-integer-arithmetic-sdk"

profile_group "Execute nullable array joins and original loop loads" 21 300
python scripts/test_mobile_android_nullable_array_loop.py \
  --neverd build-mobile/bin/neverd --d8 "$NEVERD_D8" --java-home "$JAVA_HOME" \
  --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
  --work-dir "$RUNNER_TEMP/android-nullable-array-loop"
python scripts/test_mobile_android_nullable_array_loop.py --dependency-provider \
  --neverd build-mobile/bin/neverd --d8 "$NEVERD_D8" --java-home "$JAVA_HOME" \
  --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
  --work-dir "$RUNNER_TEMP/android-nullable-array-loop-sdk"

profile_group "Preserve contextual member names and qualified dispatch" 21 300
python scripts/test_mobile_android_contextual_member_names.py \
  --neverd build-mobile/bin/neverd --d8 "$NEVERD_D8" --java-home "$JAVA_HOME" \
  --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
  --work-dir "$RUNNER_TEMP/android-contextual-member-names"
python scripts/test_mobile_android_contextual_member_names.py --dependency-provider \
  --neverd build-mobile/bin/neverd --d8 "$NEVERD_D8" --java-home "$JAVA_HOME" \
  --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
  --work-dir "$RUNNER_TEMP/android-contextual-member-names-sdk"

profile_group "Execute short circuit guards sharing final initializer arms" 21 300
python scripts/test_mobile_android_shared_else_final.py \
  --neverd build-mobile/bin/neverd --d8 "$NEVERD_D8" --java-home "$JAVA_HOME" \
  --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
  --work-dir "$RUNNER_TEMP/android-shared-else-final"
python scripts/test_mobile_android_shared_else_final.py --dependency-provider \
  --neverd build-mobile/bin/neverd --d8 "$NEVERD_D8" --java-home "$JAVA_HOME" \
  --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
  --work-dir "$RUNNER_TEMP/android-shared-else-final-sdk"

profile_group "Execute integer parameters through static superclass accessors" 21 300
python scripts/test_mobile_android_static_super_accessors.py --integer-argument \
  --neverd build-mobile/bin/neverd --d8 "$NEVERD_D8" --java-home "$JAVA_HOME" \
  --kotlin-lib "$RUNNER_TEMP/neverd-kotlin-2.0.21/kotlinc/lib" \
  --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
  --work-dir "$RUNNER_TEMP/android-static-super-integer"
python scripts/test_mobile_android_static_super_accessors.py --integer-argument --dependency-parent \
  --neverd build-mobile/bin/neverd --d8 "$NEVERD_D8" --java-home "$JAVA_HOME" \
  --kotlin-lib "$RUNNER_TEMP/neverd-kotlin-2.0.21/kotlinc/lib" \
  --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
  --work-dir "$RUNNER_TEMP/android-static-super-integer-sdk"

profile_group "Execute original object results through static superclass accessors" 21 300
python scripts/test_mobile_android_static_super_accessors.py --object-result \
  --neverd build-mobile/bin/neverd --d8 "$NEVERD_D8" --java-home "$JAVA_HOME" \
  --kotlin-lib "$RUNNER_TEMP/neverd-kotlin-2.0.21/kotlinc/lib" \
  --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
  --work-dir "$RUNNER_TEMP/android-static-super-results"
python scripts/test_mobile_android_static_super_accessors.py --object-result --dependency-parent \
  --neverd build-mobile/bin/neverd --d8 "$NEVERD_D8" --java-home "$JAVA_HOME" \
  --kotlin-lib "$RUNNER_TEMP/neverd-kotlin-2.0.21/kotlinc/lib" \
  --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
  --work-dir "$RUNNER_TEMP/android-static-super-results-sdk"

profile_group "Execute private instance calls inside allocation argument graphs" 21 300
python scripts/test_mobile_android_allocation_private_calls.py \
  --neverd build-mobile/bin/neverd --d8 "$NEVERD_D8" --java-home "$JAVA_HOME" \
  --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
  --work-dir "$RUNNER_TEMP/android-allocation-private-calls"
python scripts/test_mobile_android_allocation_private_calls.py --dependency-target \
  --neverd build-mobile/bin/neverd --d8 "$NEVERD_D8" --java-home "$JAVA_HOME" \
  --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
  --work-dir "$RUNNER_TEMP/android-allocation-private-calls-sdk"

profile_group "Execute blank final initialization stages around catch and finally" 21 300
python scripts/test_mobile_android_protected_final_stages.py \
  --neverd build-mobile/bin/neverd --d8 "$NEVERD_D8" --java-home "$JAVA_HOME" \
  --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
  --work-dir "$RUNNER_TEMP/android-protected-final-stages"
python scripts/test_mobile_android_protected_final_stages.py --dependency-provider \
  --neverd build-mobile/bin/neverd --d8 "$NEVERD_D8" --java-home "$JAVA_HOME" \
  --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
  --work-dir "$RUNNER_TEMP/android-protected-final-stages-sdk"

profile_group "Execute protected preparation before conditional final assignments" 21 300
python scripts/test_mobile_android_protected_final_suffix.py \
  --neverd build-mobile/bin/neverd --d8 "$NEVERD_D8" --java-home "$JAVA_HOME" \
  --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
  --work-dir "$RUNNER_TEMP/android-protected-final-suffix"
python scripts/test_mobile_android_protected_final_suffix.py --dependency-provider \
  --neverd build-mobile/bin/neverd --d8 "$NEVERD_D8" --java-home "$JAVA_HOME" \
  --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
  --work-dir "$RUNNER_TEMP/android-protected-final-suffix-sdk"

profile_group "Execute original void bridges through source superclass edges" 21 300
python scripts/test_mobile_android_literal_superclass_bridges.py \
  --neverd build-mobile/bin/neverd --d8 "$NEVERD_D8" --java-home "$JAVA_HOME" \
  --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
  --work-dir "$RUNNER_TEMP/android-literal-superclass-bridges"

profile_group "Execute original void bridges through frozen superclass edges" 21 300
python scripts/test_mobile_android_literal_superclass_bridges.py --dependency-parents \
  --neverd build-mobile/bin/neverd --d8 "$NEVERD_D8" --java-home "$JAVA_HOME" \
  --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
  --work-dir "$RUNNER_TEMP/android-literal-superclass-bridges-sdk"

profile_group "Verify final synthetic instance names with source base" 21 300
python scripts/test_mobile_android_final_instance_names.py \
  --neverd build-mobile/bin/neverd --d8 "$NEVERD_D8" --java-home "$JAVA_HOME" \
  --kotlin-lib "$RUNNER_TEMP/neverd-kotlin-2.0.21/kotlinc/lib" \
  --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
  --work-dir "$RUNNER_TEMP/android-final-synthetic-instance-names"

profile_group "Verify final synthetic instance names with frozen base" 21 300
python scripts/test_mobile_android_final_instance_names.py --dependency-base \
  --neverd build-mobile/bin/neverd --d8 "$NEVERD_D8" --java-home "$JAVA_HOME" \
  --kotlin-lib "$RUNNER_TEMP/neverd-kotlin-2.0.21/kotlinc/lib" \
  --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
  --work-dir "$RUNNER_TEMP/android-final-synthetic-instance-names-sdk"

profile_group "Verify final ordinary instance names with source base" 21 300
python scripts/test_mobile_android_final_instance_names.py --ordinary \
  --neverd build-mobile/bin/neverd --d8 "$NEVERD_D8" --java-home "$JAVA_HOME" \
  --kotlin-lib "$RUNNER_TEMP/neverd-kotlin-2.0.21/kotlinc/lib" \
  --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
  --work-dir "$RUNNER_TEMP/android-final-ordinary-instance-names"

profile_group "Verify final ordinary instance names with frozen base" 21 300
python scripts/test_mobile_android_final_instance_names.py --ordinary --dependency-base \
  --neverd build-mobile/bin/neverd --d8 "$NEVERD_D8" --java-home "$JAVA_HOME" \
  --kotlin-lib "$RUNNER_TEMP/neverd-kotlin-2.0.21/kotlinc/lib" \
  --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
  --work-dir "$RUNNER_TEMP/android-final-ordinary-instance-names-sdk"

profile_group "Execute protected allocation catch and finally cleanup" 21 300
python scripts/test_mobile_android_protected_allocation_cleanup.py \
  --neverd build-mobile/bin/neverd --d8 "$NEVERD_D8" --java-home "$JAVA_HOME" \
  --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
  --work-dir "$RUNNER_TEMP/android-protected-allocation-cleanup"
python scripts/test_mobile_android_protected_allocation_cleanup.py --dependency-provider \
  --neverd build-mobile/bin/neverd --d8 "$NEVERD_D8" --java-home "$JAVA_HOME" \
  --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
  --work-dir "$RUNNER_TEMP/android-protected-allocation-cleanup-sdk"

profile_group "Execute scalar allocation results and monitor cleanup" 21 300
python scripts/test_mobile_android_allocation_scalar_results.py \
  --neverd build-mobile/bin/neverd --d8 "$NEVERD_D8" --java-home "$JAVA_HOME" \
  --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
  --work-dir "$RUNNER_TEMP/android-allocation-scalar-results"
python scripts/test_mobile_android_allocation_scalar_results.py --dependency-provider \
  --neverd build-mobile/bin/neverd --d8 "$NEVERD_D8" --java-home "$JAVA_HOME" \
  --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
  --work-dir "$RUNNER_TEMP/android-allocation-scalar-results-sdk"

profile_group "Execute allocation scalar fields and numeric conversions" 21 300
python scripts/test_mobile_android_allocation_scalar_conversions.py \
  --neverd build-mobile/bin/neverd --d8 "$NEVERD_D8" --java-home "$JAVA_HOME" \
  --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
  --work-dir "$RUNNER_TEMP/android-allocation-scalar-conversions"
python scripts/test_mobile_android_allocation_scalar_conversions.py --dependency-provider \
  --neverd build-mobile/bin/neverd --d8 "$NEVERD_D8" --java-home "$JAVA_HOME" \
  --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
  --work-dir "$RUNNER_TEMP/android-allocation-scalar-conversions-sdk"

profile_group "Execute monitor bypass tails with source callbacks" 21 300
python scripts/test_mobile_android_monitor_bypass_tails.py \
  --neverd build-mobile/bin/neverd --d8 "$NEVERD_D8" --java-home "$JAVA_HOME" \
  --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
  --work-dir "$RUNNER_TEMP/android-monitor-bypass-tails"

profile_group "Execute monitor bypass tails with frozen callbacks" 21 300
python scripts/test_mobile_android_monitor_bypass_tails.py --dependency-trace \
  --neverd build-mobile/bin/neverd --d8 "$NEVERD_D8" --java-home "$JAVA_HOME" \
  --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
  --work-dir "$RUNNER_TEMP/android-monitor-bypass-tails-sdk"

profile_group "Execute package generic defaults with immutable callers" 21 300
python scripts/test_mobile_android_package_generic_defaults.py \
  --neverd build-mobile/bin/neverd --d8 "$NEVERD_D8" --java-home "$JAVA_HOME" \
  --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
  --work-dir "$RUNNER_TEMP/android-package-generic-defaults"

profile_group "Execute seeded generic scalar results with source providers" 21 300
python scripts/test_mobile_android_seeded_scalar_carrier.py \
  --neverd build-mobile/bin/neverd --d8 "$NEVERD_D8" --java-home "$JAVA_HOME" \
  --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
  --work-dir "$RUNNER_TEMP/android-seeded-scalar-carrier"

profile_group "Execute seeded generic scalar results with frozen providers" 21 300
python scripts/test_mobile_android_seeded_scalar_carrier.py --dependency-provider \
  --neverd build-mobile/bin/neverd --d8 "$NEVERD_D8" --java-home "$JAVA_HOME" \
  --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
  --work-dir "$RUNNER_TEMP/android-seeded-scalar-carrier-sdk"

profile_group "Execute raw void carriers with recursive Comparable bounds" 21 300
python scripts/test_mobile_android_recursive_void_carrier.py \
  --neverd build-mobile/bin/neverd --d8 "$NEVERD_D8" --java-home "$JAVA_HOME" \
  --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
  --work-dir "$RUNNER_TEMP/android-recursive-void-carrier"
python scripts/test_mobile_android_recursive_void_carrier.py --dependency-provider \
  --neverd build-mobile/bin/neverd --d8 "$NEVERD_D8" --java-home "$JAVA_HOME" \
  --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
  --work-dir "$RUNNER_TEMP/android-recursive-void-carrier-sdk"

profile_group "Verify final array payloads and initializer alignment" 21 300
python scripts/test_mobile_android_final_array_payload.py \
  --neverd build-mobile/bin/neverd --d8 "$NEVERD_D8" --java-home "$JAVA_HOME" \
  --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
  --work-dir "$RUNNER_TEMP/android-final-array-payload"


profile_group "Execute encoded constructor fields and captured covariance" 25 600
python scripts/test_mobile_android_constructor_encoded_fields.py \
  --neverd build-mobile/bin/neverd --d8 "$NEVERD_D8" --java-home "$JAVA_HOME" \
  --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
  --work-dir "$RUNNER_TEMP/android-constructor-encoded-fields"
python scripts/test_mobile_android_constructor_encoded_fields.py --dependency-provider \
  --neverd build-mobile/bin/neverd --d8 "$NEVERD_D8" --java-home "$JAVA_HOME" \
  --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
  --work-dir "$RUNNER_TEMP/android-constructor-encoded-fields-sdk"
python scripts/test_mobile_android_captured_covariant_bridges.py \
  --neverd build-mobile/bin/neverd --d8 "$NEVERD_D8" --java-home "$JAVA_HOME" \
  --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
  --work-dir "$RUNNER_TEMP/android-captured-covariant-bridges"
python scripts/test_mobile_android_captured_covariant_bridges.py --dependency-provider \
  --neverd build-mobile/bin/neverd --d8 "$NEVERD_D8" --java-home "$JAVA_HOME" \
  --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
  --work-dir "$RUNNER_TEMP/android-captured-covariant-bridges-sdk"

profile_group "Execute inherited covariant runtime slots" 25 600
python scripts/test_mobile_android_inherited_covariant_bridge_slots.py \
  --neverd build-mobile/bin/neverd --d8 "$NEVERD_D8" --java-home "$JAVA_HOME" \
  --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
  --work-dir "$RUNNER_TEMP/android-inherited-covariant-slots"
python scripts/test_mobile_android_inherited_covariant_bridge_slots.py --dependency-provider \
  --neverd build-mobile/bin/neverd --d8 "$NEVERD_D8" --java-home "$JAVA_HOME" \
  --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
  --work-dir "$RUNNER_TEMP/android-inherited-covariant-slots-sdk"

profile_group "Verify closed checked exception subtype narrowing" 25 300
python scripts/test_mobile_android_scoped_unchecked_exception_narrowing.py --checked-exceptions \
  --neverd build-mobile/bin/neverd --d8 "$NEVERD_D8" --java-home "$JAVA_HOME" \
  --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
  --work-dir "$RUNNER_TEMP/android-scoped-checked-exception-narrowing"
python scripts/test_mobile_android_scoped_unchecked_exception_narrowing.py --checked-exceptions --dependency-root \
  --neverd build-mobile/bin/neverd --d8 "$NEVERD_D8" --java-home "$JAVA_HOME" \
  --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
  --work-dir "$RUNNER_TEMP/android-scoped-checked-exception-narrowing-declarations"

profile_group "Execute original constructor class literals" 25 600
python scripts/test_mobile_android_constructor_class_literals.py \
  --neverd build-mobile/bin/neverd --d8 "$NEVERD_D8" --java-home "$JAVA_HOME" \
  --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
  --work-dir "$RUNNER_TEMP/android-constructor-class-literals"
python scripts/test_mobile_android_constructor_class_literals.py --dependency-provider \
  --neverd build-mobile/bin/neverd --d8 "$NEVERD_D8" --java-home "$JAVA_HOME" \
  --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
  --work-dir "$RUNNER_TEMP/android-constructor-class-literals-sdk"

profile_group "Execute linear constructor delegation continuations" 25 1200
python scripts/test_mobile_android_delegation_continuations.py \
  --neverd build-mobile/bin/neverd --java-home "$JAVA_HOME" --d8 "$NEVERD_D8" \
  --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
  --work-dir "$RUNNER_TEMP/android-delegation-continuations"
python scripts/test_mobile_android_delegation_continuations.py --dependency-provider \
  --neverd build-mobile/bin/neverd --java-home "$JAVA_HOME" --d8 "$NEVERD_D8" \
  --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
  --work-dir "$RUNNER_TEMP/android-delegation-continuations-sdk"

profile_group "Execute allocation field aliases and raw scalar copies" 25 600
python scripts/test_mobile_android_allocation_field_aliases.py \
  --neverd build-mobile/bin/neverd --java-home "$JAVA_HOME" --d8 "$NEVERD_D8" \
  --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
  --work-dir "$RUNNER_TEMP/android-allocation-field-aliases"
python scripts/test_mobile_android_allocation_field_aliases.py --dependency-provider \
  --neverd build-mobile/bin/neverd --java-home "$JAVA_HOME" --d8 "$NEVERD_D8" \
  --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
  --work-dir "$RUNNER_TEMP/android-allocation-field-aliases-sdk"

profile_group "Execute shared access rules across allocation argument paths" 25 1200
for access in package protected; do
  python scripts/test_mobile_android_allocation_static_calls.py --call-access "$access" \
    --neverd build-mobile/bin/neverd --java-home "$JAVA_HOME" --d8 "$NEVERD_D8" \
    --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
    --work-dir "$RUNNER_TEMP/android-allocation-static-calls-$access"
  python scripts/test_mobile_android_allocation_static_calls.py --call-access "$access" --dependency-provider \
    --neverd build-mobile/bin/neverd --java-home "$JAVA_HOME" --d8 "$NEVERD_D8" \
    --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
    --work-dir "$RUNNER_TEMP/android-allocation-static-calls-$access-sdk"
  python scripts/test_mobile_android_conditional_allocation.py --constructor-access "$access" \
    --neverd build-mobile/bin/neverd --java-home "$JAVA_HOME" --d8 "$NEVERD_D8" \
    --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
    --work-dir "$RUNNER_TEMP/android-conditional-allocation-$access"
  python scripts/test_mobile_android_conditional_allocation.py --constructor-access "$access" --dependency-provider --dependency-target \
    --neverd build-mobile/bin/neverd --java-home "$JAVA_HOME" --d8 "$NEVERD_D8" \
    --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
    --work-dir "$RUNNER_TEMP/android-conditional-allocation-$access-sdk"
done

profile_group "Execute exact superclass calls inside allocation arguments" 25 1200
python scripts/test_mobile_android_allocation_super_calls.py \
  --neverd build-mobile/bin/neverd --java-home "$JAVA_HOME" --d8 "$NEVERD_D8" \
  --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
  --work-dir "$RUNNER_TEMP/android-allocation-super-calls"
python scripts/test_mobile_android_allocation_super_calls.py --dependency-provider \
  --neverd build-mobile/bin/neverd --java-home "$JAVA_HOME" --d8 "$NEVERD_D8" \
  --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
  --work-dir "$RUNNER_TEMP/android-allocation-super-calls-sdk"

profile_group "Execute chained fields and int allocation arguments" 25 1200
python scripts/test_mobile_android_allocation_field_chains.py \
  --neverd build-mobile/bin/neverd --java-home "$JAVA_HOME" --d8 "$NEVERD_D8" \
  --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
  --work-dir "$RUNNER_TEMP/android-allocation-field-chains"
python scripts/test_mobile_android_allocation_field_chains.py --dependency-provider \
  --neverd build-mobile/bin/neverd --java-home "$JAVA_HOME" --d8 "$NEVERD_D8" \
  --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
  --work-dir "$RUNNER_TEMP/android-allocation-field-chains-sdk"

profile_group "Execute boolean fields inside allocation expressions" 25 1200
python scripts/test_mobile_android_allocation_field_chains.py --boolean-fields \
  --neverd build-mobile/bin/neverd --java-home "$JAVA_HOME" --d8 "$NEVERD_D8" \
  --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
  --work-dir "$RUNNER_TEMP/android-allocation-field-chains-boolean"
python scripts/test_mobile_android_allocation_field_chains.py --boolean-fields --dependency-provider \
  --neverd build-mobile/bin/neverd --java-home "$JAVA_HOME" --d8 "$NEVERD_D8" \
  --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
  --work-dir "$RUNNER_TEMP/android-allocation-field-chains-boolean-sdk"

profile_group "Execute array lengths inside original allocation expressions" 25 600
python scripts/test_mobile_android_allocation_array_lengths.py \
  --neverd build-mobile/bin/neverd --java-home "$JAVA_HOME" --d8 "$NEVERD_D8" \
  --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
  --work-dir "$RUNNER_TEMP/android-allocation-array-lengths"
python scripts/test_mobile_android_allocation_array_lengths.py --dependency-provider \
  --neverd build-mobile/bin/neverd --java-home "$JAVA_HOME" --d8 "$NEVERD_D8" \
  --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
  --work-dir "$RUNNER_TEMP/android-allocation-array-lengths-sdk"

profile_group "Execute array creation and stores inside original allocation expressions" 25 1200
python scripts/test_mobile_android_allocation_array_arguments.py \
  --neverd build-mobile/bin/neverd --java-home "$JAVA_HOME" --d8 "$NEVERD_D8" \
  --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
  --work-dir "$RUNNER_TEMP/android-allocation-array-arguments"
python scripts/test_mobile_android_allocation_array_arguments.py --dependency-provider \
  --neverd build-mobile/bin/neverd --java-home "$JAVA_HOME" --d8 "$NEVERD_D8" \
  --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
  --work-dir "$RUNNER_TEMP/android-allocation-array-arguments-sdk"

profile_group "Execute nested final assignments with original callback observations" 25 600
python scripts/test_mobile_android_nested_final_branches.py \
  --neverd build-mobile/bin/neverd --java-home "$JAVA_HOME" --d8 "$NEVERD_D8" \
  --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
  --work-dir "$RUNNER_TEMP/android-nested-final-branches"
python scripts/test_mobile_android_nested_final_branches.py --dependency-provider \
  --neverd build-mobile/bin/neverd --java-home "$JAVA_HOME" --d8 "$NEVERD_D8" \
  --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
  --work-dir "$RUNNER_TEMP/android-nested-final-branches-sdk"

profile_group "Execute nullary generic factories with original effects and results" 25 600
python scripts/test_mobile_android_nullary_carrier_factories.py \
  --neverd build-mobile/bin/neverd --java-home "$JAVA_HOME" --d8 "$NEVERD_D8" \
  --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
  --work-dir "$RUNNER_TEMP/android-nullary-carrier-factories"
python scripts/test_mobile_android_nullary_carrier_factories.py --dependency-provider \
  --neverd build-mobile/bin/neverd --java-home "$JAVA_HOME" --d8 "$NEVERD_D8" \
  --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
  --work-dir "$RUNNER_TEMP/android-nullary-carrier-factories-sdk"
python scripts/test_mobile_android_nullary_carrier_factories.py --paired-result \
  --neverd build-mobile/bin/neverd --java-home "$JAVA_HOME" --d8 "$NEVERD_D8" \
  --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
  --work-dir "$RUNNER_TEMP/android-nullary-carrier-factories-paired"
python scripts/test_mobile_android_nullary_carrier_factories.py --paired-result --dependency-provider \
  --neverd build-mobile/bin/neverd --java-home "$JAVA_HOME" --d8 "$NEVERD_D8" \
  --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
  --work-dir "$RUNNER_TEMP/android-nullary-carrier-factories-paired-sdk"

profile_group "Execute generic factories with two independent seed arguments" 25 600
python scripts/test_mobile_android_paired_seed_factories.py \
  --neverd build-mobile/bin/neverd --java-home "$JAVA_HOME" --d8 "$NEVERD_D8" \
  --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
  --work-dir "$RUNNER_TEMP/android-paired-seed-factories"
python scripts/test_mobile_android_paired_seed_factories.py --dependency-provider \
  --neverd build-mobile/bin/neverd --java-home "$JAVA_HOME" --d8 "$NEVERD_D8" \
  --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
  --work-dir "$RUNNER_TEMP/android-paired-seed-factories-sdk"

profile_group "Execute generic array factories with original stores and results" 25 600
python scripts/test_mobile_android_array_seed_factories.py \
  --neverd build-mobile/bin/neverd --java-home "$JAVA_HOME" --d8 "$NEVERD_D8" \
  --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
  --work-dir "$RUNNER_TEMP/android-array-seed-factories"
python scripts/test_mobile_android_array_seed_factories.py --dependency-provider \
  --neverd build-mobile/bin/neverd --java-home "$JAVA_HOME" --d8 "$NEVERD_D8" \
  --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
  --work-dir "$RUNNER_TEMP/android-array-seed-factories-sdk"

profile_group "Execute recursively bounded enum arrays and original carrier results" 25 600
python scripts/test_mobile_android_enum_array_factories.py \
  --neverd build-mobile/bin/neverd --java-home "$JAVA_HOME" --d8 "$NEVERD_D8" \
  --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
  --work-dir "$RUNNER_TEMP/android-enum-array-factories"
python scripts/test_mobile_android_enum_array_factories.py --dependency-provider \
  --neverd build-mobile/bin/neverd --java-home "$JAVA_HOME" --d8 "$NEVERD_D8" \
  --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
  --work-dir "$RUNNER_TEMP/android-enum-array-factories-sdk"

profile_group "Execute independent generic inputs and nested pair results" 25 600
python scripts/test_mobile_android_nested_pair_factories.py \
  --neverd build-mobile/bin/neverd --java-home "$JAVA_HOME" --d8 "$NEVERD_D8" \
  --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
  --work-dir "$RUNNER_TEMP/android-nested-pair-factories"
python scripts/test_mobile_android_nested_pair_factories.py --dependency-provider \
  --neverd build-mobile/bin/neverd --java-home "$JAVA_HOME" --d8 "$NEVERD_D8" \
  --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
  --work-dir "$RUNNER_TEMP/android-nested-pair-factories-sdk"

profile_group "Execute nested generic pair inputs with original iterator effects" 25 600
python scripts/test_mobile_android_nested_pair_consumers.py \
  --neverd build-mobile/bin/neverd --java-home "$JAVA_HOME" --d8 "$NEVERD_D8" \
  --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
  --work-dir "$RUNNER_TEMP/android-nested-pair-consumers"
python scripts/test_mobile_android_nested_pair_consumers.py --dependency-provider \
  --neverd build-mobile/bin/neverd --java-home "$JAVA_HOME" --d8 "$NEVERD_D8" \
  --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
  --work-dir "$RUNNER_TEMP/android-nested-pair-consumers-sdk"

profile_group "Execute paired generic void carriers and abstract dispatch" 25 600
python scripts/test_mobile_android_paired_void_carriers.py \
  --neverd build-mobile/bin/neverd --java-home "$JAVA_HOME" --d8 "$NEVERD_D8" \
  --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
  --work-dir "$RUNNER_TEMP/android-paired-void-carriers"
python scripts/test_mobile_android_paired_void_carriers.py --dependency-provider \
  --neverd build-mobile/bin/neverd --java-home "$JAVA_HOME" --d8 "$NEVERD_D8" \
  --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
  --work-dir "$RUNNER_TEMP/android-paired-void-carriers-sdk"

profile_group "Execute private instance void calls and private member carriers" 25 600
python scripts/test_mobile_android_private_instance_carriers.py \
  --neverd build-mobile/bin/neverd --java-home "$JAVA_HOME" --d8 "$NEVERD_D8" \
  --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
  --work-dir "$RUNNER_TEMP/android-private-instance-carriers"
python scripts/test_mobile_android_private_instance_carriers.py --dependency-provider \
  --neverd build-mobile/bin/neverd --java-home "$JAVA_HOME" --d8 "$NEVERD_D8" \
  --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
  --work-dir "$RUNNER_TEMP/android-private-instance-carriers-sdk"

profile_group "Execute independent unused static method formals" 25 600
python scripts/test_mobile_android_unused_formal_invocations.py \
  --neverd build-mobile/bin/neverd --java-home "$JAVA_HOME" --d8 "$NEVERD_D8" \
  --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
  --work-dir "$RUNNER_TEMP/android-unused-formal-invocations"
python scripts/test_mobile_android_unused_formal_invocations.py --dependency-provider \
  --neverd build-mobile/bin/neverd --java-home "$JAVA_HOME" --d8 "$NEVERD_D8" \
  --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
  --work-dir "$RUNNER_TEMP/android-unused-formal-invocations-sdk"

profile_group "Execute contravariant carrier parameters and invariant returns" 25 600
python scripts/test_mobile_android_contravariant_carrier_returns.py \
  --neverd build-mobile/bin/neverd --java-home "$JAVA_HOME" --d8 "$NEVERD_D8" \
  --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
  --work-dir "$RUNNER_TEMP/android-contravariant-carrier-returns"
python scripts/test_mobile_android_contravariant_carrier_returns.py --dependency-provider \
  --neverd build-mobile/bin/neverd --java-home "$JAVA_HOME" --d8 "$NEVERD_D8" \
  --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
  --work-dir "$RUNNER_TEMP/android-contravariant-carrier-returns-sdk"

profile_group "Execute seeded void callbacks with closed sibling carriers" 25 600
python scripts/test_mobile_android_seeded_void_carriers.py \
  --neverd build-mobile/bin/neverd --java-home "$JAVA_HOME" --d8 "$NEVERD_D8" \
  --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
  --work-dir "$RUNNER_TEMP/android-seeded-void-carriers"
python scripts/test_mobile_android_seeded_void_carriers.py --dependency-provider \
  --neverd build-mobile/bin/neverd --java-home "$JAVA_HOME" --d8 "$NEVERD_D8" \
  --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
  --work-dir "$RUNNER_TEMP/android-seeded-void-carriers-sdk"

profile_group "Execute allocation calls with reused argument registers" 25 1200
python scripts/test_mobile_android_allocation_call_aliases.py \
  --neverd build-mobile/bin/neverd --java-home "$JAVA_HOME" --d8 "$NEVERD_D8" \
  --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
  --work-dir "$RUNNER_TEMP/android-allocation-call-aliases"
python scripts/test_mobile_android_allocation_call_aliases.py --dependency-provider \
  --neverd build-mobile/bin/neverd --java-home "$JAVA_HOME" --d8 "$NEVERD_D8" \
  --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
  --work-dir "$RUNNER_TEMP/android-allocation-call-aliases-sdk"
for access in package protected private; do
  python scripts/test_mobile_android_allocation_call_aliases.py --call-access "$access" \
    --neverd build-mobile/bin/neverd --java-home "$JAVA_HOME" --d8 "$NEVERD_D8" \
    --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
    --work-dir "$RUNNER_TEMP/android-allocation-call-aliases-$access"
  python scripts/test_mobile_android_allocation_call_aliases.py --call-access "$access" --dependency-provider \
    --neverd build-mobile/bin/neverd --java-home "$JAVA_HOME" --d8 "$NEVERD_D8" \
    --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
    --work-dir "$RUNNER_TEMP/android-allocation-call-aliases-$access-sdk"
done

profile_group "Execute same-package allocation instance calls" 25 900
for access in package protected; do
  python scripts/test_mobile_android_allocation_instance_calls.py --call-access "$access" \
    --neverd build-mobile/bin/neverd --java-home "$JAVA_HOME" --d8 "$NEVERD_D8" \
    --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
    --work-dir "$RUNNER_TEMP/android-allocation-instance-calls-$access"
  python scripts/test_mobile_android_allocation_instance_calls.py --call-access "$access" --dependency-provider \
    --neverd build-mobile/bin/neverd --java-home "$JAVA_HOME" --d8 "$NEVERD_D8" \
    --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
    --work-dir "$RUNNER_TEMP/android-allocation-instance-calls-$access-sdk"
done

profile_group "Execute inherited defaults beside projected return names" 25 600
for default_prefix in sdk source; do
  default_flags=()
  if [[ "$default_prefix" == source ]]; then
    default_flags+=(--source-prefix)
  fi
  python scripts/test_mobile_android_plain_sdk_return_slots.py --interface-covariance --inherited-default "${default_flags[@]}" \
    --neverd build-mobile/bin/neverd --java-home "$JAVA_HOME" --d8 "$NEVERD_D8" \
    --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
    --work-dir "$RUNNER_TEMP/android-default-return-slot-names-$default_prefix"
done

profile_group "Execute plain interface covariance bridges" 25 600
for covariance_profile in plain fields interface reabstract; do
  covariance_flags=()
  if [[ "$covariance_profile" != plain ]]; then
    covariance_flags+=(--interface-fields)
  fi
  if [[ "$covariance_profile" == interface || "$covariance_profile" == reabstract ]]; then
    covariance_flags+=(--interface-bridge)
  fi
  if [[ "$covariance_profile" == reabstract ]]; then
    covariance_flags+=(--reabstract-default)
  fi
  python scripts/test_mobile_android_plain_interface_covariance.py "${covariance_flags[@]}" \
    --neverd build-mobile/bin/neverd --java-home "$JAVA_HOME" --d8 "$NEVERD_D8" \
    --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
    --work-dir "$RUNNER_TEMP/android-plain-interface-covariance-$covariance_profile"
  python scripts/test_mobile_android_plain_interface_covariance.py "${covariance_flags[@]}" --dependency-provider \
    --neverd build-mobile/bin/neverd --java-home "$JAVA_HOME" --d8 "$NEVERD_D8" \
    --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
    --work-dir "$RUNNER_TEMP/android-plain-interface-covariance-$covariance_profile-sdk"
done

profile_group "Execute private generic result calls in captured scopes" 25 600
python scripts/test_mobile_android_private_result_invocations.py \
  --neverd build-mobile/bin/neverd --java-home "$JAVA_HOME" --d8 "$NEVERD_D8" \
  --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
  --work-dir "$RUNNER_TEMP/android-private-result-invocations"
python scripts/test_mobile_android_private_result_invocations.py --dependency-provider \
  --neverd build-mobile/bin/neverd --java-home "$JAVA_HOME" --d8 "$NEVERD_D8" \
  --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
  --work-dir "$RUNNER_TEMP/android-private-result-invocations-sdk"

profile_group "Execute protected constant allocation preparations" 25 600
python scripts/test_mobile_android_protected_allocation_preparations.py \
  --neverd build-mobile/bin/neverd --java-home "$JAVA_HOME" --d8 "$NEVERD_D8" \
  --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
  --work-dir "$RUNNER_TEMP/android-protected-allocation-preparations"
python scripts/test_mobile_android_protected_allocation_preparations.py --dependency-provider \
  --neverd build-mobile/bin/neverd --java-home "$JAVA_HOME" --d8 "$NEVERD_D8" \
  --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
  --work-dir "$RUNNER_TEMP/android-protected-allocation-preparations-sdk"

profile_group "Execute captured member allocation argument programs" 25 900
python scripts/test_mobile_android_captured_allocation_arguments.py \
  --neverd build-mobile/bin/neverd --java-home "$JAVA_HOME" --d8 "$NEVERD_D8" \
  --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
  --work-dir "$RUNNER_TEMP/android-captured-allocation-arguments"
python scripts/test_mobile_android_captured_allocation_arguments.py --dependency-provider \
  --neverd build-mobile/bin/neverd --java-home "$JAVA_HOME" --d8 "$NEVERD_D8" \
  --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
  --work-dir "$RUNNER_TEMP/android-captured-allocation-arguments-sdk"

profile_group "Execute nested initialization in conditional allocation arguments" 25 900
python scripts/test_mobile_android_conditional_nested_allocations.py \
  --neverd build-mobile/bin/neverd --java-home "$JAVA_HOME" --d8 "$NEVERD_D8" \
  --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
  --work-dir "$RUNNER_TEMP/android-conditional-nested-allocations"
python scripts/test_mobile_android_conditional_nested_allocations.py --dependency-provider \
  --neverd build-mobile/bin/neverd --java-home "$JAVA_HOME" --d8 "$NEVERD_D8" \
  --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
  --work-dir "$RUNNER_TEMP/android-conditional-nested-allocations-sdk"

profile_group "Execute generic instance calls through exact raw receivers" 25 600
python scripts/test_mobile_android_raw_receiver_invocations.py \
  --neverd build-mobile/bin/neverd --java-home "$JAVA_HOME" --d8 "$NEVERD_D8" \
  --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
  --work-dir "$RUNNER_TEMP/android-raw-receiver-invocations"
python scripts/test_mobile_android_raw_receiver_invocations.py --dependency-provider \
  --neverd build-mobile/bin/neverd --java-home "$JAVA_HOME" --d8 "$NEVERD_D8" \
  --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
  --work-dir "$RUNNER_TEMP/android-raw-receiver-invocations-sdk"

profile_group "Execute captured static superclass accessors with source parents" 25 300
python scripts/test_mobile_android_static_super_accessors.py --captured-member \
  --neverd build-mobile/bin/neverd --d8 "$NEVERD_D8" --java-home "$JAVA_HOME" \
  --kotlin-java-home "$NEVERD_KOTLIN_JAVA_HOME" \
  --kotlin-lib "$RUNNER_TEMP/neverd-kotlin-2.0.21/kotlinc/lib" \
  --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
  --work-dir "$RUNNER_TEMP/android-captured-static-super"

profile_group "Execute captured static superclass accessors with frozen parents" 25 300
python scripts/test_mobile_android_static_super_accessors.py --captured-member --dependency-parent \
  --neverd build-mobile/bin/neverd --d8 "$NEVERD_D8" --java-home "$JAVA_HOME" \
  --kotlin-java-home "$NEVERD_KOTLIN_JAVA_HOME" \
  --kotlin-lib "$RUNNER_TEMP/neverd-kotlin-2.0.21/kotlinc/lib" \
  --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
  --work-dir "$RUNNER_TEMP/android-captured-static-super-sdk"

profile_group "Execute plain superclass calls through captured scopes" 25 600
python scripts/test_mobile_android_plain_super_calls.py \
  --neverd build-mobile/bin/neverd --java-home "$JAVA_HOME" --d8 "$NEVERD_D8" \
  --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
  --work-dir "$RUNNER_TEMP/android-plain-super-calls"
python scripts/test_mobile_android_plain_super_calls.py --dependency-provider \
  --neverd build-mobile/bin/neverd --java-home "$JAVA_HOME" --d8 "$NEVERD_D8" \
  --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
  --work-dir "$RUNNER_TEMP/android-plain-super-calls-sdk"
python scripts/test_mobile_android_plain_super_calls.py --generic-ancestor \
  --neverd build-mobile/bin/neverd --java-home "$JAVA_HOME" --d8 "$NEVERD_D8" \
  --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
  --work-dir "$RUNNER_TEMP/android-plain-super-calls-generic"
python scripts/test_mobile_android_plain_super_calls.py --generic-ancestor --dependency-provider \
  --neverd build-mobile/bin/neverd --java-home "$JAVA_HOME" --d8 "$NEVERD_D8" \
  --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
  --work-dir "$RUNNER_TEMP/android-plain-super-calls-generic-sdk"

profile_group "Execute original Kotlin erased constructor delegations" 25 600
python scripts/test_mobile_android_erased_constructor_delegations.py \
  --neverd build-mobile/bin/neverd --java-home "$JAVA_HOME" --d8 "$NEVERD_D8" \
  --kotlin-java-home "$NEVERD_KOTLIN_JAVA_HOME" \
  --kotlin-lib "$RUNNER_TEMP/neverd-kotlin-2.0.21/kotlinc/lib" \
  --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
  --work-dir "$RUNNER_TEMP/android-erased-constructor-delegations"

profile_group "Execute captured generic superclass constructors" 25 600
python scripts/test_mobile_android_captured_generic_constructor.py \
  --neverd build-mobile/bin/neverd --java-home "$JAVA_HOME" --d8 "$NEVERD_D8" \
  --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
  --work-dir "$RUNNER_TEMP/android-captured-generic-constructor"
python scripts/test_mobile_android_captured_generic_constructor.py --dependency-provider \
  --neverd build-mobile/bin/neverd --java-home "$JAVA_HOME" --d8 "$NEVERD_D8" \
  --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
  --work-dir "$RUNNER_TEMP/android-captured-generic-constructor-sdk"

profile_group "Execute closed array superclass arguments and overload identity" 25 1200
python scripts/test_mobile_android_closed_array_super_constructor.py \
  --neverd build-mobile/bin/neverd --java-home "$JAVA_HOME" --d8 "$NEVERD_D8" \
  --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
  --work-dir "$RUNNER_TEMP/android-closed-array-super-constructor"
python scripts/test_mobile_android_closed_array_super_constructor.py --dependency-provider \
  --neverd build-mobile/bin/neverd --java-home "$JAVA_HOME" --d8 "$NEVERD_D8" \
  --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
  --work-dir "$RUNNER_TEMP/android-closed-array-super-constructor-sdk"

profile_group "Execute original Kotlin nonsynthetic static names with unused formals" 25 600
python scripts/test_mobile_android_unused_formal_names.py \
  --neverd build-mobile/bin/neverd --java-home "$JAVA_HOME" --d8 "$NEVERD_D8" \
  --kotlin-java-home "$NEVERD_KOTLIN_JAVA_HOME" \
  --kotlin-lib "$RUNNER_TEMP/neverd-kotlin-2.0.21/kotlinc/lib" \
  --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
  --work-dir "$RUNNER_TEMP/android-unused-formal-names"
python scripts/test_mobile_android_unused_formal_names.py --dependency-provider \
  --neverd build-mobile/bin/neverd --java-home "$JAVA_HOME" --d8 "$NEVERD_D8" \
  --kotlin-java-home "$NEVERD_KOTLIN_JAVA_HOME" \
  --kotlin-lib "$RUNNER_TEMP/neverd-kotlin-2.0.21/kotlinc/lib" \
  --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
  --work-dir "$RUNNER_TEMP/android-unused-formal-names-sdk"

profile_group "Execute unused method formals with unchanged integer slots" 25 300
python scripts/test_mobile_android_unused_formal_scalar_names.py \
  --neverd build-mobile/bin/neverd --java-home "$JAVA_HOME" --d8 "$NEVERD_D8" \
  --kotlin-java-home "$NEVERD_KOTLIN_JAVA_HOME" \
  --kotlin-lib "$RUNNER_TEMP/neverd-kotlin-2.0.21/kotlinc/lib" \
  --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
  --work-dir "$RUNNER_TEMP/android-unused-formal-scalar-names"
python scripts/test_mobile_android_unused_formal_scalar_names.py --dependency-provider \
  --neverd build-mobile/bin/neverd --java-home "$JAVA_HOME" --d8 "$NEVERD_D8" \
  --kotlin-java-home "$NEVERD_KOTLIN_JAVA_HOME" \
  --kotlin-lib "$RUNNER_TEMP/neverd-kotlin-2.0.21/kotlinc/lib" \
  --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
  --work-dir "$RUNNER_TEMP/android-unused-formal-scalar-names-sdk"

profile_group "Execute original D8 package synthetic instance names" 25 900
python scripts/test_mobile_android_synthetic_instance_names.py \
  --neverd build-mobile/bin/neverd --java-home "$JAVA_HOME" --d8 "$NEVERD_D8" \
  --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
  --work-dir "$RUNNER_TEMP/android-synthetic-instance-names"
python scripts/test_mobile_android_synthetic_instance_names.py --dependency-provider \
  --neverd build-mobile/bin/neverd --java-home "$JAVA_HOME" --d8 "$NEVERD_D8" \
  --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
  --work-dir "$RUNNER_TEMP/android-synthetic-instance-names-sdk"

profile_group "Execute original Kotlin mangled interface and final class dispatch" 25 600
python scripts/test_mobile_android_mangled_interface_names.py \
  --neverd build-mobile/bin/neverd --java-home "$JAVA_HOME" --d8 "$NEVERD_D8" \
  --kotlin-java-home "$NEVERD_KOTLIN_JAVA_HOME" \
  --kotlin-lib "$RUNNER_TEMP/neverd-kotlin-2.0.21/kotlinc/lib" \
  --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
  --work-dir "$RUNNER_TEMP/android-mangled-interface-names"

profile_group "Execute original Kotlin guarded constructor chains" 25 600
python scripts/test_mobile_android_guarded_delegation_chains.py \
  --neverd build-mobile/bin/neverd --java-home "$JAVA_HOME" --d8 "$NEVERD_D8" \
  --kotlin-java-home "$NEVERD_KOTLIN_JAVA_HOME" \
  --kotlin-lib "$RUNNER_TEMP/neverd-kotlin-2.0.21/kotlinc/lib" \
  --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
  --work-dir "$RUNNER_TEMP/android-guarded-delegation-chains"
python scripts/test_mobile_android_guarded_delegation_chains.py --dependency-parent \
  --neverd build-mobile/bin/neverd --java-home "$JAVA_HOME" --d8 "$NEVERD_D8" \
  --kotlin-java-home "$NEVERD_KOTLIN_JAVA_HOME" \
  --kotlin-lib "$RUNNER_TEMP/neverd-kotlin-2.0.21/kotlinc/lib" \
  --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
  --work-dir "$RUNNER_TEMP/android-guarded-delegation-chains-sdk"

profile_group "Execute original Kotlin constructor parameter static reads" 25 600
python scripts/test_mobile_android_constructor_parameter_reads.py \
  --neverd build-mobile/bin/neverd --java-home "$JAVA_HOME" --d8 "$NEVERD_D8" \
  --kotlin-java-home "$NEVERD_KOTLIN_JAVA_HOME" \
  --kotlin-lib "$RUNNER_TEMP/neverd-kotlin-2.0.21/kotlinc/lib" \
  --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
  --work-dir "$RUNNER_TEMP/android-constructor-parameter-reads"
python scripts/test_mobile_android_constructor_parameter_reads.py --dependency-provider \
  --neverd build-mobile/bin/neverd --java-home "$JAVA_HOME" --d8 "$NEVERD_D8" \
  --kotlin-java-home "$NEVERD_KOTLIN_JAVA_HOME" \
  --kotlin-lib "$RUNNER_TEMP/neverd-kotlin-2.0.21/kotlinc/lib" \
  --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
  --work-dir "$RUNNER_TEMP/android-constructor-parameter-reads-sdk"

profile_group "Execute preserved pre-super field initialization" 25 600
python scripts/test_mobile_android_delegation_argument_calls.py \
  --neverd build-mobile/bin/neverd --java-home "$JAVA_HOME" --d8 "$NEVERD_D8" \
  --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
  --work-dir "$RUNNER_TEMP/android-delegation-argument-calls"
python scripts/test_mobile_android_delegation_argument_calls.py --dependency-provider \
  --neverd build-mobile/bin/neverd --java-home "$JAVA_HOME" --d8 "$NEVERD_D8" \
  --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
  --work-dir "$RUNNER_TEMP/android-delegation-argument-calls-sdk"
python scripts/test_mobile_android_nested_capture_allocations.py \
  --neverd build-mobile/bin/neverd --java-home "$JAVA_HOME" --d8 "$NEVERD_D8" \
  --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
  --work-dir "$RUNNER_TEMP/android-nested-capture-allocations"
python scripts/test_mobile_android_nested_capture_allocations.py --dependency-provider \
  --neverd build-mobile/bin/neverd --java-home "$JAVA_HOME" --d8 "$NEVERD_D8" \
  --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
  --work-dir "$RUNNER_TEMP/android-nested-capture-allocations-sdk"
python scripts/test_mobile_android_nonreturning_void_results.py \
  --neverd build-mobile/bin/neverd --java-home "$JAVA_HOME" --d8 "$NEVERD_D8" \
  --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
  --work-dir "$RUNNER_TEMP/android-nonreturning-void-results"
python scripts/test_mobile_android_constructors.py \
  --neverd build-mobile/bin/neverd --java-home "$JAVA_HOME" \
  --work-dir "$RUNNER_TEMP/android-constructor-prologue"
python scripts/test_mobile_android_constructors.py --inner-members \
  --neverd build-mobile/bin/neverd --java-home "$JAVA_HOME" \
  --work-dir "$RUNNER_TEMP/android-inner-members"
python scripts/test_mobile_android_constructors.py --guarded-inner-members \
  --neverd build-mobile/bin/neverd --java-home "$JAVA_HOME" \
  --work-dir "$RUNNER_TEMP/android-guarded-inner-members"
python scripts/test_mobile_android_constructors.py --generic-inner-members \
  --neverd build-mobile/bin/neverd --java-home "$JAVA_HOME" \
  --work-dir "$RUNNER_TEMP/android-generic-inner-members"
python scripts/test_mobile_android_constructors.py --getter-inner-members \
  --neverd build-mobile/bin/neverd --java-home "$JAVA_HOME" \
  --work-dir "$RUNNER_TEMP/android-getter-inner-members"
python scripts/test_mobile_android_constructors.py --delegating-inner-members \
  --neverd build-mobile/bin/neverd --java-home "$JAVA_HOME" \
  --work-dir "$RUNNER_TEMP/android-delegating-inner-members"
python scripts/test_mobile_android_guarded_constructor_captures.py \
  --neverd build-mobile/bin/neverd --java-home "$JAVA_HOME" \
  --work-dir "$RUNNER_TEMP/android-guarded-constructor-captures"
python scripts/test_mobile_android_indexed_throwing_bridges.py --captured-member \
  --neverd build-mobile/bin/neverd --java-home "$JAVA_HOME" --d8 "$NEVERD_D8" \
  --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
  --work-dir "$RUNNER_TEMP/android-captured-terminal-throw-bridges"
python scripts/test_mobile_android_plain_integer_bridge.py --captured-member \
  --neverd build-mobile/bin/neverd --java-home "$JAVA_HOME" --d8 "$NEVERD_D8" \
  --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
  --work-dir "$RUNNER_TEMP/android-captured-integer-forwarding"
python scripts/test_mobile_android_plain_integer_bridge.py --captured-member --guarded-reference \
  --neverd build-mobile/bin/neverd --java-home "$JAVA_HOME" --d8 "$NEVERD_D8" \
  --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
  --work-dir "$RUNNER_TEMP/android-captured-boolean-guard"
python scripts/test_mobile_android_plain_integer_bridge.py --captured-member --guarded-integer \
  --neverd build-mobile/bin/neverd --java-home "$JAVA_HOME" --d8 "$NEVERD_D8" \
  --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
  --work-dir "$RUNNER_TEMP/android-captured-integer-guard"
python scripts/test_mobile_android_constructors.py --cast-inner-members \
  --neverd build-mobile/bin/neverd --java-home "$JAVA_HOME" \
  --work-dir "$RUNNER_TEMP/android-cast-inner-members"
python scripts/test_mobile_android_constructors.py --allocating-inner-members \
  --neverd build-mobile/bin/neverd --java-home "$JAVA_HOME" \
  --work-dir "$RUNNER_TEMP/android-allocating-inner-members"
python scripts/test_mobile_android_constructors.py --static-field-inner-members \
  --neverd build-mobile/bin/neverd --java-home "$JAVA_HOME" \
  --work-dir "$RUNNER_TEMP/android-static-field-inner-members"
python scripts/test_mobile_android_integer_void_consumers.py \
  --neverd build-mobile/bin/neverd --java-home "$JAVA_HOME" --d8 "$NEVERD_D8" \
  --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
  --work-dir "$RUNNER_TEMP/android-integer-void-consumers"
python scripts/test_mobile_android_integer_void_consumers.py --dependency-interface \
  --neverd build-mobile/bin/neverd --java-home "$JAVA_HOME" --d8 "$NEVERD_D8" \
  --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
  --work-dir "$RUNNER_TEMP/android-integer-void-consumer-declarations"
python scripts/test_mobile_android_integer_void_consumers.py --inherited-interface \
  --neverd build-mobile/bin/neverd --java-home "$JAVA_HOME" --d8 "$NEVERD_D8" \
  --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
  --work-dir "$RUNNER_TEMP/android-inherited-integer-void-consumers"
python scripts/test_mobile_android_integer_void_consumers.py --inherited-interface --dependency-interface \
  --neverd build-mobile/bin/neverd --java-home "$JAVA_HOME" --d8 "$NEVERD_D8" \
  --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
  --work-dir "$RUNNER_TEMP/android-inherited-integer-void-consumer-declarations"
python scripts/test_mobile_android_generic_default_class_bridges.py \
  --neverd build-mobile/bin/neverd --java-home "$JAVA_HOME" --d8 "$NEVERD_D8" \
  --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
  --work-dir "$RUNNER_TEMP/android-generic-default-class-bridges"
python scripts/test_mobile_android_generic_default_class_bridges.py --dependency-interface \
  --neverd build-mobile/bin/neverd --java-home "$JAVA_HOME" --d8 "$NEVERD_D8" \
  --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
  --work-dir "$RUNNER_TEMP/android-generic-default-class-bridge-declarations"
python scripts/test_mobile_android_captured_sibling_ancestors.py \
  --neverd build-mobile/bin/neverd --d8 "$NEVERD_D8" --java-home "$JAVA_HOME" \
  --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
  --work-dir "$RUNNER_TEMP/android-captured-sibling-ancestors"
python scripts/test_mobile_android_captured_sibling_ancestors.py --dependency-root \
  --neverd build-mobile/bin/neverd --d8 "$NEVERD_D8" --java-home "$JAVA_HOME" \
  --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
  --work-dir "$RUNNER_TEMP/android-captured-sibling-ancestor-declarations"
python scripts/test_mobile_android_captured_sibling_ancestors.py --inherited-default \
  --neverd build-mobile/bin/neverd --d8 "$NEVERD_D8" --java-home "$JAVA_HOME" \
  --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
  --work-dir "$RUNNER_TEMP/android-captured-sibling-defaults"
python scripts/test_mobile_android_captured_sibling_ancestors.py --inherited-default --dependency-root \
  --neverd build-mobile/bin/neverd --d8 "$NEVERD_D8" --java-home "$JAVA_HOME" \
  --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
  --work-dir "$RUNNER_TEMP/android-captured-sibling-default-declarations"
python scripts/test_mobile_android_anonymous_guarded_super_adapters.py \
  --neverd build-mobile/bin/neverd --d8 "$NEVERD_D8" --java-home "$JAVA_HOME" \
  --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
  --work-dir "$RUNNER_TEMP/android-anonymous-guarded-super-adapters"
python scripts/test_mobile_android_anonymous_guarded_super_adapters.py --dependency-root \
  --neverd build-mobile/bin/neverd --d8 "$NEVERD_D8" --java-home "$JAVA_HOME" \
  --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
  --work-dir "$RUNNER_TEMP/android-anonymous-guarded-super-declarations"
python scripts/test_mobile_android_anonymous_guarded_super_adapters.py --nullable \
  --neverd build-mobile/bin/neverd --d8 "$NEVERD_D8" --java-home "$JAVA_HOME" \
  --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
  --work-dir "$RUNNER_TEMP/android-nullable-anonymous-guarded-super-adapters"
python scripts/test_mobile_android_anonymous_guarded_super_adapters.py --nullable --dependency-root \
  --neverd build-mobile/bin/neverd --d8 "$NEVERD_D8" --java-home "$JAVA_HOME" \
  --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
  --work-dir "$RUNNER_TEMP/android-nullable-anonymous-guarded-super-declarations"
python scripts/test_mobile_android_local_nominal_scalar_guards.py \
  --neverd build-mobile/bin/neverd --d8 "$NEVERD_D8" --java-home "$JAVA_HOME" \
  --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
  --work-dir "$RUNNER_TEMP/android-local-nominal-scalar-guards"
python scripts/test_mobile_android_local_nominal_scalar_guards.py --dependency-root \
  --neverd build-mobile/bin/neverd --d8 "$NEVERD_D8" --java-home "$JAVA_HOME" \
  --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
  --work-dir "$RUNNER_TEMP/android-local-nominal-scalar-guard-declarations"
python scripts/test_mobile_android_captured_sdk_inheritance.py \
  --neverd build-mobile/bin/neverd --d8 "$NEVERD_D8" --java-home "$JAVA_HOME" \
  --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
  --work-dir "$RUNNER_TEMP/android-captured-sdk-inheritance"
python scripts/test_mobile_android_raw_captured_interface.py \
  --neverd build-mobile/bin/neverd --d8 "$NEVERD_D8" --java-home "$JAVA_HOME" \
  --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
  --work-dir "$RUNNER_TEMP/android-raw-captured-interface"
python scripts/test_mobile_android_protected_array_callbacks.py \
  --neverd build-mobile/bin/neverd --d8 "$NEVERD_D8" --java-home "$JAVA_HOME" \
  --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
  --work-dir "$RUNNER_TEMP/android-protected-array-callbacks"
python scripts/test_mobile_android_protected_array_callbacks.py --dependency-parent \
  --neverd build-mobile/bin/neverd --d8 "$NEVERD_D8" --java-home "$JAVA_HOME" \
  --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
  --work-dir "$RUNNER_TEMP/android-protected-array-callbacks-dependency"
python scripts/test_mobile_android_protected_array_callbacks.py --package-private \
  --neverd build-mobile/bin/neverd --d8 "$NEVERD_D8" --java-home "$JAVA_HOME" \
  --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
  --work-dir "$RUNNER_TEMP/android-package-array-callbacks"
python scripts/test_mobile_android_protected_array_callbacks.py --package-private --dependency-parent \
  --neverd build-mobile/bin/neverd --d8 "$NEVERD_D8" --java-home "$JAVA_HOME" \
  --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
  --work-dir "$RUNNER_TEMP/android-package-array-callbacks-dependency"
python scripts/test_mobile_android_anonymous_inherited_contracts.py \
  --neverd build-mobile/bin/neverd --d8 "$NEVERD_D8" --java-home "$JAVA_HOME" \
  --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
  --work-dir "$RUNNER_TEMP/android-anonymous-inherited-contracts"
python scripts/test_mobile_android_anonymous_inherited_contracts.py --dependency-parent \
  --neverd build-mobile/bin/neverd --d8 "$NEVERD_D8" --java-home "$JAVA_HOME" \
  --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
  --work-dir "$RUNNER_TEMP/android-anonymous-inherited-dependency"
python scripts/test_mobile_android_anonymous_inherited_contracts.py --fixed-throws \
  --neverd build-mobile/bin/neverd --d8 "$NEVERD_D8" --java-home "$JAVA_HOME" \
  --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
  --work-dir "$RUNNER_TEMP/android-inherited-fixed-throws"
python scripts/test_mobile_android_anonymous_inherited_contracts.py --fixed-throws --dependency-parent \
  --neverd build-mobile/bin/neverd --d8 "$NEVERD_D8" --java-home "$JAVA_HOME" \
  --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
  --work-dir "$RUNNER_TEMP/android-inherited-fixed-throws-dependency"
python scripts/test_mobile_android_enclosed_inheritance.py \
  --neverd build-mobile/bin/neverd --java-home "$JAVA_HOME" --d8 "$NEVERD_D8" \
  --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
  --work-dir "$RUNNER_TEMP/android-enclosed-inheritance"

profile_group "Execute named projections of Android captured classes" 25 600
python scripts/test_mobile_android_local_captures.py \
  --neverd build-mobile/bin/neverd --d8 "$NEVERD_D8" --java-home "$JAVA_HOME" \
  --work-dir "$RUNNER_TEMP/android-local-captures"
python scripts/test_mobile_android_local_captures.py --generic \
  --neverd build-mobile/bin/neverd --d8 "$NEVERD_D8" --java-home "$JAVA_HOME" \
  --work-dir "$RUNNER_TEMP/android-local-generic"
python scripts/test_mobile_android_local_captures.py --constructor \
  --neverd build-mobile/bin/neverd --d8 "$NEVERD_D8" --java-home "$JAVA_HOME" \
  --work-dir "$RUNNER_TEMP/android-constructor-captures"
python scripts/test_mobile_android_local_captures.py --static-members \
  --neverd build-mobile/bin/neverd --d8 "$NEVERD_D8" --java-home "$JAVA_HOME" \
  --work-dir "$RUNNER_TEMP/android-local-static"
python scripts/test_mobile_android_local_captures.py --nested \
  --neverd build-mobile/bin/neverd --d8 "$NEVERD_D8" --java-home "$JAVA_HOME" \
  --work-dir "$RUNNER_TEMP/android-local-nested"
python scripts/test_mobile_android_local_captures.py --field-members \
  --neverd build-mobile/bin/neverd --d8 "$NEVERD_D8" --java-home "$JAVA_HOME" \
  --work-dir "$RUNNER_TEMP/android-field-members"
python scripts/test_mobile_android_anonymous_members.py \
  --neverd build-mobile/bin/neverd --d8 "$NEVERD_D8" --java-home "$JAVA_HOME" \
  --work-dir "$RUNNER_TEMP/android-generic-anonymous-members"
python scripts/test_mobile_android_inner_bounds.py \
  --neverd build-mobile/bin/neverd --d8 "$NEVERD_D8" --java-home "$JAVA_HOME" \
  --work-dir "$RUNNER_TEMP/android-inner-interface-bounds"
python scripts/test_mobile_android_inner_bounds.py --class-bound \
  --neverd build-mobile/bin/neverd --d8 "$NEVERD_D8" --java-home "$JAVA_HOME" \
  --work-dir "$RUNNER_TEMP/android-inner-class-bounds"
python scripts/test_mobile_android_shared_anonymous.py \
  --neverd build-mobile/bin/neverd --java-home "$JAVA_HOME" \
  --work-dir "$RUNNER_TEMP/android-shared-anonymous"
python scripts/test_mobile_android_shared_anonymous.py --self-types \
  --neverd build-mobile/bin/neverd --java-home "$JAVA_HOME" \
  --work-dir "$RUNNER_TEMP/android-anonymous-self-types"
python scripts/test_mobile_android_shared_anonymous.py --captured-signatures \
  --neverd build-mobile/bin/neverd --java-home "$JAVA_HOME" \
  --work-dir "$RUNNER_TEMP/android-captured-signatures"
python scripts/test_mobile_android_shared_anonymous.py --class-captured-signatures \
  --neverd build-mobile/bin/neverd --java-home "$JAVA_HOME" \
  --work-dir "$RUNNER_TEMP/android-class-captured-signatures"
python scripts/test_mobile_android_shared_anonymous.py --omitted-captures \
  --neverd build-mobile/bin/neverd --java-home "$JAVA_HOME" \
  --work-dir "$RUNNER_TEMP/android-omitted-captures"
python scripts/test_mobile_android_shared_anonymous.py --default-scopes \
  --neverd build-mobile/bin/neverd --java-home "$JAVA_HOME" \
  --work-dir "$RUNNER_TEMP/android-default-scopes"
python scripts/test_mobile_android_kotlin_scopes.py \
  --neverd build-mobile/bin/neverd --java-home "$JAVA_HOME" --d8 "$NEVERD_D8" \
  --work-dir "$RUNNER_TEMP/android-kotlin-scopes"
python scripts/test_mobile_android_kotlin_scopes.py --unit-package-part \
  --neverd build-mobile/bin/neverd --java-home "$JAVA_HOME" --d8 "$NEVERD_D8" \
  --work-dir "$RUNNER_TEMP/android-kotlin-unit-parts"
python scripts/test_mobile_android_kotlin_scopes.py --primitive-parameter \
  --neverd build-mobile/bin/neverd --java-home "$JAVA_HOME" --d8 "$NEVERD_D8" \
  --work-dir "$RUNNER_TEMP/android-kotlin-primitive-parts"
python scripts/test_mobile_android_kotlin_scopes.py --suspend-callback \
  --neverd build-mobile/bin/neverd --java-home "$JAVA_HOME" --d8 "$NEVERD_D8" \
  --work-dir "$RUNNER_TEMP/android-kotlin-suspend-callbacks"
python scripts/test_mobile_android_lambda_scopes.py \
  --neverd build-mobile/bin/neverd --java-home "$JAVA_HOME" --d8 "$NEVERD_D8" \
  --work-dir "$RUNNER_TEMP/android-lambda-scopes"
python scripts/test_mobile_android_instance_defaults.py \
  --neverd build-mobile/bin/neverd --java-home "$JAVA_HOME" --d8 "$NEVERD_D8" \
  --work-dir "$RUNNER_TEMP/android-instance-default-scopes"
python scripts/test_mobile_android_instance_defaults.py --copied-receiver \
  --neverd build-mobile/bin/neverd --java-home "$JAVA_HOME" --d8 "$NEVERD_D8" \
  --work-dir "$RUNNER_TEMP/android-instance-default-copies"
python scripts/test_mobile_android_static_defaults.py \
  --neverd build-mobile/bin/neverd --java-home "$JAVA_HOME" --d8 "$NEVERD_D8" \
  --work-dir "$RUNNER_TEMP/android-static-default-scopes"
python scripts/test_mobile_android_shared_upper_bounds.py \
  --neverd build-mobile/bin/neverd --java-home "$JAVA_HOME" --d8 "$NEVERD_D8" \
  --work-dir "$RUNNER_TEMP/android-shared-upper-bounds"
python scripts/test_mobile_android_shared_upper_bounds.py --marker-method-bound \
  --neverd build-mobile/bin/neverd --java-home "$JAVA_HOME" --d8 "$NEVERD_D8" \
  --work-dir "$RUNNER_TEMP/android-shared-marker-bounds"
python scripts/test_mobile_android_recursive_bounds.py \
  --neverd build-mobile/bin/neverd --java-home "$JAVA_HOME" --d8 "$NEVERD_D8" \
  --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
  --work-dir "$RUNNER_TEMP/android-shared-recursive-bounds"
python scripts/test_mobile_android_array_bounds.py \
  --neverd build-mobile/bin/neverd --java-home "$JAVA_HOME" --d8 "$NEVERD_D8" \
  --work-dir "$RUNNER_TEMP/android-array-bounds"
python scripts/test_mobile_android_array_bounds.py --interface-bound \
  --neverd build-mobile/bin/neverd --java-home "$JAVA_HOME" --d8 "$NEVERD_D8" \
  --work-dir "$RUNNER_TEMP/android-interface-bounds"
python scripts/test_mobile_android_synthetic_bounds.py \
  --neverd build-mobile/bin/neverd --java-home "$JAVA_HOME" --d8 "$NEVERD_D8" \
  --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
  --work-dir "$RUNNER_TEMP/android-synthetic-bounds"
python scripts/test_mobile_android_enum_erasure.py \
  --neverd build-mobile/bin/neverd --java-home "$JAVA_HOME" --d8 "$NEVERD_D8" \
  --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
  --work-dir "$RUNNER_TEMP/android-enum-erasure"
python scripts/test_mobile_android_shared_anonymous.py --member-host \
  --neverd build-mobile/bin/neverd --java-home "$JAVA_HOME" \
  --work-dir "$RUNNER_TEMP/android-shared-member-host"
python scripts/test_mobile_android_shared_anonymous.py --public-facade \
  --neverd build-mobile/bin/neverd --java-home "$JAVA_HOME" \
  --work-dir "$RUNNER_TEMP/android-shared-public-facade"
python scripts/test_mobile_android_shared_anonymous.py --static-only-member \
  --neverd build-mobile/bin/neverd --java-home "$JAVA_HOME" \
  --work-dir "$RUNNER_TEMP/android-static-inner-member"
python scripts/test_mobile_android_shared_anonymous.py --static-utility \
  --neverd build-mobile/bin/neverd --java-home "$JAVA_HOME" \
  --work-dir "$RUNNER_TEMP/android-static-anonymous-utility"

profile_group "Execute anonymous covariant bridges with source and frozen parents" 25 600
python scripts/test_mobile_android_anonymous_covariant_bridges.py \
  --neverd build-mobile/bin/neverd --java-home "$JAVA_HOME" --d8 "$NEVERD_D8" \
  --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
  --work-dir "$RUNNER_TEMP/android-anonymous-covariant-bridges"
python scripts/test_mobile_android_anonymous_covariant_bridges.py --dependency-provider \
  --neverd build-mobile/bin/neverd --java-home "$JAVA_HOME" --d8 "$NEVERD_D8" \
  --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
  --work-dir "$RUNNER_TEMP/android-anonymous-covariant-bridges-sdk"

profile_group "Execute captured constructor field reads with source and frozen providers" 25 600
python scripts/test_mobile_android_captured_constructor_reads.py \
  --neverd build-mobile/bin/neverd --java-home "$JAVA_HOME" --d8 "$NEVERD_D8" \
  --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
  --work-dir "$RUNNER_TEMP/android-captured-constructor-reads"
python scripts/test_mobile_android_captured_constructor_reads.py --dependency-provider \
  --neverd build-mobile/bin/neverd --java-home "$JAVA_HOME" --d8 "$NEVERD_D8" \
  --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
  --work-dir "$RUNNER_TEMP/android-captured-constructor-reads-sdk"

profile_group "Execute instance intersection invocations with source and frozen providers" 25 600
python scripts/test_mobile_android_instance_intersection_invocations.py \
  --neverd build-mobile/bin/neverd --java-home "$JAVA_HOME" --d8 "$NEVERD_D8" \
  --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
  --work-dir "$RUNNER_TEMP/android-instance-intersection-invocations"
python scripts/test_mobile_android_instance_intersection_invocations.py --dependency-provider \
  --neverd build-mobile/bin/neverd --java-home "$JAVA_HOME" --d8 "$NEVERD_D8" \
  --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
  --work-dir "$RUNNER_TEMP/android-instance-intersection-invocations-sdk"

profile_group "Execute implicit static defaults with source and frozen providers" 25 600
python scripts/test_mobile_android_implicit_static_defaults.py \
  --neverd build-mobile/bin/neverd --java-home "$JAVA_HOME" --d8 "$NEVERD_D8" \
  --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
  --work-dir "$RUNNER_TEMP/android-implicit-static-defaults"
python scripts/test_mobile_android_implicit_static_defaults.py --dependency-provider \
  --neverd build-mobile/bin/neverd --java-home "$JAVA_HOME" --d8 "$NEVERD_D8" \
  --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
  --work-dir "$RUNNER_TEMP/android-implicit-static-defaults-sdk"

profile_group "Execute anonymous member allocations with source and frozen providers" 25 600
python scripts/test_mobile_android_anonymous_member_allocations.py \
  --neverd build-mobile/bin/neverd --java-home "$JAVA_HOME" --d8 "$NEVERD_D8" \
  --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
  --work-dir "$RUNNER_TEMP/android-anonymous-member-allocations"
python scripts/test_mobile_android_anonymous_member_allocations.py --dependency-provider \
  --neverd build-mobile/bin/neverd --java-home "$JAVA_HOME" --d8 "$NEVERD_D8" \
  --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
  --work-dir "$RUNNER_TEMP/android-anonymous-member-allocations-sdk"
python scripts/test_mobile_android_anonymous_member_allocations.py --conditional \
  --neverd build-mobile/bin/neverd --java-home "$JAVA_HOME" --d8 "$NEVERD_D8" \
  --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
  --work-dir "$RUNNER_TEMP/android-anonymous-member-allocations-conditional"
python scripts/test_mobile_android_anonymous_member_allocations.py --conditional --dependency-provider \
  --neverd build-mobile/bin/neverd --java-home "$JAVA_HOME" --d8 "$NEVERD_D8" \
  --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
  --work-dir "$RUNNER_TEMP/android-anonymous-member-allocations-conditional-sdk"

profile_group "Execute seeded nominal results with source and frozen providers" 25 600
python scripts/test_mobile_android_seeded_nominal_results.py \
  --neverd build-mobile/bin/neverd --java-home "$JAVA_HOME" --d8 "$NEVERD_D8" \
  --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
  --work-dir "$RUNNER_TEMP/android-seeded-nominal-results"
python scripts/test_mobile_android_seeded_nominal_results.py --dependency-provider \
  --neverd build-mobile/bin/neverd --java-home "$JAVA_HOME" --d8 "$NEVERD_D8" \
  --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
  --work-dir "$RUNNER_TEMP/android-seeded-nominal-results-sdk"

profile_group "Execute conditional scalar conversions with source and frozen providers" 25 600
python scripts/test_mobile_android_conditional_scalar_conversions.py \
  --neverd build-mobile/bin/neverd --java-home "$JAVA_HOME" --d8 "$NEVERD_D8" \
  --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
  --work-dir "$RUNNER_TEMP/android-conditional-scalar-conversions"
python scripts/test_mobile_android_conditional_scalar_conversions.py --dependency-provider \
  --neverd build-mobile/bin/neverd --java-home "$JAVA_HOME" --d8 "$NEVERD_D8" \
  --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
  --work-dir "$RUNNER_TEMP/android-conditional-scalar-conversions-sdk"

profile_group "Execute private static names with source and frozen targets" 21 600
python scripts/test_mobile_android_synthetic_method_names.py --private-forwarder \
  --neverd build-mobile/bin/neverd --java-home "$JAVA_HOME" --d8 "$NEVERD_D8" \
  --kotlin-lib "$RUNNER_TEMP/neverd-kotlin-2.0.21/kotlinc/lib" \
  --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
  --work-dir "$RUNNER_TEMP/android-private-static-names"
python scripts/test_mobile_android_synthetic_method_names.py --private-forwarder --dependency-box \
  --neverd build-mobile/bin/neverd --java-home "$JAVA_HOME" --d8 "$NEVERD_D8" \
  --kotlin-lib "$RUNNER_TEMP/neverd-kotlin-2.0.21/kotlinc/lib" \
  --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
  --work-dir "$RUNNER_TEMP/android-private-static-names-sdk"
python scripts/test_mobile_android_synthetic_method_names.py --private-forwarder --ordinary-static \
  --neverd build-mobile/bin/neverd --java-home "$JAVA_HOME" --d8 "$NEVERD_D8" \
  --kotlin-lib "$RUNNER_TEMP/neverd-kotlin-2.0.21/kotlinc/lib" \
  --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
  --work-dir "$RUNNER_TEMP/android-private-static-names-ordinary"
python scripts/test_mobile_android_synthetic_method_names.py --private-forwarder --ordinary-static --dependency-box \
  --neverd build-mobile/bin/neverd --java-home "$JAVA_HOME" --d8 "$NEVERD_D8" \
  --kotlin-lib "$RUNNER_TEMP/neverd-kotlin-2.0.21/kotlinc/lib" \
  --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
  --work-dir "$RUNNER_TEMP/android-private-static-names-ordinary-sdk"

profile_group "Execute protected superclass allocation calls across source and SDK" 25 600
python scripts/test_mobile_android_allocation_super_calls.py --protected-parent \
  --neverd build-mobile/bin/neverd --java-home "$JAVA_HOME" --d8 "$NEVERD_D8" \
  --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
  --work-dir "$RUNNER_TEMP/android-allocation-protected-super-calls"
python scripts/test_mobile_android_allocation_super_calls.py --protected-parent --dependency-provider \
  --neverd build-mobile/bin/neverd --java-home "$JAVA_HOME" --d8 "$NEVERD_D8" \
  --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
  --work-dir "$RUNNER_TEMP/android-allocation-protected-super-calls-sdk"

profile_group "Execute implicit finality and generic final-class method names" 21 600
python scripts/test_mobile_android_final_instance_names.py --implicit-finality --ordinary \
  --neverd build-mobile/bin/neverd --java-home "$JAVA_HOME" --d8 "$NEVERD_D8" \
  --kotlin-lib "$RUNNER_TEMP/neverd-kotlin-2.0.21/kotlinc/lib" \
  --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
  --work-dir "$RUNNER_TEMP/android-final-instance-names-implicit"
python scripts/test_mobile_android_final_instance_names.py --implicit-finality --ordinary --dependency-base \
  --neverd build-mobile/bin/neverd --java-home "$JAVA_HOME" --d8 "$NEVERD_D8" \
  --kotlin-lib "$RUNNER_TEMP/neverd-kotlin-2.0.21/kotlinc/lib" \
  --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
  --work-dir "$RUNNER_TEMP/android-final-instance-names-implicit-sdk"
python scripts/test_mobile_android_final_instance_names.py --generic-owner --ordinary \
  --neverd build-mobile/bin/neverd --java-home "$JAVA_HOME" --d8 "$NEVERD_D8" \
  --kotlin-lib "$RUNNER_TEMP/neverd-kotlin-2.0.21/kotlinc/lib" \
  --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
  --work-dir "$RUNNER_TEMP/android-final-instance-names-generic"
python scripts/test_mobile_android_final_instance_names.py --generic-owner --ordinary --dependency-base \
  --neverd build-mobile/bin/neverd --java-home "$JAVA_HOME" --d8 "$NEVERD_D8" \
  --kotlin-lib "$RUNNER_TEMP/neverd-kotlin-2.0.21/kotlinc/lib" \
  --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
  --work-dir "$RUNNER_TEMP/android-final-instance-names-generic-sdk"
python scripts/test_mobile_android_final_instance_names.py --implicit-finality --generic-owner \
  --neverd build-mobile/bin/neverd --java-home "$JAVA_HOME" --d8 "$NEVERD_D8" \
  --kotlin-lib "$RUNNER_TEMP/neverd-kotlin-2.0.21/kotlinc/lib" \
  --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
  --work-dir "$RUNNER_TEMP/android-final-instance-names-implicit-generic"
python scripts/test_mobile_android_final_instance_names.py --implicit-finality --generic-owner --dependency-base \
  --neverd build-mobile/bin/neverd --java-home "$JAVA_HOME" --d8 "$NEVERD_D8" \
  --kotlin-lib "$RUNNER_TEMP/neverd-kotlin-2.0.21/kotlinc/lib" \
  --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
  --work-dir "$RUNNER_TEMP/android-final-instance-names-implicit-generic-sdk"

profile_group "Execute scalar unary operators inside allocation arguments" 25 600
python scripts/test_mobile_android_allocation_scalar_conversions.py --unary \
  --neverd build-mobile/bin/neverd --java-home "$JAVA_HOME" --d8 "$NEVERD_D8" \
  --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
  --work-dir "$RUNNER_TEMP/android-allocation-scalar-unary"
python scripts/test_mobile_android_allocation_scalar_conversions.py --unary --dependency-provider \
  --neverd build-mobile/bin/neverd --java-home "$JAVA_HOME" --d8 "$NEVERD_D8" \
  --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
  --work-dir "$RUNNER_TEMP/android-allocation-scalar-unary-sdk"

profile_group "Execute floating arithmetic inside allocation arguments" 25 600
python scripts/test_mobile_android_allocation_scalar_conversions.py --floating \
  --neverd build-mobile/bin/neverd --java-home "$JAVA_HOME" --d8 "$NEVERD_D8" \
  --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
  --work-dir "$RUNNER_TEMP/android-allocation-floating"
python scripts/test_mobile_android_allocation_scalar_conversions.py --floating --dependency-provider \
  --neverd build-mobile/bin/neverd --java-home "$JAVA_HOME" --d8 "$NEVERD_D8" \
  --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
  --work-dir "$RUNNER_TEMP/android-allocation-floating-sdk"

profile_group "Execute primitive array payloads inside allocation arguments" 25 600
python scripts/test_mobile_android_allocation_array_payload.py \
  --neverd build-mobile/bin/neverd --java-home "$JAVA_HOME" --d8 "$NEVERD_D8" \
  --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
  --work-dir "$RUNNER_TEMP/android-allocation-array-payload"
python scripts/test_mobile_android_allocation_array_payload.py --dependency-provider \
  --neverd build-mobile/bin/neverd --java-home "$JAVA_HOME" --d8 "$NEVERD_D8" \
  --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
  --work-dir "$RUNNER_TEMP/android-allocation-array-payload-sdk"

profile_group "Execute concrete source superclass virtual naming families" 21 600
python scripts/test_mobile_android_concrete_virtual_names.py \
  --neverd build-mobile/bin/neverd --java-home "$JAVA_HOME" --d8 "$NEVERD_D8" \
  --kotlin-lib "$RUNNER_TEMP/neverd-kotlin-2.0.21/kotlinc/lib" \
  --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
  --work-dir "$RUNNER_TEMP/android-concrete-virtual-names"
python scripts/test_mobile_android_concrete_virtual_names.py --dependency-base \
  --neverd build-mobile/bin/neverd --java-home "$JAVA_HOME" --d8 "$NEVERD_D8" \
  --kotlin-lib "$RUNNER_TEMP/neverd-kotlin-2.0.21/kotlinc/lib" \
  --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
  --work-dir "$RUNNER_TEMP/android-concrete-virtual-names-sdk"

profile_group "Execute primitive constructor selection against generic siblings" 25 600
python scripts/test_mobile_android_primitive_constructor_overloads.py \
  --neverd build-mobile/bin/neverd --java-home "$JAVA_HOME" --d8 "$NEVERD_D8" \
  --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
  --work-dir "$RUNNER_TEMP/android-primitive-constructor-overloads"
python scripts/test_mobile_android_primitive_constructor_overloads.py --dependency-provider \
  --neverd build-mobile/bin/neverd --java-home "$JAVA_HOME" --d8 "$NEVERD_D8" \
  --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
  --work-dir "$RUNNER_TEMP/android-primitive-constructor-overloads-sdk"

profile_group "Execute primitive constructor widening views against narrower overloads" 25 600
python scripts/test_mobile_android_primitive_constructor_overloads.py --byte-forwarder \
  --neverd build-mobile/bin/neverd --java-home "$JAVA_HOME" --d8 "$NEVERD_D8" \
  --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
  --work-dir "$RUNNER_TEMP/android-primitive-constructor-byte-view"
python scripts/test_mobile_android_primitive_constructor_overloads.py --byte-forwarder --dependency-provider \
  --neverd build-mobile/bin/neverd --java-home "$JAVA_HOME" --d8 "$NEVERD_D8" \
  --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
  --work-dir "$RUNNER_TEMP/android-primitive-constructor-byte-view-sdk"

profile_group "Execute class-scoped raw superclass constructor forwarding" 25 600
python scripts/test_mobile_android_scoped_raw_constructors.py \
  --neverd build-mobile/bin/neverd --java-home "$JAVA_HOME" --d8 "$NEVERD_D8" \
  --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
  --work-dir "$RUNNER_TEMP/android-scoped-raw-constructors"
python scripts/test_mobile_android_scoped_raw_constructors.py --dependency-provider \
  --neverd build-mobile/bin/neverd --java-home "$JAVA_HOME" --d8 "$NEVERD_D8" \
  --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
  --work-dir "$RUNNER_TEMP/android-scoped-raw-constructors-sdk"

profile_group "Execute unseeded static scalar carrier calls and package scope" 25 600
python scripts/test_mobile_android_unseeded_scalar_carrier.py \
  --neverd build-mobile/bin/neverd --java-home "$JAVA_HOME" --d8 "$NEVERD_D8" \
  --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
  --work-dir "$RUNNER_TEMP/android-unseeded-scalar-carrier"
python scripts/test_mobile_android_unseeded_scalar_carrier.py --dependency-provider \
  --neverd build-mobile/bin/neverd --java-home "$JAVA_HOME" --d8 "$NEVERD_D8" \
  --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
  --work-dir "$RUNNER_TEMP/android-unseeded-scalar-carrier-sdk"
python scripts/test_mobile_android_unseeded_scalar_carrier.py --package-helper \
  --neverd build-mobile/bin/neverd --java-home "$JAVA_HOME" --d8 "$NEVERD_D8" \
  --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
  --work-dir "$RUNNER_TEMP/android-unseeded-scalar-carrier-package"
python scripts/test_mobile_android_unseeded_scalar_carrier.py --package-helper --dependency-provider \
  --neverd build-mobile/bin/neverd --java-home "$JAVA_HOME" --d8 "$NEVERD_D8" \
  --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
  --work-dir "$RUNNER_TEMP/android-unseeded-scalar-carrier-package-sdk"

profile_group "Execute generic array transfers with covariant and overlapping arrays" 25 600
python scripts/test_mobile_android_generic_array_transfers.py \
  --neverd build-mobile/bin/neverd --java-home "$JAVA_HOME" --d8 "$NEVERD_D8" \
  --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
  --work-dir "$RUNNER_TEMP/android-generic-array-transfers"
python scripts/test_mobile_android_generic_array_transfers.py --dependency-provider \
  --neverd build-mobile/bin/neverd --java-home "$JAVA_HOME" --d8 "$NEVERD_D8" \
  --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
  --work-dir "$RUNNER_TEMP/android-generic-array-transfers-sdk"

profile_group "Execute package-visible generic array transfers" 25 600
python scripts/test_mobile_android_generic_array_transfers.py --package-provider \
  --neverd build-mobile/bin/neverd --java-home "$JAVA_HOME" --d8 "$NEVERD_D8" \
  --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
  --work-dir "$RUNNER_TEMP/android-package-array-transfers"
python scripts/test_mobile_android_generic_array_transfers.py --package-provider --dependency-provider \
  --neverd build-mobile/bin/neverd --java-home "$JAVA_HOME" --d8 "$NEVERD_D8" \
  --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
  --work-dir "$RUNNER_TEMP/android-package-array-transfers-sdk"

profile_group "Execute immutable array element reads inside allocation arguments" 25 600
python scripts/test_mobile_android_allocation_array_reads.py \
  --neverd build-mobile/bin/neverd --java-home "$JAVA_HOME" --d8 "$NEVERD_D8" \
  --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
  --work-dir "$RUNNER_TEMP/android-allocation-array-reads"
python scripts/test_mobile_android_allocation_array_reads.py --dependency-provider \
  --neverd build-mobile/bin/neverd --java-home "$JAVA_HOME" --d8 "$NEVERD_D8" \
  --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
  --work-dir "$RUNNER_TEMP/android-allocation-array-reads-sdk"

profile_group "Execute integer arithmetic after original allocation argument calls" 25 600
python scripts/test_mobile_android_allocation_integer_arithmetic.py --calls \
  --neverd build-mobile/bin/neverd --java-home "$JAVA_HOME" --d8 "$NEVERD_D8" \
  --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
  --work-dir "$RUNNER_TEMP/android-allocation-integer-calls"
python scripts/test_mobile_android_allocation_integer_arithmetic.py --calls --dependency-target \
  --neverd build-mobile/bin/neverd --java-home "$JAVA_HOME" --d8 "$NEVERD_D8" \
  --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
  --work-dir "$RUNNER_TEMP/android-allocation-integer-calls-sdk"

profile_group "Execute independently owned sequential monitor regions" 25 600
python scripts/test_mobile_android_sequential_monitors.py \
  --neverd build-mobile/bin/neverd --java-home "$JAVA_HOME" --d8 "$NEVERD_D8" \
  --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
  --work-dir "$RUNNER_TEMP/android-sequential-monitors"
python scripts/test_mobile_android_sequential_monitors.py --dependency-provider \
  --neverd build-mobile/bin/neverd --java-home "$JAVA_HOME" --d8 "$NEVERD_D8" \
  --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
  --work-dir "$RUNNER_TEMP/android-sequential-monitors-sdk"

profile_group "Execute released continuations that reacquire original monitors" 25 600
python scripts/test_mobile_android_sequential_monitors.py --loop \
  --neverd build-mobile/bin/neverd --java-home "$JAVA_HOME" --d8 "$NEVERD_D8" \
  --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
  --work-dir "$RUNNER_TEMP/android-monitor-reacquisition"
python scripts/test_mobile_android_sequential_monitors.py --loop --dependency-provider \
  --neverd build-mobile/bin/neverd --java-home "$JAVA_HOME" --d8 "$NEVERD_D8" \
  --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
  --work-dir "$RUNNER_TEMP/android-monitor-reacquisition-sdk"

profile_group "Execute exact Object-bounded generic void array writers" 25 600
python scripts/test_mobile_android_generic_void_array_writers.py \
  --neverd build-mobile/bin/neverd --java-home "$JAVA_HOME" --d8 "$NEVERD_D8" \
  --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
  --work-dir "$RUNNER_TEMP/android-generic-void-array-writers"
python scripts/test_mobile_android_generic_void_array_writers.py --dependency-provider \
  --neverd build-mobile/bin/neverd --java-home "$JAVA_HOME" --d8 "$NEVERD_D8" \
  --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
  --work-dir "$RUNNER_TEMP/android-generic-void-array-writers-sdk"

profile_group "Execute complete generic constructor reference upcasts" 25 600
python scripts/test_mobile_android_generic_constructor_upcasts.py \
  --neverd build-mobile/bin/neverd --java-home "$JAVA_HOME" \
  --d8 "$RUNNER_TEMP/neverd-java25-d8/lib/d8.jar" \
  --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
  --work-dir "$RUNNER_TEMP/android-generic-constructor-upcasts"
python scripts/test_mobile_android_generic_constructor_upcasts.py --dependency-provider \
  --neverd build-mobile/bin/neverd --java-home "$JAVA_HOME" \
  --d8 "$RUNNER_TEMP/neverd-java25-d8/lib/d8.jar" \
  --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
  --work-dir "$RUNNER_TEMP/android-generic-constructor-upcasts-sdk"

profile_group "Execute unchanged scalar results from generic array operations" 25 600
python scripts/test_mobile_android_generic_void_array_writers.py --scalar \
  --neverd build-mobile/bin/neverd --java-home "$JAVA_HOME" --d8 "$NEVERD_D8" \
  --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
  --work-dir "$RUNNER_TEMP/android-generic-array-scalar-results"
python scripts/test_mobile_android_generic_void_array_writers.py --scalar --dependency-provider \
  --neverd build-mobile/bin/neverd --java-home "$JAVA_HOME" --d8 "$NEVERD_D8" \
  --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
  --work-dir "$RUNNER_TEMP/android-generic-array-scalar-results-sdk"

profile_group "Execute raw generic Kotlin default-mask constructor delegations" 25 600
python scripts/test_mobile_android_scoped_mask_constructors.py \
  --neverd build-mobile/bin/neverd --java-home "$JAVA_HOME" --d8 "$NEVERD_D8" \
  --kotlin-java-home "$NEVERD_KOTLIN_JAVA_HOME" \
  --kotlin-lib "$RUNNER_TEMP/neverd-kotlin-2.0.21/kotlinc/lib" \
  --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
  --work-dir "$RUNNER_TEMP/android-scoped-mask-constructors"
python scripts/test_mobile_android_scoped_mask_constructors.py --dependency-defaults \
  --neverd build-mobile/bin/neverd --java-home "$JAVA_HOME" --d8 "$NEVERD_D8" \
  --kotlin-java-home "$NEVERD_KOTLIN_JAVA_HOME" \
  --kotlin-lib "$RUNNER_TEMP/neverd-kotlin-2.0.21/kotlinc/lib" \
  --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
  --work-dir "$RUNNER_TEMP/android-scoped-mask-constructors-sdk"

profile_group "Execute genuine raw local covariant singleton callbacks" 25 600
python scripts/test_mobile_android_raw_local_covariance.py \
  --neverd build-mobile/bin/neverd --java-home "$JAVA_HOME" --d8 "$NEVERD_D8" \
  --kotlin-java-home "$NEVERD_KOTLIN_JAVA_HOME" \
  --kotlin-lib "$RUNNER_TEMP/neverd-kotlin-2.0.21/kotlinc/lib" \
  --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
  --work-dir "$RUNNER_TEMP/android-raw-local-covariance"
python scripts/test_mobile_android_raw_local_covariance.py --class-lambdas \
  --neverd build-mobile/bin/neverd --java-home "$JAVA_HOME" --d8 "$NEVERD_D8" \
  --kotlin-java-home "$NEVERD_KOTLIN_JAVA_HOME" \
  --kotlin-lib "$RUNNER_TEMP/neverd-kotlin-2.0.21/kotlinc/lib" \
  --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
  --work-dir "$RUNNER_TEMP/android-raw-local-covariance-class"

profile_group "Execute shared method nominal upper bounds" 21 600
python scripts/test_mobile_android_shared_upper_bounds.py --method-class-bound \
  --neverd build-mobile/bin/neverd --java-home "$JAVA_HOME" --d8 "$NEVERD_D8" \
  --work-dir "$RUNNER_TEMP/android-shared-method-upper-bound"

profile_group "Execute raw local covariance with original input runtime classes" 25 600
python scripts/test_mobile_android_raw_local_covariance.py --class-lambdas --input-runtime \
  --neverd build-mobile/bin/neverd --java-home "$JAVA_HOME" --d8 "$NEVERD_D8" \
  --kotlin-java-home "$NEVERD_KOTLIN_JAVA_HOME" \
  --kotlin-lib "$RUNNER_TEMP/neverd-kotlin-2.0.21/kotlinc/lib" \
  --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
  --work-dir "$RUNNER_TEMP/android-raw-local-covariance-input-runtime"

profile_group "Execute field multiplication in constructor arguments" 25 600
python scripts/test_mobile_android_allocation_integer_arithmetic.py --multiply-fields \
  --neverd build-mobile/bin/neverd --java-home "$JAVA_HOME" --d8 "$NEVERD_D8" \
  --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
  --work-dir "$RUNNER_TEMP/android-allocation-field-multiply"
python scripts/test_mobile_android_allocation_integer_arithmetic.py --multiply-fields --dependency-target \
  --neverd build-mobile/bin/neverd --java-home "$JAVA_HOME" --d8 "$NEVERD_D8" \
  --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
  --work-dir "$RUNNER_TEMP/android-allocation-field-multiply-sdk"

profile_group "Execute initialized constructor delegation loops" 21 600
python scripts/test_mobile_android_delegation_postludes.py --loop \
  --neverd build-mobile/bin/neverd --java-home "$JAVA_HOME" --d8 "$NEVERD_D8" \
  --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
  --work-dir "$RUNNER_TEMP/android-delegation-loop-postludes"
python scripts/test_mobile_android_delegation_postludes.py --loop --dependency-trace \
  --neverd build-mobile/bin/neverd --java-home "$JAVA_HOME" --d8 "$NEVERD_D8" \
  --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
  --work-dir "$RUNNER_TEMP/android-delegation-loop-postludes-sdk"

profile_group "Execute local carrier target JVM names" 21 600
python scripts/test_mobile_android_carrier_target_names.py \
  --neverd build-mobile/bin/neverd --java-home "$JAVA_HOME" --d8 "$NEVERD_D8" \
  --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
  --work-dir "$RUNNER_TEMP/android-carrier-target-names"
python scripts/test_mobile_android_carrier_target_names.py --dependency-carriers \
  --neverd build-mobile/bin/neverd --java-home "$JAVA_HOME" --d8 "$NEVERD_D8" \
  --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
  --work-dir "$RUNNER_TEMP/android-carrier-target-names-sdk"

profile_group "Execute carrier target enclosing-method mappings" 25 600
python scripts/test_mobile_android_carrier_target_names.py --enclosing \
  --neverd build-mobile/bin/neverd --java-home "$JAVA_HOME" --d8 "$NEVERD_D8" \
  --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
  --work-dir "$RUNNER_TEMP/android-carrier-enclosing-names"
python scripts/test_mobile_android_carrier_target_names.py --enclosing --dependency-carriers \
  --neverd build-mobile/bin/neverd --java-home "$JAVA_HOME" --d8 "$NEVERD_D8" \
  --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
  --work-dir "$RUNNER_TEMP/android-carrier-enclosing-names-sdk"

profile_group "Execute virtual family enclosing-method mappings" 25 600
python scripts/test_mobile_android_mangled_interface_names.py --enclosing \
  --neverd build-mobile/bin/neverd --java-home "$JAVA_HOME" --d8 "$NEVERD_D8" \
  --kotlin-java-home "$NEVERD_KOTLIN_JAVA_HOME" \
  --kotlin-lib "$RUNNER_TEMP/neverd-kotlin-2.0.21/kotlinc/lib" \
  --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
  --work-dir "$RUNNER_TEMP/android-family-enclosing-names"

profile_group "Execute anchored generic void callbacks" 21 300
python scripts/test_mobile_android_anchored_void_carrier.py \
  --neverd build-mobile/bin/neverd --java-home "$JAVA_HOME" --d8 "$NEVERD_D8" \
  --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
  --work-dir "$RUNNER_TEMP/android-anchored-void-carrier"
python scripts/test_mobile_android_anchored_void_carrier.py --dependency-provider \
  --neverd build-mobile/bin/neverd --java-home "$JAVA_HOME" --d8 "$NEVERD_D8" \
  --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
  --work-dir "$RUNNER_TEMP/android-anchored-void-carrier-sdk"

profile_group "Execute unary generic void callbacks" 21 300
python scripts/test_mobile_android_unary_void_carrier.py \
  --neverd build-mobile/bin/neverd --java-home "$JAVA_HOME" --d8 "$NEVERD_D8" \
  --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
  --work-dir "$RUNNER_TEMP/android-unary-void-carrier"
python scripts/test_mobile_android_unary_void_carrier.py --dependency-provider \
  --neverd build-mobile/bin/neverd --java-home "$JAVA_HOME" --d8 "$NEVERD_D8" \
  --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
  --work-dir "$RUNNER_TEMP/android-unary-void-carrier-sdk"

profile_group "Execute projected static wrapper method seeds" 21 300
python scripts/test_mobile_android_projected_seed_carrier.py \
  --neverd build-mobile/bin/neverd --java-home "$JAVA_HOME" --d8 "$NEVERD_D8" \
  --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
  --work-dir "$RUNNER_TEMP/android-projected-seed-carrier"
python scripts/test_mobile_android_projected_seed_carrier.py --dependency-callback \
  --neverd build-mobile/bin/neverd --java-home "$JAVA_HOME" --d8 "$NEVERD_D8" \
  --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
  --work-dir "$RUNNER_TEMP/android-projected-seed-carrier-sdk"

profile_group "Execute checked reference return slots" 21 300
python scripts/test_mobile_android_checked_reference_return.py \
  --neverd build-mobile/bin/neverd --java-home "$JAVA_HOME" --d8 "$NEVERD_D8" \
  --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
  --work-dir "$RUNNER_TEMP/android-checked-reference-return"
python scripts/test_mobile_android_checked_reference_return.py --mangled \
  --neverd build-mobile/bin/neverd --java-home "$JAVA_HOME" --d8 "$NEVERD_D8" \
  --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
  --work-dir "$RUNNER_TEMP/android-checked-reference-return-mangled"
python scripts/test_mobile_android_checked_reference_return.py --mangled --dependency-callback \
  --neverd build-mobile/bin/neverd --java-home "$JAVA_HOME" --d8 "$NEVERD_D8" \
  --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
  --work-dir "$RUNNER_TEMP/android-checked-reference-return-sdk"

profile_group "Execute static method anonymous enclosures" 25 600
python scripts/test_mobile_android_static_method_enclosures.py \
  --neverd build-mobile/bin/neverd --java-home "$JAVA_HOME" --d8 "$NEVERD_D8" \
  --kotlin-java-home "$NEVERD_KOTLIN_JAVA_HOME" \
  --kotlin-lib "$RUNNER_TEMP/neverd-kotlin-2.0.21/kotlinc/lib" \
  --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
  --work-dir "$RUNNER_TEMP/android-static-method-enclosures"

profile_group "Execute private instance method names" 21 600
python scripts/test_mobile_android_private_instance_names.py \
  --neverd build-mobile/bin/neverd --java-home "$JAVA_HOME" --d8 "$NEVERD_D8" \
  --kotlin-lib "$RUNNER_TEMP/neverd-kotlin-2.0.21/kotlinc/lib" \
  --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
  --work-dir "$RUNNER_TEMP/android-private-instance-names"
python scripts/test_mobile_android_private_instance_names.py --synthetic \
  --neverd build-mobile/bin/neverd --java-home "$JAVA_HOME" --d8 "$NEVERD_D8" \
  --kotlin-lib "$RUNNER_TEMP/neverd-kotlin-2.0.21/kotlinc/lib" \
  --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
  --work-dir "$RUNNER_TEMP/android-private-instance-names-synthetic"

profile_group "Execute bounded captured factories" 25 300
python scripts/test_mobile_android_bounded_captured_factory.py \
  --neverd build-mobile/bin/neverd --java-home "$JAVA_HOME" --d8 "$NEVERD_D8" \
  --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
  --work-dir "$RUNNER_TEMP/android-bounded-captured-factory"
python scripts/test_mobile_android_bounded_captured_factory.py --dependency-factory \
  --neverd build-mobile/bin/neverd --java-home "$JAVA_HOME" --d8 "$NEVERD_D8" \
  --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
  --work-dir "$RUNNER_TEMP/android-bounded-captured-factory-sdk"

profile_group "Execute discarded allocation results" 21 300
python scripts/test_mobile_android_allocation_statements.py --discarded-result object \
  --neverd build-mobile/bin/neverd --java-home "$JAVA_HOME" --d8 "$NEVERD_D8" \
  --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
  --work-dir "$RUNNER_TEMP/android-allocation-discarded-object"
python scripts/test_mobile_android_allocation_statements.py --discarded-result object --dependency-provider \
  --neverd build-mobile/bin/neverd --java-home "$JAVA_HOME" --d8 "$NEVERD_D8" \
  --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
  --work-dir "$RUNNER_TEMP/android-allocation-discarded-object-sdk"
python scripts/test_mobile_android_allocation_statements.py --discarded-result integer \
  --neverd build-mobile/bin/neverd --java-home "$JAVA_HOME" --d8 "$NEVERD_D8" \
  --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
  --work-dir "$RUNNER_TEMP/android-allocation-discarded-integer"
python scripts/test_mobile_android_allocation_statements.py --discarded-result integer --dependency-provider \
  --neverd build-mobile/bin/neverd --java-home "$JAVA_HOME" --d8 "$NEVERD_D8" \
  --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
  --work-dir "$RUNNER_TEMP/android-allocation-discarded-integer-sdk"
python scripts/test_mobile_android_allocation_statements.py --discarded-result wide \
  --neverd build-mobile/bin/neverd --java-home "$JAVA_HOME" --d8 "$NEVERD_D8" \
  --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
  --work-dir "$RUNNER_TEMP/android-allocation-discarded-wide"
python scripts/test_mobile_android_allocation_statements.py --discarded-result wide --dependency-provider \
  --neverd build-mobile/bin/neverd --java-home "$JAVA_HOME" --d8 "$NEVERD_D8" \
  --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
  --work-dir "$RUNNER_TEMP/android-allocation-discarded-wide-sdk"

profile_group "Execute separate carrier name families" 21 300
python scripts/test_mobile_android_carrier_target_names.py --separate-family \
  --neverd build-mobile/bin/neverd --java-home "$JAVA_HOME" --d8 "$NEVERD_D8" \
  --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
  --work-dir "$RUNNER_TEMP/android-separated-carrier-family"
python scripts/test_mobile_android_carrier_target_names.py --separate-family --dependency-carriers \
  --neverd build-mobile/bin/neverd --java-home "$JAVA_HOME" --d8 "$NEVERD_D8" \
  --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
  --work-dir "$RUNNER_TEMP/android-separated-carrier-family-sdk"

profile_group "Execute separate carrier family enclosures" 25 300
python scripts/test_mobile_android_carrier_target_names.py --separate-family --enclosing \
  --neverd build-mobile/bin/neverd --java-home "$JAVA_HOME" --d8 "$NEVERD_D8" \
  --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
  --work-dir "$RUNNER_TEMP/android-separated-carrier-family-enclosing"
python scripts/test_mobile_android_carrier_target_names.py --separate-family --enclosing --dependency-carriers \
  --neverd build-mobile/bin/neverd --java-home "$JAVA_HOME" --d8 "$NEVERD_D8" \
  --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
  --work-dir "$RUNNER_TEMP/android-separated-carrier-family-enclosing-sdk"

profile_group "Execute raw primitive bridges with closed tails" 21 300
python scripts/test_mobile_android_raw_primitive_tail.py \
  --neverd build-mobile/bin/neverd --java-home "$JAVA_HOME" --d8 "$NEVERD_D8" \
  --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
  --work-dir "$RUNNER_TEMP/android-raw-primitive-tail-boolean"
python scripts/test_mobile_android_raw_primitive_tail.py --dependency-provider \
  --neverd build-mobile/bin/neverd --java-home "$JAVA_HOME" --d8 "$NEVERD_D8" \
  --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
  --work-dir "$RUNNER_TEMP/android-raw-primitive-tail-boolean-sdk"
python scripts/test_mobile_android_raw_primitive_tail.py --wide \
  --neverd build-mobile/bin/neverd --java-home "$JAVA_HOME" --d8 "$NEVERD_D8" \
  --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
  --work-dir "$RUNNER_TEMP/android-raw-primitive-tail-wide"
python scripts/test_mobile_android_raw_primitive_tail.py --wide --dependency-provider \
  --neverd build-mobile/bin/neverd --java-home "$JAVA_HOME" --d8 "$NEVERD_D8" \
  --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
  --work-dir "$RUNNER_TEMP/android-raw-primitive-tail-wide-sdk"

profile_group "Execute private seeded fixed results" 21 300
python scripts/test_mobile_android_seeded_nominal_results.py --private-helper \
  --neverd build-mobile/bin/neverd --java-home "$JAVA_HOME" --d8 "$NEVERD_D8" \
  --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
  --work-dir "$RUNNER_TEMP/android-private-seeded-nominal"
python scripts/test_mobile_android_seeded_nominal_results.py --private-helper --dependency-carrier \
  --neverd build-mobile/bin/neverd --java-home "$JAVA_HOME" --d8 "$NEVERD_D8" \
  --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
  --work-dir "$RUNNER_TEMP/android-private-seeded-nominal-sdk"

profile_group "Execute separate parent and child factory formals" 21 300
python scripts/test_mobile_android_split_seed_factory.py \
  --neverd build-mobile/bin/neverd --java-home "$JAVA_HOME" --d8 "$NEVERD_D8" \
  --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
  --work-dir "$RUNNER_TEMP/android-split-seed-factory"
python scripts/test_mobile_android_split_seed_factory.py --dependency-provider \
  --neverd build-mobile/bin/neverd --java-home "$JAVA_HOME" --d8 "$NEVERD_D8" \
  --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
  --work-dir "$RUNNER_TEMP/android-split-seed-factory-sdk"

profile_group "Execute owned interface default compatibility access" 25 600
python scripts/test_mobile_android_interface_default_access.py \
  --neverd build-mobile/bin/neverd --java-home "$JAVA_HOME" --d8 "$NEVERD_D8" \
  --kotlin-java-home "$NEVERD_KOTLIN_JAVA_HOME" \
  --kotlin-lib "$RUNNER_TEMP/neverd-kotlin-2.0.21/kotlinc/lib" \
  --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
  --work-dir "$RUNNER_TEMP/android-interface-default-access"

profile_group "Execute mangled default interface family roots" 21 600
python scripts/test_mobile_android_mangled_interface_names.py --default-root \
  --neverd build-mobile/bin/neverd --java-home "$JAVA_HOME" --d8 "$NEVERD_D8" \
  --kotlin-java-home "$NEVERD_KOTLIN_JAVA_HOME" \
  --kotlin-lib "$RUNNER_TEMP/neverd-kotlin-2.0.21/kotlinc/lib" \
  --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
  --work-dir "$RUNNER_TEMP/android-mangled-default-interface-names"
profile_group "Execute mangled default roots with local enclosures" 25 600
python scripts/test_mobile_android_mangled_interface_names.py --default-root --enclosing \
  --neverd build-mobile/bin/neverd --java-home "$JAVA_HOME" --d8 "$NEVERD_D8" \
  --kotlin-java-home "$NEVERD_KOTLIN_JAVA_HOME" \
  --kotlin-lib "$RUNNER_TEMP/neverd-kotlin-2.0.21/kotlinc/lib" \
  --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
  --work-dir "$RUNNER_TEMP/android-mangled-default-interface-enclosures"

profile_group "Execute fixed nominal default constructor parameters" 25 600
python scripts/test_mobile_android_scoped_mask_constructors.py --fixed-nominal \
  --neverd build-mobile/bin/neverd --java-home "$JAVA_HOME" --d8 "$NEVERD_D8" \
  --kotlin-java-home "$NEVERD_KOTLIN_JAVA_HOME" \
  --kotlin-lib "$RUNNER_TEMP/neverd-kotlin-2.0.21/kotlinc/lib" \
  --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
  --work-dir "$RUNNER_TEMP/android-fixed-nominal-mask-constructors"
python scripts/test_mobile_android_scoped_mask_constructors.py --fixed-nominal --dependency-defaults \
  --neverd build-mobile/bin/neverd --java-home "$JAVA_HOME" --d8 "$NEVERD_D8" \
  --kotlin-java-home "$NEVERD_KOTLIN_JAVA_HOME" \
  --kotlin-lib "$RUNNER_TEMP/neverd-kotlin-2.0.21/kotlinc/lib" \
  --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
  --work-dir "$RUNNER_TEMP/android-fixed-nominal-mask-constructors-sdk"

profile_group "Execute bidirectional bounded callback factories" 21 300
python scripts/test_mobile_android_bidirectional_factory.py \
  --neverd build-mobile/bin/neverd --java-home "$JAVA_HOME" --d8 "$NEVERD_D8" \
  --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
  --work-dir "$RUNNER_TEMP/android-bidirectional-bounded-factory"
python scripts/test_mobile_android_bidirectional_factory.py --dependency-provider \
  --neverd build-mobile/bin/neverd --java-home "$JAVA_HOME" --d8 "$NEVERD_D8" \
  --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
  --work-dir "$RUNNER_TEMP/android-bidirectional-bounded-factory-sdk"

profile_group "Execute directly seeded bounded carrier bindings" 21 300
python scripts/test_mobile_android_split_seed_factory.py --bounded-carrier \
  --neverd build-mobile/bin/neverd --java-home "$JAVA_HOME" --d8 "$NEVERD_D8" \
  --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
  --work-dir "$RUNNER_TEMP/android-seeded-bounded-carrier"
python scripts/test_mobile_android_split_seed_factory.py --bounded-carrier --dependency-provider \
  --neverd build-mobile/bin/neverd --java-home "$JAVA_HOME" --d8 "$NEVERD_D8" \
  --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
  --work-dir "$RUNNER_TEMP/android-seeded-bounded-carrier-sdk"

profile_group "Execute invariant collection and variant predicate calls" 21 300
python scripts/test_mobile_android_collection_predicate.py \
  --neverd build-mobile/bin/neverd --java-home "$JAVA_HOME" --d8 "$NEVERD_D8" \
  --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
  --work-dir "$RUNNER_TEMP/android-collection-predicate"
python scripts/test_mobile_android_collection_predicate.py --dependency-provider \
  --neverd build-mobile/bin/neverd --java-home "$JAVA_HOME" --d8 "$NEVERD_D8" \
  --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
  --work-dir "$RUNNER_TEMP/android-collection-predicate-sdk"

profile_group "Execute covariant binary input and invariant result carriers" 21 300
python scripts/test_mobile_android_flat_pair_transfer.py \
  --neverd build-mobile/bin/neverd --java-home "$JAVA_HOME" --d8 "$NEVERD_D8" \
  --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
  --work-dir "$RUNNER_TEMP/android-flat-pair-transfer"
python scripts/test_mobile_android_flat_pair_transfer.py --dependency-provider \
  --neverd build-mobile/bin/neverd --java-home "$JAVA_HOME" --d8 "$NEVERD_D8" \
  --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
  --work-dir "$RUNNER_TEMP/android-flat-pair-transfer-sdk"

profile_group "Execute nested conditional allocation argument programs" 21 300
python scripts/test_mobile_android_nested_conditional_arguments.py \
  --neverd build-mobile/bin/neverd --java-home "$JAVA_HOME" --d8 "$NEVERD_D8" \
  --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
  --work-dir "$RUNNER_TEMP/android-nested-conditional-arguments"
python scripts/test_mobile_android_nested_conditional_arguments.py --dependency-provider \
  --neverd build-mobile/bin/neverd --java-home "$JAVA_HOME" --d8 "$NEVERD_D8" \
  --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
  --work-dir "$RUNNER_TEMP/android-nested-conditional-arguments-sdk"

profile_group "Execute two complete binary Map input contracts" 21 300
python scripts/test_mobile_android_dual_map_transfer.py \
  --neverd build-mobile/bin/neverd --java-home "$JAVA_HOME" --d8 "$NEVERD_D8" \
  --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
  --work-dir "$RUNNER_TEMP/android-dual-map-transfer"
python scripts/test_mobile_android_dual_map_transfer.py --dependency-provider \
  --neverd build-mobile/bin/neverd --java-home "$JAVA_HOME" --d8 "$NEVERD_D8" \
  --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
  --work-dir "$RUNNER_TEMP/android-dual-map-transfer-sdk"

profile_group "Execute complete nested carriers with fixed nominal results" 21 300
python scripts/test_mobile_android_unseeded_nominal_carrier.py \
  --neverd build-mobile/bin/neverd --java-home "$JAVA_HOME" --d8 "$NEVERD_D8" \
  --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
  --work-dir "$RUNNER_TEMP/android-unseeded-nominal-carrier"
python scripts/test_mobile_android_unseeded_nominal_carrier.py --dependency-provider \
  --neverd build-mobile/bin/neverd --java-home "$JAVA_HOME" --d8 "$NEVERD_D8" \
  --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
  --work-dir "$RUNNER_TEMP/android-unseeded-nominal-carrier-sdk"

profile_group "Execute class/interface intersections with a closed callback" 21 600
python scripts/test_mobile_android_intersection_callback.py \
  --neverd build-mobile/bin/neverd --java-home "$JAVA_HOME" --d8 "$NEVERD_D8" \
  --kotlin-java-home "$NEVERD_KOTLIN_JAVA_HOME" \
  --kotlin-lib "$RUNNER_TEMP/neverd-kotlin-2.0.21/kotlinc/lib" \
  --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
  --work-dir "$RUNNER_TEMP/android-intersection-callback"
python scripts/test_mobile_android_intersection_callback.py --dependency-provider \
  --neverd build-mobile/bin/neverd --java-home "$JAVA_HOME" --d8 "$NEVERD_D8" \
  --kotlin-java-home "$NEVERD_KOTLIN_JAVA_HOME" \
  --kotlin-lib "$RUNNER_TEMP/neverd-kotlin-2.0.21/kotlinc/lib" \
  --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
  --work-dir "$RUNNER_TEMP/android-intersection-callback-sdk"

profile_group "Preserve original wide captured field identifiers and immutable callers" 25 600
python scripts/test_mobile_android_captured_field_names.py \
  --neverd build-mobile/bin/neverd --d8 "$NEVERD_D8" --java-home "$JAVA_HOME" \
  --kotlin-java-home "$NEVERD_KOTLIN_JAVA_HOME" \
  --kotlin-lib "$RUNNER_TEMP/neverd-kotlin-2.0.21/kotlinc/lib" \
  --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
  --work-dir "$RUNNER_TEMP/android-captured-field-names"
python scripts/test_mobile_android_captured_field_names.py --dependency-provider \
  --neverd build-mobile/bin/neverd --d8 "$NEVERD_D8" --java-home "$JAVA_HOME" \
  --kotlin-java-home "$NEVERD_KOTLIN_JAVA_HOME" \
  --kotlin-lib "$RUNNER_TEMP/neverd-kotlin-2.0.21/kotlinc/lib" \
  --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
  --work-dir "$RUNNER_TEMP/android-captured-field-names-sdk"

profile_group "Execute private instance generic anchors with fixed interface results" 21 600
python scripts/test_mobile_android_private_anchored_nominal.py \
  --neverd build-mobile/bin/neverd --java-home "$JAVA_HOME" --d8 "$NEVERD_D8" \
  --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
  --work-dir "$RUNNER_TEMP/android-private-anchored-nominal"
python scripts/test_mobile_android_private_anchored_nominal.py --dependency-carrier \
  --neverd build-mobile/bin/neverd --java-home "$JAVA_HOME" --d8 "$NEVERD_D8" \
  --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
  --work-dir "$RUNNER_TEMP/android-private-anchored-nominal-sdk"

profile_group "Execute star Comparable bounds through original static facades" 21 600
python scripts/test_mobile_android_star_comparable.py \
  --neverd build-mobile/bin/neverd --java-home "$JAVA_HOME" --d8 "$NEVERD_D8" \
  --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
  --work-dir "$RUNNER_TEMP/android-star-comparable"
python scripts/test_mobile_android_star_comparable.py --dependency-provider \
  --neverd build-mobile/bin/neverd --java-home "$JAVA_HOME" --d8 "$NEVERD_D8" \
  --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
  --work-dir "$RUNNER_TEMP/android-star-comparable-sdk"

profile_group "Execute stateless anonymous covariant value-class method names" 21 600
python scripts/test_mobile_android_stateless_covariant_names.py \
  --neverd build-mobile/bin/neverd --java-home "$JAVA_HOME" --d8 "$NEVERD_D8" \
  --kotlin-java-home "$NEVERD_KOTLIN_JAVA_HOME" \
  --kotlin-lib "$RUNNER_TEMP/neverd-kotlin-2.0.21/kotlinc/lib" \
  --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
  --work-dir "$RUNNER_TEMP/android-stateless-covariant-names"
python scripts/test_mobile_android_stateless_covariant_names.py --dependency-provider \
  --neverd build-mobile/bin/neverd --java-home "$JAVA_HOME" --d8 "$NEVERD_D8" \
  --kotlin-java-home "$NEVERD_KOTLIN_JAVA_HOME" \
  --kotlin-lib "$RUNNER_TEMP/neverd-kotlin-2.0.21/kotlinc/lib" \
  --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
  --work-dir "$RUNNER_TEMP/android-stateless-covariant-names-sdk"

profile_group "Execute bounded converter factories with two direct seeds" 21 600
python scripts/test_mobile_android_dual_seed_bounded_factory.py \
  --neverd build-mobile/bin/neverd --java-home "$JAVA_HOME" --d8 "$NEVERD_D8" \
  --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
  --work-dir "$RUNNER_TEMP/android-dual-seed-bounded-factory"
python scripts/test_mobile_android_dual_seed_bounded_factory.py --dependency-provider \
  --neverd build-mobile/bin/neverd --java-home "$JAVA_HOME" --d8 "$NEVERD_D8" \
  --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
  --work-dir "$RUNNER_TEMP/android-dual-seed-bounded-factory-sdk"

profile_group "Execute package subclass raw mask constructors" 21 600
python scripts/test_mobile_android_scoped_mask_constructors.py --fixed-nominal --package-subclass \
  --neverd build-mobile/bin/neverd --java-home "$JAVA_HOME" --d8 "$NEVERD_D8" \
  --kotlin-java-home "$NEVERD_KOTLIN_JAVA_HOME" \
  --kotlin-lib "$RUNNER_TEMP/neverd-kotlin-2.0.21/kotlinc/lib" \
  --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
  --work-dir "$RUNNER_TEMP/android-package-subclass-mask"
python scripts/test_mobile_android_scoped_mask_constructors.py --fixed-nominal --package-subclass --dependency-defaults \
  --neverd build-mobile/bin/neverd --java-home "$JAVA_HOME" --d8 "$NEVERD_D8" \
  --kotlin-java-home "$NEVERD_KOTLIN_JAVA_HOME" \
  --kotlin-lib "$RUNNER_TEMP/neverd-kotlin-2.0.21/kotlinc/lib" \
  --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
  --work-dir "$RUNNER_TEMP/android-package-subclass-mask-sdk"

profile_group "Execute long bitwise preparation in nested allocations" 21 600
python scripts/test_mobile_android_allocation_long_bitwise.py \
  --neverd build-mobile/bin/neverd --java-home "$JAVA_HOME" --d8 "$NEVERD_D8" \
  --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
  --work-dir "$RUNNER_TEMP/android-allocation-long-bitwise"
python scripts/test_mobile_android_allocation_long_bitwise.py --dependency-provider \
  --neverd build-mobile/bin/neverd --java-home "$JAVA_HOME" --d8 "$NEVERD_D8" \
  --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
  --work-dir "$RUNNER_TEMP/android-allocation-long-bitwise-sdk"

profile_group "Execute register Boolean masks with final constructor fields" 21 600
python scripts/test_mobile_android_scoped_mask_constructors.py --fixed-nominal --package-subclass --register-mask \
  --neverd build-mobile/bin/neverd --java-home "$JAVA_HOME" --d8 "$NEVERD_D8" \
  --kotlin-java-home "$NEVERD_KOTLIN_JAVA_HOME" \
  --kotlin-lib "$RUNNER_TEMP/neverd-kotlin-2.0.21/kotlinc/lib" \
  --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
  --work-dir "$RUNNER_TEMP/android-register-mask-delegation"
python scripts/test_mobile_android_scoped_mask_constructors.py --fixed-nominal --package-subclass --register-mask --dependency-defaults \
  --neverd build-mobile/bin/neverd --java-home "$JAVA_HOME" --d8 "$NEVERD_D8" \
  --kotlin-java-home "$NEVERD_KOTLIN_JAVA_HOME" \
  --kotlin-lib "$RUNNER_TEMP/neverd-kotlin-2.0.21/kotlinc/lib" \
  --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
  --work-dir "$RUNNER_TEMP/android-register-mask-delegation-sdk"

profile_group "Execute raw masks with an explicitly closed generic parent" 21 600
python scripts/test_mobile_android_scoped_mask_constructors.py --fixed-nominal --package-subclass --closed-parent \
  --neverd build-mobile/bin/neverd --java-home "$JAVA_HOME" --d8 "$NEVERD_D8" \
  --kotlin-java-home "$NEVERD_KOTLIN_JAVA_HOME" \
  --kotlin-lib "$RUNNER_TEMP/neverd-kotlin-2.0.21/kotlinc/lib" \
  --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
  --work-dir "$RUNNER_TEMP/android-closed-parent-mask"
python scripts/test_mobile_android_scoped_mask_constructors.py --fixed-nominal --package-subclass --closed-parent --dependency-defaults \
  --neverd build-mobile/bin/neverd --java-home "$JAVA_HOME" --d8 "$NEVERD_D8" \
  --kotlin-java-home "$NEVERD_KOTLIN_JAVA_HOME" \
  --kotlin-lib "$RUNNER_TEMP/neverd-kotlin-2.0.21/kotlinc/lib" \
  --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
  --work-dir "$RUNNER_TEMP/android-closed-parent-mask-sdk"

profile_group "Execute bounded star parameters in raw mask constructors" 21 600
python scripts/test_mobile_android_scoped_mask_constructors.py --fixed-nominal --star-bounded \
  --neverd build-mobile/bin/neverd --java-home "$JAVA_HOME" --d8 "$NEVERD_D8" \
  --kotlin-java-home "$NEVERD_KOTLIN_JAVA_HOME" \
  --kotlin-lib "$RUNNER_TEMP/neverd-kotlin-2.0.21/kotlinc/lib" \
  --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
  --work-dir "$RUNNER_TEMP/android-star-bounded-mask"
python scripts/test_mobile_android_scoped_mask_constructors.py --fixed-nominal --star-bounded --dependency-defaults \
  --neverd build-mobile/bin/neverd --java-home "$JAVA_HOME" --d8 "$NEVERD_D8" \
  --kotlin-java-home "$NEVERD_KOTLIN_JAVA_HOME" \
  --kotlin-lib "$RUNNER_TEMP/neverd-kotlin-2.0.21/kotlinc/lib" \
  --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
  --work-dir "$RUNNER_TEMP/android-star-bounded-mask-sdk"

profile_group "Execute raw label constructors with bounded owner formals" 21 600
python scripts/test_mobile_android_bounded_owner_label.py \
  --neverd build-mobile/bin/neverd --java-home "$JAVA_HOME" --d8 "$NEVERD_D8" \
  --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
  --work-dir "$RUNNER_TEMP/android-bounded-owner-label"
python scripts/test_mobile_android_bounded_owner_label.py --dependency-provider \
  --neverd build-mobile/bin/neverd --java-home "$JAVA_HOME" --d8 "$NEVERD_D8" \
  --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
  --work-dir "$RUNNER_TEMP/android-bounded-owner-label-sdk"

profile_group "Execute raw masks with bounded owner and carrier formals" 21 600
python scripts/test_mobile_android_bounded_owner_label.py --mask \
  --neverd build-mobile/bin/neverd --java-home "$JAVA_HOME" --d8 "$NEVERD_D8" \
  --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
  --work-dir "$RUNNER_TEMP/android-bounded-owner-mask"
python scripts/test_mobile_android_bounded_owner_label.py --mask --dependency-provider \
  --neverd build-mobile/bin/neverd --java-home "$JAVA_HOME" --d8 "$NEVERD_D8" \
  --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
  --work-dir "$RUNNER_TEMP/android-bounded-owner-mask-sdk"

profile_group "Execute seeded bounded factories with both result formals" 21 600
python scripts/test_mobile_android_paired_seed_bounded_result.py \
  --neverd build-mobile/bin/neverd --java-home "$JAVA_HOME" --d8 "$NEVERD_D8" \
  --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
  --work-dir "$RUNNER_TEMP/android-paired-seed-bounded-result"
python scripts/test_mobile_android_paired_seed_bounded_result.py --dependency-provider \
  --neverd build-mobile/bin/neverd --java-home "$JAVA_HOME" --d8 "$NEVERD_D8" \
  --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
  --work-dir "$RUNNER_TEMP/android-paired-seed-bounded-result-sdk"

profile_group "Execute projected captures with unchanged superclass consumers" 25 600
python scripts/test_mobile_android_projected_capture_super.py \
  --neverd build-mobile/bin/neverd --java-home "$JAVA_HOME" \
  --d8 "$RUNNER_TEMP/neverd-java25-d8/lib/d8.jar" \
  --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
  --work-dir "$RUNNER_TEMP/android-projected-capture-super"
python scripts/test_mobile_android_projected_capture_super.py --dependency-provider \
  --neverd build-mobile/bin/neverd --java-home "$JAVA_HOME" \
  --d8 "$RUNNER_TEMP/neverd-java25-d8/lib/d8.jar" \
  --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
  --work-dir "$RUNNER_TEMP/android-projected-capture-super-sdk"

profile_group "Execute void consumers with complete bounded input pairs" 21 600
python scripts/test_mobile_android_bounded_pair_consumer.py \
  --neverd build-mobile/bin/neverd --java-home "$JAVA_HOME" --d8 "$NEVERD_D8" \
  --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
  --work-dir "$RUNNER_TEMP/android-bounded-pair-consumer"
python scripts/test_mobile_android_bounded_pair_consumer.py --dependency-provider \
  --neverd build-mobile/bin/neverd --java-home "$JAVA_HOME" --d8 "$NEVERD_D8" \
  --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
  --work-dir "$RUNNER_TEMP/android-bounded-pair-consumer-sdk"

profile_group "Execute bounded input pairs with nested callbacks and completion consumers" 21 600
python scripts/test_mobile_android_bounded_pair_callback.py \
  --neverd build-mobile/bin/neverd --java-home "$JAVA_HOME" --d8 "$NEVERD_D8" \
  --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
  --work-dir "$RUNNER_TEMP/android-bounded-pair-callback"
python scripts/test_mobile_android_bounded_pair_callback.py --dependency-provider \
  --neverd build-mobile/bin/neverd --java-home "$JAVA_HOME" --d8 "$NEVERD_D8" \
  --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
  --work-dir "$RUNNER_TEMP/android-bounded-pair-callback-sdk"

profile_group "Execute nested callback extractors with fixed result declarations" 21 600
python scripts/test_mobile_android_nested_fixed_callback.py \
  --neverd build-mobile/bin/neverd --java-home "$JAVA_HOME" --d8 "$NEVERD_D8" \
  --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
  --work-dir "$RUNNER_TEMP/android-nested-fixed-callback"
python scripts/test_mobile_android_nested_fixed_callback.py --dependency-inputs \
  --neverd build-mobile/bin/neverd --java-home "$JAVA_HOME" --d8 "$NEVERD_D8" \
  --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
  --work-dir "$RUNNER_TEMP/android-nested-fixed-callback-sdk"

profile_group "Execute private extractors of complete unary nested carriers" 21 600
python scripts/test_mobile_android_nested_fixed_callback.py --unary \
  --neverd build-mobile/bin/neverd --java-home "$JAVA_HOME" --d8 "$NEVERD_D8" \
  --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
  --work-dir "$RUNNER_TEMP/android-unary-nested-carrier"
python scripts/test_mobile_android_nested_fixed_callback.py --unary --dependency-inputs \
  --neverd build-mobile/bin/neverd --java-home "$JAVA_HOME" --d8 "$NEVERD_D8" \
  --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
  --work-dir "$RUNNER_TEMP/android-unary-nested-carrier-sdk"

profile_group "Execute exact default accessors with unused generic interface formals" 25 600
python scripts/test_mobile_android_interface_default_access.py --generic-owner \
  --neverd build-mobile/bin/neverd --java-home "$JAVA_HOME" --d8 "$NEVERD_D8" \
  --kotlin-java-home "$NEVERD_KOTLIN_JAVA_HOME" \
  --kotlin-lib "$RUNNER_TEMP/neverd-kotlin-2.0.21/kotlinc/lib" \
  --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
  --work-dir "$RUNNER_TEMP/android-generic-owner-default-access"

profile_group "Execute private converters with an exact bounded return formal" 21 600
python scripts/test_mobile_android_bounded_converter.py \
  --neverd build-mobile/bin/neverd --java-home "$JAVA_HOME" --d8 "$NEVERD_D8" \
  --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
  --work-dir "$RUNNER_TEMP/android-bounded-converter"
python scripts/test_mobile_android_bounded_converter.py --dependency-inputs \
  --neverd build-mobile/bin/neverd --java-home "$JAVA_HOME" --d8 "$NEVERD_D8" \
  --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
  --work-dir "$RUNNER_TEMP/android-bounded-converter-sdk"

profile_group "Execute bounded owner masks with an exact unary interface edge" 21 600
python scripts/test_mobile_android_bounded_owner_label.py --mask --interface-owner \
  --neverd build-mobile/bin/neverd --java-home "$JAVA_HOME" --d8 "$NEVERD_D8" \
  --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
  --work-dir "$RUNNER_TEMP/android-bounded-interface-mask"
python scripts/test_mobile_android_bounded_owner_label.py --mask --interface-owner --dependency-provider \
  --neverd build-mobile/bin/neverd --java-home "$JAVA_HOME" --d8 "$NEVERD_D8" \
  --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
  --work-dir "$RUNNER_TEMP/android-bounded-interface-mask-sdk"

profile_group "Execute converter and two-seed factories with complete binary results" 21 600
python scripts/test_mobile_android_converter_seed_pair_factory.py \
  --neverd build-mobile/bin/neverd --java-home "$JAVA_HOME" --d8 "$NEVERD_D8" \
  --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
  --work-dir "$RUNNER_TEMP/android-converter-seed-pair-factory"
python scripts/test_mobile_android_converter_seed_pair_factory.py --dependency-provider \
  --neverd build-mobile/bin/neverd --java-home "$JAVA_HOME" --d8 "$NEVERD_D8" \
  --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
  --work-dir "$RUNNER_TEMP/android-converter-seed-pair-factory-sdk"

profile_group "Execute complete bounded pairs with independent Object and nominal seeds" 21 600
python scripts/test_mobile_android_converter_seed_pair_factory.py --bounded-input \
  --neverd build-mobile/bin/neverd --java-home "$JAVA_HOME" --d8 "$NEVERD_D8" \
  --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
  --work-dir "$RUNNER_TEMP/android-bounded-pair-transfer-seeds"
python scripts/test_mobile_android_converter_seed_pair_factory.py --bounded-input --dependency-provider \
  --neverd build-mobile/bin/neverd --java-home "$JAVA_HOME" --d8 "$NEVERD_D8" \
  --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
  --work-dir "$RUNNER_TEMP/android-bounded-pair-transfer-seeds-sdk"

profile_group "Execute final branch stores after a closed switch prefix" 21 600
python scripts/test_mobile_android_shared_else_final.py --switch-prefix \
  --neverd build-mobile/bin/neverd --java-home "$JAVA_HOME" --d8 "$NEVERD_D8" \
  --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
  --work-dir "$RUNNER_TEMP/android-final-switch-prefix"
python scripts/test_mobile_android_shared_else_final.py --switch-prefix --dependency-provider \
  --neverd build-mobile/bin/neverd --java-home "$JAVA_HOME" --d8 "$NEVERD_D8" \
  --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
  --work-dir "$RUNNER_TEMP/android-final-switch-prefix-sdk"

profile_group "Execute unwritten instance Boolean finals with mutable field reads" 21 600
python scripts/test_mobile_android_unwritten_instance_boolean.py \
  --neverd build-mobile/bin/neverd --java-home "$JAVA_HOME" --d8 "$NEVERD_D8" \
  --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
  --work-dir "$RUNNER_TEMP/android-unwritten-instance-boolean"
python scripts/test_mobile_android_unwritten_instance_boolean.py --dependency-observer \
  --neverd build-mobile/bin/neverd --java-home "$JAVA_HOME" --d8 "$NEVERD_D8" \
  --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
  --work-dir "$RUNNER_TEMP/android-unwritten-instance-boolean-sdk"

profile_group "Execute typed interface conversion before exact constructor delegation" 25 600
python scripts/test_mobile_android_typed_vectorizing_constructor.py \
  --neverd build-mobile/bin/neverd --java-home "$JAVA_HOME" --d8 "$NEVERD_D8" \
  --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
  --work-dir "$RUNNER_TEMP/android-typed-vectorizing-constructor"
python scripts/test_mobile_android_typed_vectorizing_constructor.py --dependency-contracts \
  --neverd build-mobile/bin/neverd --java-home "$JAVA_HOME" --d8 "$NEVERD_D8" \
  --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
  --work-dir "$RUNNER_TEMP/android-typed-vectorizing-constructor-sdk"

profile_group "Execute typed callback result casts before constructor delegation" 25 600
python scripts/test_mobile_android_typed_vectorizing_constructor.py --callback-input \
  --neverd build-mobile/bin/neverd --java-home "$JAVA_HOME" --d8 "$NEVERD_D8" \
  --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
  --work-dir "$RUNNER_TEMP/android-typed-callback-constructor"
python scripts/test_mobile_android_typed_vectorizing_constructor.py --callback-input --dependency-contracts \
  --neverd build-mobile/bin/neverd --java-home "$JAVA_HOME" --d8 "$NEVERD_D8" \
  --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
  --work-dir "$RUNNER_TEMP/android-typed-callback-constructor-sdk"

profile_group "Execute bounded generic default access with complete helper Signatures" 25 600
python scripts/test_mobile_android_generic_default_access.py \
  --neverd build-mobile/bin/neverd --java-home "$JAVA_HOME" --d8 "$NEVERD_D8" \
  --kotlin-java-home "$NEVERD_KOTLIN_JAVA_HOME" \
  --kotlin-lib "$RUNNER_TEMP/neverd-kotlin-2.0.21/kotlinc/lib" \
  --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
  --work-dir "$RUNNER_TEMP/android-generic-default-access"
python scripts/test_mobile_android_generic_default_access.py --dependency-contracts \
  --neverd build-mobile/bin/neverd --java-home "$JAVA_HOME" --d8 "$NEVERD_D8" \
  --kotlin-java-home "$NEVERD_KOTLIN_JAVA_HOME" \
  --kotlin-lib "$RUNNER_TEMP/neverd-kotlin-2.0.21/kotlinc/lib" \
  --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
  --work-dir "$RUNNER_TEMP/android-generic-default-access-sdk"

profile_group "Execute closed callback inputs with one shared completion formal" 21 600
python scripts/test_mobile_android_fixed_input_callback.py \
  --neverd build-mobile/bin/neverd --java-home "$JAVA_HOME" --d8 "$NEVERD_D8" \
  --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
  --work-dir "$RUNNER_TEMP/android-fixed-input-callback"
python scripts/test_mobile_android_fixed_input_callback.py --dependency-inputs \
  --neverd build-mobile/bin/neverd --java-home "$JAVA_HOME" --d8 "$NEVERD_D8" \
  --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
  --work-dir "$RUNNER_TEMP/android-fixed-input-callback-sdk"

profile_group "Execute invariant keys and exact nominal result bounds" 21 600
python scripts/test_mobile_android_same_bound_key.py \
  --neverd build-mobile/bin/neverd --java-home "$JAVA_HOME" --d8 "$NEVERD_D8" \
  --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
  --work-dir "$RUNNER_TEMP/android-same-bound-key"
python scripts/test_mobile_android_same_bound_key.py --dependency-inputs \
  --neverd build-mobile/bin/neverd --java-home "$JAVA_HOME" --d8 "$NEVERD_D8" \
  --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
  --work-dir "$RUNNER_TEMP/android-same-bound-key-sdk"
python scripts/test_mobile_android_same_bound_key.py --interface-provider \
  --neverd build-mobile/bin/neverd --java-home "$JAVA_HOME" --d8 "$NEVERD_D8" \
  --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
  --work-dir "$RUNNER_TEMP/android-same-bound-key-interface"
python scripts/test_mobile_android_same_bound_key.py --interface-provider --dependency-inputs \
  --neverd build-mobile/bin/neverd --java-home "$JAVA_HOME" --d8 "$NEVERD_D8" \
  --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
  --work-dir "$RUNNER_TEMP/android-same-bound-key-interface-sdk"

profile_group "Execute nested completion callbacks on the original interface receiver" 21 600
python scripts/test_mobile_android_nested_completion_callback.py \
  --neverd build-mobile/bin/neverd --java-home "$JAVA_HOME" --d8 "$NEVERD_D8" \
  --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
  --work-dir "$RUNNER_TEMP/android-nested-completion-callback"
python scripts/test_mobile_android_nested_completion_callback.py --dependency-inputs \
  --neverd build-mobile/bin/neverd --java-home "$JAVA_HOME" --d8 "$NEVERD_D8" \
  --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
  --work-dir "$RUNNER_TEMP/android-nested-completion-callback-sdk"

profile_group "Execute raw marker delegation in a unary generic interface owner" 25 600
python scripts/test_mobile_android_erased_constructor_delegations.py --unary-owner \
  --neverd build-mobile/bin/neverd --java-home "$JAVA_HOME" --d8 "$NEVERD_D8" \
  --kotlin-java-home "$NEVERD_KOTLIN_JAVA_HOME" \
  --kotlin-lib "$RUNNER_TEMP/neverd-kotlin-2.0.21/kotlinc/lib" \
  --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
  --work-dir "$RUNNER_TEMP/android-unary-owner-marker"

profile_group "Execute unchanged super constructors beside proven interface projections" 21 600
python scripts/test_mobile_android_projected_interface_super.py \
  --neverd build-mobile/bin/neverd --java-home "$JAVA_HOME" --d8 "$NEVERD_D8" \
  --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
  --work-dir "$RUNNER_TEMP/android-projected-interface-super"
python scripts/test_mobile_android_projected_interface_super.py --dependency-inputs \
  --neverd build-mobile/bin/neverd --java-home "$JAVA_HOME" --d8 "$NEVERD_D8" \
  --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
  --work-dir "$RUNNER_TEMP/android-projected-interface-super-sdk"

profile_group "Execute complete producer predicate and completion generic slots" 21 600
python scripts/test_mobile_android_producer_predicate_completion.py \
  --neverd build-mobile/bin/neverd --java-home "$JAVA_HOME" --d8 "$NEVERD_D8" \
  --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
  --work-dir "$RUNNER_TEMP/android-producer-predicate-completion"
python scripts/test_mobile_android_producer_predicate_completion.py --dependency-inputs \
  --neverd build-mobile/bin/neverd --java-home "$JAVA_HOME" --d8 "$NEVERD_D8" \
  --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
  --work-dir "$RUNNER_TEMP/android-producer-predicate-completion-sdk"

profile_group "Execute exact raw marker access to an abstract owner's private constructor" 21 600
python scripts/test_mobile_android_abstract_owner_marker.py \
  --neverd build-mobile/bin/neverd --java-home "$JAVA_HOME" --d8 "$NEVERD_D8" \
  --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
  --work-dir "$RUNNER_TEMP/android-abstract-owner-marker"
python scripts/test_mobile_android_abstract_owner_marker.py --dependency-inputs \
  --neverd build-mobile/bin/neverd --java-home "$JAVA_HOME" --d8 "$NEVERD_D8" \
  --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
  --work-dir "$RUNNER_TEMP/android-abstract-owner-marker-sdk"

profile_group "Execute exact marker delegation beside complete generic parent edges" 21 600
python scripts/test_mobile_android_generic_parent_marker.py \
  --neverd build-mobile/bin/neverd --java-home "$JAVA_HOME" --d8 "$NEVERD_D8" \
  --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
  --work-dir "$RUNNER_TEMP/android-generic-parent-marker"
python scripts/test_mobile_android_generic_parent_marker.py --dependency-inputs \
  --neverd build-mobile/bin/neverd --java-home "$JAVA_HOME" --d8 "$NEVERD_D8" \
  --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
  --work-dir "$RUNNER_TEMP/android-generic-parent-marker-sdk"

profile_group "Execute original typed config delegation before post-call field writes" 21 600
python scripts/test_mobile_android_typed_config_constructor.py \
  --neverd build-mobile/bin/neverd --java-home "$JAVA_HOME" --d8 "$NEVERD_D8" \
  --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
  --work-dir "$RUNNER_TEMP/android-typed-config-constructor"
python scripts/test_mobile_android_typed_config_constructor.py --dependency-inputs \
  --neverd build-mobile/bin/neverd --java-home "$JAVA_HOME" --d8 "$NEVERD_D8" \
  --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
  --work-dir "$RUNNER_TEMP/android-typed-config-constructor-sdk"

profile_group "Execute closed nominal tags beside exact nested completion callback slots" 21 600
python scripts/test_mobile_android_tagged_completion_callback.py \
  --neverd build-mobile/bin/neverd --java-home "$JAVA_HOME" --d8 "$NEVERD_D8" \
  --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
  --work-dir "$RUNNER_TEMP/android-tagged-completion-callback"
python scripts/test_mobile_android_tagged_completion_callback.py --dependency-inputs \
  --neverd build-mobile/bin/neverd --java-home "$JAVA_HOME" --d8 "$NEVERD_D8" \
  --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
  --work-dir "$RUNNER_TEMP/android-tagged-completion-callback-sdk"

profile_group "Execute independent seeded callback and completion formals with closed tags" 21 600
python scripts/test_mobile_android_seeded_tagged_completion.py \
  --neverd build-mobile/bin/neverd --java-home "$JAVA_HOME" --d8 "$NEVERD_D8" \
  --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
  --work-dir "$RUNNER_TEMP/android-seeded-tagged-completion"
python scripts/test_mobile_android_seeded_tagged_completion.py --dependency-inputs \
  --neverd build-mobile/bin/neverd --java-home "$JAVA_HOME" --d8 "$NEVERD_D8" \
  --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
  --work-dir "$RUNNER_TEMP/android-seeded-tagged-completion-sdk"

profile_group "Execute closed scope inputs and exact nested result completion slots" 21 600
python scripts/test_mobile_android_scoped_completion_callback.py \
  --neverd build-mobile/bin/neverd --java-home "$JAVA_HOME" --d8 "$NEVERD_D8" \
  --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
  --work-dir "$RUNNER_TEMP/android-scoped-completion-callback"
python scripts/test_mobile_android_scoped_completion_callback.py --dependency-inputs \
  --neverd build-mobile/bin/neverd --java-home "$JAVA_HOME" --d8 "$NEVERD_D8" \
  --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
  --work-dir "$RUNNER_TEMP/android-scoped-completion-callback-sdk"

profile_group "Execute marker delegation beside complete inherited interface formals" 21 600
python scripts/test_mobile_android_inherited_interface_marker.py \
  --neverd build-mobile/bin/neverd --java-home "$JAVA_HOME" --d8 "$NEVERD_D8" \
  --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
  --work-dir "$RUNNER_TEMP/android-inherited-interface-marker"
python scripts/test_mobile_android_inherited_interface_marker.py --dependency-inputs \
  --neverd build-mobile/bin/neverd --java-home "$JAVA_HOME" --d8 "$NEVERD_D8" \
  --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
  --work-dir "$RUNNER_TEMP/android-inherited-interface-marker-sdk"

profile_group "Execute bounded carriers beside independent frame callback result formals" 21 600
python scripts/test_mobile_android_bounded_frame_consumer.py \
  --neverd build-mobile/bin/neverd --java-home "$JAVA_HOME" --d8 "$NEVERD_D8" \
  --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
  --work-dir "$RUNNER_TEMP/android-bounded-frame-consumer"
python scripts/test_mobile_android_bounded_frame_consumer.py --dependency-inputs \
  --neverd build-mobile/bin/neverd --java-home "$JAVA_HOME" --d8 "$NEVERD_D8" \
  --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
  --work-dir "$RUNNER_TEMP/android-bounded-frame-consumer-sdk"

profile_group "Execute bounded converters with exact seeds spec callback and completion slots" 21 600
python scripts/test_mobile_android_seeded_void_callback.py \
  --neverd build-mobile/bin/neverd --java-home "$JAVA_HOME" --d8 "$NEVERD_D8" \
  --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
  --work-dir "$RUNNER_TEMP/android-seeded-void-callback"
python scripts/test_mobile_android_seeded_void_callback.py --dependency-inputs \
  --neverd build-mobile/bin/neverd --java-home "$JAVA_HOME" --d8 "$NEVERD_D8" \
  --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
  --work-dir "$RUNNER_TEMP/android-seeded-void-callback-sdk"

profile_group "Execute allocation copy gaps and equivalent exception adapters" 21 600
python scripts/test_mobile_android_allocation_handler_gap.py \
  --neverd build-mobile/bin/neverd --java-home "$JAVA_HOME" --d8 "$NEVERD_D8" \
  --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
  --work-dir "$RUNNER_TEMP/android-allocation-handler-gap"
python scripts/test_mobile_android_allocation_handler_gap.py --dependency-inputs \
  --neverd build-mobile/bin/neverd --java-home "$JAVA_HOME" --d8 "$NEVERD_D8" \
  --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
  --work-dir "$RUNNER_TEMP/android-allocation-handler-gap-sdk"

profile_group "Execute an uninitialized receiver alias and all initialized identity words" 21 600
python scripts/test_mobile_android_allocation_handler_gap.py --receiver-copy \
  --neverd build-mobile/bin/neverd --java-home "$JAVA_HOME" --d8 "$NEVERD_D8" \
  --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
  --work-dir "$RUNNER_TEMP/android-allocation-receiver-copy"

profile_group "Execute scaled bounded carriers and exact closed callback result slots" 21 600
python scripts/test_mobile_android_scaled_void_callback.py \
  --neverd build-mobile/bin/neverd --java-home "$JAVA_HOME" --d8 "$NEVERD_D8" \
  --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
  --work-dir "$RUNNER_TEMP/android-scaled-void-callback"
python scripts/test_mobile_android_scaled_void_callback.py --dependency-inputs \
  --neverd build-mobile/bin/neverd --java-home "$JAVA_HOME" --d8 "$NEVERD_D8" \
  --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
  --work-dir "$RUNNER_TEMP/android-scaled-void-callback-sdk"

profile_group "Execute bounded pairs beside unary seeds boolean paths and closed callbacks" 21 600
python scripts/test_mobile_android_boolean_seed_callback.py \
  --neverd build-mobile/bin/neverd --java-home "$JAVA_HOME" --d8 "$NEVERD_D8" \
  --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
  --work-dir "$RUNNER_TEMP/android-boolean-seed-callback"
python scripts/test_mobile_android_boolean_seed_callback.py --dependency-inputs \
  --neverd build-mobile/bin/neverd --java-home "$JAVA_HOME" --d8 "$NEVERD_D8" \
  --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
  --work-dir "$RUNNER_TEMP/android-boolean-seed-callback-sdk"

profile_group "Execute bounded pairs beside exact direct seeds specs and boolean paths" 21 600
python scripts/test_mobile_android_direct_seed_callback.py \
  --neverd build-mobile/bin/neverd --java-home "$JAVA_HOME" --d8 "$NEVERD_D8" \
  --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
  --work-dir "$RUNNER_TEMP/android-direct-seed-callback"
python scripts/test_mobile_android_direct_seed_callback.py --dependency-inputs \
  --neverd build-mobile/bin/neverd --java-home "$JAVA_HOME" --d8 "$NEVERD_D8" \
  --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
  --work-dir "$RUNNER_TEMP/android-direct-seed-callback-sdk"

profile_group "Execute two wide clocks beside complete bounded frame callback slots" 21 600
python scripts/test_mobile_android_wide_frame_callback.py \
  --neverd build-mobile/bin/neverd --java-home "$JAVA_HOME" --d8 "$NEVERD_D8" \
  --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
  --work-dir "$RUNNER_TEMP/android-wide-frame-callback"
python scripts/test_mobile_android_wide_frame_callback.py --dependency-inputs \
  --neverd build-mobile/bin/neverd --java-home "$JAVA_HOME" --d8 "$NEVERD_D8" \
  --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
  --work-dir "$RUNNER_TEMP/android-wide-frame-callback-sdk"

profile_group "Execute typed vectorizing this-delegation with two independent seeds" 25 600
python scripts/test_mobile_android_vectorized_pair_constructor.py \
  --neverd build-mobile/bin/neverd --java-home "$JAVA_HOME" --d8 "$NEVERD_D8" \
  --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
  --work-dir "$RUNNER_TEMP/android-vectorized-pair-constructor"
python scripts/test_mobile_android_vectorized_pair_constructor.py --dependency-contracts \
  --neverd build-mobile/bin/neverd --java-home "$JAVA_HOME" --d8 "$NEVERD_D8" \
  --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
  --work-dir "$RUNNER_TEMP/android-vectorized-pair-constructor-sdk"

profile_group "Execute local argument allocation before exact this-delegation" 25 600
python scripts/test_mobile_android_this_allocation_prefix.py \
  --neverd build-mobile/bin/neverd --java-home "$JAVA_HOME" --d8 "$NEVERD_D8" \
  --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
  --work-dir "$RUNNER_TEMP/android-this-allocation-prefix"
python scripts/test_mobile_android_this_allocation_prefix.py --dependency-inputs \
  --neverd build-mobile/bin/neverd --java-home "$JAVA_HOME" --d8 "$NEVERD_D8" \
  --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
  --work-dir "$RUNNER_TEMP/android-this-allocation-prefix-sdk"

profile_group "Execute checked generic ancestor inputs beside a null owner wildcard" 25 600
python scripts/test_mobile_android_nullable_owner_constructor.py \
  --neverd build-mobile/bin/neverd --java-home "$JAVA_HOME" --d8 "$NEVERD_D8" \
  --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
  --work-dir "$RUNNER_TEMP/android-nullable-owner-constructor"
python scripts/test_mobile_android_nullable_owner_constructor.py --dependency-inputs \
  --neverd build-mobile/bin/neverd --java-home "$JAVA_HOME" --d8 "$NEVERD_D8" \
  --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
  --work-dir "$RUNNER_TEMP/android-nullable-owner-constructor-sdk"

profile_group "Execute an original unary seed allocation and complete ancestor this-binding" 25 600
python scripts/test_mobile_android_allocated_unary_owner.py \
  --neverd build-mobile/bin/neverd --java-home "$JAVA_HOME" --d8 "$NEVERD_D8" \
  --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
  --work-dir "$RUNNER_TEMP/android-allocated-unary-owner"
python scripts/test_mobile_android_allocated_unary_owner.py --dependency-inputs \
  --neverd build-mobile/bin/neverd --java-home "$JAVA_HOME" --d8 "$NEVERD_D8" \
  --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
  --work-dir "$RUNNER_TEMP/android-allocated-unary-owner-sdk"

profile_group "Execute owner-formal pair predicates through genuine default accessors" 21 600
python scripts/test_mobile_android_owner_pair_default.py \
  --neverd build-mobile/bin/neverd --java-home "$JAVA_HOME" --d8 "$NEVERD_D8" \
  --kotlin-lib "$RUNNER_TEMP/kotlin/kotlinc/lib" --kotlin-java-home "$NEVERD_KOTLIN_JAVA_HOME" \
  --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
  --work-dir "$RUNNER_TEMP/android-owner-pair-default"

profile_group "Execute captured owner and bounded member input seeds in exact private scope" 21 600
python scripts/test_mobile_android_captured_pair_consumer.py \
  --neverd build-mobile/bin/neverd --java-home "$JAVA_HOME" --d8 "$NEVERD_D8" \
  --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
  --work-dir "$RUNNER_TEMP/android-captured-pair-consumer"

profile_group "Execute allocation cleanup with overwritten incoming words and exact saved origins" 21 600
python scripts/test_mobile_android_allocation_handler_gap.py --entry-frame \
  --neverd build-mobile/bin/neverd --java-home "$JAVA_HOME" --d8 "$NEVERD_D8" \
  --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
  --work-dir "$RUNNER_TEMP/android-allocation-entry-frame"

profile_group "Execute genuine scalar carrier getter names beside checked Comparable scope" 21 600
python scripts/test_mobile_android_carrier_accessor_name.py \
  --neverd build-mobile/bin/neverd --java-home "$JAVA_HOME" --d8 "$NEVERD_D8" \
  --kotlin-lib "$RUNNER_TEMP/kotlin/kotlinc/lib" --kotlin-java-home "$NEVERD_KOTLIN_JAVA_HOME" \
  --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
  --work-dir "$RUNNER_TEMP/android-carrier-accessor-name"

profile_group "Execute bounded owner input and result scopes through genuine default accessors" 21 600
python scripts/test_mobile_android_bounded_owner_default.py \
  --neverd build-mobile/bin/neverd --java-home "$JAVA_HOME" --d8 "$NEVERD_D8" \
  --kotlin-lib "$RUNNER_TEMP/kotlin/kotlinc/lib" --kotlin-java-home "$NEVERD_KOTLIN_JAVA_HOME" \
  --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
  --work-dir "$RUNNER_TEMP/android-bounded-owner-default"
python scripts/test_mobile_android_bounded_owner_default.py --dependency-contracts \
  --neverd build-mobile/bin/neverd --java-home "$JAVA_HOME" --d8 "$NEVERD_D8" \
  --kotlin-lib "$RUNNER_TEMP/kotlin/kotlinc/lib" --kotlin-java-home "$NEVERD_KOTLIN_JAVA_HOME" \
  --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
  --work-dir "$RUNNER_TEMP/android-bounded-owner-default-sdk"

profile_group "Execute nominally bounded seeds with primitive auxiliaries and actual fixed results" 21 600
python scripts/test_mobile_android_bounded_seed_scalar.py \
  --neverd build-mobile/bin/neverd --java-home "$JAVA_HOME" --d8 "$NEVERD_D8" \
  --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
  --work-dir "$RUNNER_TEMP/android-bounded-seed-scalar"
python scripts/test_mobile_android_bounded_seed_scalar.py --dependency-inputs \
  --neverd build-mobile/bin/neverd --java-home "$JAVA_HOME" --d8 "$NEVERD_D8" \
  --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
  --work-dir "$RUNNER_TEMP/android-bounded-seed-scalar-sdk"

profile_group "Execute stateful static local covariance and original constructor scopes" 21 600
python scripts/test_mobile_android_static_local_covariant.py \
  --neverd build-mobile/bin/neverd --java-home "$JAVA_HOME" --d8 "$NEVERD_D8" \
  --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
  --work-dir "$RUNNER_TEMP/android-static-local-covariant"
python scripts/test_mobile_android_static_local_covariant.py --dependency-inputs \
  --neverd build-mobile/bin/neverd --java-home "$JAVA_HOME" --d8 "$NEVERD_D8" \
  --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
  --work-dir "$RUNNER_TEMP/android-static-local-covariant-sdk"
python scripts/test_mobile_android_static_local_covariant.py --kotlin-constructor-scope \
  --neverd build-mobile/bin/neverd --java-home "$JAVA_HOME" --d8 "$NEVERD_D8" \
  --kotlin-lib "$RUNNER_TEMP/kotlin/kotlinc/lib" --kotlin-java-home "$NEVERD_KOTLIN_JAVA_HOME" \
  --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
  --work-dir "$RUNNER_TEMP/android-constructor-scope-covariant"
python scripts/test_mobile_android_static_local_covariant.py --kotlin-constructor-scope --dependency-inputs \
  --neverd build-mobile/bin/neverd --java-home "$JAVA_HOME" --d8 "$NEVERD_D8" \
  --kotlin-lib "$RUNNER_TEMP/kotlin/kotlinc/lib" --kotlin-java-home "$NEVERD_KOTLIN_JAVA_HOME" \
  --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
  --work-dir "$RUNNER_TEMP/android-constructor-scope-covariant-sdk"

profile_group "Execute bounded owner default long results through exact wide accessors" 21 600
python scripts/test_mobile_android_bounded_owner_wide_default.py \
  --neverd build-mobile/bin/neverd --java-home "$JAVA_HOME" --d8 "$NEVERD_D8" \
  --kotlin-lib "$RUNNER_TEMP/kotlin/kotlinc/lib" --kotlin-java-home "$NEVERD_KOTLIN_JAVA_HOME" \
  --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
  --work-dir "$RUNNER_TEMP/android-bounded-owner-wide-default"
python scripts/test_mobile_android_bounded_owner_wide_default.py --dependency-contracts \
  --neverd build-mobile/bin/neverd --java-home "$JAVA_HOME" --d8 "$NEVERD_D8" \
  --kotlin-lib "$RUNNER_TEMP/kotlin/kotlinc/lib" --kotlin-java-home "$NEVERD_KOTLIN_JAVA_HOME" \
  --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
  --work-dir "$RUNNER_TEMP/android-bounded-owner-wide-default-sdk"

profile_group "Execute inherited interface default accessors through complete formal chains" 21 600
python scripts/test_mobile_android_inherited_owner_default.py \
  --neverd build-mobile/bin/neverd --java-home "$JAVA_HOME" --d8 "$NEVERD_D8" \
  --kotlin-lib "$RUNNER_TEMP/kotlin/kotlinc/lib" --kotlin-java-home "$NEVERD_KOTLIN_JAVA_HOME" \
  --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
  --work-dir "$RUNNER_TEMP/android-inherited-owner-default"
python scripts/test_mobile_android_inherited_owner_default.py --dependency-contracts \
  --neverd build-mobile/bin/neverd --java-home "$JAVA_HOME" --d8 "$NEVERD_D8" \
  --kotlin-lib "$RUNNER_TEMP/kotlin/kotlinc/lib" --kotlin-java-home "$NEVERD_KOTLIN_JAVA_HOME" \
  --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
  --work-dir "$RUNNER_TEMP/android-inherited-owner-default-sdk"

profile_group "Execute genuine anonymous constructor arguments before this delegation" 21 600
python scripts/test_mobile_android_prefix_local_allocation.py \
  --neverd build-mobile/bin/neverd --java-home "$JAVA_HOME" --d8 "$NEVERD_D8" \
  --kotlin-lib "$RUNNER_TEMP/kotlin/kotlinc/lib" --kotlin-java-home "$NEVERD_KOTLIN_JAVA_HOME" \
  --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
  --work-dir "$RUNNER_TEMP/android-prefix-local-allocation"
python scripts/test_mobile_android_prefix_local_allocation.py --dependency-contracts \
  --neverd build-mobile/bin/neverd --java-home "$JAVA_HOME" --d8 "$NEVERD_D8" \
  --kotlin-lib "$RUNNER_TEMP/kotlin/kotlinc/lib" --kotlin-java-home "$NEVERD_KOTLIN_JAVA_HOME" \
  --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
  --work-dir "$RUNNER_TEMP/android-prefix-local-allocation-sdk"

profile_group "Execute compiler access constructors with one nominal owner bound" 21 600
python scripts/test_mobile_android_erased_constructor_delegations.py --nominal-owner \
  --neverd build-mobile/bin/neverd --java-home "$JAVA_HOME" --d8 "$NEVERD_D8" \
  --kotlin-lib "$RUNNER_TEMP/kotlin/kotlinc/lib" --kotlin-java-home "$NEVERD_KOTLIN_JAVA_HOME" \
  --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
  --work-dir "$RUNNER_TEMP/android-nominal-owner-marker"

profile_group "Execute original constructor argument loops before exact this delegation" 25 600
python scripts/test_mobile_android_constructor_loop_prefix.py \
  --neverd build-mobile/bin/neverd --java-home "$JAVA_HOME" \
  --d8 "$RUNNER_TEMP/neverd-java25-d8/lib/d8.jar" \
  --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
  --work-dir "$RUNNER_TEMP/android-constructor-loop-prefix"
python scripts/test_mobile_android_constructor_loop_prefix.py --dependency-inputs \
  --neverd build-mobile/bin/neverd --java-home "$JAVA_HOME" \
  --d8 "$RUNNER_TEMP/neverd-java25-d8/lib/d8.jar" \
  --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
  --work-dir "$RUNNER_TEMP/android-constructor-loop-prefix-sdk"

profile_group "Execute exact object allocations inside constructor argument loops" 25 600
python scripts/test_mobile_android_constructor_loop_allocation.py \
  --neverd build-mobile/bin/neverd --java-home "$JAVA_HOME" \
  --d8 "$RUNNER_TEMP/neverd-java25-d8/lib/d8.jar" \
  --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
  --work-dir "$RUNNER_TEMP/android-constructor-loop-allocation"
python scripts/test_mobile_android_constructor_loop_allocation.py --dependency-inputs \
  --neverd build-mobile/bin/neverd --java-home "$JAVA_HOME" \
  --d8 "$RUNNER_TEMP/neverd-java25-d8/lib/d8.jar" \
  --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
  --work-dir "$RUNNER_TEMP/android-constructor-loop-allocation-sdk"

profile_group "Execute bounded invariant carriers with direct seeds and formal results" 21 600
python scripts/test_mobile_android_bounded_carrier_seed_result.py \
  --neverd build-mobile/bin/neverd --java-home "$JAVA_HOME" --d8 "$NEVERD_D8" \
  --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
  --work-dir "$RUNNER_TEMP/android-bounded-carrier-seed-result"
python scripts/test_mobile_android_bounded_carrier_seed_result.py --dependency-inputs \
  --neverd build-mobile/bin/neverd --java-home "$JAVA_HOME" --d8 "$NEVERD_D8" \
  --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
  --work-dir "$RUNNER_TEMP/android-bounded-carrier-seed-result-sdk"

profile_group "Execute exact nominal constructor views beside incompatible formal siblings" 21 600
python scripts/test_mobile_android_nominal_constructor_overloads.py \
  --neverd build-mobile/bin/neverd --java-home "$JAVA_HOME" --d8 "$NEVERD_D8" \
  --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
  --work-dir "$RUNNER_TEMP/android-nominal-constructor-overloads"
python scripts/test_mobile_android_nominal_constructor_overloads.py --dependency-inputs \
  --neverd build-mobile/bin/neverd --java-home "$JAVA_HOME" --d8 "$NEVERD_D8" \
  --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
  --work-dir "$RUNNER_TEMP/android-nominal-constructor-overloads-sdk"

profile_group "Execute retained reference calls inside constructor preparation" 21 600
python scripts/test_mobile_android_allocation_retained_call.py \
  --neverd build-mobile/bin/neverd --java-home "$JAVA_HOME" --d8 "$NEVERD_D8" \
  --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
  --work-dir "$RUNNER_TEMP/android-allocation-retained-call"
python scripts/test_mobile_android_allocation_retained_call.py --dependency-inputs \
  --neverd build-mobile/bin/neverd --java-home "$JAVA_HOME" --d8 "$NEVERD_D8" \
  --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
  --work-dir "$RUNNER_TEMP/android-allocation-retained-call-sdk"

profile_group "Execute fixed-arity varargs arrays of covariant binary carriers" 21 600
python scripts/test_mobile_android_paired_array_carrier.py \
  --neverd build-mobile/bin/neverd --java-home "$JAVA_HOME" --d8 "$NEVERD_D8" \
  --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
  --work-dir "$RUNNER_TEMP/android-paired-array-carrier"
python scripts/test_mobile_android_paired_array_carrier.py --dependency-inputs \
  --neverd build-mobile/bin/neverd --java-home "$JAVA_HOME" --d8 "$NEVERD_D8" \
  --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
  --work-dir "$RUNNER_TEMP/android-paired-array-carrier-sdk"

profile_group "Execute inherited unwritten boolean finals before superclass callbacks" 25 600
python scripts/test_mobile_android_inherited_unwritten_boolean.py \
  --neverd build-mobile/bin/neverd --java-home "$JAVA_HOME" --d8 "$NEVERD_D8" \
  --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
  --work-dir "$RUNNER_TEMP/android-inherited-unwritten-boolean"
python scripts/test_mobile_android_inherited_unwritten_boolean.py --dependency-observer \
  --neverd build-mobile/bin/neverd --java-home "$JAVA_HOME" --d8 "$NEVERD_D8" \
  --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
  --work-dir "$RUNNER_TEMP/android-inherited-unwritten-boolean-sdk"

profile_group "Execute inherited abstract fixed marker constructor bindings" 21 600
python scripts/test_mobile_android_inherited_fixed_marker.py \
  --neverd build-mobile/bin/neverd --java-home "$JAVA_HOME" --d8 "$NEVERD_D8" \
  --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
  --work-dir "$RUNNER_TEMP/android-inherited-fixed-marker"
python scripts/test_mobile_android_inherited_fixed_marker.py --dependency-inputs \
  --neverd build-mobile/bin/neverd --java-home "$JAVA_HOME" --d8 "$NEVERD_D8" \
  --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
  --work-dir "$RUNNER_TEMP/android-inherited-fixed-marker-sdk"

profile_group "Execute protected bounded calls on the exact lexical receiver" 21 600
python scripts/test_mobile_android_protected_bounded_receiver.py \
  --neverd build-mobile/bin/neverd --java-home "$JAVA_HOME" --d8 "$NEVERD_D8" \
  --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
  --work-dir "$RUNNER_TEMP/android-protected-bounded-receiver"
python scripts/test_mobile_android_protected_bounded_receiver.py --dependency-inputs \
  --neverd build-mobile/bin/neverd --java-home "$JAVA_HOME" --d8 "$NEVERD_D8" \
  --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
  --work-dir "$RUNNER_TEMP/android-protected-bounded-receiver-sdk"

profile_group "Execute protected final identifiers with fixed callback signatures" 21 600
python scripts/test_mobile_android_protected_fixed_mangled.py \
  --neverd build-mobile/bin/neverd --java-home "$JAVA_HOME" --d8 "$NEVERD_D8" \
  --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
  --work-dir "$RUNNER_TEMP/android-protected-fixed-mangled"
python scripts/test_mobile_android_protected_fixed_mangled.py --dependency-base \
  --neverd build-mobile/bin/neverd --java-home "$JAVA_HOME" --d8 "$NEVERD_D8" \
  --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
  --work-dir "$RUNNER_TEMP/android-protected-fixed-mangled-sdk"

profile_group "Execute protected identifiers with owned anonymous enclosures" 21 600
python scripts/test_mobile_android_protected_fixed_mangled.py --enclosing \
  --neverd build-mobile/bin/neverd --java-home "$JAVA_HOME" --d8 "$NEVERD_D8" \
  --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
  --work-dir "$RUNNER_TEMP/android-protected-enclosing-names"
python scripts/test_mobile_android_protected_fixed_mangled.py --enclosing --dependency-base \
  --neverd build-mobile/bin/neverd --java-home "$JAVA_HOME" --d8 "$NEVERD_D8" \
  --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
  --work-dir "$RUNNER_TEMP/android-protected-enclosing-names-sdk"

profile_group "Execute complete protected abstract identifier families" 21 600
python scripts/test_mobile_android_protected_abstract_names.py \
  --neverd build-mobile/bin/neverd --java-home "$JAVA_HOME" --d8 "$NEVERD_D8" \
  --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
  --work-dir "$RUNNER_TEMP/android-protected-abstract-names"
python scripts/test_mobile_android_protected_abstract_names.py --dependency-base \
  --neverd build-mobile/bin/neverd --java-home "$JAVA_HOME" --d8 "$NEVERD_D8" \
  --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
  --work-dir "$RUNNER_TEMP/android-protected-abstract-names-sdk"

profile_group "Execute public abstract class and independent interface name roots" 21 600
python scripts/test_mobile_android_protected_abstract_names.py --public-family \
  --neverd build-mobile/bin/neverd --java-home "$JAVA_HOME" --d8 "$NEVERD_D8" \
  --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
  --work-dir "$RUNNER_TEMP/android-public-abstract-names"
python scripts/test_mobile_android_protected_abstract_names.py --public-family --dependency-base \
  --neverd build-mobile/bin/neverd --java-home "$JAVA_HOME" --d8 "$NEVERD_D8" \
  --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
  --work-dir "$RUNNER_TEMP/android-public-abstract-names-sdk"

profile_group "Execute void carriers with a single nominal method bound" 21 600
python scripts/test_mobile_android_bounded_void_carrier.py \
  --neverd build-mobile/bin/neverd --java-home "$JAVA_HOME" --d8 "$NEVERD_D8" \
  --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
  --work-dir "$RUNNER_TEMP/android-bounded-void-carrier"
python scripts/test_mobile_android_bounded_void_carrier.py --dependency-inputs \
  --neverd build-mobile/bin/neverd --java-home "$JAVA_HOME" --d8 "$NEVERD_D8" \
  --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
  --work-dir "$RUNNER_TEMP/android-bounded-void-carrier-sdk"

profile_group "Execute pure constructor receiver copies through the shared origin proof" 21 600
python scripts/test_mobile_android_pure_allocation_receiver_copy.py \
  --neverd build-mobile/bin/neverd --java-home "$JAVA_HOME" --d8 "$NEVERD_D8" \
  --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
  --work-dir "$RUNNER_TEMP/android-pure-allocation-receiver-copy"
python scripts/test_mobile_android_pure_allocation_receiver_copy.py --chain --early \
  --neverd build-mobile/bin/neverd --java-home "$JAVA_HOME" --d8 "$NEVERD_D8" \
  --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
  --work-dir "$RUNNER_TEMP/android-pure-allocation-receiver-copy-chain"

profile_group "Execute closed parent marker constructors with owned package type arguments" 21 600
python scripts/test_mobile_android_closed_parent_marker.py \
  --neverd build-mobile/bin/neverd --java-home "$JAVA_HOME" --d8 "$NEVERD_D8" \
  --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
  --work-dir "$RUNNER_TEMP/android-closed-parent-marker"
python scripts/test_mobile_android_closed_parent_marker.py --dependency-inputs \
  --neverd build-mobile/bin/neverd --java-home "$JAVA_HOME" --d8 "$NEVERD_D8" \
  --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
  --work-dir "$RUNNER_TEMP/android-closed-parent-marker-sdk"

profile_group "Execute package owners with unwritten private Boolean finals" 25 600
python scripts/test_mobile_android_inherited_unwritten_boolean.py --package-owner \
  --neverd build-mobile/bin/neverd --java-home "$JAVA_HOME" --d8 "$NEVERD_D8" \
  --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
  --work-dir "$RUNNER_TEMP/android-package-unwritten-boolean"
python scripts/test_mobile_android_inherited_unwritten_boolean.py --package-owner --dependency-observer \
  --neverd build-mobile/bin/neverd --java-home "$JAVA_HOME" --d8 "$NEVERD_D8" \
  --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
  --work-dir "$RUNNER_TEMP/android-package-unwritten-boolean-sdk"

profile_group "Execute intersection callback witnesses at argument copy entries" 21 600
python scripts/test_mobile_android_intersection_callback.py --branch-entry \
  --neverd build-mobile/bin/neverd --java-home "$JAVA_HOME" --d8 "$NEVERD_D8" \
  --kotlin-java-home "$NEVERD_KOTLIN_JAVA_HOME" \
  --kotlin-lib "$RUNNER_TEMP/neverd-kotlin-2.0.21/kotlinc/lib" \
  --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
  --work-dir "$RUNNER_TEMP/android-intersection-copy-entry"
python scripts/test_mobile_android_intersection_callback.py --branch-entry --dependency-provider \
  --neverd build-mobile/bin/neverd --java-home "$JAVA_HOME" --d8 "$NEVERD_D8" \
  --kotlin-java-home "$NEVERD_KOTLIN_JAVA_HOME" \
  --kotlin-lib "$RUNNER_TEMP/neverd-kotlin-2.0.21/kotlinc/lib" \
  --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
  --work-dir "$RUNNER_TEMP/android-intersection-copy-entry-sdk"

profile_group "Execute scalar interface super bridges with complete naming families" 21 600
python scripts/test_mobile_android_interface_super_scalars.py \
  --neverd build-mobile/bin/neverd --java-home "$JAVA_HOME" --d8 "$NEVERD_D8" \
  --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
  --work-dir "$RUNNER_TEMP/android-interface-super-scalars"
python scripts/test_mobile_android_interface_super_scalars.py --dependency-trace \
  --neverd build-mobile/bin/neverd --java-home "$JAVA_HOME" --d8 "$NEVERD_D8" \
  --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
  --work-dir "$RUNNER_TEMP/android-interface-super-scalars-sdk"

profile_group "Execute scoped nested completions through original instance receivers" 21 600
python scripts/test_mobile_android_scoped_completion_callback.py --instance-provider \
  --neverd build-mobile/bin/neverd --java-home "$JAVA_HOME" --d8 "$NEVERD_D8" \
  --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
  --work-dir "$RUNNER_TEMP/android-instance-scoped-completion"
python scripts/test_mobile_android_scoped_completion_callback.py --instance-provider --dependency-inputs \
  --neverd build-mobile/bin/neverd --java-home "$JAVA_HOME" --d8 "$NEVERD_D8" \
  --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
  --work-dir "$RUNNER_TEMP/android-instance-scoped-completion-sdk"

profile_group "Execute scoped nested completions with original leading scalar words" 21 600
python scripts/test_mobile_android_scoped_completion_callback.py --leading-word \
  --neverd build-mobile/bin/neverd --java-home "$JAVA_HOME" --d8 "$NEVERD_D8" \
  --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
  --work-dir "$RUNNER_TEMP/android-scalar-scoped-completion"
python scripts/test_mobile_android_scoped_completion_callback.py --leading-word --dependency-inputs \
  --neverd build-mobile/bin/neverd --java-home "$JAVA_HOME" --d8 "$NEVERD_D8" \
  --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
  --work-dir "$RUNNER_TEMP/android-scalar-scoped-completion-sdk"

profile_group "Execute closed private marker delegation on concrete open owners" 21 600
python scripts/test_mobile_android_inherited_fixed_marker.py --open-owner \
  --neverd build-mobile/bin/neverd --java-home "$JAVA_HOME" --d8 "$NEVERD_D8" \
  --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
  --work-dir "$RUNNER_TEMP/android-open-fixed-marker"
python scripts/test_mobile_android_inherited_fixed_marker.py --open-owner --dependency-inputs \
  --neverd build-mobile/bin/neverd --java-home "$JAVA_HOME" --d8 "$NEVERD_D8" \
  --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
  --work-dir "$RUNNER_TEMP/android-open-fixed-marker-sdk"

profile_group "Execute closed private marker delegation on package final owners" 21 600
python scripts/test_mobile_android_inherited_fixed_marker.py --final-owner \
  --neverd build-mobile/bin/neverd --java-home "$JAVA_HOME" --d8 "$NEVERD_D8" \
  --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
  --work-dir "$RUNNER_TEMP/android-final-fixed-marker"
python scripts/test_mobile_android_inherited_fixed_marker.py --final-owner --dependency-inputs \
  --neverd build-mobile/bin/neverd --java-home "$JAVA_HOME" --d8 "$NEVERD_D8" \
  --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
  --work-dir "$RUNNER_TEMP/android-final-fixed-marker-sdk"

profile_group "Execute seeded callbacks returning the same Object-erased formal" 21 600
python scripts/test_mobile_android_seeded_element_callback.py \
  --neverd build-mobile/bin/neverd --java-home "$JAVA_HOME" --d8 "$NEVERD_D8" \
  --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
  --work-dir "$RUNNER_TEMP/android-seeded-element-callback"
python scripts/test_mobile_android_seeded_element_callback.py --dependency-provider \
  --neverd build-mobile/bin/neverd --java-home "$JAVA_HOME" --d8 "$NEVERD_D8" \
  --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
  --work-dir "$RUNNER_TEMP/android-seeded-element-callback-sdk"

profile_group "Execute invariant bound keys with explicit static contexts" 21 600
python scripts/test_mobile_android_same_bound_key.py --static-context \
  --neverd build-mobile/bin/neverd --java-home "$JAVA_HOME" --d8 "$NEVERD_D8" \
  --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
  --work-dir "$RUNNER_TEMP/android-contextual-bound-key"
python scripts/test_mobile_android_same_bound_key.py --static-context --dependency-inputs \
  --neverd build-mobile/bin/neverd --java-home "$JAVA_HOME" --d8 "$NEVERD_D8" \
  --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
  --work-dir "$RUNNER_TEMP/android-contextual-bound-key-sdk"

profile_group "Execute inherited final markers with closed variant callbacks" 21 600
python scripts/test_mobile_android_closed_parent_marker.py --plain-parent \
  --neverd build-mobile/bin/neverd --java-home "$JAVA_HOME" --d8 "$NEVERD_D8" \
  --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
  --work-dir "$RUNNER_TEMP/android-inherited-variant-marker"
python scripts/test_mobile_android_closed_parent_marker.py --plain-parent --dependency-inputs \
  --neverd build-mobile/bin/neverd --java-home "$JAVA_HOME" --d8 "$NEVERD_D8" \
  --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
  --work-dir "$RUNNER_TEMP/android-inherited-variant-marker-sdk"

profile_group "Execute scoped completions with original leading reference contexts" 21 600
python scripts/test_mobile_android_scoped_completion_callback.py --leading-context \
  --neverd build-mobile/bin/neverd --java-home "$JAVA_HOME" --d8 "$NEVERD_D8" \
  --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
  --work-dir "$RUNNER_TEMP/android-context-scoped-completion"
python scripts/test_mobile_android_scoped_completion_callback.py --leading-context --dependency-inputs \
  --neverd build-mobile/bin/neverd --java-home "$JAVA_HOME" --d8 "$NEVERD_D8" \
  --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
  --work-dir "$RUNNER_TEMP/android-context-scoped-completion-sdk"

profile_group "Execute producer callbacks with matching fixed completion types" 21 600
python scripts/test_mobile_android_producer_predicate_completion.py --fixed-completion \
  --neverd build-mobile/bin/neverd --java-home "$JAVA_HOME" --d8 "$NEVERD_D8" \
  --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
  --work-dir "$RUNNER_TEMP/android-producer-fixed-completion"
python scripts/test_mobile_android_producer_predicate_completion.py --fixed-completion --dependency-inputs \
  --neverd build-mobile/bin/neverd --java-home "$JAVA_HOME" --d8 "$NEVERD_D8" \
  --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
  --work-dir "$RUNNER_TEMP/android-producer-fixed-completion-sdk"

profile_group "Execute recursive comparable ranges with original formal results" 21 600
python scripts/test_mobile_android_recursive_range_result.py \
  --neverd build-mobile/bin/neverd --java-home "$JAVA_HOME" --d8 "$NEVERD_D8" \
  --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
  --work-dir "$RUNNER_TEMP/android-recursive-range-result"
python scripts/test_mobile_android_recursive_range_result.py --dependency-provider \
  --neverd build-mobile/bin/neverd --java-home "$JAVA_HOME" --d8 "$NEVERD_D8" \
  --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
  --work-dir "$RUNNER_TEMP/android-recursive-range-result-sdk"

profile_group "Execute inherited recursive comparable range results" 21 600
python scripts/test_mobile_android_recursive_range_result.py --inherited-owner \
  --neverd build-mobile/bin/neverd --java-home "$JAVA_HOME" --d8 "$NEVERD_D8" \
  --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
  --work-dir "$RUNNER_TEMP/android-inherited-recursive-range-result"
python scripts/test_mobile_android_recursive_range_result.py --inherited-owner --dependency-provider \
  --neverd build-mobile/bin/neverd --java-home "$JAVA_HOME" --d8 "$NEVERD_D8" \
  --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
  --work-dir "$RUNNER_TEMP/android-inherited-recursive-range-result-sdk"

profile_group "Execute contextual bidirectional callback factories" 21 600
python scripts/test_mobile_android_bidirectional_factory.py --contextual \
  --neverd build-mobile/bin/neverd --java-home "$JAVA_HOME" --d8 "$NEVERD_D8" \
  --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
  --work-dir "$RUNNER_TEMP/android-contextual-bidirectional-factory"
python scripts/test_mobile_android_bidirectional_factory.py --contextual --dependency-provider \
  --neverd build-mobile/bin/neverd --java-home "$JAVA_HOME" --d8 "$NEVERD_D8" \
  --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
  --work-dir "$RUNNER_TEMP/android-contextual-bidirectional-factory-sdk"

profile_group "Execute exact private static field source names" 21 600
python scripts/test_mobile_android_private_static_field_names.py \
  --neverd build-mobile/bin/neverd --java-home "$JAVA_HOME" --d8 "$NEVERD_D8" \
  --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
  --work-dir "$RUNNER_TEMP/android-private-static-field-names"
python scripts/test_mobile_android_private_static_field_names.py --dependency-observer \
  --neverd build-mobile/bin/neverd --java-home "$JAVA_HOME" --d8 "$NEVERD_D8" \
  --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
  --work-dir "$RUNNER_TEMP/android-private-static-field-names-sdk"

profile_group "Execute closed constructor delegation after wide allocation" 25 600
python scripts/test_mobile_android_closed_allocating_constructor.py \
  --neverd build-mobile/bin/neverd --java-home "$JAVA_HOME" --d8 "$NEVERD_D8" \
  --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
  --work-dir "$RUNNER_TEMP/android-closed-allocating-constructor"
python scripts/test_mobile_android_closed_allocating_constructor.py --dependency-inputs \
  --neverd build-mobile/bin/neverd --java-home "$JAVA_HOME" --d8 "$NEVERD_D8" \
  --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
  --work-dir "$RUNNER_TEMP/android-closed-allocating-constructor-sdk"

profile_group "Execute overloaded scalar interface super dispatch" 21 600
python scripts/test_mobile_android_interface_super_scalars.py --overloaded \
  --neverd build-mobile/bin/neverd --java-home "$JAVA_HOME" --d8 "$NEVERD_D8" \
  --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
  --work-dir "$RUNNER_TEMP/android-interface-super-scalar-overloads"
python scripts/test_mobile_android_interface_super_scalars.py --overloaded --dependency-trace \
  --neverd build-mobile/bin/neverd --java-home "$JAVA_HOME" --d8 "$NEVERD_D8" \
  --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
  --work-dir "$RUNNER_TEMP/android-interface-super-scalar-overloads-sdk"

profile_group "Execute raw masks under nested generic parent arguments" 25 600
python scripts/test_mobile_android_nested_parent_mask.py \
  --neverd build-mobile/bin/neverd --java-home "$JAVA_HOME" --d8 "$NEVERD_D8" \
  --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
  --work-dir "$RUNNER_TEMP/android-nested-parent-mask"
python scripts/test_mobile_android_nested_parent_mask.py --dependency-inputs \
  --neverd build-mobile/bin/neverd --java-home "$JAVA_HOME" --d8 "$NEVERD_D8" \
  --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
  --work-dir "$RUNNER_TEMP/android-nested-parent-mask-sdk"

profile_group "Execute seeded carriers with complete closed input contracts" 21 600
python scripts/test_mobile_android_seeded_closed_inputs.py \
  --neverd build-mobile/bin/neverd --java-home "$JAVA_HOME" --d8 "$NEVERD_D8" \
  --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
  --work-dir "$RUNNER_TEMP/android-seeded-closed-inputs"
python scripts/test_mobile_android_seeded_closed_inputs.py --dependency-provider \
  --neverd build-mobile/bin/neverd --java-home "$JAVA_HOME" --d8 "$NEVERD_D8" \
  --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
  --work-dir "$RUNNER_TEMP/android-seeded-closed-inputs-sdk"

profile_group "Execute invariant inputs with private formal results" 21 600
python scripts/test_mobile_android_anchored_element_inputs.py \
  --neverd build-mobile/bin/neverd --java-home "$JAVA_HOME" --d8 "$NEVERD_D8" \
  --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
  --work-dir "$RUNNER_TEMP/android-anchored-element-inputs"
python scripts/test_mobile_android_anchored_element_inputs.py --dependency-inputs \
  --neverd build-mobile/bin/neverd --java-home "$JAVA_HOME" --d8 "$NEVERD_D8" \
  --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
  --work-dir "$RUNNER_TEMP/android-anchored-element-inputs-sdk"

profile_group "Execute private nullary factories with owned package carriers" 21 600
python scripts/test_mobile_android_private_nullary_carriers.py \
  --neverd build-mobile/bin/neverd --java-home "$JAVA_HOME" --d8 "$NEVERD_D8" \
  --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
  --work-dir "$RUNNER_TEMP/android-private-nullary-carriers"
python scripts/test_mobile_android_private_nullary_carriers.py --dependency-view \
  --neverd build-mobile/bin/neverd --java-home "$JAVA_HOME" --d8 "$NEVERD_D8" \
  --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
  --work-dir "$RUNNER_TEMP/android-private-nullary-carriers-sdk"

profile_group "Execute private producers with complete nested completions" 21 600
for completion in formal fixed; do
  completion_args=()
  if [[ "$completion" == fixed ]]; then completion_args=(--fixed-completion); fi
  python scripts/test_mobile_android_producer_predicate_completion.py --private-provider "${completion_args[@]}" \
    --neverd build-mobile/bin/neverd --java-home "$JAVA_HOME" --d8 "$NEVERD_D8" \
    --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
    --work-dir "$RUNNER_TEMP/android-private-producer-$completion"
  python scripts/test_mobile_android_producer_predicate_completion.py --private-provider "${completion_args[@]}" --dependency-inputs \
    --neverd build-mobile/bin/neverd --java-home "$JAVA_HOME" --d8 "$NEVERD_D8" \
    --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
    --work-dir "$RUNNER_TEMP/android-private-producer-$completion-sdk"
done

profile_group "Execute private static anchors with fixed interface results" 21 600
python scripts/test_mobile_android_private_anchored_nominal.py --static-entry \
  --neverd build-mobile/bin/neverd --java-home "$JAVA_HOME" --d8 "$NEVERD_D8" \
  --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
  --work-dir "$RUNNER_TEMP/android-private-static-anchors"
python scripts/test_mobile_android_private_anchored_nominal.py --static-entry --dependency-carrier \
  --neverd build-mobile/bin/neverd --java-home "$JAVA_HOME" --d8 "$NEVERD_D8" \
  --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
  --work-dir "$RUNNER_TEMP/android-private-static-anchors-sdk"

profile_group "Execute unwritten typed finals in anonymous members" 21 600
python scripts/test_mobile_android_unwritten_typed_finals.py \
  --neverd build-mobile/bin/neverd --java-home "$JAVA_HOME" --d8 "$NEVERD_D8" \
  --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
  --work-dir "$RUNNER_TEMP/android-unwritten-typed-finals"
python scripts/test_mobile_android_unwritten_typed_finals.py --dependency-view \
  --neverd build-mobile/bin/neverd --java-home "$JAVA_HOME" --d8 "$NEVERD_D8" \
  --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
  --work-dir "$RUNNER_TEMP/android-unwritten-typed-finals-sdk"

profile_group "Execute method captures with unchanged superclass consumers" 25 600
for bound in nominal object; do
  bound_args=()
  if [[ "$bound" == object ]]; then bound_args=(--object-bound); fi
  python scripts/test_mobile_android_projected_capture_super.py --method-scope "${bound_args[@]}" \
    --neverd build-mobile/bin/neverd --java-home "$JAVA_HOME" \
    --d8 "$RUNNER_TEMP/neverd-java25-d8/lib/d8.jar" \
    --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
    --work-dir "$RUNNER_TEMP/android-method-capture-super-$bound"
  python scripts/test_mobile_android_projected_capture_super.py --method-scope "${bound_args[@]}" --dependency-provider \
    --neverd build-mobile/bin/neverd --java-home "$JAVA_HOME" \
    --d8 "$RUNNER_TEMP/neverd-java25-d8/lib/d8.jar" \
    --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
    --work-dir "$RUNNER_TEMP/android-method-capture-super-$bound-sdk"
done

profile_group "Execute raw static results as closed superclass arguments" 25 600
for scope in class method; do
  scope_args=()
  if [[ "$scope" == method ]]; then scope_args=(--method-scope --object-bound); fi
  python scripts/test_mobile_android_projected_capture_super.py --static-result "${scope_args[@]}" \
    --neverd build-mobile/bin/neverd --java-home "$JAVA_HOME" \
    --d8 "$RUNNER_TEMP/neverd-java25-d8/lib/d8.jar" \
    --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
    --work-dir "$RUNNER_TEMP/android-static-result-super-$scope"
  python scripts/test_mobile_android_projected_capture_super.py --static-result "${scope_args[@]}" --dependency-provider \
    --neverd build-mobile/bin/neverd --java-home "$JAVA_HOME" \
    --d8 "$RUNNER_TEMP/neverd-java25-d8/lib/d8.jar" \
    --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
    --work-dir "$RUNNER_TEMP/android-static-result-super-$scope-sdk"
done

profile_group "Execute default allocations in incoming reference slots" 25 600
python scripts/test_mobile_android_scoped_mask_constructors.py --allocated-default \
  --neverd build-mobile/bin/neverd --java-home "$JAVA_HOME" --d8 "$NEVERD_D8" \
  --kotlin-java-home "$NEVERD_KOTLIN_JAVA_HOME" \
  --kotlin-lib "$RUNNER_TEMP/neverd-kotlin-2.0.21/kotlinc/lib" \
  --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
  --work-dir "$RUNNER_TEMP/android-allocated-mask-constructors"
python scripts/test_mobile_android_scoped_mask_constructors.py --allocated-default --dependency-defaults \
  --neverd build-mobile/bin/neverd --java-home "$JAVA_HOME" --d8 "$NEVERD_D8" \
  --kotlin-java-home "$NEVERD_KOTLIN_JAVA_HOME" \
  --kotlin-lib "$RUNNER_TEMP/neverd-kotlin-2.0.21/kotlinc/lib" \
  --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
  --work-dir "$RUNNER_TEMP/android-allocated-mask-constructors-sdk"

profile_group "Execute exact owner formals in constructor delegation" 21 600
for formal in direct array; do
  python scripts/test_mobile_android_typed_config_constructor.py --formal-input "$formal" \
    --neverd build-mobile/bin/neverd --java-home "$JAVA_HOME" --d8 "$NEVERD_D8" \
    --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
    --work-dir "$RUNNER_TEMP/android-formal-delegation-$formal"
  python scripts/test_mobile_android_typed_config_constructor.py --formal-input "$formal" --dependency-inputs \
    --neverd build-mobile/bin/neverd --java-home "$JAVA_HOME" --d8 "$NEVERD_D8" \
    --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
    --work-dir "$RUNNER_TEMP/android-formal-delegation-$formal-sdk"
done

profile_group "Execute nested and direct slots of one factory formal" 21 600
for dispatch in static instance; do
  dispatch_args=()
  if [[ "$dispatch" == instance ]]; then dispatch_args=(--instance-provider); fi
  python scripts/test_mobile_android_seeded_closed_inputs.py --nested-result "${dispatch_args[@]}" \
    --neverd build-mobile/bin/neverd --java-home "$JAVA_HOME" --d8 "$NEVERD_D8" \
    --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
    --work-dir "$RUNNER_TEMP/android-nested-formal-result-$dispatch"
  python scripts/test_mobile_android_seeded_closed_inputs.py --nested-result "${dispatch_args[@]}" --dependency-provider \
    --neverd build-mobile/bin/neverd --java-home "$JAVA_HOME" --d8 "$NEVERD_D8" \
    --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
    --work-dir "$RUNNER_TEMP/android-nested-formal-result-$dispatch-sdk"
done

profile_group "Execute original package synthetic default method names" 21 1200
python scripts/test_mobile_android_synthetic_method_names.py --private-forwarder --package-default \
  --neverd build-mobile/bin/neverd --java-home "$JAVA_HOME" --d8 "$NEVERD_D8" \
  --kotlin-lib "$RUNNER_TEMP/neverd-kotlin-2.0.21/kotlinc/lib" \
  --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
  --work-dir "$RUNNER_TEMP/android-package-static-default"
python scripts/test_mobile_android_synthetic_method_names.py --private-forwarder --package-default --dependency-box \
  --neverd build-mobile/bin/neverd --java-home "$JAVA_HOME" --d8 "$NEVERD_D8" \
  --kotlin-lib "$RUNNER_TEMP/neverd-kotlin-2.0.21/kotlinc/lib" \
  --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
  --work-dir "$RUNNER_TEMP/android-package-static-default-sdk"

profile_group "Execute allocation retries with committed handler inputs" 21 600
python scripts/test_mobile_android_allocation_handler_gap.py --retry-loop \
  --neverd build-mobile/bin/neverd --java-home "$JAVA_HOME" --d8 "$NEVERD_D8" \
  --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
  --work-dir "$RUNNER_TEMP/android-allocation-handler-retry"
python scripts/test_mobile_android_allocation_handler_gap.py --retry-loop --dependency-inputs \
  --neverd build-mobile/bin/neverd --java-home "$JAVA_HOME" --d8 "$NEVERD_D8" \
  --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
  --work-dir "$RUNNER_TEMP/android-allocation-handler-retry-sdk"

profile_group "Execute typed superclass calls with closed null arguments" 21 600
for tail in nominal carrier; do
  python scripts/test_mobile_android_erased_super_constructor.py --null-tail "$tail" \
    --neverd build-mobile/bin/neverd --java-home "$JAVA_HOME" --d8 "$NEVERD_D8" \
    --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
    --work-dir "$RUNNER_TEMP/android-null-tail-super-$tail"
  python scripts/test_mobile_android_erased_super_constructor.py --null-tail "$tail" --dependency-provider \
    --neverd build-mobile/bin/neverd --java-home "$JAVA_HOME" --d8 "$NEVERD_D8" \
    --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
    --work-dir "$RUNNER_TEMP/android-null-tail-super-$tail-sdk"
done

profile_group "Execute scoped private unary carrier factories" 21 900
for dispatch in static instance; do
  python scripts/test_mobile_android_erased_factory_invocations.py --private-factory "$dispatch" \
    --neverd build-mobile/bin/neverd --java-home "$JAVA_HOME" --d8 "$NEVERD_D8" \
    --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
    --work-dir "$RUNNER_TEMP/android-private-unary-factory-$dispatch"
  python scripts/test_mobile_android_erased_factory_invocations.py --private-factory "$dispatch" --dependency-provider \
    --neverd build-mobile/bin/neverd --java-home "$JAVA_HOME" --d8 "$NEVERD_D8" \
    --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
    --work-dir "$RUNNER_TEMP/android-private-unary-factory-$dispatch-sdk"
done

profile_group "Execute allocation defaults before typed superclass preparation" 25 900
for visibility in public package; do
  visibility_args=()
  if [[ "$visibility" == package ]]; then visibility_args=(--package-owner); fi
  python scripts/test_mobile_android_inherited_unwritten_boolean.py --typed-super "${visibility_args[@]}" \
    --neverd build-mobile/bin/neverd --java-home "$JAVA_HOME" --d8 "$NEVERD_D8" \
    --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
    --work-dir "$RUNNER_TEMP/android-typed-super-default-$visibility"
  python scripts/test_mobile_android_inherited_unwritten_boolean.py --typed-super "${visibility_args[@]}" --dependency-observer \
    --neverd build-mobile/bin/neverd --java-home "$JAVA_HOME" --d8 "$NEVERD_D8" \
    --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
    --work-dir "$RUNNER_TEMP/android-typed-super-default-$visibility-sdk"
done

profile_group "Execute stateless anonymous method contracts" 21 900
for flags in bridge synthetic; do
  flag_args=()
  if [[ "$flags" == synthetic ]]; then flag_args=(--synthetic); fi
  python scripts/test_mobile_android_stateless_method_contracts.py "${flag_args[@]}" \
    --neverd build-mobile/bin/neverd --java-home "$JAVA_HOME" --d8 "$NEVERD_D8" \
    --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
    --kotlin-java-home "$NEVERD_KOTLIN_JAVA_HOME" --kotlin-lib "$RUNNER_TEMP/kotlinc/lib" \
    --work-dir "$RUNNER_TEMP/android-stateless-method-contracts-$flags"
  python scripts/test_mobile_android_stateless_method_contracts.py "${flag_args[@]}" --dependency-root \
    --neverd build-mobile/bin/neverd --java-home "$JAVA_HOME" --d8 "$NEVERD_D8" \
    --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
    --kotlin-java-home "$NEVERD_KOTLIN_JAVA_HOME" --kotlin-lib "$RUNNER_TEMP/kotlinc/lib" \
    --work-dir "$RUNNER_TEMP/android-stateless-method-contracts-$flags-sdk"
done

profile_group "Execute invariant carrier array results" 21 900
python scripts/test_mobile_android_carrier_array_results.py \
  --neverd build-mobile/bin/neverd --java-home "$JAVA_HOME" --d8 "$NEVERD_D8" \
  --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
  --work-dir "$RUNNER_TEMP/android-carrier-array-results"
python scripts/test_mobile_android_carrier_array_results.py --dependency-provider \
  --neverd build-mobile/bin/neverd --java-home "$JAVA_HOME" --d8 "$NEVERD_D8" \
  --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
  --work-dir "$RUNNER_TEMP/android-carrier-array-results-sdk"

profile_group "Execute deferred method-owned array contracts" 21 900
python scripts/test_mobile_android_carrier_array_results.py --deferred-provider \
  --neverd build-mobile/bin/neverd --java-home "$JAVA_HOME" --d8 "$NEVERD_D8" \
  --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
  --work-dir "$RUNNER_TEMP/android-deferred-carrier-array-results"
python scripts/test_mobile_android_carrier_array_results.py --deferred-provider --dependency-provider \
  --neverd build-mobile/bin/neverd --java-home "$JAVA_HOME" --d8 "$NEVERD_D8" \
  --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
  --work-dir "$RUNNER_TEMP/android-deferred-carrier-array-results-sdk"

profile_group "Execute conditional nested constructor copies" 21 900
python scripts/test_mobile_android_conditional_nested_copies.py \
  --neverd build-mobile/bin/neverd --java-home "$JAVA_HOME" --d8 "$NEVERD_D8" \
  --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
  --work-dir "$RUNNER_TEMP/android-conditional-nested-copies"
python scripts/test_mobile_android_conditional_nested_copies.py --dependency-provider \
  --neverd build-mobile/bin/neverd --java-home "$JAVA_HOME" --d8 "$NEVERD_D8" \
  --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
  --work-dir "$RUNNER_TEMP/android-conditional-nested-copies-sdk"

profile_group "Execute private targets behind raw mask constructors" 21 900
for ancestry in direct inherited; do
  ancestry_args=()
  if [[ "$ancestry" == inherited ]]; then ancestry_args+=(--package-subclass); fi
  python scripts/test_mobile_android_scoped_mask_constructors.py --fixed-nominal --private-target "${ancestry_args[@]}" \
    --neverd build-mobile/bin/neverd --java-home "$JAVA_HOME" --d8 "$NEVERD_D8" \
    --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
    --kotlin-java-home "$NEVERD_KOTLIN_JAVA_HOME" --kotlin-lib "$RUNNER_TEMP/kotlinc/lib" \
    --work-dir "$RUNNER_TEMP/android-private-mask-target-$ancestry"
  python scripts/test_mobile_android_scoped_mask_constructors.py --fixed-nominal --private-target --dependency-defaults "${ancestry_args[@]}" \
    --neverd build-mobile/bin/neverd --java-home "$JAVA_HOME" --d8 "$NEVERD_D8" \
    --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
    --kotlin-java-home "$NEVERD_KOTLIN_JAVA_HOME" --kotlin-lib "$RUNNER_TEMP/kotlinc/lib" \
    --work-dir "$RUNNER_TEMP/android-private-mask-target-$ancestry-sdk"
done

profile_group reference-array-stores 21 600
python scripts/test_mobile_android_reference_array_stores.py \
  --neverd build-mobile/bin/neverd --java-home "$JAVA_HOME" --d8 "$NEVERD_D8" \
  --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
  --work-dir "$RUNNER_TEMP/android-reference-array-stores"
python scripts/test_mobile_android_reference_array_stores.py --dependency-provider \
  --neverd build-mobile/bin/neverd --java-home "$JAVA_HOME" --d8 "$NEVERD_D8" \
  --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
  --work-dir "$RUNNER_TEMP/android-reference-array-stores-sdk"

profile_group closed-marker-variance 21 600
for owner in abstract open; do
  owner_args=()
  if [[ "$owner" == open ]]; then owner_args+=(--open-owner); fi
  python scripts/test_mobile_android_inherited_fixed_marker.py --variant-input "${owner_args[@]}" \
    --neverd build-mobile/bin/neverd --java-home "$JAVA_HOME" --d8 "$NEVERD_D8" \
    --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
    --work-dir "$RUNNER_TEMP/android-closed-marker-variance-$owner"
  python scripts/test_mobile_android_inherited_fixed_marker.py --variant-input --dependency-inputs "${owner_args[@]}" \
    --neverd build-mobile/bin/neverd --java-home "$JAVA_HOME" --d8 "$NEVERD_D8" \
    --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
    --work-dir "$RUNNER_TEMP/android-closed-marker-variance-$owner-sdk"
done

profile_group guarded-scalar-conversions 25 600
python scripts/test_mobile_android_guarded_scalar_conversions.py \
  --neverd build-mobile/bin/neverd --java-home "$JAVA_HOME" --d8 "$NEVERD_D8" \
  --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
  --work-dir "$RUNNER_TEMP/android-guarded-scalar-conversions"
python scripts/test_mobile_android_guarded_scalar_conversions.py --dependency-provider \
  --neverd build-mobile/bin/neverd --java-home "$JAVA_HOME" --d8 "$NEVERD_D8" \
  --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
  --work-dir "$RUNNER_TEMP/android-guarded-scalar-conversions-sdk"

profile_group linear-integer-prefix 25 600
python scripts/test_mobile_android_linear_integer_prefix.py \
  --neverd build-mobile/bin/neverd --java-home "$JAVA_HOME" --d8 "$NEVERD_D8" \
  --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
  --work-dir "$RUNNER_TEMP/android-linear-integer-prefix"
python scripts/test_mobile_android_linear_integer_prefix.py --dependency-provider \
  --neverd build-mobile/bin/neverd --java-home "$JAVA_HOME" --d8 "$NEVERD_D8" \
  --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
  --work-dir "$RUNNER_TEMP/android-linear-integer-prefix-sdk"

profile_group raw-dependent-bound 25 600
python scripts/test_mobile_android_raw_receiver_invocations.py --dependent-bounds \
  --neverd build-mobile/bin/neverd --java-home "$JAVA_HOME" --d8 "$NEVERD_D8" \
  --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
  --work-dir "$RUNNER_TEMP/android-raw-dependent-bound"
python scripts/test_mobile_android_raw_receiver_invocations.py --dependent-bounds --dependency-provider \
  --neverd build-mobile/bin/neverd --java-home "$JAVA_HOME" --d8 "$NEVERD_D8" \
  --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
  --work-dir "$RUNNER_TEMP/android-raw-dependent-bound-sdk"

profile_group raw-reference-unit 25 600
python scripts/test_mobile_android_raw_reference_unit.py \
  --neverd build-mobile/bin/neverd --java-home "$JAVA_HOME" --d8 "$NEVERD_D8" \
  --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
  --work-dir "$RUNNER_TEMP/android-raw-reference-unit"
python scripts/test_mobile_android_raw_reference_unit.py --dependency-provider \
  --neverd build-mobile/bin/neverd --java-home "$JAVA_HOME" --d8 "$NEVERD_D8" \
  --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
  --work-dir "$RUNNER_TEMP/android-raw-reference-unit-sdk"

profile_group default-scoped-covariance 25 600
python scripts/test_mobile_android_default_scoped_covariance.py \
  --neverd build-mobile/bin/neverd --java-home "$JAVA_HOME" --d8 "$NEVERD_D8" \
  --kotlin-java-home "$NEVERD_KOTLIN_JAVA_HOME" \
  --kotlin-lib "$RUNNER_TEMP/neverd-kotlin-2.0.21/kotlinc/lib" \
  --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
  --work-dir "$RUNNER_TEMP/android-default-scoped-covariance"
python scripts/test_mobile_android_default_scoped_covariance.py --class-lambdas --input-runtime \
  --neverd build-mobile/bin/neverd --java-home "$JAVA_HOME" --d8 "$NEVERD_D8" \
  --kotlin-java-home "$NEVERD_KOTLIN_JAVA_HOME" \
  --kotlin-lib "$RUNNER_TEMP/neverd-kotlin-2.0.21/kotlinc/lib" \
  --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
  --work-dir "$RUNNER_TEMP/android-default-scoped-covariance-input-runtime"

profile_group raw-indexed-scope 25 600
python scripts/test_mobile_android_default_scoped_covariance.py --indexed \
  --neverd build-mobile/bin/neverd --java-home "$JAVA_HOME" --d8 "$NEVERD_D8" \
  --kotlin-java-home "$NEVERD_KOTLIN_JAVA_HOME" \
  --kotlin-lib "$RUNNER_TEMP/neverd-kotlin-2.0.21/kotlinc/lib" \
  --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
  --work-dir "$RUNNER_TEMP/android-raw-indexed-scope"
python scripts/test_mobile_android_default_scoped_covariance.py --indexed --input-runtime \
  --neverd build-mobile/bin/neverd --java-home "$JAVA_HOME" --d8 "$NEVERD_D8" \
  --kotlin-java-home "$NEVERD_KOTLIN_JAVA_HOME" \
  --kotlin-lib "$RUNNER_TEMP/neverd-kotlin-2.0.21/kotlinc/lib" \
  --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
  --work-dir "$RUNNER_TEMP/android-raw-indexed-scope-input-runtime"

profile_group closed-default-access 25 600
python scripts/test_mobile_android_generic_default_access.py --closed-parameters \
  --neverd build-mobile/bin/neverd --java-home "$JAVA_HOME" --d8 "$NEVERD_D8" \
  --kotlin-java-home "$NEVERD_KOTLIN_JAVA_HOME" \
  --kotlin-lib "$RUNNER_TEMP/neverd-kotlin-2.0.21/kotlinc/lib" \
  --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
  --work-dir "$RUNNER_TEMP/android-closed-default-access"
python scripts/test_mobile_android_generic_default_access.py --closed-parameters --dependency-contracts \
  --neverd build-mobile/bin/neverd --java-home "$JAVA_HOME" --d8 "$NEVERD_D8" \
  --kotlin-java-home "$NEVERD_KOTLIN_JAVA_HOME" \
  --kotlin-lib "$RUNNER_TEMP/neverd-kotlin-2.0.21/kotlinc/lib" \
  --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
  --work-dir "$RUNNER_TEMP/android-closed-default-access-sdk"

profile_group synthetic-interface-name 25 600
for scope in plain enclosing; do
  scope_args=()
  if [[ "$scope" == enclosing ]]; then scope_args+=(--enclosing); fi
  python scripts/test_mobile_android_mangled_interface_names.py --synthetic-implementation "${scope_args[@]}" \
    --neverd build-mobile/bin/neverd --java-home "$JAVA_HOME" --d8 "$NEVERD_D8" \
    --kotlin-java-home "$NEVERD_KOTLIN_JAVA_HOME" \
    --kotlin-lib "$RUNNER_TEMP/neverd-kotlin-2.0.21/kotlinc/lib" \
    --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
    --work-dir "$RUNNER_TEMP/android-synthetic-interface-name-$scope"
done

profile_group generic-abstract-name 25 600
python scripts/test_mobile_android_protected_abstract_names.py --public-family --closed-contracts \
  --neverd build-mobile/bin/neverd --java-home "$JAVA_HOME" --d8 "$NEVERD_D8" \
  --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
  --work-dir "$RUNNER_TEMP/android-generic-abstract-name"
python scripts/test_mobile_android_protected_abstract_names.py --public-family --closed-contracts --dependency-base \
  --neverd build-mobile/bin/neverd --java-home "$JAVA_HOME" --d8 "$NEVERD_D8" \
  --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
  --work-dir "$RUNNER_TEMP/android-generic-abstract-name-sdk"

profile_group bounded-seed-factory 25 600
python scripts/test_mobile_android_bounded_seed_factory.py \
  --neverd build-mobile/bin/neverd --java-home "$JAVA_HOME" --d8 "$NEVERD_D8" \
  --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
  --work-dir "$RUNNER_TEMP/android-bounded-seed-factory"
python scripts/test_mobile_android_bounded_seed_factory.py --dependency-provider \
  --neverd build-mobile/bin/neverd --java-home "$JAVA_HOME" --d8 "$NEVERD_D8" \
  --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
  --work-dir "$RUNNER_TEMP/android-bounded-seed-factory-sdk"

profile_group bounded-covariant-seeds 25 600
python scripts/test_mobile_android_bounded_seed_factory.py --paired-seeds \
  --neverd build-mobile/bin/neverd --java-home "$JAVA_HOME" --d8 "$NEVERD_D8" \
  --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
  --work-dir "$RUNNER_TEMP/android-bounded-covariant-seeds"
python scripts/test_mobile_android_bounded_seed_factory.py --paired-seeds --dependency-provider \
  --neverd build-mobile/bin/neverd --java-home "$JAVA_HOME" --d8 "$NEVERD_D8" \
  --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
  --work-dir "$RUNNER_TEMP/android-bounded-covariant-seeds-sdk"

profile_group moved-receiver-finals 25 600
python scripts/test_mobile_android_unwritten_typed_finals.py --moved-receiver \
  --neverd build-mobile/bin/neverd --java-home "$JAVA_HOME" --d8 "$NEVERD_D8" \
  --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
  --work-dir "$RUNNER_TEMP/android-moved-receiver-finals"
python scripts/test_mobile_android_unwritten_typed_finals.py --moved-receiver --dependency-view \
  --neverd build-mobile/bin/neverd --java-home "$JAVA_HOME" --d8 "$NEVERD_D8" \
  --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
  --work-dir "$RUNNER_TEMP/android-moved-receiver-finals-sdk"

profile_group closed-nominal-marker 25 600
for marker_kind in interface class; do
  python scripts/test_mobile_android_inherited_fixed_marker.py --nominal-bound "$marker_kind" \
    --neverd build-mobile/bin/neverd --java-home "$JAVA_HOME" --d8 "$NEVERD_D8" \
    --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
    --work-dir "$RUNNER_TEMP/android-closed-nominal-marker-$marker_kind"
  python scripts/test_mobile_android_inherited_fixed_marker.py --nominal-bound "$marker_kind" --final-owner --dependency-inputs \
    --neverd build-mobile/bin/neverd --java-home "$JAVA_HOME" --d8 "$NEVERD_D8" \
    --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
    --work-dir "$RUNNER_TEMP/android-closed-nominal-marker-$marker_kind-sdk"
done

profile_group synthetic-default-body 25 600
python scripts/test_mobile_android_generic_default_access.py --closed-parameters --synthetic-default \
  --neverd build-mobile/bin/neverd --java-home "$JAVA_HOME" --d8 "$NEVERD_D8" \
  --kotlin-java-home "$NEVERD_KOTLIN_JAVA_HOME" \
  --kotlin-lib "$RUNNER_TEMP/neverd-kotlin-2.0.21/kotlinc/lib" \
  --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
  --work-dir "$RUNNER_TEMP/android-synthetic-default-body"
python scripts/test_mobile_android_generic_default_access.py --closed-parameters --synthetic-default --dependency-contracts \
  --neverd build-mobile/bin/neverd --java-home "$JAVA_HOME" --d8 "$NEVERD_D8" \
  --kotlin-java-home "$NEVERD_KOTLIN_JAVA_HOME" \
  --kotlin-lib "$RUNNER_TEMP/neverd-kotlin-2.0.21/kotlinc/lib" \
  --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
  --work-dir "$RUNNER_TEMP/android-synthetic-default-body-sdk"

profile_group raw-index-result 25 600
python scripts/test_mobile_android_default_scoped_covariance.py --plain-indexed \
  --neverd build-mobile/bin/neverd --java-home "$JAVA_HOME" --d8 "$NEVERD_D8" \
  --kotlin-java-home "$NEVERD_KOTLIN_JAVA_HOME" \
  --kotlin-lib "$RUNNER_TEMP/neverd-kotlin-2.0.21/kotlinc/lib" \
  --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
  --work-dir "$RUNNER_TEMP/android-raw-index-result"
python scripts/test_mobile_android_default_scoped_covariance.py --plain-indexed --input-runtime \
  --neverd build-mobile/bin/neverd --java-home "$JAVA_HOME" --d8 "$NEVERD_D8" \
  --kotlin-java-home "$NEVERD_KOTLIN_JAVA_HOME" \
  --kotlin-lib "$RUNNER_TEMP/neverd-kotlin-2.0.21/kotlinc/lib" \
  --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
  --work-dir "$RUNNER_TEMP/android-raw-index-result-runtime"

profile_group nested-bidirectional-factory 25 600
python scripts/test_mobile_android_bidirectional_factory.py --nested-contextual \
  --neverd build-mobile/bin/neverd --java-home "$JAVA_HOME" --d8 "$NEVERD_D8" \
  --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
  --work-dir "$RUNNER_TEMP/android-nested-bidirectional-factory"
python scripts/test_mobile_android_bidirectional_factory.py --nested-contextual --dependency-provider \
  --neverd build-mobile/bin/neverd --java-home "$JAVA_HOME" --d8 "$NEVERD_D8" \
  --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
  --work-dir "$RUNNER_TEMP/android-nested-bidirectional-factory-sdk"

profile_group scalar-field-copies 25 600
python scripts/test_mobile_android_allocation_field_aliases.py --scalar-fields \
  --neverd build-mobile/bin/neverd --java-home "$JAVA_HOME" --d8 "$NEVERD_D8" \
  --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
  --work-dir "$RUNNER_TEMP/android-scalar-field-copies"
python scripts/test_mobile_android_allocation_field_aliases.py --scalar-fields --dependency-provider \
  --neverd build-mobile/bin/neverd --java-home "$JAVA_HOME" --d8 "$NEVERD_D8" \
  --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
  --work-dir "$RUNNER_TEMP/android-scalar-field-copies-sdk"

profile_group anchored-array-void 25 600
python scripts/test_mobile_android_anchored_array_void.py \
  --neverd build-mobile/bin/neverd --java-home "$JAVA_HOME" --d8 "$NEVERD_D8" \
  --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
  --work-dir "$RUNNER_TEMP/android-anchored-array-void"
python scripts/test_mobile_android_anchored_array_void.py --dependency-inputs \
  --neverd build-mobile/bin/neverd --java-home "$JAVA_HOME" --d8 "$NEVERD_D8" \
  --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
  --work-dir "$RUNNER_TEMP/android-anchored-array-void-sdk"
python scripts/test_mobile_android_anchored_array_void.py --instance-helper \
  --neverd build-mobile/bin/neverd --java-home "$JAVA_HOME" --d8 "$NEVERD_D8" \
  --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
  --work-dir "$RUNNER_TEMP/android-anchored-array-void-instance"
python scripts/test_mobile_android_anchored_array_void.py --instance-helper --dependency-inputs \
  --neverd build-mobile/bin/neverd --java-home "$JAVA_HOME" --d8 "$NEVERD_D8" \
  --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
  --work-dir "$RUNNER_TEMP/android-anchored-array-void-instance-sdk"

profile_group array-formal-result 25 600
python scripts/test_mobile_android_array_seed_factories.py --element-result \
  --neverd build-mobile/bin/neverd --java-home "$JAVA_HOME" --d8 "$NEVERD_D8" \
  --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
  --work-dir "$RUNNER_TEMP/android-array-formal-result"
python scripts/test_mobile_android_array_seed_factories.py --element-result --dependency-provider \
  --neverd build-mobile/bin/neverd --java-home "$JAVA_HOME" --d8 "$NEVERD_D8" \
  --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
  --work-dir "$RUNNER_TEMP/android-array-formal-result-sdk"

profile_group nested-private-scalar 25 600
python scripts/test_mobile_android_private_anchored_nominal.py --nested-scalar \
  --neverd build-mobile/bin/neverd --java-home "$JAVA_HOME" --d8 "$NEVERD_D8" \
  --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
  --work-dir "$RUNNER_TEMP/android-nested-private-scalar"
python scripts/test_mobile_android_private_anchored_nominal.py --nested-scalar --dependency-carrier \
  --neverd build-mobile/bin/neverd --java-home "$JAVA_HOME" --d8 "$NEVERD_D8" \
  --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
  --work-dir "$RUNNER_TEMP/android-nested-private-scalar-sdk"
python scripts/test_mobile_android_private_anchored_nominal.py --nested-scalar --static-entry \
  --neverd build-mobile/bin/neverd --java-home "$JAVA_HOME" --d8 "$NEVERD_D8" \
  --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
  --work-dir "$RUNNER_TEMP/android-nested-private-scalar-static"
python scripts/test_mobile_android_private_anchored_nominal.py --nested-scalar --static-entry --dependency-carrier \
  --neverd build-mobile/bin/neverd --java-home "$JAVA_HOME" --d8 "$NEVERD_D8" \
  --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
  --work-dir "$RUNNER_TEMP/android-nested-private-scalar-static-sdk"

profile_group member-formal-names 25 600
python scripts/test_mobile_android_member_formal_names.py \
  --neverd build-mobile/bin/neverd --java-home "$JAVA_HOME" --d8 "$NEVERD_D8" \
  --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
  --work-dir "$RUNNER_TEMP/android-member-formal-names"
python scripts/test_mobile_android_member_formal_names.py --dependency-trace \
  --neverd build-mobile/bin/neverd --java-home "$JAVA_HOME" --d8 "$NEVERD_D8" \
  --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
  --work-dir "$RUNNER_TEMP/android-member-formal-names-sdk"
python scripts/test_mobile_android_member_formal_names.py --reserve-name \
  --neverd build-mobile/bin/neverd --java-home "$JAVA_HOME" --d8 "$NEVERD_D8" \
  --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
  --work-dir "$RUNNER_TEMP/android-member-formal-names-reserved"
python scripts/test_mobile_android_member_formal_names.py --reserve-name --dependency-trace \
  --neverd build-mobile/bin/neverd --java-home "$JAVA_HOME" --d8 "$NEVERD_D8" \
  --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
  --work-dir "$RUNNER_TEMP/android-member-formal-names-reserved-sdk"

profile_group direct-seeded-nominal 25 600
python scripts/test_mobile_android_unused_formal_invocations.py --seeded-formal \
  --neverd build-mobile/bin/neverd --java-home "$JAVA_HOME" --d8 "$NEVERD_D8" \
  --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
  --work-dir "$RUNNER_TEMP/android-direct-seeded-nominal"
python scripts/test_mobile_android_unused_formal_invocations.py --seeded-formal --dependency-provider \
  --neverd build-mobile/bin/neverd --java-home "$JAVA_HOME" --d8 "$NEVERD_D8" \
  --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
  --work-dir "$RUNNER_TEMP/android-direct-seeded-nominal-sdk"
python scripts/test_mobile_android_unused_formal_invocations.py --seeded-formal --bounded-owner \
  --neverd build-mobile/bin/neverd --java-home "$JAVA_HOME" --d8 "$NEVERD_D8" \
  --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
  --work-dir "$RUNNER_TEMP/android-direct-seeded-nominal-bounded"
python scripts/test_mobile_android_unused_formal_invocations.py --seeded-formal --bounded-owner --dependency-provider \
  --neverd build-mobile/bin/neverd --java-home "$JAVA_HOME" --d8 "$NEVERD_D8" \
  --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
  --work-dir "$RUNNER_TEMP/android-direct-seeded-nominal-bounded-sdk"

profile_group closed-owner-delegation 25 600
python scripts/test_mobile_android_closed_allocating_constructor.py --closed-owner \
  --neverd build-mobile/bin/neverd --java-home "$JAVA_HOME" --d8 "$NEVERD_D8" \
  --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
  --work-dir "$RUNNER_TEMP/android-closed-owner-delegation"
python scripts/test_mobile_android_closed_allocating_constructor.py --closed-owner --dependency-inputs \
  --neverd build-mobile/bin/neverd --java-home "$JAVA_HOME" --d8 "$NEVERD_D8" \
  --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
  --work-dir "$RUNNER_TEMP/android-closed-owner-delegation-sdk"
python scripts/test_mobile_android_closed_allocating_constructor.py --closed-owner --private-target \
  --neverd build-mobile/bin/neverd --java-home "$JAVA_HOME" --d8 "$NEVERD_D8" \
  --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
  --work-dir "$RUNNER_TEMP/android-closed-owner-delegation-private"
python scripts/test_mobile_android_closed_allocating_constructor.py --closed-owner --private-target --dependency-inputs \
  --neverd build-mobile/bin/neverd --java-home "$JAVA_HOME" --d8 "$NEVERD_D8" \
  --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
  --work-dir "$RUNNER_TEMP/android-closed-owner-delegation-private-sdk"

profile_group projected-owner-delegation 25 600
python scripts/test_mobile_android_closed_allocating_constructor.py --terminal-owner \
  --neverd build-mobile/bin/neverd --java-home "$JAVA_HOME" --d8 "$NEVERD_D8" \
  --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
  --work-dir "$RUNNER_TEMP/android-projected-owner-delegation"
python scripts/test_mobile_android_closed_allocating_constructor.py --terminal-owner --dependency-inputs \
  --neverd build-mobile/bin/neverd --java-home "$JAVA_HOME" --d8 "$NEVERD_D8" \
  --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
  --work-dir "$RUNNER_TEMP/android-projected-owner-delegation-sdk"
python scripts/test_mobile_android_closed_allocating_constructor.py --terminal-owner --private-target \
  --neverd build-mobile/bin/neverd --java-home "$JAVA_HOME" --d8 "$NEVERD_D8" \
  --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
  --work-dir "$RUNNER_TEMP/android-projected-owner-delegation-private"
python scripts/test_mobile_android_closed_allocating_constructor.py --terminal-owner --private-target --dependency-inputs \
  --neverd build-mobile/bin/neverd --java-home "$JAVA_HOME" --d8 "$NEVERD_D8" \
  --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
  --work-dir "$RUNNER_TEMP/android-projected-owner-delegation-private-sdk"

profile_group constant-cleanup-adapters 21 600
python scripts/test_mobile_android_allocation_handler_gap.py --constant-adapters \
  --neverd build-mobile/bin/neverd --java-home "$JAVA_HOME" --d8 "$NEVERD_D8" \
  --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
  --work-dir "$RUNNER_TEMP/android-constant-cleanup-adapters"
python scripts/test_mobile_android_allocation_handler_gap.py --constant-adapters --entry-frame --adapter-constant=-2147483648 \
  --neverd build-mobile/bin/neverd --java-home "$JAVA_HOME" --d8 "$NEVERD_D8" \
  --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
  --work-dir "$RUNNER_TEMP/android-constant-cleanup-adapters-entry-frame"
python scripts/test_mobile_android_allocation_handler_gap.py --constant-adapters --wide-constants \
  --neverd build-mobile/bin/neverd --java-home "$JAVA_HOME" --d8 "$NEVERD_D8" \
  --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
  --work-dir "$RUNNER_TEMP/android-constant-cleanup-adapters-wide"
python scripts/test_mobile_android_allocation_handler_gap.py --constant-adapters --wide-constants --entry-frame --adapter-constant=9221120237041090626 \
  --neverd build-mobile/bin/neverd --java-home "$JAVA_HOME" --d8 "$NEVERD_D8" \
  --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
  --work-dir "$RUNNER_TEMP/android-constant-cleanup-adapters-wide-entry-frame"

profile_group conditional-type-test 21 600
python scripts/test_mobile_android_conditional_allocation.py --type-test \
  --neverd build-mobile/bin/neverd --java-home "$JAVA_HOME" --d8 "$NEVERD_D8" \
  --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
  --work-dir "$RUNNER_TEMP/android-conditional-type-test"
python scripts/test_mobile_android_conditional_allocation.py --type-test --dependency-provider --dependency-target \
  --neverd build-mobile/bin/neverd --java-home "$JAVA_HOME" --d8 "$NEVERD_D8" \
  --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
  --work-dir "$RUNNER_TEMP/android-conditional-type-test-sdk"
python scripts/test_mobile_android_conditional_allocation.py --interface-test \
  --neverd build-mobile/bin/neverd --java-home "$JAVA_HOME" --d8 "$NEVERD_D8" \
  --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
  --work-dir "$RUNNER_TEMP/android-conditional-interface-test"
python scripts/test_mobile_android_conditional_allocation.py --interface-test --dependency-provider --dependency-target \
  --neverd build-mobile/bin/neverd --java-home "$JAVA_HOME" --d8 "$NEVERD_D8" \
  --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
  --work-dir "$RUNNER_TEMP/android-conditional-interface-test-sdk"

profile_group captured-closed-delegation 25 600
python scripts/test_mobile_android_captured_closed_delegation.py \
  --neverd build-mobile/bin/neverd --java-home "$JAVA_HOME" --d8 "$NEVERD_D8" \
  --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
  --work-dir "$RUNNER_TEMP/android-captured-closed-delegation"
python scripts/test_mobile_android_captured_closed_delegation.py --dependency-inputs \
  --neverd build-mobile/bin/neverd --java-home "$JAVA_HOME" --d8 "$NEVERD_D8" \
  --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
  --work-dir "$RUNNER_TEMP/android-captured-closed-delegation-sdk"
python scripts/test_mobile_android_captured_closed_delegation.py --public-member \
  --neverd build-mobile/bin/neverd --java-home "$JAVA_HOME" --d8 "$NEVERD_D8" \
  --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
  --work-dir "$RUNNER_TEMP/android-captured-closed-delegation-public"
python scripts/test_mobile_android_captured_closed_delegation.py --public-member --dependency-inputs \
  --neverd build-mobile/bin/neverd --java-home "$JAVA_HOME" --d8 "$NEVERD_D8" \
  --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
  --work-dir "$RUNNER_TEMP/android-captured-closed-delegation-public-sdk"

profile_group captured-access-delegation 25 600
python scripts/test_mobile_android_captured_closed_delegation.py --synthetic-access \
  --neverd build-mobile/bin/neverd --java-home "$JAVA_HOME" --d8 "$NEVERD_D8" \
  --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
  --work-dir "$RUNNER_TEMP/android-captured-access-delegation"
python scripts/test_mobile_android_captured_closed_delegation.py --synthetic-access --dependency-inputs \
  --neverd build-mobile/bin/neverd --java-home "$JAVA_HOME" --d8 "$NEVERD_D8" \
  --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
  --work-dir "$RUNNER_TEMP/android-captured-access-delegation-sdk"
python scripts/test_mobile_android_captured_closed_delegation.py --synthetic-access --public-member \
  --neverd build-mobile/bin/neverd --java-home "$JAVA_HOME" --d8 "$NEVERD_D8" \
  --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
  --work-dir "$RUNNER_TEMP/android-captured-access-delegation-public"
python scripts/test_mobile_android_captured_closed_delegation.py --synthetic-access --public-member --dependency-inputs \
  --neverd build-mobile/bin/neverd --java-home "$JAVA_HOME" --d8 "$NEVERD_D8" \
  --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
  --work-dir "$RUNNER_TEMP/android-captured-access-delegation-public-sdk"

profile_group comparison-carrier 21 600
python scripts/test_mobile_android_comparison_carrier.py --package-owner \
  --neverd build-mobile/bin/neverd --java-home "$JAVA_HOME" --d8 "$NEVERD_D8" \
  --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
  --work-dir "$RUNNER_TEMP/android-comparison-carrier"
python scripts/test_mobile_android_comparison_carrier.py --package-owner --dependency-provider \
  --neverd build-mobile/bin/neverd --java-home "$JAVA_HOME" --d8 "$NEVERD_D8" \
  --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
  --work-dir "$RUNNER_TEMP/android-comparison-carrier-sdk"
python scripts/test_mobile_android_comparison_carrier.py --bounded-owner \
  --neverd build-mobile/bin/neverd --java-home "$JAVA_HOME" --d8 "$NEVERD_D8" \
  --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
  --work-dir "$RUNNER_TEMP/android-comparison-carrier-bounded"
python scripts/test_mobile_android_comparison_carrier.py --bounded-owner --dependency-provider \
  --neverd build-mobile/bin/neverd --java-home "$JAVA_HOME" --d8 "$NEVERD_D8" \
  --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
  --work-dir "$RUNNER_TEMP/android-comparison-carrier-bounded-sdk"

profile_group filled-int-constructor 25 600
python scripts/test_mobile_android_filled_int_constructor.py  \
  --neverd build-mobile/bin/neverd --java-home "$JAVA_HOME" --d8 "$NEVERD_D8" \
  --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
  --work-dir "$RUNNER_TEMP/android-filled-int-constructor"
python scripts/test_mobile_android_filled_int_constructor.py --dependency-inputs \
  --neverd build-mobile/bin/neverd --java-home "$JAVA_HOME" --d8 "$NEVERD_D8" \
  --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
  --work-dir "$RUNNER_TEMP/android-filled-int-constructor-sdk"
python scripts/test_mobile_android_filled_int_constructor.py --shared-array \
  --neverd build-mobile/bin/neverd --java-home "$JAVA_HOME" --d8 "$NEVERD_D8" \
  --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
  --work-dir "$RUNNER_TEMP/android-filled-int-constructor-shared"
python scripts/test_mobile_android_filled_int_constructor.py --shared-array --dependency-inputs \
  --neverd build-mobile/bin/neverd --java-home "$JAVA_HOME" --d8 "$NEVERD_D8" \
  --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
  --work-dir "$RUNNER_TEMP/android-filled-int-constructor-shared-sdk"

profile_group conditional-packed 21 600
python scripts/test_mobile_android_conditional_packed.py  \
  --neverd build-mobile/bin/neverd --java-home "$JAVA_HOME" --d8 "$NEVERD_D8" \
  --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
  --work-dir "$RUNNER_TEMP/android-conditional-packed"
python scripts/test_mobile_android_conditional_packed.py --dependency-inputs \
  --neverd build-mobile/bin/neverd --java-home "$JAVA_HOME" --d8 "$NEVERD_D8" \
  --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
  --work-dir "$RUNNER_TEMP/android-conditional-packed-sdk"
python scripts/test_mobile_android_conditional_packed.py --ordered-inputs \
  --neverd build-mobile/bin/neverd --java-home "$JAVA_HOME" --d8 "$NEVERD_D8" \
  --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
  --work-dir "$RUNNER_TEMP/android-conditional-packed-ordered"
python scripts/test_mobile_android_conditional_packed.py --ordered-inputs --dependency-inputs \
  --neverd build-mobile/bin/neverd --java-home "$JAVA_HOME" --d8 "$NEVERD_D8" \
  --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
  --work-dir "$RUNNER_TEMP/android-conditional-packed-ordered-sdk"

profile_group multiple-final-exits 21 600
python scripts/test_mobile_android_multiple_final_exits.py  \
  --neverd build-mobile/bin/neverd --java-home "$JAVA_HOME" --d8 "$NEVERD_D8" \
  --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
  --work-dir "$RUNNER_TEMP/android-multiple-final-exits"
python scripts/test_mobile_android_multiple_final_exits.py --dependency-trace \
  --neverd build-mobile/bin/neverd --java-home "$JAVA_HOME" --d8 "$NEVERD_D8" \
  --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
  --work-dir "$RUNNER_TEMP/android-multiple-final-exits-sdk"
python scripts/test_mobile_android_multiple_final_exits.py --reverse-exits \
  --neverd build-mobile/bin/neverd --java-home "$JAVA_HOME" --d8 "$NEVERD_D8" \
  --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
  --work-dir "$RUNNER_TEMP/android-multiple-final-exits-reversed"
python scripts/test_mobile_android_multiple_final_exits.py --reverse-exits --dependency-trace \
  --neverd build-mobile/bin/neverd --java-home "$JAVA_HOME" --d8 "$NEVERD_D8" \
  --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
  --work-dir "$RUNNER_TEMP/android-multiple-final-exits-reversed-sdk"

profile_group raw-zero-reference 21 600
python scripts/test_mobile_android_raw_zero_reference.py  \
  --neverd build-mobile/bin/neverd --java-home "$JAVA_HOME" --d8 "$NEVERD_D8" \
  --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
  --work-dir "$RUNNER_TEMP/android-raw-zero-reference"
python scripts/test_mobile_android_raw_zero_reference.py --dependency-inputs \
  --neverd build-mobile/bin/neverd --java-home "$JAVA_HOME" --d8 "$NEVERD_D8" \
  --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
  --work-dir "$RUNNER_TEMP/android-raw-zero-reference-sdk"
python scripts/test_mobile_android_raw_zero_reference.py --void-result \
  --neverd build-mobile/bin/neverd --java-home "$JAVA_HOME" --d8 "$NEVERD_D8" \
  --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
  --work-dir "$RUNNER_TEMP/android-raw-zero-reference-void"
python scripts/test_mobile_android_raw_zero_reference.py --void-result --dependency-inputs \
  --neverd build-mobile/bin/neverd --java-home "$JAVA_HOME" --d8 "$NEVERD_D8" \
  --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
  --work-dir "$RUNNER_TEMP/android-raw-zero-reference-void-sdk"

profile_group constructor-private-getter 25 600
python scripts/test_mobile_android_constructor_private_getter.py  \
  --neverd build-mobile/bin/neverd --java-home "$JAVA_HOME" --d8 "$NEVERD_D8" \
  --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
  --work-dir "$RUNNER_TEMP/android-constructor-private-getter"
python scripts/test_mobile_android_constructor_private_getter.py --dependency-defaults \
  --neverd build-mobile/bin/neverd --java-home "$JAVA_HOME" --d8 "$NEVERD_D8" \
  --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
  --work-dir "$RUNNER_TEMP/android-constructor-private-getter-sdk"
python scripts/test_mobile_android_constructor_private_getter.py --scalar-result \
  --neverd build-mobile/bin/neverd --java-home "$JAVA_HOME" --d8 "$NEVERD_D8" \
  --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
  --work-dir "$RUNNER_TEMP/android-constructor-private-getter-scalar"
python scripts/test_mobile_android_constructor_private_getter.py --scalar-result --dependency-defaults \
  --neverd build-mobile/bin/neverd --java-home "$JAVA_HOME" --d8 "$NEVERD_D8" \
  --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
  --work-dir "$RUNNER_TEMP/android-constructor-private-getter-scalar-sdk"

profile_group allocation-retained-int 21 600
python scripts/test_mobile_android_allocation_retained_int.py \
  --neverd build-mobile/bin/neverd --java-home "$JAVA_HOME" --d8 "$NEVERD_D8" \
  --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
  --work-dir "$RUNNER_TEMP/android-allocation-retained-int"
python scripts/test_mobile_android_allocation_retained_int.py --dependency-api \
  --neverd build-mobile/bin/neverd --java-home "$JAVA_HOME" --d8 "$NEVERD_D8" \
  --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
  --work-dir "$RUNNER_TEMP/android-allocation-retained-int-sdk"
python scripts/test_mobile_android_allocation_retained_int.py --derived \
  --neverd build-mobile/bin/neverd --java-home "$JAVA_HOME" --d8 "$NEVERD_D8" \
  --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
  --work-dir "$RUNNER_TEMP/android-allocation-retained-int-derived"
python scripts/test_mobile_android_allocation_retained_int.py --derived --dependency-api \
  --neverd build-mobile/bin/neverd --java-home "$JAVA_HOME" --d8 "$NEVERD_D8" \
  --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
  --work-dir "$RUNNER_TEMP/android-allocation-retained-int-derived-sdk"

profile_group private-seeded-result 21 600
python scripts/test_mobile_android_private_seeded_result.py \
  --neverd build-mobile/bin/neverd --java-home "$JAVA_HOME" --d8 "$NEVERD_D8" \
  --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
  --work-dir "$RUNNER_TEMP/android-private-seeded-result"
python scripts/test_mobile_android_private_seeded_result.py --dependency-api \
  --neverd build-mobile/bin/neverd --java-home "$JAVA_HOME" --d8 "$NEVERD_D8" \
  --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
  --work-dir "$RUNNER_TEMP/android-private-seeded-result-sdk"
python scripts/test_mobile_android_private_seeded_result.py --static-helper \
  --neverd build-mobile/bin/neverd --java-home "$JAVA_HOME" --d8 "$NEVERD_D8" \
  --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
  --work-dir "$RUNNER_TEMP/android-private-seeded-result-static"
python scripts/test_mobile_android_private_seeded_result.py --static-helper --dependency-api \
  --neverd build-mobile/bin/neverd --java-home "$JAVA_HOME" --d8 "$NEVERD_D8" \
  --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
  --work-dir "$RUNNER_TEMP/android-private-seeded-result-static-sdk"

profile_group plain-allocation-retained-int 21 600
python scripts/test_mobile_android_allocation_retained_int.py --plain-api \
  --neverd build-mobile/bin/neverd --java-home "$JAVA_HOME" --d8 "$NEVERD_D8" \
  --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
  --work-dir "$RUNNER_TEMP/android-plain-allocation-retained-int"
python scripts/test_mobile_android_allocation_retained_int.py --plain-api --dependency-api \
  --neverd build-mobile/bin/neverd --java-home "$JAVA_HOME" --d8 "$NEVERD_D8" \
  --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
  --work-dir "$RUNNER_TEMP/android-plain-allocation-retained-int-sdk"
python scripts/test_mobile_android_allocation_retained_int.py --plain-api --derived \
  --neverd build-mobile/bin/neverd --java-home "$JAVA_HOME" --d8 "$NEVERD_D8" \
  --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
  --work-dir "$RUNNER_TEMP/android-plain-allocation-retained-int-derived"
python scripts/test_mobile_android_allocation_retained_int.py --plain-api --derived --dependency-api \
  --neverd build-mobile/bin/neverd --java-home "$JAVA_HOME" --d8 "$NEVERD_D8" \
  --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
  --work-dir "$RUNNER_TEMP/android-plain-allocation-retained-int-derived-sdk"

profile_group constructor-literal-allocation 25 600
python scripts/test_mobile_android_constructor_literal_allocation.py \
  --neverd build-mobile/bin/neverd --java-home "$JAVA_HOME" --d8 "$NEVERD_D8" \
  --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
  --work-dir "$RUNNER_TEMP/android-constructor-literal-allocation"
python scripts/test_mobile_android_constructor_literal_allocation.py --dependency-inputs \
  --neverd build-mobile/bin/neverd --java-home "$JAVA_HOME" --d8 "$NEVERD_D8" \
  --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
  --work-dir "$RUNNER_TEMP/android-constructor-literal-allocation-sdk"
python scripts/test_mobile_android_constructor_literal_allocation.py --negative-mask \
  --neverd build-mobile/bin/neverd --java-home "$JAVA_HOME" --d8 "$NEVERD_D8" \
  --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
  --work-dir "$RUNNER_TEMP/android-constructor-literal-allocation-negative"
python scripts/test_mobile_android_constructor_literal_allocation.py --negative-mask --dependency-inputs \
  --neverd build-mobile/bin/neverd --java-home "$JAVA_HOME" --d8 "$NEVERD_D8" \
  --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
  --work-dir "$RUNNER_TEMP/android-constructor-literal-allocation-negative-sdk"

profile_group allocation-inherited-calls 21 600
python scripts/test_mobile_android_allocation_instance_calls.py --inherited-provider \
  --neverd build-mobile/bin/neverd --java-home "$JAVA_HOME" --d8 "$NEVERD_D8" \
  --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
  --work-dir "$RUNNER_TEMP/android-allocation-inherited-calls"
python scripts/test_mobile_android_allocation_instance_calls.py --inherited-provider --dependency-provider \
  --neverd build-mobile/bin/neverd --java-home "$JAVA_HOME" --d8 "$NEVERD_D8" \
  --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
  --work-dir "$RUNNER_TEMP/android-allocation-inherited-calls-sdk"
python scripts/test_mobile_android_allocation_instance_calls.py --inherited-provider --call-access protected \
  --neverd build-mobile/bin/neverd --java-home "$JAVA_HOME" --d8 "$NEVERD_D8" \
  --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
  --work-dir "$RUNNER_TEMP/android-allocation-inherited-calls-protected"
python scripts/test_mobile_android_allocation_instance_calls.py --inherited-provider --call-access protected --dependency-provider \
  --neverd build-mobile/bin/neverd --java-home "$JAVA_HOME" --d8 "$NEVERD_D8" \
  --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
  --work-dir "$RUNNER_TEMP/android-allocation-inherited-calls-protected-sdk"

profile_group allocation-integer-prefix 21 600
python scripts/test_mobile_android_allocation_array_lengths.py --arithmetic-prefix \
  --neverd build-mobile/bin/neverd --java-home "$JAVA_HOME" --d8 "$NEVERD_D8" \
  --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
  --work-dir "$RUNNER_TEMP/android-allocation-integer-prefix"
python scripts/test_mobile_android_allocation_array_lengths.py --arithmetic-prefix --dependency-provider \
  --neverd build-mobile/bin/neverd --java-home "$JAVA_HOME" --d8 "$NEVERD_D8" \
  --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
  --work-dir "$RUNNER_TEMP/android-allocation-integer-prefix-sdk"
python scripts/test_mobile_android_allocation_array_lengths.py --arithmetic-prefix --negative-bias \
  --neverd build-mobile/bin/neverd --java-home "$JAVA_HOME" --d8 "$NEVERD_D8" \
  --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
  --work-dir "$RUNNER_TEMP/android-allocation-integer-prefix-negative"
python scripts/test_mobile_android_allocation_array_lengths.py --arithmetic-prefix --negative-bias --dependency-provider \
  --neverd build-mobile/bin/neverd --java-home "$JAVA_HOME" --d8 "$NEVERD_D8" \
  --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
  --work-dir "$RUNNER_TEMP/android-allocation-integer-prefix-negative-sdk"

profile_group constructor-choice 21 600
python scripts/test_mobile_android_constructor_choice.py \
  --neverd build-mobile/bin/neverd --java-home "$JAVA_HOME" --d8 "$NEVERD_D8" \
  --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
  --work-dir "$RUNNER_TEMP/android-constructor-choice"
python scripts/test_mobile_android_constructor_choice.py --dependency-widget \
  --neverd build-mobile/bin/neverd --java-home "$JAVA_HOME" --d8 "$NEVERD_D8" \
  --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
  --work-dir "$RUNNER_TEMP/android-constructor-choice-sdk"
python scripts/test_mobile_android_constructor_choice.py --copy-on-null \
  --neverd build-mobile/bin/neverd --java-home "$JAVA_HOME" --d8 "$NEVERD_D8" \
  --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
  --work-dir "$RUNNER_TEMP/android-constructor-choice-null"
python scripts/test_mobile_android_constructor_choice.py --copy-on-null --dependency-widget \
  --neverd build-mobile/bin/neverd --java-home "$JAVA_HOME" --d8 "$NEVERD_D8" \
  --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
  --work-dir "$RUNNER_TEMP/android-constructor-choice-null-sdk"

profile_group monitor-copy-cleanup 21 600
python scripts/test_mobile_android_monitor_copy_cleanup.py \
  --neverd build-mobile/bin/neverd --java-home "$JAVA_HOME" --d8 "$NEVERD_D8" \
  --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
  --work-dir "$RUNNER_TEMP/android-monitor-copy-cleanup"
python scripts/test_mobile_android_monitor_copy_cleanup.py --dependency-provider \
  --neverd build-mobile/bin/neverd --java-home "$JAVA_HOME" --d8 "$NEVERD_D8" \
  --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
  --work-dir "$RUNNER_TEMP/android-monitor-copy-cleanup-sdk"
python scripts/test_mobile_android_monitor_copy_cleanup.py --wide-copies \
  --neverd build-mobile/bin/neverd --java-home "$JAVA_HOME" --d8 "$NEVERD_D8" \
  --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
  --work-dir "$RUNNER_TEMP/android-monitor-copy-cleanup-wide"
python scripts/test_mobile_android_monitor_copy_cleanup.py --wide-copies --dependency-provider \
  --neverd build-mobile/bin/neverd --java-home "$JAVA_HOME" --d8 "$NEVERD_D8" \
  --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
  --work-dir "$RUNNER_TEMP/android-monitor-copy-cleanup-wide-sdk"

profile_group private-binary-carrier 21 600
python scripts/test_mobile_android_private_binary_carrier.py \
  --neverd build-mobile/bin/neverd --java-home "$JAVA_HOME" --d8 "$NEVERD_D8" \
  --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
  --work-dir "$RUNNER_TEMP/android-private-binary-carrier"
python scripts/test_mobile_android_private_binary_carrier.py --dependency-api \
  --neverd build-mobile/bin/neverd --java-home "$JAVA_HOME" --d8 "$NEVERD_D8" \
  --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
  --work-dir "$RUNNER_TEMP/android-private-binary-carrier-sdk"
python scripts/test_mobile_android_private_binary_carrier.py --static-helper \
  --neverd build-mobile/bin/neverd --java-home "$JAVA_HOME" --d8 "$NEVERD_D8" \
  --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
  --work-dir "$RUNNER_TEMP/android-private-binary-carrier-static"
python scripts/test_mobile_android_private_binary_carrier.py --static-helper --dependency-api \
  --neverd build-mobile/bin/neverd --java-home "$JAVA_HOME" --d8 "$NEVERD_D8" \
  --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
  --work-dir "$RUNNER_TEMP/android-private-binary-carrier-static-sdk"

profile_group protected-self-final 21 600
python scripts/test_mobile_android_protected_self_final.py \
  --neverd build-mobile/bin/neverd --java-home "$JAVA_HOME" --d8 "$NEVERD_D8" \
  --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
  --work-dir "$RUNNER_TEMP/android-protected-self-final"
python scripts/test_mobile_android_protected_self_final.py --dependency-provider \
  --neverd build-mobile/bin/neverd --java-home "$JAVA_HOME" --d8 "$NEVERD_D8" \
  --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
  --work-dir "$RUNNER_TEMP/android-protected-self-final-sdk"
python scripts/test_mobile_android_protected_self_final.py --narrow-second \
  --neverd build-mobile/bin/neverd --java-home "$JAVA_HOME" --d8 "$NEVERD_D8" \
  --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
  --work-dir "$RUNNER_TEMP/android-protected-self-final-narrow"
python scripts/test_mobile_android_protected_self_final.py --narrow-second --dependency-provider \
  --neverd build-mobile/bin/neverd --java-home "$JAVA_HOME" --d8 "$NEVERD_D8" \
  --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
  --work-dir "$RUNNER_TEMP/android-protected-self-final-narrow-sdk"

profile_group raw-scalar-tail-bridges 21 600
python scripts/test_mobile_android_raw_scalar_tail_bridges.py \
  --neverd build-mobile/bin/neverd --java-home "$JAVA_HOME" --d8 "$NEVERD_D8" \
  --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
  --work-dir "$RUNNER_TEMP/android-raw-scalar-tail-bridges"
python scripts/test_mobile_android_raw_scalar_tail_bridges.py --dependency-provider \
  --neverd build-mobile/bin/neverd --java-home "$JAVA_HOME" --d8 "$NEVERD_D8" \
  --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
  --work-dir "$RUNNER_TEMP/android-raw-scalar-tail-bridges-sdk"
python scripts/test_mobile_android_raw_scalar_tail_bridges.py --narrow-scalar \
  --neverd build-mobile/bin/neverd --java-home "$JAVA_HOME" --d8 "$NEVERD_D8" \
  --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
  --work-dir "$RUNNER_TEMP/android-raw-scalar-tail-bridges-narrow"
python scripts/test_mobile_android_raw_scalar_tail_bridges.py --narrow-scalar --dependency-provider \
  --neverd build-mobile/bin/neverd --java-home "$JAVA_HOME" --d8 "$NEVERD_D8" \
  --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
  --work-dir "$RUNNER_TEMP/android-raw-scalar-tail-bridges-narrow-sdk"

profile_group binary-callback-carrier 21 600
python scripts/test_mobile_android_binary_callback_carrier.py \
  --neverd build-mobile/bin/neverd --java-home "$JAVA_HOME" --d8 "$NEVERD_D8" \
  --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
  --work-dir "$RUNNER_TEMP/android-binary-callback-carrier"
python scripts/test_mobile_android_binary_callback_carrier.py --dependency-provider \
  --neverd build-mobile/bin/neverd --java-home "$JAVA_HOME" --d8 "$NEVERD_D8" \
  --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
  --work-dir "$RUNNER_TEMP/android-binary-callback-carrier-sdk"
python scripts/test_mobile_android_binary_callback_carrier.py --invariant-input \
  --neverd build-mobile/bin/neverd --java-home "$JAVA_HOME" --d8 "$NEVERD_D8" \
  --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
  --work-dir "$RUNNER_TEMP/android-binary-callback-carrier-invariant"
python scripts/test_mobile_android_binary_callback_carrier.py --invariant-input --dependency-provider \
  --neverd build-mobile/bin/neverd --java-home "$JAVA_HOME" --d8 "$NEVERD_D8" \
  --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
  --work-dir "$RUNNER_TEMP/android-binary-callback-carrier-invariant-sdk"

profile_group carrier-key-selector 21 600
python scripts/test_mobile_android_binary_callback_carrier.py --key-selector \
  --neverd build-mobile/bin/neverd --java-home "$JAVA_HOME" --d8 "$NEVERD_D8" \
  --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
  --work-dir "$RUNNER_TEMP/android-carrier-key-selector"
python scripts/test_mobile_android_binary_callback_carrier.py --key-selector --dependency-provider \
  --neverd build-mobile/bin/neverd --java-home "$JAVA_HOME" --d8 "$NEVERD_D8" \
  --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
  --work-dir "$RUNNER_TEMP/android-carrier-key-selector-sdk"
python scripts/test_mobile_android_binary_callback_carrier.py --key-selector --invariant-input \
  --neverd build-mobile/bin/neverd --java-home "$JAVA_HOME" --d8 "$NEVERD_D8" \
  --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
  --work-dir "$RUNNER_TEMP/android-carrier-key-selector-invariant"
python scripts/test_mobile_android_binary_callback_carrier.py --key-selector --invariant-input --dependency-provider \
  --neverd build-mobile/bin/neverd --java-home "$JAVA_HOME" --d8 "$NEVERD_D8" \
  --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
  --work-dir "$RUNNER_TEMP/android-carrier-key-selector-invariant-sdk"

profile_group nested-conditional-tail 21 600
python scripts/test_mobile_android_nested_conditional_tail.py \
  --neverd build-mobile/bin/neverd --java-home "$JAVA_HOME" --d8 "$NEVERD_D8" \
  --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
  --work-dir "$RUNNER_TEMP/android-nested-conditional-tail"
python scripts/test_mobile_android_nested_conditional_tail.py --dependency-provider \
  --neverd build-mobile/bin/neverd --java-home "$JAVA_HOME" --d8 "$NEVERD_D8" \
  --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
  --work-dir "$RUNNER_TEMP/android-nested-conditional-tail-sdk"
python scripts/test_mobile_android_nested_conditional_tail.py --present-branch \
  --neverd build-mobile/bin/neverd --java-home "$JAVA_HOME" --d8 "$NEVERD_D8" \
  --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
  --work-dir "$RUNNER_TEMP/android-nested-conditional-tail-present"
python scripts/test_mobile_android_nested_conditional_tail.py --present-branch --dependency-provider \
  --neverd build-mobile/bin/neverd --java-home "$JAVA_HOME" --d8 "$NEVERD_D8" \
  --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
  --work-dir "$RUNNER_TEMP/android-nested-conditional-tail-present-sdk"

profile_group "Execute covariant collection and complete predicate bindings" 21 300
python scripts/test_mobile_android_collection_predicate.py --covariant-values \
  --neverd build-mobile/bin/neverd --java-home "$JAVA_HOME" --d8 "$NEVERD_D8" \
  --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
  --work-dir "$RUNNER_TEMP/android-covariant-collection-predicate"
python scripts/test_mobile_android_collection_predicate.py --covariant-values --dependency-provider \
  --neverd build-mobile/bin/neverd --java-home "$JAVA_HOME" --d8 "$NEVERD_D8" \
  --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
  --work-dir "$RUNNER_TEMP/android-covariant-collection-predicate-sdk"

profile_group "Execute exact scalar comparisons inside conditional constructor arguments" 21 300
python scripts/test_mobile_android_nested_conditional_tail.py --comparison-tail long \
  --neverd build-mobile/bin/neverd --java-home "$JAVA_HOME" --d8 "$NEVERD_D8" \
  --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
  --work-dir "$RUNNER_TEMP/android-conditional-comparison-long"
python scripts/test_mobile_android_nested_conditional_tail.py --comparison-tail long --dependency-provider \
  --neverd build-mobile/bin/neverd --java-home "$JAVA_HOME" --d8 "$NEVERD_D8" \
  --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
  --work-dir "$RUNNER_TEMP/android-conditional-comparison-long-sdk"
python scripts/test_mobile_android_nested_conditional_tail.py --comparison-tail float-less \
  --neverd build-mobile/bin/neverd --java-home "$JAVA_HOME" --d8 "$NEVERD_D8" \
  --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
  --work-dir "$RUNNER_TEMP/android-conditional-comparison-float-less"
python scripts/test_mobile_android_nested_conditional_tail.py --comparison-tail float-less --dependency-provider \
  --neverd build-mobile/bin/neverd --java-home "$JAVA_HOME" --d8 "$NEVERD_D8" \
  --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
  --work-dir "$RUNNER_TEMP/android-conditional-comparison-float-less-sdk"
python scripts/test_mobile_android_nested_conditional_tail.py --comparison-tail float-greater \
  --neverd build-mobile/bin/neverd --java-home "$JAVA_HOME" --d8 "$NEVERD_D8" \
  --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
  --work-dir "$RUNNER_TEMP/android-conditional-comparison-float-greater"
python scripts/test_mobile_android_nested_conditional_tail.py --comparison-tail float-greater --dependency-provider \
  --neverd build-mobile/bin/neverd --java-home "$JAVA_HOME" --d8 "$NEVERD_D8" \
  --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
  --work-dir "$RUNNER_TEMP/android-conditional-comparison-float-greater-sdk"
python scripts/test_mobile_android_nested_conditional_tail.py --comparison-tail double-less \
  --neverd build-mobile/bin/neverd --java-home "$JAVA_HOME" --d8 "$NEVERD_D8" \
  --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
  --work-dir "$RUNNER_TEMP/android-conditional-comparison-double-less"
python scripts/test_mobile_android_nested_conditional_tail.py --comparison-tail double-less --dependency-provider \
  --neverd build-mobile/bin/neverd --java-home "$JAVA_HOME" --d8 "$NEVERD_D8" \
  --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
  --work-dir "$RUNNER_TEMP/android-conditional-comparison-double-less-sdk"
python scripts/test_mobile_android_nested_conditional_tail.py --comparison-tail double-greater \
  --neverd build-mobile/bin/neverd --java-home "$JAVA_HOME" --d8 "$NEVERD_D8" \
  --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
  --work-dir "$RUNNER_TEMP/android-conditional-comparison-double-greater"
python scripts/test_mobile_android_nested_conditional_tail.py --comparison-tail double-greater --dependency-provider \
  --neverd build-mobile/bin/neverd --java-home "$JAVA_HOME" --d8 "$NEVERD_D8" \
  --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
  --work-dir "$RUNNER_TEMP/android-conditional-comparison-double-greater-sdk"

profile_group "Execute owned private nested callback and completion bindings" 21 300
python scripts/test_mobile_android_scoped_completion_callback.py --private-provider static \
  --neverd build-mobile/bin/neverd --java-home "$JAVA_HOME" --d8 "$NEVERD_D8" \
  --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
  --work-dir "$RUNNER_TEMP/android-private-completion-static"
python scripts/test_mobile_android_scoped_completion_callback.py --private-provider static --dependency-inputs \
  --neverd build-mobile/bin/neverd --java-home "$JAVA_HOME" --d8 "$NEVERD_D8" \
  --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
  --work-dir "$RUNNER_TEMP/android-private-completion-static-sdk"
python scripts/test_mobile_android_scoped_completion_callback.py --private-provider instance \
  --neverd build-mobile/bin/neverd --java-home "$JAVA_HOME" --d8 "$NEVERD_D8" \
  --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
  --work-dir "$RUNNER_TEMP/android-private-completion-instance"
python scripts/test_mobile_android_scoped_completion_callback.py --private-provider instance --dependency-inputs \
  --neverd build-mobile/bin/neverd --java-home "$JAVA_HOME" --d8 "$NEVERD_D8" \
  --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
  --work-dir "$RUNNER_TEMP/android-private-completion-instance-sdk"

profile_group "Execute reachable allocation ownership across interleaved branches" 21 300
python scripts/test_mobile_android_interleaved_allocation.py \
  --neverd build-mobile/bin/neverd --java-home "$JAVA_HOME" --d8 "$NEVERD_D8" \
  --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
  --work-dir "$RUNNER_TEMP/android-interleaved-allocation"
python scripts/test_mobile_android_interleaved_allocation.py --dependency-target \
  --neverd build-mobile/bin/neverd --java-home "$JAVA_HOME" --d8 "$NEVERD_D8" \
  --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
  --work-dir "$RUNNER_TEMP/android-interleaved-allocation-sdk"
python scripts/test_mobile_android_interleaved_allocation.py --nested-parent \
  --neverd build-mobile/bin/neverd --java-home "$JAVA_HOME" --d8 "$NEVERD_D8" \
  --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
  --work-dir "$RUNNER_TEMP/android-interleaved-allocation-nested"
python scripts/test_mobile_android_interleaved_allocation.py --nested-parent --dependency-target \
  --neverd build-mobile/bin/neverd --java-home "$JAVA_HOME" --d8 "$NEVERD_D8" \
  --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
  --work-dir "$RUNNER_TEMP/android-interleaved-allocation-nested-sdk"

profile_group "Execute conditional constructor arguments with a final receiver copy" 21 300
python scripts/test_mobile_android_pure_allocation_receiver_copy.py --conditional-preparation \
  --neverd build-mobile/bin/neverd --java-home "$JAVA_HOME" --d8 "$NEVERD_D8" \
  --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
  --work-dir "$RUNNER_TEMP/android-conditional-receiver-copy"
python scripts/test_mobile_android_pure_allocation_receiver_copy.py --conditional-preparation --inverse-condition \
  --neverd build-mobile/bin/neverd --java-home "$JAVA_HOME" --d8 "$NEVERD_D8" \
  --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
  --work-dir "$RUNNER_TEMP/android-conditional-receiver-copy-inverse"

profile_group "Execute guarded primary super calls and Kotlin default chains" 25 1200
python scripts/test_mobile_android_guarded_delegation_chains.py --guarded-primary \
  --neverd build-mobile/bin/neverd --d8 "$NEVERD_D8" --java-home "$JAVA_HOME" \
  --kotlin-java-home "$NEVERD_KOTLIN_JAVA_HOME" \
  --kotlin-lib "$RUNNER_TEMP/neverd-kotlin-2.0.21/kotlinc/lib" \
  --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
  --work-dir "$RUNNER_TEMP/android-guarded-primary-super"
python scripts/test_mobile_android_guarded_delegation_chains.py --guarded-primary --dependency-parent \
  --neverd build-mobile/bin/neverd --d8 "$NEVERD_D8" --java-home "$JAVA_HOME" \
  --kotlin-java-home "$NEVERD_KOTLIN_JAVA_HOME" \
  --kotlin-lib "$RUNNER_TEMP/neverd-kotlin-2.0.21/kotlinc/lib" \
  --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
  --work-dir "$RUNNER_TEMP/android-guarded-primary-super-sdk"

profile_group "Execute owner-scoped void interface default callbacks" 21 600
python scripts/test_mobile_android_owner_void_default_access.py \
  --neverd build-mobile/bin/neverd --java-home "$JAVA_HOME" --d8 "$NEVERD_D8" \
  --kotlin-java-home "$NEVERD_KOTLIN_JAVA_HOME" \
  --kotlin-lib "$RUNNER_TEMP/neverd-kotlin-2.0.21/kotlinc/lib" \
  --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
  --work-dir "$RUNNER_TEMP/android-owner-void-default"
python scripts/test_mobile_android_owner_void_default_access.py --dependency-trace \
  --neverd build-mobile/bin/neverd --java-home "$JAVA_HOME" --d8 "$NEVERD_D8" \
  --kotlin-java-home "$NEVERD_KOTLIN_JAVA_HOME" \
  --kotlin-lib "$RUNNER_TEMP/neverd-kotlin-2.0.21/kotlinc/lib" \
  --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
  --work-dir "$RUNNER_TEMP/android-owner-void-default-sdk"

profile_group "Execute private producer results with complete closed inputs" 21 600
python scripts/test_mobile_android_private_seeded_result.py --producer-only \
  --neverd build-mobile/bin/neverd --java-home "$JAVA_HOME" --d8 "$NEVERD_D8" \
  --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
  --work-dir "$RUNNER_TEMP/android-private-producer-result"
python scripts/test_mobile_android_private_seeded_result.py --producer-only --dependency-api \
  --neverd build-mobile/bin/neverd --java-home "$JAVA_HOME" --d8 "$NEVERD_D8" \
  --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
  --work-dir "$RUNNER_TEMP/android-private-producer-result-sdk"
python scripts/test_mobile_android_private_seeded_result.py --producer-only --static-helper \
  --neverd build-mobile/bin/neverd --java-home "$JAVA_HOME" --d8 "$NEVERD_D8" \
  --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
  --work-dir "$RUNNER_TEMP/android-private-producer-static-result"
python scripts/test_mobile_android_private_seeded_result.py --producer-only --static-helper --dependency-api \
  --neverd build-mobile/bin/neverd --java-home "$JAVA_HOME" --d8 "$NEVERD_D8" \
  --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
  --work-dir "$RUNNER_TEMP/android-private-producer-static-result-sdk"

profile_group "Execute two scoped callback inputs and a closed result slot" 21 600
python scripts/test_mobile_android_paired_void_carriers.py --callback-tail \
  --neverd build-mobile/bin/neverd --java-home "$JAVA_HOME" --d8 "$NEVERD_D8" \
  --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
  --work-dir "$RUNNER_TEMP/android-paired-callback-tail"
python scripts/test_mobile_android_paired_void_carriers.py --callback-tail --dependency-provider \
  --neverd build-mobile/bin/neverd --java-home "$JAVA_HOME" --d8 "$NEVERD_D8" \
  --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
  --work-dir "$RUNNER_TEMP/android-paired-callback-tail-sdk"

profile_group "Execute closed private factories with unused method formals" 21 600
python scripts/test_mobile_android_private_nullary_carriers.py --closed-scalar-result \
  --neverd build-mobile/bin/neverd --java-home "$JAVA_HOME" --d8 "$NEVERD_D8" \
  --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
  --work-dir "$RUNNER_TEMP/android-closed-unused-factory"
python scripts/test_mobile_android_private_nullary_carriers.py --closed-scalar-result --dependency-view \
  --neverd build-mobile/bin/neverd --java-home "$JAVA_HOME" --d8 "$NEVERD_D8" \
  --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
  --work-dir "$RUNNER_TEMP/android-closed-unused-factory-sdk"

profile_group "Execute monitor bodies around interleaved cleanup blocks" 21 600
for monitor_copy_width in scalar wide; do
  for monitor_provider_layout in source sdk; do
    monitor_layout_args=()
    if [[ "$monitor_copy_width" == wide ]]; then monitor_layout_args+=(--wide-copies); fi
    if [[ "$monitor_provider_layout" == sdk ]]; then monitor_layout_args+=(--dependency-provider); fi
    python scripts/test_mobile_android_monitor_copy_cleanup.py --interleaved-handlers "${monitor_layout_args[@]}" \
      --neverd build-mobile/bin/neverd --java-home "$JAVA_HOME" --d8 "$NEVERD_D8" \
      --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
      --work-dir "$RUNNER_TEMP/android-interleaved-monitor-$monitor_copy_width-$monitor_provider_layout"
  done
done

profile_group "Execute nested exceptional programs while the monitor is held" 21 600
python scripts/test_mobile_android_monitor_copy_cleanup.py --nested-cleanup \
  --neverd build-mobile/bin/neverd --java-home "$JAVA_HOME" --d8 "$NEVERD_D8" \
  --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
  --work-dir "$RUNNER_TEMP/android-nested-monitor-cleanup"
python scripts/test_mobile_android_monitor_copy_cleanup.py --nested-cleanup --dependency-provider \
  --neverd build-mobile/bin/neverd --java-home "$JAVA_HOME" --d8 "$NEVERD_D8" \
  --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
  --work-dir "$RUNNER_TEMP/android-nested-monitor-cleanup-sdk"

profile_group "Execute outer handlers around monitor acquisition and release" 21 600
python scripts/test_mobile_android_monitor_copy_cleanup.py --outer-cleanup \
  --neverd build-mobile/bin/neverd --java-home "$JAVA_HOME" --d8 "$NEVERD_D8" \
  --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
  --work-dir "$RUNNER_TEMP/android-outer-monitor-cleanup"
python scripts/test_mobile_android_monitor_copy_cleanup.py --outer-cleanup --dependency-provider \
  --neverd build-mobile/bin/neverd --java-home "$JAVA_HOME" --d8 "$NEVERD_D8" \
  --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
  --work-dir "$RUNNER_TEMP/android-outer-monitor-cleanup-sdk"

profile_group "Execute total arithmetic between protected monitor effects" 21 600
python scripts/test_mobile_android_monitor_copy_cleanup.py --arithmetic-gaps \
  --neverd build-mobile/bin/neverd --java-home "$JAVA_HOME" --d8 "$NEVERD_D8" \
  --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
  --work-dir "$RUNNER_TEMP/android-monitor-arithmetic-gaps"
python scripts/test_mobile_android_monitor_copy_cleanup.py --arithmetic-gaps --dependency-provider \
  --neverd build-mobile/bin/neverd --java-home "$JAVA_HOME" --d8 "$NEVERD_D8" \
  --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
  --work-dir "$RUNNER_TEMP/android-monitor-arithmetic-gaps-sdk"

profile_group "Keep ordinary callback loops outside synchronized ownership" 21 600
python scripts/test_mobile_android_monitor_copy_cleanup.py --unlocked-loops \
  --neverd build-mobile/bin/neverd --java-home "$JAVA_HOME" --d8 "$NEVERD_D8" \
  --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
  --work-dir "$RUNNER_TEMP/android-monitor-unlocked-loops"
python scripts/test_mobile_android_monitor_copy_cleanup.py --unlocked-loops --dependency-provider \
  --neverd build-mobile/bin/neverd --java-home "$JAVA_HOME" --d8 "$NEVERD_D8" \
  --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
  --work-dir "$RUNNER_TEMP/android-monitor-unlocked-loops-sdk"

profile_group "Preserve scoped element inputs to array-valued generic calls" 21 600
python scripts/test_mobile_android_generic_array_transfers.py --element-seed \
  --neverd build-mobile/bin/neverd --java-home "$JAVA_HOME" --d8 "$NEVERD_D8" \
  --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
  --work-dir "$RUNNER_TEMP/android-array-element-transfer"
python scripts/test_mobile_android_generic_array_transfers.py --element-seed --dependency-provider \
  --neverd build-mobile/bin/neverd --java-home "$JAVA_HOME" --d8 "$NEVERD_D8" \
  --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
  --work-dir "$RUNNER_TEMP/android-array-element-transfer-sdk"

profile_group "Keep fresh raw callbacks before scoped superclass initialization" 25 600
python scripts/test_mobile_android_erased_super_constructor.py --allocated-callback \
  --neverd build-mobile/bin/neverd --java-home "$JAVA_HOME" --d8 "$NEVERD_D8" \
  --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
  --work-dir "$RUNNER_TEMP/android-allocated-scoped-callback"
python scripts/test_mobile_android_erased_super_constructor.py --allocated-callback --dependency-provider \
  --neverd build-mobile/bin/neverd --java-home "$JAVA_HOME" --d8 "$NEVERD_D8" \
  --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
  --work-dir "$RUNNER_TEMP/android-allocated-scoped-callback-sdk"

profile_group "Keep nested completion results and original closed checked failures" 21 600
for nested_result_mode in method initializer; do
  for nested_result_provider in source sdk; do
    nested_result_args=()
    if [[ "$nested_result_mode" == initializer ]]; then nested_result_args+=(--initializer); fi
    if [[ "$nested_result_provider" == sdk ]]; then nested_result_args+=(--dependency-provider); fi
    python scripts/test_mobile_android_nested_completion_result.py "${nested_result_args[@]}" \
      --neverd build-mobile/bin/neverd --java-home "$JAVA_HOME" --d8 "$NEVERD_D8" \
      --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
      --work-dir "$RUNNER_TEMP/android-nested-completion-result-$nested_result_mode-$nested_result_provider"
  done
done

profile_group "Keep public covariant results and complete closed input contracts" 21 600
for public_producer_dispatch in instance static; do
  for public_producer_api in source sdk; do
    public_producer_args=(--producer-only --public-helper)
    if [[ "$public_producer_dispatch" == static ]]; then public_producer_args+=(--static-helper); fi
    if [[ "$public_producer_api" == sdk ]]; then public_producer_args+=(--dependency-api); fi
    python scripts/test_mobile_android_private_seeded_result.py "${public_producer_args[@]}" \
      --neverd build-mobile/bin/neverd --java-home "$JAVA_HOME" --d8 "$NEVERD_D8" \
      --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
      --work-dir "$RUNNER_TEMP/android-public-producer-result-$public_producer_dispatch-$public_producer_api"
  done
done

profile_group "Bind public lower callback inputs through the original covariant producer" 21 600
for public_scoped_api in source sdk; do
  public_scoped_args=(--producer-only --public-helper --scoped-consumer)
  if [[ "$public_scoped_api" == sdk ]]; then public_scoped_args+=(--dependency-api); fi
  python scripts/test_mobile_android_private_seeded_result.py "${public_scoped_args[@]}" \
    --neverd build-mobile/bin/neverd --java-home "$JAVA_HOME" --d8 "$NEVERD_D8" \
    --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
    --work-dir "$RUNNER_TEMP/android-public-scoped-producer-$public_scoped_api"
done

profile_group "Preserve boxed default bodies and all scalar compatibility unbox tails" 21 600
for scalar_default_type in D F J I S B C Z; do
  python scripts/test_mobile_android_scalar_default_access.py --scalar "$scalar_default_type" \
    --neverd build-mobile/bin/neverd --java-home "$JAVA_HOME" --d8 "$NEVERD_D8" \
    --kotlin-lib "$RUNNER_TEMP/neverd-kotlin-2.0.21/kotlinc/lib" \
    --kotlin-java-home "$NEVERD_KOTLIN_JAVA_HOME" \
    --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
    --work-dir "$RUNNER_TEMP/android-scalar-default-access-$scalar_default_type"
done

profile_group "Keep closed callback producers through opaque instance services" 21 600
python scripts/test_mobile_android_fixed_input_callback.py --instance-provider \
  --neverd build-mobile/bin/neverd --java-home "$JAVA_HOME" --d8 "$NEVERD_D8" \
  --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
  --work-dir "$RUNNER_TEMP/android-instance-fixed-producer"
python scripts/test_mobile_android_fixed_input_callback.py --instance-provider --dependency-inputs \
  --neverd build-mobile/bin/neverd --java-home "$JAVA_HOME" --d8 "$NEVERD_D8" \
  --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
  --work-dir "$RUNNER_TEMP/android-instance-fixed-producer-sdk"

profile_group "Keep raw reference casts and Number conversion before void Unit results" 21 600
python scripts/test_mobile_android_raw_reference_unit.py --integer-tail \
  --neverd build-mobile/bin/neverd --java-home "$JAVA_HOME" --d8 "$NEVERD_D8" \
  --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
  --work-dir "$RUNNER_TEMP/android-mixed-raw-unit"
python scripts/test_mobile_android_raw_reference_unit.py --integer-tail --dependency-provider \
  --neverd build-mobile/bin/neverd --java-home "$JAVA_HOME" --d8 "$NEVERD_D8" \
  --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
  --work-dir "$RUNNER_TEMP/android-mixed-raw-unit-sdk"

profile_group "Keep element carriers with recursively bounded Comparable selectors" 21 600
python scripts/test_mobile_android_comparable_selector_carrier.py \
  --neverd build-mobile/bin/neverd --java-home "$JAVA_HOME" --d8 "$NEVERD_D8" \
  --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
  --work-dir "$RUNNER_TEMP/android-comparable-selector-carrier"
python scripts/test_mobile_android_comparable_selector_carrier.py --dependency-provider \
  --neverd build-mobile/bin/neverd --java-home "$JAVA_HOME" --d8 "$NEVERD_D8" \
  --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
  --work-dir "$RUNNER_TEMP/android-comparable-selector-carrier-sdk"

profile_group "Keep allocation before switch arguments and terminal throw arms" 21 600
python scripts/test_mobile_android_switch_allocation.py \
  --neverd build-mobile/bin/neverd --java-home "$JAVA_HOME" --d8 "$NEVERD_D8" \
  --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
  --work-dir "$RUNNER_TEMP/android-switch-allocation"
python scripts/test_mobile_android_switch_allocation.py --dependency-inputs \
  --neverd build-mobile/bin/neverd --java-home "$JAVA_HOME" --d8 "$NEVERD_D8" \
  --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
  --work-dir "$RUNNER_TEMP/android-switch-allocation-sdk"

profile_group "Keep forward nullary initialization and independent switch siblings" 21 600
python scripts/test_mobile_android_forward_allocation.py \
  --neverd build-mobile/bin/neverd --java-home "$JAVA_HOME" --d8 "$NEVERD_D8" \
  --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
  --work-dir "$RUNNER_TEMP/android-forward-allocation"
python scripts/test_mobile_android_forward_allocation.py --dependency-inputs \
  --neverd build-mobile/bin/neverd --java-home "$JAVA_HOME" --d8 "$NEVERD_D8" \
  --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
  --work-dir "$RUNNER_TEMP/android-forward-allocation-sdk"
python scripts/test_mobile_android_forward_allocation.py --shared-cleanup \
  --neverd build-mobile/bin/neverd --java-home "$JAVA_HOME" --d8 "$NEVERD_D8" \
  --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
  --work-dir "$RUNNER_TEMP/android-forward-allocation-shared-cleanup"

profile_group "Verify sequential monitor scopes with outer cleanup" 21 600
python scripts/test_mobile_android_sequential_monitors.py --outer-cleanup \
  --neverd build-mobile/bin/neverd --java-home "$JAVA_HOME" --d8 "$NEVERD_D8" \
  --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
  --work-dir "$RUNNER_TEMP/android-sequential-outer-monitors"
python scripts/test_mobile_android_sequential_monitors.py --outer-cleanup --dependency-provider \
  --neverd build-mobile/bin/neverd --java-home "$JAVA_HOME" --d8 "$NEVERD_D8" \
  --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
  --work-dir "$RUNNER_TEMP/android-sequential-outer-monitors-sdk"
python scripts/test_mobile_android_sequential_monitors.py --covered-capture \
  --neverd build-mobile/bin/neverd --java-home "$JAVA_HOME" --d8 "$NEVERD_D8" \
  --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
  --work-dir "$RUNNER_TEMP/android-monitor-covered-capture"

profile_group "Keep producer carriers and closed callback completion tails" 21 600
python scripts/test_mobile_android_suspended_predicate_carrier.py \
  --neverd build-mobile/bin/neverd --java-home "$JAVA_HOME" --d8 "$NEVERD_D8" \
  --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
  --work-dir "$RUNNER_TEMP/android-suspended-predicate-carrier"
python scripts/test_mobile_android_suspended_predicate_carrier.py --dependency-provider \
  --neverd build-mobile/bin/neverd --java-home "$JAVA_HOME" --d8 "$NEVERD_D8" \
  --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
  --work-dir "$RUNNER_TEMP/android-suspended-predicate-carrier-sdk"

profile_group "Keep acquired object identity across lock register reuse" 21 600
python scripts/test_mobile_android_sequential_monitors.py --lock-aliases \
  --neverd build-mobile/bin/neverd --java-home "$JAVA_HOME" --d8 "$NEVERD_D8" \
  --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
  --work-dir "$RUNNER_TEMP/android-monitor-lock-aliases"

profile_group "Keep checked star superclass arguments and prefix snapshots" 25 600
python scripts/test_mobile_android_checked_star_super_constructor.py \
  --neverd build-mobile/bin/neverd --java-home "$JAVA_HOME" --d8 "$NEVERD_D8" \
  --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
  --work-dir "$RUNNER_TEMP/android-checked-star-super-constructor"

profile_group "Keep primitive array reads and private conditional constructor calls" 21 600
python scripts/test_mobile_android_conditional_array_private.py \
  --neverd build-mobile/bin/neverd --java-home "$JAVA_HOME" --d8 "$NEVERD_D8" \
  --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
  --work-dir "$RUNNER_TEMP/android-conditional-array-private"
python scripts/test_mobile_android_conditional_array_private.py --wide --dependency-target \
  --neverd build-mobile/bin/neverd --java-home "$JAVA_HOME" --d8 "$NEVERD_D8" \
  --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
  --work-dir "$RUNNER_TEMP/android-conditional-array-private-wide-sdk"

profile_group "Keep Object-bounded ternary default inputs and results" 21 600
python scripts/test_mobile_android_bounded_owner_default.py --object-bound \
  --neverd build-mobile/bin/neverd --java-home "$JAVA_HOME" --d8 "$NEVERD_D8" \
  --kotlin-lib "$RUNNER_TEMP/neverd-kotlin-2.0.21/kotlinc/lib" \
  --kotlin-java-home "$NEVERD_KOTLIN_JAVA_HOME" \
  --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
  --work-dir "$RUNNER_TEMP/android-object-owner-default"
python scripts/test_mobile_android_bounded_owner_default.py --object-bound --dependency-contracts \
  --neverd build-mobile/bin/neverd --java-home "$JAVA_HOME" --d8 "$NEVERD_D8" \
  --kotlin-lib "$RUNNER_TEMP/neverd-kotlin-2.0.21/kotlinc/lib" \
  --kotlin-java-home "$NEVERD_KOTLIN_JAVA_HOME" \
  --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
  --work-dir "$RUNNER_TEMP/android-object-owner-default-sdk"

profile_group "Keep dependent producer bounds and seeded result carriers" 21 600
python scripts/test_mobile_android_subtype_seed_carrier.py \
  --neverd build-mobile/bin/neverd --java-home "$JAVA_HOME" --d8 "$NEVERD_D8" \
  --kotlin-lib "$RUNNER_TEMP/neverd-kotlin-2.0.21/kotlinc/lib" \
  --kotlin-java-home "$NEVERD_KOTLIN_JAVA_HOME" \
  --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
  --work-dir "$RUNNER_TEMP/android-subtype-seed-carrier"
python scripts/test_mobile_android_subtype_seed_carrier.py --dependency-provider \
  --neverd build-mobile/bin/neverd --java-home "$JAVA_HOME" --d8 "$NEVERD_D8" \
  --kotlin-lib "$RUNNER_TEMP/neverd-kotlin-2.0.21/kotlinc/lib" \
  --kotlin-java-home "$NEVERD_KOTLIN_JAVA_HOME" \
  --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
  --work-dir "$RUNNER_TEMP/android-subtype-seed-carrier-sdk"

profile_group "Keep complete reifiable auxiliaries beside produced elements" 21 600
python scripts/test_mobile_android_private_seeded_result.py \
  --producer-only --public-helper --reifiable-auxiliary --static-helper \
  --neverd build-mobile/bin/neverd --java-home "$JAVA_HOME" --d8 "$NEVERD_D8" \
  --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
  --work-dir "$RUNNER_TEMP/android-public-reifiable-element-static"
python scripts/test_mobile_android_private_seeded_result.py \
  --producer-only --public-helper --reifiable-auxiliary --dependency-api \
  --neverd build-mobile/bin/neverd --java-home "$JAVA_HOME" --d8 "$NEVERD_D8" \
  --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
  --work-dir "$RUNNER_TEMP/android-public-reifiable-element-instance-sdk"

profile_group "Keep closed observers and covariant void producers" 21 600
python scripts/test_mobile_android_unary_void_carrier.py --contextual-static \
  --neverd build-mobile/bin/neverd --java-home "$JAVA_HOME" --d8 "$NEVERD_D8" \
  --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
  --work-dir "$RUNNER_TEMP/android-contextual-void-producer"
python scripts/test_mobile_android_unary_void_carrier.py --contextual-static --dependency-provider \
  --neverd build-mobile/bin/neverd --java-home "$JAVA_HOME" --d8 "$NEVERD_D8" \
  --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
  --work-dir "$RUNNER_TEMP/android-contextual-void-producer-sdk"

profile_group "Keep invariant carriers and seeded void property inputs" 21 600
python scripts/test_mobile_android_unary_void_carrier.py --contextual-static --reifiable-setter \
  --neverd build-mobile/bin/neverd --java-home "$JAVA_HOME" --d8 "$NEVERD_D8" \
  --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
  --work-dir "$RUNNER_TEMP/android-reifiable-void-setter"
python scripts/test_mobile_android_unary_void_carrier.py --contextual-static --reifiable-setter --dependency-provider \
  --neverd build-mobile/bin/neverd --java-home "$JAVA_HOME" --d8 "$NEVERD_D8" \
  --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
  --work-dir "$RUNNER_TEMP/android-reifiable-void-setter-sdk"

profile_group "Keep independent static method formals on generic owners" 21 1200
python scripts/test_mobile_android_erased_factory_invocations.py --generic-owner \
  --neverd build-mobile/bin/neverd --java-home "$JAVA_HOME" --d8 "$NEVERD_D8" \
  --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
  --work-dir "$RUNNER_TEMP/android-generic-owner-carriers"
python scripts/test_mobile_android_erased_factory_invocations.py --generic-owner --dependency-provider \
  --neverd build-mobile/bin/neverd --java-home "$JAVA_HOME" --d8 "$NEVERD_D8" \
  --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
  --work-dir "$RUNNER_TEMP/android-generic-owner-carriers-sdk"

profile_group "Keep two independent direct seeds in static void calls" 21 1200
python scripts/test_mobile_android_direct_seed_void.py \
  --neverd build-mobile/bin/neverd --java-home "$JAVA_HOME" --d8 "$NEVERD_D8" \
  --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
  --work-dir "$RUNNER_TEMP/android-direct-seed-void"
python scripts/test_mobile_android_direct_seed_void.py --dependency-provider \
  --neverd build-mobile/bin/neverd --java-home "$JAVA_HOME" --d8 "$NEVERD_D8" \
  --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
  --work-dir "$RUNNER_TEMP/android-direct-seed-void-sdk"

profile_group "Keep every independent direct seed and partial store" 21 1200
python scripts/test_mobile_android_multiple_direct_seeds.py \
  --neverd build-mobile/bin/neverd --java-home "$JAVA_HOME" --d8 "$NEVERD_D8" \
  --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
  --work-dir "$RUNNER_TEMP/android-multiple-direct-seeds"
python scripts/test_mobile_android_multiple_direct_seeds.py --dependency-provider \
  --neverd build-mobile/bin/neverd --java-home "$JAVA_HOME" --d8 "$NEVERD_D8" \
  --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
  --work-dir "$RUNNER_TEMP/android-multiple-direct-seeds-sdk"

profile_group "Keep own result formals on proven captured getters" 25 1200
python scripts/test_mobile_android_captured_scalar_getter.py \
  --neverd build-mobile/bin/neverd --java-home "$JAVA_HOME" --d8 "$NEVERD_D8" \
  --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
  --work-dir "$RUNNER_TEMP/android-captured-scalar-getter"
python scripts/test_mobile_android_captured_scalar_getter.py --dependency-interface \
  --neverd build-mobile/bin/neverd --java-home "$JAVA_HOME" --d8 "$NEVERD_D8" \
  --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
  --work-dir "$RUNNER_TEMP/android-captured-scalar-getter-sdk"

profile_group "Keep private covariant producer calls and fixed nominal results" 21 1200
python scripts/test_mobile_android_private_covariant_nominal.py \
  --neverd build-mobile/bin/neverd --java-home "$JAVA_HOME" --d8 "$NEVERD_D8" \
  --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
  --work-dir "$RUNNER_TEMP/android-private-covariant-nominal"
python scripts/test_mobile_android_private_covariant_nominal.py --dependency-provider \
  --neverd build-mobile/bin/neverd --java-home "$JAVA_HOME" --d8 "$NEVERD_D8" \
  --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
  --work-dir "$RUNNER_TEMP/android-private-covariant-nominal-sdk"

profile_group "Keep recursive Comparable selector bounds in boolean predicates" 21 1200
python scripts/test_mobile_android_comparable_selector_carrier.py --boolean-result \
  --neverd build-mobile/bin/neverd --java-home "$JAVA_HOME" --d8 "$NEVERD_D8" \
  --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
  --work-dir "$RUNNER_TEMP/android-boolean-comparable-selector"
python scripts/test_mobile_android_comparable_selector_carrier.py --boolean-result --dependency-provider \
  --neverd build-mobile/bin/neverd --java-home "$JAVA_HOME" --d8 "$NEVERD_D8" \
  --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
  --work-dir "$RUNNER_TEMP/android-boolean-comparable-selector-sdk"

profile_group "Keep recursive Comparable selector bounds in void mutation calls" 21 1200
python scripts/test_mobile_android_comparable_selector_carrier.py --void-result \
  --neverd build-mobile/bin/neverd --java-home "$JAVA_HOME" --d8 "$NEVERD_D8" \
  --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
  --work-dir "$RUNNER_TEMP/android-void-comparable-selector"
python scripts/test_mobile_android_comparable_selector_carrier.py --void-result --dependency-provider \
  --neverd build-mobile/bin/neverd --java-home "$JAVA_HOME" --d8 "$NEVERD_D8" \
  --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
  --work-dir "$RUNNER_TEMP/android-void-comparable-selector-sdk"

profile_group "Keep scoped arrays and complete contravariant void consumers" 21 1200
python scripts/test_mobile_android_array_consumer_void.py \
  --neverd build-mobile/bin/neverd --java-home "$JAVA_HOME" --d8 "$NEVERD_D8" \
  --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
  --work-dir "$RUNNER_TEMP/android-array-consumer-void"
python scripts/test_mobile_android_array_consumer_void.py --dependency-provider \
  --neverd build-mobile/bin/neverd --java-home "$JAVA_HOME" --d8 "$NEVERD_D8" \
  --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
  --work-dir "$RUNNER_TEMP/android-array-consumer-void-sdk"

profile_group "Keep actual boolean results of scoped array consumer calls" 21 1200
python scripts/test_mobile_android_array_consumer_void.py --boolean-result \
  --neverd build-mobile/bin/neverd --java-home "$JAVA_HOME" --d8 "$NEVERD_D8" \
  --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
  --work-dir "$RUNNER_TEMP/android-array-consumer-boolean"
python scripts/test_mobile_android_array_consumer_void.py --boolean-result --dependency-provider \
  --neverd build-mobile/bin/neverd --java-home "$JAVA_HOME" --d8 "$NEVERD_D8" \
  --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
  --work-dir "$RUNNER_TEMP/android-array-consumer-boolean-sdk"

profile_group "Keep complete result contracts of instance nullary factories" 21 1200
python scripts/test_mobile_android_instance_nullary_factory.py \
  --neverd build-mobile/bin/neverd --java-home "$JAVA_HOME" --d8 "$NEVERD_D8" \
  --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
  --work-dir "$RUNNER_TEMP/android-instance-nullary-factory"
python scripts/test_mobile_android_instance_nullary_factory.py --dependency-provider \
  --neverd build-mobile/bin/neverd --java-home "$JAVA_HOME" --d8 "$NEVERD_D8" \
  --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
  --work-dir "$RUNNER_TEMP/android-instance-nullary-factory-sdk"

profile_group "Keep paired producer arrays and contravariant void consumers" 21 1200
python scripts/test_mobile_android_paired_array_void.py \
  --neverd build-mobile/bin/neverd --java-home "$JAVA_HOME" --d8 "$NEVERD_D8" \
  --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
  --work-dir "$RUNNER_TEMP/android-paired-array-void"
python scripts/test_mobile_android_paired_array_void.py --dependency-provider \
  --neverd build-mobile/bin/neverd --java-home "$JAVA_HOME" --d8 "$NEVERD_D8" \
  --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
  --work-dir "$RUNNER_TEMP/android-paired-array-void-sdk"

profile_group "Keep complete paired iterable map results and void writers" 21 1200
python scripts/test_mobile_android_paired_iterable_transfer.py \
  --neverd build-mobile/bin/neverd --java-home "$JAVA_HOME" --d8 "$NEVERD_D8" \
  --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
  --work-dir "$RUNNER_TEMP/android-paired-iterable-transfer"
python scripts/test_mobile_android_paired_iterable_transfer.py --dependency-provider \
  --neverd build-mobile/bin/neverd --java-home "$JAVA_HOME" --d8 "$NEVERD_D8" \
  --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
  --work-dir "$RUNNER_TEMP/android-paired-iterable-transfer-sdk"
python scripts/test_mobile_android_paired_iterable_transfer.py --void-entry \
  --neverd build-mobile/bin/neverd --java-home "$JAVA_HOME" --d8 "$NEVERD_D8" \
  --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
  --work-dir "$RUNNER_TEMP/android-paired-iterable-void"
python scripts/test_mobile_android_paired_iterable_transfer.py --void-entry --dependency-provider \
  --neverd build-mobile/bin/neverd --java-home "$JAVA_HOME" --d8 "$NEVERD_D8" \
  --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
  --work-dir "$RUNNER_TEMP/android-paired-iterable-void-sdk"

profile_group "Keep closed carrier bounds and exact formal seed results" 21 1200
python scripts/test_mobile_android_closed_collection_bound.py \
  --neverd build-mobile/bin/neverd --java-home "$JAVA_HOME" --d8 "$NEVERD_D8" \
  --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
  --work-dir "$RUNNER_TEMP/android-closed-collection-bound"
python scripts/test_mobile_android_closed_collection_bound.py --dependency-provider \
  --neverd build-mobile/bin/neverd --java-home "$JAVA_HOME" --d8 "$NEVERD_D8" \
  --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
  --work-dir "$RUNNER_TEMP/android-closed-collection-bound-sdk"

profile_group "Keep checked covariant interface super constructor arguments" 25 1200
python scripts/test_mobile_android_checked_interface_super.py \
  --neverd build-mobile/bin/neverd --java-home "$JAVA_HOME" --d8 "$NEVERD_D8" \
  --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
  --work-dir "$RUNNER_TEMP/android-checked-interface-super"
python scripts/test_mobile_android_checked_interface_super.py --dependency-provider \
  --neverd build-mobile/bin/neverd --java-home "$JAVA_HOME" --d8 "$NEVERD_D8" \
  --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
  --work-dir "$RUNNER_TEMP/android-checked-interface-super-sdk"

profile_group "Keep scoped inherited interface default super calls" 21 1200
python scripts/test_mobile_android_scoped_interface_super.py \
  --neverd build-mobile/bin/neverd --java-home "$JAVA_HOME" --d8 "$NEVERD_D8" \
  --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
  --work-dir "$RUNNER_TEMP/android-scoped-interface-super"
python scripts/test_mobile_android_scoped_interface_super.py --dependency-provider \
  --neverd build-mobile/bin/neverd --java-home "$JAVA_HOME" --d8 "$NEVERD_D8" \
  --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
  --work-dir "$RUNNER_TEMP/android-scoped-interface-super-sdk"

profile_group "Keep proven nonzero literal division in allocation fields" 21 1200
python scripts/test_mobile_android_allocation_integer_arithmetic.py --divide-fields \
  --neverd build-mobile/bin/neverd --java-home "$JAVA_HOME" --d8 "$NEVERD_D8" \
  --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
  --work-dir "$RUNNER_TEMP/android-allocation-literal-division"
python scripts/test_mobile_android_allocation_integer_arithmetic.py --divide-fields --dependency-target \
  --neverd build-mobile/bin/neverd --java-home "$JAVA_HOME" --d8 "$NEVERD_D8" \
  --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
  --work-dir "$RUNNER_TEMP/android-allocation-literal-division-sdk"

profile_group "Keep local generic void writer and reference updater slots" 21 1200
python scripts/test_mobile_android_local_void_setter.py \
  --neverd build-mobile/bin/neverd --java-home "$JAVA_HOME" --d8 "$NEVERD_D8" \
  --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
  --work-dir "$RUNNER_TEMP/android-local-void-setter"
python scripts/test_mobile_android_local_void_setter.py --dependency-provider \
  --neverd build-mobile/bin/neverd --java-home "$JAVA_HOME" --d8 "$NEVERD_D8" \
  --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
  --work-dir "$RUNNER_TEMP/android-local-void-setter-sdk"

profile_group "Keep scoped getter results before generic superclass delegation" 25 1200
python scripts/test_mobile_android_scoped_getter_super.py \
  --neverd build-mobile/bin/neverd --java-home "$JAVA_HOME" --d8 "$NEVERD_D8" \
  --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
  --work-dir "$RUNNER_TEMP/android-scoped-getter-super"
python scripts/test_mobile_android_scoped_getter_super.py --dependency-provider \
  --neverd build-mobile/bin/neverd --java-home "$JAVA_HOME" --d8 "$NEVERD_D8" \
  --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
  --work-dir "$RUNNER_TEMP/android-scoped-getter-super-sdk"

profile_group "Keep reference-array loops before generic superclass delegation" 25 1200
python scripts/test_mobile_android_reference_array_super.py \
  --neverd build-mobile/bin/neverd --java-home "$JAVA_HOME" --d8 "$NEVERD_D8" \
  --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
  --work-dir "$RUNNER_TEMP/android-reference-array-super"
python scripts/test_mobile_android_reference_array_super.py --dependency-provider \
  --neverd build-mobile/bin/neverd --java-home "$JAVA_HOME" --d8 "$NEVERD_D8" \
  --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
  --work-dir "$RUNNER_TEMP/android-reference-array-super-sdk"

profile_group "Keep nested covariant components in superclass array loops" 25 1200
python scripts/test_mobile_android_covariant_array_super.py \
  --neverd build-mobile/bin/neverd --java-home "$JAVA_HOME" --d8 "$NEVERD_D8" \
  --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
  --work-dir "$RUNNER_TEMP/android-covariant-array-super"
python scripts/test_mobile_android_covariant_array_super.py --dependency-provider \
  --neverd build-mobile/bin/neverd --java-home "$JAVA_HOME" --d8 "$NEVERD_D8" \
  --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
  --work-dir "$RUNNER_TEMP/android-covariant-array-super-sdk"

profile_group "Keep int shift distances and allocation input snapshots" 21 1200
python scripts/test_mobile_android_allocation_int_shifts.py \
  --neverd build-mobile/bin/neverd --java-home "$JAVA_HOME" --d8 "$NEVERD_D8" \
  --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
  --work-dir "$RUNNER_TEMP/android-int-shift-register-source"
python scripts/test_mobile_android_allocation_int_shifts.py --dependency-target \
  --neverd build-mobile/bin/neverd --java-home "$JAVA_HOME" --d8 "$NEVERD_D8" \
  --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
  --work-dir "$RUNNER_TEMP/android-int-shift-register-sdk"
python scripts/test_mobile_android_allocation_int_shifts.py --fields \
  --neverd build-mobile/bin/neverd --java-home "$JAVA_HOME" --d8 "$NEVERD_D8" \
  --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
  --work-dir "$RUNNER_TEMP/android-int-shift-field-source"
python scripts/test_mobile_android_allocation_int_shifts.py --fields --dependency-target \
  --neverd build-mobile/bin/neverd --java-home "$JAVA_HOME" --d8 "$NEVERD_D8" \
  --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
  --work-dir "$RUNNER_TEMP/android-int-shift-field-sdk"

profile_group "Keep protected final calls through unchanged generic inheritance" 21 1200
python scripts/test_mobile_android_inherited_generic_allocation.py \
  --neverd build-mobile/bin/neverd --java-home "$JAVA_HOME" --d8 "$NEVERD_D8" \
  --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
  --work-dir "$RUNNER_TEMP/android-inherited-generic-allocation"
python scripts/test_mobile_android_inherited_generic_allocation.py --dependency-provider \
  --neverd build-mobile/bin/neverd --java-home "$JAVA_HOME" --d8 "$NEVERD_D8" \
  --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
  --work-dir "$RUNNER_TEMP/android-inherited-generic-allocation-sdk"

profile_group "Keep independent paired seeds with closed array context and result" 21 1200
python scripts/test_mobile_android_paired_plain_array.py \
  --neverd build-mobile/bin/neverd --java-home "$JAVA_HOME" --d8 "$NEVERD_D8" \
  --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
  --work-dir "$RUNNER_TEMP/android-paired-plain-array"
python scripts/test_mobile_android_paired_plain_array.py --dependency-provider \
  --neverd build-mobile/bin/neverd --java-home "$JAVA_HOME" --d8 "$NEVERD_D8" \
  --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
  --work-dir "$RUNNER_TEMP/android-paired-plain-array-sdk"

profile_group "Keep independent scalar indices beside paired array seeds" 21 1200
python scripts/test_mobile_android_paired_array_indices.py \
  --neverd build-mobile/bin/neverd --java-home "$JAVA_HOME" --d8 "$NEVERD_D8" \
  --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
  --work-dir "$RUNNER_TEMP/android-paired-array-indices"
python scripts/test_mobile_android_paired_array_indices.py --dependency-provider \
  --neverd build-mobile/bin/neverd --java-home "$JAVA_HOME" --d8 "$NEVERD_D8" \
  --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
  --work-dir "$RUNNER_TEMP/android-paired-array-indices-sdk"

profile_group "Resolve concrete class-chain methods before interface declarations" 21 1200
python scripts/test_mobile_android_concrete_super_resolution.py \
  --neverd build-mobile/bin/neverd --java-home "$JAVA_HOME" --d8 "$NEVERD_D8" \
  --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
  --work-dir "$RUNNER_TEMP/android-concrete-super-resolution"
python scripts/test_mobile_android_concrete_super_resolution.py --dependency-provider \
  --neverd build-mobile/bin/neverd --java-home "$JAVA_HOME" --d8 "$NEVERD_D8" \
  --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
  --work-dir "$RUNNER_TEMP/android-concrete-super-resolution-sdk"

profile_group "Keep all independent direct seeds beside closed arrays" 21 1200
python scripts/test_mobile_android_independent_array_seeds.py \
  --neverd build-mobile/bin/neverd --java-home "$JAVA_HOME" --d8 "$NEVERD_D8" \
  --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
  --work-dir "$RUNNER_TEMP/android-independent-array-seeds"
python scripts/test_mobile_android_independent_array_seeds.py --dependency-provider \
  --neverd build-mobile/bin/neverd --java-home "$JAVA_HOME" --d8 "$NEVERD_D8" \
  --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
  --work-dir "$RUNNER_TEMP/android-independent-array-seeds-sdk"

profile_group "Keep scoped constructor seeds beside closed static snapshots" 25 1200
python scripts/test_mobile_android_scoped_constructor_delegation.py \
  --neverd build-mobile/bin/neverd --java-home "$JAVA_HOME" --d8 "$NEVERD_D8" \
  --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
  --work-dir "$RUNNER_TEMP/android-scoped-constructor-delegation"
python scripts/test_mobile_android_scoped_constructor_delegation.py --dependency-provider \
  --neverd build-mobile/bin/neverd --java-home "$JAVA_HOME" --d8 "$NEVERD_D8" \
  --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
  --work-dir "$RUNNER_TEMP/android-scoped-constructor-delegation-sdk"

profile_group "Keep receiver formal getter results in generic super calls" 25 1200
python scripts/test_mobile_android_formal_getter_super.py \
  --neverd build-mobile/bin/neverd --java-home "$JAVA_HOME" --d8 "$NEVERD_D8" \
  --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
  --work-dir "$RUNNER_TEMP/android-formal-getter-super"
python scripts/test_mobile_android_formal_getter_super.py --dependency-provider \
  --neverd build-mobile/bin/neverd --java-home "$JAVA_HOME" --d8 "$NEVERD_D8" \
  --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
  --work-dir "$RUNNER_TEMP/android-formal-getter-super-sdk"

profile_group "Keep closed getter snapshots and checked generic interface upcasts" 25 1200
python scripts/test_mobile_android_closed_getter_upcast.py \
  --neverd build-mobile/bin/neverd --java-home "$JAVA_HOME" \
  --d8 "$RUNNER_TEMP/neverd-java25-d8/lib/d8.jar" \
  --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
  --work-dir "$RUNNER_TEMP/android-closed-getter-upcast"
python scripts/test_mobile_android_closed_getter_upcast.py --dependency-provider \
  --neverd build-mobile/bin/neverd --java-home "$JAVA_HOME" \
  --d8 "$RUNNER_TEMP/neverd-java25-d8/lib/d8.jar" \
  --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
  --work-dir "$RUNNER_TEMP/android-closed-getter-upcast-sdk"

profile_group "Keep plain scalar Number bridge chains and wide returns" 21 1200
python scripts/test_mobile_android_scalar_number_bridges.py \
  --neverd build-mobile/bin/neverd --java-home "$JAVA_HOME" --d8 "$NEVERD_D8" \
  --kotlin-java-home "$NEVERD_KOTLIN_JAVA_HOME" \
  --kotlin-lib "$RUNNER_TEMP/neverd-kotlin-2.0.21/kotlinc/lib" \
  --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
  --work-dir "$RUNNER_TEMP/android-scalar-number-bridges"

profile_group "Keep array component factories beside closed context references" 21 1200
python scripts/test_mobile_android_array_seed_factories.py --closed-context \
  --neverd build-mobile/bin/neverd --java-home "$JAVA_HOME" --d8 "$NEVERD_D8" \
  --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
  --work-dir "$RUNNER_TEMP/android-array-context-factory"
python scripts/test_mobile_android_array_seed_factories.py --closed-context --dependency-provider \
  --neverd build-mobile/bin/neverd --java-home "$JAVA_HOME" --d8 "$NEVERD_D8" \
  --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
  --work-dir "$RUNNER_TEMP/android-array-context-factory-sdk"

profile_group "Keep distinct generic and nominal map getter return slots" 21 1200
python scripts/test_mobile_android_nominal_getter_overloads.py \
  --neverd build-mobile/bin/neverd --java-home "$JAVA_HOME" --d8 "$NEVERD_D8" \
  --kotlin-java-home "$NEVERD_KOTLIN_JAVA_HOME" \
  --kotlin-lib "$RUNNER_TEMP/neverd-kotlin-2.0.21/kotlinc/lib" \
  --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
  --work-dir "$RUNNER_TEMP/android-nominal-getter-overloads"

profile_group "Keep closed map constructor origins and original generic headers" 25 1200
python scripts/test_mobile_android_closed_map_builder.py \
  --neverd build-mobile/bin/neverd --java-home "$JAVA_HOME" --d8 "$NEVERD_D8" \
  --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
  --work-dir "$RUNNER_TEMP/android-closed-map-builder"
python scripts/test_mobile_android_closed_map_builder.py --dependency-parent \
  --neverd build-mobile/bin/neverd --java-home "$JAVA_HOME" --d8 "$NEVERD_D8" \
  --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
  --work-dir "$RUNNER_TEMP/android-closed-map-builder-sdk"

profile_group "Keep nested carrier adapter formal and closed wrapper slots" 21 1200
python scripts/test_mobile_android_nested_carrier_adapter.py \
  --neverd build-mobile/bin/neverd --java-home "$JAVA_HOME" --d8 "$NEVERD_D8" \
  --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
  --work-dir "$RUNNER_TEMP/android-nested-carrier-adapter"
python scripts/test_mobile_android_nested_carrier_adapter.py --dependency-provider \
  --neverd build-mobile/bin/neverd --java-home "$JAVA_HOME" --d8 "$NEVERD_D8" \
  --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
  --work-dir "$RUNNER_TEMP/android-nested-carrier-adapter-sdk"

profile_group "Keep static return-only overload bodies and exact callers" 21 1200
python scripts/test_mobile_android_static_return_overloads.py \
  --neverd build-mobile/bin/neverd --java-home "$JAVA_HOME" --d8 "$NEVERD_D8" \
  --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
  --work-dir "$RUNNER_TEMP/android-static-return-overloads"

profile_group "Keep nullary carrier factories with a literal Object result tail" 21 1200
python scripts/test_mobile_android_nullary_carrier_factories.py --closed-object-tail \
  --neverd build-mobile/bin/neverd --java-home "$JAVA_HOME" --d8 "$NEVERD_D8" \
  --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
  --work-dir "$RUNNER_TEMP/android-nullary-closed-tail"
python scripts/test_mobile_android_nullary_carrier_factories.py --closed-object-tail --dependency-provider \
  --neverd build-mobile/bin/neverd --java-home "$JAVA_HOME" --d8 "$NEVERD_D8" \
  --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
  --work-dir "$RUNNER_TEMP/android-nullary-closed-tail-sdk"

profile_group "Keep scoped carrier inputs with fixed context and result tails" 21 1200
python scripts/test_mobile_android_contextual_carrier_factories.py \
  --neverd build-mobile/bin/neverd --java-home "$JAVA_HOME" --d8 "$NEVERD_D8" \
  --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
  --work-dir "$RUNNER_TEMP/android-contextual-closed-tail"
python scripts/test_mobile_android_contextual_carrier_factories.py --dependency-provider \
  --neverd build-mobile/bin/neverd --java-home "$JAVA_HOME" --d8 "$NEVERD_D8" \
  --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
  --work-dir "$RUNNER_TEMP/android-contextual-closed-tail-sdk"

profile_group "Report synthetic flags on both static return-only overload entries" 21 1200
python scripts/test_mobile_android_static_return_overloads.py --synthetic-slots \
  --neverd build-mobile/bin/neverd --java-home "$JAVA_HOME" --d8 "$NEVERD_D8" \
  --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
  --work-dir "$RUNNER_TEMP/android-static-synthetic-return-slots"

profile_group "Keep private static two-formal carrier transfer scope and order" 21 1200
python scripts/test_mobile_android_flat_pair_transfer.py --private-transfer \
  --neverd build-mobile/bin/neverd --java-home "$JAVA_HOME" --d8 "$NEVERD_D8" \
  --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
  --work-dir "$RUNNER_TEMP/android-private-pair-transfer"
python scripts/test_mobile_android_flat_pair_transfer.py --private-transfer --dependency-carrier \
  --neverd build-mobile/bin/neverd --java-home "$JAVA_HOME" --d8 "$NEVERD_D8" \
  --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
  --work-dir "$RUNNER_TEMP/android-private-pair-transfer-sdk"

profile_group "Keep fresh raw binary callbacks and complete forwarded interface graphs" 25 1200
python scripts/test_mobile_android_allocated_binary_callback.py \
  --neverd build-mobile/bin/neverd --java-home "$JAVA_HOME" --d8 "$NEVERD_D8" \
  --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
  --work-dir "$RUNNER_TEMP/android-allocated-binary-callback"
python scripts/test_mobile_android_allocated_binary_callback.py --dependency-contracts \
  --neverd build-mobile/bin/neverd --java-home "$JAVA_HOME" --d8 "$NEVERD_D8" \
  --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
  --work-dir "$RUNNER_TEMP/android-allocated-binary-callback-sdk"

profile_group "Keep private seeded void callbacks and closed sibling inputs" 21 1200
python scripts/test_mobile_android_seeded_void_carriers.py --private-helper \
  --neverd build-mobile/bin/neverd --java-home "$JAVA_HOME" --d8 "$NEVERD_D8" \
  --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
  --work-dir "$RUNNER_TEMP/android-private-seeded-void"
python scripts/test_mobile_android_seeded_void_carriers.py --private-helper --dependency-inputs \
  --neverd build-mobile/bin/neverd --java-home "$JAVA_HOME" --d8 "$NEVERD_D8" \
  --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
  --work-dir "$RUNNER_TEMP/android-private-seeded-void-sdk"

profile_group "Keep private interface-bounded results and complete input bindings" 21 1200
python scripts/test_mobile_android_private_interface_result.py \
  --neverd build-mobile/bin/neverd --java-home "$JAVA_HOME" --d8 "$NEVERD_D8" \
  --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
  --work-dir "$RUNNER_TEMP/android-private-interface-result"
python scripts/test_mobile_android_private_interface_result.py --dependency-inputs \
  --neverd build-mobile/bin/neverd --java-home "$JAVA_HOME" --d8 "$NEVERD_D8" \
  --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
  --work-dir "$RUNNER_TEMP/android-private-interface-result-sdk"

profile_group "Keep producer callback bindings with fixed nominal results" 21 1200
python scripts/test_mobile_android_producer_callback_nominal.py \
  --neverd build-mobile/bin/neverd --java-home "$JAVA_HOME" --d8 "$NEVERD_D8" \
  --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
  --work-dir "$RUNNER_TEMP/android-producer-callback-nominal"
python scripts/test_mobile_android_producer_callback_nominal.py --dependency-inputs \
  --neverd build-mobile/bin/neverd --java-home "$JAVA_HOME" --d8 "$NEVERD_D8" \
  --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
  --work-dir "$RUNNER_TEMP/android-producer-callback-nominal-sdk"

profile_group "Keep total long arithmetic in original allocation argument order" 21 1200
python scripts/test_mobile_android_allocation_long_arithmetic.py \
  --neverd build-mobile/bin/neverd --java-home "$JAVA_HOME" --d8 "$NEVERD_D8" \
  --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
  --work-dir "$RUNNER_TEMP/android-allocation-long-arithmetic"
python scripts/test_mobile_android_allocation_long_arithmetic.py --dependency-provider \
  --neverd build-mobile/bin/neverd --java-home "$JAVA_HOME" --d8 "$NEVERD_D8" \
  --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
  --work-dir "$RUNNER_TEMP/android-allocation-long-arithmetic-sdk"

profile_group "Keep private callbacks with closed inputs and scoped element results" 21 1200
python scripts/test_mobile_android_private_callback_element.py \
  --neverd build-mobile/bin/neverd --java-home "$JAVA_HOME" --d8 "$NEVERD_D8" \
  --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
  --work-dir "$RUNNER_TEMP/android-private-callback-element"
python scripts/test_mobile_android_private_callback_element.py --dependency-inputs \
  --neverd build-mobile/bin/neverd --java-home "$JAVA_HOME" --d8 "$NEVERD_D8" \
  --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
  --work-dir "$RUNNER_TEMP/android-private-callback-element-sdk"

profile_group "Keep closed auxiliary arguments beside private callback element results" 21 1200
python scripts/test_mobile_android_context_callback_element.py \
  --neverd build-mobile/bin/neverd --java-home "$JAVA_HOME" --d8 "$NEVERD_D8" \
  --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
  --work-dir "$RUNNER_TEMP/android-context-callback-element"
python scripts/test_mobile_android_context_callback_element.py --dependency-inputs \
  --neverd build-mobile/bin/neverd --java-home "$JAVA_HOME" --d8 "$NEVERD_D8" \
  --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
  --work-dir "$RUNNER_TEMP/android-context-callback-element-sdk"

profile_group "Keep original class bounds on private callback element results" 21 1200
python scripts/test_mobile_android_bounded_callback_element.py \
  --neverd build-mobile/bin/neverd --java-home "$JAVA_HOME" --d8 "$NEVERD_D8" \
  --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
  --work-dir "$RUNNER_TEMP/android-bounded-callback-element"
python scripts/test_mobile_android_bounded_callback_element.py --dependency-inputs \
  --neverd build-mobile/bin/neverd --java-home "$JAVA_HOME" --d8 "$NEVERD_D8" \
  --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
  --work-dir "$RUNNER_TEMP/android-bounded-callback-element-sdk"

profile_group "Keep raw factory results through scoped same-owner constructor delegation" 21 1200
python scripts/test_mobile_android_raw_factory_delegation.py \
  --neverd build-mobile/bin/neverd --java-home "$JAVA_HOME" --d8 "$NEVERD_D8" \
  --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
  --work-dir "$RUNNER_TEMP/android-raw-factory-delegation"
python scripts/test_mobile_android_raw_factory_delegation.py --dependency-inputs \
  --neverd build-mobile/bin/neverd --java-home "$JAVA_HOME" --d8 "$NEVERD_D8" \
  --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
  --work-dir "$RUNNER_TEMP/android-raw-factory-delegation-sdk"

profile_group "Keep nested monitor acquisition stacks and original finally releases" 21 1200
python scripts/test_mobile_android_nested_lock_scopes.py \
  --neverd build-mobile/bin/neverd --java-home "$JAVA_HOME" --d8 "$NEVERD_D8" \
  --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
  --work-dir "$RUNNER_TEMP/android-nested-lock-scopes"
python scripts/test_mobile_android_nested_lock_scopes.py --dependency-inputs \
  --neverd build-mobile/bin/neverd --java-home "$JAVA_HOME" --d8 "$NEVERD_D8" \
  --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
  --work-dir "$RUNNER_TEMP/android-nested-lock-scopes-sdk"

profile_group "Keep private instance consumers returning their own static member type" 21 1200
python scripts/test_mobile_android_private_consumer_result.py \
  --neverd build-mobile/bin/neverd --java-home "$JAVA_HOME" --d8 "$NEVERD_D8" \
  --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
  --work-dir "$RUNNER_TEMP/android-private-consumer-result"
python scripts/test_mobile_android_private_consumer_result.py --dependency-inputs \
  --neverd build-mobile/bin/neverd --java-home "$JAVA_HOME" --d8 "$NEVERD_D8" \
  --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
  --work-dir "$RUNNER_TEMP/android-private-consumer-result-sdk"

profile_group "Preserve protected conditional constructor arguments and exception frames" 21 1200
python scripts/test_mobile_android_protected_conditional_allocation.py \
  --neverd build-mobile/bin/neverd --java-home "$JAVA_HOME" --d8 "$NEVERD_D8" \
  --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
  --work-dir "$RUNNER_TEMP/android-protected-conditional-allocation"
python scripts/test_mobile_android_protected_conditional_allocation.py --dependency-inputs \
  --neverd build-mobile/bin/neverd --java-home "$JAVA_HOME" --d8 "$NEVERD_D8" \
  --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
  --work-dir "$RUNNER_TEMP/android-protected-conditional-allocation-sdk"

profile_group "Keep anonymous property writers in their captured generic scope" 25 1200
python scripts/test_mobile_android_captured_local_setter.py \
  --neverd build-mobile/bin/neverd --java-home "$JAVA_HOME" --d8 "$NEVERD_D8" \
  --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
  --work-dir "$RUNNER_TEMP/android-captured-local-setter"
python scripts/test_mobile_android_captured_local_setter.py --dependency-provider \
  --neverd build-mobile/bin/neverd --java-home "$JAVA_HOME" --d8 "$NEVERD_D8" \
  --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
  --work-dir "$RUNNER_TEMP/android-captured-local-setter-sdk"

profile_group "Keep closed generic superclass arguments in their original guarded prefix" 25 1200
python scripts/test_mobile_android_closed_super_arguments.py \
  --neverd build-mobile/bin/neverd --java-home "$JAVA_HOME" \
  --d8 "$RUNNER_TEMP/neverd-java25-d8/lib/d8.jar" \
  --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
  --work-dir "$RUNNER_TEMP/android-closed-super-arguments"
python scripts/test_mobile_android_closed_super_arguments.py --dependency-inputs \
  --neverd build-mobile/bin/neverd --java-home "$JAVA_HOME" \
  --d8 "$RUNNER_TEMP/neverd-java25-d8/lib/d8.jar" \
  --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
  --work-dir "$RUNNER_TEMP/android-closed-super-arguments-sdk"

profile_group "Retain typed allocation defaults before guarded superclass preparation" 25 1200
python scripts/test_mobile_android_guarded_typed_defaults.py \
  --neverd build-mobile/bin/neverd --java-home "$JAVA_HOME" \
  --d8 "$RUNNER_TEMP/neverd-java25-d8/lib/d8.jar" \
  --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
  --work-dir "$RUNNER_TEMP/android-guarded-typed-defaults"
python scripts/test_mobile_android_guarded_typed_defaults.py --dependency-inputs \
  --neverd build-mobile/bin/neverd --java-home "$JAVA_HOME" \
  --d8 "$RUNNER_TEMP/neverd-java25-d8/lib/d8.jar" \
  --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
  --work-dir "$RUNNER_TEMP/android-guarded-typed-defaults-sdk"

profile_group "Retain typed allocation defaults across linear superclass preparation" 25 1200
python scripts/test_mobile_android_guarded_typed_defaults.py --linear-prefix \
  --neverd build-mobile/bin/neverd --java-home "$JAVA_HOME" \
  --d8 "$RUNNER_TEMP/neverd-java25-d8/lib/d8.jar" \
  --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
  --work-dir "$RUNNER_TEMP/android-linear-typed-defaults"
python scripts/test_mobile_android_guarded_typed_defaults.py --linear-prefix --dependency-inputs \
  --neverd build-mobile/bin/neverd --java-home "$JAVA_HOME" \
  --d8 "$RUNNER_TEMP/neverd-java25-d8/lib/d8.jar" \
  --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
  --work-dir "$RUNNER_TEMP/android-linear-typed-defaults-sdk"

profile_group "Preserve inherited interface defaults without ancestor static accessors" 21 1200
python scripts/test_mobile_android_inherited_descriptor_default.py \
  --neverd build-mobile/bin/neverd --java-home "$JAVA_HOME" \
  --kotlin-java-home "$NEVERD_KOTLIN_JAVA_HOME" --kotlin-lib "$RUNNER_TEMP/neverd-kotlin-2.0.21/kotlinc/lib" \
  --d8 "$NEVERD_D8" --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
  --work-dir "$RUNNER_TEMP/android-inherited-descriptor-default"
python scripts/test_mobile_android_inherited_descriptor_default.py --dependency-trace \
  --neverd build-mobile/bin/neverd --java-home "$JAVA_HOME" \
  --kotlin-java-home "$NEVERD_KOTLIN_JAVA_HOME" --kotlin-lib "$RUNNER_TEMP/neverd-kotlin-2.0.21/kotlinc/lib" \
  --d8 "$NEVERD_D8" --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
  --work-dir "$RUNNER_TEMP/android-inherited-descriptor-default-sdk"

profile_group "Bind array consumers and invariant results to the original component" 21 1200
python scripts/test_mobile_android_array_consumer_carrier.py \
  --neverd build-mobile/bin/neverd --java-home "$JAVA_HOME" --d8 "$NEVERD_D8" \
  --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
  --work-dir "$RUNNER_TEMP/android-array-consumer-carrier"
python scripts/test_mobile_android_array_consumer_carrier.py --dependency-provider \
  --neverd build-mobile/bin/neverd --java-home "$JAVA_HOME" --d8 "$NEVERD_D8" \
  --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
  --work-dir "$RUNNER_TEMP/android-array-consumer-carrier-sdk"

profile_group "Retain closed descriptor interface super calls in static member scopes" 21 1200
python scripts/test_mobile_android_closed_interface_super.py \
  --neverd build-mobile/bin/neverd --java-home "$JAVA_HOME" --d8 "$NEVERD_D8" \
  --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
  --work-dir "$RUNNER_TEMP/android-closed-interface-super"
python scripts/test_mobile_android_closed_interface_super.py --dependency-provider \
  --neverd build-mobile/bin/neverd --java-home "$JAVA_HOME" --d8 "$NEVERD_D8" \
  --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
  --work-dir "$RUNNER_TEMP/android-closed-interface-super-sdk"

profile_group "Retain generic method defaults through canonical invocation receipts" 21 1200
python scripts/test_mobile_android_generic_default_fold.py \
  --neverd build-mobile/bin/neverd --java-home "$JAVA_HOME" \
  --kotlin-java-home "$NEVERD_KOTLIN_JAVA_HOME" --kotlin-lib "$RUNNER_TEMP/neverd-kotlin-2.0.21/kotlinc/lib" \
  --d8 "$NEVERD_D8" --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
  --work-dir "$RUNNER_TEMP/android-generic-default-fold"
python scripts/test_mobile_android_generic_default_fold.py --dependency-contracts \
  --neverd build-mobile/bin/neverd --java-home "$JAVA_HOME" \
  --kotlin-java-home "$NEVERD_KOTLIN_JAVA_HOME" --kotlin-lib "$RUNNER_TEMP/neverd-kotlin-2.0.21/kotlinc/lib" \
  --d8 "$NEVERD_D8" --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
  --work-dir "$RUNNER_TEMP/android-generic-default-fold-sdk"

profile_group "Bind private nested carrier trees in their exact declaration scope" 21 1200
python scripts/test_mobile_android_private_nested_carrier.py \
  --neverd build-mobile/bin/neverd --java-home "$JAVA_HOME" --d8 "$NEVERD_D8" \
  --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
  --work-dir "$RUNNER_TEMP/android-private-nested-carrier"
python scripts/test_mobile_android_private_nested_carrier.py --dependency-provider \
  --neverd build-mobile/bin/neverd --java-home "$JAVA_HOME" --d8 "$NEVERD_D8" \
  --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
  --work-dir "$RUNNER_TEMP/android-private-nested-carrier-sdk"

profile_group "Compose captured coroutine prefixes with closed superclass contracts" 25 1200
python scripts/test_mobile_android_captured_closed_super.py \
  --neverd build-mobile/bin/neverd --java-home "$JAVA_HOME" \
  --kotlin-java-home "$NEVERD_KOTLIN_JAVA_HOME" --kotlin-lib "$RUNNER_TEMP/neverd-kotlin-2.0.21/kotlinc/lib" \
  --d8 "$NEVERD_D8" --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
  --work-dir "$RUNNER_TEMP/android-captured-closed-super-debug"
python scripts/test_mobile_android_captured_closed_super.py --release-dex \
  --neverd build-mobile/bin/neverd --java-home "$JAVA_HOME" \
  --kotlin-java-home "$NEVERD_KOTLIN_JAVA_HOME" --kotlin-lib "$RUNNER_TEMP/neverd-kotlin-2.0.21/kotlinc/lib" \
  --d8 "$NEVERD_D8" --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
  --work-dir "$RUNNER_TEMP/android-captured-closed-super-release"

profile_group "Retain direct interface seeds and complete closed-result visitors" 21 1200
python scripts/test_mobile_android_interface_seed_visitor.py \
  --neverd build-mobile/bin/neverd --java-home "$JAVA_HOME" --d8 "$NEVERD_D8" \
  --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
  --work-dir "$RUNNER_TEMP/android-interface-seed-visitor"
python scripts/test_mobile_android_interface_seed_visitor.py --dependency-provider \
  --neverd build-mobile/bin/neverd --java-home "$JAVA_HOME" --d8 "$NEVERD_D8" \
  --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
  --work-dir "$RUNNER_TEMP/android-interface-seed-visitor-sdk"
python scripts/test_mobile_android_interface_seed_visitor.py --private-body \
  --neverd build-mobile/bin/neverd --java-home "$JAVA_HOME" --d8 "$NEVERD_D8" \
  --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
  --work-dir "$RUNNER_TEMP/android-interface-seed-visitor-private"

profile_group "Retain terminal abort tails inside nested allocation arguments" 21 1200
python scripts/test_mobile_android_trailing_allocation_abort.py \
  --neverd build-mobile/bin/neverd --java-home "$JAVA_HOME" --d8 "$NEVERD_D8" \
  --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
  --work-dir "$RUNNER_TEMP/android-trailing-allocation-abort"
python scripts/test_mobile_android_trailing_allocation_abort.py --dependency-provider \
  --neverd build-mobile/bin/neverd --java-home "$JAVA_HOME" --d8 "$NEVERD_D8" \
  --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
  --work-dir "$RUNNER_TEMP/android-trailing-allocation-abort-sdk"

profile_group "Retain scoped masks on public open generic owners" 25 1200
python scripts/test_mobile_android_open_scoped_mask.py \
  --neverd build-mobile/bin/neverd --java-home "$JAVA_HOME" \
  --kotlin-java-home "$NEVERD_KOTLIN_JAVA_HOME" --kotlin-lib "$RUNNER_TEMP/neverd-kotlin-2.0.21/kotlinc/lib" \
  --d8 "$NEVERD_D8" --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
  --work-dir "$RUNNER_TEMP/android-open-scoped-mask"
python scripts/test_mobile_android_open_scoped_mask.py --dependency-trace \
  --neverd build-mobile/bin/neverd --java-home "$JAVA_HOME" \
  --kotlin-java-home "$NEVERD_KOTLIN_JAVA_HOME" --kotlin-lib "$RUNNER_TEMP/neverd-kotlin-2.0.21/kotlinc/lib" \
  --d8 "$NEVERD_D8" --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
  --work-dir "$RUNNER_TEMP/android-open-scoped-mask-sdk"

profile_group "Retain original outer formals through captured superclass constructors" 25 1200
python scripts/test_mobile_android_captured_owner_super.py \
  --neverd build-mobile/bin/neverd --java-home "$JAVA_HOME" \
  --kotlin-java-home "$NEVERD_KOTLIN_JAVA_HOME" --kotlin-lib "$RUNNER_TEMP/neverd-kotlin-2.0.21/kotlinc/lib" \
  --d8 "$NEVERD_D8" --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
  --work-dir "$RUNNER_TEMP/android-captured-owner-super"
python scripts/test_mobile_android_captured_owner_super.py --dependency-inputs \
  --neverd build-mobile/bin/neverd --java-home "$JAVA_HOME" \
  --kotlin-java-home "$NEVERD_KOTLIN_JAVA_HOME" --kotlin-lib "$RUNNER_TEMP/neverd-kotlin-2.0.21/kotlinc/lib" \
  --d8 "$NEVERD_D8" --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
  --work-dir "$RUNNER_TEMP/android-captured-owner-super-sdk"

profile_group "Snapshot initialized instance fields before constructor delegation" 25 1200
python scripts/test_mobile_android_constructor_instance_reads.py \
  --neverd build-mobile/bin/neverd --java-home "$JAVA_HOME" --d8 "$NEVERD_D8" \
  --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
  --work-dir "$RUNNER_TEMP/android-constructor-instance-reads"
python scripts/test_mobile_android_constructor_instance_reads.py --dependency-provider \
  --neverd build-mobile/bin/neverd --java-home "$JAVA_HOME" --d8 "$NEVERD_D8" \
  --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
  --work-dir "$RUNNER_TEMP/android-constructor-instance-reads-sdk"

profile_group "Preserve scalar comparisons and conditional constructor allocations" 25 1200
python scripts/test_mobile_android_constructor_comparisons.py \
  --neverd build-mobile/bin/neverd --java-home "$JAVA_HOME" --d8 "$NEVERD_D8" \
  --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
  --work-dir "$RUNNER_TEMP/android-constructor-comparisons"
python scripts/test_mobile_android_constructor_comparisons.py --dependency-provider \
  --neverd build-mobile/bin/neverd --java-home "$JAVA_HOME" --d8 "$NEVERD_D8" \
  --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
  --work-dir "$RUNNER_TEMP/android-constructor-comparisons-sdk"

profile_group final-diamond-aborts 21 1200
python scripts/test_mobile_android_final_diamond_aborts.py \
  --neverd build-mobile/bin/neverd --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
  --java-home "$JAVA_HOME" --d8 "$NEVERD_D8" \
  --work-dir "$RUNNER_TEMP/android-final-diamond-aborts"
python scripts/test_mobile_android_final_diamond_aborts.py \
  --neverd build-mobile/bin/neverd --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
  --java-home "$JAVA_HOME" --d8 "$NEVERD_D8" --dependency-provider \
  --work-dir "$RUNNER_TEMP/android-final-diamond-aborts-sdk"

profile_group "Preserve exact copies inside constructor allocations" 25 1200
python scripts/test_mobile_android_allocation_prefix_copies.py \
  --neverd build-mobile/bin/neverd --java-home "$JAVA_HOME" --d8 "$NEVERD_D8" \
  --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
  --work-dir "$RUNNER_TEMP/android-allocation-prefix-copies"
python scripts/test_mobile_android_allocation_prefix_copies.py --dependency-provider \
  --neverd build-mobile/bin/neverd --java-home "$JAVA_HOME" --d8 "$NEVERD_D8" \
  --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
  --work-dir "$RUNNER_TEMP/android-allocation-prefix-copies-sdk"

profile_group "Preserve total floating arithmetic in constructor prefixes" 25 1200
python scripts/test_mobile_android_floating_constructor_prefix.py \
  --neverd build-mobile/bin/neverd --java-home "$JAVA_HOME" --d8 "$NEVERD_D8" \
  --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
  --work-dir "$RUNNER_TEMP/android-floating-constructor-prefix"
python scripts/test_mobile_android_floating_constructor_prefix.py --dependency-provider \
  --neverd build-mobile/bin/neverd --java-home "$JAVA_HOME" --d8 "$NEVERD_D8" \
  --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
  --work-dir "$RUNNER_TEMP/android-floating-constructor-prefix-sdk"

profile_group "Preserve complete synthetic interface naming contracts" 21 1200
python scripts/test_mobile_android_synthetic_interface_roots.py \
  --neverd build-mobile/bin/neverd --java-home "$JAVA_HOME" --d8 "$NEVERD_D8" \
  --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
  --kotlin-lib "$RUNNER_TEMP/neverd-kotlin-2.0.21/kotlinc/lib" \
  --work-dir "$RUNNER_TEMP/android-synthetic-interface-roots"
python scripts/test_mobile_android_synthetic_interface_roots.py --dependency-provider \
  --neverd build-mobile/bin/neverd --java-home "$JAVA_HOME" --d8 "$NEVERD_D8" \
  --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
  --kotlin-lib "$RUNNER_TEMP/neverd-kotlin-2.0.21/kotlinc/lib" \
  --work-dir "$RUNNER_TEMP/android-synthetic-interface-roots-sdk"

profile_group "Preserve named inherited default dispatch" 21 1200
python scripts/test_mobile_android_named_default_accessors.py \
  --neverd build-mobile/bin/neverd --java-home "$JAVA_HOME" --d8 "$NEVERD_D8" \
  --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
  --kotlin-lib "$RUNNER_TEMP/neverd-kotlin-2.0.21/kotlinc/lib" \
  --work-dir "$RUNNER_TEMP/android-named-default-accessors"
python scripts/test_mobile_android_named_default_accessors.py --dependency-provider \
  --neverd build-mobile/bin/neverd --java-home "$JAVA_HOME" --d8 "$NEVERD_D8" \
  --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
  --kotlin-lib "$RUNNER_TEMP/neverd-kotlin-2.0.21/kotlinc/lib" \
  --work-dir "$RUNNER_TEMP/android-named-default-accessors-sdk"

profile_group "Preserve closed callback interface defaults" 21 1200
python scripts/test_mobile_android_closed_callback_default_accessors.py \
  --neverd build-mobile/bin/neverd --java-home "$JAVA_HOME" --d8 "$NEVERD_D8" \
  --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
  --kotlin-lib "$RUNNER_TEMP/neverd-kotlin-2.0.21/kotlinc/lib" \
  --work-dir "$RUNNER_TEMP/android-closed-callback-default-accessors"
python scripts/test_mobile_android_closed_callback_default_accessors.py --dependency-provider \
  --neverd build-mobile/bin/neverd --java-home "$JAVA_HOME" --d8 "$NEVERD_D8" \
  --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
  --kotlin-lib "$RUNNER_TEMP/neverd-kotlin-2.0.21/kotlinc/lib" \
  --work-dir "$RUNNER_TEMP/android-closed-callback-default-accessors-sdk"

profile_group "Retain encoded static preparation reads before constructors" 25 1200
python scripts/test_mobile_android_encoded_static_prefix.py \
  --neverd build-mobile/bin/neverd --java-home "$JAVA_HOME" --d8 "$NEVERD_D8" \
  --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
  --work-dir "$RUNNER_TEMP/android-encoded-static-prefix"
python scripts/test_mobile_android_encoded_static_prefix.py --dependency-provider \
  --neverd build-mobile/bin/neverd --java-home "$JAVA_HOME" --d8 "$NEVERD_D8" \
  --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
  --work-dir "$RUNNER_TEMP/android-encoded-static-prefix-sdk"

profile_group "Retain lexically owned callback singletons before constructors" 25 1200
python scripts/test_mobile_android_local_static_prefix.py \
  --neverd build-mobile/bin/neverd --java-home "$JAVA_HOME" --d8 "$NEVERD_D8" \
  --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
  --kotlin-java-home "$NEVERD_KOTLIN_JAVA_HOME" \
  --kotlin-lib "$RUNNER_TEMP/neverd-kotlin-2.0.21/kotlinc/lib" \
  --work-dir "$RUNNER_TEMP/android-local-static-prefix"
python scripts/test_mobile_android_local_static_prefix.py --dependency-provider \
  --neverd build-mobile/bin/neverd --java-home "$JAVA_HOME" --d8 "$NEVERD_D8" \
  --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
  --kotlin-java-home "$NEVERD_KOTLIN_JAVA_HOME" \
  --kotlin-lib "$RUNNER_TEMP/neverd-kotlin-2.0.21/kotlinc/lib" \
  --work-dir "$RUNNER_TEMP/android-local-static-prefix-sdk"

profile_group "Retain total typed long arithmetic before constructor delegation" 25 1200
python scripts/test_mobile_android_long_constructor_prefix.py \
  --neverd build-mobile/bin/neverd --java-home "$JAVA_HOME" --d8 "$NEVERD_D8" \
  --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
  --kotlin-java-home "$NEVERD_KOTLIN_JAVA_HOME" \
  --kotlin-lib "$RUNNER_TEMP/neverd-kotlin-2.0.21/kotlinc/lib" \
  --work-dir "$RUNNER_TEMP/android-long-constructor-prefix"
python scripts/test_mobile_android_long_constructor_prefix.py --dependency-provider \
  --neverd build-mobile/bin/neverd --java-home "$JAVA_HOME" --d8 "$NEVERD_D8" \
  --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
  --kotlin-java-home "$NEVERD_KOTLIN_JAVA_HOME" \
  --kotlin-lib "$RUNNER_TEMP/neverd-kotlin-2.0.21/kotlinc/lib" \
  --work-dir "$RUNNER_TEMP/android-long-constructor-prefix-sdk"

profile_group "Retain descriptor-only closed nominal superclass arguments" 21 1200
python scripts/test_mobile_android_nominal_super_constructor.py \
  --neverd build-mobile/bin/neverd --java-home "$JAVA_HOME" --d8 "$NEVERD_D8" \
  --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
  --work-dir "$RUNNER_TEMP/android-nominal-super-constructor"
python scripts/test_mobile_android_nominal_super_constructor.py --dependency-provider \
  --neverd build-mobile/bin/neverd --java-home "$JAVA_HOME" --d8 "$NEVERD_D8" \
  --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
  --work-dir "$RUNNER_TEMP/android-nominal-super-constructor-sdk"

profile_group "Retain complete closed constructor contracts across guarded this calls" 25 1200
python scripts/test_mobile_android_guarded_closed_delegation.py \
  --neverd build-mobile/bin/neverd --java-home "$JAVA_HOME" --d8 "$NEVERD_D8" \
  --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
  --work-dir "$RUNNER_TEMP/android-guarded-closed-delegation"
python scripts/test_mobile_android_guarded_closed_delegation.py --dependency-provider \
  --neverd build-mobile/bin/neverd --java-home "$JAVA_HOME" --d8 "$NEVERD_D8" \
  --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
  --work-dir "$RUNNER_TEMP/android-guarded-closed-delegation-sdk"

profile_group "Retain closed receiver and wide prefixes on suspend interface bodies" 21 1200
python scripts/test_mobile_android_scoped_suspend_forwarder.py \
  --neverd build-mobile/bin/neverd --java-home "$JAVA_HOME" --d8 "$NEVERD_D8" \
  --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
  --kotlin-java-home "$NEVERD_KOTLIN_JAVA_HOME" \
  --kotlin-lib "$RUNNER_TEMP/neverd-kotlin-2.0.21/kotlinc/lib" \
  --work-dir "$RUNNER_TEMP/android-scoped-suspend-forwarder"
python scripts/test_mobile_android_scoped_suspend_forwarder.py --dependency-provider \
  --neverd build-mobile/bin/neverd --java-home "$JAVA_HOME" --d8 "$NEVERD_D8" \
  --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
  --kotlin-java-home "$NEVERD_KOTLIN_JAVA_HOME" \
  --kotlin-lib "$RUNNER_TEMP/neverd-kotlin-2.0.21/kotlinc/lib" \
  --work-dir "$RUNNER_TEMP/android-scoped-suspend-forwarder-sdk"

profile_group "Separate fixed nominal constructor views from unrelated generic overloads" 25 1200
python scripts/test_mobile_android_closed_nominal_overload.py \
  --neverd build-mobile/bin/neverd --java-home "$JAVA_HOME" --d8 "$NEVERD_D8" \
  --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
  --kotlin-java-home "$NEVERD_KOTLIN_JAVA_HOME" \
  --kotlin-lib "$RUNNER_TEMP/neverd-kotlin-2.0.21/kotlinc/lib" \
  --work-dir "$RUNNER_TEMP/android-closed-nominal-overload"
python scripts/test_mobile_android_closed_nominal_overload.py --dependency-provider \
  --neverd build-mobile/bin/neverd --java-home "$JAVA_HOME" --d8 "$NEVERD_D8" \
  --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
  --kotlin-java-home "$NEVERD_KOTLIN_JAVA_HOME" \
  --kotlin-lib "$RUNNER_TEMP/neverd-kotlin-2.0.21/kotlinc/lib" \
  --work-dir "$RUNNER_TEMP/android-closed-nominal-overload-sdk"

profile_group "Preserve closed completion results on unary and seeded factories" 21 1200
python scripts/test_mobile_android_fixed_completion_factory.py \
  --neverd build-mobile/bin/neverd --java-home "$JAVA_HOME" --d8 "$NEVERD_D8" \
  --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
  --work-dir "$RUNNER_TEMP/android-fixed-completion-factory"
python scripts/test_mobile_android_fixed_completion_factory.py --dependency-provider \
  --neverd build-mobile/bin/neverd --java-home "$JAVA_HOME" --d8 "$NEVERD_D8" \
  --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
  --work-dir "$RUNNER_TEMP/android-fixed-completion-factory-sdk"

profile_group "Preserve method-owned completion formals on captured inner owners" 25 1200
python scripts/test_mobile_android_scoped_owner_completion.py \
  --neverd build-mobile/bin/neverd --java-home "$JAVA_HOME" --d8 "$NEVERD_D8" \
  --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
  --kotlin-java-home "$NEVERD_KOTLIN_JAVA_HOME" \
  --kotlin-lib "$RUNNER_TEMP/neverd-kotlin-2.0.21/kotlinc/lib" \
  --work-dir "$RUNNER_TEMP/android-scoped-owner-completion"
python scripts/test_mobile_android_scoped_owner_completion.py --dependency-provider \
  --neverd build-mobile/bin/neverd --java-home "$JAVA_HOME" --d8 "$NEVERD_D8" \
  --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
  --kotlin-java-home "$NEVERD_KOTLIN_JAVA_HOME" \
  --kotlin-lib "$RUNNER_TEMP/neverd-kotlin-2.0.21/kotlinc/lib" \
  --work-dir "$RUNNER_TEMP/android-scoped-owner-completion-sdk"

profile_group "Preserve fixed contracts across protected overload identifiers" 21 1200
python scripts/test_mobile_android_protected_overload_identifiers.py \
  --neverd build-mobile/bin/neverd --java-home "$JAVA_HOME" --d8 "$NEVERD_D8" \
  --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
  --work-dir "$RUNNER_TEMP/android-protected-overload-identifiers"
python scripts/test_mobile_android_protected_overload_identifiers.py --dependency-base \
  --neverd build-mobile/bin/neverd --java-home "$JAVA_HOME" --d8 "$NEVERD_D8" \
  --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
  --work-dir "$RUNNER_TEMP/android-protected-overload-identifiers-sdk"

profile_group "Preserve same-package provider reads before constructor delegation" 25 1200
python scripts/test_mobile_android_package_owner_constructor.py \
  --neverd build-mobile/bin/neverd --java-home "$JAVA_HOME" --d8 "$NEVERD_D8" \
  --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
  --work-dir "$RUNNER_TEMP/android-package-owner-constructor"
python scripts/test_mobile_android_package_owner_constructor.py --dependency-provider \
  --neverd build-mobile/bin/neverd --java-home "$JAVA_HOME" --d8 "$NEVERD_D8" \
  --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
  --work-dir "$RUNNER_TEMP/android-package-owner-constructor-sdk"

profile_group "Preserve generic terminal targets and their raw void bridges" 21 1200
python scripts/test_mobile_android_generic_terminal_void.py \
  --neverd build-mobile/bin/neverd --java-home "$JAVA_HOME" --d8 "$NEVERD_D8" \
  --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
  --kotlin-java-home "$NEVERD_KOTLIN_JAVA_HOME" \
  --kotlin-lib "$RUNNER_TEMP/neverd-kotlin-2.0.21/kotlinc/lib" \
  --work-dir "$RUNNER_TEMP/android-generic-terminal-void"
python scripts/test_mobile_android_generic_terminal_void.py --dependency-provider \
  --neverd build-mobile/bin/neverd --java-home "$JAVA_HOME" --d8 "$NEVERD_D8" \
  --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
  --kotlin-java-home "$NEVERD_KOTLIN_JAVA_HOME" \
  --kotlin-lib "$RUNNER_TEMP/neverd-kotlin-2.0.21/kotlinc/lib" \
  --work-dir "$RUNNER_TEMP/android-generic-terminal-void-sdk"

profile_group "Preserve ordered instance void calls during nested allocation" 21 1200
python scripts/test_mobile_android_allocation_statements.py --instance-marker \
  --neverd build-mobile/bin/neverd --d8 "$NEVERD_D8" --java-home "$JAVA_HOME" \
  --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
  --work-dir "$RUNNER_TEMP/android-allocation-void-receivers"
python scripts/test_mobile_android_allocation_statements.py --instance-marker --dependency-provider \
  --neverd build-mobile/bin/neverd --d8 "$NEVERD_D8" --java-home "$JAVA_HOME" \
  --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
  --work-dir "$RUNNER_TEMP/android-allocation-void-receivers-sdk"

profile_group "Preserve closed scalar defaults on generic compatibility children" 21 1200
python scripts/test_mobile_android_inherited_closed_default.py \
  --neverd build-mobile/bin/neverd --d8 "$NEVERD_D8" --java-home "$JAVA_HOME" \
  --kotlin-lib "$RUNNER_TEMP/neverd-kotlin-2.0.21/kotlinc/lib" \
  --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
  --work-dir "$RUNNER_TEMP/android-inherited-closed-default"
python scripts/test_mobile_android_inherited_closed_default.py --dependency-provider \
  --neverd build-mobile/bin/neverd --d8 "$NEVERD_D8" --java-home "$JAVA_HOME" \
  --kotlin-lib "$RUNNER_TEMP/neverd-kotlin-2.0.21/kotlinc/lib" \
  --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
  --work-dir "$RUNNER_TEMP/android-inherited-closed-default-sdk"

profile_group "Preserve method-owned seed results through inherited defaults" 21 1200
python scripts/test_mobile_android_inherited_seeded_default.py \
  --neverd build-mobile/bin/neverd --d8 "$NEVERD_D8" --java-home "$JAVA_HOME" \
  --kotlin-lib "$RUNNER_TEMP/neverd-kotlin-2.0.21/kotlinc/lib" \
  --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
  --work-dir "$RUNNER_TEMP/android-inherited-seeded-default"
python scripts/test_mobile_android_inherited_seeded_default.py --dependency-provider \
  --neverd build-mobile/bin/neverd --d8 "$NEVERD_D8" --java-home "$JAVA_HOME" \
  --kotlin-lib "$RUNNER_TEMP/neverd-kotlin-2.0.21/kotlinc/lib" \
  --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
  --work-dir "$RUNNER_TEMP/android-inherited-seeded-default-sdk"

profile_group "Preserve nominal interface results through compatibility defaults" 21 1200
python scripts/test_mobile_android_inherited_fluent_default.py \
  --neverd build-mobile/bin/neverd --d8 "$NEVERD_D8" --java-home "$JAVA_HOME" \
  --kotlin-lib "$RUNNER_TEMP/neverd-kotlin-2.0.21/kotlinc/lib" \
  --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
  --work-dir "$RUNNER_TEMP/android-inherited-fluent-default"
python scripts/test_mobile_android_inherited_fluent_default.py --dependency-provider \
  --neverd build-mobile/bin/neverd --d8 "$NEVERD_D8" --java-home "$JAVA_HOME" \
  --kotlin-lib "$RUNNER_TEMP/neverd-kotlin-2.0.21/kotlinc/lib" \
  --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
  --work-dir "$RUNNER_TEMP/android-inherited-fluent-default-sdk"

profile_group "Preserve independent key and value method formals" 21 1200
python scripts/test_mobile_android_key_value_carrier.py \
  --neverd build-mobile/bin/neverd --d8 "$NEVERD_D8" --java-home "$JAVA_HOME" \
  --kotlin-lib "$RUNNER_TEMP/neverd-kotlin-2.0.21/kotlinc/lib" \
  --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
  --work-dir "$RUNNER_TEMP/android-key-value-carrier"
python scripts/test_mobile_android_key_value_carrier.py --dependency-provider \
  --neverd build-mobile/bin/neverd --d8 "$NEVERD_D8" --java-home "$JAVA_HOME" \
  --kotlin-lib "$RUNNER_TEMP/neverd-kotlin-2.0.21/kotlinc/lib" \
  --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
  --work-dir "$RUNNER_TEMP/android-key-value-carrier-sdk"

profile_group "Preserve explicit cross-package protected receivers" 21 1200
python scripts/test_mobile_android_protected_static_receivers.py \
  --neverd build-mobile/bin/neverd --d8 "$NEVERD_D8" --java-home "$JAVA_HOME" \
  --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
  --work-dir "$RUNNER_TEMP/android-protected-static-receivers"
python scripts/test_mobile_android_protected_static_receivers.py --dependency-provider \
  --neverd build-mobile/bin/neverd --d8 "$NEVERD_D8" --java-home "$JAVA_HOME" \
  --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
  --work-dir "$RUNNER_TEMP/android-protected-static-receivers-sdk"

profile_group "Preserve independently owned concrete virtual roots" 21 1200
python scripts/test_mobile_android_protected_concrete_roots.py \
  --neverd build-mobile/bin/neverd --d8 "$NEVERD_D8" --java-home "$JAVA_HOME" \
  --kotlin-lib "$RUNNER_TEMP/neverd-kotlin-2.0.21/kotlinc/lib" \
  --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
  --work-dir "$RUNNER_TEMP/android-protected-concrete-roots"
python scripts/test_mobile_android_protected_concrete_roots.py --dependency-provider \
  --neverd build-mobile/bin/neverd --d8 "$NEVERD_D8" --java-home "$JAVA_HOME" \
  --kotlin-lib "$RUNNER_TEMP/neverd-kotlin-2.0.21/kotlinc/lib" \
  --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
  --work-dir "$RUNNER_TEMP/android-protected-concrete-roots-sdk"

profile_group "Preserve bounded carriers with nominal inputs" 21 1200
python scripts/test_mobile_android_bounded_carrier_nominal_input.py \
  --neverd build-mobile/bin/neverd --d8 "$NEVERD_D8" --java-home "$JAVA_HOME" \
  --kotlin-lib "$RUNNER_TEMP/neverd-kotlin-2.0.21/kotlinc/lib" \
  --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
  --work-dir "$RUNNER_TEMP/android-bounded-carrier-nominal-input"
python scripts/test_mobile_android_bounded_carrier_nominal_input.py --dependency-provider \
  --neverd build-mobile/bin/neverd --d8 "$NEVERD_D8" --java-home "$JAVA_HOME" \
  --kotlin-lib "$RUNNER_TEMP/neverd-kotlin-2.0.21/kotlinc/lib" \
  --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
  --work-dir "$RUNNER_TEMP/android-bounded-carrier-nominal-input-sdk"

profile_group "Verify public final generic names on open source owners" 21 1200
for variant in source sdk; do
  args=()
  if [[ "$variant" == sdk ]]; then args+=(--dependency-provider); fi
  python scripts/test_mobile_android_public_final_generic_names.py \
    --neverd build-mobile/bin/neverd --d8 "$NEVERD_D8" \
    --java-home "$NEVERD_JAVA21_HOME" \
    --kotlin-lib "$RUNNER_TEMP/neverd-kotlin-2.0.21/kotlinc/lib" \
    --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
    --work-dir "$RUNNER_TEMP/android-public-final-generic-names-$variant" "${args[@]}"
done

profile_group "Verify nested static factory result scopes" 21 1200
for variant in source sdk; do
  args=()
  if [[ "$variant" == sdk ]]; then args+=(--dependency-provider); fi
  python scripts/test_mobile_android_nested_static_carrier_result.py \
    --neverd build-mobile/bin/neverd --d8 "$NEVERD_D8" \
    --java-home "$NEVERD_JAVA21_HOME" \
    --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
    --work-dir "$RUNNER_TEMP/android-nested-static-carrier-result-$variant" "${args[@]}"
done

profile_group "Verify projected nested carrier and continuation scopes" 21 1200
for variant in source sdk; do
  args=()
  if [[ "$variant" == sdk ]]; then args+=(--dependency-provider); fi
  python scripts/test_mobile_android_projected_nested_session_callback.py \
    --neverd build-mobile/bin/neverd --d8 "$NEVERD_D8" \
    --java-home "$NEVERD_JAVA21_HOME" \
    --kotlin-lib "$RUNNER_TEMP/neverd-kotlin-2.0.21/kotlinc/lib" \
    --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
    --work-dir "$RUNNER_TEMP/android-projected-nested-session-callback-$variant" "${args[@]}"
done

profile_group "Verify private interface seed and result bindings" 21 1200
for variant in source sdk; do
  args=()
  if [[ "$variant" == sdk ]]; then args+=(--dependency-provider); fi
  python scripts/test_mobile_android_private_interface_seed_result.py \
    --neverd build-mobile/bin/neverd --d8 "$NEVERD_D8" \
    --java-home "$NEVERD_JAVA21_HOME" \
    --kotlin-lib "$RUNNER_TEMP/neverd-kotlin-2.0.21/kotlinc/lib" \
    --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
    --work-dir "$RUNNER_TEMP/android-private-interface-seed-result-$variant" "${args[@]}"
done

profile_group "Verify selector array and closed key bindings" 21 1200
for variant in source sdk; do
  args=()
  if [[ "$variant" == sdk ]]; then args+=(--dependency-provider); fi
  python scripts/test_mobile_android_selector_array_carrier.py \
    --neverd build-mobile/bin/neverd --d8 "$NEVERD_D8" \
    --java-home "$NEVERD_JAVA21_HOME" \
    --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
    --work-dir "$RUNNER_TEMP/android-selector-array-carrier-$variant" "${args[@]}"
done

profile_group "Verify closed recursive constructor argument bounds" 25 1200
for variant in source sdk; do
  args=()
  if [[ "$variant" == sdk ]]; then args+=(--dependency-provider); fi
  python scripts/test_mobile_android_closed_range_constructor.py \
    --neverd build-mobile/bin/neverd --d8 "$NEVERD_D8" \
    --java-home "$NEVERD_JAVA25_HOME" \
    --kotlin-java-home "$NEVERD_KOTLIN_JAVA_HOME" \
    --kotlin-lib "$RUNNER_TEMP/neverd-kotlin-2.0.21/kotlinc/lib" \
    --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
    --work-dir "$RUNNER_TEMP/android-closed-range-constructor-$variant" "${args[@]}"
done

profile_group "Verify binary merge factory callback scopes" 21 1200
for variant in source sdk; do
  args=()
  if [[ "$variant" == sdk ]]; then args+=(--dependency-provider); fi
  python scripts/test_mobile_android_binary_merge_factory.py \
    --neverd build-mobile/bin/neverd --d8 "$NEVERD_D8" \
    --java-home "$NEVERD_JAVA21_HOME" \
    --kotlin-lib "$RUNNER_TEMP/neverd-kotlin-2.0.21/kotlinc/lib" \
    --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
    --work-dir "$RUNNER_TEMP/android-binary-merge-factory-$variant" "${args[@]}"
done

profile_group "Verify structured middle final assignments" 21 1200
for variant in source sdk; do
  args=()
  if [[ "$variant" == sdk ]]; then args+=(--dependency-provider); fi
  python scripts/test_mobile_android_final_middle_regions.py \
    --neverd build-mobile/bin/neverd --d8 "$NEVERD_D8" \
    --java-home "$NEVERD_JAVA21_HOME" \
    --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
    --work-dir "$RUNNER_TEMP/android-final-middle-regions-$variant" "${args[@]}"
done

profile_group "Verify exact constructor allocation padding" 25 1200
for variant in before interleaved; do
  args=()
  if [[ "$variant" == interleaved ]]; then args+=(--spread-padding); fi
  python scripts/test_mobile_android_padded_constructor_allocation.py \
    --neverd build-mobile/bin/neverd --d8 "$NEVERD_D8" \
    --java-home "$NEVERD_JAVA25_HOME" \
    --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
    --work-dir "$RUNNER_TEMP/android-padded-constructor-allocation-$variant" "${args[@]}"
done

profile_group "Verify producer consumer invariant factories" 21 1200
for variant in source sdk; do
  args=()
  if [[ "$variant" == sdk ]]; then args+=(--dependency-provider); fi
  python scripts/test_mobile_android_producer_consumer_factory.py \
    --neverd build-mobile/bin/neverd --d8 "$NEVERD_D8" \
    --java-home "$NEVERD_JAVA21_HOME" \
    --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
    --work-dir "$RUNNER_TEMP/android-producer-consumer-factory-$variant" "${args[@]}"
done

profile_group "Verify discarded reference result adapters" 21 1200
for variant in source sdk; do
  args=()
  if [[ "$variant" == sdk ]]; then args+=(--dependency-provider); fi
  python scripts/test_mobile_android_discarded_reference_result.py \
    --neverd build-mobile/bin/neverd --d8 "$NEVERD_D8" \
    --java-home "$NEVERD_JAVA21_HOME" \
    --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
    --work-dir "$RUNNER_TEMP/android-discarded-reference-result-$variant" "${args[@]}"
done

profile_group "Verify private enclosing unary carrier factories" 21 1200
for variant in source sdk; do
  args=()
  if [[ "$variant" == sdk ]]; then args+=(--dependency-inputs); fi
  python scripts/test_mobile_android_nested_unary_carrier.py \
    --neverd build-mobile/bin/neverd --d8 "$NEVERD_D8" \
    --java-home "$NEVERD_JAVA21_HOME" \
    --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
    --work-dir "$RUNNER_TEMP/android-nested-unary-carrier-$variant" "${args[@]}"
done

profile_group "Verify nonpublic object scalar mask constructors" 25 1200
for variant in source sdk; do
  args=()
  if [[ "$variant" == sdk ]]; then args+=(--dependency-provider); fi
  python scripts/test_mobile_android_object_scalar_mask_constructor.py \
    --neverd build-mobile/bin/neverd --d8 "$NEVERD_D8" \
    --java-home "$NEVERD_JAVA25_HOME" \
    --kotlin-lib "$RUNNER_TEMP/neverd-kotlin-2.0.21/kotlinc/lib" \
    --kotlin-java-home "$NEVERD_KOTLIN_JAVA_HOME" \
    --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
    --work-dir "$RUNNER_TEMP/android-object-scalar-mask-constructor-$variant" "${args[@]}"
done

profile_group "Verify class and method callback carrier scopes" 21 1200
for variant in source sdk; do
  args=()
  if [[ "$variant" == sdk ]]; then args+=(--dependency-provider); fi
  python scripts/test_mobile_android_scoped_callback_carrier.py \
    --neverd build-mobile/bin/neverd --d8 "$NEVERD_D8" \
    --java-home "$NEVERD_JAVA21_HOME" \
    --kotlin-java-home "$NEVERD_KOTLIN_JAVA_HOME" \
    --kotlin-lib "$RUNNER_TEMP/neverd-kotlin-2.0.21/kotlinc/lib" \
    --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
    --work-dir "$RUNNER_TEMP/android-scoped-callback-carrier-$variant" "${args[@]}"
done

profile_group "Verify typed constructor string context forwarding" 21 1200
for variant in source sdk; do
  args=()
  if [[ "$variant" == sdk ]]; then args+=(--dependency-provider); fi
  python scripts/test_mobile_android_typed_string_delegation.py \
    --neverd build-mobile/bin/neverd --d8 "$NEVERD_D8" \
    --java-home "$NEVERD_JAVA21_HOME" \
    --kotlin-java-home "$NEVERD_KOTLIN_JAVA_HOME" \
    --kotlin-lib "$RUNNER_TEMP/neverd-kotlin-2.0.21/kotlinc/lib" \
    --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
    --work-dir "$RUNNER_TEMP/android-typed-string-delegation-$variant" "${args[@]}"
done

profile_group "Verify covariant nested carrier transfers" 21 1200
for variant in source sdk; do
  args=()
  if [[ "$variant" == sdk ]]; then args+=(--dependency-provider); fi
  python scripts/test_mobile_android_nested_carrier_transfer.py \
    --neverd build-mobile/bin/neverd --d8 "$NEVERD_D8" \
    --java-home "$NEVERD_JAVA21_HOME" \
    --kotlin-java-home "$NEVERD_KOTLIN_JAVA_HOME" \
    --kotlin-lib "$RUNNER_TEMP/neverd-kotlin-2.0.21/kotlinc/lib" \
    --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
    --work-dir "$RUNNER_TEMP/android-nested-carrier-transfer-$variant" "${args[@]}"
done

profile_group "Verify owned private contextual factories" 21 1200
for variant in source sdk; do
  args=()
  if [[ "$variant" == sdk ]]; then args+=(--dependency-provider); fi
  python scripts/test_mobile_android_private_context_factory.py \
    --neverd build-mobile/bin/neverd --d8 "$NEVERD_D8" \
    --java-home "$NEVERD_JAVA21_HOME" \
    --kotlin-java-home "$NEVERD_KOTLIN_JAVA_HOME" \
    --kotlin-lib "$RUNNER_TEMP/neverd-kotlin-2.0.21/kotlinc/lib" \
    --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
    --work-dir "$RUNNER_TEMP/android-private-context-factory-$variant" "${args[@]}"
done

profile_group "Verify dependent interface consumer bounds" 21 1200
for variant in source sdk; do
  args=()
  if [[ "$variant" == sdk ]]; then args+=(--dependency-provider); fi
  python scripts/test_mobile_android_dependent_interface_consumer.py \
    --neverd build-mobile/bin/neverd --d8 "$NEVERD_D8" \
    --java-home "$NEVERD_JAVA21_HOME" \
    --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
    --work-dir "$RUNNER_TEMP/android-dependent-interface-consumer-$variant" "${args[@]}"
done

profile_group "Verify conditional allocation receiver aliases" 21 1200
for variant in source sdk; do
  args=()
  if [[ "$variant" == sdk ]]; then args+=(--dependency-provider); fi
  python scripts/test_mobile_android_conditional_receiver_alias.py \
    --neverd build-mobile/bin/neverd --d8 "$NEVERD_D8" \
    --java-home "$NEVERD_JAVA21_HOME" \
    --kotlin-java-home "$NEVERD_KOTLIN_JAVA_HOME" \
    --kotlin-lib "$RUNNER_TEMP/neverd-kotlin-2.0.21/kotlinc/lib" \
    --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
    --work-dir "$RUNNER_TEMP/android-conditional-receiver-alias-$variant" "${args[@]}"
done

profile_group "Verify long conditional constructor arguments" 21 1200
for variant in source sdk; do
  args=()
  if [[ "$variant" == sdk ]]; then args+=(--dependency-provider); fi
  python scripts/test_mobile_android_long_conditional_receiver.py \
    --neverd build-mobile/bin/neverd --d8 "$NEVERD_D8" \
    --java-home "$NEVERD_JAVA21_HOME" \
    --kotlin-java-home "$NEVERD_KOTLIN_JAVA_HOME" \
    --kotlin-lib "$RUNNER_TEMP/neverd-kotlin-2.0.21/kotlinc/lib" \
    --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
    --work-dir "$RUNNER_TEMP/android-long-conditional-receiver-$variant" "${args[@]}"
done

profile_group "Verify field-read receiver lifetimes" 21 1200
for variant in source sdk; do
  args=()
  if [[ "$variant" == sdk ]]; then args+=(--dependency-provider); fi
  python scripts/test_mobile_android_field_receiver_reuse.py \
    --neverd build-mobile/bin/neverd --d8 "$NEVERD_D8" \
    --java-home "$NEVERD_JAVA21_HOME" \
    --kotlin-java-home "$NEVERD_KOTLIN_JAVA_HOME" \
    --kotlin-lib "$RUNNER_TEMP/neverd-kotlin-2.0.21/kotlinc/lib" \
    --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
    --work-dir "$RUNNER_TEMP/android-field-receiver-reuse-$variant" "${args[@]}"
done

profile_group "Verify conditional constructor argument regions" 25 1200
for variant in source sdk; do
  args=()
  if [[ "$variant" == sdk ]]; then args+=(--dependency-provider); fi
  python scripts/test_mobile_android_constructor_argument_regions.py \
    --neverd build-mobile/bin/neverd --d8 "$RUNNER_TEMP/neverd-java25-d8/lib/d8.jar" \
    --java-home "$JAVA_HOME" --compiler-java-home "$NEVERD_JAVA21_HOME" \
    --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
    --work-dir "$RUNNER_TEMP/android-constructor-argument-regions-$variant" "${args[@]}"
done

profile_group "Verify ordered protected final stages" 25 1200
for variant in source sdk; do
  args=()
  if [[ "$variant" == sdk ]]; then args+=(--dependency-provider); fi
  python scripts/test_mobile_android_ordered_final_stages.py \
    --neverd build-mobile/bin/neverd --d8 "$RUNNER_TEMP/neverd-java25-d8/lib/d8.jar" \
    --java-home "$JAVA_HOME" --compiler-java-home "$NEVERD_JAVA21_HOME" \
    --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
    --work-dir "$RUNNER_TEMP/android-ordered-final-stages-$variant" "${args[@]}"
done

profile_group "Verify fixed nominal array results" 25 1200
for variant in source sdk source-shadow-owner sdk-shadow-owner source-projected sdk-projected source-shadow-owner-projected sdk-shadow-owner-projected; do
  args=()
  if [[ "$variant" == sdk* ]]; then args+=(--dependency-provider); fi
  if [[ "$variant" == *shadow-owner* ]]; then args+=(--shadow-owner); fi
  if [[ "$variant" == *projected ]]; then
    args+=(--projected-result-header --kotlin-java-home "$NEVERD_KOTLIN_JAVA_HOME"
      --kotlin-lib "$RUNNER_TEMP/neverd-kotlin-2.0.21/kotlinc/lib")
  fi
  python scripts/test_mobile_android_array_nominal_result.py \
    --neverd build-mobile/bin/neverd --d8 "$RUNNER_TEMP/neverd-java25-d8/lib/d8.jar" \
    --java-home "$JAVA_HOME" --compiler-java-home "$NEVERD_JAVA21_HOME" \
    --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
    --work-dir "$RUNNER_TEMP/android-array-nominal-result-$variant" "${args[@]}"
done

profile_group "Verify arrays in constructor allocation arguments" 25 1200
for variant in source sdk; do
  args=()
  if [[ "$variant" == sdk ]]; then args+=(--dependency-provider); fi
  python scripts/test_mobile_android_constructor_allocation_arrays.py \
    --neverd build-mobile/bin/neverd --d8 "$RUNNER_TEMP/neverd-java25-d8/lib/d8.jar" \
    --java-home "$JAVA_HOME" --compiler-java-home "$NEVERD_JAVA21_HOME" \
    --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
    --work-dir "$RUNNER_TEMP/android-constructor-allocation-arrays-$variant" "${args[@]}"
done

profile_group "Verify typed monitor continuations" 25 1200
for variant in source sdk; do
  args=()
  if [[ "$variant" == sdk ]]; then args+=(--dependency-provider); fi
  python scripts/test_mobile_android_monitor_typed_continuation.py \
    --neverd build-mobile/bin/neverd --d8 "$RUNNER_TEMP/neverd-java25-d8/lib/d8.jar" \
    --java-home "$JAVA_HOME" --compiler-java-home "$NEVERD_JAVA21_HOME" \
    --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
    --work-dir "$RUNNER_TEMP/android-monitor-typed-continuation-$variant" "${args[@]}"
done

profile_group "Verify array factory constructor defaults" 25 1200
for variant in source sdk; do
  args=()
  if [[ "$variant" == sdk ]]; then args+=(--dependency-provider); fi
  python scripts/test_mobile_android_masked_constructor_array.py \
    --neverd build-mobile/bin/neverd --d8 "$RUNNER_TEMP/neverd-java25-d8/lib/d8.jar" \
    --java-home "$JAVA_HOME" --compiler-java-home "$NEVERD_JAVA21_HOME" \
    --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
    --work-dir "$RUNNER_TEMP/android-masked-constructor-array-$variant" "${args[@]}"
done

profile_group "Verify array stores before constructor delegation" 25 1200
for variant in source sdk; do
  args=()
  if [[ "$variant" == sdk ]]; then args+=(--dependency-provider); fi
  python scripts/test_mobile_android_constructor_array_stores.py \
    --neverd build-mobile/bin/neverd --d8 "$RUNNER_TEMP/neverd-java25-d8/lib/d8.jar" \
    --java-home "$JAVA_HOME" --compiler-java-home "$NEVERD_JAVA21_HOME" \
    --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
    --work-dir "$RUNNER_TEMP/android-constructor-array-stores-$variant" "${args[@]}"
done

profile_group "Verify paired constructor conditions" 25 1200
for variant in source sdk; do
  args=()
  if [[ "$variant" == sdk ]]; then args+=(--dependency-provider); fi
  python scripts/test_mobile_android_paired_constructor_conditions.py \
    --neverd build-mobile/bin/neverd --d8 "$RUNNER_TEMP/neverd-java25-d8/lib/d8.jar" \
    --java-home "$JAVA_HOME" \
    --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
    --work-dir "$RUNNER_TEMP/android-paired-constructor-conditions-$variant" "${args[@]}"
done

profile_group "Verify closed constructors beside varargs overloads" 25 1200
for variant in source sdk; do
  args=()
  if [[ "$variant" == sdk ]]; then args+=(--dependency-provider); fi
  python scripts/test_mobile_android_closed_varargs_overloads.py \
    --neverd build-mobile/bin/neverd --d8 "$RUNNER_TEMP/neverd-java25-d8/lib/d8.jar" \
    --java-home "$JAVA_HOME" --compiler-java-home "$NEVERD_JAVA21_HOME" \
    --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
    --work-dir "$RUNNER_TEMP/android-closed-varargs-overloads-$variant" "${args[@]}"
done

profile_group "Verify package constructor default scopes" 25 1200
for variant in source sdk; do
  args=()
  if [[ "$variant" == sdk ]]; then args+=(--dependency-provider); fi
  python scripts/test_mobile_android_package_constructor_defaults.py \
    --neverd build-mobile/bin/neverd --d8 "$RUNNER_TEMP/neverd-java25-d8/lib/d8.jar" \
    --java-home "$JAVA_HOME" --compiler-java-home "$NEVERD_JAVA21_HOME" \
    --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
    --work-dir "$RUNNER_TEMP/android-package-constructor-defaults-$variant" "${args[@]}"
done

profile_group "Verify static final cleanup alignment" 21 1200
for variant in source sdk; do
  args=()
  if [[ "$variant" == sdk ]]; then args+=(--dependency-provider); fi
  python scripts/test_mobile_android_static_final_tail.py \
    --neverd build-mobile/bin/neverd --d8 "$NEVERD_D8" \
    --java-home "$JAVA_HOME" --compiler-java-home "$NEVERD_JAVA21_HOME" \
    --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
    --work-dir "$RUNNER_TEMP/android-static-final-tail-$variant" "${args[@]}"
done

profile_group "Verify monitors between static final assignments" 21 1200
for variant in source sdk; do
  args=()
  if [[ "$variant" == sdk ]]; then args+=(--dependency-provider); fi
  python scripts/test_mobile_android_static_final_monitor.py \
    --neverd build-mobile/bin/neverd --d8 "$NEVERD_D8" \
    --java-home "$JAVA_HOME" --compiler-java-home "$NEVERD_JAVA21_HOME" \
    --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
    --work-dir "$RUNNER_TEMP/android-static-final-monitor-$variant" "${args[@]}"
done

profile_group "Verify bounded producers with independent fixed results" 21 1200
for variant in source sdk; do
  args=()
  if [[ "$variant" == sdk ]]; then args+=(--dependency-provider); fi
  python scripts/test_mobile_android_bounded_factory_input.py \
    --neverd build-mobile/bin/neverd --d8 "$NEVERD_D8" \
    --java-home "$JAVA_HOME" --compiler-java-home "$NEVERD_JAVA21_HOME" \
    --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
    --work-dir "$RUNNER_TEMP/android-bounded-factory-input-$variant" "${args[@]}"
done

profile_group "Verify unused nominal method bounds" 21 1200
for variant in source sdk; do
  args=()
  if [[ "$variant" == sdk ]]; then args+=(--dependency-provider); fi
  python scripts/test_mobile_android_bounded_factory_input.py --unused-bound \
    --neverd build-mobile/bin/neverd --d8 "$NEVERD_D8" \
    --java-home "$JAVA_HOME" --compiler-java-home "$NEVERD_JAVA21_HOME" \
    --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
    --work-dir "$RUNNER_TEMP/android-unused-nominal-formal-$variant" "${args[@]}"
done

profile_group "Verify result-only nominal factory bounds" 21 1200
for variant in source sdk; do
  args=()
  if [[ "$variant" == sdk ]]; then args+=(--dependency-provider); fi
  python scripts/test_mobile_android_nominal_result_only_factory.py \
    --neverd build-mobile/bin/neverd --d8 "$NEVERD_D8" \
    --java-home "$JAVA_HOME" --compiler-java-home "$NEVERD_JAVA21_HOME" \
    --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
    --work-dir "$RUNNER_TEMP/android-nominal-result-only-factory-$variant" "${args[@]}"
done

profile_group "Verify bounded owners with fixed constructor headers" 25 1200
for variant in source sdk; do
  args=()
  if [[ "$variant" == sdk ]]; then args+=(--dependency-provider); fi
  python scripts/test_mobile_android_fixed_header_mask_owner.py \
    --neverd build-mobile/bin/neverd --d8 "$NEVERD_D8" \
    --java-home "$JAVA_HOME" --kotlin-java-home "$NEVERD_JAVA21_HOME" \
    --kotlin-lib "$RUNNER_TEMP/neverd-kotlin-2.0.21/kotlinc/lib" \
    --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
    --work-dir "$RUNNER_TEMP/android-fixed-header-mask-owner-$variant" "${args[@]}"
done

profile_group "Verify guarded constructor zero provenance" 25 1200
for variant in source sdk; do
  args=()
  if [[ "$variant" == sdk ]]; then args+=(--dependency-provider); fi
  python scripts/test_mobile_android_guarded_prefix_zero.py \
    --neverd build-mobile/bin/neverd --d8 "$NEVERD_D8" \
    --java-home "$JAVA_HOME" --kotlin-java-home "$NEVERD_JAVA21_HOME" \
    --kotlin-lib "$RUNNER_TEMP/neverd-kotlin-2.0.21/kotlinc/lib" \
    --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
    --work-dir "$RUNNER_TEMP/android-guarded-prefix-zero-$variant" "${args[@]}"
done

profile_group "Verify independent static pair factory formals" 21 1200
for variant in source sdk; do
  args=()
  if [[ "$variant" == sdk ]]; then args+=(--dependency-provider); fi
  python scripts/test_mobile_android_generic_pair_factory.py \
    --neverd build-mobile/bin/neverd --d8 "$NEVERD_D8" \
    --java-home "$JAVA_HOME" --compiler-java-home "$NEVERD_JAVA21_HOME" \
    --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
    --work-dir "$RUNNER_TEMP/android-generic-pair-factory-$variant" "${args[@]}"
done

profile_group "Verify package carriers with closed checked failures" 21 1200
for variant in source sdk; do
  args=()
  if [[ "$variant" == sdk ]]; then args+=(--dependency-provider); fi
  python scripts/test_mobile_android_package_checked_carrier.py \
    --neverd build-mobile/bin/neverd --d8 "$NEVERD_D8" \
    --java-home "$JAVA_HOME" --compiler-java-home "$NEVERD_JAVA21_HOME" \
    --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
    --work-dir "$RUNNER_TEMP/android-package-checked-carrier-$variant" "${args[@]}"
done

profile_group "Verify independent package identity method formals" 21 1200
for variant in source sdk; do
  args=()
  if [[ "$variant" == sdk ]]; then args+=(--dependency-provider); fi
  python scripts/test_mobile_android_package_formal_identity.py \
    --neverd build-mobile/bin/neverd --d8 "$NEVERD_D8" \
    --java-home "$JAVA_HOME" --compiler-java-home "$NEVERD_JAVA21_HOME" \
    --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
    --work-dir "$RUNNER_TEMP/android-package-formal-identity-$variant" "${args[@]}"
done

profile_group "Verify total monitor leaf lifetimes" 21 1200
for variant in source sdk; do
  args=()
  if [[ "$variant" == sdk ]]; then args+=(--dependency-provider); fi
  python scripts/test_mobile_android_total_monitor_leaf.py \
    --neverd build-mobile/bin/neverd --d8 "$NEVERD_D8" \
    --java-home "$JAVA_HOME" --compiler-java-home "$NEVERD_JAVA21_HOME" \
    --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
    --work-dir "$RUNNER_TEMP/android-total-monitor-leaf-$variant" "${args[@]}"
done

profile_group "Verify independent direct pair factory seeds" 21 1200
for variant in source sdk; do
  args=()
  if [[ "$variant" == sdk ]]; then args+=(--dependency-provider); fi
  python scripts/test_mobile_android_seeded_pair_factory.py \
    --neverd build-mobile/bin/neverd --d8 "$NEVERD_D8" \
    --java-home "$JAVA_HOME" --compiler-java-home "$NEVERD_JAVA21_HOME" \
    --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
    --work-dir "$RUNNER_TEMP/android-seeded-pair-factory-$variant" "${args[@]}"
done

profile_group "Verify bounded instance array consumers" 21 1200
for variant in source sdk; do
  args=()
  if [[ "$variant" == sdk ]]; then args+=(--dependency-provider); fi
  python scripts/test_mobile_android_bound_array_consumer.py \
    --neverd build-mobile/bin/neverd --d8 "$NEVERD_D8" \
    --java-home "$JAVA_HOME" --compiler-java-home "$NEVERD_JAVA21_HOME" \
    --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
    --work-dir "$RUNNER_TEMP/android-bound-array-consumer-$variant" "${args[@]}"
done

profile_group "Verify ordered typed handlers under original monitors" 21 1200
for variant in source sdk; do
  args=()
  if [[ "$variant" == sdk ]]; then args+=(--dependency-provider); fi
  python scripts/test_mobile_android_typed_monitor_handlers.py \
    --neverd build-mobile/bin/neverd --d8 "$NEVERD_D8" \
    --java-home "$JAVA_HOME" --compiler-java-home "$NEVERD_JAVA21_HOME" \
    --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
    --work-dir "$RUNNER_TEMP/android-typed-monitor-handlers-$variant" "${args[@]}"
done

profile_group "Verify closed package constructor arguments" 25 1200
for variant in source sdk; do
  args=()
  if [[ "$variant" == sdk ]]; then args+=(--dependency-provider); fi
  python scripts/test_mobile_android_constructor_overload_argument.py \
    --neverd build-mobile/bin/neverd --d8 "$NEVERD_D8" \
    --java-home "$JAVA_HOME" --compiler-java-home "$NEVERD_JAVA21_HOME" \
    --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
    --work-dir "$RUNNER_TEMP/android-constructor-overload-argument-$variant" "${args[@]}"
done

profile_group "Verify long initialized constructor continuations" 25 1200
for variant in source sdk; do
  args=()
  if [[ "$variant" == sdk ]]; then args+=(--dependency-provider); fi
  python scripts/test_mobile_android_long_constructor_continuation.py \
    --neverd build-mobile/bin/neverd --d8 "$NEVERD_D8" \
    --java-home "$JAVA_HOME" --compiler-java-home "$NEVERD_JAVA21_HOME" \
    --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
    --work-dir "$RUNNER_TEMP/android-long-constructor-continuation-$variant" "${args[@]}"
done

profile_group "Verify fixed collection and formal array boolean calls" 21 1200
for variant in source sdk; do
  args=()
  if [[ "$variant" == sdk ]]; then args+=(--dependency-provider); fi
  python scripts/test_mobile_android_collection_array_boolean.py \
    --neverd build-mobile/bin/neverd --d8 "$NEVERD_D8" \
    --java-home "$JAVA_HOME" --compiler-java-home "$NEVERD_JAVA21_HOME" \
    --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
    --work-dir "$RUNNER_TEMP/android-collection-array-boolean-$variant" "${args[@]}"
done

profile_group "Verify final middle stages with callee abrupt exits" 21 1200
for variant in source sdk; do
  args=()
  if [[ "$variant" == sdk ]]; then args+=(--dependency-provider); fi
  python scripts/test_mobile_android_final_middle_regions.py --callee-abrupt-exit \
    --neverd build-mobile/bin/neverd --d8 "$NEVERD_D8" \
    --java-home "$JAVA_HOME" --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
    --work-dir "$RUNNER_TEMP/android-final-middle-callee-abrupt-$variant" "${args[@]}"
done

profile_group "Verify independent array input and type-token result components" 21 1200
for variant in source sdk; do
  args=()
  if [[ "$variant" == sdk ]]; then args+=(--dependency-provider); fi
  python scripts/test_mobile_android_array_type_token.py \
    --neverd build-mobile/bin/neverd --d8 "$NEVERD_D8" \
    --java-home "$JAVA_HOME" --compiler-java-home "$NEVERD_JAVA21_HOME" \
    --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
    --work-dir "$RUNNER_TEMP/android-array-type-token-$variant" "${args[@]}"
done

profile_group "Verify nominal contexts beside invariant array tokens" 21 1200
for variant in source sdk package; do
  args=()
  if [[ "$variant" == sdk ]]; then args+=(--dependency-provider); fi
  if [[ "$variant" == package ]]; then args+=(--package-provider); fi
  python scripts/test_mobile_android_context_array_token.py \
    --neverd build-mobile/bin/neverd --d8 "$NEVERD_D8" \
    --java-home "$JAVA_HOME" --compiler-java-home "$NEVERD_JAVA21_HOME" \
    --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
    --work-dir "$RUNNER_TEMP/android-context-array-token-$variant" "${args[@]}"
done

profile_group "Verify package carrier results with producer tokens" 21 1200
for variant in source sdk package; do
  args=()
  if [[ "$variant" == sdk ]]; then args+=(--dependency-provider); fi
  if [[ "$variant" == package ]]; then args+=(--package-provider); fi
  python scripts/test_mobile_android_context_carrier_token.py \
    --neverd build-mobile/bin/neverd --d8 "$NEVERD_D8" \
    --java-home "$JAVA_HOME" --compiler-java-home "$NEVERD_JAVA21_HOME" \
    --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
    --work-dir "$RUNNER_TEMP/android-context-carrier-token-$variant" "${args[@]}"
done

profile_group "Verify bounded package token results with nominal contexts" 21 1200
for variant in source sdk package; do
  args=()
  if [[ "$variant" == sdk ]]; then args+=(--dependency-provider); fi
  if [[ "$variant" == package ]]; then args+=(--package-provider); fi
  python scripts/test_mobile_android_bound_context_token.py \
    --neverd build-mobile/bin/neverd --d8 "$NEVERD_D8" \
    --java-home "$JAVA_HOME" --compiler-java-home "$NEVERD_JAVA21_HOME" \
    --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
    --work-dir "$RUNNER_TEMP/android-bound-context-token-$variant" "${args[@]}"
done

profile_group "Verify invariant virtual inputs and independent nominal results" 21 1200
for variant in source sdk; do
  args=()
  if [[ "$variant" == sdk ]]; then args+=(--dependency-provider); fi
  python scripts/test_mobile_android_fluent_token_consumer.py \
    --neverd build-mobile/bin/neverd --d8 "$NEVERD_D8" \
    --java-home "$JAVA_HOME" --compiler-java-home "$NEVERD_JAVA21_HOME" \
    --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
    --work-dir "$RUNNER_TEMP/android-fluent-token-consumer-$variant" "${args[@]}"
done

profile_group "Verify erased public visibility bridges over generic parents" 21 1200
for variant in source sdk; do
  args=()
  if [[ "$variant" == sdk ]]; then args+=(--dependency-provider); fi
  python scripts/test_mobile_android_visibility_generic_bridge.py \
    --neverd build-mobile/bin/neverd --d8 "$NEVERD_D8" \
    --java-home "$JAVA_HOME" --compiler-java-home "$NEVERD_JAVA21_HOME" \
    --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
    --work-dir "$RUNNER_TEMP/android-visibility-generic-bridge-$variant" "${args[@]}"
done

profile_group "Verify array-bound element selectors and private member access" 21 1200
for variant in source sdk private; do
  args=()
  if [[ "$variant" == sdk ]]; then args+=(--dependency-provider); fi
  if [[ "$variant" == private ]]; then args+=(--private-provider); fi
  python scripts/test_mobile_android_array_element_selector.py \
    --neverd build-mobile/bin/neverd --d8 "$NEVERD_D8" \
    --java-home "$JAVA_HOME" --compiler-java-home "$NEVERD_JAVA21_HOME" \
    --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
    --work-dir "$RUNNER_TEMP/android-array-element-selector-$variant" "${args[@]}"
done

profile_group "Verify empty package metadata compiler identities" 21 1200
for variant in source sdk; do
  args=()
  if [[ "$variant" == sdk ]]; then args+=(--dependency-provider); fi
  python scripts/test_mobile_android_empty_package_metadata.py \
    --neverd build-mobile/bin/neverd --d8 "$NEVERD_D8" \
    --java-home "$JAVA_HOME" --compiler-java-home "$NEVERD_JAVA21_HOME" \
    --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
    --work-dir "$RUNNER_TEMP/android-empty-package-metadata-$variant" "${args[@]}"
done

profile_group "Verify captured exception copies and backward monitor cleanup" 21 1200
for variant in source sdk; do
  args=()
  if [[ "$variant" == sdk ]]; then args+=(--dependency-provider); fi
  python scripts/test_mobile_android_monitor_copy_cleanup.py \
    --neverd build-mobile/bin/neverd --d8 "$NEVERD_D8" \
    --java-home "$JAVA_HOME" --captured-exception-copies \
    --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
    --work-dir "$RUNNER_TEMP/android-monitor-captured-cleanup-$variant" "${args[@]}"
done

profile_group "Verify ordered final fields across constructor loop branches" 21 1200
for variant in source sdk; do
  args=()
  if [[ "$variant" == sdk ]]; then args+=(--dependency-provider); fi
  python scripts/test_mobile_android_looping_final_branches.py \
    --neverd build-mobile/bin/neverd --d8 "$NEVERD_D8" \
    --java-home "$JAVA_HOME" --compiler-java-home "$NEVERD_JAVA21_HOME" \
    --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
    --work-dir "$RUNNER_TEMP/android-looping-final-branches-$variant" "${args[@]}"
done

profile_group "Verify paired type tokens with independent context" 21 1200
for variant in source sdk package; do
  args=()
  if [[ "$variant" == sdk ]]; then args+=(--dependency-provider); fi
  if [[ "$variant" == package ]]; then args+=(--package-provider); fi
  python scripts/test_mobile_android_paired_context_tokens.py \
    --neverd build-mobile/bin/neverd --d8 "$NEVERD_D8" \
    --java-home "$JAVA_HOME" --compiler-java-home "$NEVERD_JAVA21_HOME" \
    --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
    --work-dir "$RUNNER_TEMP/android-paired-context-tokens-$variant" "${args[@]}"
done

profile_group "Verify paired type tokens with a binary consumer" 21 1200
for variant in source sdk package; do
  args=()
  if [[ "$variant" == sdk ]]; then args+=(--dependency-provider); fi
  if [[ "$variant" == package ]]; then args+=(--package-provider); fi
  python scripts/test_mobile_android_paired_context_sinks.py \
    --neverd build-mobile/bin/neverd --d8 "$NEVERD_D8" \
    --java-home "$JAVA_HOME" --compiler-java-home "$NEVERD_JAVA21_HOME" \
    --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
    --work-dir "$RUNNER_TEMP/android-paired-context-sinks-$variant" "${args[@]}"
done

profile_group "Verify invariant carrier inputs with type tokens" 21 1200
for variant in source sdk package; do
  args=()
  if [[ "$variant" == sdk ]]; then args+=(--dependency-provider); fi
  if [[ "$variant" == package ]]; then args+=(--package-provider); fi
  python scripts/test_mobile_android_context_token_transfer.py \
    --neverd build-mobile/bin/neverd --d8 "$NEVERD_D8" \
    --java-home "$JAVA_HOME" --compiler-java-home "$NEVERD_JAVA21_HOME" \
    --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
    --work-dir "$RUNNER_TEMP/android-context-token-transfer-$variant" "${args[@]}"
done

profile_group "Verify contextual carriers with nominal bounds" 21 1200
for variant in source sdk package; do
  args=()
  if [[ "$variant" == sdk ]]; then args+=(--dependency-provider); fi
  if [[ "$variant" == package ]]; then args+=(--package-provider); fi
  python scripts/test_mobile_android_bound_context_transfer.py \
    --neverd build-mobile/bin/neverd --d8 "$NEVERD_D8" \
    --java-home "$JAVA_HOME" --compiler-java-home "$NEVERD_JAVA21_HOME" \
    --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
    --work-dir "$RUNNER_TEMP/android-bound-context-transfer-$variant" "${args[@]}"
done

profile_group "Verify lower unary consumers with type tokens" 21 1200
for variant in source sdk package; do
  args=()
  if [[ "$variant" == sdk ]]; then args+=(--dependency-provider); fi
  if [[ "$variant" == package ]]; then args+=(--package-provider); fi
  python scripts/test_mobile_android_unary_token_sink.py \
    --neverd build-mobile/bin/neverd --d8 "$NEVERD_D8" \
    --java-home "$JAVA_HOME" --compiler-java-home "$NEVERD_JAVA21_HOME" \
    --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
    --work-dir "$RUNNER_TEMP/android-unary-token-sink-$variant" "${args[@]}"
done

profile_group "Verify contextual producers with closed exceptions" 21 1200
for variant in source sdk package; do
  args=()
  if [[ "$variant" == sdk ]]; then args+=(--dependency-provider); fi
  if [[ "$variant" == package ]]; then args+=(--package-provider); fi
  python scripts/test_mobile_android_context_producer_throws.py \
    --neverd build-mobile/bin/neverd --d8 "$NEVERD_D8" \
    --java-home "$JAVA_HOME" --compiler-java-home "$NEVERD_JAVA21_HOME" \
    --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
    --work-dir "$RUNNER_TEMP/android-context-producer-throws-$variant" "${args[@]}"
done

profile_group "Verify closed callback allocations before superclass construction" 25 1200
for variant in source sdk; do
  args=()
  if [[ "$variant" == sdk ]]; then args+=(--dependency-provider); fi
  python scripts/test_mobile_android_closed_callback_super.py \
    --neverd build-mobile/bin/neverd --d8 "$NEVERD_D8" \
    --java-home "$JAVA_HOME" --compiler-java-home "$NEVERD_JAVA21_HOME" \
    --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
    --work-dir "$RUNNER_TEMP/android-closed-callback-super-$variant" "${args[@]}"
done

profile_group "Verify closed anonymous constructor forwarding" 21 1200
for variant in source sdk; do
  args=()
  if [[ "$variant" == sdk ]]; then args+=(--dependency-provider); fi
  python scripts/test_mobile_android_closed_local_constructor.py \
    --neverd build-mobile/bin/neverd --d8 "$NEVERD_D8" \
    --java-home "$JAVA_HOME" --compiler-java-home "$NEVERD_JAVA21_HOME" \
    --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
    --work-dir "$RUNNER_TEMP/android-closed-local-constructor-$variant" "${args[@]}"
done

profile_group "Verify typed constructor integer defaults" 21 1200
for variant in source package; do
  args=()
  if [[ "$variant" == package ]]; then args+=(--package-owner); fi
  python scripts/test_mobile_android_typed_constructor_integer_default.py \
    --neverd build-mobile/bin/neverd --d8 "$NEVERD_D8" \
    --java-home "$JAVA_HOME" --compiler-java-home "$NEVERD_JAVA21_HOME" \
    --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
    --work-dir "$RUNNER_TEMP/android-typed-constructor-integer-default-$variant" "${args[@]}"
done

profile_group "Verify interface boolean field initialization" 21 1200
for variant in source sdk; do
  args=()
  if [[ "$variant" == sdk ]]; then args+=(--dependency-provider); fi
  python scripts/test_mobile_android_interface_boolean_initializer.py \
    --neverd build-mobile/bin/neverd --d8 "$NEVERD_D8" \
    --java-home "$JAVA_HOME" --compiler-java-home "$NEVERD_JAVA21_HOME" \
    --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
    --work-dir "$RUNNER_TEMP/android-interface-boolean-initializer-$variant" "${args[@]}"
done

profile_group "Verify paired adapter inputs" 21 1200
for variant in source sdk; do
  args=()
  if [[ "$variant" == sdk ]]; then args+=(--dependency-provider); fi
  python scripts/test_mobile_android_paired_adapter_inputs.py \
    --neverd build-mobile/bin/neverd --d8 "$NEVERD_D8" \
    --java-home "$JAVA_HOME" --compiler-java-home "$NEVERD_JAVA21_HOME" \
    --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
    --work-dir "$RUNNER_TEMP/android-paired-adapter-inputs-$variant" "${args[@]}"
done

profile_group "Verify primitive parameter bridges" 21 1200
for variant in source sdk; do
  args=()
  if [[ "$variant" == sdk ]]; then args+=(--dependency-provider); fi
  python scripts/test_mobile_android_primitive_parameter_bridge.py \
    --neverd build-mobile/bin/neverd --d8 "$NEVERD_D8" \
    --java-home "$JAVA_HOME" --compiler-java-home "$NEVERD_JAVA21_HOME" \
    --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
    --work-dir "$RUNNER_TEMP/android-primitive-parameter-bridge-$variant" "${args[@]}"
done

profile_group "Verify scoped superclass constructor formals" 21 1200
for variant in source sdk; do
  args=()
  if [[ "$variant" == sdk ]]; then args+=(--dependency-provider); fi
  python scripts/test_mobile_android_scoped_super_constructor.py \
    --neverd build-mobile/bin/neverd --d8 "$NEVERD_D8" \
    --java-home "$JAVA_HOME" --compiler-java-home "$NEVERD_JAVA21_HOME" \
    --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
    --work-dir "$RUNNER_TEMP/android-scoped-super-constructor-$variant" "${args[@]}"
done

profile_group "Verify scoped superclass scalar suffixes" 21 1200
for variant in source sdk; do
  args=()
  if [[ "$variant" == sdk ]]; then args+=(--dependency-provider); fi
  python scripts/test_mobile_android_scoped_super_scalar_suffix.py \
    --neverd build-mobile/bin/neverd --d8 "$NEVERD_D8" \
    --java-home "$JAVA_HOME" --compiler-java-home "$NEVERD_JAVA21_HOME" \
    --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
    --work-dir "$RUNNER_TEMP/android-scoped-super-scalar-suffix-$variant" "${args[@]}"
done

profile_group "Verify private member closed constructors" 25 1200
for variant in source public; do
  args=()
  if [[ "$variant" == public ]]; then args+=(--public-member); fi
  python scripts/test_mobile_android_private_member_constructor.py \
    --neverd build-mobile/bin/neverd --d8 "$NEVERD_D8" \
    --java-home "$JAVA_HOME" --compiler-java-home "$NEVERD_JAVA21_HOME" \
    --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
    --work-dir "$RUNNER_TEMP/android-private-member-constructor-$variant" "${args[@]}"
done

profile_group "Verify context callback results" 21 1200
for variant in source package; do
  args=()
  if [[ "$variant" == package ]]; then args+=(--package-owner); fi
  python scripts/test_mobile_android_context_callback_result.py \
    --neverd build-mobile/bin/neverd --d8 "$NEVERD_D8" \
    --java-home "$JAVA_HOME" --compiler-java-home "$NEVERD_JAVA21_HOME" \
    --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
    --work-dir "$RUNNER_TEMP/android-context-callback-result-$variant" "${args[@]}"
done

profile_group "Verify erased superclass token results" 21 1200
for variant in source sdk; do
  args=()
  if [[ "$variant" == sdk ]]; then args+=(--sdk-provider); fi
  python scripts/test_mobile_android_erased_super_token_result.py \
    --neverd build-mobile/bin/neverd --d8 "$NEVERD_D8" \
    --java-home "$JAVA_HOME" --compiler-java-home "$NEVERD_JAVA21_HOME" \
    --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
    --work-dir "$RUNNER_TEMP/android-erased-super-token-result-$variant" "${args[@]}"
done

profile_group "Verify unchanged constructor arguments" 25 1200
for variant in source sdk; do
  args=()
  if [[ "$variant" == sdk ]]; then args+=(--sdk-provider); fi
  python scripts/test_mobile_android_unchanged_constructor_arguments.py \
    --neverd build-mobile/bin/neverd --d8 "$NEVERD_D8" \
    --java-home "$JAVA_HOME" --compiler-java-home "$NEVERD_JAVA21_HOME" \
    --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
    --work-dir "$RUNNER_TEMP/android-unchanged-constructor-arguments-$variant" "${args[@]}"
done

profile_group "Verify intersection factory results" 21 1200
for variant in source sdk; do
  args=()
  if [[ "$variant" == sdk ]]; then args+=(--sdk-provider); fi
  python scripts/test_mobile_android_intersection_factory_result.py \
    --neverd build-mobile/bin/neverd --d8 "$NEVERD_D8" \
    --java-home "$JAVA_HOME" --compiler-java-home "$NEVERD_JAVA21_HOME" \
    --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
    --work-dir "$RUNNER_TEMP/android-intersection-factory-result-$variant" "${args[@]}"
done

profile_group "Verify paired callback factories" 21 1200
for variant in source sdk; do
  args=()
  if [[ "$variant" == sdk ]]; then args+=(--sdk-provider); fi
  python scripts/test_mobile_android_paired_callback_factory.py \
    --neverd build-mobile/bin/neverd --d8 "$NEVERD_D8" \
    --java-home "$JAVA_HOME" --compiler-java-home "$NEVERD_JAVA21_HOME" \
    --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
    --work-dir "$RUNNER_TEMP/android-paired-callback-factory-$variant" "${args[@]}"
done

profile_group "Verify instanceof constructor prefixes" 25 1200
for variant in source sdk; do
  args=()
  if [[ "$variant" == sdk ]]; then args+=(--sdk-provider); fi
  python scripts/test_mobile_android_instanceof_constructor_prefix.py \
    --neverd build-mobile/bin/neverd --d8 "$NEVERD_D8" \
    --java-home "$JAVA_HOME" --compiler-java-home "$NEVERD_JAVA21_HOME" \
    --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
    --work-dir "$RUNNER_TEMP/android-instanceof-constructor-prefix-$variant" "${args[@]}"
done

profile_group "Verify bounded producer token factories" 21 1200
for variant in source sdk; do
  args=()
  if [[ "$variant" == sdk ]]; then args+=(--sdk-provider); fi
  python scripts/test_mobile_android_bounded_producer_token.py \
    --neverd build-mobile/bin/neverd --d8 "$NEVERD_D8" \
    --java-home "$JAVA_HOME" --compiler-java-home "$NEVERD_JAVA21_HOME" \
    --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
    --work-dir "$RUNNER_TEMP/android-bounded-producer-token-$variant" "${args[@]}"
done

profile_group "Verify open instance allocation defaults" 21 1200
for variant in source dependency; do
  args=()
  if [[ "$variant" == dependency ]]; then args+=(--dependency-observer); fi
  python scripts/test_mobile_android_unwritten_instance_boolean.py \
    --neverd build-mobile/bin/neverd --d8 "$NEVERD_D8" \
    --java-home "$JAVA_HOME" --open-owner \
    --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
    --work-dir "$RUNNER_TEMP/android-open-instance-defaults-$variant" "${args[@]}"
done

profile_group "Verify ordinary static member tables" 21 1200
for variant in source sdk; do
  args=()
  if [[ "$variant" == sdk ]]; then args+=(--sdk-provider); fi
  python scripts/test_mobile_android_static_member_tables.py \
    --neverd build-mobile/bin/neverd --d8 "$NEVERD_D8" \
    --java-home "$JAVA_HOME" \
    --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
    --work-dir "$RUNNER_TEMP/android-static-member-tables-$variant" "${args[@]}"
done

profile_group "Verify bounded token element factories" 21 1200
for variant in source sdk; do
  args=()
  if [[ "$variant" == sdk ]]; then args+=(--sdk-provider); fi
  python scripts/test_mobile_android_bounded_token_element.py \
    --neverd build-mobile/bin/neverd --d8 "$NEVERD_D8" \
    --java-home "$JAVA_HOME" --compiler-java-home "$NEVERD_JAVA21_HOME" \
    --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
    --work-dir "$RUNNER_TEMP/android-bounded-token-element-$variant" "${args[@]}"
done

profile_group "Verify scoped mask generic parent chains" 25 1200
for variant in source sdk; do
  args=()
  if [[ "$variant" == sdk ]]; then args+=(--dependency-defaults); fi
  python scripts/test_mobile_android_scoped_mask_constructors.py --scoped-parent-chain \
    --neverd build-mobile/bin/neverd --d8 "$NEVERD_D8" \
    --java-home "$JAVA_HOME" --kotlin-java-home "$NEVERD_KOTLIN_JAVA_HOME" \
    --kotlin-lib "$RUNNER_TEMP/kotlin-compiler/kotlinc/lib" \
    --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
    --work-dir "$RUNNER_TEMP/android-scoped-parent-chain-$variant" "${args[@]}"
done

profile_group "Verify captured sibling constructor scopes" 25 1200
for variant in source sdk; do
  args=()
  if [[ "$variant" == sdk ]]; then args+=(--dependency-inputs); fi
  python scripts/test_mobile_android_captured_sibling_constructors.py \
    --neverd build-mobile/bin/neverd --d8 "$NEVERD_D8" \
    --java-home "$JAVA_HOME" \
    --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
    --work-dir "$RUNNER_TEMP/android-captured-sibling-constructors-$variant" "${args[@]}"
done

profile_group "Verify captured constructor argument subsets" 25 1200
for variant in source sdk; do
  args=()
  if [[ "$variant" == sdk ]]; then args+=(--dependency-inputs); fi
  python scripts/test_mobile_android_captured_sibling_constructors.py --context-input \
    --neverd build-mobile/bin/neverd --d8 "$NEVERD_D8" \
    --java-home "$JAVA_HOME" \
    --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
    --work-dir "$RUNNER_TEMP/android-captured-constructor-subsets-$variant" "${args[@]}"
done

profile_group "Verify private seeded carrier factories" 21 1200
for variant in source sdk; do
  args=()
  if [[ "$variant" == sdk ]]; then args+=(--dependency-inputs); fi
  python scripts/test_mobile_android_private_seeded_carrier.py \
    --neverd build-mobile/bin/neverd --d8 "$NEVERD_D8" \
    --java-home "$JAVA_HOME" \
    --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
    --work-dir "$RUNNER_TEMP/android-private-seeded-carrier-$variant" "${args[@]}"
done

profile_group "Verify anchored reifiable factory contexts" 21 1200
for variant in source sdk; do
  args=()
  if [[ "$variant" == sdk ]]; then args+=(--dependency-inputs); fi
  python scripts/test_mobile_android_anchored_reifiable_factory.py \
    --neverd build-mobile/bin/neverd --d8 "$NEVERD_D8" \
    --java-home "$JAVA_HOME" \
    --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
    --work-dir "$RUNNER_TEMP/android-anchored-reifiable-factory-$variant" "${args[@]}"
done

profile_group "Verify widened owner interface constructor scopes" 25 1200
for variant in source sdk; do
  args=()
  if [[ "$variant" == sdk ]]; then args+=(--dependency-provider); fi
  python scripts/test_mobile_android_bounded_owner_label.py --mask --interface-owner --bounded-interface \
    --neverd build-mobile/bin/neverd --d8 "$NEVERD_D8" \
    --java-home "$JAVA_HOME" \
    --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
    --work-dir "$RUNNER_TEMP/android-widened-owner-interface-$variant" "${args[@]}"
done

profile_group "Verify contextual producer completion bindings" 21 1200
for variant in source sdk; do
  args=()
  if [[ "$variant" == sdk ]]; then args+=(--dependency-inputs); fi
  python scripts/test_mobile_android_contextual_producer_completion.py \
    --neverd build-mobile/bin/neverd --d8 "$NEVERD_D8" \
    --java-home "$JAVA_HOME" \
    --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
    --work-dir "$RUNNER_TEMP/android-contextual-producer-completion-$variant" "${args[@]}"
done

profile_group "Verify anchored closed head result slots" 21 1200
for variant in source sdk; do
  args=()
  if [[ "$variant" == sdk ]]; then args+=(--dependency-inputs); fi
  python scripts/test_mobile_android_anchored_closed_head.py \
    --neverd build-mobile/bin/neverd --d8 "$NEVERD_D8" \
    --java-home "$JAVA_HOME" \
    --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
    --work-dir "$RUNNER_TEMP/android-anchored-closed-head-$variant" "${args[@]}"
done

profile_group "Verify bounded initializer array bindings" 21 1200
for variant in source sdk; do
  args=()
  if [[ "$variant" == sdk ]]; then args+=(--dependency-inputs); fi
  python scripts/test_mobile_android_bounded_initializer_array.py \
    --neverd build-mobile/bin/neverd --d8 "$NEVERD_D8" \
    --java-home "$JAVA_HOME" \
    --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
    --work-dir "$RUNNER_TEMP/android-bounded-initializer-array-$variant" "${args[@]}"
done

profile_group "Verify bounded token callback void bindings" 21 1200
for variant in source sdk; do
  args=()
  if [[ "$variant" == sdk ]]; then args+=(--dependency-inputs); fi
  python scripts/test_mobile_android_bounded_token_callback_void.py \
    --neverd build-mobile/bin/neverd --d8 "$NEVERD_D8" \
    --java-home "$JAVA_HOME" \
    --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
    --work-dir "$RUNNER_TEMP/android-bounded-token-callback-void-$variant" "${args[@]}"
done

profile_group "Verify closed reifiable constructor contracts" 25 1200
for variant in source sdk; do
  args=()
  if [[ "$variant" == sdk ]]; then args+=(--dependency-inputs); fi
  python scripts/test_mobile_android_closed_reifiable_constructors.py \
    --neverd build-mobile/bin/neverd --d8 "$NEVERD_D8" \
    --java-home "$JAVA_HOME" \
    --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
    --work-dir "$RUNNER_TEMP/android-closed-reifiable-constructors-$variant" "${args[@]}"
done

profile_group "Verify typed token constructor transfers" 25 1200
for variant in source sdk; do
  args=()
  if [[ "$variant" == sdk ]]; then args+=(--dependency-inputs); fi
  python scripts/test_mobile_android_typed_token_constructor_transfer.py \
    --neverd build-mobile/bin/neverd --d8 "$NEVERD_D8" \
    --java-home "$JAVA_HOME" \
    --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
    --work-dir "$RUNNER_TEMP/android-typed-token-constructor-transfer-$variant" "${args[@]}"
done

profile_group "Verify bounded token nominal results" 21 1200
for variant in source sdk; do
  args=()
  if [[ "$variant" == sdk ]]; then args+=(--dependency-inputs); fi
  python scripts/test_mobile_android_bounded_token_nominal.py \
    --neverd build-mobile/bin/neverd --d8 "$NEVERD_D8" \
    --java-home "$JAVA_HOME" \
    --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
    --work-dir "$RUNNER_TEMP/android-bounded-token-nominal-$variant" "${args[@]}"
done

profile_group "Verify package nominal selector results" 21 1200
for variant in source sdk; do
  args=()
  if [[ "$variant" == sdk ]]; then args+=(--dependency-inputs); fi
  python scripts/test_mobile_android_package_nominal_selector.py \
    --neverd build-mobile/bin/neverd --d8 "$NEVERD_D8" \
    --java-home "$JAVA_HOME" \
    --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
    --work-dir "$RUNNER_TEMP/android-package-nominal-selector-$variant" "${args[@]}"
done

profile_group "Verify private contextual carrier factories" 21 1200
for variant in source sdk; do
  args=()
  if [[ "$variant" == sdk ]]; then args+=(--dependency-inputs); fi
  python scripts/test_mobile_android_private_contextual_carrier.py \
    --neverd build-mobile/bin/neverd --d8 "$NEVERD_D8" \
    --java-home "$JAVA_HOME" \
    --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
    --work-dir "$RUNNER_TEMP/android-private-contextual-carrier-$variant" "${args[@]}"
done

profile_group "Verify closed superclass parameter substitutions" 21 1200
for variant in source sdk; do
  args=()
  if [[ "$variant" == sdk ]]; then args+=(--dependency-inputs); fi
  python scripts/test_mobile_android_closed_super_parameters.py \
    --neverd build-mobile/bin/neverd --d8 "$NEVERD_D8" \
    --java-home "$JAVA_HOME" \
    --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
    --work-dir "$RUNNER_TEMP/android-closed-super-parameters-$variant" "${args[@]}"
done

profile_group "Verify captured raw superclass carriers" 25 1200
for variant in source sdk; do
  args=()
  if [[ "$variant" == sdk ]]; then args+=(--dependency-inputs); fi
  python scripts/test_mobile_android_captured_raw_super.py \
    --neverd build-mobile/bin/neverd --d8 "$NEVERD_D8" \
    --java-home "$JAVA_HOME" \
    --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
    --work-dir "$RUNNER_TEMP/android-captured-raw-super-$variant" "${args[@]}"
done

profile_group "Verify backward monitor cleanup joins" 21 1200
for variant in source sdk; do
  args=()
  if [[ "$variant" == sdk ]]; then args+=(--dependency-provider); fi
  python scripts/test_mobile_android_monitor_copy_cleanup.py --backward-cleanup \
    --neverd build-mobile/bin/neverd --d8 "$NEVERD_D8" \
    --java-home "$JAVA_HOME" \
    --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
    --work-dir "$RUNNER_TEMP/android-monitor-backward-cleanup-$variant" "${args[@]}"
done

profile_group "Verify closed wildcard superclass constructor inputs" 21 1200
for variant in source sdk; do
  args=()
  if [[ "$variant" == sdk ]]; then args+=(--dependency-inputs); fi
  python scripts/test_mobile_android_closed_wildcard_super.py \
    --neverd build-mobile/bin/neverd --d8 "$NEVERD_D8" \
    --java-home "$JAVA_HOME" \
    --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
    --work-dir "$RUNNER_TEMP/android-closed-wildcard-super-$variant" "${args[@]}"
done

profile_group "Verify bounded star class token results" 21 1200
for variant in source sdk; do
  args=()
  if [[ "$variant" == sdk ]]; then args+=(--dependency-inputs); fi
  python scripts/test_mobile_android_bounded_star_token.py \
    --neverd build-mobile/bin/neverd --d8 "$NEVERD_D8" \
    --java-home "$JAVA_HOME" \
    --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
    --work-dir "$RUNNER_TEMP/android-bounded-star-token-$variant" "${args[@]}"
done

profile_group "Verify class token constructor prefix delegation" 21 1200
for variant in source sdk; do
  args=()
  if [[ "$variant" == sdk ]]; then args+=(--dependency-inputs); fi
  python scripts/test_mobile_android_token_prefix_delegation.py \
    --neverd build-mobile/bin/neverd --d8 "$NEVERD_D8" \
    --java-home "$JAVA_HOME" \
    --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
    --work-dir "$RUNNER_TEMP/android-token-prefix-delegation-$variant" "${args[@]}"
done

profile_group "Verify covariant descriptor superclass inputs" 21 1200
for variant in source sdk; do
  args=()
  if [[ "$variant" == sdk ]]; then args+=(--dependency-inputs); fi
  python scripts/test_mobile_android_covariant_descriptor_super.py \
    --neverd build-mobile/bin/neverd --d8 "$NEVERD_D8" \
    --java-home "$JAVA_HOME" \
    --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
    --work-dir "$RUNNER_TEMP/android-covariant-descriptor-super-$variant" "${args[@]}"
done

profile_group "Verify bounded covariant void consumers" 21 1200
for variant in source sdk; do
  args=()
  if [[ "$variant" == sdk ]]; then args+=(--dependency-inputs); fi
  python scripts/test_mobile_android_bounded_covariant_consumer.py \
    --neverd build-mobile/bin/neverd --d8 "$NEVERD_D8" \
    --java-home "$JAVA_HOME" \
    --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
    --work-dir "$RUNNER_TEMP/android-bounded-covariant-consumer-$variant" "${args[@]}"
done

profile_group "Verify map to nested pair list bindings" 21 1200
for variant in source sdk; do
  args=()
  if [[ "$variant" == sdk ]]; then args+=(--dependency-inputs); fi
  python scripts/test_mobile_android_map_pair_list.py \
    --neverd build-mobile/bin/neverd --d8 "$NEVERD_D8" \
    --java-home "$JAVA_HOME" \
    --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
    --work-dir "$RUNNER_TEMP/android-map-pair-list-$variant" "${args[@]}"
done

profile_group "Verify reifiable context result bindings" 21 1200
for variant in source sdk; do
  args=()
  if [[ "$variant" == sdk ]]; then args+=(--dependency-inputs); fi
  python scripts/test_mobile_android_reifiable_context_result.py \
    --neverd build-mobile/bin/neverd --d8 "$NEVERD_D8" \
    --java-home "$JAVA_HOME" \
    --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
    --work-dir "$RUNNER_TEMP/android-reifiable-context-result-$variant" "${args[@]}"
done

profile_group "Verify closed covariant constructor delegations" 21 1200
for variant in source sdk; do
  args=()
  if [[ "$variant" == sdk ]]; then args+=(--dependency-inputs); fi
  python scripts/test_mobile_android_closed_covariant_delegation.py \
    --neverd build-mobile/bin/neverd --d8 "$NEVERD_D8" \
    --java-home "$JAVA_HOME" \
    --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
    --work-dir "$RUNNER_TEMP/android-closed-covariant-delegation-$variant" "${args[@]}"
done

profile_group "Verify token results passed to generic superclass constructors" 21 1200
for variant in source sdk; do
  args=()
  if [[ "$variant" == sdk ]]; then args+=(--dependency-inputs); fi
  python scripts/test_mobile_android_token_super_result.py \
    --neverd build-mobile/bin/neverd --d8 "$NEVERD_D8" \
    --java-home "$JAVA_HOME" \
    --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
    --work-dir "$RUNNER_TEMP/android-token-super-result-$variant" "${args[@]}"
done

profile_group "Verify protected preparation and final-field abrupt tails" 21 1200
for variant in source sdk; do
  args=()
  if [[ "$variant" == sdk ]]; then args+=(--dependency-inputs); fi
  python scripts/test_mobile_android_protected_abrupt_finals.py \
    --neverd build-mobile/bin/neverd --d8 "$NEVERD_D8" \
    --java-home "$JAVA_HOME" \
    --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
    --work-dir "$RUNNER_TEMP/android-protected-abrupt-finals-$variant" "${args[@]}"
done

profile_group "Verify closed synthetic access constructor markers" 21 1200
for variant in source sdk; do
  args=()
  if [[ "$variant" == sdk ]]; then args+=(--dependency-inputs); fi
  python scripts/test_mobile_android_closed_access_marker.py \
    --neverd build-mobile/bin/neverd --d8 "$NEVERD_D8" \
    --java-home "$JAVA_HOME" \
    --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
    --work-dir "$RUNNER_TEMP/android-closed-access-marker-$variant" "${args[@]}"
done

profile_group "Verify monitor release-only exception handlers" 21 1200
for variant in source sdk; do
  args=()
  if [[ "$variant" == sdk ]]; then args+=(--dependency-provider); fi
  python scripts/test_mobile_android_monitor_copy_cleanup.py --release-only-handlers \
    --neverd build-mobile/bin/neverd --d8 "$NEVERD_D8" \
    --java-home "$JAVA_HOME" \
    --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
    --work-dir "$RUNNER_TEMP/android-monitor-release-handlers-$variant" "${args[@]}"
done

profile_group "Verify copied exceptions at monitor release-only handlers" 21 1200
for variant in source sdk; do
  args=()
  if [[ "$variant" == sdk ]]; then args+=(--dependency-provider); fi
  python scripts/test_mobile_android_monitor_copy_cleanup.py --release-only-handlers --release-exception-copies \
    --neverd build-mobile/bin/neverd --d8 "$NEVERD_D8" \
    --java-home "$JAVA_HOME" \
    --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
    --work-dir "$RUNNER_TEMP/android-monitor-release-exception-copies-$variant" "${args[@]}"
done

profile_group "Verify package synthetic names with closed generic owners" 25 900
for variant in source sdk; do
  args=()
  if [[ "$variant" == sdk ]]; then args+=(--dependency-provider); fi
  python scripts/test_mobile_android_synthetic_instance_names.py --closed-owner \
    --neverd build-mobile/bin/neverd --d8 "$NEVERD_D8" \
    --java-home "$JAVA_HOME" \
    --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
    --work-dir "$RUNNER_TEMP/android-package-closed-identifiers-$variant" "${args[@]}"
done

profile_group "Verify allocated invariant constructor transfers" 25 1200
for variant in source sdk; do
  args=()
  if [[ "$variant" == sdk ]]; then args+=(--dependency-inputs); fi
  python scripts/test_mobile_android_typed_token_constructor_transfer.py --allocated-transfer \
    --neverd build-mobile/bin/neverd --d8 "$NEVERD_D8" \
    --java-home "$JAVA_HOME" \
    --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
    --work-dir "$RUNNER_TEMP/android-allocated-token-transfer-$variant" "${args[@]}"
done

profile_group "Verify closed array list constructor delegation" 25 1200
for variant in source sdk; do
  args=()
  if [[ "$variant" == sdk ]]; then args+=(--dependency-provider); fi
  python scripts/test_mobile_android_array_list_delegation.py \
    --neverd build-mobile/bin/neverd --d8 "$NEVERD_D8" \
    --java-home "$JAVA_HOME" \
    --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
    --work-dir "$RUNNER_TEMP/android-array-list-delegation-$variant" "${args[@]}"
done

profile_group "Verify bounded array results with invariant tokens" 25 1200
for variant in source sdk; do
  args=()
  if [[ "$variant" == sdk ]]; then args+=(--dependency-provider); fi
  python scripts/test_mobile_android_bounded_array_token.py \
    --neverd build-mobile/bin/neverd --d8 "$NEVERD_D8" \
    --java-home "$JAVA_HOME" \
    --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
    --work-dir "$RUNNER_TEMP/android-bounded-array-token-$variant" "${args[@]}"
done

profile_group "Verify contextual bounded token carrier results" 25 1200
for variant in source sdk; do
  args=()
  if [[ "$variant" == sdk ]]; then args+=(--dependency-provider); fi
  python scripts/test_mobile_android_bounded_array_token.py --carrier-result \
    --neverd build-mobile/bin/neverd --d8 "$NEVERD_D8" \
    --java-home "$JAVA_HOME" \
    --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
    --work-dir "$RUNNER_TEMP/android-contextual-token-carrier-$variant" "${args[@]}"
done

profile_group "Verify scalar binary result factories through public facades" 25 1200
for variant in source sdk; do
  args=()
  if [[ "$variant" == sdk ]]; then args+=(--dependency-provider); fi
  python scripts/test_mobile_android_nullary_carrier_factories.py --scalar-argument \
    --neverd build-mobile/bin/neverd --d8 "$NEVERD_D8" \
    --java-home "$JAVA_HOME" \
    --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
    --work-dir "$RUNNER_TEMP/android-scalar-pair-factory-$variant" "${args[@]}"
done

profile_group "Verify reifiable inputs beside invariant factory results" 25 1200
for variant in source sdk; do
  args=()
  if [[ "$variant" == sdk ]]; then args+=(--dependency-provider); fi
  python scripts/test_mobile_android_nullary_carrier_factories.py --reifiable-input \
    --neverd build-mobile/bin/neverd --d8 "$NEVERD_D8" \
    --java-home "$JAVA_HOME" \
    --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
    --work-dir "$RUNNER_TEMP/android-reifiable-carrier-factory-$variant" "${args[@]}"
done

profile_group "Verify bounded array inputs beside closed contexts" 25 1200
for variant in source sdk; do
  args=()
  if [[ "$variant" == sdk ]]; then args+=(--dependency-provider); fi
  python scripts/test_mobile_android_bounded_array_context_void.py \
    --neverd build-mobile/bin/neverd --d8 "$NEVERD_D8" \
    --java-home "$JAVA_HOME" \
    --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
    --work-dir "$RUNNER_TEMP/android-bounded-array-context-void-$variant" "${args[@]}"
done

profile_group "Verify covariant bounded list inputs beside closed contexts" 25 1200
for variant in source sdk; do
  args=()
  if [[ "$variant" == sdk ]]; then args+=(--dependency-provider); fi
  python scripts/test_mobile_android_bounded_array_context_void.py --covariant-input \
    --neverd build-mobile/bin/neverd --d8 "$NEVERD_D8" \
    --java-home "$JAVA_HOME" \
    --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
    --work-dir "$RUNNER_TEMP/android-covariant-context-list-void-$variant" "${args[@]}"
done

profile_group "Verify nested invariant factory results through public facades" 25 1200
for variant in source sdk; do
  args=()
  if [[ "$variant" == sdk ]]; then args+=(--dependency-provider); fi
  python scripts/test_mobile_android_nullary_carrier_factories.py --nested-result \
    --neverd build-mobile/bin/neverd --d8 "$NEVERD_D8" \
    --java-home "$JAVA_HOME" \
    --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
    --work-dir "$RUNNER_TEMP/android-nested-invariant-factory-$variant" "${args[@]}"
done

profile_group "Verify dependent array carrier factory contracts" 25 1200
for variant in canonical-source canonical-sdk interface-source interface-sdk; do
  args=()
  if [[ "$variant" == *sdk ]]; then args+=(--dependency-provider); fi
  if [[ "$variant" == interface-* ]]; then args+=(--interface-slot); fi
  python scripts/test_mobile_android_dependent_array_carrier_factory.py \
    --neverd build-mobile/bin/neverd --d8 "$NEVERD_D8" \
    --java-home "$JAVA_HOME" \
    --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
    --work-dir "$RUNNER_TEMP/android-dependent-array-carrier-factory-$variant" "${args[@]}"
done

profile_group "Verify closed reifiable auxiliary instance consumers" 25 1200
for variant in source sdk; do
  args=()
  if [[ "$variant" == sdk ]]; then args+=(--dependency-provider); fi
  python scripts/test_mobile_android_closed_auxiliary_void.py \
    --neverd build-mobile/bin/neverd --d8 "$NEVERD_D8" \
    --java-home "$JAVA_HOME" \
    --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
    --work-dir "$RUNNER_TEMP/android-closed-auxiliary-void-$variant" "${args[@]}"
done

profile_group "Verify private seeded primitive instance consumers" 25 1200
for variant in source sdk; do
  args=()
  if [[ "$variant" == sdk ]]; then args+=(--dependency-provider); fi
  python scripts/test_mobile_android_private_seeded_primitive.py \
    --neverd build-mobile/bin/neverd --d8 "$NEVERD_D8" \
    --java-home "$JAVA_HOME" \
    --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
    --work-dir "$RUNNER_TEMP/android-private-seeded-primitive-$variant" "${args[@]}"
done

profile_group "Verify instance nested factory bindings and virtual dispatch" 25 1200
for variant in source sdk; do
  args=()
  if [[ "$variant" == sdk ]]; then args+=(--dependency-provider); fi
  python scripts/test_mobile_android_nullary_carrier_factories.py --instance-nested-result \
    --neverd build-mobile/bin/neverd --d8 "$NEVERD_D8" \
    --java-home "$JAVA_HOME" \
    --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
    --work-dir "$RUNNER_TEMP/android-instance-nested-factory-$variant" "${args[@]}"
done

profile_group "Verify exact private enclosing carrier factory scopes" 25 1200
for variant in source sdk; do
  args=()
  if [[ "$variant" == sdk ]]; then args+=(--dependency-provider); fi
  python scripts/test_mobile_android_nullary_carrier_factories.py --enclosing-nested-result \
    --neverd build-mobile/bin/neverd --d8 "$NEVERD_D8" \
    --java-home "$JAVA_HOME" \
    --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
    --work-dir "$RUNNER_TEMP/android-enclosing-nested-factory-$variant" "${args[@]}"
done

profile_group "Verify independent synthetic member constructor and scope proofs" 25 1200
for variant in source sdk; do
  args=()
  if [[ "$variant" == sdk ]]; then args+=(--dependency-provider); fi
  python scripts/test_mobile_android_nullary_carrier_factories.py --independent-member \
    --neverd build-mobile/bin/neverd --d8 "$NEVERD_D8" \
    --java-home "$JAVA_HOME" \
    --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
    --work-dir "$RUNNER_TEMP/android-independent-synthetic-member-$variant" "${args[@]}"
done

profile_group "Verify partial constructor finals and required metadata restoration" 25 1200
for variant in source sdk; do
  args=()
  if [[ "$variant" == sdk ]]; then args+=(--dependency-provider); fi
  python scripts/test_mobile_android_nullary_carrier_factories.py --partial-final-constructors \
    --neverd build-mobile/bin/neverd --d8 "$NEVERD_D8" \
    --java-home "$JAVA_HOME" \
    --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
    --work-dir "$RUNNER_TEMP/android-partial-final-constructors-$variant" "${args[@]}"
done

profile_group "Verify closed input trees and independent instance method results" 25 1200
for variant in source sdk; do
  args=()
  if [[ "$variant" == sdk ]]; then args+=(--dependency-provider); fi
  python scripts/test_mobile_android_closed_input_element.py \
    --neverd build-mobile/bin/neverd --d8 "$NEVERD_D8" \
    --java-home "$JAVA_HOME" \
    --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
    --work-dir "$RUNNER_TEMP/android-closed-input-element-$variant" "${args[@]}"
done

profile_group "Verify package seeded nominal factories and exact provider effects" 25 1200
for variant in source sdk; do
  args=()
  if [[ "$variant" == sdk ]]; then args+=(--dependency-provider); fi
  python scripts/test_mobile_android_package_seeded_nominal.py \
    --neverd build-mobile/bin/neverd --d8 "$NEVERD_D8" \
    --java-home "$JAVA_HOME" \
    --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
    --work-dir "$RUNNER_TEMP/android-package-seeded-nominal-$variant" "${args[@]}"
done

profile_group "Verify paired nominal factories and original varargs array identity" 25 1200
for variant in source sdk; do
  args=()
  if [[ "$variant" == sdk ]]; then args+=(--dependency-provider); fi
  python scripts/test_mobile_android_paired_array_nominal.py \
    --neverd build-mobile/bin/neverd --d8 "$NEVERD_D8" \
    --java-home "$JAVA_HOME" \
    --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
    --work-dir "$RUNNER_TEMP/android-paired-array-nominal-$variant" "${args[@]}"
done

profile_group "Verify raw nominal context and independent evaluator array binding" 25 1200
for variant in source sdk; do
  args=()
  if [[ "$variant" == sdk ]]; then args+=(--dependency-provider); fi
  python scripts/test_mobile_android_raw_context_array_nominal.py \
    --neverd build-mobile/bin/neverd --d8 "$NEVERD_D8" \
    --java-home "$JAVA_HOME" \
    --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
    --work-dir "$RUNNER_TEMP/android-raw-context-array-nominal-$variant" "${args[@]}"
done

profile_group "Verify allocation receiver transfer and immutable value snapshots" 25 1200
for variant in source sdk; do
  args=()
  if [[ "$variant" == sdk ]]; then args+=(--dependency-provider); fi
  python scripts/test_mobile_android_allocation_receiver_transfer.py \
    --neverd build-mobile/bin/neverd --d8 "$NEVERD_D8" \
    --java-home "$JAVA_HOME" \
    --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
    --work-dir "$RUNNER_TEMP/android-allocation-receiver-transfer-$variant" "${args[@]}"
done

profile_group "Verify closed array constructor tokens and opaque Class values" 25 1200
for variant in source sdk; do
  args=()
  if [[ "$variant" == sdk ]]; then args+=(--dependency-provider); fi
  python scripts/test_mobile_android_closed_array_constructor_token.py \
    --neverd build-mobile/bin/neverd --d8 "$NEVERD_D8" \
    --java-home "$JAVA_HOME" --compiler-java-home "$NEVERD_JAVA21_HOME" \
    --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
    --work-dir "$RUNNER_TEMP/android-closed-array-constructor-token-$variant" "${args[@]}"
done

profile_group "Verify paired seeded nominal results and complete method scopes" 25 1200
for variant in source sdk; do
  args=()
  if [[ "$variant" == sdk ]]; then args+=(--dependency-provider); fi
  python scripts/test_mobile_android_paired_seeded_nominal.py \
    --neverd build-mobile/bin/neverd --d8 "$NEVERD_D8" \
    --java-home "$JAVA_HOME" \
    --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
    --work-dir "$RUNNER_TEMP/android-paired-seeded-nominal-$variant" "${args[@]}"
done

profile_group "Verify paired converter nominal leaves and shared output scopes" 25 1200
for variant in source sdk; do
  args=()
  if [[ "$variant" == sdk ]]; then args+=(--dependency-provider); fi
  python scripts/test_mobile_android_paired_converter_nominal.py \
    --neverd build-mobile/bin/neverd --d8 "$NEVERD_D8" \
    --java-home "$JAVA_HOME" \
    --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
    --work-dir "$RUNNER_TEMP/android-paired-converter-nominal-$variant" "${args[@]}"
done

profile_group "Verify final initialization prefixes before split cleanup regions" 25 1200
for variant in source sdk; do
  args=()
  if [[ "$variant" == sdk ]]; then args+=(--dependency-provider); fi
  python scripts/test_mobile_android_final_prefix_cleanup.py \
    --neverd build-mobile/bin/neverd --d8 "$NEVERD_D8" \
    --java-home "$JAVA_HOME" \
    --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
    --work-dir "$RUNNER_TEMP/android-final-prefix-cleanup-$variant" "${args[@]}"
done

profile_group "Verify starred property heads and complete nominal output scopes" 25 1200
for variant in source sdk; do
  args=()
  if [[ "$variant" == sdk ]]; then args+=(--dependency-provider); fi
  python scripts/test_mobile_android_starred_head_nominal.py \
    --neverd build-mobile/bin/neverd --d8 "$NEVERD_D8" \
    --java-home "$JAVA_HOME" \
    --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
    --work-dir "$RUNNER_TEMP/android-starred-head-nominal-$variant" "${args[@]}"
done

profile_group "Verify package seeded carrier results and private nested access" 25 1200
for variant in source sdk; do
  args=()
  if [[ "$variant" == sdk ]]; then args+=(--dependency-provider); fi
  python scripts/test_mobile_android_package_seeded_carrier.py \
    --neverd build-mobile/bin/neverd --d8 "$NEVERD_D8" \
    --java-home "$JAVA_HOME" \
    --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
    --work-dir "$RUNNER_TEMP/android-package-seeded-carrier-$variant" "${args[@]}"
done

profile_group "Verify private seeded carrier inputs and original primitive flags" 25 1200
for variant in source sdk; do
  args=()
  if [[ "$variant" == sdk ]]; then args+=(--dependency-provider); fi
  python scripts/test_mobile_android_private_seeded_carrier_input.py \
    --neverd build-mobile/bin/neverd --d8 "$NEVERD_D8" \
    --java-home "$JAVA_HOME" \
    --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
    --work-dir "$RUNNER_TEMP/android-private-seeded-carrier-input-$variant" "${args[@]}"
done

profile_group "Verify interface allocations and original initialization failure order" 25 1200
for variant in source sdk; do
  args=()
  if [[ "$variant" == sdk ]]; then args+=(--dependency-provider); fi
  python scripts/test_mobile_android_interface_allocation_initializer.py \
    --neverd build-mobile/bin/neverd --d8 "$NEVERD_D8" \
    --java-home "$JAVA_HOME" \
    --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
    --work-dir "$RUNNER_TEMP/android-interface-allocation-initializer-$variant" "${args[@]}"
done

profile_group "Verify protected marker results and original virtual dispatch" 25 1200
for variant in source sdk; do
  args=()
  if [[ "$variant" == sdk ]]; then args+=(--dependency-provider); fi
  python scripts/test_mobile_android_protected_marker_result.py \
    --neverd build-mobile/bin/neverd --d8 "$NEVERD_D8" \
    --java-home "$JAVA_HOME" \
    --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
    --work-dir "$RUNNER_TEMP/android-protected-marker-result-$variant" "${args[@]}"
done

profile_group "Verify abstract marker producers and original virtual dispatch" 25 1200
for variant in source sdk; do
  args=()
  if [[ "$variant" == sdk ]]; then args+=(--dependency-provider); fi
  python scripts/test_mobile_android_abstract_marker_producer.py \
    --neverd build-mobile/bin/neverd --d8 "$NEVERD_D8" \
    --java-home "$JAVA_HOME" \
    --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
    --work-dir "$RUNNER_TEMP/android-abstract-marker-producer-$variant" "${args[@]}"
done

profile_group "Verify private direct inputs and every original primitive result" 25 1200
for variant in source sdk; do
  args=()
  if [[ "$variant" == sdk ]]; then args+=(--dependency-provider); fi
  python scripts/test_mobile_android_private_direct_primitive.py \
    --neverd build-mobile/bin/neverd --d8 "$NEVERD_D8" \
    --java-home "$JAVA_HOME" \
    --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
    --work-dir "$RUNNER_TEMP/android-private-direct-primitive-$variant" "${args[@]}"
done

profile_group "Verify protected array transfers and original virtual dispatch" 25 1200
for variant in source sdk; do
  args=()
  if [[ "$variant" == sdk ]]; then args+=(--dependency-provider); fi
  python scripts/test_mobile_android_protected_array_transfer.py \
    --neverd build-mobile/bin/neverd --d8 "$NEVERD_D8" \
    --java-home "$JAVA_HOME" \
    --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
    --work-dir "$RUNNER_TEMP/android-protected-array-transfer-$variant" "${args[@]}"
done

profile_group "Verify private dependent carrier bounds and original results" 25 1200
for variant in source sdk; do
  args=()
  if [[ "$variant" == sdk ]]; then args+=(--dependency-provider); fi
  python scripts/test_mobile_android_private_bound_carrier_transfer.py \
    --neverd build-mobile/bin/neverd --d8 "$NEVERD_D8" \
    --java-home "$JAVA_HOME" \
    --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
    --work-dir "$RUNNER_TEMP/android-private-bound-carrier-transfer-$variant" "${args[@]}"
done

profile_group "Verify closed constructor exceptions and captured delegation effects" 25 1200
for variant in source sdk; do
  args=()
  if [[ "$variant" == sdk ]]; then args+=(--dependency-provider); fi
  python scripts/test_mobile_android_constructor_closed_exceptions.py \
    --neverd build-mobile/bin/neverd --d8 "$NEVERD_D8" \
    --java-home "$JAVA_HOME" \
    --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
    --work-dir "$RUNNER_TEMP/android-constructor-closed-exceptions-$variant" "${args[@]}"
done

profile_group "Verify captured anonymous wildcard superclass calls" 25 1200
for variant in source sdk; do
  args=()
  if [[ "$variant" == sdk ]]; then args+=(--dependency-provider); fi
  python scripts/test_mobile_android_anonymous_super_token_result.py \
    --neverd build-mobile/bin/neverd --d8 "$NEVERD_D8" \
    --java-home "$JAVA_HOME" \
    --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
    --work-dir "$RUNNER_TEMP/android-anonymous-super-token-result-$variant" "${args[@]}"
done

profile_group "Verify protected array consumers and original virtual effects" 25 1200
for variant in source sdk; do
  args=()
  if [[ "$variant" == sdk ]]; then args+=(--dependency-provider); fi
  python scripts/test_mobile_android_protected_array_void.py \
    --neverd build-mobile/bin/neverd --d8 "$NEVERD_D8" \
    --java-home "$JAVA_HOME" \
    --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
    --work-dir "$RUNNER_TEMP/android-protected-array-void-$variant" "${args[@]}"
done

profile_group "Verify protected interface-bound consumers and context identity" 25 1200
for variant in source sdk; do
  args=()
  if [[ "$variant" == sdk ]]; then args+=(--dependency-provider); fi
  python scripts/test_mobile_android_protected_marker_void.py \
    --neverd build-mobile/bin/neverd --d8 "$NEVERD_D8" \
    --java-home "$JAVA_HOME" \
    --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
    --work-dir "$RUNNER_TEMP/android-protected-marker-void-$variant" "${args[@]}"
done

profile_group "Verify package constructor contracts across original parent edges" 25 1200
for variant in source sdk; do
  args=()
  if [[ "$variant" == sdk ]]; then args+=(--dependency-provider); fi
  python scripts/test_mobile_android_package_closed_constructor_parent.py \
    --neverd build-mobile/bin/neverd --d8 "$NEVERD_D8" \
    --java-home "$JAVA_HOME" \
    --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
    --work-dir "$RUNNER_TEMP/android-package-closed-constructor-parent-$variant" "${args[@]}"
done

profile_group "Verify inherited final argument calls in constructor allocations" 21 1200
for variant in source sdk; do
  args=()
  if [[ "$variant" == sdk ]]; then args+=(--dependency-provider); fi
  python scripts/test_mobile_android_inherited_final_arguments.py \
    --neverd build-mobile/bin/neverd --d8 "$NEVERD_D8" \
    --java-home "$JAVA_HOME" \
    --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
    --work-dir "$RUNNER_TEMP/android-inherited-final-arguments-$variant" "${args[@]}"
done

profile_group "Verify static argument results in closed superclass constructors" 25 1200
for variant in source sdk; do
  args=()
  if [[ "$variant" == sdk ]]; then args+=(--dependency-inputs); fi
  python scripts/test_mobile_android_static_argument_super_result.py \
    --neverd build-mobile/bin/neverd --d8 "$NEVERD_D8" \
    --java-home "$JAVA_HOME" \
    --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
    --work-dir "$RUNNER_TEMP/android-static-argument-super-result-$variant" "${args[@]}"
done

profile_group "Verify paired invariant instance inputs and closed results" 21 1200
for variant in source sdk; do
  args=()
  if [[ "$variant" == sdk ]]; then args+=(--dependency-provider); fi
  python scripts/test_mobile_android_paired_instance_nominal.py \
    --neverd build-mobile/bin/neverd --d8 "$NEVERD_D8" \
    --java-home "$JAVA_HOME" --compiler-java-home "$NEVERD_JAVA21_HOME" \
    --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
    --work-dir "$RUNNER_TEMP/android-paired-instance-nominal-$variant" "${args[@]}"
done

profile_group "Verify static independent results with reifiable array inputs" 21 1200
for variant in source sdk; do
  args=()
  if [[ "$variant" == sdk ]]; then args+=(--dependency-provider); fi
  python scripts/test_mobile_android_static_reifiable_results.py \
    --neverd build-mobile/bin/neverd --d8 "$NEVERD_D8" \
    --java-home "$JAVA_HOME" --compiler-java-home "$NEVERD_JAVA21_HOME" \
    --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
    --work-dir "$RUNNER_TEMP/android-static-reifiable-results-$variant" "${args[@]}"
done

profile_group "Verify seeded results with closed covariant auxiliary inputs" 21 1200
for variant in source sdk; do
  args=()
  if [[ "$variant" == sdk ]]; then args+=(--dependency-provider); fi
  python scripts/test_mobile_android_seeded_covariant_auxiliary.py \
    --neverd build-mobile/bin/neverd --d8 "$NEVERD_D8" \
    --java-home "$JAVA_HOME" --compiler-java-home "$NEVERD_JAVA21_HOME" \
    --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
    --work-dir "$RUNNER_TEMP/android-seeded-covariant-auxiliary-$variant" "${args[@]}"
done

profile_group "Verify invariant tokens and nominal array results" 21 1200
for variant in source sdk; do
  args=()
  if [[ "$variant" == sdk ]]; then args+=(--dependency-provider); fi
  python scripts/test_mobile_android_token_nominal_array_results.py \
    --neverd build-mobile/bin/neverd --d8 "$NEVERD_D8" \
    --java-home "$JAVA_HOME" --compiler-java-home "$NEVERD_JAVA21_HOME" \
    --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
    --work-dir "$RUNNER_TEMP/android-token-nominal-array-results-$variant" "${args[@]}"
done

profile_group "Verify private static helpers with reifiable inputs" 21 1200
for variant in source sdk; do
  args=()
  if [[ "$variant" == sdk ]]; then args+=(--dependency-provider); fi
  python scripts/test_mobile_android_private_static_reifiable_results.py \
    --neverd build-mobile/bin/neverd --d8 "$NEVERD_D8" \
    --java-home "$JAVA_HOME" --compiler-java-home "$NEVERD_JAVA21_HOME" \
    --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
    --work-dir "$RUNNER_TEMP/android-private-static-reifiable-results-$variant" "${args[@]}"
done

profile_group "Verify throwing token factories with ordinary arrays" 21 1200
for variant in source sdk; do
  args=()
  if [[ "$variant" == sdk ]]; then args+=(--dependency-provider); fi
  python scripts/test_mobile_android_throwing_token_arrays.py \
    --neverd build-mobile/bin/neverd --d8 "$NEVERD_D8" \
    --java-home "$JAVA_HOME" --compiler-java-home "$NEVERD_JAVA21_HOME" \
    --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
    --work-dir "$RUNNER_TEMP/android-throwing-token-arrays-$variant" "${args[@]}"
done

profile_group "Verify throwing long operations in nested allocations" 21 1200
for variant in source sdk; do
  args=()
  if [[ "$variant" == sdk ]]; then args+=(--dependency-provider); fi
  python scripts/test_mobile_android_allocation_long_division.py \
    --neverd build-mobile/bin/neverd --d8 "$NEVERD_D8" \
    --java-home "$JAVA_HOME" --compiler-java-home "$NEVERD_JAVA21_HOME" \
    --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
    --work-dir "$RUNNER_TEMP/android-allocation-long-division-$variant" "${args[@]}"
done

profile_group "Verify inherited static constructor prefix fields" 25 1200
for variant in source sdk; do
  args=()
  if [[ "$variant" == sdk ]]; then args+=(--dependency-provider); fi
  python scripts/test_mobile_android_inherited_static_prefix.py \
    --neverd build-mobile/bin/neverd --d8 "$NEVERD_D8" \
    --java-home "$JAVA_HOME" --compiler-java-home "$NEVERD_JAVA21_HOME" \
    --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
    --work-dir "$RUNNER_TEMP/android-inherited-static-prefix-$variant" "${args[@]}"
done

profile_group "Verify nullary nested carrier factory results" 21 1200
for variant in source sdk; do
  args=()
  if [[ "$variant" == sdk ]]; then args+=(--dependency-provider); fi
  python scripts/test_mobile_android_nullary_nested_carrier.py \
    --neverd build-mobile/bin/neverd --d8 "$NEVERD_D8" \
    --java-home "$JAVA_HOME" --compiler-java-home "$NEVERD_JAVA21_HOME" \
    --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
    --work-dir "$RUNNER_TEMP/android-nullary-nested-carrier-$variant" "${args[@]}"
done

profile_group "Verify package synthetic names with retained owner formals" 25 1200
for variant in source sdk; do
  args=()
  if [[ "$variant" == sdk ]]; then args+=(--dependency-provider); fi
  python scripts/test_mobile_android_synthetic_instance_names.py --generic-owner \
    --neverd build-mobile/bin/neverd --d8 "$NEVERD_D8" \
    --java-home "$JAVA_HOME" \
    --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
    --work-dir "$RUNNER_TEMP/android-generic-package-identifiers-$variant" "${args[@]}"
done

profile_group "Verify allocated nested carrier constructor delegation" 25 1200
for variant in source sdk; do
  args=()
  if [[ "$variant" == sdk ]]; then args+=(--dependency-provider); fi
  python scripts/test_mobile_android_allocated_nested_carrier_delegation.py \
    --neverd build-mobile/bin/neverd --d8 "$NEVERD_D8" \
    --java-home "$JAVA_HOME" --compiler-java-home "$NEVERD_JAVA21_HOME" \
    --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
    --work-dir "$RUNNER_TEMP/android-allocated-nested-carrier-delegation-$variant" "${args[@]}"
done

profile_group "Verify allocated nested carriers in generic superclass calls" 25 1200
for variant in source sdk; do
  args=()
  if [[ "$variant" == sdk ]]; then args+=(--dependency-provider); fi
  python scripts/test_mobile_android_allocated_nested_carrier_delegation.py --superclass \
    --neverd build-mobile/bin/neverd --d8 "$NEVERD_D8" \
    --java-home "$JAVA_HOME" --compiler-java-home "$NEVERD_JAVA21_HOME" \
    --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
    --work-dir "$RUNNER_TEMP/android-allocated-nested-carrier-superclass-$variant" "${args[@]}"
done

profile_group "Verify direct generic seeds with closed declared exceptions" 21 1200
for variant in source sdk; do
  args=()
  if [[ "$variant" == sdk ]]; then args+=(--dependency-provider); fi
  python scripts/test_mobile_android_seed_context_throws.py \
    --neverd build-mobile/bin/neverd --d8 "$NEVERD_D8" \
    --java-home "$JAVA_HOME" --compiler-java-home "$NEVERD_JAVA21_HOME" \
    --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
    --work-dir "$RUNNER_TEMP/android-seed-context-throws-$variant" "${args[@]}"
done

profile_group "Verify recursive selector factories and standard comparators" 21 1200
for variant in source sdk; do
  args=()
  if [[ "$variant" == sdk ]]; then args+=(--dependency-provider); fi
  python scripts/test_mobile_android_recursive_selector_factory.py \
    --neverd build-mobile/bin/neverd --d8 "$NEVERD_D8" \
    --java-home "$JAVA_HOME" --compiler-java-home "$NEVERD_JAVA21_HOME" \
    --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
    --work-dir "$RUNNER_TEMP/android-recursive-selector-factory-$variant" "${args[@]}"
done

profile_group "Verify scoped interface token element factories" 21 1200
for variant in source sdk; do
  args=()
  if [[ "$variant" == sdk ]]; then args+=(--sdk-provider); fi
  python scripts/test_mobile_android_scoped_interface_token.py \
    --neverd build-mobile/bin/neverd --d8 "$NEVERD_D8" \
    --java-home "$JAVA_HOME" --compiler-java-home "$NEVERD_JAVA21_HOME" \
    --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
    --work-dir "$RUNNER_TEMP/android-scoped-interface-token-$variant" "${args[@]}"
done

profile_group "Verify reifiable result contexts with closed declared exceptions" 21 1200
for variant in source sdk; do
  args=()
  if [[ "$variant" == sdk ]]; then args+=(--dependency-provider); fi
  python scripts/test_mobile_android_reifiable_context_throws.py \
    --neverd build-mobile/bin/neverd --d8 "$NEVERD_D8" \
    --java-home "$JAVA_HOME" --compiler-java-home "$NEVERD_JAVA21_HOME" \
    --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
    --work-dir "$RUNNER_TEMP/android-reifiable-context-throws-$variant" "${args[@]}"
done

profile_group "Verify wildcard carrier slots with independent nominal results" 21 1200
for variant in source sdk; do
  args=()
  if [[ "$variant" == sdk ]]; then args+=(--dependency-provider); fi
  python scripts/test_mobile_android_wildcard_carrier_nominal.py \
    --neverd build-mobile/bin/neverd --d8 "$NEVERD_D8" \
    --java-home "$JAVA_HOME" --compiler-java-home "$NEVERD_JAVA21_HOME" \
    --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
    --work-dir "$RUNNER_TEMP/android-wildcard-carrier-nominal-$variant" "${args[@]}"
done

profile_group "Verify closed covariant contexts with independent generic results" 21 1200
for variant in source sdk; do
  args=()
  if [[ "$variant" == sdk ]]; then args+=(--dependency-provider); fi
  python scripts/test_mobile_android_closed_covariant_result_context.py \
    --neverd build-mobile/bin/neverd --d8 "$NEVERD_D8" \
    --java-home "$JAVA_HOME" --compiler-java-home "$NEVERD_JAVA21_HOME" \
    --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
    --work-dir "$RUNNER_TEMP/android-closed-covariant-result-context-$variant" "${args[@]}"
done

profile_group "Verify result-only arrays and original allocation casts" 21 1200
for variant in source sdk; do
  args=()
  if [[ "$variant" == sdk ]]; then args+=(--dependency-provider); fi
  python scripts/test_mobile_android_reifiable_array_result.py \
    --neverd build-mobile/bin/neverd --d8 "$NEVERD_D8" \
    --java-home "$JAVA_HOME" --compiler-java-home "$NEVERD_JAVA21_HOME" \
    --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
    --work-dir "$RUNNER_TEMP/android-reifiable-array-result-$variant" "${args[@]}"
done

profile_group "Verify scoped array consumers with actual array results" 21 1200
for variant in source sdk; do
  args=()
  if [[ "$variant" == sdk ]]; then args+=(--dependency-provider); fi
  python scripts/test_mobile_android_array_consumer_array_result.py \
    --neverd build-mobile/bin/neverd --d8 "$NEVERD_D8" \
    --java-home "$JAVA_HOME" --compiler-java-home "$NEVERD_JAVA21_HOME" \
    --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
    --work-dir "$RUNNER_TEMP/android-array-consumer-array-result-$variant" "${args[@]}"
done

profile_group "Verify closed carrier substitutions of parent constructor formals" 21 1200
for variant in source sdk; do
  args=()
  if [[ "$variant" == sdk ]]; then args+=(--dependency-provider); fi
  python scripts/test_mobile_android_closed_carrier_parent_formal.py \
    --neverd build-mobile/bin/neverd --d8 "$NEVERD_D8" \
    --java-home "$JAVA_HOME" --compiler-java-home "$NEVERD_JAVA21_HOME" \
    --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
    --work-dir "$RUNNER_TEMP/android-closed-carrier-parent-formal-$variant" "${args[@]}"
done

profile_group "Verify paired mapper bindings and opaque collector results" 21 1200
for variant in source sdk; do
  args=()
  if [[ "$variant" == sdk ]]; then args+=(--dependency-provider); fi
  python scripts/test_mobile_android_paired_mapper_collector.py \
    --neverd build-mobile/bin/neverd --d8 "$NEVERD_D8" \
    --java-home "$JAVA_HOME" --compiler-java-home "$NEVERD_JAVA21_HOME" \
    --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
    --work-dir "$RUNNER_TEMP/android-paired-mapper-collector-$variant" "${args[@]}"
done

profile_group "Verify closed generic interface final field initialization" 21 1200
for variant in source sdk; do
  args=()
  if [[ "$variant" == sdk ]]; then args+=(--dependency-provider); fi
  python scripts/test_mobile_android_generic_interface_final_field.py \
    --neverd build-mobile/bin/neverd --d8 "$NEVERD_D8" \
    --java-home "$JAVA_HOME" --compiler-java-home "$NEVERD_JAVA21_HOME" \
    --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
    --work-dir "$RUNNER_TEMP/android-generic-interface-final-field-$variant" "${args[@]}"
done

profile_group "Verify interface allocations with typed scalar constructor arguments" 21 1200
for variant in source sdk; do
  args=()
  if [[ "$variant" == sdk ]]; then args+=(--dependency-provider); fi
  python scripts/test_mobile_android_interface_constructor_arguments.py \
    --neverd build-mobile/bin/neverd --d8 "$NEVERD_D8" \
    --java-home "$JAVA_HOME" --compiler-java-home "$NEVERD_JAVA21_HOME" \
    --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
    --work-dir "$RUNNER_TEMP/android-interface-constructor-arguments-$variant" "${args[@]}"
done

profile_group "Verify fixed-arity paired generic array boolean calls" 21 1200
for variant in source sdk; do
  args=()
  if [[ "$variant" == sdk ]]; then args+=(--dependency-provider); fi
  python scripts/test_mobile_android_paired_array_comparison.py \
    --neverd build-mobile/bin/neverd --d8 "$NEVERD_D8" \
    --java-home "$JAVA_HOME" --compiler-java-home "$NEVERD_JAVA21_HOME" \
    --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
    --work-dir "$RUNNER_TEMP/android-paired-array-comparison-$variant" "${args[@]}"
done

profile_group "Verify direct generic value transfers and declared exceptions" 21 1200
for variant in source sdk; do
  args=()
  if [[ "$variant" == sdk ]]; then args+=(--dependency-provider); fi
  python scripts/test_mobile_android_direct_formal_transfer.py \
    --neverd build-mobile/bin/neverd --d8 "$NEVERD_D8" \
    --java-home "$JAVA_HOME" --compiler-java-home "$NEVERD_JAVA21_HOME" \
    --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
    --work-dir "$RUNNER_TEMP/android-direct-formal-transfer-$variant" "${args[@]}"
done

profile_group "Verify binary carrier and explicit generic array transfers" 21 1200
for variant in source sdk; do
  args=()
  if [[ "$variant" == sdk ]]; then args+=(--dependency-provider); fi
  python scripts/test_mobile_android_binary_carrier_array_transfer.py \
    --neverd build-mobile/bin/neverd --d8 "$NEVERD_D8" \
    --java-home "$JAVA_HOME" --compiler-java-home "$NEVERD_JAVA21_HOME" \
    --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
    --work-dir "$RUNNER_TEMP/android-binary-carrier-array-transfer-$variant" "${args[@]}"
done

profile_group "Verify scalar contexts and explicit final generic arrays" 21 1200
for variant in source sdk; do
  args=()
  if [[ "$variant" == sdk ]]; then args+=(--dependency-provider); fi
  python scripts/test_mobile_android_scalar_array_factory.py \
    --neverd build-mobile/bin/neverd --d8 "$NEVERD_D8" \
    --java-home "$JAVA_HOME" --compiler-java-home "$NEVERD_JAVA21_HOME" \
    --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
    --work-dir "$RUNNER_TEMP/android-scalar-array-factory-$variant" "${args[@]}"
done

profile_group "Verify direct generic seeds beside reifiable contexts" 21 1200
for variant in source sdk; do
  args=()
  if [[ "$variant" == sdk ]]; then args+=(--dependency-provider); fi
  python scripts/test_mobile_android_reifiable_seed_context.py \
    --neverd build-mobile/bin/neverd --d8 "$NEVERD_D8" \
    --java-home "$JAVA_HOME" --compiler-java-home "$NEVERD_JAVA21_HOME" \
    --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
    --work-dir "$RUNNER_TEMP/android-reifiable-seed-context-$variant" "${args[@]}"
done

profile_group "Verify invariant producers with mapped generic results" 21 1200
for variant in source sdk; do
  args=()
  if [[ "$variant" == sdk ]]; then args+=(--dependency-provider); fi
  python scripts/test_mobile_android_iterable_function_context.py \
    --neverd build-mobile/bin/neverd --d8 "$NEVERD_D8" \
    --java-home "$JAVA_HOME" --compiler-java-home "$NEVERD_JAVA21_HOME" \
    --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
    --work-dir "$RUNNER_TEMP/android-iterable-function-context-$variant" "${args[@]}"
done

profile_group "Verify scoped tokens and mapped array results" 21 1200
for variant in source sdk; do
  args=()
  if [[ "$variant" == sdk ]]; then args+=(--dependency-provider); fi
  python scripts/test_mobile_android_mapped_array_token.py \
    --neverd build-mobile/bin/neverd --d8 "$NEVERD_D8" \
    --java-home "$JAVA_HOME" --compiler-java-home "$NEVERD_JAVA21_HOME" \
    --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
    --work-dir "$RUNNER_TEMP/android-mapped-array-token-$variant" "${args[@]}"
done

profile_group "Verify direct seeds beside closed invariant contexts" 21 1200
for variant in source sdk; do
  args=()
  if [[ "$variant" == sdk ]]; then args+=(--dependency-provider); fi
  python scripts/test_mobile_android_closed_invariant_seed_context.py \
    --neverd build-mobile/bin/neverd --d8 "$NEVERD_D8" \
    --java-home "$JAVA_HOME" --compiler-java-home "$NEVERD_JAVA21_HOME" \
    --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
    --work-dir "$RUNNER_TEMP/android-closed-invariant-seed-context-$variant" "${args[@]}"
done

profile_group "Verify unchecked getters before constructor delegation" 25 1200
for variant in source sdk; do
  args=()
  if [[ "$variant" == sdk ]]; then args+=(--dependency-provider); fi
  python scripts/test_mobile_android_guarded_constructor_unchecked_getter.py \
    --neverd build-mobile/bin/neverd --d8 "$NEVERD_D8" \
    --java-home "$JAVA_HOME" --compiler-java-home "$NEVERD_JAVA21_HOME" \
    --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
    --work-dir "$RUNNER_TEMP/android-guarded-constructor-unchecked-getter-$variant" "${args[@]}"
done

profile_group "Verify Object views beside parameterized constructor siblings" 25 1200
for variant in source sdk; do
  args=()
  if [[ "$variant" == sdk ]]; then args+=(--dependency-provider); fi
  python scripts/test_mobile_android_guarded_constructor_unchecked_getter.py \
    --neverd build-mobile/bin/neverd --d8 "$NEVERD_D8" \
    --java-home "$JAVA_HOME" --compiler-java-home "$NEVERD_JAVA21_HOME" \
    --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
    --parameterized-sibling \
    --work-dir "$RUNNER_TEMP/android-constructor-object-overload-$variant" "${args[@]}"
done

profile_group "Verify public instance reifiable result bindings" 21 1200
for variant in source sdk; do
  args=()
  if [[ "$variant" == sdk ]]; then args+=(--dependency-provider); fi
  python scripts/test_mobile_android_instance_reifiable_result.py \
    --neverd build-mobile/bin/neverd --d8 "$NEVERD_D8" \
    --java-home "$JAVA_HOME" --compiler-java-home "$NEVERD_JAVA21_HOME" \
    --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
    --work-dir "$RUNNER_TEMP/android-instance-reifiable-result-$variant" "${args[@]}"
done

profile_group "Verify raw nominal superclass constructor arguments" 21 1200
for variant in source sdk; do
  args=()
  if [[ "$variant" == sdk ]]; then args+=(--dependency-provider); fi
  python scripts/test_mobile_android_raw_nominal_superclass_constructor.py \
    --neverd build-mobile/bin/neverd --d8 "$NEVERD_D8" \
    --java-home "$JAVA_HOME" --compiler-java-home "$NEVERD_JAVA21_HOME" \
    --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
    --work-dir "$RUNNER_TEMP/android-raw-nominal-superclass-constructor-$variant" "${args[@]}"
done

profile_group "Verify unchecked wildcard carrier nominal results" 21 1200
for variant in source sdk; do
  args=(--declared-unchecked)
  if [[ "$variant" == sdk ]]; then args+=(--dependency-provider); fi
  python scripts/test_mobile_android_wildcard_carrier_nominal.py \
    --neverd build-mobile/bin/neverd --d8 "$NEVERD_D8" \
    --java-home "$JAVA_HOME" --compiler-java-home "$NEVERD_JAVA21_HOME" \
    --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
    --work-dir "$RUNNER_TEMP/android-unchecked-wildcard-carrier-nominal-$variant" "${args[@]}"
done

profile_group "Verify array sources in mapped carrier bindings" 21 1200
for variant in source sdk; do
  args=()
  if [[ "$variant" == sdk ]]; then args+=(--dependency-provider); fi
  python scripts/test_mobile_android_array_mapped_carrier.py \
    --neverd build-mobile/bin/neverd --d8 "$NEVERD_D8" \
    --java-home "$JAVA_HOME" --compiler-java-home "$NEVERD_JAVA21_HOME" \
    --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
    --work-dir "$RUNNER_TEMP/android-array-mapped-carrier-$variant" "${args[@]}"
done


profile_group "Verify closed wildcard superclass substitutions" 21 1200
for variance in any closed; do
  for variant in source sdk; do
    args=(--wildcard-target "$variance")
    if [[ "$variant" == sdk ]]; then args+=(--dependency-provider); fi
    python scripts/test_mobile_android_raw_nominal_superclass_constructor.py \
      --neverd build-mobile/bin/neverd --d8 "$NEVERD_D8" \
      --java-home "$JAVA_HOME" --compiler-java-home "$NEVERD_JAVA21_HOME" \
      --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
      --work-dir "$RUNNER_TEMP/android-wildcard-superclass-constructor-$variance-$variant" "${args[@]}"
  done
done


profile_group "Verify repeated invariant inputs with array results" 21 1200
for variant in source sdk; do
  args=()
  if [[ "$variant" == sdk ]]; then args+=(--dependency-provider); fi
  python scripts/test_mobile_android_repeated_array_tokens.py \
    --neverd build-mobile/bin/neverd --d8 "$NEVERD_D8" \
    --java-home "$JAVA_HOME" --compiler-java-home "$NEVERD_JAVA21_HOME" \
    --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
    --work-dir "$RUNNER_TEMP/android-repeated-array-tokens-$variant" "${args[@]}"
done


profile_group "Verify direct seeds with unbounded three-slot results" 21 1200
for variant in source sdk; do
  args=()
  if [[ "$variant" == sdk ]]; then args+=(--dependency-provider); fi
  python scripts/test_mobile_android_seeded_reducer_carrier.py \
    --neverd build-mobile/bin/neverd --d8 "$NEVERD_D8" \
    --java-home "$JAVA_HOME" --compiler-java-home "$NEVERD_JAVA21_HOME" \
    --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
    --work-dir "$RUNNER_TEMP/android-seeded-reducer-carrier-$variant" "${args[@]}"
done


profile_group "Verify complete mapper and downstream carrier bindings" 21 1200
for variant in source sdk; do
  args=()
  if [[ "$variant" == sdk ]]; then args+=(--dependency-provider); fi
  python scripts/test_mobile_android_mapped_collector_carrier.py \
    --neverd build-mobile/bin/neverd --d8 "$NEVERD_D8" \
    --java-home "$JAVA_HOME" --compiler-java-home "$NEVERD_JAVA21_HOME" \
    --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
    --work-dir "$RUNNER_TEMP/android-mapped-collector-carrier-$variant" "${args[@]}"
done


profile_group "Verify classifier and nested result carrier bindings" 21 1200
for variant in source sdk; do
  args=()
  if [[ "$variant" == sdk ]]; then args+=(--dependency-provider); fi
  python scripts/test_mobile_android_grouped_collector_carrier.py \
    --neverd build-mobile/bin/neverd --d8 "$NEVERD_D8" \
    --java-home "$JAVA_HOME" --compiler-java-home "$NEVERD_JAVA21_HOME" \
    --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
    --work-dir "$RUNNER_TEMP/android-grouped-collector-carrier-$variant" "${args[@]}"
done


profile_group "Verify Object constructor views beside generic arrays" 21 1200
for variant in source sdk; do
  args=()
  if [[ "$variant" == sdk ]]; then args+=(--dependency-provider); fi
  python scripts/test_mobile_android_object_array_constructor.py \
    --neverd build-mobile/bin/neverd --d8 "$NEVERD_D8" \
    --java-home "$JAVA_HOME" --compiler-java-home "$NEVERD_JAVA21_HOME" \
    --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
    --work-dir "$RUNNER_TEMP/android-object-array-constructor-$variant" "${args[@]}"
done


profile_group "Verify guarded integer constructor division" 25 1200
for variant in source sdk; do
  args=()
  if [[ "$variant" == sdk ]]; then args+=(--dependency-provider); fi
  python scripts/test_mobile_android_guarded_integer_division.py \
    --neverd build-mobile/bin/neverd --d8 "$NEVERD_D8" \
    --java-home "$JAVA_HOME" --compiler-java-home "$NEVERD_JAVA21_HOME" \
    --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
    --work-dir "$RUNNER_TEMP/android-guarded-integer-division-$variant" "${args[@]}"
done


profile_group "Verify class scoped null constructor carriers" 25 1200
for variant in source sdk; do
  args=()
  if [[ "$variant" == sdk ]]; then args+=(--dependency-provider); fi
  python scripts/test_mobile_android_scoped_null_constructor.py \
    --neverd build-mobile/bin/neverd --d8 "$NEVERD_D8" \
    --java-home "$JAVA_HOME" --compiler-java-home "$NEVERD_JAVA21_HOME" \
    --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
    --work-dir "$RUNNER_TEMP/android-scoped-null-constructor-$variant" "${args[@]}"
done


profile_group "Verify raw constructor carrier allocation" 25 1200
for variant in source sdk; do
  args=()
  if [[ "$variant" == sdk ]]; then args+=(--dependency-provider); fi
  python scripts/test_mobile_android_raw_binary_carrier_allocation.py \
    --neverd build-mobile/bin/neverd --d8 "$NEVERD_D8" \
    --java-home "$JAVA_HOME" --compiler-java-home "$NEVERD_JAVA21_HOME" \
    --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
    --work-dir "$RUNNER_TEMP/android-raw-binary-carrier-allocation-$variant" "${args[@]}"
done


profile_group "Verify keyed map carrier relations" 21 1200
for variant in source sdk; do
  args=()
  if [[ "$variant" == sdk ]]; then args+=(--dependency-provider); fi
  python scripts/test_mobile_android_keyed_map_carrier.py \
    --neverd build-mobile/bin/neverd --d8 "$NEVERD_D8" \
    --java-home "$JAVA_HOME" --compiler-java-home "$NEVERD_JAVA21_HOME" \
    --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
    --work-dir "$RUNNER_TEMP/android-keyed-map-carrier-$variant" "${args[@]}"
done


profile_group "Verify nullary binary method formals" 21 1200
for variant in source sdk; do
  args=()
  if [[ "$variant" == sdk ]]; then args+=(--dependency-provider); fi
  python scripts/test_mobile_android_nullary_binary_formal.py \
    --neverd build-mobile/bin/neverd --d8 "$NEVERD_D8" \
    --java-home "$JAVA_HOME" --compiler-java-home "$NEVERD_JAVA21_HOME" \
    --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
    --work-dir "$RUNNER_TEMP/android-nullary-binary-formal-$variant" "${args[@]}"
done


profile_group "Verify mapped value merge carriers" 21 1200
for variant in source sdk; do
  args=()
  if [[ "$variant" == sdk ]]; then args+=(--dependency-provider); fi
  python scripts/test_mobile_android_merged_mapper_collector.py \
    --neverd build-mobile/bin/neverd --d8 "$NEVERD_D8" \
    --java-home "$JAVA_HOME" --compiler-java-home "$NEVERD_JAVA21_HOME" \
    --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
    --work-dir "$RUNNER_TEMP/android-merged-mapper-collector-$variant" "${args[@]}"
done


profile_group "Verify collection downstream map context" 21 1200
for variant in source sdk; do
  args=()
  if [[ "$variant" == sdk ]]; then args+=(--dependency-provider); fi
  python scripts/test_mobile_android_grouped_map_context.py \
    --neverd build-mobile/bin/neverd --d8 "$NEVERD_D8" \
    --java-home "$JAVA_HOME" --compiler-java-home "$NEVERD_JAVA21_HOME" \
    --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
    --work-dir "$RUNNER_TEMP/android-grouped-map-context-$variant" "${args[@]}"
done


profile_group "Verify classified map contexts" 21 1200
for variant in source sdk; do
  args=()
  if [[ "$variant" == sdk ]]; then args+=(--dependency-provider); fi
  python scripts/test_mobile_android_classified_map_context.py \
    --neverd build-mobile/bin/neverd --d8 "$NEVERD_D8" \
    --java-home "$JAVA_HOME" --compiler-java-home "$NEVERD_JAVA21_HOME" \
    --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
    --work-dir "$RUNNER_TEMP/android-classified-map-context-$variant" "${args[@]}"
done


profile_group "Verify iterator closed nominal map contexts" 21 1200
for variant in source sdk; do
  args=()
  if [[ "$variant" == sdk ]]; then args+=(--dependency-provider); fi
  python scripts/test_mobile_android_iterator_map_context.py \
    --neverd build-mobile/bin/neverd --d8 "$NEVERD_D8" \
    --java-home "$JAVA_HOME" --compiler-java-home "$NEVERD_JAVA21_HOME" \
    --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
    --work-dir "$RUNNER_TEMP/android-iterator-map-context-$variant" "${args[@]}"
done


profile_group "Verify recursive Enum type token factories" 21 1200
for variant in source sdk; do
  args=()
  if [[ "$variant" == sdk ]]; then args+=(--dependency-provider); fi
  python scripts/test_mobile_android_enum_set_token_factory.py \
    --neverd build-mobile/bin/neverd --d8 "$NEVERD_D8" \
    --java-home "$JAVA_HOME" --compiler-java-home "$NEVERD_JAVA21_HOME" \
    --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
    --work-dir "$RUNNER_TEMP/android-enum-set-token-factory-$variant" "${args[@]}"
done


profile_group "Verify unbound pair results with fixed contexts" 21 1200
for variant in source sdk; do
  args=()
  if [[ "$variant" == sdk ]]; then args+=(--dependency-provider); fi
  python scripts/test_mobile_android_unbound_pair_context.py \
    --neverd build-mobile/bin/neverd --d8 "$NEVERD_D8" \
    --java-home "$JAVA_HOME" --compiler-java-home "$NEVERD_JAVA21_HOME" \
    --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
    --work-dir "$RUNNER_TEMP/android-unbound-pair-context-$variant" "${args[@]}"
done


profile_group "Verify partially anchored result contexts" 21 1200
for variant in source sdk; do
  args=()
  if [[ "$variant" == sdk ]]; then args+=(--dependency-provider); fi
  python scripts/test_mobile_android_iterator_result_context.py \
    --neverd build-mobile/bin/neverd --d8 "$NEVERD_D8" \
    --java-home "$JAVA_HOME" --compiler-java-home "$NEVERD_JAVA21_HOME" \
    --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
    --work-dir "$RUNNER_TEMP/android-iterator-result-context-$variant" "${args[@]}"
done


profile_group "Verify dependent interface callback transfers" 21 1200
for variant in source sdk; do
  args=()
  if [[ "$variant" == sdk ]]; then args+=(--dependency-provider); fi
  python scripts/test_mobile_android_iterable_callback_forwarder.py \
    --neverd build-mobile/bin/neverd --d8 "$NEVERD_D8" \
    --java-home "$JAVA_HOME" --compiler-java-home "$NEVERD_JAVA21_HOME" \
    --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
    --work-dir "$RUNNER_TEMP/android-iterable-callback-forwarder-$variant" "${args[@]}"
done


profile_group "Verify indexed iterator results with declared exceptions" 21 1200
for variant in source sdk; do
  args=()
  if [[ "$variant" == sdk ]]; then args+=(--dependency-provider); fi
  python scripts/test_mobile_android_iterator_indexed_result.py \
    --neverd build-mobile/bin/neverd --d8 "$NEVERD_D8" \
    --java-home "$JAVA_HOME" --compiler-java-home "$NEVERD_JAVA21_HOME" \
    --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
    --work-dir "$RUNNER_TEMP/android-iterator-indexed-result-$variant" "${args[@]}"
done


profile_group "Verify nested collection results with classifiers" 21 1200
for variant in source sdk; do
  args=()
  if [[ "$variant" == sdk ]]; then args+=(--dependency-provider); fi
  python scripts/test_mobile_android_collection_list_mapper.py \
    --neverd build-mobile/bin/neverd --d8 "$NEVERD_D8" \
    --java-home "$JAVA_HOME" --compiler-java-home "$NEVERD_JAVA21_HOME" \
    --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
    --work-dir "$RUNNER_TEMP/android-collection-list-mapper-$variant" "${args[@]}"
done


profile_group "Verify Object-erased recursive Comparable selectors" 21 1200
for variant in source sdk; do
  args=()
  if [[ "$variant" == sdk ]]; then args+=(--dependency-provider); fi
  python scripts/test_mobile_android_recursive_comparable_extrema.py \
    --neverd build-mobile/bin/neverd --d8 "$NEVERD_D8" \
    --java-home "$JAVA_HOME" --compiler-java-home "$NEVERD_JAVA21_HOME" \
    --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
    --work-dir "$RUNNER_TEMP/android-recursive-comparable-extrema-$variant" "${args[@]}"
done
profile_group "Verify repeated collection seeds with lower callbacks" 21 1200
for variant in source sdk; do
  args=()
  if [[ "$variant" == sdk ]]; then args+=(--dependency-provider); fi
  python scripts/test_mobile_android_collection_filter_collector.py \
    --neverd build-mobile/bin/neverd --d8 "$NEVERD_D8" \
    --java-home "$JAVA_HOME" --compiler-java-home "$NEVERD_JAVA21_HOME" \
    --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
    --work-dir "$RUNNER_TEMP/android-collection-filter-collector-$variant" "${args[@]}"
done


profile_group "Verify nested pair comparator and result bindings" 21 1200
for variant in source sdk; do
  args=()
  if [[ "$variant" == sdk ]]; then args+=(--dependency-provider); fi
  python scripts/test_mobile_android_sorted_entry_map.py \
    --neverd build-mobile/bin/neverd --d8 "$NEVERD_D8" \
    --java-home "$JAVA_HOME" --compiler-java-home "$NEVERD_JAVA21_HOME" \
    --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
    --work-dir "$RUNNER_TEMP/android-sorted-entry-map-$variant" "${args[@]}"
done


profile_group "Verify invariant nested list results with scalar contexts" 21 1200
for variant in source sdk; do
  args=()
  if [[ "$variant" == sdk ]]; then args+=(--dependency-provider); fi
  python scripts/test_mobile_android_indexed_list_transfer.py \
    --neverd build-mobile/bin/neverd --d8 "$NEVERD_D8" \
    --java-home "$JAVA_HOME" --compiler-java-home "$NEVERD_JAVA21_HOME" \
    --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
    --work-dir "$RUNNER_TEMP/android-indexed-list-transfer-$variant" "${args[@]}"
done


profile_group "Verify iterable pair inputs with nested result slots" 21 1200
for variant in source sdk; do
  args=()
  if [[ "$variant" == sdk ]]; then args+=(--dependency-provider); fi
  python scripts/test_mobile_android_iterable_map_result.py \
    --neverd build-mobile/bin/neverd --d8 "$NEVERD_D8" \
    --java-home "$JAVA_HOME" --compiler-java-home "$NEVERD_JAVA21_HOME" \
    --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
    --work-dir "$RUNNER_TEMP/android-iterable-map-result-$variant" "${args[@]}"
done


profile_group "Verify iterator map seeds and key/value callbacks" 21 1200
for variant in source sdk; do
  args=()
  if [[ "$variant" == sdk ]]; then args+=(--dependency-provider); fi
  python scripts/test_mobile_android_iterator_map_callback.py \
    --neverd build-mobile/bin/neverd --d8 "$NEVERD_D8" \
    --java-home "$JAVA_HOME" --compiler-java-home "$NEVERD_JAVA21_HOME" \
    --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
    --work-dir "$RUNNER_TEMP/android-iterator-map-callback-$variant" "${args[@]}"
done


profile_group "Verify nested map inputs and unary pair results" 21 1200
for variant in source sdk; do
  args=()
  if [[ "$variant" == sdk ]]; then args+=(--dependency-provider); fi
  python scripts/test_mobile_android_map_unary_result.py \
    --neverd build-mobile/bin/neverd --d8 "$NEVERD_D8" \
    --java-home "$JAVA_HOME" --compiler-java-home "$NEVERD_JAVA21_HOME" \
    --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
    --work-dir "$RUNNER_TEMP/android-map-unary-result-$variant" "${args[@]}"
done


profile_group "Verify paired arrays and ordered map results" 21 1200
for variant in source sdk; do
  args=()
  if [[ "$variant" == sdk ]]; then args+=(--dependency-provider); fi
  python scripts/test_mobile_android_paired_array_map.py \
    --neverd build-mobile/bin/neverd --d8 "$NEVERD_D8" \
    --java-home "$JAVA_HOME" --compiler-java-home "$NEVERD_JAVA21_HOME" \
    --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
    --work-dir "$RUNNER_TEMP/android-paired-array-map-$variant" "${args[@]}"
done


profile_group "Verify committed final fields before branch-tail loops" 21 1200
for variant in source sdk; do
  args=()
  if [[ "$variant" == sdk ]]; then args+=(--dependency-provider); fi
  python scripts/test_mobile_android_tail_loop_final.py \
    --neverd build-mobile/bin/neverd --d8 "$NEVERD_D8" \
    --java-home "$JAVA_HOME" --compiler-java-home "$NEVERD_JAVA21_HOME" \
    --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
    --work-dir "$RUNNER_TEMP/android-tail-loop-final-$variant" "${args[@]}"
done


profile_group "Verify complete SDK class covariance in inherited contexts" 21 1200
for variant in source sdk; do
  args=()
  if [[ "$variant" == sdk ]]; then args+=(--dependency-provider); fi
  python scripts/test_mobile_android_sdk_covariant_inheritance.py \
    --neverd build-mobile/bin/neverd --d8 "$NEVERD_D8" \
    --java-home "$JAVA_HOME" --compiler-java-home "$NEVERD_JAVA21_HOME" \
    --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
    --work-dir "$RUNNER_TEMP/android-sdk-covariant-inheritance-$variant" "${args[@]}"
done


profile_group "Verify invariant inputs beside closed list results and contexts" 21 1200
for variant in source sdk; do
  args=()
  if [[ "$variant" == sdk ]]; then args+=(--dependency-provider); fi
  python scripts/test_mobile_android_iterator_closed_list_context.py \
    --neverd build-mobile/bin/neverd --d8 "$NEVERD_D8" \
    --java-home "$JAVA_HOME" --compiler-java-home "$NEVERD_JAVA21_HOME" \
    --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
    --work-dir "$RUNNER_TEMP/android-iterator-closed-list-context-$variant" "${args[@]}"
done


profile_group "Verify map-first nested seeds and iterable callback scopes" 21 1200
for variant in source sdk; do
  args=()
  if [[ "$variant" == sdk ]]; then args+=(--dependency-provider); fi
  python scripts/test_mobile_android_seeded_iterable_map.py \
    --neverd build-mobile/bin/neverd --d8 "$NEVERD_D8" \
    --java-home "$JAVA_HOME" --compiler-java-home "$NEVERD_JAVA21_HOME" \
    --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
    --work-dir "$RUNNER_TEMP/android-seeded-iterable-map-$variant" "${args[@]}"
done


profile_group "Verify flag/capacity contexts beside independent pair results" 21 1200
for variant in source sdk; do
  args=()
  if [[ "$variant" == sdk ]]; then args+=(--dependency-provider); fi
  python scripts/test_mobile_android_flag_pair_factory.py \
    --neverd build-mobile/bin/neverd --d8 "$NEVERD_D8" \
    --java-home "$JAVA_HOME" --compiler-java-home "$NEVERD_JAVA21_HOME" \
    --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
    --work-dir "$RUNNER_TEMP/android-flag-pair-factory-$variant" "${args[@]}"
done

profile_group "Verify complete carriers beside opaque primitive-array results" 21 1200
for variant in source sdk; do
  args=()
  if [[ "$variant" == sdk ]]; then args+=(--dependency-provider); fi
  python scripts/test_mobile_android_collection_primitive_result.py \
    --neverd build-mobile/bin/neverd --d8 "$NEVERD_D8" \
    --java-home "$JAVA_HOME" --compiler-java-home "$NEVERD_JAVA21_HOME" \
    --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
    --work-dir "$RUNNER_TEMP/android-collection-primitive-result-$variant" "${args[@]}"
done

profile_group "Verify generic constructor choices with independent collection inputs" 21 1200
for variant in source sdk; do
  args=()
  if [[ "$variant" == sdk ]]; then args+=(--dependency-widget); fi
  python scripts/test_mobile_android_collection_constructor_choice.py \
    --neverd build-mobile/bin/neverd --d8 "$NEVERD_D8" \
    --java-home "$JAVA_HOME" \
    --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
    --work-dir "$RUNNER_TEMP/android-collection-constructor-choice-$variant" "${args[@]}"
done

profile_group "Verify complete pair transfers with independently shadowed static formals" 21 1200
for variant in source sdk; do
  args=()
  if [[ "$variant" == sdk ]]; then args+=(--dependency-provider); fi
  python scripts/test_mobile_android_scoped_pair_transfer.py \
    --neverd build-mobile/bin/neverd --d8 "$NEVERD_D8" \
    --java-home "$JAVA_HOME" \
    --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
    --work-dir "$RUNNER_TEMP/android-scoped-pair-transfer-$variant" "${args[@]}"
done

profile_group "Verify configured allocations and pair transfers before constructor delegation" 25 1200
for variant in source sdk; do
  args=()
  if [[ "$variant" == sdk ]]; then args+=(--dependency-provider); fi
  python scripts/test_mobile_android_pair_constructor_transfer.py \
    --neverd build-mobile/bin/neverd --d8 "$NEVERD_D8" \
    --java-home "$JAVA_HOME" \
    --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
    --work-dir "$RUNNER_TEMP/android-pair-constructor-transfer-$variant" "${args[@]}"
done

profile_group "Verify boolean defaults before scoped constructor delegation" 21 1200
for variant in source sdk; do
  args=()
  if [[ "$variant" == sdk ]]; then args+=(--dependency-callback); fi
  python scripts/test_mobile_android_boolean_constructor_default.py \
    --neverd build-mobile/bin/neverd --d8 "$NEVERD_D8" \
    --java-home "$JAVA_HOME" \
    --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
    --work-dir "$RUNNER_TEMP/android-boolean-constructor-default-$variant" "${args[@]}"
done

profile_group "Verify scalar pair factories with independent static formals" 21 1200
for variant in source sdk; do
  args=()
  if [[ "$variant" == sdk ]]; then args+=(--dependency-provider); fi
  python scripts/test_mobile_android_scoped_scalar_pair_factory.py \
    --neverd build-mobile/bin/neverd --d8 "$NEVERD_D8" \
    --java-home "$JAVA_HOME" \
    --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
    --work-dir "$RUNNER_TEMP/android-scoped-scalar-pair-factory-$variant" "${args[@]}"
done

profile_group "Verify scalar factory contracts before typed constructor delegation" 25 1200
for variant in source sdk; do
  args=()
  if [[ "$variant" == sdk ]]; then args+=(--dependency-provider); fi
  python scripts/test_mobile_android_scalar_factory_constructor.py \
    --neverd build-mobile/bin/neverd --d8 "$NEVERD_D8" \
    --java-home "$JAVA_HOME" \
    --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
    --work-dir "$RUNNER_TEMP/android-scalar-factory-constructor-$variant" "${args[@]}"
done

profile_group "Verify ancestor superclass calls retain original native dispatch" 21 1200
for variant in source sdk; do
  args=()
  if [[ "$variant" == sdk ]]; then args+=(--dependency-base); fi
  python scripts/test_mobile_android_ancestor_super_call.py \
    --neverd build-mobile/bin/neverd --d8 "$NEVERD_D8" \
    --java-home "$JAVA_HOME" \
    --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
    --work-dir "$RUNNER_TEMP/android-ancestor-super-call-$variant" "${args[@]}"
done

profile_group "Verify direct generic pairs retain opaque integer results" 21 1200
for variant in source sdk; do
  args=()
  if [[ "$variant" == sdk ]]; then args+=(--dependency-provider); fi
  python scripts/test_mobile_android_object_pair_scalar_result.py \
    --neverd build-mobile/bin/neverd --d8 "$NEVERD_D8" \
    --java-home "$JAVA_HOME" \
    --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
    --work-dir "$RUNNER_TEMP/android-object-pair-scalar-result-$variant" "${args[@]}"
done

profile_group "Verify allocated contexts beside scoped constructor carriers" 25 1200
for variant in source sdk; do
  args=()
  if [[ "$variant" == sdk ]]; then args+=(--dependency-provider); fi
  python scripts/test_mobile_android_allocated_constructor_context.py \
    --neverd build-mobile/bin/neverd --d8 "$NEVERD_D8" \
    --java-home "$JAVA_HOME" \
    --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
    --work-dir "$RUNNER_TEMP/android-allocated-constructor-context-$variant" "${args[@]}"
done

profile_group "Verify computed primitive allocation arguments before typed delegation" 25 1200
for variant in source sdk; do
  args=()
  if [[ "$variant" == sdk ]]; then args+=(--dependency-provider); fi
  python scripts/test_mobile_android_computed_scalar_allocation.py \
    --neverd build-mobile/bin/neverd --d8 "$NEVERD_D8" \
    --java-home "$JAVA_HOME" \
    --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
    --work-dir "$RUNNER_TEMP/android-computed-scalar-allocation-$variant" "${args[@]}"
done

profile_group "Verify nested carrier inputs and independent static self factories" 21 1200
for variant in source sdk; do
  args=()
  if [[ "$variant" == sdk ]]; then args+=(--dependency-provider); fi
  python scripts/test_mobile_android_nested_carrier_factory.py \
    --neverd build-mobile/bin/neverd --d8 "$NEVERD_D8" \
    --java-home "$JAVA_HOME" \
    --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
    --work-dir "$RUNNER_TEMP/android-nested-carrier-factory-$variant" "${args[@]}"
done

profile_group "Verify recursive comparable pairs retain boolean context and opaque integers" 21 1200
for variant in source sdk; do
  args=()
  if [[ "$variant" == sdk ]]; then args+=(--dependency-provider); fi
  python scripts/test_mobile_android_bounded_pair_scalar.py \
    --neverd build-mobile/bin/neverd --d8 "$NEVERD_D8" \
    --java-home "$JAVA_HOME" \
    --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
    --work-dir "$RUNNER_TEMP/android-bounded-pair-scalar-$variant" "${args[@]}"
done

profile_group "Verify callback output arrays and invariant input results" 21 1200
for variant in source sdk; do
  args=()
  if [[ "$variant" == sdk ]]; then args+=(--dependency-provider); fi
  python scripts/test_mobile_android_callback_array_factory.py \
    --neverd build-mobile/bin/neverd --d8 "$NEVERD_D8" \
    --java-home "$JAVA_HOME" \
    --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
    --work-dir "$RUNNER_TEMP/android-callback-array-factory-$variant" "${args[@]}"
done

profile_group "Verify scoped variadic constructors keep exact fixed array views" 25 1200
for variant in source sdk; do
  args=()
  if [[ "$variant" == sdk ]]; then args+=(--dependency-provider); fi
  python scripts/test_mobile_android_typed_variadic_constructor.py \
    --neverd build-mobile/bin/neverd --d8 "$NEVERD_D8" \
    --java-home "$JAVA_HOME" \
    --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
    --work-dir "$RUNNER_TEMP/android-typed-variadic-constructor-$variant" "${args[@]}"
done

profile_group "Verify scoped nominal captures before superclass construction" 25 1200
for variant in source sdk; do
  args=()
  if [[ "$variant" == sdk ]]; then args+=(--dependency-provider); fi
  python scripts/test_mobile_android_scoped_capture_super.py \
    --neverd build-mobile/bin/neverd --d8 "$NEVERD_D8" \
    --java-home "$JAVA_HOME" \
    --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
    --work-dir "$RUNNER_TEMP/android-scoped-capture-super-$variant" "${args[@]}"
done

profile_group "Verify literal null keeps complete scoped superclass contracts" 25 1200
for variant in source sdk; do
  args=()
  if [[ "$variant" == sdk ]]; then args+=(--dependency-provider); fi
  python scripts/test_mobile_android_scoped_null_super.py \
    --neverd build-mobile/bin/neverd --d8 "$NEVERD_D8" \
    --java-home "$JAVA_HOME" \
    --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
    --work-dir "$RUNNER_TEMP/android-scoped-null-super-$variant" "${args[@]}"
done

profile_group "Verify generic arrays and Object inputs retain opaque int results" 25 1200
for variant in source sdk; do
  args=()
  if [[ "$variant" == sdk ]]; then args+=(--dependency-provider); fi
  python scripts/test_mobile_android_array_object_scalar.py \
    --neverd build-mobile/bin/neverd --d8 "$NEVERD_D8" \
    --java-home "$JAVA_HOME" \
    --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
    --work-dir "$RUNNER_TEMP/android-array-object-scalar-$variant" "${args[@]}"
done

profile_group "Verify descriptor context preserves complete closed parent bounds" 25 1200
for variant in source sdk; do
  args=()
  if [[ "$variant" == sdk ]]; then args+=(--dependency-provider); fi
  python scripts/test_mobile_android_closed_bound_super.py \
    --neverd build-mobile/bin/neverd --d8 "$NEVERD_D8" \
    --java-home "$JAVA_HOME" \
    --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
    --work-dir "$RUNNER_TEMP/android-closed-bound-super-$variant" "${args[@]}"
done

profile_group "Verify instance method formals preserve context and exception handlers" 25 1200
for variant in source sdk; do
  args=()
  if [[ "$variant" == sdk ]]; then args+=(--dependency-provider); fi
  python scripts/test_mobile_android_instance_formal_context.py \
    --neverd build-mobile/bin/neverd --d8 "$NEVERD_D8" \
    --java-home "$JAVA_HOME" \
    --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
    --work-dir "$RUNNER_TEMP/android-instance-formal-context-$variant" "${args[@]}"
done

profile_group "Verify Enum self bounds retain tokens defaults and opaque results" 25 1200
for variant in source sdk; do
  args=()
  if [[ "$variant" == sdk ]]; then args+=(--dependency-provider); fi
  python scripts/test_mobile_android_enum_token_default.py \
    --neverd build-mobile/bin/neverd --d8 "$NEVERD_D8" \
    --java-home "$JAVA_HOME" \
    --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
    --work-dir "$RUNNER_TEMP/android-enum-token-default-$variant" "${args[@]}"
done

profile_group "Verify paired tokens keep independent raw result class contexts" 25 1200
for variant in source sdk; do
  args=()
  if [[ "$variant" == sdk ]]; then args+=(--dependency-provider); fi
  python scripts/test_mobile_android_paired_raw_token_context.py \
    --neverd build-mobile/bin/neverd --d8 "$NEVERD_D8" \
    --java-home "$JAVA_HOME" \
    --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
    --work-dir "$RUNNER_TEMP/android-paired-raw-token-context-$variant" "${args[@]}"
done

profile_group "Verify Object formals retain opaque primitive array results" 25 1200
for variant in source sdk; do
  args=()
  if [[ "$variant" == sdk ]]; then args+=(--dependency-provider); fi
  python scripts/test_mobile_android_object_primitive_array.py \
    --neverd build-mobile/bin/neverd --d8 "$NEVERD_D8" \
    --java-home "$JAVA_HOME" \
    --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
    --work-dir "$RUNNER_TEMP/android-object-primitive-array-$variant" "${args[@]}"
done

profile_group "Verify recursive Enum tokens retain independent int contexts" 25 1200
for variant in source sdk; do
  args=()
  if [[ "$variant" == sdk ]]; then args+=(--dependency-provider); fi
  python scripts/test_mobile_android_enum_token_scalar.py \
    --neverd build-mobile/bin/neverd --d8 "$NEVERD_D8" \
    --java-home "$JAVA_HOME" \
    --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
    --work-dir "$RUNNER_TEMP/android-enum-token-scalar-$variant" "${args[@]}"
done

for variant in source sdk; do
  args=()
  if [[ "$variant" == sdk ]]; then args+=(--dependency-provider); fi
  python scripts/test_mobile_android_enum_closed_context.py \
    --neverd build-mobile/bin/neverd --d8 "$NEVERD_D8" \
    --java-home "$JAVA_HOME" \
    --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
    --work-dir "$RUNNER_TEMP/android-enum-closed-context-$variant" "${args[@]}"
done

for variant in source sdk; do
  args=()
  if [[ "$variant" == sdk ]]; then args+=(--dependency-provider); fi
  python scripts/test_mobile_android_bounded_token_primitive_array.py \
    --neverd build-mobile/bin/neverd --d8 "$NEVERD_D8" \
    --java-home "$JAVA_HOME" \
    --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
    --work-dir "$RUNNER_TEMP/android-bounded-token-primitive-array-$variant" "${args[@]}"
done

for variant in source sdk; do
  args=()
  if [[ "$variant" == sdk ]]; then args+=(--dependency-provider); fi
  python scripts/test_mobile_android_inherited_protected_allocation.py \
    --neverd build-mobile/bin/neverd --d8 "$NEVERD_D8" \
    --java-home "$JAVA_HOME" \
    --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
    --work-dir "$RUNNER_TEMP/android-inherited-protected-allocation-$variant" "${args[@]}"
done

for variant in source sdk; do
  args=()
  if [[ "$variant" == sdk ]]; then args+=(--dependency-provider); fi
  python scripts/test_mobile_android_constructor_static_nominal.py \
    --neverd build-mobile/bin/neverd --d8 "$NEVERD_D8" \
    --java-home "$JAVA_HOME" \
    --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
    --work-dir "$RUNNER_TEMP/android-constructor-static-nominal-$variant" "${args[@]}"
done

for variant in source sdk; do
  args=()
  if [[ "$variant" == sdk ]]; then args+=(--dependency-provider); fi
  python scripts/test_mobile_android_interface_seed_varargs.py \
    --neverd build-mobile/bin/neverd --d8 "$NEVERD_D8" \
    --java-home "$JAVA_HOME" \
    --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
    --work-dir "$RUNNER_TEMP/android-interface-seed-varargs-$variant" "${args[@]}"
done

for variant in source sdk; do
  args=()
  if [[ "$variant" == sdk ]]; then args+=(--dependency-provider); fi
  python scripts/test_mobile_android_reused_allocation_read.py \
    --neverd build-mobile/bin/neverd --d8 "$NEVERD_D8" \
    --java-home "$JAVA_HOME" \
    --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
    --work-dir "$RUNNER_TEMP/android-reused-allocation-read-$variant" "${args[@]}"
done

for variant in source sdk; do
  args=()
  if [[ "$variant" == sdk ]]; then args+=(--dependency-provider); fi
  python scripts/test_mobile_android_recursive_carrier_factory.py \
    --neverd build-mobile/bin/neverd --d8 "$NEVERD_D8" \
    --java-home "$JAVA_HOME" \
    --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
    --work-dir "$RUNNER_TEMP/android-recursive-carrier-factory-$variant" "${args[@]}"
done

for variant in source sdk; do
  args=()
  if [[ "$variant" == sdk ]]; then args+=(--dependency-provider); fi
  python scripts/test_mobile_android_private_interface_context.py \
    --neverd build-mobile/bin/neverd --d8 "$NEVERD_D8" \
    --java-home "$JAVA_HOME" \
    --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
    --work-dir "$RUNNER_TEMP/android-private-interface-context-$variant" "${args[@]}"
done

for variant in source sdk; do
  args=()
  if [[ "$variant" == sdk ]]; then args+=(--dependency-provider); fi
  python scripts/test_mobile_android_binary_closed_context.py \
    --neverd build-mobile/bin/neverd --d8 "$NEVERD_D8" \
    --java-home "$JAVA_HOME" \
    --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
    --work-dir "$RUNNER_TEMP/android-binary-closed-context-$variant" "${args[@]}"
done

for variant in source sdk; do
  args=()
  if [[ "$variant" == sdk ]]; then args+=(--dependency-provider); fi
  python scripts/test_mobile_android_array_callback_index.py \
    --neverd build-mobile/bin/neverd --d8 "$NEVERD_D8" \
    --java-home "$JAVA_HOME" \
    --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
    --work-dir "$RUNNER_TEMP/android-array-callback-index-$variant" "${args[@]}"
done

for variant in source sdk; do
  args=()
  if [[ "$variant" == sdk ]]; then args+=(--dependency-provider); fi
  python scripts/test_mobile_android_enum_interface_key.py \
    --neverd build-mobile/bin/neverd --d8 "$NEVERD_D8" \
    --java-home "$JAVA_HOME" \
    --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
    --work-dir "$RUNNER_TEMP/android-enum-interface-key-$variant" "${args[@]}"
done

for variant in source sdk; do
  args=()
  if [[ "$variant" == sdk ]]; then args+=(--dependency-provider); fi
  python scripts/test_mobile_android_array_boolean_varargs.py \
    --neverd build-mobile/bin/neverd --d8 "$NEVERD_D8" \
    --java-home "$JAVA_HOME" \
    --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
    --work-dir "$RUNNER_TEMP/android-array-boolean-varargs-$variant" "${args[@]}"
done

python scripts/test_mobile_android_enums.py \
  --neverd build-mobile/bin/neverd --d8 "$NEVERD_D8" \
  --java-home "$JAVA_HOME" \
  --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
  --work-dir "$RUNNER_TEMP/android-enum-inherited-static-field" --inherited-static-field

for variant in source sdk; do
  args=()
  if [[ "$variant" == sdk ]]; then args+=(--dependency-provider); fi
  python scripts/test_mobile_android_carrier_nominal_context.py \
    --neverd build-mobile/bin/neverd --d8 "$NEVERD_D8" \
    --java-home "$JAVA_HOME" \
    --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
    --work-dir "$RUNNER_TEMP/android-carrier-nominal-context-$variant" "${args[@]}"
done

for variant in source sdk; do
  args=()
  if [[ "$variant" == sdk ]]; then args+=(--dependency-provider); fi
  python scripts/test_mobile_android_instance_nominal_throws.py \
    --neverd build-mobile/bin/neverd --d8 "$NEVERD_D8" \
    --java-home "$JAVA_HOME" \
    --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
    --work-dir "$RUNNER_TEMP/android-instance-nominal-throws-$variant" "${args[@]}"
done

for variant in source sdk; do
  args=()
  if [[ "$variant" == sdk ]]; then args+=(--dependency-provider); fi
  python scripts/test_mobile_android_producer_throws.py \
    --neverd build-mobile/bin/neverd --d8 "$NEVERD_D8" \
    --java-home "$JAVA_HOME" \
    --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
    --work-dir "$RUNNER_TEMP/android-producer-throws-$variant" "${args[@]}"
done

for variant in source sdk; do
  args=()
  if [[ "$variant" == sdk ]]; then args+=(--dependency-provider); fi
  python scripts/test_mobile_android_parameterized_interface_seed.py \
    --neverd build-mobile/bin/neverd --d8 "$NEVERD_D8" \
    --java-home "$JAVA_HOME" \
    --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
    --work-dir "$RUNNER_TEMP/android-parameterized-interface-seed-$variant" "${args[@]}"
done

for variant in source sdk; do
  args=()
  if [[ "$variant" == sdk ]]; then args+=(--dependency-provider); fi
  python scripts/test_mobile_android_interface_token_context.py \
    --neverd build-mobile/bin/neverd --d8 "$NEVERD_D8" \
    --java-home "$JAVA_HOME" \
    --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
    --work-dir "$RUNNER_TEMP/android-interface-token-context-$variant" "${args[@]}"
done

for variant in source sdk; do
  args=()
  if [[ "$variant" == sdk ]]; then args+=(--dependency-provider); fi
  python scripts/test_mobile_android_enum_direct_seed.py \
    --neverd build-mobile/bin/neverd --d8 "$NEVERD_D8" \
    --java-home "$JAVA_HOME" --compiler-java-home "$NEVERD_JAVA21_HOME" \
    --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
    --work-dir "$RUNNER_TEMP/android-enum-direct-seed-$variant" "${args[@]}"
done

for variant in source sdk; do
  args=()
  if [[ "$variant" == sdk ]]; then args+=(--dependency-provider); fi
  python scripts/test_mobile_android_array_overload_separation.py \
    --neverd build-mobile/bin/neverd --d8 "$NEVERD_D8" \
    --java-home "$JAVA_HOME" \
    --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
    --work-dir "$RUNNER_TEMP/android-array-overload-separation-$variant" "${args[@]}"
done

for variant in source sdk; do
  args=()
  if [[ "$variant" == sdk ]]; then args+=(--dependency-provider); fi
  python scripts/test_mobile_android_protected_static_finals.py \
    --neverd build-mobile/bin/neverd --d8 "$NEVERD_D8" \
    --java-home "$JAVA_HOME" \
    --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
    --work-dir "$RUNNER_TEMP/android-protected-static-finals-$variant" "${args[@]}"
done

for variant in source sdk; do
  args=()
  if [[ "$variant" == sdk ]]; then args+=(--dependency-provider); fi
  python scripts/test_mobile_android_reifiable_array_delegation.py \
    --neverd build-mobile/bin/neverd --d8 "$NEVERD_D8" \
    --java-home "$JAVA_HOME" \
    --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
    --work-dir "$RUNNER_TEMP/android-reifiable-array-delegation-$variant" "${args[@]}"
done

for variant in source sdk; do
  args=()
  if [[ "$variant" == sdk ]]; then args+=(--dependency-provider); fi
  python scripts/test_mobile_android_nominal_super_result.py \
    --neverd build-mobile/bin/neverd --d8 "$NEVERD_D8" \
    --java-home "$JAVA_HOME" \
    --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
    --work-dir "$RUNNER_TEMP/android-nominal-super-result-$variant" "${args[@]}"
done

for variant in source sdk; do
  args=()
  if [[ "$variant" == sdk ]]; then args+=(--dependency-provider); fi
  python scripts/test_mobile_android_throwable_carrier_scalar.py \
    --neverd build-mobile/bin/neverd --d8 "$NEVERD_D8" \
    --java-home "$JAVA_HOME" \
    --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
    --work-dir "$RUNNER_TEMP/android-throwable-carrier-scalar-$variant" "${args[@]}"
done

for variant in source sdk; do
  args=()
  if [[ "$variant" == sdk ]]; then args+=(--dependency-provider); fi
  python scripts/test_mobile_android_throwable_carrier_context.py \
    --neverd build-mobile/bin/neverd --d8 "$NEVERD_D8" \
    --java-home "$JAVA_HOME" \
    --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
    --work-dir "$RUNNER_TEMP/android-throwable-carrier-context-$variant" "${args[@]}"
done

for variant in source sdk; do
  args=()
  if [[ "$variant" == sdk ]]; then args+=(--dependency-provider); fi
  python scripts/test_mobile_android_array_transfer_context.py \
    --neverd build-mobile/bin/neverd --d8 "$NEVERD_D8" \
    --java-home "$JAVA_HOME" \
    --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
    --work-dir "$RUNNER_TEMP/android-array-transfer-context-$variant" "${args[@]}"
done

for variant in source sdk; do
  args=()
  if [[ "$variant" == sdk ]]; then args+=(--dependency-provider); fi
  python scripts/test_mobile_android_throwable_array_carrier.py \
    --neverd build-mobile/bin/neverd --d8 "$NEVERD_D8" \
    --java-home "$JAVA_HOME" \
    --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
    --work-dir "$RUNNER_TEMP/android-throwable-array-carrier-$variant" "${args[@]}"
done

for variant in source sdk; do
  args=()
  if [[ "$variant" == sdk ]]; then args+=(--dependency-provider); fi
  python scripts/test_mobile_android_throwable_bound_seed.py \
    --neverd build-mobile/bin/neverd --d8 "$NEVERD_D8" \
    --java-home "$JAVA_HOME" \
    --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
    --work-dir "$RUNNER_TEMP/android-throwable-bound-seed-$variant" "${args[@]}"
done

for variant in source sdk; do
  args=()
  if [[ "$variant" == sdk ]]; then args+=(--dependency-provider); fi
  python scripts/test_mobile_android_dependent_seed_bound.py \
    --neverd build-mobile/bin/neverd --d8 "$NEVERD_D8" \
    --java-home "$JAVA_HOME" \
    --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
    --work-dir "$RUNNER_TEMP/android-dependent-seed-bound-$variant" "${args[@]}"
done

for variant in source sdk; do
  args=()
  if [[ "$variant" == sdk ]]; then args+=(--dependency-provider); fi
  python scripts/test_mobile_android_closed_map_super.py \
    --neverd build-mobile/bin/neverd --d8 "$NEVERD_D8" \
    --java-home "$JAVA_HOME" \
    --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
    --work-dir "$RUNNER_TEMP/android-closed-map-super-$variant" "${args[@]}"
done


for variant in source sdk; do
  args=()
  if [[ "$variant" == sdk ]]; then args+=(--dependency-provider); fi
  python scripts/test_mobile_android_direct_seed_nominal.py \
    --neverd build-mobile/bin/neverd --d8 "$NEVERD_D8" \
    --java-home "$JAVA_HOME" \
    --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
    --work-dir "$RUNNER_TEMP/android-direct-seed-nominal-$variant" "${args[@]}"
done


for variant in source sdk; do
  args=()
  if [[ "$variant" == sdk ]]; then args+=(--dependency-provider); fi
  python scripts/test_mobile_android_boolean_super_result.py \
    --neverd build-mobile/bin/neverd --d8 "$NEVERD_D8" \
    --java-home "$JAVA_HOME" \
    --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
    --work-dir "$RUNNER_TEMP/android-boolean-super-result-$variant" "${args[@]}"
done


for variant in source sdk; do
  args=()
  if [[ "$variant" == sdk ]]; then args+=(--dependency-provider); fi
  python scripts/test_mobile_android_class_throwable_context.py \
    --neverd build-mobile/bin/neverd --d8 "$NEVERD_D8" \
    --java-home "$JAVA_HOME" \
    --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
    --work-dir "$RUNNER_TEMP/android-class-throwable-context-$variant" "${args[@]}"
done


for variant in source sdk; do
  args=()
  if [[ "$variant" == sdk ]]; then args+=(--dependency-provider); fi
  python scripts/test_mobile_android_scoped_null_delegation.py \
    --neverd build-mobile/bin/neverd --d8 "$NEVERD_D8" \
    --java-home "$JAVA_HOME" \
    --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
    --work-dir "$RUNNER_TEMP/android-scoped-null-delegation-$variant" "${args[@]}"
done


for variant in source sdk; do
  args=()
  if [[ "$variant" == sdk ]]; then args+=(--dependency-provider); fi
  python scripts/test_mobile_android_array_nominal_context.py \
    --neverd build-mobile/bin/neverd --d8 "$NEVERD_D8" \
    --java-home "$JAVA_HOME" \
    --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
    --work-dir "$RUNNER_TEMP/android-array-nominal-context-$variant" "${args[@]}"
done


for variant in source sdk; do
  args=()
  if [[ "$variant" == sdk ]]; then args+=(--dependency-provider); fi
  python scripts/test_mobile_android_comparable_map_transfer.py \
    --neverd build-mobile/bin/neverd --d8 "$NEVERD_D8" \
    --java-home "$JAVA_HOME" \
    --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
    --work-dir "$RUNNER_TEMP/android-comparable-map-transfer-$variant" "${args[@]}"
done


for variant in source sdk; do
  args=()
  if [[ "$variant" == sdk ]]; then args+=(--dependency-provider); fi
  python scripts/test_mobile_android_direct_seed_raw_carrier.py \
    --neverd build-mobile/bin/neverd --d8 "$NEVERD_D8" \
    --java-home "$JAVA_HOME" \
    --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
    --work-dir "$RUNNER_TEMP/android-direct-seed-raw-carrier-$variant" "${args[@]}"
done


for variant in source sdk; do
  args=()
  if [[ "$variant" == sdk ]]; then args+=(--dependency-provider); fi
  python scripts/test_mobile_android_nested_seed_transfer.py \
    --neverd build-mobile/bin/neverd --d8 "$NEVERD_D8" \
    --java-home "$JAVA_HOME" \
    --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
    --work-dir "$RUNNER_TEMP/android-nested-seed-transfer-$variant" "${args[@]}"
done


for variant in source sdk; do
  args=()
  if [[ "$variant" == sdk ]]; then args+=(--dependency-provider); fi
  python scripts/test_mobile_android_seeded_nested_callback.py \
    --neverd build-mobile/bin/neverd --d8 "$NEVERD_D8" \
    --java-home "$JAVA_HOME" \
    --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
    --work-dir "$RUNNER_TEMP/android-seeded-nested-callback-$variant" "${args[@]}"
done


for variant in source sdk; do
  args=()
  if [[ "$variant" == sdk ]]; then args+=(--dependency-provider); fi
  python scripts/test_mobile_android_seeded_carrier_callback.py \
    --neverd build-mobile/bin/neverd --d8 "$NEVERD_D8" \
    --java-home "$JAVA_HOME" \
    --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
    --work-dir "$RUNNER_TEMP/android-seeded-carrier-callback-$variant" "${args[@]}"
done


for variant in source sdk; do
  args=()
  if [[ "$variant" == sdk ]]; then args+=(--dependency-provider); fi
  python scripts/test_mobile_android_reversed_carrier_slots.py \
    --neverd build-mobile/bin/neverd --d8 "$NEVERD_D8" \
    --java-home "$JAVA_HOME" \
    --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
    --work-dir "$RUNNER_TEMP/android-reversed-carrier-slots-$variant" "${args[@]}"
done

for variant in source sdk; do
  args=()
  if [[ "$variant" == sdk ]]; then args+=(--dependency-provider); fi
  python scripts/test_mobile_android_primitive_map_super.py \
    --neverd build-mobile/bin/neverd --d8 "$NEVERD_D8" \
    --java-home "$JAVA_HOME" \
    --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
    --work-dir "$RUNNER_TEMP/android-primitive-map-super-$variant" "${args[@]}"
done

for variant in source sdk; do
  args=()
  if [[ "$variant" == sdk ]]; then args+=(--dependency-provider); fi
  python scripts/test_mobile_android_floating_constructor_default.py \
    --neverd build-mobile/bin/neverd --d8 "$NEVERD_D8" \
    --java-home "$JAVA_HOME" \
    --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
    --work-dir "$RUNNER_TEMP/android-floating-constructor-default-$variant" "${args[@]}"
done

for variant in source sdk; do
  args=()
  if [[ "$variant" == sdk ]]; then args+=(--dependency-provider); fi
  python scripts/test_mobile_android_scoped_builder_callback_super.py \
    --neverd build-mobile/bin/neverd --d8 "$NEVERD_D8" \
    --java-home "$JAVA_HOME" \
    --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
    --work-dir "$RUNNER_TEMP/android-scoped-builder-callback-super-$variant" "${args[@]}"
done

for variant in source sdk; do
  args=()
  if [[ "$variant" == sdk ]]; then args+=(--dependency-provider); fi
  python scripts/test_mobile_android_scoped_comparator_allocation.py \
    --neverd build-mobile/bin/neverd --d8 "$NEVERD_D8" \
    --java-home "$JAVA_HOME" \
    --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
    --work-dir "$RUNNER_TEMP/android-scoped-comparator-allocation-$variant" "${args[@]}"
done

for variant in source sdk; do
  args=()
  if [[ "$variant" == sdk ]]; then args+=(--dependency-provider); fi
  python scripts/test_mobile_android_scoped_map_initializer_upcast.py \
    --neverd build-mobile/bin/neverd --d8 "$NEVERD_D8" \
    --java-home "$JAVA_HOME" \
    --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
    --work-dir "$RUNNER_TEMP/android-scoped-map-initializer-upcast-$variant" "${args[@]}"
done

for variant in source sdk; do
  args=()
  if [[ "$variant" == sdk ]]; then args+=(--dependency-provider); fi
  python scripts/test_mobile_android_scoped_supplier_delegation.py \
    --neverd build-mobile/bin/neverd --d8 "$NEVERD_D8" \
    --java-home "$JAVA_HOME" \
    --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
    --work-dir "$RUNNER_TEMP/android-scoped-supplier-delegation-$variant" "${args[@]}"
done

for variant in source sdk; do
  args=()
  if [[ "$variant" == sdk ]]; then args+=(--dependency-provider); fi
  python scripts/test_mobile_android_carrier_nominal_varargs.py \
    --neverd build-mobile/bin/neverd --d8 "$NEVERD_D8" \
    --java-home "$JAVA_HOME" \
    --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
    --work-dir "$RUNNER_TEMP/android-carrier-nominal-varargs-$variant" "${args[@]}"
done

for variant in source sdk; do
  args=()
  if [[ "$variant" == sdk ]]; then args+=(--dependency-provider); fi
  python scripts/test_mobile_android_carrier_scalar_varargs.py \
    --neverd build-mobile/bin/neverd --d8 "$NEVERD_D8" \
    --java-home "$JAVA_HOME" \
    --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
    --work-dir "$RUNNER_TEMP/android-carrier-scalar-varargs-$variant" "${args[@]}"
done

for variant in source sdk; do
  args=()
  if [[ "$variant" == sdk ]]; then args+=(--dependency-provider); fi
  python scripts/test_mobile_android_paired_seed_element.py \
    --neverd build-mobile/bin/neverd --d8 "$NEVERD_D8" \
    --java-home "$JAVA_HOME" \
    --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
    --work-dir "$RUNNER_TEMP/android-paired-seed-element-$variant" "${args[@]}"
done

for variant in source sdk; do
  args=()
  if [[ "$variant" == sdk ]]; then args+=(--dependency-provider); fi
  python scripts/test_mobile_android_paired_seed_scalar.py \
    --neverd build-mobile/bin/neverd --d8 "$NEVERD_D8" \
    --java-home "$JAVA_HOME" \
    --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
    --work-dir "$RUNNER_TEMP/android-paired-seed-scalar-$variant" "${args[@]}"
done

for variant in source sdk; do
  args=()
  if [[ "$variant" == sdk ]]; then args+=(--dependency-provider); fi
  python scripts/test_mobile_android_token_seed_unchecked.py \
    --neverd build-mobile/bin/neverd --d8 "$NEVERD_D8" \
    --java-home "$JAVA_HOME" \
    --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
    --work-dir "$RUNNER_TEMP/android-token-seed-unchecked-$variant" "${args[@]}"
done

for variant in source sdk; do
  args=()
  if [[ "$variant" == sdk ]]; then args+=(--dependency-provider); fi
  python scripts/test_mobile_android_paired_key_consumer.py \
    --neverd build-mobile/bin/neverd --d8 "$NEVERD_D8" \
    --java-home "$JAVA_HOME" \
    --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
    --work-dir "$RUNNER_TEMP/android-paired-key-consumer-$variant" "${args[@]}"
done

for variant in source sdk; do
  args=()
  if [[ "$variant" == sdk ]]; then args+=(--dependency-provider); fi
  python scripts/test_mobile_android_comparable_pair_factory.py \
    --neverd build-mobile/bin/neverd --d8 "$NEVERD_D8" \
    --java-home "$JAVA_HOME" \
    --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
    --work-dir "$RUNNER_TEMP/android-comparable-pair-factory-$variant" "${args[@]}"
done

for variant in source sdk; do
  args=()
  if [[ "$variant" == sdk ]]; then args+=(--dependency-provider); fi
  python scripts/test_mobile_android_scoped_static_field_super.py \
    --neverd build-mobile/bin/neverd --d8 "$NEVERD_D8" \
    --java-home "$JAVA_HOME" \
    --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
    --work-dir "$RUNNER_TEMP/android-scoped-static-field-super-$variant" "${args[@]}"
done

for variant in source sdk; do
  args=()
  if [[ "$variant" == sdk ]]; then args+=(--dependency-provider); fi
  python scripts/test_mobile_android_dependent_carrier_allocation.py \
    --neverd build-mobile/bin/neverd --d8 "$NEVERD_D8" \
    --java-home "$JAVA_HOME" \
    --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
    --work-dir "$RUNNER_TEMP/android-dependent-carrier-allocation-$variant" "${args[@]}"
done

for variant in source sdk; do
  args=()
  if [[ "$variant" == sdk ]]; then args+=(--dependency-provider); fi
  python scripts/test_mobile_android_synthetic_nullary_allocation.py \
    --neverd build-mobile/bin/neverd --d8 "$NEVERD_D8" \
    --java-home "$JAVA_HOME" \
    --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
    --work-dir "$RUNNER_TEMP/android-synthetic-nullary-allocation-$variant" "${args[@]}"
done

for variant in source sdk; do
  args=()
  if [[ "$variant" == sdk ]]; then args+=(--dependency-provider); fi
  python scripts/test_mobile_android_scoped_integer_getter_delegation.py \
    --neverd build-mobile/bin/neverd --d8 "$NEVERD_D8" \
    --java-home "$JAVA_HOME" \
    --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
    --work-dir "$RUNNER_TEMP/android-scoped-integer-getter-delegation-$variant" "${args[@]}"
done

profile_group "Verify package synthetic names on captured generic members" 25 1200
for variant in source sdk; do
  args=()
  if [[ "$variant" == sdk ]]; then args+=(--dependency-provider); fi
  python scripts/test_mobile_android_synthetic_instance_names.py --captured-owner \
    --neverd build-mobile/bin/neverd --d8 "$NEVERD_D8" \
    --java-home "$JAVA_HOME" \
    --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
    --work-dir "$RUNNER_TEMP/android-captured-synthetic-identifiers-$variant" "${args[@]}"
done

profile_group "Verify covariant allocation constructor delegation" 25 1200
for variant in source sdk; do
  args=()
  if [[ "$variant" == sdk ]]; then args+=(--dependency-provider); fi
  python scripts/test_mobile_android_synthetic_nullary_allocation.py --covariant-maker \
    --neverd build-mobile/bin/neverd --d8 "$NEVERD_D8" \
    --java-home "$JAVA_HOME" \
    --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
    --work-dir "$RUNNER_TEMP/android-covariant-allocation-delegation-$variant" "${args[@]}"
done

profile_group "Verify primitive captures in synthetic constructor allocations" 25 1200
for variant in source sdk; do
  args=()
  if [[ "$variant" == sdk ]]; then args+=(--dependency-provider); fi
  python scripts/test_mobile_android_synthetic_nullary_allocation.py --primitive-capture \
    --neverd build-mobile/bin/neverd --d8 "$NEVERD_D8" \
    --java-home "$JAVA_HOME" \
    --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
    --work-dir "$RUNNER_TEMP/android-synthetic-primitive-allocation-$variant" "${args[@]}"
done

profile_group "Verify method-owned array formals on generic instance receivers" 25 1200
for variant in source sdk; do
  args=()
  if [[ "$variant" == sdk ]]; then args+=(--dependency-provider); fi
  python scripts/test_mobile_android_instance_array_transfer.py \
    --neverd build-mobile/bin/neverd --d8 "$NEVERD_D8" \
    --java-home "$JAVA_HOME" \
    --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
    --work-dir "$RUNNER_TEMP/android-instance-array-transfer-$variant" "${args[@]}"
done

profile_group "Verify closed constructor inputs with mixed wildcard slots" 25 1200
for variant in source sdk; do
  args=()
  if [[ "$variant" == sdk ]]; then args+=(--dependency-callback); fi
  python scripts/test_mobile_android_mixed_star_constructor.py \
    --neverd build-mobile/bin/neverd --d8 "$NEVERD_D8" \
    --java-home "$JAVA_HOME" \
    --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
    --work-dir "$RUNNER_TEMP/android-mixed-star-constructor-$variant" "${args[@]}"
done

profile_group "Verify closed mixed-star constructor access and marker roles" 25 1200
for access in public-target plain-marker inherited-marker; do
  for variant in source sdk; do
    args=()
    if [[ "$variant" == sdk ]]; then args+=(--dependency-inputs); fi
    python scripts/test_mobile_android_inherited_fixed_marker.py --mixed-star-access "$access" \
      --neverd build-mobile/bin/neverd --d8 "$NEVERD_D8" \
      --java-home "$JAVA_HOME" \
      --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
      --work-dir "$RUNNER_TEMP/android-mixed-star-marker-$access-$variant" "${args[@]}"
  done
done

profile_group "Verify mapper factories with chained interface bounds" 25 1200
for variant in source sdk; do
  args=()
  if [[ "$variant" == sdk ]]; then args+=(--dependency-provider); fi
  python scripts/test_mobile_android_dependent_collector.py \
    --neverd build-mobile/bin/neverd --d8 "$NEVERD_D8" \
    --java-home "$JAVA_HOME" \
    --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
    --work-dir "$RUNNER_TEMP/android-dependent-collector-$variant" "${args[@]}"
done

profile_group "Verify produced collectors with dependent interface bounds" 25 1200
for variant in source sdk; do
  args=()
  if [[ "$variant" == sdk ]]; then args+=(--dependency-provider); fi
  python scripts/test_mobile_android_produced_collector.py \
    --neverd build-mobile/bin/neverd --d8 "$NEVERD_D8" \
    --java-home "$JAVA_HOME" \
    --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
    --work-dir "$RUNNER_TEMP/android-produced-collector-$variant" "${args[@]}"
done

profile_group "Verify grouped collectors with independent downstream state" 25 1200
for variant in source sdk; do
  args=()
  if [[ "$variant" == sdk ]]; then args+=(--dependency-provider); fi
  python scripts/test_mobile_android_grouped_collector.py \
    --neverd build-mobile/bin/neverd --d8 "$NEVERD_D8" \
    --java-home "$JAVA_HOME" \
    --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
    --work-dir "$RUNNER_TEMP/android-grouped-collector-$variant" "${args[@]}"
done

profile_group "Verify unary operators with dependent interface elements" 25 1200
for variant in source sdk; do
  args=()
  if [[ "$variant" == sdk ]]; then args+=(--dependency-provider); fi
  python scripts/test_mobile_android_dependent_operator.py \
    --neverd build-mobile/bin/neverd --d8 "$NEVERD_D8" \
    --java-home "$JAVA_HOME" \
    --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
    --work-dir "$RUNNER_TEMP/android-dependent-operator-$variant" "${args[@]}"
done

profile_group "Verify configured collectors with closed nominal results" 25 1200
for variant in source sdk; do
  args=()
  if [[ "$variant" == sdk ]]; then args+=(--dependency-provider); fi
  python scripts/test_mobile_android_configured_collector.py \
    --neverd build-mobile/bin/neverd --d8 "$NEVERD_D8" \
    --java-home "$JAVA_HOME" \
    --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
    --work-dir "$RUNNER_TEMP/android-configured-collector-$variant" "${args[@]}"
done

profile_group "Verify produced collectors with nested interface bounds" 25 1200
for variant in source sdk; do
  args=()
  if [[ "$variant" == sdk ]]; then args+=(--dependency-provider); fi
  python scripts/test_mobile_android_nested_produced_collector.py \
    --neverd build-mobile/bin/neverd --d8 "$NEVERD_D8" \
    --java-home "$JAVA_HOME" \
    --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
    --work-dir "$RUNNER_TEMP/android-nested-produced-collector-$variant" "${args[@]}"
done

profile_group "Verify mapped seeded reducers and deferred callbacks" 25 1200
for variant in source sdk; do
  args=()
  if [[ "$variant" == sdk ]]; then args+=(--dependency-provider); fi
  python scripts/test_mobile_android_mapped_reducer.py \
    --neverd build-mobile/bin/neverd --d8 "$NEVERD_D8" \
    --java-home "$JAVA_HOME" \
    --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
    --work-dir "$RUNNER_TEMP/android-mapped-reducer-$variant" "${args[@]}"
done

profile_group "Verify dependent map collectors with merge callbacks" 25 1200
for variant in source sdk; do
  args=()
  if [[ "$variant" == sdk ]]; then args+=(--dependency-provider); fi
  python scripts/test_mobile_android_merging_collector.py \
    --neverd build-mobile/bin/neverd --d8 "$NEVERD_D8" \
    --java-home "$JAVA_HOME" \
    --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
    --work-dir "$RUNNER_TEMP/android-merging-collector-$variant" "${args[@]}"
done

profile_group "Verify result-only collector formals with closed contexts" 25 1200
for variant in source sdk; do
  args=()
  if [[ "$variant" == sdk ]]; then args+=(--dependency-provider); fi
  python scripts/test_mobile_android_contextual_collector.py \
    --neverd build-mobile/bin/neverd --d8 "$NEVERD_D8" \
    --java-home "$JAVA_HOME" \
    --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
    --work-dir "$RUNNER_TEMP/android-contextual-collector-$variant" "${args[@]}"
done

profile_group "Verify final field alternatives after constructor prefix loops" 25 1200
for variant in source sdk; do
  args=()
  if [[ "$variant" == sdk ]]; then args+=(--dependency-provider); fi
  python scripts/test_mobile_android_prefix_loop_final.py \
    --neverd build-mobile/bin/neverd --d8 "$NEVERD_D8" \
    --java-home "$JAVA_HOME" --compiler-java-home "$NEVERD_JAVA21_HOME" \
    --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
    --work-dir "$RUNNER_TEMP/android-prefix-loop-final-$variant" "${args[@]}"
done

profile_group "Verify guarded constructor delegation continuations" 25 1200
for variant in source sdk; do
  args=()
  if [[ "$variant" == sdk ]]; then args+=(--dependency-provider); fi
  python scripts/test_mobile_android_guarded_delegation_tail.py \
    --neverd build-mobile/bin/neverd --d8 "$NEVERD_D8" \
    --java-home "$JAVA_HOME" --compiler-java-home "$NEVERD_JAVA21_HOME" \
    --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
    --work-dir "$RUNNER_TEMP/android-guarded-delegation-tail-$variant" "${args[@]}"
done

profile_group "Verify inherited static constructor reads through known interface scopes" 25 1200
for variant in source sdk; do
  args=(--interface-scope)
  if [[ "$variant" == sdk ]]; then args+=(--dependency-provider); fi
  python scripts/test_mobile_android_inherited_static_prefix.py \
    --neverd build-mobile/bin/neverd --d8 "$NEVERD_D8" \
    --java-home "$JAVA_HOME" --compiler-java-home "$NEVERD_JAVA21_HOME" \
    --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
    --work-dir "$RUNNER_TEMP/android-interface-static-prefix-$variant" "${args[@]}"
done

profile_group "Verify nested carrier scopes on opaque instance transfers" 25 1200
for variant in source sdk; do
  args=()
  if [[ "$variant" == sdk ]]; then args+=(--dependency-provider); fi
  python scripts/test_mobile_android_nested_task_transfer.py \
    --neverd build-mobile/bin/neverd --d8 "$NEVERD_D8" \
    --java-home "$JAVA_HOME" --compiler-java-home "$NEVERD_JAVA21_HOME" \
    --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
    --work-dir "$RUNNER_TEMP/android-nested-task-transfer-$variant" "${args[@]}"
done

profile_group "Verify direct own-formal results from nested instance producers" 25 1200
for variant in source sdk; do
  args=()
  if [[ "$variant" == sdk ]]; then args+=(--dependency-provider); fi
  python scripts/test_mobile_android_nested_task_result.py \
    --neverd build-mobile/bin/neverd --d8 "$NEVERD_D8" \
    --java-home "$JAVA_HOME" --compiler-java-home "$NEVERD_JAVA21_HOME" \
    --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
    --work-dir "$RUNNER_TEMP/android-nested-task-result-$variant" "${args[@]}"
done

profile_group "Verify later strings retain deferred allocation effect order" 25 1200
for variant in source sdk; do
  args=()
  if [[ "$variant" == sdk ]]; then args+=(--dependency-provider); fi
  python scripts/test_mobile_android_deferred_field_string.py \
    --neverd build-mobile/bin/neverd --d8 "$NEVERD_D8" \
    --java-home "$JAVA_HOME" --compiler-java-home "$NEVERD_JAVA21_HOME" \
    --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
    --work-dir "$RUNNER_TEMP/android-deferred-field-string-$variant" "${args[@]}"
done

profile_group "Verify generic array tails preserve fixed-arity nominal results" 25 1200
for variant in source sdk; do
  args=()
  if [[ "$variant" == sdk ]]; then args+=(--dependency-provider); fi
  python scripts/test_mobile_android_array_nominal_varargs.py \
    --neverd build-mobile/bin/neverd --d8 "$NEVERD_D8" \
    --java-home "$JAVA_HOME" --compiler-java-home "$NEVERD_JAVA21_HOME" \
    --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
    --work-dir "$RUNNER_TEMP/android-array-nominal-varargs-$variant" "${args[@]}"
done

profile_group "Verify scalar contexts beside generic arrays and independent objects" 25 1200
for variant in source sdk; do
  args=()
  if [[ "$variant" == sdk ]]; then args+=(--dependency-provider); fi
  python scripts/test_mobile_android_array_object_context.py \
    --neverd build-mobile/bin/neverd --d8 "$NEVERD_D8" \
    --java-home "$JAVA_HOME" --compiler-java-home "$NEVERD_JAVA21_HOME" \
    --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
    --work-dir "$RUNNER_TEMP/android-array-object-context-$variant" "${args[@]}"
done

profile_group "Verify recursive Comparable array predicates preserve original effects" 25 1200
for variant in source sdk; do
  args=()
  if [[ "$variant" == sdk ]]; then args+=(--dependency-provider); fi
  python scripts/test_mobile_android_comparable_array_predicate.py \
    --neverd build-mobile/bin/neverd --d8 "$NEVERD_D8" \
    --java-home "$JAVA_HOME" --compiler-java-home "$NEVERD_JAVA21_HOME" \
    --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
    --work-dir "$RUNNER_TEMP/android-comparable-array-predicate-$variant" "${args[@]}"
done

profile_group "Verify recursive array results retain invariant contexts and original effects" 25 1200
for variant in source sdk; do
  args=()
  if [[ "$variant" == sdk ]]; then args+=(--dependency-provider); fi
  python scripts/test_mobile_android_comparable_array_result.py \
    --neverd build-mobile/bin/neverd --d8 "$NEVERD_D8" \
    --java-home "$JAVA_HOME" --compiler-java-home "$NEVERD_JAVA21_HOME" \
    --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
    --work-dir "$RUNNER_TEMP/android-comparable-array-result-$variant" "${args[@]}"
done

profile_group "Verify generic array transfers retain independent unbounded contexts" 25 1200
for variant in source sdk; do
  args=()
  if [[ "$variant" == sdk ]]; then args+=(--dependency-provider); fi
  python scripts/test_mobile_android_array_star_context.py \
    --neverd build-mobile/bin/neverd --d8 "$NEVERD_D8" \
    --java-home "$JAVA_HOME" --compiler-java-home "$NEVERD_JAVA21_HOME" \
    --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
    --work-dir "$RUNNER_TEMP/android-array-star-context-$variant" "${args[@]}"
done

profile_group "Verify recursive enum tokens retain closed map heads and opaque results" 25 1200
for variant in source sdk; do
  args=()
  if [[ "$variant" == sdk ]]; then args+=(--dependency-provider); fi
  python scripts/test_mobile_android_enum_token_map.py \
    --neverd build-mobile/bin/neverd --d8 "$NEVERD_D8" \
    --java-home "$JAVA_HOME" --compiler-java-home "$NEVERD_JAVA21_HOME" \
    --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
    --work-dir "$RUNNER_TEMP/android-enum-token-map-$variant" "${args[@]}"
done

profile_group "Verify recursive enum and independent key formals retain paired inputs" 25 1200
for variant in source sdk; do
  args=()
  if [[ "$variant" == sdk ]]; then args+=(--dependency-provider); fi
  python scripts/test_mobile_android_enum_carrier_key.py \
    --neverd build-mobile/bin/neverd --d8 "$NEVERD_D8" \
    --java-home "$JAVA_HOME" --compiler-java-home "$NEVERD_JAVA21_HOME" \
    --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
    --work-dir "$RUNNER_TEMP/android-enum-carrier-key-$variant" "${args[@]}"
done

profile_group "Verify recursive enum tokens retain closed string contexts and Boolean results" 25 1200
for variant in source sdk; do
  args=()
  if [[ "$variant" == sdk ]]; then args+=(--dependency-provider); fi
  python scripts/test_mobile_android_enum_token_predicate.py \
    --neverd build-mobile/bin/neverd --d8 "$NEVERD_D8" \
    --java-home "$JAVA_HOME" --compiler-java-home "$NEVERD_JAVA21_HOME" \
    --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
    --work-dir "$RUNNER_TEMP/android-enum-token-predicate-$variant" "${args[@]}"
done

profile_group "Verify input arrays bind mapper inputs and independent token results" 25 1200
for variant in source sdk; do
  args=()
  if [[ "$variant" == sdk ]]; then args+=(--dependency-provider); fi
  python scripts/test_mobile_android_array_input_mapped_token.py \
    --neverd build-mobile/bin/neverd --d8 "$NEVERD_D8" \
    --java-home "$JAVA_HOME" --compiler-java-home "$NEVERD_JAVA21_HOME" \
    --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
    --work-dir "$RUNNER_TEMP/android-array-input-mapped-token-$variant" "${args[@]}"
done

profile_group "Verify invariant factories retain independent all-star array contexts" 25 1200
for variant in source sdk; do
  args=()
  if [[ "$variant" == sdk ]]; then args+=(--dependency-provider); fi
  python scripts/test_mobile_android_token_star_array_factory.py \
    --neverd build-mobile/bin/neverd --d8 "$NEVERD_D8" \
    --java-home "$JAVA_HOME" --compiler-java-home "$NEVERD_JAVA21_HOME" \
    --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
    --work-dir "$RUNNER_TEMP/android-token-star-array-factory-$variant" "${args[@]}"
done

profile_group "Verify recursive carrier inputs retain Comparable result erasure" 25 1200
for variant in source sdk; do
  args=()
  if [[ "$variant" == sdk ]]; then args+=(--dependency-provider); fi
  python scripts/test_mobile_android_comparable_carrier_result.py \
    --neverd build-mobile/bin/neverd --d8 "$NEVERD_D8" \
    --java-home "$JAVA_HOME" --compiler-java-home "$NEVERD_JAVA21_HOME" \
    --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
    --work-dir "$RUNNER_TEMP/android-comparable-carrier-result-$variant" "${args[@]}"
done

profile_group "Verify fixed source and SDK interfaces regenerate checked bridges" 25 1200
for variant in source sdk; do
  args=()
  if [[ "$variant" == sdk ]]; then args+=(--dependency-provider); fi
  python scripts/test_mobile_android_fixed_sdk_interface_bridge.py \
    --neverd build-mobile/bin/neverd --d8 "$NEVERD_D8" \
    --java-home "$JAVA_HOME" --compiler-java-home "$NEVERD_JAVA21_HOME" \
    --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
    --work-dir "$RUNNER_TEMP/android-fixed-sdk-interface-bridge-$variant" "${args[@]}"
done

profile_group "Verify nested allocations preserve constructor initialization order" 25 1200
for variant in source sdk; do
  args=()
  if [[ "$variant" == sdk ]]; then args+=(--dependency-provider); fi
  python scripts/test_mobile_android_nested_prefix_allocation.py \
    --neverd build-mobile/bin/neverd --d8 "$NEVERD_D8" \
    --java-home "$JAVA_HOME" --compiler-java-home "$NEVERD_JAVA21_HOME" \
    --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
    --work-dir "$RUNNER_TEMP/android-nested-prefix-allocation-$variant" "${args[@]}"
done

profile_group "Verify filled reference arrays preserve constructor prefix identity" 25 1200
for variant in source sdk; do
  args=()
  if [[ "$variant" == sdk ]]; then args+=(--dependency-provider); fi
  python scripts/test_mobile_android_filled_reference_prefix.py \
    --neverd build-mobile/bin/neverd --d8 "$NEVERD_D8" \
    --java-home "$JAVA_HOME" --compiler-java-home "$NEVERD_JAVA21_HOME" \
    --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
    --work-dir "$RUNNER_TEMP/android-filled-reference-prefix-$variant" "${args[@]}"
done

profile_group "Verify fixed context bounded carrier transfers" 25 1200
for variant in source sdk; do
  args=()
  if [[ "$variant" == sdk ]]; then args+=(--dependency-provider); fi
  python scripts/test_mobile_android_contextual_carrier_transfer.py \
    --neverd build-mobile/bin/neverd --d8 "$NEVERD_D8" \
    --java-home "$JAVA_HOME" --compiler-java-home "$NEVERD_JAVA21_HOME" \
    --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
    --work-dir "$RUNNER_TEMP/android-contextual-carrier-transfer-$variant" "${args[@]}"
done

profile_group "Verify closed contexts and producer results retain checked exceptions" 25 1200
for variant in source sdk; do
  args=()
  if [[ "$variant" == sdk ]]; then args+=(--dependency-provider); fi
  python scripts/test_mobile_android_closed_carrier_producer.py \
    --neverd build-mobile/bin/neverd --d8 "$NEVERD_D8" \
    --java-home "$JAVA_HOME" --compiler-java-home "$NEVERD_JAVA21_HOME" \
    --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
    --work-dir "$RUNNER_TEMP/android-closed-carrier-producer-$variant" "${args[@]}"
done

profile_group "Verify instance producer varargs preserve fixed arrays and checked exceptions" 25 1200
for variant in source sdk; do
  args=()
  if [[ "$variant" == sdk ]]; then args+=(--dependency-provider); fi
  python scripts/test_mobile_android_instance_producer_varargs.py \
    --neverd build-mobile/bin/neverd --d8 "$NEVERD_D8" \
    --java-home "$JAVA_HOME" --compiler-java-home "$NEVERD_JAVA21_HOME" \
    --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
    --work-dir "$RUNNER_TEMP/android-instance-producer-varargs-$variant" "${args[@]}"
done

profile_group "Verify closed context slots retain producer scope and checked exceptions" 25 1200
for variant in source sdk; do
  args=()
  if [[ "$variant" == sdk ]]; then args+=(--dependency-provider); fi
  python scripts/test_mobile_android_closed_context_producer.py \
    --neverd build-mobile/bin/neverd --d8 "$NEVERD_D8" \
    --java-home "$JAVA_HOME" --compiler-java-home "$NEVERD_JAVA21_HOME" \
    --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
    --work-dir "$RUNNER_TEMP/android-closed-context-producer-$variant" "${args[@]}"
done

profile_group "Verify direct method seeds preserve primitive inputs and closed ancestors" 25 600
for variant in source sdk; do
  args=()
  if [[ "$variant" == sdk ]]; then args+=(--dependency-provider); fi
  python scripts/test_mobile_android_instance_seed_nominal.py \
    --neverd build-mobile/bin/neverd --d8 "$NEVERD_D8" \
    --java-home "$JAVA_HOME" \
    --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
    --work-dir "$RUNNER_TEMP/android-instance-seed-nominal-$variant" "${args[@]}"
done

profile_group "Verify method seeds preserve covariant class bridges and super result casts" 25 900
for variant in source sdk; do
  args=()
  if [[ "$variant" == sdk ]]; then args+=(--dependency-provider); fi
  python scripts/test_mobile_android_method_seed_bridge.py \
    --neverd build-mobile/bin/neverd --d8 "$NEVERD_D8" \
    --java-home "$JAVA_HOME" \
    --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
    --work-dir "$RUNNER_TEMP/android-method-seed-bridge-$variant" "${args[@]}"
done

profile_group "Verify unused instance formals retain scalar results and closed exceptions" 25 900
for variant in source sdk; do
  args=()
  if [[ "$variant" == sdk ]]; then args+=(--dependency-provider); fi
  python scripts/test_mobile_android_unused_scalar_throws.py \
    --neverd build-mobile/bin/neverd --d8 "$NEVERD_D8" \
    --java-home "$JAVA_HOME" \
    --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
    --work-dir "$RUNNER_TEMP/android-unused-scalar-throws-$variant" "${args[@]}"
done

profile_group "Verify direct method seeds retain fixed contexts and closed exceptions" 25 900
for variant in source sdk; do
  args=()
  if [[ "$variant" == sdk ]]; then args+=(--dependency-provider); fi
  python scripts/test_mobile_android_nominal_seed_context_throws.py \
    --neverd build-mobile/bin/neverd --d8 "$NEVERD_D8" \
    --java-home "$JAVA_HOME" \
    --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
    --work-dir "$RUNNER_TEMP/android-nominal-seed-context-throws-$variant" "${args[@]}"
done

profile_group "Verify closed parent interface contracts on plain class descendants" 25 900
for variant in source sdk; do
  args=()
  if [[ "$variant" == sdk ]]; then args+=(--dependency-provider); fi
  python scripts/test_mobile_android_closed_class_interface_bridge.py \
    --neverd build-mobile/bin/neverd --d8 "$NEVERD_D8" \
    --java-home "$JAVA_HOME" --compiler-java-home "$JAVA_HOME" \
    --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
    --work-dir "$RUNNER_TEMP/android-closed-class-interface-bridge-$variant" "${args[@]}"
done

profile_group "Verify classes containing parameter and method covariance bridges" 25 900
for variant in source sdk; do
  args=()
  if [[ "$variant" == sdk ]]; then args+=(--dependency-provider); fi
  python scripts/test_mobile_android_mixed_bridge_contract_ownership.py \
    --neverd build-mobile/bin/neverd --d8 "$NEVERD_D8" \
    --java-home "$JAVA_HOME" --compiler-java-home "$JAVA_HOME" \
    --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
    --work-dir "$RUNNER_TEMP/android-mixed-bridge-contract-ownership-$variant" "${args[@]}"
done

profile_group "Verify inherited concrete parameter bridges without child methods" 25 900
for variant in source sdk; do
  args=()
  if [[ "$variant" == sdk ]]; then args+=(--dependency-provider); fi
  python scripts/test_mobile_android_inherited_fixed_parameter_bridge.py \
    --neverd build-mobile/bin/neverd --d8 "$NEVERD_D8" \
    --java-home "$JAVA_HOME" --compiler-java-home "$JAVA_HOME" \
    --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
    --work-dir "$RUNNER_TEMP/android-inherited-fixed-parameter-bridge-$variant" "${args[@]}"
done

profile_group "Verify inherited checked bridges with complete method generic scopes" 25 900
for variant in source sdk; do
  args=()
  if [[ "$variant" == sdk ]]; then args+=(--dependency-provider); fi
  python scripts/test_mobile_android_inherited_method_parameter_bridge.py \
    --neverd build-mobile/bin/neverd --d8 "$NEVERD_D8" \
    --java-home "$JAVA_HOME" --compiler-java-home "$JAVA_HOME" \
    --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
    --work-dir "$RUNNER_TEMP/android-inherited-method-parameter-bridge-$variant" "${args[@]}"
done

profile_group "Verify closed covariant slots through checked bridge discovery" 25 900
for variant in source sdk; do
  args=()
  if [[ "$variant" == sdk ]]; then args+=(--dependency-provider); fi
  python scripts/test_mobile_android_closed_covariant_slot_ownership.py \
    --neverd build-mobile/bin/neverd --d8 "$NEVERD_D8" \
    --java-home "$JAVA_HOME" --compiler-java-home "$JAVA_HOME" \
    --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
    --work-dir "$RUNNER_TEMP/android-closed-covariant-slot-ownership-$variant" "${args[@]}"
done

profile_group "Verify interface token results with closed declared exceptions" 25 900
for variant in source sdk; do
  args=()
  if [[ "$variant" == sdk ]]; then args+=(--dependency-provider); fi
  python scripts/test_mobile_android_interface_token_result_throws.py \
    --neverd build-mobile/bin/neverd --d8 "$NEVERD_D8" \
    --java-home "$JAVA_HOME" --compiler-java-home "$JAVA_HOME" \
    --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
    --work-dir "$RUNNER_TEMP/android-interface-token-result-throws-$variant" "${args[@]}"
done

profile_group "Verify constructor literal contexts with closed generic tokens" 25 900
for variant in source sdk; do
  args=()
  if [[ "$variant" == sdk ]]; then args+=(--dependency-provider); fi
  python scripts/test_mobile_android_literal_token_constructor.py \
    --neverd build-mobile/bin/neverd --d8 "$NEVERD_D8" \
    --java-home "$JAVA_HOME" --compiler-java-home "$JAVA_HOME" \
    --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
    --work-dir "$RUNNER_TEMP/android-literal-token-constructor-$variant" "${args[@]}"
done

profile_group "Verify complete generic null superclass bindings" 25 900
for variant in source sdk; do
  args=()
  if [[ "$variant" == sdk ]]; then args+=(--dependency-provider); fi
  python scripts/test_mobile_android_closed_null_super_binding.py \
    --neverd build-mobile/bin/neverd --d8 "$NEVERD_D8" \
    --java-home "$JAVA_HOME" --compiler-java-home "$JAVA_HOME" \
    --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
    --work-dir "$RUNNER_TEMP/android-closed-null-super-binding-$variant" "${args[@]}"
done

profile_group "Verify checked exceptions for closed interface seeds" 25 900
for variant in source sdk; do
  args=()
  if [[ "$variant" == sdk ]]; then args+=(--dependency-provider); fi
  python scripts/test_mobile_android_closed_interface_seed_throws.py \
    --neverd build-mobile/bin/neverd --d8 "$NEVERD_D8" \
    --java-home "$JAVA_HOME" \
    --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
    --work-dir "$RUNNER_TEMP/android-closed-interface-seed-throws-$variant" "${args[@]}"
done

profile_group "Verify checked exceptions for closed class seeds" 25 900
for variant in source sdk; do
  args=()
  if [[ "$variant" == sdk ]]; then args+=(--dependency-provider); fi
  python scripts/test_mobile_android_closed_class_seed_throws.py \
    --neverd build-mobile/bin/neverd --d8 "$NEVERD_D8" \
    --java-home "$JAVA_HOME" \
    --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
    --work-dir "$RUNNER_TEMP/android-closed-class-seed-throws-$variant" "${args[@]}"
done

profile_group "Verify nested array inputs and original row results" 25 900
for variant in source sdk; do
  args=()
  if [[ "$variant" == sdk ]]; then args+=(--dependency-provider); fi
  python scripts/test_mobile_android_nested_array_result.py \
    --neverd build-mobile/bin/neverd --d8 "$NEVERD_D8" \
    --java-home "$JAVA_HOME" \
    --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
    --work-dir "$RUNNER_TEMP/android-nested-array-result-$variant" "${args[@]}"
done

profile_group "Verify closed nominal arrays and fixed constructor selection" 25 900
for variant in source sdk; do
  args=()
  if [[ "$variant" == sdk ]]; then args+=(--dependency-provider); fi
  python scripts/test_mobile_android_closed_nominal_array_constructor.py \
    --neverd build-mobile/bin/neverd --d8 "$NEVERD_D8" \
    --java-home "$JAVA_HOME" --compiler-java-home "$JAVA_HOME" \
    --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
    --work-dir "$RUNNER_TEMP/android-closed-nominal-array-constructor-$variant" "${args[@]}"
done

profile_group "Verify private context callback checked exceptions" 25 900
for variant in source sdk; do
  for caller_throws in declared empty; do
    args=()
    if [[ "$variant" == sdk ]]; then args+=(--dependency-provider); fi
    if [[ "$caller_throws" == empty ]]; then args+=(--omit-caller-throws); fi
    python scripts/test_mobile_android_private_context_callback_throws.py \
      --neverd build-mobile/bin/neverd --d8 "$NEVERD_D8" \
      --java-home "$JAVA_HOME" --compiler-java-home "$JAVA_HOME" \
      --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
      --work-dir "$RUNNER_TEMP/android-private-context-callback-throws-$variant-$caller_throws" "${args[@]}"
  done
done

profile_group "Verify nominal bounds on superclass constructor results" 25 900
for variant in source sdk; do
  args=()
  if [[ "$variant" == sdk ]]; then args+=(--dependency-provider); fi
  python scripts/test_mobile_android_bound_nominal_super_results.py \
    --neverd build-mobile/bin/neverd --d8 "$NEVERD_D8" \
    --java-home "$JAVA_HOME" --compiler-java-home "$JAVA_HOME" \
    --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
    --work-dir "$RUNNER_TEMP/android-bound-nominal-super-results-$variant" "${args[@]}"
done

profile_group "Verify protected single final constructor assignments" 25 900
for variant in source sdk; do
  args=()
  if [[ "$variant" == sdk ]]; then args+=(--dependency-provider); fi
  python scripts/test_mobile_android_protected_single_final_store.py \
    --neverd build-mobile/bin/neverd --d8 "$NEVERD_D8" \
    --java-home "$JAVA_HOME" --compiler-java-home "$JAVA_HOME" \
    --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
    --work-dir "$RUNNER_TEMP/android-protected-single-final-store-$variant" "${args[@]}"
done

profile_group "Verify closed covariant result bounds and checked exceptions" 25 900
for variant in source sdk; do
  args=()
  if [[ "$variant" == sdk ]]; then args+=(--dependency-provider); fi
  python scripts/test_mobile_android_closed_covariant_bound_result.py \
    --neverd build-mobile/bin/neverd --d8 "$NEVERD_D8" \
    --java-home "$JAVA_HOME" --compiler-java-home "$JAVA_HOME" \
    --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
    --work-dir "$RUNNER_TEMP/android-closed-covariant-bound-result-$variant" "${args[@]}"
done

profile_group "Verify independent generic results beside closed input contexts" 25 1200
python scripts/test_mobile_android_closed_context_result.py \
  --neverd build-mobile/bin/neverd --d8 "$NEVERD_D8" --java-home "$JAVA_HOME" \
  --compiler-java-home "$JAVA_HOME" --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
  --work-dir "$RUNNER_TEMP/android-closed-context-result"
python scripts/test_mobile_android_closed_context_result.py --dependency-provider \
  --neverd build-mobile/bin/neverd --d8 "$NEVERD_D8" --java-home "$JAVA_HOME" \
  --compiler-java-home "$JAVA_HOME" --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
  --work-dir "$RUNNER_TEMP/android-closed-context-result-sdk"

profile_group "Verify array lengths in conditional constructor arguments" 25 600
python scripts/test_mobile_android_conditional_array_lengths.py \
  --neverd build-mobile/bin/neverd --d8 "$NEVERD_D8" --java-home "$JAVA_HOME" \
  --compiler-java-home "$JAVA_HOME" --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
  --work-dir "$RUNNER_TEMP/android-conditional-array-lengths"
python scripts/test_mobile_android_conditional_array_lengths.py --dependency-target \
  --neverd build-mobile/bin/neverd --d8 "$NEVERD_D8" --java-home "$JAVA_HOME" \
  --compiler-java-home "$JAVA_HOME" --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
  --work-dir "$RUNNER_TEMP/android-conditional-array-lengths-sdk"

profile_group "Verify member bindings with closed head results and checked exceptions" 25 1200
python scripts/test_mobile_android_closed_head_member.py \
  --neverd build-mobile/bin/neverd --d8 "$NEVERD_D8" --java-home "$JAVA_HOME" \
  --compiler-java-home "$JAVA_HOME" --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
  --work-dir "$RUNNER_TEMP/android-closed-head-member"
python scripts/test_mobile_android_closed_head_member.py --dependency-provider \
  --neverd build-mobile/bin/neverd --d8 "$NEVERD_D8" --java-home "$JAVA_HOME" \
  --compiler-java-home "$JAVA_HOME" --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
  --work-dir "$RUNNER_TEMP/android-closed-head-member-sdk"

profile_group "Verify static final stores with resource cleanup and suppressed failures" 25 900
python scripts/test_mobile_android_static_final_resource.py \
  --neverd build-mobile/bin/neverd --d8 "$NEVERD_D8" --java-home "$JAVA_HOME" \
  --compiler-java-home "$JAVA_HOME" --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
  --work-dir "$RUNNER_TEMP/android-static-final-resource"
python scripts/test_mobile_android_static_final_resource.py --dependency-provider \
  --neverd build-mobile/bin/neverd --d8 "$NEVERD_D8" --java-home "$JAVA_HOME" \
  --compiler-java-home "$JAVA_HOME" --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
  --work-dir "$RUNNER_TEMP/android-static-final-resource-sdk"

profile_group "Verify method seeds beside reifiable arrays and nested results" 25 1200
python scripts/test_mobile_android_array_star_nested_member.py \
  --neverd build-mobile/bin/neverd --d8 "$NEVERD_D8" --java-home "$JAVA_HOME" \
  --compiler-java-home "$JAVA_HOME" --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
  --work-dir "$RUNNER_TEMP/android-array-star-nested-member"
python scripts/test_mobile_android_array_star_nested_member.py --dependency-provider \
  --neverd build-mobile/bin/neverd --d8 "$NEVERD_D8" --java-home "$JAVA_HOME" \
  --compiler-java-home "$JAVA_HOME" --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
  --work-dir "$RUNNER_TEMP/android-array-star-nested-member-sdk"

profile_group "Verify protected static factories retain closed inputs and exceptions" 25 1200
python scripts/test_mobile_android_reifiable_context_throws.py --protected-provider \
  --neverd build-mobile/bin/neverd --d8 "$NEVERD_D8" --java-home "$JAVA_HOME" \
  --compiler-java-home "$JAVA_HOME" --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
  --work-dir "$RUNNER_TEMP/android-protected-static-result"
python scripts/test_mobile_android_reifiable_context_throws.py --protected-provider --dependency-provider \
  --neverd build-mobile/bin/neverd --d8 "$NEVERD_D8" --java-home "$JAVA_HOME" \
  --compiler-java-home "$JAVA_HOME" --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
  --work-dir "$RUNNER_TEMP/android-protected-static-result-sdk"

profile_group "Verify null generic constructor arguments and closed exceptions" 21 600
python scripts/test_mobile_android_null_constructor_arguments.py --closed-throws \
  --neverd build-mobile/bin/neverd --d8 "$NEVERD_D8" --java-home "$JAVA_HOME" \
  --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
  --work-dir "$RUNNER_TEMP/android-null-constructor-throws"
python scripts/test_mobile_android_null_constructor_arguments.py --closed-throws --dependency-provider \
  --neverd build-mobile/bin/neverd --d8 "$NEVERD_D8" --java-home "$JAVA_HOME" \
  --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
  --work-dir "$RUNNER_TEMP/android-null-constructor-throws-sdk"

profile_group "Verify protected static tokens and invariant carrier results" 25 1200
python scripts/test_mobile_android_context_carrier_token.py --protected-provider \
  --neverd build-mobile/bin/neverd --d8 "$NEVERD_D8" --java-home "$JAVA_HOME" \
  --compiler-java-home "$JAVA_HOME" --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
  --work-dir "$RUNNER_TEMP/android-protected-carrier-token"
python scripts/test_mobile_android_context_carrier_token.py --protected-provider --dependency-provider \
  --neverd build-mobile/bin/neverd --d8 "$NEVERD_D8" --java-home "$JAVA_HOME" \
  --compiler-java-home "$JAVA_HOME" --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
  --work-dir "$RUNNER_TEMP/android-protected-carrier-token-sdk"

profile_group "Verify scoped interface default tokens and closed exceptions" 25 1200
python scripts/test_mobile_android_interface_token_result_throws.py --scoped-default \
  --neverd build-mobile/bin/neverd --d8 "$NEVERD_D8" --java-home "$JAVA_HOME" \
  --compiler-java-home "$JAVA_HOME" --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
  --work-dir "$RUNNER_TEMP/android-scoped-interface-token-throws"
python scripts/test_mobile_android_interface_token_result_throws.py --scoped-default --dependency-provider \
  --neverd build-mobile/bin/neverd --d8 "$NEVERD_D8" --java-home "$JAVA_HOME" \
  --compiler-java-home "$JAVA_HOME" --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
  --work-dir "$RUNNER_TEMP/android-scoped-interface-token-throws-sdk"

profile_group "Verify straight constructor type tests preserve call order" 25 1200
python scripts/test_mobile_android_instanceof_constructor_prefix.py --linear-prefix \
  --neverd build-mobile/bin/neverd --d8 "$NEVERD_D8" --java-home "$JAVA_HOME" \
  --compiler-java-home "$NEVERD_JAVA21_HOME" --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
  --work-dir "$RUNNER_TEMP/android-linear-constructor-type-test"
python scripts/test_mobile_android_instanceof_constructor_prefix.py --linear-prefix --sdk-provider \
  --neverd build-mobile/bin/neverd --d8 "$NEVERD_D8" --java-home "$JAVA_HOME" \
  --compiler-java-home "$NEVERD_JAVA21_HOME" --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
  --work-dir "$RUNNER_TEMP/android-linear-constructor-type-test-sdk"

python scripts/test_mobile_android_object_root_covariance.py --narrowed-child \
  --neverd build-mobile/bin/neverd --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
  --java-home "$NEVERD_JAVA25_HOME" --d8 "$NEVERD_D8" \
  --work-dir "$RUNNER_TEMP/android-generic-parent-result-bridges"
python scripts/test_mobile_android_object_root_covariance.py --narrowed-child --sdk-parent \
  --neverd build-mobile/bin/neverd --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
  --java-home "$NEVERD_JAVA25_HOME" --d8 "$NEVERD_D8" \
  --work-dir "$RUNNER_TEMP/android-generic-parent-result-bridges-sdk"

python scripts/test_mobile_android_constructor_overload_argument.py --nominal-sibling \
  --neverd build-mobile/bin/neverd --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
  --java-home "$NEVERD_JAVA25_HOME" --compiler-java-home "$NEVERD_JAVA21_HOME" --d8 "$NEVERD_D8" \
  --work-dir "$RUNNER_TEMP/android-closed-class-constructor-overloads"
python scripts/test_mobile_android_constructor_overload_argument.py --nominal-sibling --dependency-provider \
  --neverd build-mobile/bin/neverd --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
  --java-home "$NEVERD_JAVA25_HOME" --compiler-java-home "$NEVERD_JAVA21_HOME" --d8 "$NEVERD_D8" \
  --work-dir "$RUNNER_TEMP/android-closed-class-constructor-overloads-sdk"

python scripts/test_mobile_android_constructor_overload_argument.py --ancestor-input \
  --neverd build-mobile/bin/neverd --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
  --java-home "$NEVERD_JAVA25_HOME" --compiler-java-home "$NEVERD_JAVA21_HOME" --d8 "$NEVERD_D8" \
  --work-dir "$RUNNER_TEMP/android-ancestor-constructor-overloads"
python scripts/test_mobile_android_constructor_overload_argument.py --ancestor-input --dependency-provider \
  --neverd build-mobile/bin/neverd --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
  --java-home "$NEVERD_JAVA25_HOME" --compiler-java-home "$NEVERD_JAVA21_HOME" --d8 "$NEVERD_D8" \
  --work-dir "$RUNNER_TEMP/android-ancestor-constructor-overloads-sdk"

for variant in source sdk; do
  args=()
  if [[ "$variant" == sdk ]]; then args+=(--dependency-provider); fi
  python scripts/test_mobile_android_nominal_super_result.py --closed-reference \
    --neverd build-mobile/bin/neverd --d8 "$NEVERD_D8" \
    --java-home "$JAVA_HOME" \
    --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
    --work-dir "$RUNNER_TEMP/android-closed-super-reference-result-$variant" "${args[@]}"
done

for dispatch in static instance; do
  for variant in source sdk; do
    args=(--string-context)
    if [[ "$dispatch" == instance ]]; then args+=(--instance-provider); fi
    if [[ "$variant" == sdk ]]; then args+=(--dependency-provider); fi
    python scripts/test_mobile_android_enum_token_default.py \
      --neverd build-mobile/bin/neverd --d8 "$NEVERD_D8" \
      --java-home "$JAVA_HOME" \
      --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
      --work-dir "$RUNNER_TEMP/android-enum-string-default-$dispatch-$variant" "${args[@]}"
  done
done

for variant in source sdk; do
  args=()
  if [[ "$variant" == sdk ]]; then args+=(--dependency-provider); fi
  python scripts/test_mobile_android_anchored_seed_nominal.py \
    --neverd build-mobile/bin/neverd --d8 "$NEVERD_D8" \
    --java-home "$JAVA_HOME" \
    --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
    --work-dir "$RUNNER_TEMP/android-anchored-seed-nominal-$variant" "${args[@]}"
done

for variant in source sdk; do
  args=(--stateful-marker)
  if [[ "$variant" == sdk ]]; then args+=(--dependency-inputs); fi
  python scripts/test_mobile_android_closed_access_marker.py \
    --neverd build-mobile/bin/neverd --d8 "$NEVERD_D8" \
    --java-home "$JAVA_HOME" \
    --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
    --work-dir "$RUNNER_TEMP/android-stateful-access-marker-$variant" "${args[@]}"
done

profile_group "Execute long allocation handler continuations" 25 600
python scripts/test_mobile_android_allocation_handler_gap.py --long-cleanup \
  --neverd build-mobile/bin/neverd --java-home "$JAVA_HOME" \
  --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" --d8 "$NEVERD_D8" \
  --work-dir "$RUNNER_TEMP/android-long-allocation-cleanup"
python scripts/test_mobile_android_allocation_handler_gap.py --long-cleanup --dependency-inputs \
  --neverd build-mobile/bin/neverd --java-home "$JAVA_HOME" \
  --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" --d8 "$NEVERD_D8" \
  --work-dir "$RUNNER_TEMP/android-long-allocation-cleanup-sdk"

profile_group "Execute checked closed access constructors" 25 600
for marker in empty stateful; do
  for variant in source sdk; do
    args=(--checked-throws)
    if [[ "$marker" == stateful ]]; then args+=(--stateful-marker); fi
    if [[ "$variant" == sdk ]]; then args+=(--dependency-inputs); fi
    python scripts/test_mobile_android_closed_access_marker.py \
      --neverd build-mobile/bin/neverd --d8 "$NEVERD_D8" \
      --java-home "$JAVA_HOME" \
      --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
      --work-dir "$RUNNER_TEMP/android-checked-access-marker-$marker-$variant" "${args[@]}"
  done
done

profile_group "Execute nested closed access constructors" 25 600
for exceptions in unchecked checked; do
  for variant in source sdk; do
    args=(--stateful-marker --nested-owner)
    if [[ "$exceptions" == checked ]]; then args+=(--checked-throws); fi
    if [[ "$variant" == sdk ]]; then args+=(--dependency-inputs); fi
    python scripts/test_mobile_android_closed_access_marker.py \
      --neverd build-mobile/bin/neverd --d8 "$NEVERD_D8" \
      --java-home "$JAVA_HOME" \
      --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
      --work-dir "$RUNNER_TEMP/android-nested-access-marker-$exceptions-$variant" "${args[@]}"
  done
done

profile_group "Execute forward allocation argument tails" 21 600
for variant in source sdk; do
  args=(--argument-tail)
  if [[ "$variant" == sdk ]]; then args+=(--dependency-inputs); fi
  python scripts/test_mobile_android_forward_allocation.py \
    --neverd build-mobile/bin/neverd --d8 "$NEVERD_D8" \
    --java-home "$JAVA_HOME" \
    --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
    --work-dir "$RUNNER_TEMP/android-forward-argument-tail-$variant" "${args[@]}"
done

profile_group "Execute array seed comparator bindings" 25 600
for variant in source sdk; do
  args=(--seed-comparator)
  if [[ "$variant" == sdk ]]; then args+=(--dependency-provider); fi
  python scripts/test_mobile_android_array_object_scalar.py \
    --neverd build-mobile/bin/neverd --d8 "$NEVERD_D8" \
    --java-home "$JAVA_HOME" \
    --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
    --work-dir "$RUNNER_TEMP/android-array-seed-comparator-$variant" "${args[@]}"
done

profile_group "Execute instance bounded token checked throws" 25 600
for variant in source sdk; do
  args=(--instance-member)
  if [[ "$variant" == sdk ]]; then args+=(--dependency-provider); fi
  python scripts/test_mobile_android_interface_token_context.py \
    --neverd build-mobile/bin/neverd --d8 "$NEVERD_D8" \
    --java-home "$JAVA_HOME" \
    --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
    --work-dir "$RUNNER_TEMP/android-instance-bound-token-$variant" "${args[@]}"
done

profile_group "Execute bounded scalar carrier transfers" 25 600
for variant in source sdk; do
  args=()
  if [[ "$variant" == sdk ]]; then args+=(--dependency-provider); fi
  python scripts/test_mobile_android_bounded_scalar_transfer.py \
    --neverd build-mobile/bin/neverd --d8 "$NEVERD_D8" \
    --java-home "$JAVA_HOME" \
    --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
    --work-dir "$RUNNER_TEMP/android-bounded-scalar-transfer-$variant" "${args[@]}"
done

profile_group "Execute private sibling constructor types" 25 600
for variant in source sdk; do
  args=()
  if [[ "$variant" == sdk ]]; then args+=(--dependency-inputs); fi
  python scripts/test_mobile_android_private_sibling_constructor.py \
    --neverd build-mobile/bin/neverd --d8 "$NEVERD_D8" \
    --java-home "$JAVA_HOME" \
    --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
    --work-dir "$RUNNER_TEMP/android-private-sibling-constructor-$variant" "${args[@]}"
done

profile_group "Execute invariant token carriers with checked throws" 25 600
for variant in source sdk; do
  args=()
  if [[ "$variant" == sdk ]]; then args+=(--dependency-provider); fi
  python scripts/test_mobile_android_token_carrier_throws.py \
    --neverd build-mobile/bin/neverd --d8 "$NEVERD_D8" \
    --java-home "$JAVA_HOME" \
    --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
    --work-dir "$RUNNER_TEMP/android-token-carrier-throws-$variant" "${args[@]}"
done

profile_group "Execute private token carriers with checked throws" 25 600
for variant in source sdk; do
  args=(--private-member)
  if [[ "$variant" == sdk ]]; then args+=(--dependency-provider); fi
  python scripts/test_mobile_android_token_carrier_throws.py \
    --neverd build-mobile/bin/neverd --d8 "$NEVERD_D8" \
    --java-home "$JAVA_HOME" \
    --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
    --work-dir "$RUNNER_TEMP/android-private-token-carrier-throws-$variant" "${args[@]}"
done

profile_group "Execute exception-only monitor releases" 25 600
for variant in source sdk; do
  args=()
  if [[ "$variant" == sdk ]]; then args+=(--dependency-provider); fi
  python scripts/test_mobile_android_exception_only_monitor.py \
    --neverd build-mobile/bin/neverd --d8 "$NEVERD_D8" \
    --java-home "$JAVA_HOME" \
    --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
    --work-dir "$RUNNER_TEMP/android-exception-only-monitor-$variant" "${args[@]}"
done

profile_group "Execute array lengths before constructor delegation" 25 600
for variant in source sdk; do
  args=()
  if [[ "$variant" == sdk ]]; then args+=(--dependency-provider); fi
  python scripts/test_mobile_android_array_length_constructor_prefix.py \
    --neverd build-mobile/bin/neverd --d8 "$NEVERD_D8" \
    --java-home "$JAVA_HOME" \
    --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
    --work-dir "$RUNNER_TEMP/android-array-length-constructor-$variant" "${args[@]}"
done

profile_group "Execute conditional array arguments" 25 600
for variant in source sdk; do
  args=()
  if [[ "$variant" == sdk ]]; then args+=(--dependency-provider); fi
  python scripts/test_mobile_android_conditional_array_arguments.py \
    --neverd build-mobile/bin/neverd --d8 "$NEVERD_D8" \
    --java-home "$JAVA_HOME" \
    --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
    --work-dir "$RUNNER_TEMP/android-conditional-array-arguments-$variant" "${args[@]}"
done

profile_group "Execute unary callbacks with closed carrier results" 21 600
for input_shape in covariant invariant; do
  for variant in source sdk; do
    args=(--unary-callback)
    if [[ "$input_shape" == invariant ]]; then args+=(--invariant-input); fi
    if [[ "$variant" == sdk ]]; then args+=(--dependency-provider); fi
    python scripts/test_mobile_android_binary_callback_carrier.py \
      --neverd build-mobile/bin/neverd --d8 "$NEVERD_D8" \
      --java-home "$JAVA_HOME" \
      --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
      --work-dir "$RUNNER_TEMP/android-unary-callback-carrier-$input_shape-$variant" "${args[@]}"
  done
done

profile_group "Execute recursive pair contracts with opaque formal results" 21 600
for variant in source sdk; do
  args=()
  if [[ "$variant" == sdk ]]; then args+=(--dependency-provider); fi
  python scripts/test_mobile_android_recursive_pair_result.py \
    --neverd build-mobile/bin/neverd --d8 "$NEVERD_D8" \
    --java-home "$JAVA_HOME" \
    --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
    --work-dir "$RUNNER_TEMP/android-recursive-pair-result-$variant" "${args[@]}"
done

profile_group "Execute bounded static factories with independent class and method scopes" 21 600
for variant in source sdk; do
  args=()
  if [[ "$variant" == sdk ]]; then args+=(--dependency-provider); fi
  python scripts/test_mobile_android_bounded_static_self_result.py \
    --neverd build-mobile/bin/neverd --d8 "$NEVERD_D8" \
    --java-home "$JAVA_HOME" \
    --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
    --work-dir "$RUNNER_TEMP/android-bounded-static-self-result-$variant" "${args[@]}"
done

profile_group "Execute recursive array contracts with opaque invariant carrier results" 21 600
for variant in source sdk; do
  args=()
  if [[ "$variant" == sdk ]]; then args+=(--dependency-provider); fi
  python scripts/test_mobile_android_recursive_array_carrier_result.py \
    --neverd build-mobile/bin/neverd --d8 "$NEVERD_D8" \
    --java-home "$JAVA_HOME" \
    --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
    --work-dir "$RUNNER_TEMP/android-recursive-array-carrier-result-$variant" "${args[@]}"
done

profile_group "Execute nullary constructors with closed generic superclass tokens" 25 600
for variant in source sdk; do
  args=()
  if [[ "$variant" == sdk ]]; then args+=(--dependency-provider); fi
  python scripts/test_mobile_android_nullary_token_super_constructor.py \
    --neverd build-mobile/bin/neverd --d8 "$NEVERD_D8" \
    --java-home "$JAVA_HOME" \
    --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
    --work-dir "$RUNNER_TEMP/android-nullary-token-super-constructor-$variant" "${args[@]}"
done

profile_group "Execute inherited superclass calls with bounded child formals" 21 600
for variant in source sdk; do
  args=()
  if [[ "$variant" == sdk ]]; then args+=(--dependency-provider); fi
  python scripts/test_mobile_android_bounded_formal_super_call.py \
    --neverd build-mobile/bin/neverd --d8 "$NEVERD_D8" \
    --java-home "$JAVA_HOME" \
    --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
    --work-dir "$RUNNER_TEMP/android-bounded-formal-super-call-$variant" "${args[@]}"
done

profile_group "Execute protected nested superclass visibility bridges" 21 600
for variant in source sdk; do
  args=()
  if [[ "$variant" == sdk ]]; then args+=(--dependency-provider); fi
  python scripts/test_mobile_android_nested_visibility_super_bridge.py \
    --neverd build-mobile/bin/neverd --d8 "$NEVERD_D8" \
    --java-home "$JAVA_HOME" \
    --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
    --work-dir "$RUNNER_TEMP/android-nested-visibility-super-bridge-$variant" "${args[@]}"
done

profile_group "Execute generic superclass arguments during pending allocation" 21 600
for variant in source sdk; do
  args=()
  if [[ "$variant" == sdk ]]; then args+=(--dependency-provider); fi
  python scripts/test_mobile_android_nested_generic_super_arguments.py \
    --neverd build-mobile/bin/neverd --d8 "$NEVERD_D8" \
    --java-home "$JAVA_HOME" \
    --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
    --work-dir "$RUNNER_TEMP/android-nested-generic-super-arguments-$variant" "${args[@]}"
done

profile_group "Execute closed constructors with owned package member elements" 21 600
for variant in source sdk; do
  args=(--package-element)
  if [[ "$variant" == sdk ]]; then args+=(--dependency-inputs); fi
  python scripts/test_mobile_android_closed_access_marker.py \
    --neverd build-mobile/bin/neverd --d8 "$NEVERD_D8" \
    --java-home "$JAVA_HOME" \
    --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
    --work-dir "$RUNNER_TEMP/android-owned-member-carrier-constructor-$variant" "${args[@]}"
done

profile_group "Execute non-first intersection arguments with scalar context" 21 600
for variant in source sdk; do
  args=()
  if [[ "$variant" == sdk ]]; then args+=(--dependency-provider); fi
  python scripts/test_mobile_android_scalar_intersection_argument.py \
    --neverd build-mobile/bin/neverd --d8 "$NEVERD_D8" \
    --java-home "$JAVA_HOME" \
    --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
    --work-dir "$RUNNER_TEMP/android-scalar-intersection-argument-$variant" "${args[@]}"
done

profile_group "Execute abrupt constructors with blank final fields" 21 600
for variant in source sdk; do
  args=()
  if [[ "$variant" == sdk ]]; then args+=(--dependency-provider); fi
  python scripts/test_mobile_android_abrupt_blank_final_constructor.py \
    --neverd build-mobile/bin/neverd --d8 "$NEVERD_D8" \
    --java-home "$JAVA_HOME" \
    --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
    --work-dir "$RUNNER_TEMP/android-abrupt-blank-final-constructor-$variant" "${args[@]}"
done

profile_group "Execute mapped consumer factories and platform comparisons" 21 600
for variant in source sdk; do
  args=()
  if [[ "$variant" == sdk ]]; then args+=(--dependency-provider); fi
  python scripts/test_mobile_android_mapped_consumer_factory.py \
    --neverd build-mobile/bin/neverd --d8 "$NEVERD_D8" \
    --java-home "$JAVA_HOME" \
    --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
    --work-dir "$RUNNER_TEMP/android-mapped-consumer-factory-$variant" "${args[@]}"
done

profile_group "Execute original anonymous bodies reused as access markers" 21 600
for variant in source sdk; do
  args=()
  if [[ "$variant" == sdk ]]; then args+=(--dependency-inputs); fi
  python scripts/test_mobile_android_stateless_access_marker.py \
    --neverd build-mobile/bin/neverd --d8 "$NEVERD_D8" \
    --java-home "$JAVA_HOME" \
    --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
    --work-dir "$RUNNER_TEMP/android-stateless-access-marker-$variant" "${args[@]}"
done

profile_group "Execute independent static carrier scopes with fixed contexts" 21 600
for variant in source sdk; do
  args=()
  if [[ "$variant" == sdk ]]; then args+=(--dependency-provider); fi
  python scripts/test_mobile_android_independent_carrier_factory.py \
    --neverd build-mobile/bin/neverd --d8 "$NEVERD_D8" \
    --java-home "$JAVA_HOME" \
    --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
    --work-dir "$RUNNER_TEMP/android-independent-carrier-factory-$variant" "${args[@]}"
done

profile_group "Execute captured integer constructor choices" 21 600
for variant in source sdk; do
  for polarity in positive nonpositive; do
    args=()
    if [[ "$variant" == sdk ]]; then args+=(--dependency-widget); fi
    if [[ "$polarity" == nonpositive ]]; then args+=(--argument-on-nonpositive); fi
    python scripts/test_mobile_android_integer_constructor_choice.py \
      --neverd build-mobile/bin/neverd --d8 "$NEVERD_D8" \
      --java-home "$JAVA_HOME" \
      --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
      --work-dir "$RUNNER_TEMP/android-integer-constructor-choice-$variant-$polarity" "${args[@]}"
  done
done

profile_group "Execute final superclass parameter bridge targets" 21 600
for variant in source sdk; do
  args=()
  if [[ "$variant" == sdk ]]; then args+=(--dependency-parents); fi
  python scripts/test_mobile_android_final_superclass_parameter_bridge.py \
    --neverd build-mobile/bin/neverd --d8 "$NEVERD_D8" \
    --java-home "$JAVA_HOME" \
    --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
    --work-dir "$RUNNER_TEMP/android-final-superclass-parameter-bridge-$variant" "${args[@]}"
done

profile_group "Execute package-visible independent carrier factories" 21 600
for variant in source sdk; do
  args=()
  if [[ "$variant" == sdk ]]; then args+=(--dependency-provider); fi
  python scripts/test_mobile_android_package_carrier_factory.py \
    --neverd build-mobile/bin/neverd --d8 "$NEVERD_D8" \
    --java-home "$JAVA_HOME" \
    --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
    --work-dir "$RUNNER_TEMP/android-package-carrier-factory-$variant" "${args[@]}"
done

profile_group "Execute directly seeded wildcard property bindings" 21 600
for variant in source sdk; do
  args=()
  if [[ "$variant" == sdk ]]; then args+=(--dependency-provider); fi
  python scripts/test_mobile_android_seeded_star_property.py \
    --neverd build-mobile/bin/neverd --d8 "$NEVERD_D8" \
    --java-home "$JAVA_HOME" \
    --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
    --work-dir "$RUNNER_TEMP/android-seeded-star-property-$variant" "${args[@]}"
done

profile_group "Execute closed anonymous superclass bridge contracts" 25 600
for variant in source sdk; do
  args=()
  if [[ "$variant" == sdk ]]; then args+=(--dependency-provider); fi
  python scripts/test_mobile_android_anonymous_closed_parent_bridge.py \
    --neverd build-mobile/bin/neverd --d8 "$NEVERD_D8" \
    --java-home "$JAVA_HOME" --compiler-java-home "$NEVERD_JAVA21_HOME" \
    --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
    --work-dir "$RUNNER_TEMP/android-anonymous-closed-parent-bridge-$variant" "${args[@]}"
done

profile_group "Execute method-owned intersection constructor witnesses" 25 600
for variant in source sdk; do
  args=()
  if [[ "$variant" == sdk ]]; then args+=(--dependency-provider); fi
  python scripts/test_mobile_android_intersection_constructor_witness.py \
    --neverd build-mobile/bin/neverd --d8 "$NEVERD_D8" \
    --java-home "$JAVA_HOME" --compiler-java-home "$NEVERD_JAVA21_HOME" \
    --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
    --work-dir "$RUNNER_TEMP/android-intersection-constructor-witness-$variant" "${args[@]}"
done

profile_group "Execute seeded generic constructor carriers" 25 600
for variant in source sdk; do
  args=()
  if [[ "$variant" == sdk ]]; then args+=(--dependency-provider); fi
  python scripts/test_mobile_android_seeded_carrier_constructor.py \
    --neverd build-mobile/bin/neverd --d8 "$NEVERD_D8" \
    --java-home "$JAVA_HOME" --compiler-java-home "$NEVERD_JAVA21_HOME" \
    --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
    --work-dir "$RUNNER_TEMP/android-seeded-carrier-constructor-$variant" "${args[@]}"
done

profile_group "Execute package abstract bounded scalar calls" 25 600
for variant in source sdk; do
  args=()
  if [[ "$variant" == sdk ]]; then args+=(--dependency-provider); fi
  python scripts/test_mobile_android_package_bound_scalar.py \
    --neverd build-mobile/bin/neverd --d8 "$NEVERD_D8" \
    --java-home "$JAVA_HOME" --compiler-java-home "$NEVERD_JAVA21_HOME" \
    --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
    --work-dir "$RUNNER_TEMP/android-package-bound-scalar-$variant" "${args[@]}"
done

profile_group "Execute closed superclass visibility parameter bridges" 25 600
for variant in source sdk; do
  args=()
  if [[ "$variant" == sdk ]]; then args+=(--dependency-provider); fi
  python scripts/test_mobile_android_closed_visibility_parameter_bridge.py \
    --neverd build-mobile/bin/neverd --d8 "$NEVERD_D8" \
    --java-home "$JAVA_HOME" --compiler-java-home "$NEVERD_JAVA21_HOME" \
    --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
    --work-dir "$RUNNER_TEMP/android-closed-visibility-parameter-bridge-$variant" "${args[@]}"
done

profile_group "Execute nested anonymous constructor arguments" 25 600
for variant in source sdk; do
  for shape in linear conditional; do
    args=()
    if [[ "$variant" == sdk ]]; then args+=(--dependency-provider); fi
    if [[ "$shape" == conditional ]]; then args+=(--conditional); fi
    python scripts/test_mobile_android_nested_anonymous_allocation.py \
      --neverd build-mobile/bin/neverd --d8 "$NEVERD_D8" \
      --java-home "$JAVA_HOME" --compiler-java-home "$NEVERD_JAVA21_HOME" \
      --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
      --work-dir "$RUNNER_TEMP/android-nested-anonymous-allocation-$variant-$shape" "${args[@]}"
  done
done

profile_group "Execute final nested visibility superclass bridges" 25 600
for variant in source sdk; do
  args=()
  if [[ "$variant" == sdk ]]; then args+=(--dependency-provider); fi
  python scripts/test_mobile_android_final_nested_visibility_bridge.py \
    --neverd build-mobile/bin/neverd --d8 "$NEVERD_D8" \
    --java-home "$JAVA_HOME" --compiler-java-home "$NEVERD_JAVA21_HOME" \
    --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
    --work-dir "$RUNNER_TEMP/android-final-nested-visibility-bridge-$variant" "${args[@]}"
done

profile_group "Execute instance array carrier void invocations" 25 600
for variant in source sdk; do
  args=()
  if [[ "$variant" == sdk ]]; then args+=(--dependency-provider); fi
  python scripts/test_mobile_android_instance_array_carrier_void.py \
    --neverd build-mobile/bin/neverd --d8 "$NEVERD_D8" \
    --java-home "$JAVA_HOME" --compiler-java-home "$NEVERD_JAVA21_HOME" \
    --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
    --work-dir "$RUNNER_TEMP/android-instance-array-carrier-void-$variant" "${args[@]}"
done

profile_group "Execute unused private interface intersection formals" 25 600
for variant in source sdk; do
  args=()
  if [[ "$variant" == sdk ]]; then args+=(--dependency-provider); fi
  python scripts/test_mobile_android_unused_private_intersection.py \
    --neverd build-mobile/bin/neverd --d8 "$NEVERD_D8" \
    --java-home "$JAVA_HOME" --compiler-java-home "$NEVERD_JAVA21_HOME" \
    --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
    --work-dir "$RUNNER_TEMP/android-unused-private-intersection-$variant" "${args[@]}"
done

profile_group "Execute intersection superclass parameter forwarding" 25 600
for variant in source sdk; do
  args=()
  if [[ "$variant" == sdk ]]; then args+=(--dependency-provider); fi
  python scripts/test_mobile_android_intersection_super_forwarding.py \
    --neverd build-mobile/bin/neverd --d8 "$NEVERD_D8" \
    --java-home "$JAVA_HOME" --compiler-java-home "$NEVERD_JAVA21_HOME" \
    --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
    --work-dir "$RUNNER_TEMP/android-intersection-super-forwarding-$variant" "${args[@]}"
done

profile_group "Execute captured member intersection witnesses" 25 600
for variant in source sdk; do
  args=()
  if [[ "$variant" == sdk ]]; then args+=(--dependency-provider); fi
  python scripts/test_mobile_android_captured_intersection_witness.py \
    --neverd build-mobile/bin/neverd --d8 "$NEVERD_D8" \
    --java-home "$JAVA_HOME" --compiler-java-home "$NEVERD_JAVA21_HOME" \
    --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
    --work-dir "$RUNNER_TEMP/android-captured-intersection-witness-$variant" "${args[@]}"
done

profile_group "Execute private constructor prefix factories" 25 600
for variant in source sdk; do
  args=()
  if [[ "$variant" == sdk ]]; then args+=(--dependency-provider); fi
  python scripts/test_mobile_android_private_prefix_constructor_factory.py \
    --neverd build-mobile/bin/neverd --d8 "$NEVERD_D8" \
    --java-home "$JAVA_HOME" --compiler-java-home "$NEVERD_JAVA21_HOME" \
    --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
    --work-dir "$RUNNER_TEMP/android-private-prefix-constructor-factory-$variant" "${args[@]}"
done

profile_group "Execute closed visibility result bridges" 25 600
for variant in source sdk; do
  args=()
  if [[ "$variant" == sdk ]]; then args+=(--dependency-provider); fi
  python scripts/test_mobile_android_closed_visibility_result_bridge.py \
    --neverd build-mobile/bin/neverd --d8 "$NEVERD_D8" \
    --java-home "$JAVA_HOME" --compiler-java-home "$NEVERD_JAVA21_HOME" \
    --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
    --work-dir "$RUNNER_TEMP/android-closed-visibility-result-bridge-$variant" "${args[@]}"
done

profile_group "Preserve inherited defaults across direct interface branches" 21 1200
for variant in source sdk; do
  args=()
  if [[ "$variant" == sdk ]]; then args+=(--dependency-trace); fi
  python scripts/test_mobile_android_branched_interface_super_accessor.py \
    --neverd build-mobile/bin/neverd --java-home "$JAVA_HOME" \
    --compiler-java-home "$NEVERD_JAVA21_HOME" \
    --kotlin-java-home "$NEVERD_KOTLIN_JAVA_HOME" \
    --kotlin-lib "$RUNNER_TEMP/neverd-kotlin-2.0.21/kotlinc/lib" \
    --d8 "$NEVERD_D8" --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
    --work-dir "$RUNNER_TEMP/android-branched-interface-super-accessor-$variant" "${args[@]}"
done

profile_group "Preserve fixed nominal interface default forwarding" 21 1200

for variant in source sdk; do
  args=()
  if [[ "$variant" == sdk ]]; then args+=(--dependency-trace); fi
  python scripts/test_mobile_android_nominal_interface_super_forwarding.py \
    --neverd build-mobile/bin/neverd --java-home "$JAVA_HOME" \
    --d8 "$NEVERD_D8" --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
    --work-dir "$RUNNER_TEMP/android-nominal-interface-super-forwarding-$variant" "${args[@]}"
done

profile_group "Preserve mixed interface default forwarding with class ancestry" 21 1200

for variant in source sdk; do
  args=()
  if [[ "$variant" == sdk ]]; then args+=(--dependency-providers); fi
  python scripts/test_mobile_android_mixed_interface_super_forwarding.py \
    --neverd build-mobile/bin/neverd --java-home "$JAVA_HOME" \
    --d8 "$NEVERD_D8" --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
    --work-dir "$RUNNER_TEMP/android-mixed-interface-super-forwarding-$variant" "${args[@]}"
done

profile_group "Preserve void interface default forwarding without result slots" 21 1200

for variant in source sdk; do
  args=()
  if [[ "$variant" == sdk ]]; then args+=(--dependency-providers); fi
  python scripts/test_mobile_android_void_interface_super_forwarding.py \
    --neverd build-mobile/bin/neverd --java-home "$JAVA_HOME" \
    --d8 "$NEVERD_D8" --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
    --work-dir "$RUNNER_TEMP/android-void-interface-super-forwarding-$variant" "${args[@]}"
done

profile_group "Preserve allocation defaults after linear Object prefixes" 25 1200

for variant in source sdk; do
  args=()
  if [[ "$variant" == sdk ]]; then args+=(--dependency-trace); fi
  python scripts/test_mobile_android_object_prefix_defaults.py \
    --neverd build-mobile/bin/neverd --java-home "$JAVA_HOME" \
    --d8 "$NEVERD_D8" --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
    --work-dir "$RUNNER_TEMP/android-object-prefix-defaults-$variant" "${args[@]}"
done

profile_group "Preserve ordered paired inputs and nested completion callbacks" 21 1200

for variant in source sdk; do
  args=()
  if [[ "$variant" == sdk ]]; then args+=(--dependency-provider); fi
  python scripts/test_mobile_android_paired_seed_completion.py \
    --neverd build-mobile/bin/neverd --java-home "$JAVA_HOME" \
    --d8 "$NEVERD_D8" --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
    --work-dir "$RUNNER_TEMP/android-paired-seed-completion-$variant" "${args[@]}"
done

profile_group "Execute private instance field source names" 21 600
python scripts/test_mobile_android_private_instance_field_names.py \
  --neverd build-mobile/bin/neverd --java-home "$JAVA_HOME" --d8 "$NEVERD_D8" \
  --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
  --work-dir "$RUNNER_TEMP/android-private-instance-field-names"
python scripts/test_mobile_android_private_instance_field_names.py --dependency-observer \
  --neverd build-mobile/bin/neverd --java-home "$JAVA_HOME" --d8 "$NEVERD_D8" \
  --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
  --work-dir "$RUNNER_TEMP/android-private-instance-field-names-sdk"

profile_group "Execute scoped mask constructors with complete interface graphs" 21 600
python scripts/test_mobile_android_scoped_mask_constructors.py --scoped-interfaces \
  --neverd build-mobile/bin/neverd --java-home "$JAVA_HOME" --kotlin-java-home "$NEVERD_KOTLIN_JAVA_HOME" \
  --kotlin-lib "$RUNNER_TEMP/neverd-kotlin-2.0.21/kotlinc/lib" --d8 "$NEVERD_D8" \
  --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
  --work-dir "$RUNNER_TEMP/android-scoped-interface-mask"
python scripts/test_mobile_android_scoped_mask_constructors.py --scoped-interfaces --dependency-defaults \
  --neverd build-mobile/bin/neverd --java-home "$JAVA_HOME" --kotlin-java-home "$NEVERD_KOTLIN_JAVA_HOME" \
  --kotlin-lib "$RUNNER_TEMP/neverd-kotlin-2.0.21/kotlinc/lib" --d8 "$NEVERD_D8" \
  --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
  --work-dir "$RUNNER_TEMP/android-scoped-interface-mask-sdk"

profile_group "Execute long carrier getter words with source trace" 21 600
python scripts/test_mobile_android_carrier_accessor_name.py --primitive long \
  --neverd build-mobile/bin/neverd --java-home "$JAVA_HOME" --d8 "$NEVERD_D8" \
  --kotlin-lib "$RUNNER_TEMP/neverd-kotlin-2.0.21/kotlinc/lib" --kotlin-java-home "$NEVERD_KOTLIN_JAVA_HOME" \
  --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
  --work-dir "$RUNNER_TEMP/android-wide-carrier-accessor-long-source"

profile_group "Execute long carrier getter words with imported trace" 21 600
python scripts/test_mobile_android_carrier_accessor_name.py --primitive long --dependency-trace \
  --neverd build-mobile/bin/neverd --java-home "$JAVA_HOME" --d8 "$NEVERD_D8" \
  --kotlin-lib "$RUNNER_TEMP/neverd-kotlin-2.0.21/kotlinc/lib" --kotlin-java-home "$NEVERD_KOTLIN_JAVA_HOME" \
  --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
  --work-dir "$RUNNER_TEMP/android-wide-carrier-accessor-long-sdk"

profile_group "Execute double carrier getter words with source trace" 21 600
python scripts/test_mobile_android_carrier_accessor_name.py --primitive double \
  --neverd build-mobile/bin/neverd --java-home "$JAVA_HOME" --d8 "$NEVERD_D8" \
  --kotlin-lib "$RUNNER_TEMP/neverd-kotlin-2.0.21/kotlinc/lib" --kotlin-java-home "$NEVERD_KOTLIN_JAVA_HOME" \
  --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
  --work-dir "$RUNNER_TEMP/android-wide-carrier-accessor-double-source"

profile_group "Execute double carrier getter words with imported trace" 21 600
python scripts/test_mobile_android_carrier_accessor_name.py --primitive double --dependency-trace \
  --neverd build-mobile/bin/neverd --java-home "$JAVA_HOME" --d8 "$NEVERD_D8" \
  --kotlin-lib "$RUNNER_TEMP/neverd-kotlin-2.0.21/kotlinc/lib" --kotlin-java-home "$NEVERD_KOTLIN_JAVA_HOME" \
  --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
  --work-dir "$RUNNER_TEMP/android-wide-carrier-accessor-double-sdk"

profile_group "Execute float carrier getter words with imported trace" 21 600
python scripts/test_mobile_android_carrier_accessor_name.py --primitive float --dependency-trace \
  --neverd build-mobile/bin/neverd --java-home "$JAVA_HOME" --d8 "$NEVERD_D8" \
  --kotlin-lib "$RUNNER_TEMP/neverd-kotlin-2.0.21/kotlinc/lib" --kotlin-java-home "$NEVERD_KOTLIN_JAVA_HOME" \
  --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
  --work-dir "$RUNNER_TEMP/android-wide-carrier-accessor-float-sdk"

profile_group "Execute scalar getter names beside a complete inlined interface body (source)" 21 600
python scripts/test_mobile_android_inlined_accessor_names.py \
  --neverd build-mobile/bin/neverd --java-home "$JAVA_HOME" --d8 "$NEVERD_D8" \
  --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
  --work-dir "$RUNNER_TEMP/android-inlined-accessor-names-source"

profile_group "Execute scalar getter names beside a complete inlined interface body (sdk)" 21 600
python scripts/test_mobile_android_inlined_accessor_names.py --dependency-support \
  --neverd build-mobile/bin/neverd --java-home "$JAVA_HOME" --d8 "$NEVERD_D8" \
  --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
  --work-dir "$RUNNER_TEMP/android-inlined-accessor-names-sdk"

profile_group "Execute primitive array getter names beside an erased terminal consumer (source)" 21 600
python scripts/test_mobile_android_terminal_array_accessor_names.py \
  --neverd build-mobile/bin/neverd --java-home "$JAVA_HOME" --d8 "$NEVERD_D8" \
  --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
  --work-dir "$RUNNER_TEMP/android-terminal-array-accessor-names-source"

profile_group "Execute primitive array getter names beside an erased terminal consumer (sdk)" 21 600
python scripts/test_mobile_android_terminal_array_accessor_names.py --dependency-support \
  --neverd build-mobile/bin/neverd --java-home "$JAVA_HOME" --d8 "$NEVERD_D8" \
  --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
  --work-dir "$RUNNER_TEMP/android-terminal-array-accessor-names-sdk"

profile_group "Execute fixed scalar entry names beside an erased terminal consumer (source)" 21 600
python scripts/test_mobile_android_terminal_scalar_entry_names.py \
  --neverd build-mobile/bin/neverd --java-home "$JAVA_HOME" --d8 "$NEVERD_D8" \
  --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
  --work-dir "$RUNNER_TEMP/android-terminal-scalar-entry-names-source"

profile_group "Execute fixed scalar entry names beside an erased terminal consumer (sdk)" 21 600
python scripts/test_mobile_android_terminal_scalar_entry_names.py --dependency-support \
  --neverd build-mobile/bin/neverd --java-home "$JAVA_HOME" --d8 "$NEVERD_D8" \
  --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
  --work-dir "$RUNNER_TEMP/android-terminal-scalar-entry-names-sdk"

profile_group "Execute original boxed result providers with illegal JVM names (source)" 21 600
python scripts/test_mobile_android_boxed_generic_interface_results.py --mangled-target \
  --neverd build-mobile/bin/neverd --java-home "$JAVA_HOME" --d8 "$NEVERD_D8" \
  --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
  --work-dir "$RUNNER_TEMP/android-boxed-result-entry-names-source"

profile_group "Execute original boxed result providers with illegal JVM names (sdk)" 21 600
python scripts/test_mobile_android_boxed_generic_interface_results.py --mangled-target --dependency-root \
  --neverd build-mobile/bin/neverd --java-home "$JAVA_HOME" --d8 "$NEVERD_D8" \
  --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
  --work-dir "$RUNNER_TEMP/android-boxed-result-entry-names-sdk"

profile_group "Execute fixed reference array results with scoped input components (source)" 21 600
python scripts/test_mobile_android_array_fixed_reference_result.py \
  --neverd build-mobile/bin/neverd --java-home "$JAVA_HOME" --d8 "$NEVERD_D8" \
  --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
  --work-dir "$RUNNER_TEMP/android-array-fixed-reference-result-source"

profile_group "Execute fixed reference array results with scoped input components (sdk)" 21 600
python scripts/test_mobile_android_array_fixed_reference_result.py --dependency-provider \
  --neverd build-mobile/bin/neverd --java-home "$JAVA_HOME" --d8 "$NEVERD_D8" \
  --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
  --work-dir "$RUNNER_TEMP/android-array-fixed-reference-result-sdk"

profile_group "Execute private generic array consumers with closed contexts (source)" 21 600
python scripts/test_mobile_android_array_closed_context_void.py \
  --neverd build-mobile/bin/neverd --java-home "$JAVA_HOME" --d8 "$NEVERD_D8" \
  --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
  --work-dir "$RUNNER_TEMP/android-array-closed-context-void-source"

profile_group "Execute private generic array consumers with closed contexts (sdk)" 21 600
python scripts/test_mobile_android_array_closed_context_void.py --dependency-inputs \
  --neverd build-mobile/bin/neverd --java-home "$JAVA_HOME" --d8 "$NEVERD_D8" \
  --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
  --work-dir "$RUNNER_TEMP/android-array-closed-context-void-sdk"

profile_group "Execute package static array factories with invariant results (source)" 21 600
python scripts/test_mobile_android_package_array_factory.py \
  --neverd build-mobile/bin/neverd --java-home "$JAVA_HOME" --d8 "$NEVERD_D8" \
  --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
  --work-dir "$RUNNER_TEMP/android-package-array-factory-source"

profile_group "Execute package static array factories with invariant results (sdk)" 21 600
python scripts/test_mobile_android_package_array_factory.py --dependency-provider \
  --neverd build-mobile/bin/neverd --java-home "$JAVA_HOME" --d8 "$NEVERD_D8" \
  --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
  --work-dir "$RUNNER_TEMP/android-package-array-factory-sdk"

profile_group "Compile and execute bounded destination transfers (source)" 21 600
python scripts/test_mobile_android_destination_bounds.py \
  --neverd build-mobile/bin/neverd --java-home "$JAVA_HOME" --d8 "$NEVERD_D8" \
  --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
  --work-dir "$RUNNER_TEMP/android-destination-bounds-source"

profile_group "Compile and execute bounded destination transfers (sdk)" 21 600
python scripts/test_mobile_android_destination_bounds.py --dependency-provider \
  --neverd build-mobile/bin/neverd --java-home "$JAVA_HOME" --d8 "$NEVERD_D8" \
  --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
  --work-dir "$RUNNER_TEMP/android-destination-bounds-sdk"

profile_group "Compile scoped invocations with source and SDK declarations" 21 1200
python scripts/test_mobile_android_scoped_invocations.py --neverd build-mobile/bin/neverd \
  --d8 "$NEVERD_D8" --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
  --work-dir "$RUNNER_TEMP/android-scoped-invocations"
