import fixture.*;
import java.lang.invoke.*;
import java.lang.reflect.*;

public final class SelectorArrayCarrierHarness {
 static final String MARKER="owned selector array carrier";
 static void require(boolean ok){if(!ok)throw new AssertionError(MARKER);}
 static final StringBuilder events=new StringBuilder();
 static Throwable selectorFailure,resultFailure;
 static boolean throwNull;
 static int selected,compared;
 static Object observedInput,observedLeft,observedRight;
 static final class Token implements Key<Object> {
  public int hashCode(){throw new AssertionError(MARKER);}
  public String toString(){throw new AssertionError(MARKER);}
 }
 static final class Checked extends Exception {}
 @SuppressWarnings("unchecked") static <E extends Throwable> void fail(Throwable error)throws E{throw(E)error;}
 @SuppressWarnings("rawtypes") static final class RawSelector implements Selector {
  final Object value;RawSelector(Object value){this.value=value;}
  public Object select(Object input){events.append("r;");selected++;observedInput=input;if(throwNull)throw (RuntimeException)null;if(selectorFailure!=null)SelectorArrayCarrierHarness.<RuntimeException>fail(selectorFailure);return value;}
 }
 static final class TypedSelector implements Selector<String,Key<?>> {
  final Key<?> value;TypedSelector(Key<?> value){this.value=value;}
  public Key<?> select(String input){events.append("t;");selected++;observedInput=input;if(selectorFailure!=null)SelectorArrayCarrierHarness.<RuntimeException>fail(selectorFailure);return value;}
 }
 static final class RawResult implements Result<Object> {
  public int compare(Object left,Object right){events.append("c;");compared++;observedLeft=left;observedRight=right;if(resultFailure!=null)SelectorArrayCarrierHarness.<RuntimeException>fail(resultFailure);return 71;}
 }
 static final class TypedResult implements Result<String> {
  public int compare(String left,String right){events.append("n;");compared++;observedLeft=left;observedRight=right;return 72;}
 }
 @SuppressWarnings({"rawtypes","unchecked"}) static Result make(int route,Selector[] items)throws Throwable {
  if(route==0)return Client.send(items);
  if(route==1)return Client.typed(items);
  if(route==2){try{return(Result)Provider.class.getMethod("choose",Selector[].class).invoke(null,(Object)items);}catch(InvocationTargetException error){throw error.getCause();}}
  return(Result)MethodHandles.publicLookup().findStatic(Provider.class,"choose",MethodType.methodType(Result.class,Selector[].class)).asFixedArity().invokeExact(items);
 }
 static void contract()throws Exception {
  Method method=Provider.class.getMethod("choose",Selector[].class);
  require(method.isVarArgs()&&method.getTypeParameters().length==1);
  TypeVariable<?> formal=method.getTypeParameters()[0];
  ParameterizedType callback=(ParameterizedType)((GenericArrayType)method.getGenericParameterTypes()[0]).getGenericComponentType();
  require(callback.getRawType()==Selector.class);
  WildcardType input=(WildcardType)callback.getActualTypeArguments()[0];
  require(input.getLowerBounds().length==1&&input.getLowerBounds()[0]==formal);
  WildcardType output=(WildcardType)callback.getActualTypeArguments()[1];
  ParameterizedType key=(ParameterizedType)output.getUpperBounds()[0];
  require(key.getRawType()==Key.class&&key.getActualTypeArguments()[0] instanceof WildcardType);
  WildcardType star=(WildcardType)key.getActualTypeArguments()[0];
  require(star.getLowerBounds().length==0&&star.getUpperBounds().length==1&&star.getUpperBounds()[0]==Object.class);
  ParameterizedType result=(ParameterizedType)method.getGenericReturnType();
  require(result.getRawType()==Result.class&&result.getActualTypeArguments()[0]==formal);
 }
 @SuppressWarnings({"rawtypes","unchecked"}) public static void main(String[] args)throws Throwable {
  int route=Integer.parseInt(args[0]),mode=Integer.parseInt(args[1]);contract();
  for(int attempt=0;attempt<2;attempt++){
   events.setLength(0);selected=compared=0;selectorFailure=resultFailure=null;throwNull=false;
   Object unseen=new Object();observedInput=observedLeft=observedRight=unseen;
   Key<?> firstKey=new Token(),secondKey=new Token();
   RawSelector raw=new RawSelector(mode==8?new Object():mode==9?null:firstKey);
   TypedSelector typed=new TypedSelector(secondKey);
   Selector[] items=new Selector[]{raw,typed};
   if(mode==1||mode==27)items=null;
   if(mode==2)items=new Selector[0];
   if(mode==7)items[0]=null;
   if(mode==21)items=new TypedSelector[]{typed,typed};
   if(mode==22)items[1]=null;
   Selector initial=items==null||items.length==0?null:items[0];
   Trace.slot=mode==3?-1:mode==4||mode==5||mode==6?1:0;
   Trace.input=mode==4||mode==21?new String("input"):mode==6||mode==28?null:new Object();
   Trace.write=mode!=20;Trace.calls=Trace.phase=0;Trace.array=Trace.first=Trace.key=unseen;
   Object left=mode==19?new String("left"):mode==29?null:new Object();
   Object right=mode==19?new String("right"):mode==29?null:new Object();
   boolean narrowResult=mode==18||mode==19||mode==29;
   Object expected=mode==16?null:mode==17?new Object():narrowResult?new TypedResult():new RawResult();
   Trace.output=expected;
   final Throwable failure=mode==11||mode==26||mode==27?new AssertionError("failure"):mode==12||mode==14||mode==23||mode==24?new Checked():new IllegalStateException("failure");
   final int failPhase=mode==10||mode==23||mode==27?1:mode==11||mode==25?2:mode==12||mode==26?3:0;
   Trace.probe=new Point(){public void visit(int phase){events.append(phase).append(';');if(phase==failPhase)SelectorArrayCarrierHarness.<RuntimeException>fail(failure);}};
   if(mode==13||mode==14)selectorFailure=failure;
   if(mode==15)throwNull=true;
   if(mode==24)resultFailure=failure;
   Throwable caught=null;Result actual=null;int value=0;
   try{actual=make(route,items);require(actual==expected);if(actual!=null)value=actual.compare(left,right);}catch(Throwable error){caught=error;}
   boolean point1Failure=mode==10||mode==23||mode==27;
   boolean noIndex=items==null||items.length==0||mode==3||point1Failure;
   boolean bridgeFailure=mode==5;
   boolean callbackBody=!noIndex&&mode!=7&&!bridgeFailure;
   boolean keyReached=!noIndex&&mode!=7&&!bridgeFailure&&mode!=8&&mode!=13&&mode!=14&&mode!=15;
   int phase=keyReached?(mode==11||mode==25?2:3):1;
   boolean invoked=keyReached&&failPhase!=2&&failPhase!=3&&mode!=16&&mode!=17&&mode!=18;
   String trace="1;"+(callbackBody?(mode==4||mode==6||mode==21?"t;":"r;"):"")+(keyReached?"2;":"")+(phase==3?"3;":"")+(invoked?(narrowResult?"n;":"c;"):"");
   require(Trace.calls==1&&Trace.array==items&&Trace.phase==phase&&events.toString().equals(trace));
   require(Trace.first==(noIndex?unseen:items==null?null:Trace.slot==1?typed:initial));
   require(selected==(callbackBody?1:0)&&observedInput==(callbackBody?Trace.input:unseen));
   require(Trace.key==(keyReached?(mode==9?null:mode==4||mode==6||mode==21?secondKey:firstKey):unseen));
   require(compared==(invoked?1:0)&&observedLeft==(invoked?left:unseen)&&observedRight==(invoked?right:unseen));
   if(items!=null&&items.length!=0)require(items[0]==(Trace.write&&phase==3?null:initial));
   if(mode==10||mode==11||mode==12||mode==13||mode==14||mode==23||mode==24||mode==25||mode==26||mode==27)require(caught==failure);
   else if(mode==1||mode==7||mode==15)require(caught instanceof NullPointerException);
   else if(mode==2||mode==3)require(caught instanceof ArrayIndexOutOfBoundsException);
   else if(mode==5||mode==8||mode==17||mode==18)require(caught instanceof ClassCastException);
   else require(caught==null&&(mode==16?actual==null:value==(narrowResult?72:71)));
  }
  System.out.println("attempts=2,selector-array-carrier=matched");
 }
}
