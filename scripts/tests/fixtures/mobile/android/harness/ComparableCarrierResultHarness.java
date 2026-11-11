package fixture;
import java.io.IOException;
import java.lang.invoke.MethodHandles;
import java.lang.invoke.MethodType;
import java.lang.reflect.InvocationTargetException;
import java.util.AbstractCollection;
import java.util.ArrayList;
import java.util.Collection;
import java.util.Collections;
import java.util.Iterator;

public final class ComparableCarrierResultHarness {
    private static void require(boolean ok) {
        if(!ok) throw new AssertionError("owned comparable carrier result mismatch");
    }
    @SuppressWarnings("unchecked") private static <X extends Throwable> void fail(Throwable failure) throws X { throw (X)failure; }
    private static class Card implements Comparable<Object> {
        final State state;
        Card(State state) { this.state=state; }
        public int compareTo(Object value) {
            require(++state.comparisons==1 && value==state.argument && Trace.events==12 && Trace.result==state.result);
            if(state.mode==14) ComparableCarrierResultHarness.<RuntimeException>fail(state.failure);
            return state.number;
        }
    }
    private static final class Alternate extends Card { Alternate(State state) { super(state); } }
    private static final class Probe extends AbstractCollection<Object> {
        final State state;
        final Object[] values;
        Probe(State state) { this.state=state;values=state.mode==2?new Object[0]:new Object[]{state.mode==3?null:state.sentinel}; }
        public Iterator<Object> iterator() { state.probes++;throw new AssertionError("owned collection traversal"); }
        public int size() { state.probes++;throw new AssertionError("owned collection probe"); }
    }
    private static final class State {
        final int mode;
        final Object sentinel=new Object(), changed=new Object();
        final Collection<Object> input;
        final Object result;
        final Throwable failure;
        final String argument=new String("owned token");
        final int number;
        int actions,probes,comparisons;
        State(int mode,int operation,int attempt) {
            this.mode=mode;number=attempt==0?Integer.MIN_VALUE:Integer.MAX_VALUE;
            Collection<Object> probe=new Probe(this);
            input=mode==1?null:mode==5?Collections.unmodifiableCollection(probe):probe;
            result=mode==9?null:mode==10?new Object():mode==11?(operation==2?new Card(this):new String("owned token")):
                operation==2?new String("owned token"):mode==12?new Alternate(this):new Card(this);
            failure=mode==6?new IllegalStateException("owned runtime"):mode==7?new AssertionError("owned error"):new IOException("owned checked");
        }
    }
    private static Object call(int route,int operation,Object values) throws Throwable {
        if(route==0) {
            if(operation==0) return Core.call((Collection)values);
            if(operation==1) return Core.checked(values);
            return Core.narrow((Collection)values);
        }
        if(route==1) {
            if(operation==0) return ComparableCarrierResultCaller.call((Collection)values);
            if(operation==1) return ComparableCarrierResultCaller.checked(values);
            return ComparableCarrierResultCaller.narrow((Collection)values);
        }
        if(route==2) {
            try { return Core.class.getMethod(operation==0?"call":operation==1?"checked":"narrow",operation==1?Object.class:Collection.class).invoke(null,values); }
            catch(InvocationTargetException failure) { throw failure.getCause(); }
        }
        if(operation==1) return (Comparable)MethodHandles.publicLookup().findStatic(Core.class,"checked",MethodType.methodType(Comparable.class,Object.class)).invokeExact(values);
        if(operation==2) return (String)MethodHandles.publicLookup().findStatic(Core.class,"narrow",MethodType.methodType(String.class,Collection.class)).invokeExact((Collection)values);
        return (Comparable)MethodHandles.publicLookup().findStatic(Core.class,"call",MethodType.methodType(Comparable.class,Collection.class)).invokeExact((Collection)values);
    }
    public static void main(String[] args) throws Throwable {
        final int route=Integer.parseInt(args[0]),operation=Integer.parseInt(args[1]),mode=Integer.parseInt(args[2]);
        for(int attempt=0;attempt<2;++attempt) {
            final State state=new State(mode,operation,attempt);
            final Object values=mode==4 && operation==1?new Object():state.input;
            final Collection<Object> sentinelInput=new ArrayList<Object>();
            Trace.events=0;Trace.input=sentinelInput;Trace.result=state.sentinel;
            Trace.action=new Runnable() { public void run() {
                require(state.actions++==0 && Trace.events==1 && Trace.input==state.input && Trace.result==state.sentinel && state.probes==0 && state.comparisons==0);
                Trace.result=state.result;
                if(mode>=6 && mode<=8) ComparableCarrierResultHarness.<RuntimeException>fail(state.failure);
                if(mode==13) { ((Probe)state.input).values[0]=state.changed;Trace.input=sentinelInput; }
                if(mode==15) Trace.input=null;
            } };
            Object result=state.sentinel;Throwable caught=null;
            try { result=call(route,operation,values); } catch(Throwable failure) { caught=failure; }
            final boolean badCast=operation==1 && mode==4, actionFailure=mode>=6 && mode<=8;
            require(state.actions==(badCast?0:1) && Trace.events==(badCast?0:actionFailure?1:12) && state.probes==0 && state.comparisons==0);
            require(Trace.result==(badCast?state.sentinel:state.result));
            require(Trace.input==(badCast || mode==13?sentinelInput:mode==15?null:state.input));
            if(mode==13) require(((Probe)state.input).values[0]==state.changed);
            if(badCast || mode==10 || (mode==11 && operation==2)) require(caught!=null && caught.getClass()==ClassCastException.class && result==state.sentinel);
            else if(actionFailure) require(caught==state.failure && result==state.sentinel);
            else require(caught==null && result==state.result);
            if(caught==null) {
                Throwable later=null;int compared=41;
                try { compared=operation==2?((String)result).compareTo(state.argument):((Card)result).compareTo(state.argument); } catch(Throwable failure) { later=failure; }
                if(mode==9) require(later!=null && later.getClass()==NullPointerException.class && state.comparisons==0);
                else if(mode==11) require(later!=null && later.getClass()==ClassCastException.class && state.comparisons==0);
                else if(operation==2) require(later==null && compared==0 && state.comparisons==0);
                else if(mode==14) require(later==state.failure && state.comparisons==1);
                else require(later==null && compared==state.number && state.comparisons==1);
            }
        }
        System.out.println("attempts=2,comparable-carrier-result=matched");
    }
}
