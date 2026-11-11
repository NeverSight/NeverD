package fixture;

public class OuterProbe {
    public final int observed;
    public final int passed;
    public final long passedWide;
    public final InnerHost passedToken;
    public OuterProbe(int value, long wide, InnerHost token) {
        InnerHost.trace = InnerHost.trace * 10 + 2;
        StaticValues.initial += 7;
        StaticValues.wide += 13;
        StaticValues.token = null;
        observed = readOuter();
        passed = value;
        passedWide = wide;
        passedToken = token;
    }
    public int readOuter() { return 0; }
}
