import fixture.*;
import java.util.*;
import java.lang.reflect.*;
import java.lang.invoke.*;
public final class ConstructorArgumentRegionsHarness {
 static final String MARKER="owned constructor regions";
 static void require(boolean value){if(!value)throw new AssertionError(MARKER);}
 static final class Token {public int hashCode(){throw new AssertionError(MARKER);}public String toString(){throw new AssertionError(MARKER);}}
 static final class Checked extends Exception {}
 @SuppressWarnings("unchecked") static <E extends Throwable> void fail(Throwable value)throws E{throw(E)value;}
 static final Constructor<Core> NORMAL,NESTED;static final MethodHandle NH,IH;
 static {try{NORMAL=Core.class.getConstructor(long.class,int.class,Context.class,Object.class);NESTED=Core.class.getConstructor(Object.class,Context.class,long.class);NH=MethodHandles.lookup().unreflectConstructor(NORMAL);IH=MethodHandles.lookup().unreflectConstructor(NESTED);}catch(Exception error){throw new ExceptionInInitializerError(error);}}
 static Object invoke(int route,boolean nested,long left,int right,long wide,Context context,Object seed)throws Throwable {
  try{if(route==0)return nested?NESTED.newInstance(seed,context,wide):NORMAL.newInstance(left,right,context,seed);if(route==1)return nested?(Core)IH.invokeExact(seed,context,wide):(Core)NH.invokeExact(left,right,context,seed);return nested?new Core(seed,context,wide):new Core(left,right,context,seed);}catch(InvocationTargetException error){throw error.getCause();}
 }
 static int fault(int mode){if(mode==2||mode==3)return 20;if(mode==4||mode==5)return 21;if(mode>=6&&mode<=11)return new int[]{2,3,5,6,7,8}[mode-6];if(mode==12||mode==13)return 1;if(mode==14||mode==15)return 4;if(mode==16||mode==17)return 0;return -1;}
 static int classIndex(int phase){return phase==0?0:phase==1?1:phase==4?2:-1;}
 static final class Plan {
  final List<Integer> events=new ArrayList<>();final int mode;final int[] flags;int outcome;
  Plan(int mode,int[] flags){this.mode=mode;this.flags=flags;}
  boolean add(int phase){events.add(phase);if(phase==fault(mode)){outcome=classIndex(phase)>=0&&mode%2==0?2:1;return false;}return true;}
  boolean init(int phase){int value=flags[classIndex(phase)];if(value==2){outcome=3;return false;}return value==1||add(phase);}
  void prepare(boolean nested){
   if(!init(0))return;
   if(nested){if(!add(7)||!add(8))return;}
   else {if(!init(1))return;if(mode!=1&&!add(20))return;if(!add(2)||!add(3))return;}
   if(!init(4))return;if(mode!=1&&!add(21))return;if(!add(5)||!add(6)||!add(7)||!add(8))return;outcome=0;
  }
 }
 static final class Record {
  final Object object;final int kind;final Object first,second,context;boolean assigned;
  Record(Object object,int kind,Object first,Object second,Object context){this.object=object;this.kind=kind;this.first=first;this.second=second;this.context=context;}
 }
 static final class Observer implements Context,Probe {
  final boolean nested;final int mode;final int[] flags;final long left,wide;final int right;final Object seed,lt=new Token(),rt=new Token();final Throwable failure;final Plan plan;final List<Record> records=new ArrayList<>();
  boolean started;int at,baseEntries;Object leftValue,rightValue,inner;Record firstRecord,secondRecord,lastCore;
  Observer(boolean nested,int mode,int attempt,int[] flags){this.nested=nested;this.mode=mode;this.flags=flags;left=attempt==0?Long.MIN_VALUE+73:Long.MAX_VALUE-91;right=attempt==0?Integer.MIN_VALUE+37:Integer.MAX_VALUE-29;wide=attempt==0?0x12345678fffffff0L:0xfedcba98000000a1L;seed=mode==20?new Object[]{new Token()}:new Token();failure=mode>=3&&mode<=17&&mode%2==1?new AssertionError("failure"):new Checked();plan=new Plan(mode,flags);plan.prepare(nested);}
  void step(int phase){require(started&&at<plan.events.size()&&plan.events.get(at++)==phase);}
  void raise(int phase){if(phase==fault(mode))ConstructorArgumentRegionsHarness.<RuntimeException>fail(failure);}
  public Object left(Object input){require(input==seed);step(20);raise(20);if(mode==20)((Object[])seed)[0]=this;leftValue=mode==19?null:mode==18||mode==20?seed:lt;return leftValue;}
  public Object right(Object input){require(input==seed);step(21);raise(21);if(mode==20)((Object[])seed)[0]=this;rightValue=mode==19?null:mode==18||mode==20?seed:rt;return rightValue;}
  void fields(Record record){
   if(record.kind==0){First value=(First)record.object;require(value.number==(record.assigned?left:0)&&value.token==(record.assigned?leftValue:null));}
   else if(record.kind==1){Second value=(Second)record.object;require(value.number==(record.assigned?(nested?(int)wide:right):0)&&value.token==(record.assigned?rightValue:null));}
   else {Core value=(Core)record.object;require(value.first==(record.assigned?record.first:null)&&value.second==(record.assigned?record.second:null)&&value.context==(record.assigned?record.context:null));}
  }
  public void visit(int phase,Object value){
   step(phase);int ci=classIndex(phase);
   if(ci>=0){require(value==null&&flags[ci]==0);flags[ci]=phase==fault(mode)?2:1;raise(phase);return;}
   if(phase==2){require(!nested&&firstRecord==null&&Trace.first==value);firstRecord=new Record(value,0,null,null,null);records.add(firstRecord);fields(firstRecord);}
   else if(phase==3){require(firstRecord!=null&&value==firstRecord.object);firstRecord.assigned=true;fields(firstRecord);}
   else if(phase==5){require(secondRecord==null&&Trace.second==value);secondRecord=new Record(value,1,null,null,null);records.add(secondRecord);fields(secondRecord);}
   else if(phase==6){require(secondRecord!=null&&value==secondRecord.object);secondRecord.assigned=true;fields(secondRecord);}
   else if(phase==7){require(Trace.core==value);boolean inside=nested&&baseEntries++==0;if(!nested)baseEntries++;Object expectedFirst=inside?null:nested?inner:firstRecord.object;Object expectedSecond=inside?null:secondRecord.object;lastCore=new Record(value,2,expectedFirst,expectedSecond,mode==1?null:this);records.add(lastCore);fields(lastCore);if(inside)inner=value;}
   else {require(phase==8&&lastCore!=null&&value==lastCore.object);lastCore.assigned=true;fields(lastCore);}
   raise(phase);
  }
 }
 public static void main(String[] args)throws Throwable {
  int route=Integer.parseInt(args[0]);boolean nested=Integer.parseInt(args[1])!=0;int mode=Integer.parseInt(args[2]);int[] flags={0,0,0};
  for(int attempt=0;attempt<2;attempt++)for(int round=0;round<(mode==21?2:1);round++){
   Observer state=new Observer(nested,mode,attempt,flags);Trace.probe=state;Trace.first=Trace.second=Trace.core=null;Object result=null;Throwable caught=null;state.started=true;
   try{result=invoke(route,nested,state.left,state.right,state.wide,mode==1?null:state,state.seed);}catch(Throwable error){caught=error;}
   require(state.at==state.plan.events.size());for(Record record:state.records)state.fields(record);
   if(state.plan.outcome==0){require(caught==null&&result==Trace.core&&state.lastCore!=null&&result==state.lastCore.object&&state.lastCore.assigned);if(mode==20)require(((Object[])state.seed)[0]==state);}
   else {require(result==null);if(state.plan.outcome==1)require(caught==state.failure);else if(state.plan.outcome==2)require(caught instanceof ExceptionInInitializerError&&caught.getCause()==state.failure);else require(caught instanceof NoClassDefFoundError);}
  }
  System.out.println("attempts=2,constructor-regions=matched");
 }
}
