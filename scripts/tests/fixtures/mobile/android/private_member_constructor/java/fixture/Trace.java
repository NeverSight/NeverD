package fixture;
import java.util.Collection;
public class Trace {
    public static Trace hook;
    public Trace() {}
    public void observe(int phase,Runnable value,Collection<Host.Callback> data,int code,Throwable problem) {}
    public static void hit(int phase,Runnable value,Collection<Host.Callback> data,int code,Throwable problem) {
        Trace current=hook;
        if(current!=null)current.observe(phase,value,data,code,problem);
    }
}
