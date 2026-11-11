package fixture;
import java.io.IOException;
import java.lang.invoke.MethodHandles;
import java.lang.invoke.MethodType;
import java.lang.reflect.InvocationTargetException;
import java.util.HashMap;
import java.util.LinkedHashMap;

public final class EnumTokenMapHarness {
    static int coldInitializations;
    public enum Cold { ONE; static { coldInitializations++; } }
    public enum Color { RED, BLUE }
    public enum Other { VALUE }
    private static void require(boolean ok) {
        if (!ok) throw new AssertionError("owned enum token map mismatch");
    }
    @SuppressWarnings("unchecked") private static <X extends Throwable> void fail(Throwable failure) throws X { throw (X) failure; }
    private static final class State {
        final int mode, operation, attempt;
        final boolean present;
        final Object sentinel = new Object();
        final Object query = new Object() {
            public int hashCode() { throw new AssertionError("owned unexpected hashing"); }
            public String toString() { throw new AssertionError("owned unexpected rendering"); }
        };
        final Class<?> type;
        final Object result;
        final Throwable failure;
        int actions, queries, gets;
        Object entry = sentinel;
        State(int operation, int mode, int attempt) {
            this.operation=operation; this.mode=mode; this.attempt=attempt; present=attempt!=0;
            type=mode==1?null:mode==2?String.class:Cold.class;
            ProbeMap first=new ProbeMap(this), second=new ProbeMap(this);
            result=mode==3?null:mode==7?new HashMap():mode==15?new Object():mode==12?second:first;
            failure=mode==4?new IllegalStateException("owned runtime"):mode==5?new AssertionError("owned error"):new IOException("owned checked");
        }
    }
    private static final class ProbeMap extends LinkedHashMap<String,Object> {
        final State state;
        ProbeMap(State state) { this.state=state; }
        public boolean containsKey(Object key) {
            require(state.queries++==0 && key==state.query && this==state.result && Trace.events==12 && Trace.result==state.result);
            if(state.mode==11) EnumTokenMapHarness.<RuntimeException>fail(state.failure);
            return state.present;
        }
        public Object get(Object key) {
            require(state.gets++==0 && key==state.query && this==state.result && Trace.events==12 && Trace.result==state.result);
            if(state.mode==13) EnumTokenMapHarness.<RuntimeException>fail(state.failure);
            return state.entry;
        }
    }
    private static Object call(int route, int operation, Object type, Object key) throws Throwable {
        if(route==0) {
            if(operation==0) return Core.call((Class)type);
            if(operation==1) return Core.checked(type);
            return Boolean.valueOf(Core.contains((Class)type,key));
        }
        if(route==1) {
            if(operation==0) return EnumTokenMapCaller.call((Class)type);
            if(operation==1) return EnumTokenMapCaller.checked(type);
            return Boolean.valueOf(EnumTokenMapCaller.contains((Class)type,key));
        }
        if(route==2) {
            try {
                if(operation==0) return Core.class.getMethod("call",Class.class).invoke(null,type);
                if(operation==1) return Core.class.getMethod("checked",Object.class).invoke(null,type);
                return Core.class.getMethod("contains",Class.class,Object.class).invoke(null,type,key);
            } catch(InvocationTargetException failure) { throw failure.getCause(); }
        }
        if(operation==0) return (LinkedHashMap)MethodHandles.publicLookup().findStatic(Core.class,"call",
                MethodType.methodType(LinkedHashMap.class,Class.class)).invokeExact((Class)type);
        if(operation==1) return (LinkedHashMap)MethodHandles.publicLookup().findStatic(Core.class,"checked",
                MethodType.methodType(LinkedHashMap.class,Object.class)).invokeExact(type);
        return Boolean.valueOf((boolean)MethodHandles.publicLookup().findStatic(Core.class,"contains",
                MethodType.methodType(boolean.class,Class.class,Object.class)).invokeExact((Class)type,key));
    }
    public static void main(String[] args) throws Throwable {
        final int route=Integer.parseInt(args[0]), operation=Integer.parseInt(args[1]), mode=Integer.parseInt(args[2]);
        for(int attempt=0;attempt<2;++attempt) {
            final State state=new State(operation,mode,attempt);
            final Object input=mode==8 && operation==1?new Object():state.type;
            Trace.events=0;Trace.type=Object.class;Trace.result=state.sentinel;
            Trace.action=new Runnable() { public void run() {
                require(state.actions++==0 && Trace.events==1 && Trace.type==state.type && Trace.result==state.sentinel);
                Trace.result=state.result;
                state.entry=mode==9?null:mode==10?Other.VALUE:state.attempt==0?Color.RED:Color.BLUE;
                if(mode>=4 && mode<=6) EnumTokenMapHarness.<RuntimeException>fail(state.failure);
                if(mode==14) Trace.type=String.class;
            } };
            Object result=state.sentinel;Throwable caught=null;
            try { result=call(route,operation,input,state.query); } catch(Throwable failure) { caught=failure; }
            final boolean badCast=mode==8 && operation==1, actionFailure=mode>=4 && mode<=6, badResult=mode==7 || mode==15;
            final boolean queried=!badCast && !actionFailure && !badResult && operation==2 && mode!=3;
            require(state.actions==(badCast?0:1) && state.queries==(queried?1:0) && state.gets==0);
            require(Trace.events==(badCast?0:actionFailure?1:12));
            require(Trace.result==(badCast?state.sentinel:state.result) && Trace.type==(badCast?Object.class:mode==14?String.class:state.type));
            if(badCast || badResult) require(caught!=null && caught.getClass()==ClassCastException.class && result==state.sentinel);
            else if(actionFailure || operation==2 && mode==11) require(caught==state.failure && result==state.sentinel);
            else if(operation==2 && mode==3) require(caught!=null && caught.getClass()==NullPointerException.class && result==state.sentinel);
            else require(caught==null && result==(operation==2?Boolean.valueOf(state.present):state.result));
            if(caught==null && operation!=2) {
                Throwable consumerFailure=null;Color value=null;
                try {
                    LinkedHashMap<String,Color> map=(LinkedHashMap)result;
                    value=map.get(state.query);value.name();
                } catch(Throwable failure) { consumerFailure=failure; }
                require(state.gets==(mode==3?0:1));
                if(mode==3 || mode==9) require(consumerFailure!=null && consumerFailure.getClass()==NullPointerException.class);
                else if(mode==10) require(consumerFailure!=null && consumerFailure.getClass()==ClassCastException.class);
                else if(mode==13) require(consumerFailure==state.failure);
                else require(consumerFailure==null && value==state.entry);
            }
            require(coldInitializations==0);
        }
        System.out.println("attempts=2,enum-token-map=matched");
    }
}
