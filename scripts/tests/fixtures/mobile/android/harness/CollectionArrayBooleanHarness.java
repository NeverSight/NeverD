package fixture;

import java.lang.invoke.MethodHandles;
import java.lang.invoke.MethodType;
import java.util.AbstractCollection;
import java.util.ArrayList;
import java.util.Iterator;

public final class CollectionArrayBooleanHarness {
    private static void check(boolean value) {
        if (!value) throw new AssertionError("owned collection array boolean mismatch");
    }
    private static final class Failure extends Exception {}
    private static final class Fault extends Error {}
    @SuppressWarnings("unchecked")
    private static <T extends Throwable> RuntimeException unchecked(Throwable value) throws T {
        throw (T)value;
    }
    private static final class Sink extends AbstractCollection<String> {
        final ArrayList<String> actual=new ArrayList<String>();
        Observer observer;
        @Override public Iterator<String> iterator() { return actual.iterator(); }
        @Override public int size() { return actual.size(); }
        @Override public boolean add(String value) {
            Observer o=observer;
            check(o.waiting && value==o.current);
            o.waiting=false;
            if (o.mode==11) { o.failed=true; throw unchecked(o.marker); }
            boolean accepted=o.mode!=1;
            if (accepted) { actual.add(value); o.expected.add(value); }
            if (o.mode==15 && o.index==2) {
                o.values[2]=o.later; o.slots[2]=o.later;
                o.failed=true; throw unchecked(o.marker);
            }
            return accepted;
        }
    }
    private static final class Observer extends Trace {
        final int mode,route;
        final Object carrier,raw;
        final Object[] values,slots;
        final Sink sink;
        final Throwable marker;
        final String replacement,later;
        final ArrayList<Object> expected=new ArrayList<Object>();
        final boolean wanted;
        int stage,index;
        boolean waiting,failed;
        Object current;
        Observer(int mode,int route,Object carrier,Object raw,Sink sink,Throwable marker,
                 String replacement,String later) {
            this.mode=mode; this.route=route; this.carrier=carrier; this.raw=raw;
            this.sink=sink; this.marker=marker; this.replacement=replacement; this.later=later;
            values=raw instanceof Object[] ? (Object[])raw : null;
            slots=values==null ? null : values.clone();
            wanted=mode==16 ? !(mode!=1) : mode!=1;
        }
        void fail(int wantedMode) {
            if (mode==wantedMode) { failed=true; throw unchecked(marker); }
        }
        @Override public void observe(int phase,Object owner,Object array,Object value) {
            check(owner==carrier && array==raw);
            if (phase==3) { check(stage==0 && value==null); stage=1; return; }
            if (phase==0) { check(stage==1 && value==null); stage=2; fail(9); return; }
            if (phase==1) {
                check(stage==2 && !waiting && index<slots.length && value==slots[index]);
                current=value; int at=index++; waiting=true;
                if (at==0) fail(10);
                if (mode==14 && at==0) {
                    values[0]=replacement; slots[0]=replacement;
                    values[1]=later; slots[1]=later;
                }
                if (mode==18 && at==0) values[0]=new Object();
                return;
            }
            if (phase==2) {
                check(stage==2 && !waiting && index==slots.length && value==null);
                stage=3;
                if (mode==16) Trace.returned=!Trace.returned;
                fail(12); return;
            }
            check(phase==4 && stage==3 && !waiting);
            check(route==2 ? value==null : value==Boolean.valueOf(wanted));
            stage=4;
            if (mode==17) Trace.returned=!Trace.returned;
            fail(13);
        }
        void finish(Object result,Throwable failure) {
            if (mode==2 || mode==3) {
                check(failure instanceof NullPointerException && stage==2);
                check(index==(mode==2 ? 1 : 0));
            } else if (mode==4 || mode==5 || mode==6 || mode==19) {
                check(failure instanceof ClassCastException);
                check(stage==((mode==4 || mode==5) ? 0 : 2));
                check(index==(mode==6 ? 2 : mode==19 ? 1 : 0));
            } else if (mode==18) {
                check(failure instanceof ArrayStoreException && stage==2 && index==1 && waiting);
            } else if ((mode>=9 && mode<=13) || mode==15) {
                check(failed && failure==marker && result==null);
            } else {
                check(failure==null && !failed && stage==4 && index==slots.length && !waiting);
                check(route==2 ? result==null : result==Boolean.valueOf(wanted));
            }
            check(sink.actual.size()==expected.size());
            for (int i=0;i<expected.size();++i) check(sink.actual.get(i)==expected.get(i));
            if (values!=null) {
                check(values.length==slots.length);
                for (int i=0;i<slots.length;++i) check(values[i]==slots[i]);
            }
        }
    }
    public static void main(String[] args) throws Throwable {
        int route=Integer.parseInt(args[0]),mode=Integer.parseInt(args[1]);
        check(route>=0 && route<3 && mode>=0 && mode<20);
        for (int attempt=0;attempt<2;++attempt) {
            String a=new String("a"),b=new String("b"),c=new String("c");
            String replacement=new String("replacement"),later=new String("later");
            Sink sink=new Sink();
            Object carrier=mode==2 || mode==5 || mode==7 ? null : mode==4 ? new Object() : sink;
            Object values=new Object[]{a,b,c};
            if (mode==3) values=null;
            if (mode==5) values=new Object();
            if (mode==6) values=new Object[]{a,new Object(),c};
            if (mode==7) values=new Object[0];
            if (mode==8) values=new Object[]{null,b,c};
            if (mode==18) values=new String[]{a,b,c};
            if (mode==19) values=new Object[][]{{a},{b}};
            Throwable marker=mode==10 || mode==13 ? new Fault() : new Failure();
            Observer observer=new Observer(mode,route,carrier,values,sink,marker,replacement,later);
            sink.observer=observer; Trace.hook=observer; Trace.returned=mode!=1;
            Object result=null; Throwable failure=null;
            try {
                if (route==0) result=Boolean.valueOf(Core.run(carrier,values));
                else if (route==1) result=MethodHandles.lookup().findStatic(Core.class,"run",
                    MethodType.methodType(boolean.class,Object.class,Object.class)).invokeWithArguments(carrier,values);
                else Core.ignore(carrier,values);
            } catch(Throwable caught) { failure=caught; }
            observer.finish(result,failure); check(Trace.hook==observer); Trace.hook=null;
        }
        System.out.println("attempts=2,collection-array-boolean=matched");
    }
}
