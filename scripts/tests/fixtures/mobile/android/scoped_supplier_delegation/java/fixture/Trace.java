package fixture;
public class Trace {
    public static int gets,parents,constructors,keyCalls,valueCalls,wrong,failAt;
    public static long events;
    public static Object provided,input,getPayload,parentPayload,constructorPayload,dynamicSource,dynamicKey,dynamicValue;
    public static Parent<?,?> parent;
    public static Core<?,?> core;
    public static Runnable getAction,parentAction,constructorAction;
    public static RuntimeException failure;
    public static void hit(int event){events=events*10+event;if(event==failAt)throw failure;}
}
