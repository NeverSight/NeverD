package fixture;
public final class CharacterCore extends Base<char[]> {
    public int calls, index;
    public Object seen;
    public RuntimeException failure;
    protected void write(Token token, char[] array, int index) {
        calls++; seen = array; this.index = index;
        if (failure != null) throw failure;
        token.calls++;
        array[index] = 'Q';
    }
}
