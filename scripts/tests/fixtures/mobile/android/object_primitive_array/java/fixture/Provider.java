package fixture;
public class Provider {
    public static <E> byte[] pack(E value) {Trace.calls++;Trace.input=value;Trace.hit(1);Trace.action.run();Trace.hit(2);return Trace.result;}
}
