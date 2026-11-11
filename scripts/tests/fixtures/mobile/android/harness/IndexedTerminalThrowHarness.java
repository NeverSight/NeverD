import fixture.*;
import java.lang.reflect.*;

public final class IndexedTerminalThrowHarness {
    private static int failures, fresh, frames;
    private static UnsupportedOperationException previous;
    private static boolean recovered;
    private static void throwing(Runnable action, boolean resultBridge) {
        try { action.run(); throw new AssertionError("owned exception missing"); }
        catch (UnsupportedOperationException error) {
            if (!"owned immutable operation".equals(error.getMessage()))
                throw new AssertionError("message changed");
            if (error == previous) throw new AssertionError("exception reused");
            previous = error;
            fresh++;
            int count = 0;
            for (StackTraceElement frame : error.getStackTrace())
                if (frame.getClassName().equals("fixture.View")) count++;
            if (count != (recovered && resultBridge ? 2 : 1))
                throw new AssertionError("stack frame difference missed: " + count);
            frames++;
            failures++;
        }
    }
    public static void main(String[] args) throws Exception {
        recovered = args[0].equals("recovered");
        View view = new View();
        Contract raw = view;
        Element item = new Element();
        for (Object argument : new Object[] {item, new Object(), "incompatible", null}) {
            throwing(() -> view.add(argument), false);
            throwing(() -> raw.add(argument), false);
            throwing(() -> view.begin(argument), false);
            throwing(() -> raw.begin(argument), false);
            for (int index : new int[] {Integer.MIN_VALUE, -1, 0, Integer.MAX_VALUE}) {
                throwing(() -> view.insert(index, argument), false);
                throwing(() -> raw.insert(index, argument), false);
                throwing(() -> view.replace(index, argument), false);
                throwing(() -> raw.replace(index, argument), false);
            }
        }
        for (Element argument : new Element[] {item, null}) {
            throwing(() -> view.add(argument), false);
            throwing(() -> view.begin(argument), false);
            for (int index : new int[] {-1, 0, Integer.MAX_VALUE}) {
                throwing(() -> view.insert(index, argument), false);
                throwing(() -> view.replace(index, argument), false);
            }
        }
        for (int index : new int[] {Integer.MIN_VALUE, -1, 0, Integer.MAX_VALUE}) {
            throwing(() -> view.remove(index), false);
            throwing(() -> raw.remove(index), true);
        }
        for (int i = 0; i < 2; ++i) {
            throwing(() -> view.first(), false);
            throwing(() -> raw.first(), true);
        }
        Type api = View.class.getGenericInterfaces()[0];
        if (recovered ? !api.equals(Contract.class) :
                !(api instanceof ParameterizedType) ||
                !((ParameterizedType) api).getActualTypeArguments()[0].equals(Element.class))
            throw new AssertionError("interface projection receipt changed");
        for (Method method : View.class.getDeclaredMethods()) {
            boolean resultOnly = method.getReturnType() == Object.class &&
                    (method.getName().equals("remove") || method.getName().equals("first"));
            boolean erased = resultOnly;
            for (Class<?> parameter : method.getParameterTypes())
                if (parameter == Object.class) erased = true;
            boolean expectedBridge = erased && (!recovered || resultOnly);
            if (method.isBridge() != expectedBridge || method.isSynthetic() != expectedBridge)
                throw new AssertionError("bridge flags differ from report: " + method);
            if (!java.util.Arrays.equals(method.getParameterTypes(), method.getGenericParameterTypes())
                    || !method.getReturnType().equals(method.getGenericReturnType()))
                throw new AssertionError("plain typed method reflection changed");
        }
        System.out.println("failures=" + failures + ",fresh-exceptions=" + fresh + ",stack-frame-cases=" + frames);
        System.out.println("unchecked-arguments=retained,result-only-stack-difference=reported");
    }
}
