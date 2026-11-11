package fixture;
public class Provider {
    static { Trace.hit(1); }
    public <T> boolean direct(int key, T value) {
        Trace.calls++;
        Trace.value = value;
        Trace.hit(2);
        if (key < 0) return false;
        Trace.hit(3);
        return value != null;
    }
    public static <T extends Readable> boolean checked(Creator<T> creator, int key, long flags, double fraction) {
        Trace.calls++;
        Trace.hit(2);
        T value = creator.read();
        Trace.hit(4);
        return value.accept(key, flags, fraction);
    }
}
