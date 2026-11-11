package fixture;
public class Text implements CharSequence, Indexed {
    public char[] values;
    public int calls, lastIndex, events, overrides;
    public RuntimeException failure;
    public Class<?> witness;
    public Text(char[] values) { this.values = values; }
    public final char charAt(int index) { return get(index); }
    public char get(int index) {
        calls++;
        lastIndex = index;
        events = events * 10 + 1;
        char value = values[index];
        if (failure != null) throw failure;
        events = events * 10 + 2;
        return value;
    }
    public final int length() { return values.length; }
    public Text subSequence(int begin, int end) { return this; }
}
