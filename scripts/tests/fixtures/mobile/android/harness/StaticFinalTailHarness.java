import fixture.Core;
import fixture.Trace;
import java.io.File;
import java.lang.invoke.MethodHandle;
import java.lang.invoke.MethodHandles;
import java.lang.reflect.Field;
import java.lang.reflect.InvocationTargetException;
import java.net.URL;
import java.net.URLClassLoader;

public final class StaticFinalTailHarness {
    private static void check(boolean condition) {
        if (!condition) throw new AssertionError("owned static final tail mismatch");
    }

    public static void main(String[] args) throws Throwable {
        String[] entries = System.getProperty("java.class.path").split(File.pathSeparator);
        URL[] urls = new URL[entries.length];
        for (int i = 0; i < urls.length; ++i) urls[i] = new File(entries[i]).toURI().toURL();
        for (int attempt = 0; attempt < 2; ++attempt) {
            try (URLClassLoader loader = new URLClassLoader(urls, null)) {
                Class<?> runner = loader.loadClass("StaticFinalTailHarness$CaseRunner");
                try {
                    runner.getMethod("run", int.class, int.class).invoke(null,
                        Integer.parseInt(args[0]), Integer.parseInt(args[1]));
                } catch (InvocationTargetException failure) {
                    throw failure.getCause();
                }
            }
        }
        System.out.println("attempts=2,static-final-tail=matched");
    }

    public static final class CaseRunner {
        public static void run(int route, int mode) throws Throwable {
            Observer observer = new Observer(mode);
            Trace.hook = observer;
            Field field = Core.class.getField("VALUE");
            MethodHandle getter = MethodHandles.lookup().findStaticGetter(Core.class, "VALUE", int.class);
            Throwable expected = observer.expectedFailure();
            Throwable caught = null;
            int value = 0;
            try {
                value = read(route, field, getter);
            } catch (Throwable failure) {
                caught = failure;
            }
            if (expected == null) {
                check(caught == null && value == observer.expectedValue());
                check(observer.events.equals("inside,cleanup,finish,"));
                check(Core.TOKEN == observer.token && Core.DATA == observer.data);
            } else {
                check(caught != null);
                if (expected instanceof Error) check(caught == expected);
                else check(caught instanceof ExceptionInInitializerError
                    && ((ExceptionInInitializerError)caught).getException() == expected);
                String events = mode >= 8 && mode <= 9 ? "inside,cleanup,finish," : "inside,cleanup,";
                check(observer.events.equals(events));
            }
            String before = observer.events;
            for (int repeat = 0; repeat < 2; ++repeat) {
                try {
                    int again = read(route, field, getter);
                    check(expected == null && again == observer.expectedValue());
                } catch (Throwable failure) {
                    check(expected != null && failure instanceof NoClassDefFoundError);
                }
                check(observer.events.equals(before));
            }
        }

        private static int read(int route, Field field, MethodHandle getter) throws Throwable {
            if (route == 0) return field.getInt(null);
            if (route == 1) return (int)getter.invokeExact();
            return Core.VALUE;
        }
    }

    public static final class Observer extends Trace {
        final int mode;
        final Exception checked = new Exception("owned checked failure");
        final Error error = new LinkageError("owned error failure");
        String events = "";
        Object token;
        float[] data;

        Observer(int mode) { this.mode = mode; }

        private void preparation(Object current) {
            check(current != null && current == token && Core.TOKEN == token);
            check(Core.DATA == data && Core.VALUE == 0);
        }

        @Override public void observe(int phase, Object current, float[] values) {
            events += "inside,";
            token = current;
            data = values;
            check(phase == 17 && values.length == 4);
            preparation(current);
            check(Float.floatToRawIntBits(values[0]) == 0x3f800000);
            check(Float.floatToRawIntBits(values[1]) == 0x40200000);
            check(Float.floatToRawIntBits(values[2]) == 0x80000000);
            check(Float.floatToRawIntBits(values[3]) == 0x41100000);
            if (mode == 3) values[3] = 7.0f;
            if (mode == 4 || mode == 10) fail(checked);
            if (mode == 5 || mode == 11) fail(error);
        }

        @Override public void onCleanup(Object current) {
            events += "cleanup,";
            preparation(current);
            check(events.equals("inside,cleanup,"));
            if (mode == 6 || mode == 11) fail(checked);
            if (mode == 7 || mode == 10) fail(error);
        }

        @Override public int answer(Object current) {
            events += "finish,";
            preparation(current);
            check(events.equals("inside,cleanup,finish,"));
            if (mode == 8) fail(checked);
            if (mode == 9) fail(error);
            return expectedValue();
        }

        int expectedValue() {
            if (mode == 1) return 0;
            if (mode == 2) return Integer.MIN_VALUE;
            if (mode == 3) {
                check(data[3] == 7.0f);
                return Float.floatToRawIntBits(data[3]);
            }
            return 17;
        }

        Throwable expectedFailure() {
            if (mode == 4 || mode == 6 || mode == 8 || mode == 11) return checked;
            if (mode == 5 || mode == 7 || mode == 9 || mode == 10) return error;
            return null;
        }

        @SuppressWarnings("unchecked")
        private static <T extends Throwable> void fail(Throwable failure) throws T {
            throw (T)failure;
        }
    }
}
