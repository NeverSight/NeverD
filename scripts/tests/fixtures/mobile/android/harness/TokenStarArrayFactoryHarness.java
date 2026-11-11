package fixture;
import java.io.IOException;
import java.lang.invoke.MethodHandles;
import java.lang.invoke.MethodType;
import java.lang.reflect.Constructor;
import java.lang.reflect.InvocationTargetException;

public final class TokenStarArrayFactoryHarness {
    static int constructions;
    static Object observed;
    static int observedNumber;
    static Throwable constructorFailure;
    public static final class Widget {
        public Widget(String value,int number) { constructions++;observed=value;observedNumber=number; }
    }
    public static final class Other {
        public Other(String value,int number) { constructions++;observed=value;observedNumber=number; }
    }
    public static final class Broken {
        public Broken(String value,int number) throws Throwable { constructions++;observed=value;observedNumber=number;throw constructorFailure; }
    }
    private static void require(boolean ok) {
        if(!ok) throw new AssertionError("owned token star array factory mismatch");
    }
    @SuppressWarnings("unchecked") private static <X extends Throwable> void fail(Throwable failure) throws X { throw (X)failure; }
    private static final class State {
        final int mode;
        final Object sentinel=new Object();
        final Class<?> type;
        final Class<?>[] parts;
        final Object result;
        final Throwable failure;
        final String text=new String("owned argument");
        final int number;
        int actions;
        State(int mode,int attempt) throws Exception {
            this.mode=mode;number=attempt==0?Integer.MIN_VALUE:Integer.MAX_VALUE;
            type=mode==1?null:mode==5?int.class:Widget.class;
            parts=mode==2?null:mode==3?new Class<?>[0]:mode==4?new Class<?>[]{null,int.class}:new Class<?>[]{String.class,int.class};
            result=mode==9?null:mode==10?new Object():(mode==11?Other.class:mode==12?Broken.class:Widget.class).getConstructor(String.class,int.class);
            failure=mode==6?new IllegalStateException("owned runtime"):mode==7?new AssertionError("owned error"):new IOException("owned checked");
        }
    }
    private static Object call(int route,int operation,Object type,Object parts) throws Throwable {
        if(route==0) {
            if(operation==0) return Core.call((Class)type,(Class<?>[])parts);
            if(operation==1) return Core.checked(type,parts);
            return Core.ordinary((Class)type,(Class<?>[])parts);
        }
        if(route==1) {
            if(operation==0) return TokenStarArrayFactoryCaller.call((Class)type,(Class<?>[])parts);
            if(operation==1) return TokenStarArrayFactoryCaller.checked(type,parts);
            return TokenStarArrayFactoryCaller.ordinary((Class)type,(Class<?>[])parts);
        }
        if(route==2) {
            try {
                if(operation==1) return Core.class.getMethod("checked",Object.class,Object.class).invoke(null,type,parts);
                return Core.class.getMethod(operation==0?"call":"ordinary",Class.class,Class[].class).invoke(null,type,parts);
            } catch(InvocationTargetException failure) { throw failure.getCause(); }
        }
        if(operation==1) return (Constructor)MethodHandles.publicLookup().findStatic(Core.class,"checked",
            MethodType.methodType(Constructor.class,Object.class,Object.class)).invokeExact(type,parts);
        return (Constructor)MethodHandles.publicLookup().findStatic(Core.class,operation==0?"call":"ordinary",
            MethodType.methodType(Constructor.class,Class.class,Class[].class)).asFixedArity().invokeExact((Class)type,(Class[])parts);
    }
    public static void main(String[] args) throws Throwable {
        final int route=Integer.parseInt(args[0]),operation=Integer.parseInt(args[1]),mode=Integer.parseInt(args[2]);
        for(int attempt=0;attempt<2;++attempt) {
            final State state=new State(mode,attempt);
            final Object type=mode==4 && operation==1?new Object():state.type;
            final Object parts=mode==15 && operation==1?new Object[]{String.class}:state.parts;
            final Class<?>[] sentinelParts=new Class<?>[]{Object.class};
            constructions=0;observed=state.sentinel;observedNumber=0;constructorFailure=state.failure;
            Trace.events=0;Trace.type=Object.class;Trace.parts=sentinelParts;Trace.result=state.sentinel;
            Trace.action=new Runnable() { public void run() {
                require(state.actions++==0 && Trace.events==1 && Trace.type==state.type && Trace.parts==state.parts && Trace.result==state.sentinel && constructions==0);
                Trace.result=state.result;
                if(mode>=6 && mode<=8) TokenStarArrayFactoryHarness.<RuntimeException>fail(state.failure);
                if(mode==13) { state.parts[0]=double.class;Trace.parts=sentinelParts; }
                if(mode==14) Trace.type=Other.class;
            } };
            Object result=state.sentinel;Throwable caught=null;
            try { result=call(route,operation,type,parts); } catch(Throwable failure) { caught=failure; }
            final boolean badCast=operation==1 && (mode==4 || mode==15), actionFailure=mode>=6 && mode<=8;
            require(state.actions==(badCast?0:1) && Trace.events==(badCast?0:actionFailure?1:12) && constructions==0);
            require(Trace.result==(badCast?state.sentinel:state.result));
            require(Trace.parts==(badCast || mode==13?sentinelParts:state.parts));
            require(Trace.type==(badCast?Object.class:mode==14?Other.class:state.type));
            if(mode==13) require(state.parts[0]==double.class);
            if(badCast || mode==10) require(caught!=null && caught.getClass()==ClassCastException.class && result==state.sentinel);
            else if(actionFailure) require(caught==state.failure && result==state.sentinel);
            else require(caught==null && result==state.result);
            if(caught==null) {
                Throwable later=null;Widget value=null;
                try { Constructor<Widget> constructor=(Constructor)result;value=constructor.newInstance(state.text,state.number); } catch(Throwable failure) { later=failure; }
                require(constructions==(mode==9?0:1));
                if(mode==9) require(later!=null && later.getClass()==NullPointerException.class);
                else {
                    require(observed==state.text && observedNumber==state.number);
                    if(mode==11) require(later!=null && later.getClass()==ClassCastException.class);
                    else if(mode==12) require(later instanceof InvocationTargetException && later.getCause()==state.failure);
                    else require(later==null && value!=null && value.getClass()==Widget.class);
                }
            }
        }
        System.out.println("attempts=2,token-star-array-factory=matched");
    }
}
