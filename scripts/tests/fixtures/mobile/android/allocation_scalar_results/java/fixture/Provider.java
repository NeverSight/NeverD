package fixture;

public class Provider {
    public long number;
    public double wide;
    public float narrow;
    public byte small;
    public short medium;
    public char character;
    public boolean flag;

    public long readLong(Object input) {
        Trace.hit(1); Trace.consume(input); return number;
    }
    public double readDouble(Object input) {
        Trace.hit(2); Trace.consume(input); number = 0; return wide;
    }
    public float readFloat(Object input) {
        Trace.hit(3); Trace.consume(input); wide = 0; return narrow;
    }
    public byte readByte(Object input) {
        Trace.hit(4); Trace.consume(input); narrow = 0; return small;
    }
    public short readShort(Object input) {
        Trace.hit(5); Trace.consume(input); small = 0; return medium;
    }
    public char readChar(Object input) {
        Trace.hit(6); Trace.consume(input); medium = 0; return character;
    }
    public boolean readBoolean(Object input) {
        Trace.hit(7); Trace.consume(input); character = 0; return flag;
    }
}
