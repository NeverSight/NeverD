package fixture;

public final class Subject extends Base {
    public Subject() {}

    @Override
    @SuppressWarnings("unchecked")
    protected <T> T[] read(T[] input) {
        Trace.hit(4, this, input);
        saved = input;
        input[0] = (T)Trace.value;
        T[] result = Trace.replace ? (T[])Trace.replacement : input;
        Trace.hit(5, this, input);
        return result;
    }
}
