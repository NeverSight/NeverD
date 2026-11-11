import fixture.*;
import java.lang.invoke.*;
import java.lang.reflect.*;
public final class RawPrimitiveTailHarness {
    static void require(boolean ok) { if (!ok) throw new AssertionError("owned raw primitive tail"); }
    static Object call(int route, Api target, Object value, Sink tail) throws Throwable {
        try {
            if (route == 0) return target.emit(value, tail);
            if (route == 1) return RawPrimitiveTailCaller.call(target, value, tail);
            if (route == 2) return Core.class.getMethod("emit", Object.class, Sink.class).invoke(target, value, tail);
            return (Object)MethodHandles.publicLookup().findVirtual(Api.class, "emit",
                MethodType.methodType(Object.class, Object.class, Sink.class)).invokeExact(target, value, tail);
        } catch (InvocationTargetException problem) { throw problem.getCause(); }
    }
    public static void main(String[] args) throws Throwable {
        int route = Integer.parseInt(args[0]), mode = Integer.parseInt(args[1]), state = Integer.parseInt(args[2]);
        boolean wide = Boolean.parseBoolean(args[3]);
        Method erased = Core.class.getMethod("emit", Object.class, Sink.class);
        require(erased.isBridge() == (state == 1) && erased.isSynthetic() == (state == 1));
        require(Core.class.getTypeParameters().length == 1 && Core.class.getGenericInterfaces()[0] == Api.class);
        Method typed = Core.class.getMethod("emit", wide ? long.class : boolean.class, Sink.class);
        Type tailType = ((ParameterizedType)typed.getGenericParameterTypes()[1]).getActualTypeArguments()[0];
        require(tailType instanceof WildcardType && ((WildcardType)tailType).getLowerBounds()[0] == Token.class);
        Core<Object> core = new Core<Object>();
        RuntimeException original = new IllegalArgumentException("owned tail failure");
        Sink<Object> sink = new Sink<Object>() {
            public Object resume(Object value) {
                Trace.callbacks++; Trace.seen = value;
                if (mode == 6) throw original;
                return Trace.result;
            }
        };
        long[] values = {Long.MIN_VALUE, Long.MAX_VALUE, 0, -1, 1, 0x100000000L,
                         0x80000000L, 0x7fffffffL, -0x100000001L};
        for (int attempt = 0; attempt < 2; attempt++) {
            final long expected = wide ? values[(mode + attempt) % values.length] : attempt;
            Trace.targets = Trace.callbacks = Trace.unboxCalls = 0;
            Trace.seen = Trace.tail = null;
            Trace.token = mode == 9 ? null : new Token();
            Trace.result = mode == 7 ? null : new Object();
            Trace.targetFailure = mode == 4 ? original : null;
            Number custom = new Number() {
                public long longValue() { Trace.unboxCalls++; if (mode == 12) throw original; return expected; }
                public int intValue() { return (int)expected; }
                public float floatValue() { return expected; }
                public double doubleValue() { return expected; }
            };
            Object value = mode == 1 ? null : mode == 2 ? new Object()
                : mode == 3 || mode == 12 ? custom
                : wide ? Long.valueOf(expected) : Boolean.valueOf(expected != 0);
            Sink<Object> tail = mode == 5 ? null : sink;
            boolean conversionFailure = mode == 1 || mode == 2 || (!wide && (mode == 3 || mode == 12)) || (wide && mode == 12);
            boolean nullReceiver = mode == 10;
            boolean enters = !conversionFailure && !nullReceiver;
            boolean resumes = enters && mode != 4 && mode != 5;
            try {
                Object result = mode == 11 ? RawPrimitiveTailCaller.primitive(core, expected != 0, tail)
                    : call(route, nullReceiver ? null : core, value, tail);
                require(enters && mode != 4 && mode != 5 && mode != 6 && result == Trace.result);
            } catch (Throwable failure) {
                if (mode == 4 || mode == 6 || (wide && mode == 12)) require(failure == original);
                else require((mode == 1 || mode == 5 || nullReceiver) ? failure instanceof NullPointerException
                    : conversionFailure && failure instanceof ClassCastException);
            }
            require(Trace.targets == (enters ? 1 : 0) && Trace.callbacks == (resumes ? 1 : 0));
            if (enters) require(Trace.value == expected && Trace.tail == tail);
            if (resumes) require(Trace.seen == Trace.token);
            require(Trace.unboxCalls == (wide && !nullReceiver && (mode == 3 || mode == 12) ? 1 : 0));
            System.out.println("route=" + route + ",mode=" + mode + ",attempt=" + attempt
                + ",targets=" + Trace.targets + ",callbacks=" + Trace.callbacks + ",unbox=" + Trace.unboxCalls);
        }
    }
}
