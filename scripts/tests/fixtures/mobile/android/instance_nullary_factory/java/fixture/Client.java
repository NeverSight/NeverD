package fixture;
public class Client {
    public static Object call() {
        Pair<?, ?> result = Trace.current.make();
        Trace.after = result; Trace.hit(4);
        return result;
    }
    public static String checkedKey() {
        Pair<String, ?> result = Trace.current.make();
        Trace.after = result; Trace.hit(4);
        return result.key;
    }
    public static String checkedValue() {
        Pair<?, String> result = Trace.current.make();
        Trace.after = result; Trace.hit(4);
        return result.value;
    }
}
