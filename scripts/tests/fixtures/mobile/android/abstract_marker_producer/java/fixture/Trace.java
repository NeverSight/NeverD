package fixture;
public abstract class Trace {
    public static Trace hook;
    public static Marker value, alternative;
    public Trace() {}
    public abstract void observe(int phase, Base subject);
    public static void hit(int phase, Base subject) { hook.observe(phase, subject); }
}
