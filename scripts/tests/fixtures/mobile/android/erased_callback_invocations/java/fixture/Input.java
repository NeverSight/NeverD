package fixture;
public class Input {
    public int tag, reads, writes;
    public RuntimeException failure;
    public int readInt() { reads++; if (failure != null) throw failure; return tag; }
    public void writeInt(int value) { writes++; if (failure != null) throw failure; tag = value; }
}
