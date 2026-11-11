import fixture.*;
import java.lang.invoke.*;
import java.lang.reflect.*;
public final class InheritedGenericHarness {
 private static void require(boolean value) {
  if(!value) throw new AssertionError("owned inherited generic allocation changed");
 }
 public static void main(String[] args) throws Throwable {
  int route=Integer.parseInt(args[0]),mode=Integer.parseInt(args[1]);
  for(int attempt=0;attempt<2;++attempt) {
   Object left=new Object(),right=new Object();
   Object[] a=mode==2?null:mode==3?new Object[0]:new Object[]{left};
   Object[] b=mode==4?null:mode==5?new Object[0]:new Object[]{new Object(),new Object(),right};
   Child<Object,Object> receiver=mode==1?null:new Child<Object,Object>();
   int step=mode>=18?9:mode>=14?3+(mode-14)/2:mode>=6?1+((mode-6)%4)/2:0;
   int occurrence=mode>=10 && mode<14?2:1;
   Throwable failure=mode%2==1?new AssertionError("sentinel"):new IllegalStateException("sentinel");
   Trace.events=0;Trace.failStep=step;Trace.failOccurrence=occurrence;Trace.matching=0;
   Trace.buffers=0;Trace.indexes=0;Trace.index=2;Trace.nextIndex=3;
   Trace.failure=failure;Trace.current=attempt==0?new Object[0]:a;Trace.initialized=a;Trace.alternate=b;
   Trace.receiver=null;Trace.partial=null;
   Target output=null;Throwable caught=null;
   try {
    if(route==0) output=InheritedGenericCaller.make(receiver);
    else if(route==1) output=(Target)Child.class.getMethod("make").invoke(receiver);
    else if(route==2) output=(Target)MethodHandles.publicLookup().findVirtual(Child.class,"make",MethodType.methodType(Target.class)).invoke(receiver);
    else output=receiver.make();
   } catch(InvocationTargetException e) {caught=e.getCause();} catch(Throwable e) {caught=e;}
   long expected=0;int buffers=0,indexes=0,last=0,matches=0;boolean stopped=receiver==null;
   if(!stopped && step==9) {if(attempt==0) expected=9;stopped=true;}
   if(!stopped) {
    if(attempt==0) expected=9;
    int[] steps={1,2,1,2,3,4};
    for(int at=0;at<steps.length;++at) {
     int s=steps[at];expected=expected*10+s;last=at+1;
     if(s==1) ++buffers;if(s==2) ++indexes;
     if(s==step && ++matches==occurrence) break;
     if(at==1 && (a==null || a.length==0) || at==3 && (b==null || b.length<3)) break;
    }
   }
   require(Trace.events==expected && Trace.buffers==buffers && Trace.indexes==indexes);
   if(step==9) {
    if(attempt==1) require(caught instanceof NoClassDefFoundError);
    else if(failure instanceof Error) require(caught==failure);
    else require(caught instanceof ExceptionInInitializerError && caught.getCause()==failure);
   } else if(mode==1 || mode==2 || mode==4) require(caught instanceof NullPointerException);
   else if(mode==3 || mode==5) require(caught instanceof ArrayIndexOutOfBoundsException);
   else require(caught==(step==0?null:failure));
   require(Trace.receiver==(buffers==0?null:receiver));
   require((Trace.partial!=null)==(last>=5));
   if(Trace.partial!=null) require(Trace.partial.left==left && Trace.partial.right==(last>=6?right:null));
   require(output==(caught==null?Trace.partial:null));
   require(Trace.index==(indexes==0 || last==2 && step==2?2:3));
   System.out.println("route="+route+",mode="+mode+",attempt="+attempt+",events="+expected);
  }
 }
}
