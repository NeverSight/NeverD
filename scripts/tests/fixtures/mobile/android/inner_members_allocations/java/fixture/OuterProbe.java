package fixture;

public class OuterProbe {
    public static int trace;
    public static int constructions;
    public static int allocations;
    public static boolean fail;
    public final int observed;
    public final Token token;
    public OuterProbe(Token token) {
        trace = trace * 10 + 4;
        constructions++;
        observed = readOuter();
        this.token = token;
    }
    public static void before() { trace = trace * 10 + 1; }
    public int readOuter() { return 0; }
}
