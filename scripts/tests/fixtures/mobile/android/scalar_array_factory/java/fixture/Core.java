package fixture;

import java.util.HashSet;

public final class Core {
    private Core() {}
    public static HashSet raw(boolean flag, Object[] values) { return Provider.make(flag, values); }
    public static HashSet<String> typed(boolean flag, String[] values) { return Provider.make(flag, values); }
    public static Object ignored(boolean flag, Object[] values) {
        Provider.make(flag, values);
        return Trace.marker;
    }
    public static HashSet fixed(boolean flag, Object[] values) { return Provider.fixed(flag, values); }
    public static HashSet wide(boolean flag, long stamp, Object[] values) { return Provider.wide(flag, stamp, values); }
}
