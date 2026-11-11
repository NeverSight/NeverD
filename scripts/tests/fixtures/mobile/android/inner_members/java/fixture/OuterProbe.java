package fixture;

public class OuterProbe {
    public final int observed;
    public OuterProbe() { observed = readOuter(); }
    public int readOuter() { return 0; }
}
