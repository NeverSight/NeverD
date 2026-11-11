package fixture;
public class Owner {
    public static String render(Object[] values, StringBuilder text, Bucket<Object[]> path) {
        Trace.hit(3);
        append(values, text, path);
        Trace.hit(4);
        return text.toString();
    }
    private static final <T> void append(T[] values, StringBuilder text, Bucket<Object[]> path) {
        Trace.hit(1);
        if (path.contains(values)) { text.append('!'); return; }
        path.push(values);
        text.append('[');
        for (int i = 0; i < values.length; i++) {
            if (i != 0) text.append(',');
            Object value = values[i];
            if (value instanceof Object[]) append((Object[]) value, text, path);
            else text.append(String.valueOf(value));
        }
        text.append(']');
        path.pop();
        Trace.hit(2);
    }
}
