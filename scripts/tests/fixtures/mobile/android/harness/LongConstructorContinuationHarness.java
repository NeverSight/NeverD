package fixture;

import java.lang.invoke.MethodHandle;
import java.lang.invoke.MethodHandles;
import java.lang.invoke.MethodType;

public final class LongConstructorContinuationHarness {
    private static void check(boolean value) {
        if (!value) throw new AssertionError("owned long constructor continuation mismatch");
    }
    private static final class Failure extends Exception {}
    private static final class Fault extends Error {}
    @SuppressWarnings("unchecked")
    private static <T extends Throwable> RuntimeException unchecked(Throwable value) throws T {
        throw (T)value;
    }
    private static final class Derived extends Core {
        Derived(Provider provider, boolean chooseTail) { super(provider, chooseTail); }
        @Override public String kind() { return "derived"; }
    }
    private static final class Observer extends Trace {
        final int mode, route;
        final Provider input;
        final Object first, second, third;
        final boolean chooseTail;
        final Throwable marker;
        Core escaped;
        Object seed, tail;
        int phase, index, state;
        boolean failed;
        Observer(int mode, int route, Provider input, Object first, Object second, Object third,
                 boolean chooseTail, Throwable marker) {
            this.mode=mode; this.route=route; this.input=input; this.first=first;
            this.second=second; this.third=third; this.chooseTail=chooseTail;
            this.marker=marker; this.seed=mode==3 ? null : first;
        }
        void fail(int wanted) {
            if (mode==wanted) { failed=true; throw unchecked(marker); }
        }
        void receiver(Core value) {
            check(value!=null && (escaped==null || value==escaped));
            escaped=value;
            check(value.kind().equals(route==2 ? "derived" : "base"));
            check(value.state==state);
        }
        @Override public void observe(int event, Core value, Provider provider, int at) {
            if (event==0) {
                check(phase==0 && value==null && provider==input && at==-1);
                phase=1; fail(4);
                if (mode==14) { provider.seed=second; seed=second; }
                return;
            }
            if (event==5) {
                check(phase==5 && value==null && provider==input && at==-1 && chooseTail);
                check(escaped.tail==null); phase=6; fail(11);
                if (mode==14) provider.tail=third;
                tail=provider.tail;
                return;
            }
            receiver(value);
            if (event==1) {
                check(phase==1 && value.token==null && provider==null && at==-1);
                check(value.tail==null); phase=2; fail(5); return;
            }
            check(value.token==seed);
            if (event==2) {
                check(phase==2 && provider==null && at==-1 && value.tail==null);
                phase=3; fail(6); return;
            }
            check(provider==input);
            if (event==3) {
                check(phase==3 && at==-1 && value.tail==null);
                phase=4; fail(7); return;
            }
            if (event==4) {
                check(phase==4 && at==index && value.tail==null);
                ++index;
                if (at==0) fail(8);
                if (at==31) fail(9);
                if (at==71) fail(10);
                if (mode==13 && at==17) { value.state=13; state=13; }
                if (mode==14 && at==60) provider.tail=second;
                state=value.state*31+at;
                if (index==72) phase=5;
                return;
            }
            check(event==6 && at==-1 && index==72 && phase==(chooseTail ? 6 : 5));
            if (!chooseTail) tail=seed;
            check(value.tail==tail); phase=7; fail(12);
        }
        void finish(Core result, Throwable failure) {
            if (mode==2) {
                check(failure instanceof NullPointerException && phase==1 && escaped==null);
            } else if (mode>=4 && mode<=12) {
                check(failed && failure==marker && result==null);
            } else {
                check(!failed && failure==null && phase==7 && index==72 && result==escaped);
            }
            if (escaped!=null) {
                check(escaped.state==state && escaped.tail==tail);
                check(escaped.token==(phase>=3 ? seed : null));
            }
        }
    }
    public static void main(String[] args) throws Throwable {
        int route=Integer.parseInt(args[0]), mode=Integer.parseInt(args[1]);
        check(route>=0 && route<3 && mode>=0 && mode<16);
        for (int attempt=0;attempt<2;++attempt) {
            Object first=new Object(),second=new Object(),third=new Object();
            Provider provider=mode==2 ? null : new Provider(mode==3 ? null : first,mode==15 ? null : second);
            boolean chooseTail=mode==1 || mode==3 || mode==11 || mode==14 || mode==15;
            Throwable marker=mode==10 ? new Fault() : new Failure();
            Observer observer=new Observer(mode,route,provider,first,second,third,chooseTail,marker);
            Trace.hook=observer;
            Core result=null; Throwable failure=null;
            try {
                if (route==0) result=new Core(provider,chooseTail);
                else if (route==1) {
                    MethodHandle ctor=MethodHandles.lookup().findConstructor(Core.class,
                        MethodType.methodType(void.class,Provider.class,boolean.class));
                    result=(Core)ctor.invokeWithArguments(provider,chooseTail);
                } else result=new Derived(provider,chooseTail);
            } catch(Throwable caught) { failure=caught; }
            observer.finish(result,failure);
            check(Trace.hook==observer);
            Trace.hook=null;
        }
        System.out.println("attempts=2,long-constructor-continuation=matched");
    }
}
