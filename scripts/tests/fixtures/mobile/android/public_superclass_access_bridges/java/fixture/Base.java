package fixture;
public class Base {
    public int calls;
    public Object marker;
    public int number;
    public long word;
    public RuntimeException failure;
    private void observe(Object marker, int number, long word) {
        calls++;
        this.marker = marker;
        this.number = number;
        this.word = word;
        if (marker instanceof int[]) ((int[]) marker)[0] += 3;
        if (failure != null) throw failure;
    }
    public void write(Object marker, int number, long word) {
        observe(marker, number, word);

    }
    public boolean flag(Object marker, int number, long word) {
        observe(marker, number, word);
        return number > 0;
    }
    public int scalar(Object marker, int number, long word) {
        observe(marker, number, word);
        return number;
    }
    public long wide(Object marker, int number, long word) {
        observe(marker, number, word);
        return word;
    }
    public double decimal(Object marker, int number, long word) {
        observe(marker, number, word);
        return Double.longBitsToDouble(word);
    }
    public Object reference(Object marker, int number, long word) {
        observe(marker, number, word);
        return marker;
    }
}
