package fixture;

public class Base {
    public Object[] saved;
    public Base() {}

    @SuppressWarnings("unchecked")
    protected <T> T[] read(T[] input) {
        Trace.hit(1, this, input);
        saved = input;
        input[0] = (T)Trace.value;
        T[] result = Trace.replace ? (T[])Trace.replacement : input;
        Trace.hit(2, this, input);
        return result;
    }
}
