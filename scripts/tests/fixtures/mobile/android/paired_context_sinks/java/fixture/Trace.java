package fixture;

import java.util.Map;

public class Trace {
    public static Trace hook;
    public Trace() {}
    public void observe(int phase,Context context,Map target,String key,Class first,Class second,Object left,Object right) {}
    public static void hit(int phase,Context context,Map target,String key,Class first,Class second,Object left,Object right) {
        Trace current=hook;
        if(current!=null)current.observe(phase,context,target,key,first,second,left,right);
    }
}
