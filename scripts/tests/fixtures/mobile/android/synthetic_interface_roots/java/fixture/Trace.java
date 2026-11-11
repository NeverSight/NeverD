package fixture;
public class Trace {
    public static int calls,tag,bits,mode,fail;
    public static long events,a,b,c,d;
    public static Object token;
    public static Throwable failure;
    public static void accept(int t,Object value,long aa,long bb,long cc,long dd,float scale,int m) {
        calls++;tag=t;events=t;
        if(fail==1) { if(failure instanceof RuntimeException)throw (RuntimeException)failure;throw (Error)failure; }
        token=value;a=aa;b=bb;c=cc;d=dd;bits=Float.floatToRawIntBits(scale);mode=m;
        events=t*10+9;
        if(fail==2) { if(failure instanceof RuntimeException)throw (RuntimeException)failure;throw (Error)failure; }
    }
}
