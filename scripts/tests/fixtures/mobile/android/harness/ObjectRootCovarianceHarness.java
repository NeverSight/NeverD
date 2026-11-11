import fixture.*;
import java.lang.invoke.*;
import java.lang.reflect.*;
public final class ObjectRootCovarianceHarness {
    static int values, identities, states, failures;
    static void value(boolean ok) { if (!ok) throw new AssertionError("value"); values++; }
    static void same(Object a, Object b) { if (a != b) throw new AssertionError("identity"); identities++; }
    static void state(boolean ok) { if (!ok) throw new AssertionError("state"); states++; }
    interface Action { void run() throws Throwable; }
    static void failure(Action action, Class<?> kind, Throwable original) throws Throwable {
        try { action.run(); throw new AssertionError("missing failure"); }
        catch (Throwable problem) {
            if (problem.getClass() != kind) throw new AssertionError("wrong failure", problem);
            if (original != null) same(problem, original);
            failures++;
        }
    }
    static final class Derived extends Core<String> {
        int entries;
        public Derived clone() throws CloneNotSupportedException {
            entries++;
            return (Derived)super.clone();
        }
    }
    static Method original(Class<?> owner, boolean bridge) {
        for (Method method : owner.getDeclaredMethods())
            if (method.getName().equals("clone") && method.isBridge() == bridge) return method;
        throw new AssertionError("missing original method");
    }
    static Object copy(int route, Core<String> self) throws Throwable {
        try {
            switch (route) {
            case 0: return self.clone();
            case 1: return original(Core.class, true).invoke(self);
            case 2: return original(Core.class, false).invoke(self);
            case 3: return (Object)MethodHandles.publicLookup().findVirtual(Core.class, "clone",
                MethodType.methodType(Object.class)).invokeExact((Core)self);
            case 4: return (Core)MethodHandles.publicLookup().findVirtual(Core.class, "clone",
                MethodType.methodType(Core.class)).invokeExact((Core)self);
            case 5: return ObjectRootCovarianceCaller.copy(self);
            default: return Caller.copy(self);
            }
        } catch (InvocationTargetException problem) { throw problem.getCause(); }
    }
    static Object broken(int route, Broken<String> self) throws Throwable {
        try {
            switch (route) {
            case 0: return self.clone();
            case 1: return original(Broken.class, true).invoke(self);
            default: return (Object)MethodHandles.publicLookup().findVirtual(Broken.class, "clone",
                MethodType.methodType(Object.class)).invokeExact((Broken)self);
            }
        } catch (InvocationTargetException problem) { throw problem.getCause(); }
    }
    static final class NarrowFuture extends Child<String> {
        int futureEntries;
        public NarrowFuture clone() throws CloneNotSupportedException {
            futureEntries++;
            return (NarrowFuture)super.clone();
        }
    }
    static Method childSlot(Class<?> result) {
        for (Method method : Child.class.getDeclaredMethods())
            if (method.getName().equals("clone") && method.getReturnType() == result) return method;
        throw new AssertionError("missing original child slot");
    }
    static Object childCopy(int route, Child<String> self) throws Throwable {
        try {
            switch (route) {
            case 0: return self.clone();
            case 1: return ((Core<String>)self).clone();
            case 2: return childSlot(Object.class).invoke(self);
            case 3: return childSlot(Core.class).invoke(self);
            case 4: return childSlot(Child.class).invoke(self);
            case 5: return (Object)MethodHandles.publicLookup().findVirtual(Child.class, "clone",
                MethodType.methodType(Object.class)).invokeExact((Child)self);
            case 6: return (Core)MethodHandles.publicLookup().findVirtual(Child.class, "clone",
                MethodType.methodType(Core.class)).invokeExact((Child)self);
            default: return (Child)MethodHandles.publicLookup().findVirtual(Child.class, "clone",
                MethodType.methodType(Child.class)).invokeExact((Child)self);
            }
        } catch (InvocationTargetException problem) { throw problem.getCause(); }
    }
    static Object field(Object self, String name) throws Exception {
        return Child.class.getField(name).get(self);
    }
    static void field(Object self, String name, Object value) throws Exception {
        Child.class.getField(name).set(self, value);
    }
    static void narrowedChildren() throws Throwable {
        for (Class<?> result : new Class<?>[]{Object.class, Core.class, Child.class}) {
            Method method = childSlot(result);
            if (method.isBridge() != (result != Child.class)
                    || method.isSynthetic() != (result != Child.class)
                    || !java.util.Arrays.equals(method.getExceptionTypes(), new Class<?>[]{CloneNotSupportedException.class}))
                throw new AssertionError("child declaration roles or exceptions");
        }
        int cases = 0;
        for (int route = 0; route < 8; route++) for (int kind = 0; kind < 2; kind++)
            for (int mode = 0; mode < 5; mode++) {
                Child<String> self = kind == 0 ? new Child<String>() : new NarrowFuture();
                self.payload = new String("owned child alias"); self.tag = 29;
                CloneNotSupportedException checked = new CloneNotSupportedException("owned checked identity");
                RuntimeException unchecked = new IllegalStateException("owned parent identity");
                field(self, "checked", mode == 1 || mode == 2 ? checked : null);
                field(self, "after", mode == 2); field(self, "nullable", mode == 4);
                if (mode == 3) self.failure = unchecked;
                Object result = null; Throwable thrown = null;
                try { result = childCopy(route, self); } catch (Throwable problem) { thrown = problem; }
                if (thrown != (mode == 1 || mode == 2 ? checked : mode == 3 ? unchecked : null))
                    throw new AssertionError("exact original exception", thrown);
                if (!field(self, "entries").equals(1) || field(self, "childSeen") != self
                        || self.calls != (mode == 1 ? 0 : 1) || self.tag != 29
                        || self.seen != (mode == 1 ? null : self))
                    throw new AssertionError("child and parent effect order");
                if (kind == 1 && ((NarrowFuture)self).futureEntries != 1)
                    throw new AssertionError("future virtual override");
                if (mode == 0) {
                    if (result == self || result == null || result.getClass() != self.getClass())
                        throw new AssertionError("native clone identity");
                    Child<?> copy = (Child<?>)result;
                    if (copy.payload != self.payload || copy.seen != self || field(copy, "childSeen") != self
                            || copy.tag != 30 || copy.calls != 1 || !field(copy, "entries").equals(1)
                            || (kind == 1 && ((NarrowFuture)copy).futureEntries != 1))
                        throw new AssertionError("copied fields and aliases");
                } else if (result != null) throw new AssertionError("null result or exception exit");
                cases++;
            }
        System.out.println("narrowed-child-slots-and-checked-exception-identity=matched,cases=" + cases);
    }
    public static void main(String[] args) throws Throwable {
        boolean empty = args.length > 0 && args[0].startsWith("empty-");
        boolean narrowed = args.length > 0 && args[0].equals("narrowed-child");
        boolean rebuilt = empty && args[0].equals("empty-rebuilt");
        for (Class<?> owner : new Class<?>[]{Core.class, Broken.class}) {
            Method typed = original(owner, false), bridge = original(owner, true);
            value(bridge.isSynthetic() && bridge.getReturnType() == Object.class && Modifier.isPublic(bridge.getModifiers()));
            value(typed.getGenericReturnType() instanceof ParameterizedType);
            ParameterizedType returned = (ParameterizedType)typed.getGenericReturnType();
            same(returned.getRawType(), owner); same(returned.getActualTypeArguments()[0], owner.getTypeParameters()[0]);
            Class<?>[] bridgeExceptions = !empty || rebuilt
                ? new Class<?>[]{CloneNotSupportedException.class} : new Class<?>[]{};
            Class<?>[] typedExceptions = empty ? new Class<?>[]{}
                : new Class<?>[]{CloneNotSupportedException.class};
            value(java.util.Arrays.equals(bridge.getExceptionTypes(), bridgeExceptions));
            value(java.util.Arrays.equals(typed.getExceptionTypes(), typedExceptions));
            value(bridge.getGenericExceptionTypes().length == bridgeExceptions.length);
        }
        if (!narrowed) for (Method method : Child.class.getDeclaredMethods())
            if (method.getName().equals("clone")) throw new AssertionError("invented inherited clone");
        for (int route = 0; route < 7; route++) for (int kind = 0; kind < 3; kind++) {
            final int selected = route;
            Core<String> self = kind == 0 ? new Core<String>() : kind == 1 ? new Child<String>() : new Derived();
            for (String item : new String[]{null, new String("owned mutable alias")}) {
                self.payload = item;
                Object result = copy(route, self);
                value(result != self && result.getClass() == self.getClass());
                Core<?> clone = (Core<?>)result;
                same(clone.payload, item); same(clone.seen, self); same(self.seen, self);
                state(clone.calls == self.calls);
                if (self instanceof Derived) state(((Derived)clone).entries == ((Derived)self).entries);
            }
            state(self.calls == 2);
            RuntimeException original = new IllegalStateException("owned failure before native clone");
            self.failure = original;
            failure(() -> copy(selected, self), original.getClass(), original);
            state(self.calls == 3); same(self.seen, self);
            if (self instanceof Derived) state(((Derived)self).entries == 3);
            failure(() -> copy(selected, null), NullPointerException.class, null);
        }
        for (int route = 0; route < 3; route++) {
            final int selected = route;
            Broken<String> self = new Broken<String>();
            failure(() -> broken(selected, self), CloneNotSupportedException.class, null);
            state(self.calls == 1); same(self.seen, self);
            RuntimeException original = new IllegalArgumentException("owned failure before unsupported clone");
            self.failure = original;
            failure(() -> broken(selected, self), original.getClass(), original);
            state(self.calls == 2); same(self.seen, self);
        }
        System.out.println("values=" + values + ",identities=" + identities + ",states=" + states + ",failures=" + failures);
        System.out.println("object-root-and-generic-result-metadata=retained,original-checked-exceptions-and-shallow-clone-aliases=matched,external-subclass-dispatch=matched");
        if (narrowed) narrowedChildren();
    }
}
