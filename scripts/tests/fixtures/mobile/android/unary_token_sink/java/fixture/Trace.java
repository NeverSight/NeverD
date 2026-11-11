package fixture;

import java.util.List;

public class Trace {
    public static Trace hook;
    public Trace() {}
    public void observe(int phase,Context context,List target,String key,Class token,Object value) {}
    public static void hit(int phase,Context context,List target,String key,Class token,Object value) {
        Trace current=hook;
        if(current!=null)current.observe(phase,context,target,key,token,value);
    }
}
