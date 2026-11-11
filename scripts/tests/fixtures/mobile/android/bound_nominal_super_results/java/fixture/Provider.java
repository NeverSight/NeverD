package fixture;
public class Provider {
    public static Integer take(int value) {
        int n = ++Trace.calls;
        if (n == 1) Trace.inFirst = value;
        else Trace.inSecond = value;
        Trace.hit(n);
        return n == 1 ? Trace.first : Trace.second;
    }
}
