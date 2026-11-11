package fixture;
public class Subject {
    public boolean z;
    public byte b;
    public char c;
    public short s;
    public int i, state;
    public long j;
    public float f;
    public double d;
    public Object saved;
    public Subject() {}
    public boolean runZ(Object value) { return selectZ(value); }
    private <T> boolean selectZ(T value) {
        Trace.hit(1, this, value);
        saved = value; state++;
        boolean result = z;
        Trace.hit(2, this, value);
        return result;
    }
    public byte runB(Object value) { return selectB(value); }
    private <T> byte selectB(T value) {
        Trace.hit(1, this, value);
        saved = value; state++;
        byte result = b;
        Trace.hit(2, this, value);
        return result;
    }
    public char runC(Object value) { return selectC(value); }
    private <T> char selectC(T value) {
        Trace.hit(1, this, value);
        saved = value; state++;
        char result = c;
        Trace.hit(2, this, value);
        return result;
    }
    public short runS(Object value) { return selectS(value); }
    private <T> short selectS(T value) {
        Trace.hit(1, this, value);
        saved = value; state++;
        short result = s;
        Trace.hit(2, this, value);
        return result;
    }
    public int runI(Object value) { return selectI(value); }
    private <T> int selectI(T value) {
        Trace.hit(1, this, value);
        saved = value; state++;
        int result = i;
        if (value instanceof String) result ^= 0x13579bdf;
        Trace.hit(2, this, value);
        return result;
    }
    public long runJ(Object value) { return selectJ(value); }
    private <T> long selectJ(T value) {
        Trace.hit(1, this, value);
        saved = value; state++;
        long result = j;
        Trace.hit(2, this, value);
        return result;
    }
    public float runF(Object value) { return selectF(value); }
    private <T> float selectF(T value) {
        Trace.hit(1, this, value);
        saved = value; state++;
        float result = f;
        Trace.hit(2, this, value);
        return result;
    }
    public double runD(Object value) { return selectD(value); }
    private <T> double selectD(T value) {
        Trace.hit(1, this, value);
        saved = value; state++;
        double result = d;
        Trace.hit(2, this, value);
        return result;
    }
}
