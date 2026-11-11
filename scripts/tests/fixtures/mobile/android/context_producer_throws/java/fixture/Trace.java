package fixture;

import java.util.concurrent.Callable;

public class Trace {
    public static Trace hook;
    public Trace() {}
    public void observe(int phase,Context context,Callable producer,int limit,Object result) {}
    public static void hit(int phase,Context context,Callable producer,int limit,Object result) {
        Trace current=hook;
        if(current!=null)current.observe(phase,context,producer,limit,result);
    }
}
