import fixture.*;
import java.io.IOException;
import java.lang.ref.Reference;
import java.lang.ref.WeakReference;
import java.util.concurrent.ConcurrentMap;
import java.util.concurrent.ConcurrentHashMap;
import java.lang.invoke.MethodHandles;
import java.lang.invoke.MethodType;
import java.lang.reflect.InvocationTargetException;
public final class ScopedStaticFieldSuperHarness {
    private static void require(boolean ok){if(!ok)throw new AssertionError("owned scoped static field super");}
    @SuppressWarnings("unchecked") private static <X extends Throwable> void fail(Throwable failure)throws X{throw (X)failure;}
    @SuppressWarnings({"unchecked","rawtypes"}) private static Core<String,String> call(int route,int operation,ConcurrentMap<Reference<String>,String> value)throws Throwable {
        if(route==0)return operation==0?new Core<String,String>(value):new Core<String,String>(value,7);
        if(route==1)return operation==0?ScopedStaticFieldSuperCaller.first(value):ScopedStaticFieldSuperCaller.second(value);
        if(route==2){try{return (Core)(operation==0?Core.class.getConstructor(ConcurrentMap.class).newInstance(value):Core.class.getConstructor(ConcurrentMap.class,int.class).newInstance(value,7));}catch(InvocationTargetException failure){throw failure.getCause();}}
        if(operation==0)return (Core)MethodHandles.publicLookup().findConstructor(Core.class,MethodType.methodType(void.class,ConcurrentMap.class)).invokeExact(value);
        return (Core)MethodHandles.publicLookup().findConstructor(Core.class,MethodType.methodType(void.class,ConcurrentMap.class,int.class)).invokeExact(value,7);
    }
    @SuppressWarnings({"unchecked","rawtypes"}) public static void main(String[] args)throws Throwable {
        final int route=Integer.parseInt(args[0]),operation=Integer.parseInt(args[1]),mode=Integer.parseInt(args[2]);
        final Failure runtime=new Failure("owned failure");final Error error=new Error("owned error");final IOException checked=new IOException("owned checked");Trace.failure=new Failure("owned stage");
        Trace.failAt=mode==6?1:mode==7?2:mode==8?3:mode==9?4:0;
        for(int attempt=0;attempt<2;attempt++) {
            final Reference<String> key=new WeakReference<String>("owned key");final Object sentinel=new Object(),payload=new Object();
            final ConcurrentMap<Reference<String>,String> value=mode==1?null:new ConcurrentHashMap<Reference<String>,String>();
            final ConcurrentMap<Reference<String>,String> other=new ConcurrentHashMap<Reference<String>,String>();
            final Marker initial=mode==2?null:Marker.SECOND,expected=operation==0?Marker.FIRST:initial;
            Trace.events=0;Trace.received=null;Trace.other=other;Trace.observed=null;Trace.selected=initial;Trace.payload=sentinel;Trace.parentObject=null;
            final int calls=Trace.parentCalls,completed=Trace.completed;
            Trace.action=new Runnable(){public void run(){require(Trace.events==1&&Trace.received==value&&Trace.observed==expected);Trace.hit(8);Trace.payload=payload;Trace.selected=expected==Marker.FIRST?Marker.SECOND:Marker.FIRST;Trace.other=new ConcurrentHashMap<Object,Object>();if(value!=null)((ConcurrentMap)value).put(key,mode==3?Integer.valueOf(91):"owned result");if(mode==4)throw runtime;if(mode==5)throw error;if(mode==10)ScopedStaticFieldSuperHarness.<RuntimeException>fail(checked);}};
            Core<String,String> result=null;String read=null;Throwable failure=null;
            try{result=call(route,operation,value);if(value!=null)read=result.original.get(key);}catch(Throwable caught){failure=caught;}
            boolean actionReached=mode!=6,actionReturned=actionReached&&mode!=4&&mode!=5&&mode!=10,stored=actionReturned&&mode!=7,returned=stored&&mode!=8&&mode!=9;
            long events=mode==6?1:!actionReturned?18:mode==7?182:mode==8?1823:18234;
            require(Trace.parentCalls==calls+1&&Trace.completed==completed+(returned?1:0)&&Trace.events==events&&Trace.received==value&&Trace.observed==expected);
            require(Trace.payload==(actionReached?payload:sentinel)&&Trace.selected==(actionReached?(expected==Marker.FIRST?Marker.SECOND:Marker.FIRST):initial));
            require((Trace.parentObject!=null)==stored&&(result!=null)==returned);
            if(stored){Parent<?,?> partial=(Parent<?,?>)Trace.parentObject;require((Object)partial.original==value&&partial.marker==expected);if(returned)require(result==partial&&result instanceof Tagged);}
            if(value!=null)require(value.containsKey(key)==actionReached);
            Throwable expectedFailure=mode==4?runtime:mode==5?error:mode>=6&&mode<=9?Trace.failure:mode==10?checked:null;
            if(expectedFailure!=null)require(failure==expectedFailure&&read==null);
            else if(mode==3)require(failure instanceof ClassCastException&&read==null);
            else require(failure==null&&(value==null?read==null:"owned result".equals(read)));
            System.out.println("route="+route+",operation="+operation+",mode="+mode+",attempt="+attempt+",trace="+Trace.events);
        }
    }
}
