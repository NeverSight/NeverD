package fixture;
public class Provider {
    @SuppressWarnings("unchecked")
    public static <E extends CharSequence> E select(E text,String message,Object... arguments)throws IllegalArgumentException {Trace.calls++;Trace.text=text;Trace.message=message;Trace.arguments=arguments;Trace.hit(1);Trace.action.run();Trace.hit(2);return (E)Trace.result;}
}
