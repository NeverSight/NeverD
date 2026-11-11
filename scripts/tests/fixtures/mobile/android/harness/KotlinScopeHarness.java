import fixture.Box;
import fixture.Callback;
import fixture.MetadataHost;
import java.lang.reflect.InvocationTargetException;
import java.lang.reflect.ParameterizedType;

public class KotlinScopeHarness {
    private static final class Recording implements Callback<Object, Box<Object>> {
        Object expected;
        Object output;
        boolean fail;
        int count;
        public Box<Object> apply(Object value) {
            if (value != expected) throw new AssertionError("callback argument lost identity");
            count++;
            if (fail) throw new IllegalStateException("callback");
            Box<Object> result = new Box<Object>();
            result.value = output;
            return result;
        }
    }
    @SuppressWarnings("unchecked")
    public static void main(String[] args) throws Exception {
        var method = MetadataHost.class.getMethod("adapt", Box.class, Callback.class);
        if (method.getTypeParameters().length != 0)
            throw new AssertionError("a new formal was invented on the erased method");
        Object[] inputs = {null, "text", Integer.valueOf(11), new Object()};
        for (int i = 0; i < inputs.length; i++) {
            Box<Object> input = new Box<Object>();
            Recording callback = new Recording();
            Box<Object> result = (Box<Object>) method.invoke(null, input, callback);
            if (result == input || input.watcher == null || result.value != null)
                throw new AssertionError("construction or initial state changed");
            for (int round = 0; round < 2; round++) {
                input.value = callback.expected = inputs[(i + round) % inputs.length];
                Object expected = callback.output = new Object();
                input.fire();
                if (result.value != expected || callback.count != round + 1)
                    throw new AssertionError("result capture or callback count changed");
            }
            Object previous = result.value;
            callback.fail = true;
            try { input.fire(); throw new AssertionError("callback failure was swallowed"); }
            catch (IllegalStateException expected) {
                if (result.value != previous || callback.count != 3)
                    throw new AssertionError("failed callback changed the stored result");
            }
            System.out.println(i + ":count=" + callback.count);
            if (i == 0) {
                try {
                    var contract = (ParameterizedType) input.watcher.getClass().getGenericInterfaces()[0];
                    var type = contract.getActualTypeArguments()[0];
                    if (args[0].equals("original") ? type != null : type != Object.class)
                        throw new AssertionError("generic reflection disagrees with the projection");
                } catch (TypeNotPresentException missing) {
                    // Newer JVMs report the dangling original variable as an
                    // exception; older JVMs expose a null type argument.
                    if (!args[0].equals("original") || !missing.typeName().equals("X")) throw missing;
                }
            }
        }
        Box<Object> input = new Box<Object>();
        method.invoke(null, input, null);
        try { input.fire(); throw new AssertionError("null callback was accepted"); }
        catch (NullPointerException expected) { System.out.println("null-callback=matched"); }
        try { method.invoke(null, null, new Recording()); throw new AssertionError("null input was accepted"); }
        catch (InvocationTargetException expected) {
            if (!(expected.getCause() instanceof NullPointerException)) throw expected;
            System.out.println("null-input=matched");
        }
    }
}
