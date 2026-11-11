import fixture.*;import java.lang.invoke.*;import java.lang.reflect.*;
public final class BoundedCarrierSeedResultHarness {
 static void require(boolean ok){if(!ok)throw new AssertionError("owned bounded carrier seed result");}
 static final class Output extends Bound {Output(long value){super(value);}}
 static final class Actual implements Spec<Bound> {
  final Bound expected;Actual(Bound expected){this.expected=expected;}
  public Bound value(long time,Bound first,Bound second,Bound third){Trace.time=time;Trace.first=first;Trace.second=second;Trace.third=third;Trace.hit(3);return expected;}
 }
 static Bound call(int route,Spec<Bound> spec,long time,Bound first,Bound second,Bound third)throws Throwable{
  if(route==0)return Client.call(spec,time,first,second,third);
  if(route==1)return BoundedCarrierSeedResultCaller.call(spec,time,first,second,third);
  if(route==2){try{return(Bound)Provider.class.getMethod("value",Spec.class,long.class,Bound.class,Bound.class,Bound.class).invoke(null,spec,time,first,second,third);}catch(InvocationTargetException f){throw f.getCause();}}
  return(Bound)MethodHandles.publicLookup().findStatic(Provider.class,"value",MethodType.methodType(Bound.class,Spec.class,long.class,Bound.class,Bound.class,Bound.class)).invokeExact((Spec)spec,time,first,second,third);
 }
 public static void main(String[] args)throws Throwable{
  int route=Integer.parseInt(args[0]),mode=Integer.parseInt(args[1]);
  Method method=Provider.class.getMethod("value",Spec.class,long.class,Bound.class,Bound.class,Bound.class);
  TypeVariable<?>[] formals=method.getTypeParameters();require(formals.length==1 && formals[0].getBounds()[0]==Bound.class && method.getGenericReturnType().equals(formals[0]));
  Type[] parameters=method.getGenericParameterTypes();require(((ParameterizedType)parameters[0]).getActualTypeArguments()[0].equals(formals[0]) && parameters[1]==long.class && parameters[2].equals(formals[0]) && parameters[3].equals(formals[0]) && parameters[4].equals(formals[0]));
  for(int attempt=0;attempt<2;attempt++){
   Bound first=mode==1?null:new Bound(1+attempt),second=mode==7?null:new Output(2+attempt),third=mode==8?null:new Bound(3+attempt),expected=mode==2?null:new Output(4+attempt);
   Spec<Bound> spec=mode==6?null:new Actual(expected);long time=mode==9?Long.MIN_VALUE:mode==10?Long.MAX_VALUE:0x1234567887654321L+attempt;
   Trace.events=0;Trace.time=0;Trace.first=Trace.second=Trace.third=Trace.result=null;Trace.failAt=mode==3?1:mode==4?2:mode==5?3:0;Trace.failure=new RuntimeException("owned result failure");
   Bound actual=null;Throwable caught=null;try{actual=call(route,spec,time,first,second,third);}catch(Throwable t){caught=t;}
   long events=mode==3||mode==6?1:mode==5?13:132;require(Trace.events==events);
   if(mode==6)require(caught instanceof NullPointerException);else if(Trace.failAt!=0)require(caught==Trace.failure);else require(caught==null && actual==expected);
   boolean called=mode!=3&&mode!=6;require(Trace.time==(called?time*1000000L:0) && Trace.first==(called?first:null) && Trace.second==(called?second:null) && Trace.third==(called?third:null));
   require(Trace.result==((mode==3||mode==5||mode==6)?null:expected));
   System.out.println("route="+route+",mode="+mode+",attempt="+attempt+",trace="+events);
  }
 }
}
