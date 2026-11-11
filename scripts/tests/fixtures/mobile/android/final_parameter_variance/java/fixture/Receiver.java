package fixture;
@SuppressWarnings("rawtypes")
public final class Receiver<S> implements Function {
    public Input input;
    public Mapper<? super Input, Value> last;
    public Box<? super Boolean> box;
    public Receiver() {}
    public Object invoke(Mapper<? super Input, Value> mapper, Box<? super Boolean> box) {
        Trace.events = Trace.events * 10 + 1;
        this.last = mapper;
        this.box = box;
        if (Trace.targetFailure != null) throw Trace.targetFailure;
        return mapper.map(input);
    }
    @SuppressWarnings("unchecked")
    public Object invoke(Object first, Object second) {
        return invoke((Mapper<? super Input, Value>) first, (Box<? super Boolean>) second);
    }
}
