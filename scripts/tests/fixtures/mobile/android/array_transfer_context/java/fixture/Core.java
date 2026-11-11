package fixture;
public class Core {
    public static <T> T[] call(T[] values)throws IllegalArgumentException {
        Trace.hit(1);T[] result=Provider.transfer(values,"owned-message",new Object[0]);Trace.hit(4);return result;
    }
    public static Object[] checked(Object values,String message,Object... data)throws IllegalArgumentException {
        Trace.hit(1);Object[] argument=(Object[])values;Object[] result=Provider.transfer(argument,message,data);Trace.hit(4);return result;
    }
}
