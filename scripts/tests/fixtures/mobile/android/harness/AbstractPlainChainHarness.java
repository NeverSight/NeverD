import fixture.*;
import java.lang.reflect.*;

public final class AbstractPlainChainHarness {
    private static int values, identities, states, failures;

    private interface Call { Object run(); }

    private static final class Case {
        final int kind;
        final boolean forwarding;
        final Call call;
        Case(int kind, boolean forwarding, Call call) {
            this.kind = kind;
            this.forwarding = forwarding;
            this.call = call;
        }
    }

    private static final class Expected {
        int calls, forwardingCalls, events, kind, first, second;
        Object payload, left, right;

        void record(Case path, int first, int second, Object payload, Object left, Object right) {
            if (path.forwarding) {
                forwardingCalls++;
                this.payload = payload;
                events = events * 31 + 6;
            }
            if (path.kind == 5) {
                this.left = left;
                this.right = right;
                first = second = 0;
            }
            calls++;
            kind = path.kind;
            this.first = first;
            this.second = second;
            events = events * 31 + path.kind;
        }

        void check(Child<?, ?> receiver) {
            if (receiver.calls != calls || receiver.forwardingCalls != forwardingCalls
                    || receiver.events != events || receiver.lastKind != kind
                    || receiver.lastFirst != first || receiver.lastSecond != second)
                throw new AssertionError("callback dispatch or ordered primitive effects changed");
            states++;
            same(receiver.lastPayload, payload);
            same(receiver.lastLeft, left);
            same(receiver.lastRight, right);
        }
    }

    private static void same(Object actual, Object expected) {
        if (actual != expected) throw new AssertionError("callback reference identity changed");
        identities++;
    }

    private static Case[] paths(Child<Object, Object> child, int first, int second,
                                Object payload, Object left, Object right) {
        Parent<Object, Object> parent = child;
        Middle<Object, Object> middle = child;
        Updates updates = child;
        Order<Object> order = child;
        return new Case[] {
            new Case(1, false, () -> { child.added(first, second); return null; }),
            new Case(1, false, () -> { parent.added(first, second); return null; }),
            new Case(1, false, () -> { updates.added(first, second); return null; }),
            new Case(2, false, () -> { child.moved(first, second); return null; }),
            new Case(2, false, () -> { parent.moved(first, second); return null; }),
            new Case(2, false, () -> { updates.moved(first, second); return null; }),
            new Case(3, false, () -> { child.removed(first, second); return null; }),
            new Case(3, false, () -> { parent.removed(first, second); return null; }),
            new Case(3, false, () -> { updates.removed(first, second); return null; }),
            new Case(4, false, () -> { child.changed(first, second); return null; }),
            new Case(4, false, () -> { parent.changed(first, second); return null; }),
            new Case(4, true, () -> { child.changed(first, second, payload); return null; }),
            new Case(4, true, () -> { parent.changed(first, second, payload); return null; }),
            new Case(4, true, () -> { updates.changed(first, second, payload); return null; }),
            new Case(5, false, () -> child.compare(left, right)),
            new Case(5, false, () -> parent.compare(left, right)),
            new Case(5, false, () -> order.compare(left, right)),
            new Case(1, false, () -> { middle.added(first, second); return null; }),
            new Case(3, false, () -> { middle.removed(first, second); return null; }),
            new Case(4, true, () -> { middle.changed(first, second, payload); return null; }),
            new Case(5, false, () -> middle.compare(left, right))
        };
    }

    private static void failure(Call call, Class<? extends Throwable> kind, RuntimeException same) {
        try {
            call.run();
            throw new AssertionError("callback failure missing");
        } catch (Throwable error) {
            if (error.getClass() != kind || (same != null && error != same))
                throw new AssertionError("callback exception type or identity changed", error);
            if (same != null) identities++;
            failures++;
        }
    }

    public static void main(String[] args) throws Exception {
        if (!Modifier.isAbstract(Parent.class.getModifiers())
                || !Child.class.getGenericSuperclass().getTypeName().equals("fixture.Middle<S, V>"))
            throw new AssertionError("original generic parent scope changed");
        int middleVirtualMethods = 0;
        for (Method method : Middle.class.getDeclaredMethods())
            if (!Modifier.isStatic(method.getModifiers())) middleVirtualMethods++;
        if (!Modifier.isAbstract(Middle.class.getModifiers())
                || !Middle.class.getGenericSuperclass().getTypeName().equals("fixture.Parent<M, N>")
                || middleVirtualMethods != (args.length > 0 ? 1 : 0))
            throw new AssertionError("abstract middle declaration or scope changed");
        Type[] interfaces = Parent.class.getGenericInterfaces();
        if (interfaces.length != 2 || !interfaces[0].getTypeName().equals("fixture.Order<T>")
                || interfaces[1] != Updates.class)
            throw new AssertionError("mixed generic and plain interface contracts changed");
        for (String name : new String[] {"added", "moved", "removed"}) {
            for (Method method : Parent.class.getDeclaredMethods()) {
                if (method.getName().equals(name))
                    throw new AssertionError("abstract parent acquired an invented declaration");
            }
            if (Parent.class.getMethod(name, int.class, int.class).getDeclaringClass() != Updates.class)
                throw new AssertionError("deferred interface declaration ownership changed");
        }
        for (Class<?> owner : new Class<?>[] {Parent.class, Child.class}) {
            Method compare = owner.getDeclaredMethod("compare", Object.class, Object.class);
            Type own = owner.getTypeParameters()[0];
            if (!compare.getGenericParameterTypes()[0].equals(own)
                    || !compare.getGenericParameterTypes()[1].equals(own))
                throw new AssertionError("complete compare parameter scopes changed");
        }
        Child<Object, Object> receiver = new Child<>();
        Expected expected = new Expected();
        Object a = new Object();
        Object b = new Object();
        Object[] references = {null, a, b, a, null};
        int[] numbers = {0, 1, -1, Integer.MIN_VALUE, Integer.MAX_VALUE};
        for (int round = 0; round < numbers.length; round++) {
            int first = numbers[round];
            int second = numbers[(round + 1) % numbers.length];
            Object payload = references[round];
            Object left = references[round];
            Object right = round % 2 == 0 ? left : references[(round + 1) % references.length];
            for (Case path : paths(receiver, first, second, payload, left, right)) {
                expected.record(path, first, second, payload, left, right);
                Object result = path.call.run();
                if (path.kind == 5) {
                    if (!(result instanceof Integer) || ((Integer) result).intValue() != (left == right ? 0 : 1))
                        throw new AssertionError("generic compare result changed");
                    values++;
                } else if (result != null) throw new AssertionError("void callback returned a value");
                expected.check(receiver);
            }
        }
        RuntimeException problem = new IllegalStateException("owned deferred callback failure");
        receiver.failure = problem;
        for (Case path : paths(receiver, 7, -9, b, a, b)) {
            expected.record(path, 7, -9, b, a, b);
            failure(path.call, IllegalStateException.class, problem);
            expected.check(receiver);
        }
        receiver.failure = null;
        for (Case path : paths(null, 23, 29, a, null, a)) {
            failure(path.call, NullPointerException.class, null);
            expected.check(receiver);
        }
        System.out.println("values=" + values + ",identities=" + identities
                + ",states=" + states + ",failures=" + failures);
        System.out.println("deferred-declarations=unchanged,child-dispatch-and-parent-forwarding=matched,generic-scopes=retained");
    }
}
