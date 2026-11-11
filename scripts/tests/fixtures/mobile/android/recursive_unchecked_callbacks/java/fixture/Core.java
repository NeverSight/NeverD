package fixture;
public class Core extends Parent<Core> {
    public int calls;
    public byte[] seen;
    public LeafFailure failure;
    public FatalFailure fatal;
    public byte[] update(byte[] input) throws LeafFailure, FatalFailure {
        calls++;
        seen = input;
        if (failure != null) throw failure;
        if (fatal != null) throw fatal;
        return input;
    }
}
