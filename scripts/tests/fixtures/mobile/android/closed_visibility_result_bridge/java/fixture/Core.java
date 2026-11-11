package fixture;
public class Core {
    public static Input input(Child child) {
        Trace.hit(1);
        Input result = child.first();
        Trace.hit(8);
        return result;
    }
    public static Value value(Child child) {
        Trace.hit(1);
        Value result = child.first();
        Trace.hit(8);
        return result;
    }
}
