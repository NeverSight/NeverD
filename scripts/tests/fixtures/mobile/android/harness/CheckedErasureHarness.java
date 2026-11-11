import fixture.CheckedTransform;
import fixture.Transform;
import java.lang.reflect.Method;
import java.lang.reflect.ParameterizedType;

public class CheckedErasureHarness {
    private static int argumentEffects;
    private static Object invalidArgument() { argumentEffects++; return new Object(); }

    public static void main(String[] args) throws Exception {
        CheckedTransform target = new CheckedTransform();
        Transform raw = target;
        Object[] inputs = {"", new String("token"), null, new Object(), Integer.valueOf(7), new StringBuilder()};
        for (Object input : inputs) {
            for (boolean bridge : new boolean[] {false, true}) {
                CheckedTransform.calls = 0;
                boolean accepted = input == null || input instanceof String;
                try {
                    Object result = bridge ? raw.apply(input) : target.apply(input);
                    if (!accepted || result != input || CheckedTransform.calls != 1)
                        throw new AssertionError("checked body changed identity or effects");
                    System.out.println("accepted:" + bridge + ":" + result);
                } catch (ClassCastException error) {
                    if (accepted || CheckedTransform.calls != 0)
                        throw new AssertionError("failed cast changed body effects", error);
                    System.out.println("cast:" + bridge);
                }
            }
        }
        for (boolean bridge : new boolean[] {false, true}) {
            CheckedTransform absent = null;
            Transform absentRaw = null;
            argumentEffects = 0;
            try {
                if (bridge) absentRaw.apply(invalidArgument());
                else absent.apply(invalidArgument());
                throw new AssertionError("null receiver did not fail");
            } catch (NullPointerException expected) {
                if (argumentEffects != 1)
                    throw new AssertionError("argument evaluation moved past receiver check");
                System.out.println("null:" + bridge);
            }
        }
        int bridges = 0;
        Method implementation = null;
        for (Method method : CheckedTransform.class.getDeclaredMethods()) {
            if (method.isBridge()) bridges++;
            else if (method.getName().equals("apply")) implementation = method;
        }
        if (bridges != 1 || implementation == null
                || implementation.getParameterTypes()[0] != Object.class
                || implementation.getReturnType() != String.class)
            throw new AssertionError("runtime descriptors or bridge count changed");
        ParameterizedType contract = (ParameterizedType) CheckedTransform.class.getGenericInterfaces()[0];
        System.out.println("generic=" + implementation.getGenericParameterTypes()[0].getTypeName()
                + ":" + contract.getActualTypeArguments()[0].getTypeName());
    }
}
