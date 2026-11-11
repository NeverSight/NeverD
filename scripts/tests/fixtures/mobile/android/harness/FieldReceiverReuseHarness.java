import fixture.*;
import java.lang.invoke.*;
import java.lang.reflect.*;

public final class FieldReceiverReuseHarness {
 static final String MARKER="owned field receiver reuse";
 static void require(boolean value){if(!value)throw new AssertionError(MARKER);}
 static final class Token {public int hashCode(){throw new AssertionError(MARKER);}public String toString(){throw new AssertionError(MARKER);}}
 static final class Checked extends Exception {}
 @SuppressWarnings("unchecked") static <E extends Throwable> void fail(Throwable error)throws E{throw(E)error;}
 static final Method MAKE;
 static final MethodHandle HANDLE;
 static {try{MAKE=CoreKt.class.getMethod("make",Provider.class);HANDLE=MethodHandles.lookup().unreflect(MAKE);}catch(Exception e){throw new ExceptionInInitializerError(e);}}
 static Object invoke(int route,Provider p)throws Throwable {
  try{if(route==0)return MAKE.invoke(null,p);if(route==1)return(Result)HANDLE.invokeExact(p);return CoreKt.make(p);}
  catch(InvocationTargetException error){throw error.getCause();}
 }
 static final class Observer implements Point,Provider {
  final int mode,attempt;final Object[] values=new Object[24],mapped=new Object[24],markers=new Object[25];final Throwable failure;
  boolean initialized,defaultsInitialized;int index,stage,reads,gets,calls,entry,after,inits,defaultInits;
  Observer(int mode,int attempt){
   this.mode=mode;this.attempt=attempt;initialized=attempt>0&&mode!=1&&mode!=18&&mode!=19;
   defaultsInitialized=attempt>0&&mode!=1&&mode!=18&&mode!=19&&mode!=23&&mode!=24;
   failure=mode==4||mode==6||mode==8||mode==12||mode==14||mode==18||mode==23?new AssertionError("failure"):new Checked();
   for(int i=0;i<25;i++)markers[i]=new Token();
   for(int i=0;i<24;i++){values[i]=new Token();mapped[i]=mode==17?(i%2==0?markers[i]:markers[0]):new Token();}
   if(mode==10)mapped[4]=null;
   if(mode==15){markers[3]=new Object[]{new Token()};mapped[3]=markers[3];}
   if(mode==16)mapped[4]=new Object[]{new Token()};
   if(mode==21){values[3]=null;mapped[3]=markers[3];}
   if(mode==22){values[3]=Boolean.FALSE;mapped[3]=markers[3];}
  }
  public Object read(int i){
   require(initialized&&defaultsInitialized&&i==index&&stage==0&&Defaults.witness==markers[i]);reads++;stage=1;Defaults.witness=markers[i+1];
   if((mode==3||mode==4)&&i==5)FieldReceiverReuseHarness.<RuntimeException>fail(failure);
   return values[i];
  }
  public Mapper mapper(int i){
   require(initialized&&i==index&&stage==1);gets++;stage=2;
   if((mode==5||mode==6)&&i==6)FieldReceiverReuseHarness.<RuntimeException>fail(failure);
   if(values[i]==null||values[i] instanceof Boolean){index++;stage=0;}
   if(mode==2&&i==7)return null;
   return new Adapter(this,i);
  }
  Object apply(int i,Object value){
   require(i==index&&stage==2&&value==markers[i]);calls++;index++;stage=0;
   if(mode==15&&i==3)((Object[])value)[0]=this;
   if((mode==7||mode==8)&&i==8)FieldReceiverReuseHarness.<RuntimeException>fail(failure);
   if(mode==9&&i==8)throw(RuntimeException)null;
   return mapped[i];
  }
  public void visit(int phase,int unused,Object value){
   require(unused==-1);
   if(phase==3){require(initialized&&!defaultsInitialized&&attempt==0&&reads==0&&value==null);defaultInits++;defaultsInitialized=true;Defaults.witness=markers[0];if(mode==23||mode==24)FieldReceiverReuseHarness.<RuntimeException>fail(failure);return;}
   if(phase==0){require(!initialized&&attempt==0&&reads==0&&value==null);inits++;initialized=true;if(mode==18||mode==19)FieldReceiverReuseHarness.<RuntimeException>fail(failure);return;}
   require(initialized&&defaultsInitialized&&Defaults.witness==markers[24]&&index==24&&stage==0&&reads==24&&gets==24&&calls==(mode==21||mode==22?23:24)&&Trace.created==value&&value!=null);
   if(phase==1){require(entry++==0&&after==0);if(mode==11||mode==12)FieldReceiverReuseHarness.<RuntimeException>fail(failure);}
   else {require(phase==2&&entry==1&&after++==0);fields(value,true);if(mode==16)((Object[])mapped[4])[0]=this;if(mode==13||mode==14)FieldReceiverReuseHarness.<RuntimeException>fail(failure);}
  }
  void fields(Object value,boolean assigned){
   try{for(int i=0;i<24;i++)require(Result.class.getField("f"+i).get(value)==(assigned?mapped[i]:null));}
   catch(ReflectiveOperationException error){throw new AssertionError(MARKER,error);}
  }
  void reset(){index=stage=reads=gets=calls=entry=after=inits=defaultInits=0;}
 }
 static final class Adapter implements Mapper {
  final Observer state;final int index;Adapter(Observer state,int index){this.state=state;this.index=index;}
  public Object apply(Object value){return state.apply(index,value);}
 }
 public static void main(String[] args)throws Throwable {
  int route=Integer.parseInt(args[0]),mode=Integer.parseInt(args[1]);
  for(int attempt=0;attempt<2;attempt++){
   Observer state=new Observer(mode,attempt);Trace.probe=state;
   if(state.defaultsInitialized)Defaults.witness=state.markers[0];
   for(int round=0;round<(mode==20?2:1);round++){
    if(round>0){for(int i=0;i<24;i++)state.mapped[i]=new Token();state.reset();Defaults.witness=state.markers[0];}
    Trace.created=null;Object result=null;Throwable caught=null;
    try{result=invoke(route,mode==1?null:state);}catch(Throwable error){caught=error;}
    if(mode==1){require(caught instanceof NullPointerException&&result==null&&state.reads==0&&state.inits==0&&state.defaultInits==0);}
    else if(mode==18||mode==19){
     require(result==null&&state.reads==0&&Trace.created==null);
     if(attempt==0)require(state.inits==1&&(mode==18?caught==state.failure:caught instanceof ExceptionInInitializerError&&caught.getCause()==state.failure));
     else require(state.inits==0&&caught instanceof NoClassDefFoundError);
    }else if(mode==23||mode==24){
     require(result==null&&state.reads==0&&Trace.created==null&&state.initialized);
     if(attempt==0)require(state.defaultInits==1&&(mode==23?caught==state.failure:caught instanceof ExceptionInInitializerError&&caught.getCause()==state.failure));
     else require(state.defaultInits==0&&caught instanceof NoClassDefFoundError);
    }else if(mode>=2&&mode<=9){
     int expected=mode==2?8:mode==3||mode==4?6:mode==5||mode==6?7:9;
     require(result==null&&state.reads==expected&&state.entry==0&&state.after==0&&Trace.created==null);
     require(state.gets==expected-(mode==3||mode==4?1:0));
     require(state.calls==expected-(mode==2||mode==5||mode==6?1:mode==3||mode==4?1:0));
     require(mode==2||mode==9?caught instanceof NullPointerException:caught==state.failure);
    }else if(mode>=11&&mode<=14){
     require(result==null&&caught==state.failure&&state.entry==1&&state.after==(mode>=13?1:0)&&Trace.created!=null);state.fields(Trace.created,mode>=13);
    }else {
     require(caught==null&&result==Trace.created&&state.entry==1&&state.after==1);state.fields(result,true);
     if(mode==15)require(((Object[])state.markers[3])[0]==state);
     if(mode==16)require(((Object[])state.mapped[4])[0]==state);
    }
   }
  }
  System.out.println("attempts=2,field-receiver-reuse=matched");
 }
}
