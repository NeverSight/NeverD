package fixture;
public class Trace {
 public static Trace hook;
 public Trace() {}
 public void observe(int phase,Object self,Object value,int seed) {}
 public static int seed(int value) { if(hook!=null)hook.observe(0,null,null,value); return value; }
 public static void parent(Object self,int seed,Token token) { if(hook!=null)hook.observe(1,self,token,seed); }
 public static void accept(Object self,Child child) { if(hook!=null)hook.observe(2,self,child,0); }
 public static void finish(Object self) { if(hook!=null)hook.observe(3,self,null,0); }
}
