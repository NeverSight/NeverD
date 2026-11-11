import fixture.Core;
public final class ArrayTransferContextCaller {
    public static String[] call(String[] values){return Core.call(values);}
    public static String[] checked(Object values,String message,Object[] data){return (String[])Core.checked(values,message,data);}
}
