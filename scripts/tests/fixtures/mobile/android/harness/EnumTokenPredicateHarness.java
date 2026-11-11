package fixture;
import java.io.IOException;
import java.lang.invoke.MethodHandles;
import java.lang.invoke.MethodType;
import java.lang.reflect.InvocationTargetException;

public final class EnumTokenPredicateHarness {
    static int initializations;
    public enum Cold { ONE; static { initializations++; } }
    private static void require(boolean ok) {
        if(!ok) throw new AssertionError("owned enum token predicate mismatch");
    }
    @SuppressWarnings("unchecked") private static <X extends Throwable> void fail(Throwable failure) throws X { throw (X)failure; }
    private static final class State {
        final int mode;
        final Class<?> type;
        final String key;
        final boolean result;
        final Throwable failure;
        int actions;
        State(int mode,int attempt) {
            this.mode=mode;result=attempt!=0;
            type=mode==1?null:mode==2?String.class:mode==11?int.class:mode==12?void.class:mode==13?Object[].class:Cold.class;
            key=mode==3?null:mode==14?new String(""):new String("owned query "+attempt);
            failure=mode==4?new IllegalStateException("owned runtime"):mode==5?new AssertionError("owned error"):new IOException("owned checked");
        }
    }
    private static boolean call(int route,int operation,Object type,Object key) throws Throwable {
        if(route==0) {
            if(operation==0) return Core.call((Class)type,(String)key);
            if(operation==1) return Core.checked(type,key);
            return Core.negate((Class)type,(String)key);
        }
        if(route==1) {
            if(operation==0) return EnumTokenPredicateCaller.call((Class)type,(String)key);
            if(operation==1) return EnumTokenPredicateCaller.checked(type,key);
            return EnumTokenPredicateCaller.negate((Class)type,(String)key);
        }
        if(route==2) {
            try {
                if(operation==0) return ((Boolean)Core.class.getMethod("call",Class.class,String.class).invoke(null,type,key)).booleanValue();
                if(operation==1) return ((Boolean)Core.class.getMethod("checked",Object.class,Object.class).invoke(null,type,key)).booleanValue();
                return ((Boolean)Core.class.getMethod("negate",Class.class,String.class).invoke(null,type,key)).booleanValue();
            } catch(InvocationTargetException failure) { throw failure.getCause(); }
        }
        if(operation==1) return (boolean)MethodHandles.publicLookup().findStatic(Core.class,"checked",
            MethodType.methodType(boolean.class,Object.class,Object.class)).invokeExact(type,key);
        return (boolean)MethodHandles.publicLookup().findStatic(Core.class,operation==0?"call":"negate",
            MethodType.methodType(boolean.class,Class.class,String.class)).invokeExact((Class)type,(String)key);
    }
    public static void main(String[] args) throws Throwable {
        final int route=Integer.parseInt(args[0]),operation=Integer.parseInt(args[1]),mode=Integer.parseInt(args[2]);
        for(int attempt=0;attempt<2;++attempt) {
            final State state=new State(mode,attempt);
            final Object type=mode==7 && operation==1?new Object():state.type;
            final Object key=mode==8 && operation==1?new Object():state.key;
            final String sentinel=new String("owned untouched");
            Trace.events=0;Trace.type=Object.class;Trace.key=sentinel;Trace.result=!state.result;
            Trace.action=new Runnable() { public void run() {
                require(state.actions++==0 && Trace.events==1 && Trace.type==state.type && Trace.key==state.key && Trace.result!=state.result);
                Trace.result=state.result;
                if(mode>=4 && mode<=6) EnumTokenPredicateHarness.<RuntimeException>fail(state.failure);
                if(mode==9 || mode==15) Trace.type=String.class;
                if(mode==10 || mode==15) Trace.key=sentinel;
            } };
            boolean result=operation==2?state.result:!state.result;Throwable caught=null;
            try { result=call(route,operation,type,key); } catch(Throwable failure) { caught=failure; }
            final boolean badCast=operation==1 && (mode==7 || mode==8), actionFailure=mode>=4 && mode<=6;
            require(state.actions==(badCast?0:1) && Trace.events==(badCast?0:actionFailure?1:12));
            require(Trace.result==(badCast?!state.result:state.result));
            require(Trace.type==(badCast?Object.class:mode==9 || mode==15?String.class:state.type));
            require(Trace.key==(badCast || mode==10 || mode==15?sentinel:state.key));
            if(badCast) require(caught!=null && caught.getClass()==ClassCastException.class);
            else if(actionFailure) require(caught==state.failure);
            else require(caught==null && result==(operation==2?!state.result:state.result));
            require(initializations==0);
        }
        System.out.println("attempts=2,enum-token-predicate=matched");
    }
}
