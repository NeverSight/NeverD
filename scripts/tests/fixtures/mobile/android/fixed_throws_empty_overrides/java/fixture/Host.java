package fixture;

public final class Host {
    private Host() {}

    public abstract static class Parent<T> implements Handler {
        public T value;
        public Object fallback;
        public int invokes;
        public int events;
        public Object seenReceiver;
        public java.lang.reflect.Method seenMethod;
        public Object[] seenArguments;
        public RuntimeException failure;

        public Parent() {}

        public abstract boolean test(Object receiver, T value);

        public Object invoke(Object receiver, java.lang.reflect.Method method, Object[] arguments) {
            invokes++;
            events = events * 10 + 7;
            seenReceiver = receiver;
            seenMethod = method;
            seenArguments = arguments;
            if (failure != null) throw failure;
            return test(receiver, value) ? arguments[0] : fallback;
        }
    }

    public static final class Child extends Parent<String> {
        public int tests;
        public Object seenTarget;
        public String seenValue;
        public RuntimeException testFailure;

        public Child() {}

        public boolean test(Object receiver, String value) {
            tests++;
            events = events * 10 + 3;
            seenTarget = receiver;
            seenValue = value;
            if (testFailure != null) throw testFailure;
            return receiver == value;
        }
    }
}
