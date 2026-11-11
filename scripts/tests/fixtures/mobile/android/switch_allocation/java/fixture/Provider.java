package fixture;
public final class Provider {
    public static Object choice(int selected) {
        Trace.selected = selected;
        Object value = Trace.second;
        Trace.hit(4);
        return value;
    }
    public static int invalid(int selected) { Trace.hit(7); return selected; }
}
