package fixture;
public class Provider<E> {
    @SuppressWarnings("unchecked")
    public <T> T[] transfer(T[] values) {
        synchronized (this) {
        Trace.calls++;Trace.hit(2);Trace.input=values;Trace.receiver=this;
        Trace.action.run();Trace.hit(3);return (T[])Trace.result;
        }
    }
}
