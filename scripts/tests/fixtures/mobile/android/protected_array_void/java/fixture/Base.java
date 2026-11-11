package fixture;

public abstract class Base {
    public Object[] saved;
    public Object seen;
    public int marker;
    public int writes;

    protected void mark(int value) {
        marker = value;
        Trace.hit(0, this, null, -1, null);
    }

    @SuppressWarnings("unchecked")
    protected <T> void consume(T[] input) {
        Trace.hit(1, this, input, -1, null);
        saved = input;
        for (int i = 0; i < input.length; ++i) {
            T value = input[i];
            seen = value;
            Trace.hit(2, this, input, i, value);
            input[i] = (T)Trace.value;
            writes++;
            Trace.hit(3, this, input, i, value);
        }
        Trace.hit(4, this, input, -1, null);
    }

    public <T> void write(T[] input, int tag) {
        mark(tag);
        consume(input);
    }
}
