package fixture;
@SuppressWarnings({"rawtypes","unchecked"})
public class Carrier<E extends Enum<E>> {
    public Object marker;
    public Carrier(Object value) {marker=value;}
    public static <E extends Enum<E>> Carrier<E> make(E token) {
        Trace.calls++;Trace.token=token;Trace.stage=1;
        Trace.action.run();Trace.stage=2;return (Carrier<E>)Trace.result;
    }
}
