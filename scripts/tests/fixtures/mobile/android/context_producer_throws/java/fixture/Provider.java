package fixture;

import java.util.concurrent.Callable;

public final class Provider {
    private Provider() {}
    public static <T> T read(Context context,Callable<T> producer,int limit) throws InterruptedException {
        return Entry.read(context,producer,limit);
    }
    static final class Entry {
        private Entry() {}
        @SuppressWarnings("unchecked")
        static <T> T read(Context context,Callable<T> producer,int limit) throws InterruptedException {
            Trace.hit(1,context,producer,limit,null);
            Object result=context.produce(producer,limit);
            Trace.hit(2,context,producer,limit,result);
            return (T)result;
        }
    }
}
