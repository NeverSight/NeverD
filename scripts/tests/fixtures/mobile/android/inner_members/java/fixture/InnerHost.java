package fixture;

public class InnerHost {
    public int base;
    public InnerHost(int value) { base = value; }
    public Member make(int value) { return new Member(value); }

    public final class Member extends OuterProbe {
        public final int offset;
        public Member(int value) { offset = value; }
        @Override public int readOuter() { return base; }
        public int result() { return observed + offset; }
    }
}
