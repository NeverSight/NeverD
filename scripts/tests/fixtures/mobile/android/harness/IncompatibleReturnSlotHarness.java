import fixture.*;
import java.lang.reflect.*;

public final class IncompatibleReturnSlotHarness {
    private static int identities, failures;
    private static void check(boolean condition) { if (!condition) throw new AssertionError(); }
    private static void same(Object a, Object b) { check(a == b); identities++; }
    private static Method slot(Class<?> owner, String name, Class<?>... parameters) throws Exception {
        for (Method method : owner.getMethods())
            if (method.getName().equals(name) && method.getReturnType() == Persistent.class &&
                    java.util.Arrays.equals(method.getParameterTypes(), parameters)) return method;
        throw new AssertionError(name);
    }
    private static Object call(Actor<Object> actor, Object value, int operation, int mode, Method[] methods) throws Exception {
        if (mode == 0) {
            if (operation == 0) return Caller.remove(actor, value, value);
            if (operation == 1) return Caller.put(actor, value);
            return Caller.clear(actor);
        }
        Object[] parameters = operation == 0 ? new Object[] {value,value} :
                              operation == 1 ? new Object[] {value} : new Object[] {};
        return methods[operation].invoke(actor, parameters);
    }
    public static void main(String[] args) throws Exception {
        String[] names = {args[0],args[1],args[2]};
        Method[] contracts = {slot(Persistent.class,names[0],Object.class,Object.class),
                              slot(Persistent.class,names[1],Object.class),slot(Persistent.class,names[2])};
        Object first = new Object(), second = new Object();
        Object[] values = {first,second,null};
        Actor<Object>[] actors = new Actor[] {new Actor<>(),new Derived()};
        for (Actor<Object> actor : actors) {
            boolean derived = actor instanceof Derived;
            Method[] concrete = {slot(actor.getClass(),names[0],Object.class,Object.class),
                                 slot(actor.getClass(),names[1],Object.class),slot(actor.getClass(),names[2])};
            for (Object value : values) {
                for (int operation = 0; operation < 3; operation++) for (int mode = 0; mode < 3; mode++) {
                    actor.value = second;
                    Trace.events = 0;
                    int before = actor.calls;
                    same(call(actor,value,operation,mode,mode == 1 ? contracts : concrete),actor);
                    check(actor.calls == before + (derived ? 2 : 1));
                    check(Trace.events == operation + (derived ? 7 : 4));
                    check(actor.value == (operation == 0 ? second : operation == 1 ? value : null));
                }
                RootBase<Object> root = actor;
                Trace.events = 0; same(root.put(value),value); check(Trace.events == 2);
                Trace.events = 0; check(root.remove(value,value) && Trace.events == 1);
                Trace.events = 0; root.clear(); check(Trace.events == 3);
            }
            RuntimeException sentinel = new IllegalStateException("owned");
            actor.failure = sentinel;
            for (int operation = 0; operation < 3; operation++) {
                Trace.events = 0;
                int before = actor.calls;
                try { call(actor,first,operation,0,contracts); throw new AssertionError(); }
                catch (RuntimeException actual) { same(actual,sentinel); failures++; }
                check(actor.calls == before + (derived ? 2 : 1));
                check(Trace.events == operation + (derived ? 7 : 4));
            }
        }
        check(Actor.__neverdReturnSlot() == 7);
        check(contracts[1].getGenericReturnType().getTypeName().equals("fixture.Persistent<P>"));
        check(slot(Actor.class,names[1],Object.class).getGenericReturnType().getTypeName().equals("fixture.Persistent<A>"));
        check(RootBase.class.getDeclaredMethod("put",Object.class).getGenericReturnType().getTypeName().equals("T"));
        check(Actor.class.getMethod("remove",Object.class,Object.class).getDeclaringClass() == RootBase.class || names[0].equals("remove"));
        check(Leaf.class.getGenericInterfaces()[0].getTypeName().equals("fixture.Persistent<L>"));
        System.out.println("identities=" + identities + ",failures=" + failures);
        System.out.println("original-return-dispatch=matched,external-defaults=retained,source-name-abi-difference=reported");
    }
}
