import fixture.*;
import java.lang.invoke.*;

@SuppressWarnings({"rawtypes","unchecked"})
public final class SeededPairFactoryHarness {
    static void check(boolean ok) {
        if (!ok) throw new AssertionError("owned seeded pair factory mismatch");
    }
    public static void main(String[] args) throws Throwable {
        int route=Integer.parseInt(args[0]), mode=Integer.parseInt(args[1]);
        MethodHandle call=MethodHandles.lookup().findStatic(Core.class,"call",
            MethodType.methodType(PairValue.class,Object.class,Object.class));
        for (int attempt=0;attempt<2;++attempt) {
            Observer observer=new Observer(mode);
            Trace.hook=observer;
            Trace.next=mode==4 ? new Object() : mode==5 ? null : observer.primary;
            PairValue result=null;
            Throwable caught=null;
            try {
                if (route==1) {
                    PairValue<Base,Base> pair=Core.typed(observer.left,observer.right);
                    result=pair;
                    Base first=pair.first; observer.events+="F,";
                    Base second=pair.second; observer.events+="S,";
                    check(first==observer.first() && second==observer.second());
                } else result=route==0 ? Core.call(observer.left,observer.right)
                    : (PairValue)call.invokeExact((Object)observer.left,(Object)observer.right);
            } catch (Throwable failure) { caught=failure; }
            if (observer.failure()!=null) check(caught==observer.failure());
            else if (mode==4 || route==1 && (mode==10 || mode==11 || mode==16 || mode==18))
                check(caught!=null && caught.getClass()==ClassCastException.class);
            else if (route==1 && mode==5)
                check(caught!=null && caught.getClass()==NullPointerException.class);
            else check(caught==null);
            check(observer.events.equals(observer.events(route)));
            if (observer.failure()==null && mode!=4) {
                check(result==observer.result());
                if (result!=null) check(result.first==observer.first()
                                      && result.second==observer.second());
            }
        }
        System.out.println("attempts=2,seeded-pair-factory=matched");
    }
    public static final class Observer extends Trace {
        final int mode;
        final Object left,right;
        final PairValue primary=new PairValue(), alternate=new PairValue();
        final Object first=new Base(new Object()), second=new Base(new Object());
        final Object changed=new Base(new Object());
        final Object rawFirst,rawSecond;
        final Object changedRaw="owned changed raw field";
        final Exception checked=new Exception("owned checked failure");
        final Error error=new LinkageError("owned error failure");
        String events="";
        Observer(int mode) {
            this.mode=mode;
            left=mode==1 || mode==3 ? null : new Object();
            right=mode==2 || mode==3 ? null : new Base(new Object());
            rawFirst=mode==10 ? new String("owned raw field")
                : mode==12 ? null : mode==18 ? new Object[]{first} : first;
            rawSecond=mode==11 ? new Object() : mode==13 ? null : second;
            primary.first=rawFirst; primary.second=rawSecond;
            alternate.first=second; alternate.second=first;
        }
        @Override public void observe(int phase,Object a,Object b,Object value) {
            events+=phase+",";
            check(a==left && b==right);
            if (phase==1) {
                check(value==null);
                if (mode==15) Trace.next=alternate;
            } else {
                check(phase==2 && value==result());
                if (mode==14) Trace.next=alternate;
                if (mode==19) Trace.next=null;
                if (mode==16) primary.first=changedRaw;
                if (mode==17) primary.second=changed;
            }
            if (phase==1 && mode==6 || phase==2 && mode==8) fail(error);
            if (phase==1 && mode==7 || phase==2 && mode==9) fail(checked);
        }
        PairValue result() { return mode==5 ? null : mode==15 ? alternate : primary; }
        Object first() { return mode==5 ? null : mode==15 ? second : mode==16 ? changedRaw : rawFirst; }
        Object second() { return mode==5 ? null : mode==15 ? first : mode==17 ? changed : rawSecond; }
        Throwable failure() {
            return mode==6 || mode==8 ? error : mode==7 || mode==9 ? checked : null;
        }
        String events(int route) {
            String result=mode==4 || mode==6 || mode==7 ? "1," : "1,2,";
            if (route!=1 || failure()!=null || mode==4 || mode==5
                || mode==10 || mode==16 || mode==18) return result;
            return result+(mode==11 ? "F," : "F,S,");
        }
        @SuppressWarnings("unchecked")
        static <T extends Throwable> void fail(Throwable failure) throws T { throw (T)failure; }
    }
}
