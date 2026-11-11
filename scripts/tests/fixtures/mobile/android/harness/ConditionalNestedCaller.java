import fixture.*;
public final class ConditionalNestedCaller {
    public static Outer make(Input input, Object tag) { return Creator.make(input, tag); }
    public static Outer withTag(Input input, Object tag) { return Creator.withTag(input, tag); }
    public static IllegalStateException failure(Object input) { return Creator.failure(input); }
}
