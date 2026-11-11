import fixture.Core;
public final class ArrayFixedReferenceResultCaller {
    public static Object[] snapshot(Core<?> value) { return value.snapshot(); }
    public static String[] narrowed(Core<?> value) { return value.narrowed(); }
    public static void ignored(Core<?> value) { value.ignored(); }
}
