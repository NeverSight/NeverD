import fixture.*;
public final class ArrayClosedContextVoidCaller {
    public static String render(Object[] values, StringBuilder text, Bucket<Object[]> path) {
        return Owner.render(values, text, path);
    }
}
