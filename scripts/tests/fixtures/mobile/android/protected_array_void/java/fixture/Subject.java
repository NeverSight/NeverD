package fixture;

public class Subject extends Passive {
    @Override
    @SuppressWarnings("unchecked")
    protected <T> void consume(T[] input) {
        Trace.hit(5, this, input, -1, null);
        saved = input;
        for (int i = 0; i < input.length; ++i) {
            T value = input[i];
            seen = value;
            Trace.hit(6, this, input, i, value);
            input[i] = (T)Trace.value;
            writes++;
            Trace.hit(7, this, input, i, value);
        }
        Trace.hit(8, this, input, -1, null);
    }
}
