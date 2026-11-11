import fixture.*;
public final class NestedAllocationCaller {
    public static Outer<Object> make(Provider provider, int length) {
        return new Outer<Object>(new Inner(provider.read()), Provider.bytes(length));
    }
    public static StackTraceElement frame(StackTraceElement original, String label) {
        return new StackTraceElement(new StringBuilder().append("owned.").append(label).toString(),
            "frame", original.getFileName(), original.getLineNumber());
    }
    public static Inner checked(Object input) { return new Inner((String) input); }
    public static IllegalStateException failure(Object problem) {
        return new IllegalStateException(new StringBuilder().append("owned ").append(problem).toString());
    }
    public static Inner read(Provider provider) { return Creator.read(provider); }
    public static IllegalStateException literal() { return Creator.literal(); }

}
