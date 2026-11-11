import fixture.*;
import java.lang.invoke.*;
import java.lang.reflect.*;
import java.util.*;

public final class ProducerConsumerFactoryHarness {
 static final String MARKER="owned producer consumer factory";
 static void require(boolean ok){if(!ok)throw new AssertionError(MARKER);}
 static final class Token {
  public int hashCode(){throw new AssertionError(MARKER);}
  public String toString(){throw new AssertionError(MARKER);}
 }
 static final class Checked extends Exception {}
 @SuppressWarnings("unchecked") static <E extends Throwable> void fail(Throwable error)throws E{throw(E)error;}
 static final class Scenario implements Point,Iterable<Object>,Comparator<Object> {
  final int mode,decision;final Object a,b,c=new Token();final Throwable failure;
  final StringBuilder events=new StringBuilder();Object second;List<?> result;int iterators,reads,comparisons;
  Scenario(int mode){
   this.mode=mode;decision=mode==5?0:mode==6?17:-3;
   a=mode==1||mode==2?null:new Token();b=mode==2?null:mode==3?a:new Token();second=b;
   failure=mode==8||mode==10?new AssertionError("failure"):mode==7||mode==11?new Checked():new IllegalStateException("failure");
  }
  void event(String text){events.append(text);}
  void abort(){ProducerConsumerFactoryHarness.<RuntimeException>fail(failure);}
  public Iterator<Object> iterator(){
   event("I");require(++iterators==1);if(mode==10)abort();
   return new Iterator<Object>() {
    public boolean hasNext(){throw new AssertionError(MARKER);}
    public Object next(){
     int index=reads++;event(index==0?"N0":"N1");require(index<2);
     if(mode==11&&index==0||mode==12&&index==1)abort();
     return index==0?a:second;
    }
    public void remove(){throw new AssertionError(MARKER);}
   };
  }
  public int compare(Object left,Object right){
   event("C");require(++comparisons==1&&left==a&&right==second);
   if(mode==7||mode==8)abort();if(mode==9)throw(RuntimeException)null;
   return decision;
  }
  public void visit(int phase,Object first,Object other){
   event("P"+phase);
   if(phase==0)require(first==this&&other==this&&reads==0&&iterators==0);
   else if(phase==1){require(first==a&&other==this&&reads==1);if(mode==14)second=c;}
   else if(phase==2)require(first==second&&other==this&&reads==2);
   else if(phase==3){require(first==a&&other==second&&comparisons==1);if(mode==13)abort();}
   else {
    require(phase==4&&other==this&&first instanceof ArrayList);result=(List<?>)first;
    require(result.size()==2&&result.get(0)==(decision<=0?a:second)&&result.get(1)==(decision<=0?second:a));
    if(mode==15)((List<Object>)result).set(0,c);
   }
  }
 }
 static List<?> call(int route,Scenario value)throws Throwable {
  if(route==0)return Core.call(value,value);
  if(route==1){try{return(List<?>)Core.class.getMethod("call",Iterable.class,Comparator.class).invoke(null,value,value);}catch(InvocationTargetException error){throw error.getCause();}}
  MethodHandle handle=MethodHandles.publicLookup().findStatic(Core.class,"call",MethodType.methodType(List.class,Iterable.class,Comparator.class));
  return(List<?>)handle.invokeExact((Iterable)value,(Comparator)value);
 }
 public static void main(String[] args)throws Throwable {
  int route=Integer.parseInt(args[0]),mode=Integer.parseInt(args[1]);
  for(int attempt=0;attempt<2;attempt++){
   Scenario s=new Scenario(mode);Trace.probe=s;List<?> actual=null;Throwable caught=null;
   try{actual=call(route,s);}catch(Throwable error){caught=error;}
   String full="P0IN0P1N1P2CP3P4",expected=full;
   if(mode==10)expected="P0I";else if(mode==11)expected="P0IN0";else if(mode==12)expected="P0IN0P1N1";
   else if(mode>=7&&mode<=9)expected="P0IN0P1N1P2C";else if(mode==13)expected="P0IN0P1N1P2CP3";
   require(s.events.toString().equals(expected));
   if(mode>=7&&mode<=13){require(actual==null&&s.result==null);if(mode==9)require(caught instanceof NullPointerException);else require(caught==s.failure);}
   else {require(caught==null&&actual==s.result&&actual.size()==2);require(actual.get(0)==(mode==15?s.c:s.decision<=0?s.a:s.second));require(actual.get(1)==(s.decision<=0?s.second:s.a));}
  }
  System.out.println("attempts=2,producer-consumer-factory=matched");
 }
}
