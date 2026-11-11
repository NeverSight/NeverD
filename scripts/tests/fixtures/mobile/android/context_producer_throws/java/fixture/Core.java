package fixture;

import java.util.concurrent.Callable;

public final class Core {
    private Core() {}
    @SuppressWarnings({"rawtypes","unchecked"})
    public static String run(Object owner,Object callback,int limit) throws InterruptedException {
        Context context=(Context)owner;
        Callable producer=(Callable)callback;
        Trace.hit(3,context,producer,limit,null);
        Object result=Provider.read(context,producer,limit);
        Trace.hit(4,context,producer,limit,result);
        return (String)result;
    }
    @SuppressWarnings({"rawtypes","unchecked"})
    public static void ignore(Object owner,Object callback,int limit) throws InterruptedException {
        Context context=(Context)owner;
        Callable producer=(Callable)callback;
        Trace.hit(3,context,producer,limit,null);
        Provider.read(context,producer,limit);
        Trace.hit(4,context,producer,limit,null);
    }
}
