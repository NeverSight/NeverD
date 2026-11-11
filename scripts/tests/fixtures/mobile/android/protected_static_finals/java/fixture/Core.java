package fixture;
public class Core {
    public static final Object FIRST,SECOND,THIRD;
    static {try {FIRST=Provider.read(1);SECOND=Provider.read(2);THIRD=Provider.read(3);}catch(NoSuchMethodException failure){throw new IllegalStateException("owned lookup",failure);}}
    public static Object read(){Trace.hit(8);return THIRD;}
    public static Object touch(){Trace.hit(9);return FIRST;}
}
