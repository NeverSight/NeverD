package fixture;

public class OuterProbe {
    public static int constructions;
    public final int observed;
    public final String passed;
    public final Object owner;
    public OuterProbe(String token, Object owner) {
        InnerHost.trace = InnerHost.trace * 10 + 3;
        constructions++;
        observed = readOuter();
        passed = token;
        this.owner = owner;
    }
    public int readOuter() { return 0; }
}
