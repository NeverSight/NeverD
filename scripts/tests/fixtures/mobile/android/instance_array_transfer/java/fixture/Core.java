package fixture;
public class Core {
    public static <T> T[] call(Derived<?> receiver,T[] values) {
        Trace.hit(1);T[] result=receiver.transfer(values);Trace.hit(4);return result;
    }
    public static Object[] checked(Derived<?> receiver,Object values) {
        Trace.hit(1);Object[] argument=(Object[])values;Object[] result=receiver.transfer(argument);Trace.hit(4);return result;
    }
}
