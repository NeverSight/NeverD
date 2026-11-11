package fixture;
import java.io.IOException;
import java.lang.invoke.MethodHandles;
import java.lang.invoke.MethodType;
import java.lang.reflect.InvocationTargetException;
import java.lang.reflect.Method;
import java.lang.reflect.ParameterizedType;

public final class FixedSDKInterfaceBridgeHarness {
    private static int actions, guards;
    private static final long UNREAD=0x123456789abcdefL;
    private static final int MASK=0x5a5a5a5a;
    private static void need(boolean value) {
        if (!value) throw new AssertionError("owned fixed SDK interface bridge mismatch");
    }
    @SuppressWarnings("unchecked")
    private static <Failure extends Throwable> void raise(Throwable failure) throws Failure {
        throw (Failure)failure;
    }
    private static final class Other implements Fixed {
        public int dispatch(Fixed ignored) { throw new AssertionError("unexpected typed receiver"); }
        public long guard(long value) {
            Trace.guardSelf=this; Trace.guardArg=value; Trace.hit(4);
            Trace.guardAction.run(); Trace.hit(5); return Trace.reply;
        }
    }
    private static final class Sub extends Receiver {
        private final int kind;
        Sub(int kind) { this.kind=kind; }
        @Override public int dispatch(Fixed other) {
            Trace.hit(7);
            int value=super.dispatch(other);
            if (kind==2) { Trace.hit(8); return value^MASK; }
            return value;
        }
        int originalBridge(Object value) throws Throwable {
            return (int)MethodHandles.lookup().findSpecial(Receiver.class, "dispatch",
                MethodType.methodType(int.class, Object.class), Sub.class).invokeExact(this, value);
        }
    }
    @SuppressWarnings({"rawtypes", "unchecked"})
    private static int invoke(int route, int operation, Receiver receiver, Object value) throws Throwable {
        if (route==0) return ((Contract)receiver).dispatch(value);
        if (route==1) return FixedSDKInterfaceBridgeCaller.raw(receiver, value);
        if (route==2) {
            try { return (Integer)Receiver.class.getDeclaredMethod("dispatch", Object.class).invoke(receiver, value); }
            catch (InvocationTargetException failure) { throw failure.getCause(); }
        }
        if (operation!=0) return ((Sub)receiver).originalBridge(value);
        return (int)MethodHandles.publicLookup().findVirtual(Receiver.class, "dispatch",
            MethodType.methodType(int.class, Object.class)).invokeExact(receiver, value);
    }
    private static void shape() throws Exception {
        Method bridge=Receiver.class.getDeclaredMethod("dispatch", Object.class);
        Method typed=Receiver.class.getDeclaredMethod("dispatch", Fixed.class);
        need(bridge.isBridge() && bridge.isSynthetic() && bridge.getReturnType()==int.class);
        need(!typed.isBridge() && !typed.isSynthetic() && typed.getReturnType()==int.class);
        need(Receiver.class.getTypeParameters().length==0 && Receiver.class.getGenericInterfaces().length==1
            && Receiver.class.getGenericInterfaces()[0]==Fixed.class);
        ParameterizedType ancestor=(ParameterizedType)Fixed.class.getGenericInterfaces()[0];
        need(ancestor.getRawType()==Contract.class && ancestor.getActualTypeArguments()[0]==Fixed.class);
    }
    private static void run(int route, int operation, final int mode, final int attempt) throws Throwable {
        actions=guards=0;
        Trace.self=Trace.other=Trace.guardSelf=null;
        Trace.events=0; Trace.guardArg=UNREAD; Trace.observed=UNREAD;
        final long word=attempt==0 ? Long.MIN_VALUE : Long.MAX_VALUE;
        final long reply=attempt==0 ? Long.MAX_VALUE : Long.MIN_VALUE;
        final int result=attempt==0 ? Integer.MIN_VALUE : Integer.MAX_VALUE;
        Trace.word=word; Trace.reply=reply; Trace.result=result;
        Trace.invoke=mode!=1 && mode!=12;
        final RuntimeException runtime=new IllegalStateException("owned");
        final Error error=new AssertionError("owned");
        final IOException checked=new IOException("owned");
        final Object changedOther=new Object();
        Trace.action=new Runnable() { public void run() {
            actions++;
            if (mode==6) throw runtime;
            if (mode==7) throw error;
            if (mode==8) FixedSDKInterfaceBridgeHarness.<RuntimeException>raise(checked);
            if (mode==14) { Trace.other=changedOther; Trace.word=17+attempt; }
            if (mode==15) Trace.invoke=false;
        }};
        Trace.guardAction=mode==9 ? null : new Runnable() { public void run() {
            guards++;
            if (mode==10) throw runtime;
            if (mode==11) FixedSDKInterfaceBridgeHarness.<RuntimeException>raise(checked);
            if (mode==13) { Trace.result=0x10203040^attempt; Trace.word=99; Trace.reply=23+attempt; }
        }};
        Receiver receiver=operation==0 ? new Receiver() : new Sub(operation);
        Object argument=mode==5 ? receiver : new Other();
        if (mode==1 || mode==2) argument=null;
        if (mode==3 || mode==4) argument=new Object();
        if (mode==4) receiver=null;
        int value=0; Throwable failure=null;
        try { value=invoke(route,operation,receiver,argument); }
        catch (Throwable observed) { failure=observed; }
        boolean start=mode!=3 && mode!=4;
        boolean afterAction=start && mode!=6 && mode!=7 && mode!=8;
        boolean guard=afterAction && mode!=1 && mode!=12 && mode!=15 && mode!=2;
        boolean finish=afterAction && mode!=2 && mode!=9 && mode!=10 && mode!=11;
        int events=0;
        if (start) { if (operation!=0) events=7; events=events*10+1; }
        if (afterAction) events=events*10+2;
        if (guard) events=events*10+4;
        if (guard && finish) events=events*10+5;
        if (finish) { events=events*10+3; if (operation==2) events=events*10+8; }
        need(Trace.events==events && actions==(start ? 1 : 0) && guards==(guard && mode!=9 ? 1 : 0));
        need(Trace.self==(start ? receiver : null));
        need(Trace.other==(mode==14 ? changedOther : start ? argument : null));
        need(Trace.guardSelf==(guard ? argument : null));
        need(Trace.guardArg==(guard ? mode==14 ? 17+attempt : word : UNREAD));
        need(Trace.observed==(guard && finish ? mode==13 ? 23+attempt : reply : UNREAD));
        if (mode==3) need(failure instanceof ClassCastException);
        else if (mode==2 || mode==4 || mode==9) need(failure instanceof NullPointerException);
        else if (mode==6 || mode==10) need(failure==runtime);
        else if (mode==7) need(failure==error);
        else if (mode==8 || mode==11) need(failure==checked);
        else {
            int wanted=mode==13 ? 0x10203040^attempt : result;
            if (operation==2) wanted^=MASK;
            need(failure==null && value==wanted);
        }
    }
    public static void main(String[] args) throws Throwable {
        shape();
        for (int attempt=0; attempt<2; attempt++) run(Integer.parseInt(args[0]), Integer.parseInt(args[1]), Integer.parseInt(args[2]), attempt);
        System.out.println("attempts=2,fixed-sdk-interface-bridge=matched");
    }
}
