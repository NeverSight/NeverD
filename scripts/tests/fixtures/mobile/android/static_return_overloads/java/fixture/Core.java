package fixture;
public final class Core {
 static {Trace.hit(1); if(Trace.mode==1)throw Trace.failure;}
 public static <T> T genericSlot(Object[] input,Box<T> box,Factory<? extends T> factory,String stamp,int flags,long wide){
  Trace.slot=0;Trace.hit(2);Trace.input=input;Trace.box=box;Trace.factory=factory;
  Trace.stamp=stamp;Trace.flags=flags;Trace.wide=wide;Trace.hit(3);
  if(Trace.mode==2)throw Trace.failure;
  Object result=Trace.result;Trace.hit(4);return (T)result;
 }
 public static <T> Box<T> nominalSlot(Object[] input,Box<T> box,Factory<? extends Box<T>> factory,String stamp,int flags,long wide){
  Trace.slot=1;Trace.hit(5);Trace.input=input;Trace.box=box;Trace.factory=factory;
  Trace.stamp=stamp;Trace.flags=flags;Trace.wide=wide;Trace.hit(6);
  if(Trace.mode==2)throw Trace.failure;
  Object result=Trace.result;Trace.hit(7);return (Box<T>)result;
 }
 public static void __neverdStaticReturnSlot(){}
}
