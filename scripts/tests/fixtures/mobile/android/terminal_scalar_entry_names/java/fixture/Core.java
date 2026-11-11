package fixture;
public final class Core implements Api<Object>, Marker {
    public boolean byteEntry(byte value) {
        Trace.hit(2);
        return (value & 255) >= 128;
    }
    public long longEntry(long value) {
        Trace.hit(3);
        return Long.rotateLeft(value, 17) ^ 0x123456789abcdef0L;
    }
    public double doubleEntry(double value) {
        Trace.hit(4);
        return value;
    }
    public boolean rejectInt(int value) {
        throw new UnsupportedOperationException("owned scalar consumer");
    }
    @Override public boolean add(Object value) {
        throw new UnsupportedOperationException("owned erased consumer");
    }
}
