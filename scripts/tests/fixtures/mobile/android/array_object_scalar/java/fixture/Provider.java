package fixture;
public class Provider {
    public static <E> int score(E[] values,Object key) {
        Trace.calls++;Trace.values=values;Trace.key=key;Trace.hit(1);
        Trace.action.run();Trace.hit(2);return ((Integer)Trace.result).intValue();
    }
}
