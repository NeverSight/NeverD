import fixture.*;
import java.lang.invoke.*;
import java.lang.reflect.*;
import kotlin.jvm.functions.Function2;

public final class BinaryMergeFactoryHarness {
 static final String MARKER="owned binary merge factory";
 static void require(boolean ok){if(!ok)throw new AssertionError(MARKER);}
 static int calls;static Object seenLeft,seenRight;static Throwable callbackFailure;static boolean throwNull;
 static final class Checked extends Exception {}
 @SuppressWarnings("unchecked") static <E extends Throwable> void fail(Throwable value)throws E{throw(E)value;}
 @SuppressWarnings("rawtypes") static final class Raw implements Function2 {
  final Object result;Raw(Object value){result=value;}
  public Object invoke(Object left,Object right){calls++;seenLeft=left;seenRight=right;if(throwNull)throw (RuntimeException)null;if(callbackFailure!=null)BinaryMergeFactoryHarness.<RuntimeException>fail(callbackFailure);return result;}
  public int hashCode(){throw new AssertionError(MARKER);}
  public String toString(){throw new AssertionError(MARKER);}
 }
 static final class Typed implements Function2<String,String,String> {
  final String result;Typed(String value){result=value;}
  public String invoke(String left,String right){calls++;seenLeft=left;seenRight=right;return result;}
  public int hashCode(){throw new AssertionError(MARKER);}
  public String toString(){throw new AssertionError(MARKER);}
 }
 @SuppressWarnings({"rawtypes","unchecked"}) static Box make(int route,String first,String second,Function2 merge)throws Throwable {
  if(route==0)return CoreKt.call(first,second,merge);
  if(route==1){try{return(Box)CoreKt.class.getMethod("build",String.class,String.class,Function2.class).invoke(null,first,second,merge);}catch(InvocationTargetException error){throw error.getCause();}}
  return(Box)MethodHandles.publicLookup().findStatic(CoreKt.class,"build",MethodType.methodType(Box.class,String.class,String.class,Function2.class)).invokeExact(first,second,merge);
 }
 static void contract()throws Exception {
  Method method=CoreKt.class.getMethod("build",String.class,String.class,Function2.class);
  require(method.getTypeParameters().length==1);
  TypeVariable<?> formal=method.getTypeParameters()[0];
  ParameterizedType callback=(ParameterizedType)method.getGenericParameterTypes()[2];
  require(callback.getRawType()==Function2.class);
  for(int i=0;i<3;i++){
   WildcardType slot=(WildcardType)callback.getActualTypeArguments()[i];
   Type[] bound=i==2?slot.getUpperBounds():slot.getLowerBounds();
   require(bound.length==1&&bound[0]==formal);
  }
  ParameterizedType output=(ParameterizedType)method.getGenericReturnType();
  require(output.getRawType()==Box.class&&output.getActualTypeArguments()[0]==formal);
 }
 @SuppressWarnings({"rawtypes","unchecked"}) public static void main(String[] args)throws Throwable {
  int route=Integer.parseInt(args[0]),mode=Integer.parseInt(args[1]);contract();
  for(int attempt=0;attempt<2;attempt++){
   final String first=mode==1||mode==3?null:new String("first");
   final String second=mode==2||mode==3?null:mode==4?first:new String("second");
   Object left=mode==6||mode==8?new String("left"):mode==9?null:new Object();
   Object right=mode==6||mode==7||mode==9?new String("right"):new Object();
   Object result=mode==10?null:mode>=6&&mode<=9?new String("result"):new Object();
   final Function2 merge=mode==5?null:mode>=6&&mode<=9?new Typed((String)result):new Raw(result);
   Box expected=new Box();final Object tag=new Object();expected.tag=tag;
   Trace.output=mode==11?null:mode==12?new Object():expected;
   final Throwable failure=mode==15?new AssertionError("failure"):mode==14||mode==17?new Checked():new IllegalStateException("failure");
   calls=0;seenLeft=seenRight=null;callbackFailure=mode==16||mode==17?failure:null;throwNull=mode==18;
   Trace.calls=0;Trace.first=Trace.second=Trace.callback=null;
   Trace.probe=new Point(){public void visit(){require(calls==0&&Trace.calls==1&&Trace.first==first&&Trace.second==second&&Trace.callback==merge);if(mode>=13&&mode<=15)BinaryMergeFactoryHarness.<RuntimeException>fail(failure);}};
   Throwable caught=null;Box actual=null;Object value=null;
   try{actual=make(route,first,second,merge);require(actual==expected&&actual.tag==tag);value=merge.invoke(left,right);}catch(Throwable error){caught=error;}
   require(Trace.calls==1&&Trace.first==first&&Trace.second==second&&Trace.callback==merge);
   boolean invoked=mode!=5&&mode!=7&&mode!=8&&!(mode>=11&&mode<=15);
   require(calls==(invoked?1:0)&&seenLeft==(invoked?left:null)&&seenRight==(invoked?right:null));
   if(mode>=13&&mode<=17)require(caught==failure);
   else if(mode==5||mode==11||mode==18)require(caught instanceof NullPointerException);
   else if(mode==7||mode==8||mode==12)require(caught instanceof ClassCastException);
   else require(caught==null&&actual==expected&&value==result);
  }
  System.out.println("attempts=2,binary-merge-factory=matched");
 }
}
