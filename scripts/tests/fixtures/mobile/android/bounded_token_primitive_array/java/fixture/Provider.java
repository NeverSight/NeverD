package fixture;
import java.nio.ByteOrder;
public class Provider {
    @SuppressWarnings("unchecked")
    public static <N extends Number> N read(byte[] data,Class<N> type,ByteOrder order)throws IllegalArgumentException {Trace.calls++;Trace.data=data;Trace.type=type;Trace.order=order;Trace.hit(1);Trace.action.run();Trace.hit(2);return (N)Trace.result;}
}
