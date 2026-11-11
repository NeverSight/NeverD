import fixture.*;
import java.lang.invoke.*;
import java.lang.reflect.*;
public final class ReferenceArrayHarness {
 private static void require(boolean value) {
  if(!value) throw new AssertionError("owned reference array super changed");
 }
 public static void main(String[] args) throws Throwable {
  int route=Integer.parseInt(args[0]),mode=Integer.parseInt(args[1]);
  int count=mode==0?0:mode==1?1:mode==3?-1:3;
  int step=0,occurrence=1;
  if(mode>=5 && mode<13) {step=1+(mode-5)/4;occurrence=((mode-5)%4)/2==0?1:3;}
  else if(mode>=13 && mode<21) step=3+(mode-13)/2;
  else if(mode>=21) step=8;
  boolean error=mode>=5 && (mode-5)%2==1;
  for(int attempt=0;attempt<2;++attempt) {
   Object marker=new Object();Builder<Object,Object> original=new Builder<Object,Object>(marker);
   Builder<Object,Object> builder=mode==4?null:original;
   Throwable failure=error?new AssertionError("sentinel"):new IllegalStateException("sentinel");
   Trace.events=0;Trace.failStep=step;Trace.failOccurrence=occurrence;Trace.matching=0;
   Trace.started=0;Trace.completed=0;Trace.failure=failure;Trace.parent=null;Trace.partialElement=null;Trace.first=null;
   Object output=null;Throwable caught=null;
   try {
    if(route==0) output=ReferenceArrayCaller.make(builder,count);
    else if(route==1) output=Child.class.getConstructor(Builder.class,int.class).newInstance(builder,count);
    else if(route==2) output=MethodHandles.publicLookup().findConstructor(Child.class,
      MethodType.methodType(void.class,Builder.class,int.class)).invoke(builder,count);
    else output=new Child<Object,Object>(builder,count);
   } catch(InvocationTargetException e) {caught=e.getCause();} catch(Throwable e) {caught=e;}
   long expected=attempt==0?7:0;int started=0,completed=0,last=0,matches=0;
   boolean stopped=count<0,initFailure=step==8;
   if(!stopped && count>0) {
    if(attempt==0) {expected=expected*10+9;expected=expected*10+8;}
    if(initFailure) stopped=true;
    for(int i=0;i<count && !stopped;++i) {
     ++started;expected=expected*10+1;
     if(step==1 && ++matches==occurrence) {stopped=true;break;}
     expected=expected*10+2;
     if(step==2 && ++matches==occurrence) {stopped=true;break;}
     ++completed;
    }
   }
   if(!stopped) {
    for(int s=3;s<=6;++s) {
     if(s==5 && builder==null) break;
     expected=expected*10+s;last=s;
     if(s==step) break;
    }
   }
   require(Trace.events==expected && Trace.started==started && Trace.completed==completed);
   if(initFailure) {
    if(attempt==1) require(caught instanceof NoClassDefFoundError);
    else if(error) require(caught==failure);
    else require(caught instanceof ExceptionInInitializerError && caught.getCause()==failure);
   } else if(count<0) require(caught instanceof NegativeArraySizeException);
   else if(mode==4) require(caught instanceof NullPointerException);
   else require(caught==(step==0?null:failure));
   require((Trace.partialElement!=null)==(started!=0));
   if(started!=0) require(Trace.partialElement.serial==started && Trace.partialElement.getClass()==Element.class);
   require((Trace.parent!=null)==(last>=3));
   if(Trace.parent!=null) {
    require(Trace.parent.builder==builder && Trace.parent.path!=null && Trace.parent.path.length==count);
    require(Trace.parent.path.getClass().getComponentType()==Path.class);
    Path<?,?,?>[] path=Trace.parent.path;
    require((Trace.first!=null)==(count!=0 && last>=4));
    if(count!=0) {
     Path<?,?,?> first=last>=4?Trace.first:path[0];
     require(first!=null && first.serial==1 && first.getClass()==Element.class);
     require(path[0]==(last>=4?null:first));
     for(int i=1;i<count;++i) require(path[i]!=null && path[i].serial==i+1 && path[i].getClass()==Element.class && path[i]!=first);
    }
    Child<?,?> partial=(Child<?,?>)Trace.parent;
    require(partial.marker==(last>=5?marker:null) && partial.done==(last>=6?count:0));
   }
   require(output==(caught==null?Trace.parent:null));
   System.out.println("route="+route+",mode="+mode+",attempt="+attempt+",events="+expected);
  }
 }
}
