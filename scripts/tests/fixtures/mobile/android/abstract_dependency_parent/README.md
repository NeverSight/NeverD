# Plain abstract dependency parents

An unchanged SDK `AbstractWidget` extends a plain concrete `Root`, reabstracts
its protected callback and owns a public abstract getter and a method-local
generic identity implementation. The raw-parameter forwarder keeps original
concrete providers, protected access and an alpha-equivalent method-owned
formal without acquiring class formals or bridges.

Run `test_mobile_android_original_raw_parameters.py --dependency-interface
--abstract-dependency-parent`. Its immutable wrapper verifies nearest abstract
requirements, full provider metadata, inherited state, protected reflected
dispatch, independent virtual overrides, method-formal ownership and effects
before the same exception object. A generic provider mutation keeps Code and
descriptors unchanged but fails recovery and independent javac. Missing,
abstract, private, static or incompatible nearest providers remain rejected.

All seven input classes and sixteen methods retain complete original metadata.
The wrapper adds eighteen identity checks, nine state observations and three
ordered failures to the raw-parameter oracle, with zero source bridges.
