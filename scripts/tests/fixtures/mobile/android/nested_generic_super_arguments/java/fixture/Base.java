package fixture;
public class Base<V extends Bound> {
    public Object read(Context context, V input, long bits) {
        Trace.hit(2); Trace.receiver=this; Trace.context=context;
        Trace.input=input; Trace.bits=bits; Trace.hit(3);
        Trace.action.run(); Trace.hit(4); return Trace.reply;
    }
}
