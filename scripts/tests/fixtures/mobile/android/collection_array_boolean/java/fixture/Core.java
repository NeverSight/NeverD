package fixture;

import java.util.Collection;

public final class Core {
    private Core() {}
    public static boolean run(Object carrier, Object values) {
        Collection sink = (Collection)carrier;
        Object[] array = (Object[])values;
        Trace.hit(3, sink, array, null);
        boolean result = Provider.transfer(sink, array);
        Trace.hit(4, sink, array, Boolean.valueOf(result));
        return result;
    }
    public static void ignore(Object carrier, Object values) {
        Collection sink = (Collection)carrier;
        Object[] array = (Object[])values;
        Trace.hit(3, sink, array, null);
        Provider.transfer(sink, array);
        Trace.hit(4, sink, array, null);
    }
}
