package fixture;

public class InnerHost {
    public static int trace;
    public int base;
    public InnerHost(int value) { base = value; }
    public int readValue() {
        trace = trace * 10 + 1;
        if (base == 42) throw new IllegalStateException();
        return base;
    }
    public static long readWide() {
        trace = trace * 10 + 2;
        return 0x12345678abcdef01L;
    }
    public InnerHost readToken() {
        trace = trace * 10 + 3;
        return this;
    }
    public Member make(int value) { return new Member(value); }

    public final class Member extends OuterProbe {
        public final int offset;
        public final long retainedWide;
        public final InnerHost retainedToken;
        public Member(int value) {
            int initial = InnerHost.this.readValue();
            long wide = InnerHost.readWide();
            InnerHost token = InnerHost.this.readToken();
            super(initial, wide, token);
            offset = initial + value;
            retainedWide = wide;
            retainedToken = token;
        }
        @Override public int readOuter() { return base; }
        public int result() { return observed + offset; }
    }
}
