package fixture;
public final class Core implements Api<Object>, Marker {
    private final byte[] bytes;
    private final short[] shorts;
    private final int[] ints;
    private final long[] longs;
    public Core(byte[] bytes, short[] shorts, int[] ints, long[] longs) {
        this.bytes = bytes;
        this.shorts = shorts;
        this.ints = ints;
        this.longs = longs;
    }
    public final byte[] readBytes() { return bytes; }
    public final short[] readShorts() { return shorts; }
    public final int[] readInts() { return ints; }
    public final long[] readLongs() { return longs; }
    @Override public boolean add(Object value) {
        throw new UnsupportedOperationException("owned array consumer");
    }
}
