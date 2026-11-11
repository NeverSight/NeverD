package fixture;
public final class Core {
    public Core() {}
    public static Marker call(Base subject) { return subject.read(); }
    public static Object object(Base subject) { return subject.relay("owned", subject); }
    public static void discard(Base subject) { subject.read(); }
    public static Token checked(Base subject) {
        Marker result = subject.read(); Trace.hit(3, subject); return (Token)result;
    }
}
