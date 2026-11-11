package fixture;

public final class Core {
    private Core() {}
    public static Object[] call(Base subject, Object[] input) { return subject.read(input); }
    public static Object object(Base subject, Object[] input) { return subject.read(input); }
    public static void discard(Base subject, Object[] input) { subject.read(input); }
    public static String[] checked(Base subject, Object[] input) {
        Object[] result = subject.read(input);
        Trace.hit(3, subject, input);
        return (String[])result;
    }
}
