package fixture;
public final class Subject<E> {
    public final Object value;
    public final int tag;
    public Subject(E[] input) {
        this((Object)input);
        Trace.arrays++;
    }
    public Subject(Object input) {
        this(input,23);
        Trace.objects++;
    }
    public Subject(Object input,int tag) {
        Trace.calls++;
        Trace.value=input;
        Trace.tag=tag;
        Trace.stage=1;
        Trace.action.run();
        this.value=Trace.value;
        this.tag=tag;
        Trace.stage=2;
    }
}
