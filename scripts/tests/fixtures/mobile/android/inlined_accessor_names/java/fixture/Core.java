package fixture;
public final class Core implements Api<Object> {
    private final byte small;
    private final long wide;
    private final double real;
    public Core(byte small, long wide, double real) {
        this.small = small;
        this.wide = wide;
        this.real = real;
    }
    public final byte readByte() { return small; }
    public final long readLong() { return wide; }
    public final double readDouble() { return real; }
    @Override public int compare(Object value) {
        Core checked = (Core) value;
        byte otherByte = checked.readByte();
        long otherWide = checked.readLong();
        double otherReal = checked.readDouble();
        Trace.hit(2);
        byte ownByte = readByte();
        long ownWide = readLong();
        double ownReal = readDouble();
        Trace.hit(3);
        int a = Integer.compare(ownByte & 255, otherByte & 255);
        int b = Long.compare(ownWide, otherWide);
        int c = Double.compare(ownReal, otherReal);
        return a * 100 + b * 10 + c;
    }
}
