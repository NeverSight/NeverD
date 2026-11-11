import fixture.*;
import java.lang.invoke.MethodHandles;
import java.lang.invoke.MethodType;
import java.lang.reflect.InvocationTargetException;
public class PrimitiveConstructorOverloadsHarness {
    static void require(boolean value) {
        if (!value) throw new AssertionError("owned primitive constructor overload contract");
    }
    static Core<?, ?> call(int width, int route, Object input) throws Throwable {
        if (route == 0) return width == 0 ? new Core<String, Integer>((int) Trace.read(input))
                                        : new Core<String, Integer>(Trace.read(input));
        if (route == 1) return PrimitiveConstructorOverloadsCaller.create(width, input);
        Class<?> parameter = width == 0 ? int.class : long.class;
        if (route == 2) {
            Object argument = width == 0 ? (Object) Integer.valueOf((int) Trace.read(input))
                                        : (Object) Long.valueOf(Trace.read(input));
            try { return (Core<?, ?>) Core.class.getConstructor(parameter).newInstance(argument); }
            catch (InvocationTargetException failure) { throw failure.getCause(); }
        }
        if (width == 0) return (Core<?, ?>) MethodHandles.publicLookup()
                .findConstructor(Core.class, MethodType.methodType(void.class, int.class)).invoke((int) Trace.read(input));
        return (Core<?, ?>) MethodHandles.publicLookup()
                .findConstructor(Core.class, MethodType.methodType(void.class, long.class)).invoke(Trace.read(input));
    }
    public static void main(String[] args) throws Throwable {
        int width = Integer.parseInt(args[0]), route = Integer.parseInt(args[1]);
        int kind = Integer.parseInt(args[2]), mode = Integer.parseInt(args[3]);
        require(Parent.class.getTypeParameters().length == 2 && Core.class.getTypeParameters().length == 2);
        require(!Parent.class.getConstructor(int.class).isVarArgs() && !Parent.class.getConstructor(long.class).isVarArgs());
        require(Parent.class.getConstructor(Object[].class).isVarArgs());
        long[] values = {kind == 0 ? 0x123456789abcdefL : kind == 1 ? Long.MIN_VALUE + 77 : Long.MAX_VALUE - 77};
        Object input = mode == 5 ? null : mode == 6 ? new Object() : values;
        for (int attempt = 0; attempt < 2; ++attempt) {
            Trace.events = 0; Trace.seed = attempt == 0 ? 7 : 200; Trace.last = null;
            Trace.fail = mode == 1 ? 6 : mode == 2 ? (width == 0 ? 1 : 3) : mode == 3 ? 5 : mode == 4 ? 9 : 0;
            Trace.failure = new IllegalArgumentException("owned constructor overload failure");
            long old = values[0], expected = 0;
            boolean reflective = route >= 2;
            boolean readFailed = mode == 1 || mode == 5 || mode == 6;
            boolean read = mode != 4 || reflective;
            boolean parent = !readFailed && mode != 4;
            long seed = attempt == 0 ? (reflective ? 7 : 100) : 200;
            long number = old + seed;
            if (width == 0) number = (int) number;
            if (reflective) expected = 6;
            if (!(reflective && readFailed) && attempt == 0) expected = expected * 11 + 9;
            if (!reflective && read) expected = expected * 11 + 6;
            if (parent) {
                expected = expected * 11 + (width == 0 ? 1 : 3);
                if (mode != 2) expected = expected * 11 + 5;
            }
            Throwable caught = null; Core<?, ?> actual = null;
            try { actual = call(width, route, input); } catch (Throwable failure) { caught = failure; }
            if (mode == 0) require(caught == null && actual == Trace.last);
            else if (mode == 1 || mode == 2 || mode == 3) require(caught == Trace.failure);
            else if (mode == 4) {
                if (attempt == 0) require(caught instanceof ExceptionInInitializerError && caught.getCause() == Trace.failure);
                else require(caught instanceof NoClassDefFoundError);
            } else if (mode == 5) require(caught instanceof NullPointerException);
            else require(caught instanceof ClassCastException);
            require(Trace.events == expected && values[0] == old + (read && !readFailed ? 1 : 0));
            require(parent ? Trace.last instanceof Core : Trace.last == null);
            if (parent) {
                Parent<?, ?> value = (Parent<?, ?>) Trace.last;
                require(value.selected == (width == 0 ? 1 : 3) && value.number == number);
            }
            System.out.println("width=" + width + ",route=" + route + ",kind=" + kind + ",mode=" + mode
                    + ",attempt=" + attempt + ",trace=" + Trace.events);
        }
    }
}
