package fixture;
public class Provider {
    static { Trace.hit(1); }
    public static <T> T[] move(T[] source, T[] destination, int offset, int begin, int end) {
        Trace.source = source; Trace.destination = destination; Trace.hit(2);
        System.arraycopy(source, begin, destination, offset, end - begin);
        Trace.hit(3);
        return destination;
    }
}
