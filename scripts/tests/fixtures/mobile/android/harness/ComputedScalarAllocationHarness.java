import fixture.*;
import java.util.*;
import java.lang.reflect.*;
import java.lang.invoke.*;
public final class ComputedScalarAllocationHarness {
    static void require(boolean ok) { if(!ok)throw new AssertionError("owned computed scalar allocation"); }
    static final class Trap implements Comparator<String> {
        public int compare(String left,String right) { throw new AssertionError("owned computed scalar allocation eager compare"); }
    }
    static final class ObservedList extends ArrayList<Comparator<String>> {
        public int size() { require(Trace.ready);Trace.readAction.run();return Trace.count; }
    }
    static Target<?> call(int route,int kind,List<Comparator<String>> values,Context context) throws Throwable {
        if(route==0)return kind==0?new Target<String>(values):new Target<String>(values,context);
        if(route==1)return kind==0?ComputedScalarAllocationCaller.computed(values):ComputedScalarAllocationCaller.given(values,context);
        if(route==2) {
            try { return kind==0?(Target<?>)Target.class.getConstructor(List.class).newInstance(values):(Target<?>)Target.class.getConstructor(List.class,Context.class).newInstance(values,context); }
            catch(InvocationTargetException error) { throw error.getCause(); }
        }
        if(kind==0)return (Target)MethodHandles.publicLookup().findConstructor(Target.class,MethodType.methodType(void.class,List.class)).invokeExact((List)values);
        return (Target)MethodHandles.publicLookup().findConstructor(Target.class,MethodType.methodType(void.class,List.class,Context.class)).invokeExact((List)values,context);
    }
    @SuppressWarnings({"unchecked","rawtypes"})
    public static void main(String[] args) throws Throwable {
        final int route=Integer.parseInt(args[0]),kind=Integer.parseInt(args[1]),mode=Integer.parseInt(args[2]);
        final Object token=new Object(),afterRead=new Object(),changed=new Object();Trace.token=token;Trace.failAt=0;
        final Context suppliedContext=kind==1?new Context(33):null;
        final List<Comparator<String>> values=mode==5?null:new ObservedList();
        final Comparator<String> initial=new Trap(),intermediate=new Trap();
        final Object element=mode==6?null:mode==7?new Object():mode==11?initial:new Trap();
        final int expectedCount=mode==11?Integer.MAX_VALUE:Integer.MIN_VALUE+37+mode;
        final RuntimeException getterFailure=new RuntimeException("owned getter failure"),runtimeFailure=new RuntimeException("owned callback failure");
        final Error getterError=new Error("owned getter error"),errorFailure=new Error("owned callback error");
        Trace.failure=new RuntimeException("owned constructor failure");
        Trace.failAt=mode==2?2:mode==3?3:mode==8?4:0;
        Object previousContext=null,previousTarget=null;
        for(int attempt=0;attempt<2;attempt++) {
            final Object sentinel=new Object();Trace.events=0;Trace.context=Trace.target=sentinel;Trace.token=token;Trace.count=123;
            if(values!=null) {values.clear();values.add(initial);}if(suppliedContext!=null)suppliedContext.value=null;
            final boolean readyBefore=Trace.ready;final Object oldContext=previousContext,oldTarget=previousTarget;
            Trace.readAction=new Runnable() { public void run() {
                Trace.hit(6);Trace.token=afterRead;Trace.count=expectedCount;
                values.clear();values.add(intermediate);
                if(mode==1)throw getterFailure;if(mode==10)throw getterError;
            }};
            Trace.action=new Runnable() { public void run() {
                require(Trace.target instanceof Target&&Trace.target!=oldTarget);
                Target<?> target=(Target<?>)Trace.target;
                require((Object)target.values==values&&target.context==(kind==0?Trace.context:suppliedContext));
                require(target.context.token==(kind==0?afterRead:token)&&target.context.snapshot==(kind==0?expectedCount:33)&&target.context.value==null);
                if(kind==0)require(Trace.context!=oldContext);
                if(values!=null)require(values.toArray().length==1&&values.get(0)==(kind==0?intermediate:initial));
                Trace.hit(5);target.context.value=changed;Trace.count=999;
                if(values!=null) {values.clear();((List)values).add(element);}
                if(mode==4)throw runtimeFailure;if(mode==9)throw errorFailure;
            }};
            Throwable failure=null;Target<?> result=null;Object actual=sentinel;
            try { result=call(route,kind,values,suppliedContext);actual=(Comparator<?>)result.values.get(0); }
            catch(Throwable error) { failure=error; }
            boolean nullGetter=kind==0&&mode==5,getterFailed=kind==0&&(mode==1||mode==10),contextFailed=kind==0&&mode==2;
            boolean beforeTarget=nullGetter||getterFailed||contextFailed,third=mode==3;
            long expected=kind==0&&!readyBefore?7:0;
            if(kind==0&&!nullGetter) {expected=expected*10+6;if(!getterFailed)expected=expected*10+2;}
            if(!beforeTarget) {expected=expected*10+3;if(!third) {expected=expected*10+5;if(mode!=4&&mode!=9)expected=expected*10+4;}}
            require(Trace.events==expected&&Trace.ready);
            Throwable wanted=getterFailed?(mode==1?getterFailure:getterError):contextFailed||third||mode==8?Trace.failure:mode==4?runtimeFailure:mode==9?errorFailure:null;
            if(nullGetter)require(failure instanceof NullPointerException&&result==null);
            else if(wanted!=null)require(failure==wanted&&result==null);
            else if(mode==5)require(failure instanceof NullPointerException&&result==Trace.target);
            else if(mode==7)require(failure instanceof ClassCastException&&result==Trace.target);
            else require(failure==null&&result==Trace.target&&actual==element);
            if(kind==0) {
                if(beforeTarget)require(Trace.context==sentinel);
                else {Context partial=(Context)Trace.context;require(partial!=previousContext&&partial.token==afterRead&&partial.snapshot==expectedCount);previousContext=partial;}
            } else require(Trace.context==sentinel);
            if(beforeTarget||third)require(Trace.target==sentinel);
            else {Target<?> partial=(Target<?>)Trace.target;require(partial!=previousTarget&&partial.context.value==changed);previousTarget=partial;}
            if(values!=null)require(values.toArray().length==1&&((List)values).get(0)==(!beforeTarget&&!third?element:kind==0?intermediate:initial));
            System.out.println("route="+route+",operation="+kind+",mode="+mode+",attempt="+attempt+",trace="+Trace.events);
        }
    }
}
