import fixture.*;
import java.lang.reflect.*;

public final class RawDependencyDefaultsHarness {
    private static int identities, states, failures;
    private static void identity(Object actual, Object expected) {
        if (actual != expected) throw new AssertionError("raw SDK default identity changed");
        identities++;
    }
    private static Object invoke(Method method, Object receiver, Object box) throws Throwable {
        try { return method.invoke(receiver, box); }
        catch (InvocationTargetException problem) { throw problem.getCause(); }
    }
    private static void state() {
        if (Trace.events != 6) throw new AssertionError("raw SDK default failure order changed");
        Trace.events = 0;
        states++;
    }
    @SuppressWarnings({"rawtypes", "unchecked"})
    public static void main(String[] args) throws Throwable {
        OriginalRawParametersHarness.main(args);
        TypeVariable<Class<Api>>[] owners = Api.class.getTypeParameters();
        Method inherited = Api.class.getMethod("inherited", RawBox.class);
        TypeVariable<Method>[] variables = inherited.getTypeParameters();
        if (owners.length != 1 || !owners[0].getName().equals("C")
                || owners[0].getGenericDeclaration() != Api.class || owners[0].getBounds()[0] != Object.class
                || variables.length != 1 || !variables[0].getName().equals("M")
                || !variables[0].getGenericDeclaration().equals(inherited) || variables[0].getBounds()[0] != Mark.class
                || !inherited.getGenericReturnType().equals(variables[0]) || !inherited.isDefault())
            throw new AssertionError("raw Java view changed original SDK generic declarations");
        ParameterizedType parameter = (ParameterizedType) inherited.getGenericParameterTypes()[0];
        if (parameter.getRawType() != RawBox.class
                || !parameter.getActualTypeArguments()[0].equals(variables[0]))
            throw new AssertionError("SDK default lost its complete original method scope");
        Api[] receivers = {new Forwarder(new Sink()), new Leaf(), new Leaf() {}};
        RawBox[] inputs = {null, new RawBox(null), new RawBox(new Payload(1))};
        for (Api receiver : receivers) {
            if (receiver.getClass().getMethod("inherited", RawBox.class).getDeclaringClass() != Api.class)
                throw new AssertionError("raw SDK default was copied into an input class");
            for (RawBox box : inputs) for (int route = 0; route < 2; route++) {
                Object actual = route == 0 ? receiver.inherited(box) : invoke(inherited, receiver, box);
                identity(actual, box == null ? null : box.value);
                if (actual != null) {
                    ((Payload) actual).tag++;
                    identity(box.value, actual);
                }
                state();
            }
            for (int route = 0; route < 2; route++) {
                RawBox invalid = new RawBox("outside original SDK method bound");
                try {
                    if (route == 0) receiver.inherited(invalid); else invoke(inherited, receiver, invalid);
                    throw new AssertionError("original SDK cast failure missing");
                } catch (ClassCastException expected) { failures++; }
                state();
            }
        }
        System.out.println("raw-sdk-default-identities=" + identities + ",states=" + states
                + ",failures=" + failures + ",source-bridges=0");
        System.out.println("raw-default-view-and-original-sdk-metadata=retained,default-ownership-and-failure-order=matched");
    }
}
