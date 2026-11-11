# Closed primitive-array constructor token

Authored closed anonymous subclass forwards an opaque Class argument to a
parent specialized with float[]. The source contract retains Class<float[]>;
its cast checks only the original Class descriptor. The frozen caller checks
null and foreign class tokens, exact class-initialization and constructor
failures, partial state and returned identity across repeated JVM attempts.
