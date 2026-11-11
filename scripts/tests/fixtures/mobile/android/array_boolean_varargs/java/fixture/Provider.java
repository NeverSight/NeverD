package fixture;
public class Provider {
    public static <T> boolean test(T... values){return decide(values);}
    public static <T> boolean decide(T... values){Trace.calls++;Trace.values=values;Trace.hit(1);Trace.action.run();Trace.hit(2);return Trace.result;}
}
