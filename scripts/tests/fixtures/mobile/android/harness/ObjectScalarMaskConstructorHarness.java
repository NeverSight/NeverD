import fixture.*;
import java.lang.invoke.*;
import java.lang.reflect.*;

public final class ObjectScalarMaskConstructorHarness {
 static final String MARKER="owned object scalar mask constructor";
 static void require(boolean ok){if(!ok)throw new AssertionError(MARKER);}
 static final class Token {
  public int hashCode(){throw new AssertionError(MARKER);}
  public String toString(){throw new AssertionError(MARKER);}
 }
 static final class Checked extends Exception {}
 @SuppressWarnings("unchecked") static <E extends Throwable> void fail(Throwable error)throws E{throw(E)error;}
 static final Class<?> CORE,MARKER_TYPE;
 static final Constructor<?> CTOR;
 static final MethodHandle HANDLE;
 static final Field[] FIELDS=new Field[4];
 static {
  try {
   CORE=Class.forName("fixture.Core");MARKER_TYPE=Class.forName("kotlin.jvm.internal.DefaultConstructorMarker");
   CTOR=CORE.getDeclaredConstructor(Object.class,int.class,int.class,String.class,int.class,MARKER_TYPE);CTOR.setAccessible(true);
   HANDLE=MethodHandles.lookup().unreflectConstructor(CTOR).asType(MethodType.methodType(Object.class,Object.class,int.class,int.class,String.class,int.class,Object.class));
   String[] names={"value","first","second","tag"};
   for(int i=0;i<4;i++){FIELDS[i]=CORE.getDeclaredField(names[i]);FIELDS[i].setAccessible(true);require(Modifier.isFinal(FIELDS[i].getModifiers()));}
  }catch(Exception error){throw new ExceptionInInitializerError(error);}
 }
 static final class Observer implements Point {
  final int mode,first,second,failPhase;final Object item;final String tag;final Throwable failure;
  Object instance;int phases;
  Observer(int mode,Object item,int first,int second,String tag){
   this.mode=mode;this.item=item;this.first=first;this.second=second;this.tag=tag;
   failPhase=mode==6||mode==8||mode==17?0:mode==7||mode==9||mode==16?1:-1;
   failure=mode==7||mode==17?new AssertionError("failure"):mode==8||mode==16?new Checked():new IllegalStateException("failure");
  }
  void fields(boolean initialized){
   try{require(FIELDS[0].get(instance)==(initialized?item:null));require(FIELDS[1].getInt(instance)==(initialized?first:0));require(FIELDS[2].getInt(instance)==(initialized?second:0));require(FIELDS[3].get(instance)==(initialized?tag:null));}
   catch(IllegalAccessException error){throw new AssertionError(MARKER,error);}
  }
  public void visit(int phase,Object owner,Object actualItem,int actualFirst,int actualSecond,String actualTag){
   require(phase==phases++&&actualItem==item&&actualFirst==first&&actualSecond==second&&actualTag==tag);
   if(phase==0){require(instance==null&&owner.getClass()==CORE);instance=owner;fields(false);}
   else {require(phase==1&&owner==instance);fields(true);}
   if(mode==11&&phase==0)((Object[])item)[0]=this;
   if(phase==failPhase){if(mode==9)throw(RuntimeException)null;ObjectScalarMaskConstructorHarness.<RuntimeException>fail(failure);}
  }
 }
 static Object create(int route,Object item,int first,int second,String tag,int mask,Object marker)throws Throwable {
  if(route==0){try{return CTOR.newInstance(item,first,second,tag,mask,marker);}catch(InvocationTargetException error){throw error.getCause();}}
  return(Object)HANDLE.invokeExact(item,first,second,tag,mask,marker);
 }
 public static void main(String[] args)throws Throwable {
  int route=Integer.parseInt(args[0]),mode=Integer.parseInt(args[1]);
  Object marker=null;
  if(mode>=18){Constructor<?> m=MARKER_TYPE.getDeclaredConstructor();m.setAccessible(true);marker=m.newInstance();}
  int[] masks=new int[36];for(int i=0;i<32;i++)masks[i]=i;masks[32]=Integer.MIN_VALUE;masks[33]=-1;masks[34]=64;masks[35]=Integer.MIN_VALUE|8;
  for(int attempt=0;attempt<2;attempt++)for(int mask:masks){
   String text=mode==5||mode==12?null:mode==4||mode==13?new String(""):mode==19?new String(new char[]{'\ud800',0,'\udc00'}):new String("tag");
   Object item=mode==1||mode==12?null:mode==2?new int[]{1,2}:mode==3?text:mode==11?new Object[]{new Token()}:new Token();
   int left=mode==4?Integer.MIN_VALUE:mode==5?Integer.MAX_VALUE:mode==19?0:7;
   int right=mode==4?Integer.MAX_VALUE:mode==5?Integer.MIN_VALUE:mode==19?-1:19;
   Observer observer=new Observer(mode,item,left,(mask&4)!=0?Integer.MIN_VALUE:right,(mask&8)!=0?"":text);Trace.probe=observer;
   Object actual=null;Throwable caught=null;
   try{actual=create(route,item,left,right,text,mask,marker);}catch(Throwable error){caught=error;}
   require(observer.instance!=null&&observer.phases==(observer.failPhase<0?2:observer.failPhase+1));
   if(observer.failPhase>=0){require(actual==null);if(mode==9)require(caught instanceof NullPointerException);else require(caught==observer.failure);}
   else require(caught==null&&actual==observer.instance);
   observer.fields(observer.failPhase!=0);
   if(mode==11)require(((Object[])item)[0]==observer);
  }
  System.out.println("attempts=2,masks=36,object-scalar-mask-constructor=matched");
 }
}
