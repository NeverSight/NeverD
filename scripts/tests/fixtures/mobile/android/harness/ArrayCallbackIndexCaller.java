import fixture.*;
@SuppressWarnings({"rawtypes","unchecked"})
public final class ArrayCallbackIndexCaller {
    public static int call(Filter filter,Object[] values){return Client.call(filter,values);}
    public static int from(Filter filter,int start,Object[] values){return Client.from(filter,start,values);}
}
