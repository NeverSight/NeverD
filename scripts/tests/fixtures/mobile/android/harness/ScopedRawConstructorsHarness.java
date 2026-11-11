import fixture.*;
import java.lang.invoke.MethodHandles;
import java.lang.invoke.MethodType;
import java.lang.reflect.InvocationTargetException;
public class ScopedRawConstructorsHarness {
    static void require(boolean value) {
        if (!value) throw new AssertionError("owned scoped raw constructor contract");
    }
    static Core<?, ?> call(int route, Parent input) throws Throwable {
        if (route == 0) return new Core<String, Integer>(input);
        if (route == 1) return ScopedRawConstructorsCaller.create(input);
        if (route == 2) {
            try { return (Core<?, ?>) Core.class.getConstructor(Parent.class).newInstance(input); }
            catch (InvocationTargetException failure) { throw failure.getCause(); }
        }
        return (Core<?, ?>) MethodHandles.publicLookup()
                .findConstructor(Core.class, MethodType.methodType(void.class, Parent.class)).invoke(input);
    }
    public static void main(String[] args) throws Throwable {
        int route = Integer.parseInt(args[0]), kind = Integer.parseInt(args[1]), mode = Integer.parseInt(args[2]);
        require(Core.class.getTypeParameters().length == 2 && Parent.class.getTypeParameters().length == 2);
        require(Core.class.getConstructor(Parent.class).getGenericParameterTypes()[0] == Parent.class);
        Parent source = new Parent(0);
        Trace.source = source;
        Trace.initialKey = kind == 0 ? new Object() : kind == 1 ? "owned key" : Float.valueOf(Float.NaN);
        Trace.initialValue = kind == 0 ? new Object() : kind == 1 ? Integer.valueOf(17) : "untyped original value";
        Trace.lateValue = mode == 6 ? null : new Object();
        Trace.fail = mode == 1 ? 1 : mode == 2 ? 2 : mode == 3 ? 3 : mode == 4 ? 9 : 0;
        Trace.failure = new IllegalArgumentException("owned raw constructor failure");
        for (int attempt = 0; attempt < 2; attempt++) {
            Object key = new Object(), value = new Object();
            source.key = key; source.value = value;
            if (attempt == 0 && mode != 4) { key = Trace.initialKey; value = Trace.initialValue; }
            Trace.events = 0; Trace.published = null;
            Parent argument = mode == 5 ? null : source;
            Core<?, ?> result = null; Throwable caught = null;
            try { result = call(route, argument); } catch (Throwable failure) { caught = failure; }
            long expected = attempt == 0 ? 9 : 0;
            boolean parent = mode != 4;
            if (parent) expected = expected * 11 + 1;
            if (mode != 1 && mode != 4 && mode != 5) expected = expected * 11 + 2;
            if (mode == 0 || mode == 3 || mode == 6) expected = expected * 11 + 3;
            if (mode == 0 || mode == 6) require(caught == null && result == Trace.published);
            else if (mode >= 1 && mode <= 3) require(caught == Trace.failure);
            else if (mode == 4) {
                if (attempt == 0) require(caught instanceof ExceptionInInitializerError && caught.getCause() == Trace.failure);
                else require(caught instanceof NoClassDefFoundError);
            } else require(caught instanceof NullPointerException);
            require(Trace.events == expected);
            boolean published = mode != 1 && mode != 4;
            require(published ? Trace.published instanceof Core : Trace.published == null);
            if (published) {
                Core<?, ?> partial = (Core<?, ?>) Trace.published;
                require(partial.local == null);
                require(partial.key == (mode == 5 ? null : key));
                require(partial.value == (mode == 2 || mode == 5 ? null : Trace.lateValue));
            }
            if (mode == 0 || mode == 3 || mode == 6) require(source.value == Trace.lateValue);
            else require(source.value == value);
            System.out.println("route=" + route + ",kind=" + kind + ",mode=" + mode
                    + ",attempt=" + attempt + ",trace=" + Trace.events);
        }
    }
}
