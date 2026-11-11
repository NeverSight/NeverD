package fixture;
public class Parent {
    public static Object marker;
    static {
        Trace.hit(1);
        Observer.observe();
        marker = Trace.seed;
        Trace.hit(2);
        Observer.observe();
        Trace.hit(3);
    }
}
