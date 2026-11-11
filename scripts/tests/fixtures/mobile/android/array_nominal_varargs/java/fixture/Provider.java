package fixture;
public class Provider {
    public static <Entry> Object forward(Object context, int selector, Entry... values) {
        Trace.hit(1);
        Trace.context = context;
        Trace.selector = selector;
        Trace.values = values;
        Trace.action.run();
        Trace.hit(2);
        return Trace.result;
    }
}
