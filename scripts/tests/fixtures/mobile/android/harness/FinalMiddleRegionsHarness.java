import fixture.*;
import java.lang.invoke.*;
import java.lang.reflect.*;
import java.util.*;

public final class FinalMiddleRegionsHarness {
 static final String MARKER="owned final middle regions";
 static void require(boolean ok){if(!ok)throw new AssertionError(MARKER);}
 static final class Token {
  public int hashCode(){throw new AssertionError(MARKER);}
  public String toString(){throw new AssertionError(MARKER);}
 }
 static final class Checked extends Exception {}
 @SuppressWarnings("unchecked") static <E extends Throwable> void fail(Throwable value)throws E{throw(E)value;}
 static final class Observer implements Context {
  final int mode,choice,rounds,kind;
  final Object openValue,leftValue,rightValue,lastValue;
  final Object[] values={new Token(),new Token(),new Token()};
  final Throwable failure;
  final String failAt;
  final List<String> expected=new ArrayList<>(),actual=new ArrayList<>();
  Core leaked;boolean firstReturned,selectedReturned;
  Observer(int mode,int attempt){
   this.mode=mode;
   choice=mode==24?Integer.MIN_VALUE:mode==0||mode==3||mode==5||mode==8||mode==9||mode==13||mode==15||mode==17||mode==19||mode==20||mode==22||mode==26?0:1;
   rounds=mode==2?-5:mode==25?Integer.MIN_VALUE:mode==0||mode==1||mode==6||mode>=8&&mode<=16?0:mode==4||mode==18||mode==21||mode>=24?3:1;
   kind=mode==23?99:mode==4||mode==18||mode==27?1:mode==5||mode==6||mode==19||mode==20?2:0;
   openValue=mode==8?null:new Token();leftValue=mode==9?null:new Token();rightValue=mode==10?null:new Token();lastValue=mode==11?null:new Token();
   failure=mode==13||mode==20?new AssertionError("failure"):mode==14||mode==16||mode==18||mode==21?new Checked():new IllegalStateException("failure");
   failAt=mode==12?"open":mode==13?"left":mode==14?"right":mode==15?"after0":mode==16?"after1":mode==17||mode==22?"step0":mode==18?"other1":mode==20?"problem":mode==21?"finish":null;
   if(mode==7)return;
   if(add("open"))return;
   if(add(choice==0?"left":"right"))return;
   if(add(choice==0?"after0":"after1"))return;
   for(int i=0;i<rounds;i++){
    if(kind!=0&&kind!=1){add("problem");return;}
    if(add((kind==0?"step":"other")+i))return;
   }
   add("finish");
  }
  boolean add(String value){expected.add(value);return value.equals(failAt);}
  Object selected(){return mode==26||mode==27?leaked:choice==0?leftValue:rightValue;}
  Object result(){return mode==28?leaked:lastValue;}
  Object value(int index){return mode==29?selected():values[index];}
  void hit(Core target,String operation){
   if(leaked==null)leaked=target;
   require(target==leaked&&actual.size()<expected.size()&&expected.get(actual.size()).equals(operation));
   boolean opening=operation.equals("open"),selecting=operation.equals("left")||operation.equals("right");
   require(target.first==(opening?null:openValue)&&target.selected==(opening||selecting?null:selected())&&target.last==null);
   actual.add(operation);
   if(operation.equals(failAt)){
    if(mode==22)throw (RuntimeException)null;
    FinalMiddleRegionsHarness.<RuntimeException>fail(failure);
   }
  }
  public Object open(Core target){hit(target,"open");firstReturned=true;return openValue;}
  public Object left(Core target){hit(target,"left");selectedReturned=true;return selected();}
  public Object right(Core target){hit(target,"right");selectedReturned=true;return selected();}
  public void after(Core target,int branch){hit(target,"after"+branch);require(branch==(choice==0?0:1));}
  public Object step(Core target,int index){hit(target,"step"+index);require(kind==0);return value(index);}
  public Object other(Core target,int index){hit(target,"other"+index);require(kind==1);return value(index);}
  public RuntimeException problem(Core target){hit(target,"problem");require(kind!=0&&kind!=1);return mode==19?null:(RuntimeException)failure;}
  public Object finish(Core target,Object current){hit(target,"finish");require(current==(rounds<=0?null:value(rounds-1)));return result();}
 }
 static Core create(int route,Context context,int choice,int rounds,int kind)throws Throwable {
  if(route==0)return new Core(context,choice,rounds,kind);
  if(route==1){try{return Core.class.getConstructor(Context.class,int.class,int.class,int.class).newInstance(context,choice,rounds,kind);}catch(InvocationTargetException error){throw error.getCause();}}
  return(Core)MethodHandles.publicLookup().findConstructor(Core.class,MethodType.methodType(void.class,Context.class,int.class,int.class,int.class)).invokeExact(context,choice,rounds,kind);
 }
 public static void main(String[] args)throws Throwable {
  int route=Integer.parseInt(args[0]),mode=Integer.parseInt(args[1]);
  for(String name:new String[]{"first","selected","last"})require(Modifier.isFinal(Core.class.getField(name).getModifiers()));
  for(int attempt=0;attempt<2;attempt++){
   Observer observer=new Observer(mode,attempt);Core result=null;Throwable caught=null;
   try{result=create(route,mode==7?null:observer,observer.choice,observer.rounds,observer.kind);}catch(Throwable error){caught=error;}
   require(observer.expected.equals(observer.actual));
   boolean abrupt=observer.failAt!=null||mode==7||mode==5||mode==19||mode==23;
   if(mode==7||mode==19||mode==22)require(caught instanceof NullPointerException);
   else if(abrupt)require(caught==observer.failure);
   else require(caught==null&&result==observer.leaked&&result.last==observer.result());
   if(mode==7)require(observer.leaked==null);
   else {
    require(observer.leaked!=null);
    require(observer.leaked.first==(observer.firstReturned?observer.openValue:null));
    require(observer.leaked.selected==(observer.selectedReturned?observer.selected():null));
    if(abrupt)require(result==null&&observer.leaked.last==null);
   }
  }
  System.out.println("attempts=2,final-middle-regions=matched");
 }
}
