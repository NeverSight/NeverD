package fixture;
public abstract class Ancestor implements java.io.Serializable {
    protected static volatile int code=Trace.seedInt();
    protected static volatile long wide=Trace.seedLong();
    protected static Object token=Trace.seedObject();
    protected Ancestor() { Trace.partial=this; Trace.hit(4); }
    public static void replace(int nextCode, long nextWide, Object nextToken) {
        code=nextCode; wide=nextWide; token=nextToken;
    }
}
