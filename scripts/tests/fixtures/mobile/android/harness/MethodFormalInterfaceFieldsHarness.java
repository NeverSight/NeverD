import fixture.FormalView;
import fixture.Key;
import fixture.Receiver;
import fixture.Wrapper;
import java.lang.reflect.Method;
import java.lang.reflect.Modifier;
import java.lang.reflect.ParameterizedType;
import java.lang.reflect.TypeVariable;

public class MethodFormalInterfaceFieldsHarness {
    private static int values, identities, failures;

    private static void value(boolean ok) {
        values++;
        if (!ok) throw new AssertionError("value " + values);
    }

    private static void identity(Object actual, Object expected) {
        identities++;
        if (actual != expected) throw new AssertionError("identity " + identities);
    }

    public static class Child extends Receiver<String> {
        public int visits;
        @Override public <T> T get(Key<T> key) {
            visits++;
            return super.get(key);
        }
    }

    private static void exercise(Receiver<String> receiver) {
        FormalView view = receiver;
        Wrapper<String> wrapped = receiver;
        Key<Object> key = new Key<Object>();
        Object first = new Object();
        key.value = first;
        identity(view.get(key), first);
        identity(wrapped.get(key), first);
        identity(receiver.last, key);
        value(receiver.calls == 2);
        Object second = new Object();
        key.value = second;
        identity(view.get(key), second);
        key.value = null;
        identity(view.get(key), null);
        value(receiver.calls == 4);
        try {
            view.get(null);
            throw new AssertionError("missing null-key exception");
        } catch (NullPointerException expected) {
            failures++;
            value(receiver.calls == 5);
            identity(receiver.last, null);
        }
        RuntimeException sentinel = new RuntimeException("owned sentinel");
        receiver.failure = sentinel;
        for (Key<Object> input : new Key[] {key, null}) {
            try {
                view.get(input);
                throw new AssertionError("missing sentinel");
            } catch (RuntimeException actual) {
                failures++;
                identity(actual, sentinel);
                identity(receiver.last, input);
            }
        }
        value(receiver.calls == 7);
        receiver.failure = null;
        key.value = second;
        identity(view.get(key), second);
        value(receiver.calls == 8);
        if (receiver instanceof Child) value(((Child) receiver).visits == 8);
    }

    private static void methodScope(Class<?> owner) throws Exception {
        Method method = owner.getDeclaredMethod("get", Key.class);
        TypeVariable<?>[] formals = method.getTypeParameters();
        value(formals.length == 1);
        value(formals[0].getName().equals("T"));
        value(formals[0].getGenericDeclaration().equals(method));
        value(formals[0].getBounds().length == 1);
        identity(formals[0].getBounds()[0], Object.class);
        value(method.getGenericReturnType().equals(formals[0]));
        ParameterizedType parameter = (ParameterizedType) method.getGenericParameterTypes()[0];
        identity(parameter.getRawType(), Key.class);
        value(parameter.getActualTypeArguments()[0].equals(formals[0]));
    }

    private static void fixedCalls(Receiver<String> receiver) {
        FormalView view = receiver;
        Key<Object> key = new Key<Object>();
        Object payload = new Object();
        key.value = payload;
        identity(view.fixed(key), payload);
        identity(receiver.last, key);
        identity(view.plain(payload), payload);
        identity(receiver.last, payload);
        value(receiver.calls == 10);
        try {
            view.fixed(null);
            throw new AssertionError("missing fixed null-key exception");
        } catch (NullPointerException expected) {
            failures++;
            value(receiver.calls == 11);
            identity(receiver.last, null);
        }
        RuntimeException sentinel = new RuntimeException("owned fixed sentinel");
        receiver.failure = sentinel;
        try {
            view.plain(null);
            throw new AssertionError("missing plain sentinel");
        } catch (RuntimeException actual) {
            failures++;
            identity(actual, sentinel);
            identity(receiver.last, null);
            value(receiver.calls == 12);
        }
        receiver.failure = null;
        identity(view.plain(null), null);
        value(receiver.calls == 13);
    }

    public static void main(String[] args) throws Exception {
        value(Receiver.class.getDeclaredMethod("get", Key.class).isBridge() ==
                (args.length != 0 && args[0].equals("bridge")));
        boolean fixed = args.length > 1 && args[1].equals("fixed");
        value(Receiver.class.getDeclaredMethod("fixed", Key.class).isBridge() == fixed);
        value(Receiver.class.getDeclaredMethod("plain", Object.class).isBridge() == fixed);
        value(FormalView.class.getTypeParameters().length == 0);
        value(FormalView.TOKEN == 11);
        value(FormalView.class.getDeclaredField("TOKEN").getInt(null) == 11);
        int flags = FormalView.class.getDeclaredField("TOKEN").getModifiers();
        value(Modifier.isPublic(flags) && Modifier.isStatic(flags) && Modifier.isFinal(flags));
        value(Wrapper.class.getTypeParameters()[0].getName().equals("W"));
        value(Receiver.class.getTypeParameters()[0].getName().equals("C"));
        methodScope(FormalView.class);
        methodScope(Receiver.class);
        Receiver<String> receiver = new Receiver<String>();
        Child child = new Child();
        exercise(receiver);
        fixedCalls(receiver);
        exercise(child);
        fixedCalls(child);
        System.out.println("values=" + values + ",identities=" + identities + ",failures=" + failures);
        System.out.println("source-interface-fields-and-method-formals=retained,virtual-effects-and-exceptions=matched");
    }
}
