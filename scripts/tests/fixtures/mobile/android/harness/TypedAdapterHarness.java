import fixture.Callback;
import fixture.Input;
import fixture.Output;
import fixture.Signal;
import fixture.Task;
import java.lang.reflect.Constructor;
import java.lang.reflect.Field;
import java.lang.reflect.ParameterizedType;
import java.lang.reflect.Type;
import java.lang.reflect.WildcardType;

public class TypedAdapterHarness {
    private static final class Recording implements Callback<Object, Object, Object> {
        Object first, second, result;
        int count;
        boolean fail;
        public Object call(Object a, Object b) {
            first = a; second = b; count++;
            if (fail) throw new IllegalStateException("callback");
            return result;
        }
    }
    public static void main(String[] args) throws Exception {
        Class<?> adapter = Class.forName(args[0]);
        Constructor<?> create = adapter.getDeclaredConstructor(Callback.class);
        create.setAccessible(true);
        Recording callback = new Recording();
        Task first = (Task) create.newInstance(callback);
        Task second = (Task) create.newInstance(callback);
        Signal<Output> signal = new Signal<Output>() {};
        int probes = 0;
        for (Input input : new Input[] {null, new Input()}) {
            for (Signal<Output> continuation : new Signal[] {null, signal}) {
                for (Task task : new Task[] {first, second}) {
                    callback.result = new Object(); callback.count = 0;
                    Object result = task.run(input, continuation);
                    if (result != callback.result || callback.first != input
                            || callback.second != continuation || callback.count != 1)
                        throw new AssertionError("adapter changed arguments, result identity, or call count");
                    callback.fail = true;
                    try {
                        task.run(input, continuation);
                        throw new AssertionError("callback exception was lost");
                    } catch (IllegalStateException expected) {
                        if (callback.count != 2 || !"callback".equals(expected.getMessage()))
                            throw new AssertionError("callback exception effects changed");
                    }
                    callback.fail = false;
                    System.out.println("forward:" + probes++);
                }
            }
        }
        Task absent = (Task) create.newInstance(new Object[] {null});
        try {
            absent.run(null, null);
            throw new AssertionError("null callback did not fail");
        } catch (NullPointerException expected) {
            System.out.println("null:matched");
        }
        Field field = adapter.getDeclaredField("function");
        field.setAccessible(true);
        if (field.get(first) != callback || field.get(second) != callback)
            throw new AssertionError("stored callback identity changed");
        ParameterizedType signature = (ParameterizedType) field.getGenericType();
        Type[] arguments = signature.getActualTypeArguments();
        if ("original".equals(args[1])) {
            if (arguments[0] != null)
                throw new AssertionError("owned baseline did not retain its unresolved variable");
            System.out.println("generic=unresolved");
        } else {
            ParameterizedType nested = (ParameterizedType) arguments[1];
            WildcardType output = (WildcardType) nested.getActualTypeArguments()[0];
            if (arguments[0] != Input.class || output.getLowerBounds()[0] != Output.class
                    || arguments[2] != Object.class)
                throw new AssertionError("source binding disagrees with the typed interface");
            System.out.println("generic=typed");
        }
    }
}
