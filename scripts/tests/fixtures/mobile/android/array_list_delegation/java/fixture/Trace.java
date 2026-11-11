package fixture;
public class Trace {
    public static Trace hook;
    public static Core latest;
    public Trace() {}
    public void hit(int phase, Core value) {}
    public static void touch(int phase, Core value) { hook.hit(phase, value); }
}
