package fixture;
public final class Trace {
 public static Point probe;
 public static void construct(int phase,Object save,Object restore,Object result){probe.construct(phase,save,restore,result);}
 public static void operation(int phase,Object owner,Object context,Object input,Object output){probe.operation(phase,owner,context,input,output);}
}
