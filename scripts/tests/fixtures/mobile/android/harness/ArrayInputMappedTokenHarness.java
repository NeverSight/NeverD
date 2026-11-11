package fixture;
import java.io.IOException;
import java.lang.invoke.MethodHandles;
import java.lang.invoke.MethodType;
import java.lang.reflect.InvocationTargetException;
import java.util.function.Function;

public final class ArrayInputMappedTokenHarness {
    private static void require(boolean ok) {
        if(!ok) throw new AssertionError("owned array input mapped token mismatch");
    }
    @SuppressWarnings("unchecked") private static <X extends Throwable> void fail(Throwable failure) throws X { throw (X)failure; }
    private static final class State {
        final int mode;
        final Object sentinel=new Object(), mapped;
        final String before=new String("owned before"), after=new String("owned after");
        final Object[] source;
        final Class<?> type;
        final Object result;
        final Function mapper;
        final Throwable failure;
        final boolean apply;
        int actions,maps;
        State(int mode,int attempt) {
            this.mode=mode;apply=mode!=1 && mode!=6;
            source=mode==1 || mode==2?null:mode==3?new String[0]:new String[]{before};
            type=mode==4?null:mode==5?int.class:String.class;
            result=mode==10?null:mode==11?new Object():mode==12?new Object[]{new String("owned broad")}:new String[]{new String("owned result "+attempt)};
            mapped=mode==14?Integer.valueOf(41+attempt):new String("owned mapped "+attempt);
            failure=mode==8?new IllegalStateException("owned action"):attempt==0?new AssertionError("owned mapper error"):new IOException("owned mapper checked");
            mapper=mode==6 || mode==7?null:new Function<Object,Object>() { public Object apply(Object value) {
                require(maps++==0 && value==(State.this.mode==13?after:before) && Trace.events==12 && Trace.result==result);
                if(State.this.mode==9) ArrayInputMappedTokenHarness.<RuntimeException>fail(failure);
                return mapped;
            } };
        }
    }
    private static Object[] call(int route,int operation,Object source,Object type,Object mapper) throws Throwable {
        if(route==0) {
            if(operation==0) return Core.call((Object[])source,(Class)type,(Function)mapper);
            if(operation==1) return Core.checked(source,type,mapper);
            return Core.narrow((Object[])source,(Class)type,(Function)mapper);
        }
        if(route==1) {
            if(operation==0) return ArrayInputMappedTokenCaller.call((Object[])source,(Class)type,(Function)mapper);
            if(operation==1) return ArrayInputMappedTokenCaller.checked(source,type,mapper);
            return ArrayInputMappedTokenCaller.narrow((Object[])source,(Class)type,(Function)mapper);
        }
        if(route==2) {
            try {
                if(operation==1) return (Object[])Core.class.getMethod("checked",Object.class,Object.class,Object.class).invoke(null,source,type,mapper);
                return (Object[])Core.class.getMethod(operation==0?"call":"narrow",Object[].class,Class.class,Function.class).invoke(null,source,type,mapper);
            } catch(InvocationTargetException failure) { throw failure.getCause(); }
        }
        if(operation==0) return (Object[])MethodHandles.publicLookup().findStatic(Core.class,"call",
            MethodType.methodType(Object[].class,Object[].class,Class.class,Function.class)).invokeExact((Object[])source,(Class)type,(Function)mapper);
        if(operation==1) return (Object[])MethodHandles.publicLookup().findStatic(Core.class,"checked",
            MethodType.methodType(Object[].class,Object.class,Object.class,Object.class)).invokeExact(source,type,mapper);
        return (String[])MethodHandles.publicLookup().findStatic(Core.class,"narrow",
            MethodType.methodType(String[].class,Object[].class,Class.class,Function.class)).invokeExact((Object[])source,(Class)type,(Function)mapper);
    }
    public static void main(String[] args) throws Throwable {
        final int route=Integer.parseInt(args[0]),operation=Integer.parseInt(args[1]),mode=Integer.parseInt(args[2]);
        for(int attempt=0;attempt<2;++attempt) {
            final State state=new State(mode,attempt);
            final Object source=mode==15 && operation==1?new Object():state.source;
            Trace.events=0;Trace.source=state.sentinel;Trace.type=Object.class;Trace.mapper=state.sentinel;
            Trace.result=state.sentinel;Trace.mapped=state.sentinel;Trace.apply=!state.apply;Trace.index=7;
            Trace.action=new Runnable() { public void run() {
                require(state.actions++==0 && Trace.events==1 && Trace.source==state.source && Trace.type==state.type && Trace.mapper==state.mapper && Trace.result==state.sentinel);
                Trace.result=state.result;Trace.apply=state.apply;Trace.index=0;
                if(mode==8) ArrayInputMappedTokenHarness.<RuntimeException>fail(state.failure);
                if(mode==13) { state.source[0]=state.after;Trace.source=state.sentinel; }
            } };
            Object result=state.sentinel;Throwable caught=null;
            try { result=call(route,operation,source,state.type,state.mapper); } catch(Throwable failure) { caught=failure; }
            final boolean badCast=operation==1 && mode==15, actionFailure=mode==8;
            final boolean mapStarted=!badCast && !actionFailure && mode!=1 && mode!=2 && mode!=3 && mode!=6 && mode!=7;
            final boolean failedAfterAction=mode==2 || mode==3 || mode==7 || mode==9;
            require(state.actions==(badCast?0:1) && state.maps==(mapStarted?1:0));
            require(Trace.events==(badCast?0:actionFailure?1:failedAfterAction?12:123));
            require(Trace.result==(badCast?state.sentinel:state.result));
            require(Trace.source==(badCast || mode==13?state.sentinel:state.source) && Trace.type==(badCast?Object.class:state.type));
            require(Trace.mapper==(badCast?state.sentinel:state.mapper));
            require(Trace.mapped==(mapStarted && mode!=9?state.mapped:state.sentinel));
            if(badCast || mode==11 || operation==2 && mode==12) require(caught!=null && caught.getClass()==ClassCastException.class && result==state.sentinel);
            else if(actionFailure || mode==9) require(caught==state.failure && result==state.sentinel);
            else if(mode==2 || mode==7) require(caught!=null && caught.getClass()==NullPointerException.class && result==state.sentinel);
            else if(mode==3) require(caught!=null && caught.getClass()==ArrayIndexOutOfBoundsException.class && result==state.sentinel);
            else require(caught==null && result==state.result);
            if(caught==null) {
                Throwable later=null;String value=null;
                try { String[] strings=(String[])result;value=strings[0]; } catch(Throwable failure) { later=failure; }
                if(mode==10) require(later!=null && later.getClass()==NullPointerException.class);
                else if(mode==12) require(later!=null && later.getClass()==ClassCastException.class);
                else {
                    require(later==null && value==((Object[])state.result)[0]);
                    Throwable store=null;
                    try { ((Object[])result)[0]=Integer.valueOf(31); } catch(Throwable failure) { store=failure; }
                    require(store!=null && store.getClass()==ArrayStoreException.class && ((Object[])result)[0]==value);
                }
            }
        }
        System.out.println("attempts=2,array-input-mapped-token=matched");
    }
}
