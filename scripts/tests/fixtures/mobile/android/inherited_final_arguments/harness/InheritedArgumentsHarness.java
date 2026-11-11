import fixture.*;
import java.lang.invoke.*;
import java.lang.reflect.*;
public final class InheritedArgumentsHarness {
 private static void require(boolean value) {
  if(!value) throw new AssertionError("owned inherited final arguments changed");
 }
 public static void main(String[] args) throws Throwable {
  int route=Integer.parseInt(args[0]),mode=Integer.parseInt(args[1]);
  for(int attempt=0;attempt<2;++attempt) {
   Object left=mode==1 || mode==3?null:new Object();
   Object right=mode==4?left:new Object();
   String tag=mode==2 || mode==3?null:new String("first");
   String next=new String("second");
   int[] failureSteps={1,1,2,3,4,9};
   int selected=mode<5?-1:(mode-5)/2;
   int step=selected<0?0:failureSteps[selected];
   int occurrence=selected==1?2:1;
   Throwable failure=mode%2==0?new AssertionError("sentinel"):new IllegalStateException("sentinel");
   Trace.events=0;Trace.failStep=step;Trace.failOccurrence=occurrence;Trace.matching=0;Trace.calls=0;
   Trace.failure=failure;Trace.alternate=attempt==0?new Object():right;Trace.initialized=right;
   Trace.nextTag=next;Trace.tag=null;Trace.value=null;Trace.receiver=null;Trace.owner=null;Trace.partial=null;
   Child<?,?> output=null;Throwable caught=null;
   try {
    if(route==0) output=InheritedArgumentsCaller.make(left,tag);
    else if(route==1) output=(Child<?,?>)Child.class.getConstructor(Object.class,String.class).newInstance(left,tag);
    else if(route==2) output=(Child<?,?>)MethodHandles.publicLookup().findConstructor(Child.class,MethodType.methodType(void.class,Object.class,String.class)).invoke(left,tag);
    else output=new Child<Object,Object>(left,tag);
   } catch(InvocationTargetException e) {caught=e.getCause();} catch(Throwable e) {caught=e;}
   long expected=0;int calls=0,last=0,matches=0;
   if(step==9) {
    if(attempt==0) expected=9;
   } else {
    if(attempt==0) expected=9;
    int[] steps={1,1,2,3,4};
    for(int at=0;at<steps.length;++at) {
     int s=steps[at];expected=expected*10+s;last=at+1;
     if(s==1) ++calls;
     if(s==step && ++matches==occurrence) break;
    }
   }
   require(Trace.events==expected && Trace.calls==calls);
   require(Trace.owner!=null && Trace.owner.item==left && Trace.owner.tag==tag);
   require(Trace.receiver==(calls==0?null:Trace.owner));
   require(Trace.value==(calls==0?null:calls==1?left:right));
   require(Trace.tag==(calls==0?null:calls==1?tag:next));
   require((Trace.partial!=null)==(last>=3));
   if(Trace.partial!=null) require(Trace.partial.left==left && Trace.partial.right==(last>=4?right:null));
   require(Trace.owner.output==(last>=5?Trace.partial:null));
   require(output==(caught==null?Trace.owner:null));
   if(step==9) {
    if(attempt==1) require(caught instanceof NoClassDefFoundError);
    else if(failure instanceof Error) require(caught==failure);
    else require(caught instanceof ExceptionInInitializerError && caught.getCause()==failure);
   } else require(caught==(step==0?null:failure));
   System.out.println("route="+route+",mode="+mode+",attempt="+attempt+",events="+expected);
  }
 }
}
