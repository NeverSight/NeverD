import fixture.*;
import java.io.*;
import java.lang.invoke.*;
import java.lang.reflect.*;
import java.net.*;
public final class StaticFinalResourceHarness {
    static void require(boolean condition) {
        if (!condition) throw new AssertionError("owned static final resource contract");
    }
    public static void main(String[] args) throws Throwable {
        int route = Integer.parseInt(args[0]), mode = Integer.parseInt(args[1]);
        String[] entries = System.getProperty("java.class.path").split(File.pathSeparator);
        URL[] urls = new URL[entries.length];
        for (int i = 0; i < urls.length; i++) urls[i] = new File(entries[i]).toURI().toURL();
        for (int attempt = 0; attempt < 2; attempt++) {
            try (URLClassLoader loader = new URLClassLoader(urls, null)) {
                Class<?> runner = loader.loadClass("StaticFinalResourceHarness$CaseRunner");
                Object events;
                try { events = runner.getMethod("run", int.class, int.class).invoke(null, route, mode); }
                catch (InvocationTargetException failure) { throw failure.getCause(); }
                System.out.println("route=" + route + ",mode=" + mode + ",attempt=" + attempt + ",events=" + events);
            }
        }
    }
    public static final class CaseRunner {
        static Object read(int route, Method method, MethodHandle handle) throws Throwable {
            if (route == 0) return Core.value();
            if (route == 1) return StaticFinalResourceCaller.read();
            if (route == 2) {
                try { return method.invoke(null); }
                catch (InvocationTargetException failure) { throw failure.getCause(); }
            }
            return (Object) handle.invokeExact();
        }
        public static int run(int route, int mode) throws Throwable {
            Observer observer = new Observer(mode);
            Trace.hook = observer;
            Method method = Core.class.getMethod("value");
            MethodHandle handle = MethodHandles.publicLookup().findStatic(Core.class, "value", MethodType.methodType(Object.class));
            Object result = null; Throwable failure = null;
            try { result = read(route, method, handle); }
            catch (Throwable problem) { failure = problem; }
            boolean opened = observer.openFailure == null;
            boolean present = opened && mode != 1;
            int events = 1;
            if (present) events = mode == 17 ? 1423 : 123;
            require(observer.events == events && observer.opens == 1
                && observer.reads == (present ? 1 : 0) && observer.closes == (present ? 1 : 0));
            Throwable expected = observer.openFailure != null ? observer.openFailure
                : observer.readFailure != null ? observer.readFailure : observer.closeFailure;
            boolean failed = expected != null || mode == 1;
            if (!failed) require(failure == null && result == observer.initialValue);
            else {
                Throwable cause = failure instanceof ExceptionInInitializerError
                    ? ((ExceptionInInitializerError) failure).getException() : failure;
                if (mode == 1) require(cause instanceof NullPointerException);
                else if (mode == 14) require(cause instanceof IllegalArgumentException && cause.getCause() == expected);
                else require(cause == expected);
                if (expected instanceof Error) require(failure == expected);
                else require(failure instanceof ExceptionInInitializerError);
            }
            if (observer.readFailure != null) {
                Throwable[] suppressed = observer.readFailure.getSuppressed();
                int count = mode == 12 || mode == 13 ? 1 : mode == 15 ? 2 : 0;
                require(suppressed.length == count);
                if (mode == 15) require(suppressed[0] == observer.prior);
                if (count != 0) require(suppressed[count - 1] == observer.closeFailure);
            }
            for (int repeat = 0; repeat < 2; repeat++) {
                try { Object again = read(route, method, handle); require(!failed && again == observer.initialValue); }
                catch (Throwable problem) { require(failed && problem instanceof NoClassDefFoundError); }
                require(observer.events == events && observer.opens == 1
                    && observer.reads == (present ? 1 : 0) && observer.closes == (present ? 1 : 0));
            }
            return events;
        }
    }
    static class Resource implements Provider {
        final Observer observer;
        Resource(Observer observer) { this.observer = observer; }
        public Object get() { return observer.read(this); }
        public void close() { observer.close(this); }
    }
    public static final class Observer extends Trace {
        final int mode;
        final Object initialValue;
        Object currentValue;
        final Provider resource;
        final Throwable openFailure, readFailure, closeFailure;
        final Throwable prior = new IOException("owned prior failure");
        int events, opens, reads, closes;
        Observer(int mode) {
            this.mode = mode;
            initialValue = mode == 2 ? null : new Object();
            currentValue = initialValue;
            resource = mode == 17 ? new Resource(this) {
                @Override public Object get() { events = events * 10 + 4; return super.get(); }
            } : new Resource(this);
            openFailure = mode == 3 ? new IOException("owned open failure")
                : mode == 4 ? new IllegalStateException("owned open failure")
                : mode == 5 ? new LinkageError("owned open failure") : null;
            readFailure = mode == 6 || mode == 12 || mode == 14 || mode == 15 ? new IOException("owned read failure")
                : mode == 7 ? new IllegalStateException("owned read failure")
                : mode == 8 || mode == 13 ? new LinkageError("owned read failure") : null;
            closeFailure = mode == 9 || mode == 13 ? new IOException("owned close failure")
                : mode == 10 || mode == 12 ? new IllegalStateException("owned close failure")
                : mode == 11 || mode == 15 ? new LinkageError("owned close failure")
                : mode == 14 ? readFailure : null;
            if (mode == 15) readFailure.addSuppressed(prior);
        }
        public Provider open() {
            opens++; events = events * 10 + 1;
            require(Core.value() == null);
            if (openFailure != null) fail(openFailure);
            return mode == 1 ? null : resource;
        }
        public Object read(Provider value) {
            reads++; events = events * 10 + 2;
            require(value == resource && Core.value() == null);
            if (readFailure != null) fail(readFailure);
            return currentValue;
        }
        public void close(Provider value) {
            closes++; events = events * 10 + 3;
            require(value == resource && Core.value() == (readFailure == null ? initialValue : null));
            if (mode == 16) currentValue = new Object();
            if (closeFailure != null) fail(closeFailure);
        }
        @SuppressWarnings("unchecked")
        private static <T extends Throwable> void fail(Throwable failure) throws T { throw (T) failure; }
    }
}
