package fixture;
import java.io.IOException;
import java.lang.invoke.MethodHandles;
import java.lang.invoke.MethodType;
import java.lang.reflect.InvocationTargetException;
import java.lang.reflect.Modifier;
import java.util.Arrays;

public final class NestedPrefixAllocationHarness {
    private static void need(boolean value) {
        if (!value) throw new AssertionError("owned nested prefix allocation mismatch");
    }
    @SuppressWarnings("unchecked")
    private static <Failure extends Throwable> void raise(Throwable failure) throws Failure { throw (Failure)failure; }
    private static final class Observer {
        final int mode, attempt, phase;
        final Object head, first, second;
        final Object replacement=new Object();
        final Throwable fault;
        final int[] seen=new int[6];
        Observer(int mode,int attempt) {
            this.mode=mode; this.attempt=attempt;
            head=mode==1 || mode==4 ? null : new Object();
            first=mode==1 || mode==2 ? null : new int[]{attempt==0 ? Integer.MIN_VALUE : Integer.MAX_VALUE};
            second=mode==1 || mode==3 ? null : new int[]{attempt==0 ? Integer.MAX_VALUE : Integer.MIN_VALUE};
            phase=mode==5 || mode==6 ? 0 : mode==7 || mode==8 ? 1
                : mode>=9 && mode<=20 ? 2+(mode-9)/3 : -1;
            boolean checked=mode>=9 && mode<=20 && (mode-9)%3==2;
            boolean error=mode==6 || mode==8 || mode>=9 && mode<=20 && (mode-9)%3==1;
            fault=checked ? new IOException("owned") : error ? new AssertionError("owned") : new IllegalStateException("owned");
        }
        Runnable callback(final int kind) {
            return new Runnable() { public void run() {
                seen[kind]++;
                if (kind==phase) NestedPrefixAllocationHarness.<RuntimeException>raise(fault);
                if (mode==21 && kind==0) { Trace.head=replacement; Trace.first=replacement; Trace.second=replacement; }
                if (mode==22 && kind==2) ((int[])first)[0]=17+attempt;
                if (mode==23 && kind==3) ((int[])second)[0]=31+attempt;
            }};
        }
        void reset() {
            Trace.events=0;
            Trace.leafSelf=Trace.wrapSelf=Trace.baseSelf=Trace.coreSelf=null;
            Trace.head=head; Trace.first=first; Trace.second=second;
            Trace.outerInit=callback(0); Trace.innerInit=callback(1);
            Trace.innerAction=callback(2); Trace.outerAction=callback(3);
            Trace.baseAction=callback(4); Trace.after=callback(5);
        }
        void verify(Core result,Throwable failure) {
            boolean initializationFailure=phase==0 || phase==1;
            boolean running=!(attempt==1 && initializationFailure);
            int[] wanted=new int[6]; long events=0;
            if (attempt==0) {
                events=events*16+1; wanted[0]++;
                if (phase==0) running=false;
                else {
                    events=events*16+2; events=events*16+3; wanted[1]++;
                    if (phase==1) running=false;
                    else events=events*16+4;
                }
            }
            boolean[] entered=new boolean[4];
            for (int kind=2; kind<6 && running; kind++) {
                entered[kind-2]=true;
                events=events*16+(kind*2+1); wanted[kind]++;
                if (kind==phase) running=false;
                else events=events*16+(kind*2+2);
            }
            need(Trace.events==events && Arrays.equals(seen,wanted));
            need((Trace.leafSelf!=null)==entered[0] && (Trace.wrapSelf!=null)==entered[1]
                && (Trace.baseSelf!=null)==entered[2] && (Trace.coreSelf!=null)==entered[3]);
            if (entered[0]) need(((Leaf)Trace.leafSelf).value==first);
            if (entered[1]) need(((Wrap)Trace.wrapSelf).child==Trace.leafSelf && ((Wrap)Trace.wrapSelf).tail==second);
            if (entered[2]) need(((Base)Trace.baseSelf).head==head && ((Base)Trace.baseSelf).value==Trace.wrapSelf);
            if (entered[3]) need(Trace.coreSelf==Trace.baseSelf);
            need(result==(failure==null ? Trace.coreSelf : null));
            if (initializationFailure && attempt==1) need(failure instanceof NoClassDefFoundError);
            else if (initializationFailure && !(fault instanceof Error))
                need(failure instanceof ExceptionInInitializerError && failure.getCause()==fault);
            else need(failure==(phase>=0 ? fault : null));
            if (first!=null) need(((int[])first)[0]==(mode==22 ? 17+attempt : attempt==0 ? Integer.MIN_VALUE : Integer.MAX_VALUE));
            if (second!=null) need(((int[])second)[0]==(mode==23 ? 31+attempt : attempt==0 ? Integer.MAX_VALUE : Integer.MIN_VALUE));
            boolean changed=mode==21 && attempt==0;
            need(Trace.head==(changed ? replacement : head) && Trace.first==(changed ? replacement : first)
                && Trace.second==(changed ? replacement : second));
        }
    }
    private static Core invoke(int route,int operation,Observer o) throws Throwable {
        if (route==0) return operation==0 ? new Core(o.head,o.first,o.second) : new Core(o.head,o.first,o.second,7);
        if (route==1) return NestedPrefixAllocationCaller.create(operation,o.head,o.first,o.second);
        if (route==2) {
            try {
                if (operation==0) return Core.class.getConstructor(Object.class,Object.class,Object.class).newInstance(o.head,o.first,o.second);
                return Core.class.getConstructor(Object.class,Object.class,Object.class,int.class).newInstance(o.head,o.first,o.second,7);
            } catch (InvocationTargetException failure) { throw failure.getCause(); }
        }
        if (operation==0) return (Core)MethodHandles.publicLookup().findConstructor(Core.class,
            MethodType.methodType(void.class,Object.class,Object.class,Object.class)).invokeExact(o.head,o.first,o.second);
        return (Core)MethodHandles.publicLookup().findConstructor(Core.class,
            MethodType.methodType(void.class,Object.class,Object.class,Object.class,int.class)).invokeExact(o.head,o.first,o.second,7);
    }
    public static void main(String[] args) throws Throwable {
        for (Class<?> owner : new Class<?>[]{Leaf.class,Wrap.class,Base.class})
            for (java.lang.reflect.Field field : owner.getDeclaredFields()) need(Modifier.isFinal(field.getModifiers()));
        int route=Integer.parseInt(args[0]), operation=Integer.parseInt(args[1]), mode=Integer.parseInt(args[2]);
        for (int attempt=0;attempt<2;attempt++) {
            Observer o=new Observer(mode,attempt);o.reset();Core result=null;Throwable failure=null;
            try { result=invoke(route,operation,o); } catch (Throwable actual) { failure=actual; }
            o.verify(result,failure);
        }
        System.out.println("attempts=2,nested-prefix-allocation=matched");
    }
}
