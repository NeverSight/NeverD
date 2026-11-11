package fixture;

public class Token {
    static { OuterProbe.trace = OuterProbe.trace * 10 + 2; }
    public Token() {
        OuterProbe.trace = OuterProbe.trace * 10 + 3;
        OuterProbe.allocations++;
        if (OuterProbe.fail) throw new IllegalStateException();
    }
}
