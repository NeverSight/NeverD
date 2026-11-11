package fixture;

public class InnerHost<T> {
    public int base;
    public InnerHost(int value) { base = value; }
    public Member make(int value) { return new Member(value); }

    public final class Member extends OuterProbe {
        public final int offset;
        public final Token retained;
        public Member(int value) {
            OuterProbe.before();
            Token token = new Token();
            super(token);
            offset = value;
            retained = token;
        }
        @Override public int readOuter() { return base; }
    }
}
