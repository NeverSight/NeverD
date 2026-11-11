package fixture;

public class InnerHost {
    public int base;
    public static int trace;
    public InnerHost() {}
    public static void before() { trace = trace * 10 + 1; }
    public static void after() { trace = trace * 10 + 2; }
    public Member make(Object token) { return new Member(token); }

    public final class Member extends OuterProbe {
        public final String retained;
        public Member(Object token) {
            InnerHost.before();
            String converted = (String) token;
            InnerHost.after();
            Object owner = (Object) InnerHost.this;
            super(converted, owner);
            retained = converted;
        }
        @Override public int readOuter() { return base; }
    }
}
