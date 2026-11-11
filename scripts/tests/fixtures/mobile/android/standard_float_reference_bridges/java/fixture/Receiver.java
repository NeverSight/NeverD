package fixture;

@SuppressWarnings("rawtypes")
public final class Receiver<S> implements Function {
    public Object result;
    public Box<? super Boolean> last;
    public Receiver() {}
    public Object apply(float input, Box<? super Boolean> box) {
        Trace.events = Trace.events * 10 + 1;
        Trace.first = Float.floatToRawIntBits(input);
        this.last = box;
        if (Trace.targetFailure != null) throw Trace.targetFailure;
        return result;
    }
    @SuppressWarnings("unchecked")
    public Object invoke(Object first, Object second) {
        return apply(((Number) first).floatValue(), (Box<? super Boolean>) second);
    }
}
