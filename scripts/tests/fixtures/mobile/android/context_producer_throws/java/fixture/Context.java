package fixture;

import java.util.concurrent.Callable;

public class Context {
    public Object reply;
    public Context() {}
    public Object produce(Callable producer,int limit) throws InterruptedException {
        Trace.hit(5,this,producer,limit,null);
        return reply;
    }
}
