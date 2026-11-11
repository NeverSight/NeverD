package fixture;
import java.io.IOException;
import java.lang.invoke.MethodHandles;
import java.lang.invoke.MethodType;
import java.lang.reflect.InvocationTargetException;

public final class EnumCarrierKeyHarness {
    public enum Color { RED, BLUE }
    public enum Other { VALUE }
    private static void require(boolean ok) {
        if(!ok) throw new AssertionError("owned enum carrier key mismatch");
    }
    @SuppressWarnings("unchecked") private static <X extends Throwable> void fail(Throwable failure) throws X { throw (X)failure; }
    private static final class State {
        final int mode,operation;
        final Object sentinel=new Object(), observed=new Object();
        final Object key,input,fallback,result,initialResult;
        final Throwable failure;
        int actions,probes;
        State(int operation,int mode,int attempt) {
            this.operation=operation;this.mode=mode;
            key=mode==2?null:new Object() {
                public int hashCode() { throw new AssertionError("owned unexpected hashing"); }
                public boolean equals(Object other) { throw new AssertionError("owned unexpected equality"); }
                public String toString() { throw new AssertionError("owned unexpected rendering"); }
            };
            input=mode==1?null:new ProbePair(this);
            fallback=operation==0?null:mode==10?Other.VALUE:attempt==0?Color.BLUE:Color.RED;
            result=mode==3?null:mode==7?new Object():mode==9?Other.VALUE:attempt==0?Color.RED:Color.BLUE;
            initialResult=mode==13?fallback:result;
            failure=mode==4?new IllegalStateException("owned runtime"):mode==5?new AssertionError("owned error"):new IOException("owned checked");
        }
    }
    private static final class ProbePair implements Pair<Color,Object> {
        final State state;
        ProbePair(State state) { this.state=state; }
        public Object probe(Object key) {
            require(state.probes++==0 && key==state.key && this==state.input && Trace.events==12 && Trace.result==state.initialResult);
            if(state.mode==11) EnumCarrierKeyHarness.<RuntimeException>fail(state.failure);
            if(state.mode==13) Trace.result=state.result;
            return state.observed;
        }
    }
    private static Object call(int route,int operation,Object input,Object key,Object fallback) throws Throwable {
        if(route==0) {
            if(operation==0) return Core.call((Pair)input,key);
            if(operation==1) return Core.fallback((Pair)input,key,(Enum)fallback);
            return Core.checked(input,key,fallback);
        }
        if(route==1) {
            if(operation==0) return EnumCarrierKeyCaller.call((Pair)input,key);
            if(operation==1) return EnumCarrierKeyCaller.fallback((Pair)input,key,(Enum)fallback);
            return EnumCarrierKeyCaller.checked(input,key,fallback);
        }
        if(route==2) {
            try {
                if(operation==0) return Core.class.getMethod("call",Pair.class,Object.class).invoke(null,input,key);
                if(operation==1) return Core.class.getMethod("fallback",Pair.class,Object.class,Enum.class).invoke(null,input,key,fallback);
                return Core.class.getMethod("checked",Object.class,Object.class,Object.class).invoke(null,input,key,fallback);
            } catch(InvocationTargetException failure) { throw failure.getCause(); }
        }
        if(operation==0) return (Enum)MethodHandles.publicLookup().findStatic(Core.class,"call",
            MethodType.methodType(Enum.class,Pair.class,Object.class)).invokeExact((Pair)input,key);
        if(operation==1) return (Enum)MethodHandles.publicLookup().findStatic(Core.class,"fallback",
            MethodType.methodType(Enum.class,Pair.class,Object.class,Enum.class)).invokeExact((Pair)input,key,(Enum)fallback);
        return (Enum)MethodHandles.publicLookup().findStatic(Core.class,"checked",
            MethodType.methodType(Enum.class,Object.class,Object.class,Object.class)).invokeExact(input,key,fallback);
    }
    public static void main(String[] args) throws Throwable {
        final int route=Integer.parseInt(args[0]),operation=Integer.parseInt(args[1]),mode=Integer.parseInt(args[2]);
        for(int attempt=0;attempt<2;++attempt) {
            final State state=new State(operation,mode,attempt);
            final Object input=mode==8 && operation==2?new Object():state.input;
            final Object fallback=mode==15 && operation==2?new Object():state.fallback;
            Trace.events=0;Trace.input=state.sentinel;Trace.key=state.sentinel;Trace.fallback=state.sentinel;
            Trace.result=state.sentinel;Trace.observed=state.sentinel;
            Trace.action=new Runnable() { public void run() {
                require(state.actions++==0 && Trace.events==1 && Trace.input==state.input && Trace.key==state.key && Trace.fallback==state.fallback && Trace.result==state.sentinel);
                Trace.result=state.initialResult;
                if(mode>=4 && mode<=6) EnumCarrierKeyHarness.<RuntimeException>fail(state.failure);
                if(mode==14) { Trace.input=state.sentinel;Trace.key=state.sentinel; }
            } };
            Object result=state.sentinel;Throwable caught=null;
            try { result=call(route,operation,input,state.key,fallback); } catch(Throwable failure) { caught=failure; }
            final boolean badCast=operation==2 && (mode==8 || mode==15), actionFailure=mode>=4 && mode<=6;
            final boolean probed=!badCast && !actionFailure && mode!=1;
            require(state.actions==(badCast?0:1) && state.probes==(probed?1:0));
            require(Trace.events==(badCast?0:actionFailure?1:mode==11?12:123));
            require(Trace.observed==(probed && mode!=11?state.observed:state.sentinel));
            require(Trace.result==(badCast?state.sentinel:state.result));
            require(Trace.input==(badCast || mode==14?state.sentinel:state.input) && Trace.key==(badCast || mode==14?state.sentinel:state.key));
            require(Trace.fallback==(badCast?state.sentinel:state.fallback));
            if(badCast || mode==7) require(caught!=null && caught.getClass()==ClassCastException.class && result==state.sentinel);
            else if(actionFailure || mode==11) require(caught==state.failure && result==state.sentinel);
            else require(caught==null && result==state.result);
            if(caught==null) {
                Throwable later=null;Color value=null;
                try { value=(Color)result;value.name(); } catch(Throwable failure) { later=failure; }
                if(mode==3) require(later!=null && later.getClass()==NullPointerException.class);
                else if(mode==9) require(later!=null && later.getClass()==ClassCastException.class);
                else require(later==null && value==state.result);
            }
        }
        System.out.println("attempts=2,enum-carrier-key=matched");
    }
}
