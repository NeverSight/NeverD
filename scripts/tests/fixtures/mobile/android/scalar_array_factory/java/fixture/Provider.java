package fixture;

import java.util.HashSet;

public final class Provider {
    private Provider() {}
    @SuppressWarnings("unchecked")
    public static <T> HashSet<T> make(boolean flag, T... values) {
        Trace.calls++;
        Trace.entry = 1;
        Trace.flag = flag;
        Trace.stamp = 0L;
        Trace.values = values;
        Object saved = Trace.store;
        Trace.saved = saved;
        Trace.stage = 1;
        if (Trace.inspect) {
            Trace.element = values[0];
            Trace.stage = 2;
        }
        Trace.action.run();
        Trace.stage = 3;
        Trace.afterFlag = flag;
        Trace.afterStamp = 0L;
        if (Trace.write) {
            values[Trace.index] = (T)saved;
            Trace.stage = 4;
        }
        return (HashSet<T>)Trace.value;
    }
    @SuppressWarnings("unchecked")
    public static <T> HashSet<T> wide(boolean flag, long stamp, T... values) {
        Trace.calls++;
        Trace.entry = 2;
        Trace.flag = flag;
        Trace.stamp = stamp;
        Trace.values = values;
        Object saved = Trace.store;
        Trace.saved = saved;
        Trace.stage = 1;
        if (Trace.inspect) {
            Trace.element = values[0];
            Trace.stage = 2;
        }
        Trace.action.run();
        Trace.stage = 3;
        Trace.afterFlag = flag;
        Trace.afterStamp = stamp;
        if (Trace.write) {
            values[Trace.index] = (T)saved;
            Trace.stage = 4;
        }
        return (HashSet<T>)Trace.value;
    }
    public static <T> HashSet<T> fixed(boolean flag, T[] values) {
        Trace.forwards++;
        return Provider.make(flag, values);
    }
}
