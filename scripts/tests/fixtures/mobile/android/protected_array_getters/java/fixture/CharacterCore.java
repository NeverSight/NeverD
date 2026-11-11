package fixture;
public final class CharacterCore extends Base<char[]> {
    public int calls, length;
    public boolean fresh;
    public char[] value, last;
    public RuntimeException failure;
    protected char[] get() {
        calls++;
        if (failure != null) throw failure;
        if (fresh) value = new char[length];
        last = value;
        return value;
    }
}
