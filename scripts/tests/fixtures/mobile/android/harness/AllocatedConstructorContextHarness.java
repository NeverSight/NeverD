import fixture.*;
import java.util.*;
import java.lang.reflect.*;
import java.lang.invoke.*;
public final class AllocatedConstructorContextHarness {
    static void require(boolean ok) { if(!ok)throw new AssertionError("owned allocated constructor context"); }
    static final class Trap implements Comparator<String> {
        public int compare(String left,String right) { throw new AssertionError("owned allocated constructor context eager compare"); }
    }
    static Target<?> call(int route,int kind,List<Comparator<String>> values,Context context) throws Throwable {
        if(route==0)return kind==0?new Target<String>():new Target<String>(values,context);
        if(route==1)return kind==0?AllocatedConstructorContextCaller.fresh():AllocatedConstructorContextCaller.given(values,context);
        if(route==2) {
            try { return kind==0?(Target<?>)Target.class.getConstructor().newInstance():(Target<?>)Target.class.getConstructor(List.class,Context.class).newInstance(values,context); }
            catch(InvocationTargetException error) { throw error.getCause(); }
        }
        if(kind==0)return (Target)MethodHandles.publicLookup().findConstructor(Target.class,MethodType.methodType(void.class)).invokeExact();
        return (Target)MethodHandles.publicLookup().findConstructor(Target.class,MethodType.methodType(void.class,List.class,Context.class)).invokeExact((List)values,context);
    }
    @SuppressWarnings({"unchecked","rawtypes"})
    public static void main(String[] args) throws Throwable {
        final int route=Integer.parseInt(args[0]),kind=Integer.parseInt(args[1]),mode=Integer.parseInt(args[2]);
        final Object token=new Object(),changed=new Object();Trace.token=token;Trace.failAt=0;
        final Context suppliedContext=new Context();
        final List<Comparator<String>> suppliedValues=mode==5?null:new ArrayList<Comparator<String>>();
        final Object element=mode==6?null:mode==7?new Object():new Trap();
        final RuntimeException runtimeFailure=new RuntimeException("owned callback failure");
        final Error errorFailure=new Error("owned callback error");
        Trace.failure=new RuntimeException("owned constructor failure");
        Trace.failAt=mode==1?1:mode==2?2:mode==3?3:mode==8?4:0;
        Object previousBucket=null,previousContext=null,previousTarget=null;
        for(int attempt=0;attempt<2;attempt++) {
            final Object sentinel=new Object();Trace.events=0;Trace.bucket=Trace.context=Trace.target=sentinel;
            if(suppliedValues!=null)suppliedValues.clear();suppliedContext.value=null;
            final Object oldBucket=previousBucket,oldContext=previousContext,oldTarget=previousTarget;
            Trace.action=new Runnable() { public void run() {
                require(Trace.events==(kind==0?123:3)&&Trace.target instanceof Target&&Trace.target!=oldTarget);
                Target<?> target=(Target<?>)Trace.target;
                require(target.values==(kind==0?Trace.bucket:suppliedValues)&&target.context==(kind==0?Trace.context:suppliedContext));
                require(target.context.token==token&&target.context.value==null);
                if(kind==0)require(Trace.bucket instanceof Bucket&&Trace.bucket!=oldBucket&&Trace.context!=oldContext);
                Trace.hit(5);target.context.value=changed;
                if(target.values!=null)((List)target.values).add(element);
                if(mode==4)throw runtimeFailure;if(mode==9)throw errorFailure;
            }};
            Throwable failure=null;Target<?> result=null;Object actual=sentinel;
            try { result=call(route,kind,suppliedValues,suppliedContext);actual=(Comparator<?>)result.values.get(0); }
            catch(Throwable error) { failure=error; }
            boolean first=kind==0&&mode==1,second=kind==0&&mode==2,third=mode==3;
            long expected=first?1:second?12:third?(kind==0?123:3):mode==4||mode==9?(kind==0?1235:35):(kind==0?12354:354);
            require(Trace.events==expected);
            if(kind==0) {
                require(first?Trace.bucket==sentinel:Trace.bucket instanceof Bucket&&Trace.bucket!=previousBucket);
                require(first||second?Trace.context==sentinel:Trace.context instanceof Context&&Trace.context!=previousContext);
                if(!first)previousBucket=Trace.bucket;if(!first&&!second)previousContext=Trace.context;
            } else require(Trace.bucket==sentinel&&Trace.context==sentinel);
            Throwable wanted=first||second||third||mode==8?Trace.failure:mode==4?runtimeFailure:mode==9?errorFailure:null;
            if(wanted!=null)require(failure==wanted&&result==null);
            else if(kind==1&&mode==5)require(failure instanceof NullPointerException&&result==Trace.target);
            else if(mode==7)require(failure instanceof ClassCastException&&result==Trace.target);
            else require(failure==null&&result==Trace.target&&actual==element);
            if(first||second||third)require(Trace.target==sentinel);
            else {
                Target<?> partial=(Target<?>)Trace.target;require(partial!=previousTarget&&partial.context.value==changed);
                if(partial.values!=null)require(partial.values.size()==1&&((List)partial.values).get(0)==element);
                previousTarget=partial;
            }
            System.out.println("route="+route+",operation="+kind+",mode="+mode+",attempt="+attempt+",trace="+Trace.events);
        }
    }
}
