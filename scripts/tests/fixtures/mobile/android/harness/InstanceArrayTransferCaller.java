import fixture.*;
public final class InstanceArrayTransferCaller {
    public static String[] call(Derived<?> receiver,String[] values){return Core.call(receiver,values);}
    public static String[] checked(Derived<?> receiver,Object values){return (String[])Core.checked(receiver,values);}
}
