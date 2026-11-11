package fixture;
public class Provider {
    @SuppressWarnings("unchecked")
    public static <T> T[] transfer(T[] values,String message,Object... data)throws IllegalArgumentException {
        Trace.calls++;Trace.hit(2);Trace.input=values;Trace.message=message;Trace.data=data;
        Trace.action.run();Trace.hit(3);return (T[])Trace.result;
    }
}
